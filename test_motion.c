/* test_motion.c - the detector against synthetic frame sequences
 *
 * motion.c takes a grayscale buffer and its dimensions and knows nothing
 * about V4L2, which means it can be exercised without a camera. Build and
 * run with `make test`.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "log.h"
#include "motion.h"
#include "ringbuf.h"

#define W 160
#define H 120

static int failures = 0;

static void check(int cond, const char *what)
{
    printf("%-58s %s\n", what, cond ? "ok" : "FAIL");
    if (!cond)
        failures++;
}

static void fill(uint8_t *f, uint8_t v)
{
    memset(f, v, (size_t)W * H);
}

static void draw_box(uint8_t *f, int x0, int y0, int w, int h, uint8_t v)
{
    int x, y;

    for (y = y0; y < y0 + h && y < H; y++)
        for (x = x0; x < x0 + w && x < W; x++)
            f[y * W + x] = v;
}

/* Feed the same frame repeatedly so the background model settles. */
static void settle(motion_t *m, const uint8_t *f, int n)
{
    int i;
    for (i = 0; i < n; i++)
        motion_process(m, f, NULL, 0);
}

static void test_static_scene(void)
{
    config_t cfg;
    motion_t m;
    uint8_t *f = malloc(W * H);
    double   score = 0;
    int      i, triggered = 0;

    config_defaults(&cfg);
    cfg.downscale = 1;
    motion_init(&m, W, H, &cfg);

    fill(f, 120);
    for (i = 0; i < 30; i++)
        triggered |= motion_process(&m, f, &score, 0);

    check(!triggered, "static scene does not trigger");
    check(score < 0.01, "static scene scores ~0%");

    motion_free(&m);
    free(f);
}

static void test_moving_object(void)
{
    config_t cfg;
    motion_t m;
    uint8_t *bg = malloc(W * H);
    uint8_t *fg = malloc(W * H);
    double   score = 0;
    int      i, triggered = 0;

    config_defaults(&cfg);
    cfg.downscale  = 1;
    cfg.min_frames = 3;
    motion_init(&m, W, H, &cfg);

    fill(bg, 100);
    settle(&m, bg, 40);

    memcpy(fg, bg, (size_t)W * H);
    draw_box(fg, 40, 30, 40, 40, 220);      /* ~8% of the frame */

    for (i = 0; i < cfg.min_frames; i++)
        triggered = motion_process(&m, fg, &score, 1);

    check(triggered, "a large bright object triggers detection");
    check(score > cfg.sensitivity, "score exceeds the sensitivity threshold");

    motion_free(&m);
    free(bg);
    free(fg);
}

static void test_debounce(void)
{
    config_t cfg;
    motion_t m;
    uint8_t *bg = malloc(W * H);
    uint8_t *fg = malloc(W * H);
    int      first;

    config_defaults(&cfg);
    cfg.downscale  = 1;
    cfg.min_frames = 4;
    motion_init(&m, W, H, &cfg);

    fill(bg, 100);
    settle(&m, bg, 40);

    memcpy(fg, bg, (size_t)W * H);
    draw_box(fg, 40, 30, 40, 40, 220);

    first = motion_process(&m, fg, NULL, 1);
    check(!first, "a single changed frame does not trigger (min-frames 4)");

    motion_process(&m, fg, NULL, 1);
    motion_process(&m, fg, NULL, 1);
    check(motion_process(&m, fg, NULL, 1),
          "the fourth consecutive frame does trigger");

    motion_free(&m);
    free(bg);
    free(fg);
}

static void test_gradual_lighting(void)
{
    config_t cfg;
    motion_t m;
    uint8_t *f = malloc(W * H);
    int      i, triggered = 0;

    config_defaults(&cfg);
    cfg.downscale     = 1;
    cfg.learning_rate = 0.20;
    motion_init(&m, W, H, &cfg);

    /* A slow brightness ramp, the way a cloud clearing looks. The
     * adaptive background should absorb it. */
    for (i = 0; i < 120; i++) {
        fill(f, (uint8_t)(60 + i / 2));
        triggered |= motion_process(&m, f, NULL, 0);
    }

    check(!triggered, "a slow brightness ramp is absorbed, not reported");

    motion_free(&m);
    free(f);
}

static void test_noise_rejected(void)
{
    config_t cfg;
    motion_t m;
    uint8_t *f = malloc(W * H);
    int      i, j, triggered = 0;

    config_defaults(&cfg);
    cfg.downscale = 1;
    motion_init(&m, W, H, &cfg);

    fill(f, 100);
    settle(&m, f, 40);

    /* Salt-and-pepper speckle: blur plus the morphological open should
     * remove it before it reaches the score. */
    srand(1);
    for (i = 0; i < 10; i++) {
        fill(f, 100);
        for (j = 0; j < (W * H) / 200; j++)
            f[rand() % (W * H)] = 255;
        triggered |= motion_process(&m, f, NULL, 1);
    }

    check(!triggered, "isolated speckle is removed by the open operation");

    motion_free(&m);
    free(f);
}

static void test_ringbuf(void)
{
    ringbuf_t rb;
    uint8_t   frame[4];
    int       i;

    ringbuf_init(&rb, sizeof frame, 3);

    for (i = 0; i < 5; i++) {
        memset(frame, (uint8_t)i, sizeof frame);
        ringbuf_push(&rb, frame);
    }

    check(ringbuf_count(&rb) == 3, "ring buffer caps at its capacity");
    check(ringbuf_at(&rb, 0)[0] == 2, "oldest retained frame is correct");
    check(ringbuf_at(&rb, 2)[0] == 4, "newest retained frame is correct");
    check(ringbuf_at(&rb, 3) == NULL, "out-of-range index returns NULL");

    ringbuf_clear(&rb);
    check(ringbuf_count(&rb) == 0, "clear empties the buffer");

    ringbuf_free(&rb);
}

int main(void)
{
    log_set_verbose(0);

    test_static_scene();
    test_moving_object();
    test_debounce();
    test_gradual_lighting();
    test_noise_rejected();
    test_ringbuf();

    printf("\n%s\n", failures ? "FAILURES" : "all tests passed");
    return failures ? 1 : 0;
}
