/* recorder.c - ffmpeg child process and pipe management */
#include "recorder.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static char encoder[64] = "libx264";
static int  probed = 0;

const char *recorder_encoder(void) { return encoder; }

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static int mkdir_p(const char *path)
{
    char tmp[1024];
    char *p;
    size_t len;

    len = strlen(path);
    if (len == 0 || len >= sizeof tmp)
        return -1;

    memcpy(tmp, path, len + 1);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';

    for (p = tmp + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        if (mkdir(tmp, 0755) == -1 && errno != EEXIST)
            return -1;
        *p = '/';
    }

    if (mkdir(tmp, 0755) == -1 && errno != EEXIST)
        return -1;
    return 0;
}

/* fork/exec rather than popen, so the argument vector is explicit and the
 * child can be reaped deterministically. */
static int spawn(char *const argv[], pid_t *pid_out, int *stdin_fd_out)
{
    int pfd[2];
    pid_t pid;

    if (pipe(pfd) == -1) {
        log_errno("pipe");
        return -1;
    }

    pid = fork();
    if (pid == -1) {
        log_errno("fork");
        close(pfd[0]);
        close(pfd[1]);
        return -1;
    }

    if (pid == 0) {
        /* child */
        int devnull;

        close(pfd[1]);
        if (dup2(pfd[0], STDIN_FILENO) == -1)
            _exit(127);
        close(pfd[0]);

        if (!log_is_verbose()) {
            devnull = open("/dev/null", O_WRONLY);
            if (devnull != -1) {
                dup2(devnull, STDOUT_FILENO);
                dup2(devnull, STDERR_FILENO);
                if (devnull > STDERR_FILENO)
                    close(devnull);
            }
        }

        signal(SIGPIPE, SIG_DFL);
        signal(SIGINT, SIG_IGN);     /* let the parent finalize the file */

        execvp(argv[0], argv);
        _exit(127);
    }

    /* parent */
    close(pfd[0]);
    *pid_out      = pid;
    *stdin_fd_out = pfd[1];
    return 0;
}

static ssize_t write_all(int fd, const uint8_t *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        ssize_t n = write(fd, buf + off, len - off);

        if (n == -1) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)n;
    }
    return (ssize_t)off;
}

/* ------------------------------------------------------------------ */
/* probe                                                               */
/* ------------------------------------------------------------------ */

/* Runs `cmd` and reports whether it exited successfully. */
static int run_quiet(const char *cmd)
{
    int status = system(cmd);

    if (status == -1)
        return 0;
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/* Listing an encoder is not the same as being able to use it: ffmpeg
 * advertises h264_v4l2m2m on any Linux build, whether or not there is an
 * M2M device behind it. So the probe actually encodes a fraction of a
 * second of black and checks that ffmpeg survives. */
static int encoder_works(const char *name)
{
    char cmd[512];

    snprintf(cmd, sizeof cmd,
             "ffmpeg -hide_banner -loglevel quiet "
             "-f lavfi -i color=c=black:s=64x64:r=5:d=0.2 "
             "-c:v %s -f null - >/dev/null 2>&1", name);

    return run_quiet(cmd);
}

int recorder_probe(void)
{
    const char *override;

    probed = 1;

    if (!run_quiet("ffmpeg -hide_banner -version >/dev/null 2>&1")) {
        log_error("ffmpeg not found on PATH (apt install ffmpeg)");
        return -1;
    }

    /* Escape hatch: PMC_ENCODER=h264_omx, and so on. */
    override = getenv("PMC_ENCODER");
    if (override && *override) {
        snprintf(encoder, sizeof encoder, "%s", override);
        if (!encoder_works(encoder)) {
            log_error("encoder '%s' from PMC_ENCODER does not work here",
                      encoder);
            return -1;
        }
        log_info("encoder: %s (from PMC_ENCODER)", encoder);
        return 0;
    }

    if (encoder_works("h264_v4l2m2m")) {
        snprintf(encoder, sizeof encoder, "h264_v4l2m2m");
        log_verbose("encoder: h264_v4l2m2m (hardware)");
        return 0;
    }

    if (encoder_works("libx264")) {
        snprintf(encoder, sizeof encoder, "libx264");
        log_warn("no working hardware H.264 encoder, falling back to libx264 "
                 "(expect noticeably higher CPU use)");
        return 0;
    }

    log_error("ffmpeg has no usable H.264 encoder");
    return -1;
}

/* ------------------------------------------------------------------ */
/* output paths                                                        */
/* ------------------------------------------------------------------ */

int recorder_make_stem(const config_t *cfg, char *out, size_t outlen)
{
    char      dir[1024];
    char      day[16], hms[16];
    time_t    now = time(NULL);
    struct tm tm;

    localtime_r(&now, &tm);
    strftime(day, sizeof day, "%Y-%m-%d", &tm);
    strftime(hms, sizeof hms, "%H%M%S", &tm);

    if (snprintf(dir, sizeof dir, "%s/%s", cfg->output, day) >= (int)sizeof dir) {
        log_error("output path is too long");
        return -1;
    }
    if (mkdir_p(dir) == -1) {
        log_error("cannot create '%s': %s", dir, strerror(errno));
        return -1;
    }
    if (snprintf(out, outlen, "%s/%s", dir, hms) >= (int)outlen) {
        log_error("output path is too long");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* video                                                               */
/* ------------------------------------------------------------------ */

void recorder_init(recorder_t *r)
{
    memset(r, 0, sizeof *r);
    r->fd  = -1;
    r->pid = -1;
}

int recorder_start(recorder_t *r, const config_t *cfg,
                   unsigned width, unsigned height, unsigned fps)
{
    char stem[1024];
    char size[32], rate[16];
    char *argv[32];
    int   i = 0;

    if (r->active)
        return 0;
    if (!probed && recorder_probe() < 0)
        return -1;

    if (recorder_make_stem(cfg, stem, sizeof stem) < 0)
        return -1;
    if (snprintf(r->path, sizeof r->path, "%s.mp4", stem) >= (int)sizeof r->path)
        return -1;

    snprintf(size, sizeof size, "%ux%u", width, height);
    snprintf(rate, sizeof rate, "%u", fps);

    argv[i++] = (char *)"ffmpeg";
    argv[i++] = (char *)"-hide_banner";
    argv[i++] = (char *)"-loglevel";
    argv[i++] = (char *)(log_is_verbose() ? "warning" : "error");
    argv[i++] = (char *)"-y";
    /* input: raw frames on stdin */
    argv[i++] = (char *)"-f";        argv[i++] = (char *)"rawvideo";
    argv[i++] = (char *)"-pix_fmt";  argv[i++] = (char *)"yuyv422";
    argv[i++] = (char *)"-s";        argv[i++] = size;
    argv[i++] = (char *)"-r";        argv[i++] = rate;
    argv[i++] = (char *)"-i";        argv[i++] = (char *)"-";
    /* output */
    argv[i++] = (char *)"-c:v";      argv[i++] = encoder;
    argv[i++] = (char *)"-pix_fmt";  argv[i++] = (char *)"yuv420p";
    argv[i++] = (char *)"-b:v";      argv[i++] = (char *)"2M";
    if (!strcmp(encoder, "libx264")) {
        argv[i++] = (char *)"-preset";
        argv[i++] = (char *)"ultrafast";
    }
    argv[i++] = (char *)"-movflags"; argv[i++] = (char *)"+faststart";
    argv[i++] = r->path;
    argv[i]   = NULL;

    if (spawn(argv, &r->pid, &r->fd) < 0)
        return -1;

    r->active      = 1;
    r->frames      = 0;
    r->started     = time(NULL);
    r->last_motion = r->started;
    return 0;
}

int recorder_write(recorder_t *r, const uint8_t *frame, size_t len)
{
    if (!r->active)
        return 0;

    if (write_all(r->fd, frame, len) < 0) {
        if (errno == EPIPE)
            log_error("ffmpeg exited early - run with --verbose to see why");
        else
            log_errno("write to encoder");
        return -1;
    }
    r->frames++;
    return 0;
}

void recorder_stop(recorder_t *r)
{
    int status;

    if (!r->active)
        return;

    /* Closing stdin is what tells ffmpeg to flush and write the moov
     * atom. Killing the process here would leave an unplayable file. */
    if (r->fd != -1) {
        close(r->fd);
        r->fd = -1;
    }

    if (r->pid > 0) {
        if (waitpid(r->pid, &status, 0) == -1)
            log_errno("waitpid");
        else if (WIFEXITED(status) && WEXITSTATUS(status) != 0)
            log_warn("ffmpeg exited with status %d", WEXITSTATUS(status));
        r->pid = -1;
    }

    r->active = 0;
}

/* ------------------------------------------------------------------ */
/* stills                                                              */
/* ------------------------------------------------------------------ */

int recorder_snapshot(const config_t *cfg, const uint8_t *frame, size_t len,
                      unsigned width, unsigned height, const char *stem)
{
    char  path[1024];
    char  size[32];
    char *argv[24];
    pid_t pid;
    int   fd, status, i = 0;

    (void)cfg;

    if (snprintf(path, sizeof path, "%s.jpg", stem) >= (int)sizeof path)
        return -1;
    snprintf(size, sizeof size, "%ux%u", width, height);

    argv[i++] = (char *)"ffmpeg";
    argv[i++] = (char *)"-hide_banner";
    argv[i++] = (char *)"-loglevel";
    argv[i++] = (char *)(log_is_verbose() ? "warning" : "error");
    argv[i++] = (char *)"-y";
    argv[i++] = (char *)"-f";       argv[i++] = (char *)"rawvideo";
    argv[i++] = (char *)"-pix_fmt"; argv[i++] = (char *)"yuyv422";
    argv[i++] = (char *)"-s";       argv[i++] = size;
    argv[i++] = (char *)"-i";       argv[i++] = (char *)"-";
    argv[i++] = (char *)"-frames:v"; argv[i++] = (char *)"1";
    argv[i++] = (char *)"-q:v";     argv[i++] = (char *)"3";
    argv[i++] = path;
    argv[i]   = NULL;

    if (spawn(argv, &pid, &fd) < 0)
        return -1;

    if (write_all(fd, frame, len) < 0)
        log_warn("snapshot write failed: %s", strerror(errno));
    close(fd);

    if (waitpid(pid, &status, 0) == -1) {
        log_errno("waitpid");
        return -1;
    }
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
        log_warn("snapshot encode failed (ffmpeg status %d)",
                 WEXITSTATUS(status));
        return -1;
    }
    return 0;
}
