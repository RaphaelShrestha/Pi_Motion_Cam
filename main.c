/* main.c - argument parsing, the capture loop, and signal handling
 *
 *   capture -> ring buffer -> detector -> recorder
 *
 * pi-motion-cam, a personal project. Not a security product.
 */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "config.h"
#include "log.h"
#include "motion.h"
#include "recorder.h"
#include "ringbuf.h"
#include "v4l2_capture.h"

static volatile sig_atomic_t stop_requested = 0;

static void on_signal(int sig)
{
    (void)sig;
    stop_requested = 1;
}

static void install_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;                 /* no SA_RESTART: interrupt select() */

    sigaction(SIGINT,  &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* If ffmpeg dies, write() should fail rather than kill us. */
    signal(SIGPIPE, SIG_IGN);
}

static void default_config_path(char *out, size_t outlen)
{
    const char *home = getenv("HOME");

    if (home && *home)
        snprintf(out, outlen, "%s/.config/pi-motion-cam/config.ini", home);
    else
        out[0] = '\0';
}

/* Writes the retained pre-roll frames into the encoder, so the clip
 * starts before the trigger rather than partway through it. */
static int flush_preroll(recorder_t *rec, ringbuf_t *rb, size_t frame_size)
{
    size_t i, n = ringbuf_count(rb);

    for (i = 0; i < n; i++) {
        const uint8_t *f = ringbuf_at(rb, i);

        if (!f)
            break;
        if (recorder_write(rec, f, frame_size) < 0)
            return -1;
    }

    if (n)
        log_verbose("flushed %zu pre-roll frames", n);
    return 0;
}

int main(int argc, char **argv)
{
    config_t   cfg;
    capture_t  cap;
    motion_t   det;
    ringbuf_t  ring;
    recorder_t rec;

    const char *cfg_path;
    char        default_path[1024];
    char        stem[1024];

    uint8_t *packed = NULL;
    uint8_t *gray   = NULL;
    int      rc     = EXIT_FAILURE;
    int      have_capture = 0, have_detector = 0, have_ring = 0;
    int      snapshot_pending = 0;

    /* --- configuration ------------------------------------------- */
    config_defaults(&cfg);

    cfg_path = config_peek_path(argc, argv);
    if (cfg_path) {
        if (config_load_file(&cfg, cfg_path, 1) < 0)
            return EXIT_FAILURE;
    } else {
        default_config_path(default_path, sizeof default_path);
        if (default_path[0] && config_load_file(&cfg, default_path, 0) < 0)
            return EXIT_FAILURE;
    }

    switch (config_parse_args(&cfg, argc, argv)) {
    case 1:  return EXIT_SUCCESS;          /* --help */
    case -1: return EXIT_FAILURE;
    default: break;
    }

    log_set_verbose(cfg.verbose);

    if (config_validate(&cfg) < 0)
        return EXIT_FAILURE;

    install_handlers();
    recorder_init(&rec);

    if (cfg.mode != MODE_STILLS || cfg.mode == MODE_BOTH) {
        if (recorder_probe() < 0)
            return EXIT_FAILURE;
    }

    /* --- capture -------------------------------------------------- */
    if (capture_open(&cap, cfg.device, (unsigned)cfg.width,
                     (unsigned)cfg.height, (unsigned)cfg.fps) < 0)
        return EXIT_FAILURE;
    have_capture = 1;

    packed = malloc(cap.frame_size);
    gray   = malloc((size_t)cap.width * cap.height);
    if (!packed || !gray) {
        log_error("out of memory allocating frame buffers");
        goto cleanup;
    }

    /* --- detector -------------------------------------------------- */
    if (motion_init(&det, cap.width, cap.height, &cfg) < 0)
        goto cleanup;
    have_detector = 1;

    if (cfg.mask[0] && motion_load_mask(&det, cfg.mask) < 0)
        goto cleanup;

    /* --- pre-roll -------------------------------------------------- */
    {
        size_t frames = (size_t)cfg.preroll * cap.fps;
        size_t bytes  = frames * cap.frame_size;

        if (ringbuf_init(&ring, cap.frame_size, frames) < 0)
            goto cleanup;
        have_ring = 1;

        if (frames)
            log_verbose("pre-roll: %zu frames, %.1f MB",
                        frames, (double)bytes / (1024.0 * 1024.0));
    }

    if (capture_start(&cap) < 0)
        goto cleanup;

    /* --- loop ------------------------------------------------------ */
    while (!stop_requested) {
        const uint8_t *raw;
        size_t         raw_len;
        unsigned       index;
        double         score = 0.0;
        int            triggered, r;

        r = capture_frame(&cap, &raw, &raw_len, &index);
        if (r < 0)
            goto cleanup;
        if (r == 0)
            continue;                       /* timeout, EAGAIN, or a signal */

        capture_pack(&cap, raw, packed);
        capture_extract_gray(&cap, packed, gray);

        if (capture_release(&cap, index) < 0)
            goto cleanup;

        triggered = motion_process(&det, gray, &score, rec.active);

        if (cfg.verbose)
            log_verbose("score=%.2f%%%s", score, rec.active ? " (recording)" : "");

        if (!rec.active) {
            if (triggered) {
                if (recorder_make_stem(&cfg, stem, sizeof stem) < 0)
                    goto cleanup;

                if (cfg.mode == MODE_STILLS) {
                    if (recorder_snapshot(&cfg, packed, cap.frame_size,
                                          cap.width, cap.height, stem) == 0)
                        log_info("motion  score=%.1f%%  -> %s.jpg", score, stem);
                    /* Re-use the cooldown as a simple rate limit on stills. */
                    sleep((unsigned)cfg.cooldown);
                    motion_reset(&det);
                } else {
                    if (recorder_start(&rec, &cfg, cap.width, cap.height,
                                       cap.fps) < 0)
                        goto cleanup;

                    log_info("motion  score=%.1f%%  -> %s", score, rec.path);

                    if (flush_preroll(&rec, &ring, cap.frame_size) < 0) {
                        recorder_stop(&rec);
                        goto cleanup;
                    }
                    ringbuf_clear(&ring);

                    snapshot_pending = (cfg.mode == MODE_BOTH);
                    if (recorder_write(&rec, packed, cap.frame_size) < 0) {
                        recorder_stop(&rec);
                        goto cleanup;
                    }
                    rec.last_motion = time(NULL);
                }
            } else {
                ringbuf_push(&ring, packed);
            }
        } else {
            time_t now = time(NULL);

            if (recorder_write(&rec, packed, cap.frame_size) < 0) {
                recorder_stop(&rec);
                goto cleanup;
            }

            if (snapshot_pending) {
                recorder_snapshot(&cfg, packed, cap.frame_size,
                                  cap.width, cap.height, stem);
                snapshot_pending = 0;
            }

            if (score >= cfg.sensitivity)
                rec.last_motion = now;

            if (now - rec.last_motion >= cfg.cooldown ||
                now - rec.started    >= cfg.max_duration) {
                double secs   = difftime(now, rec.started);
                int    capped = (now - rec.started >= cfg.max_duration);

                recorder_stop(&rec);
                log_info("stopped after %.1fs (%lu frames)%s",
                         secs, rec.frames, capped ? " - duration cap" : "");

                /* The model was frozen throughout the clip, so let it
                 * re-prime against the scene as it now stands. */
                motion_reset(&det);
                ringbuf_clear(&ring);
            }
        }
    }

    rc = EXIT_SUCCESS;

cleanup:
    if (rec.active) {
        /* Finalize rather than abandon: closing ffmpeg's stdin is what
         * writes the index, and killing it here leaves a broken file. */
        log_info("finalizing %s", rec.path);
        recorder_stop(&rec);
    }
    if (have_ring)
        ringbuf_free(&ring);
    if (have_detector)
        motion_free(&det);
    if (have_capture)
        capture_close(&cap);

    free(packed);
    free(gray);

    if (stop_requested)
        log_info("stopped");

    return rc;
}
