/* motion.c - the detection pipeline
 *
 * Per frame:
 *   1. downscale with a box filter
 *   2. blur with a separable 5x5 Gaussian
 *   3. difference against the background model
 *   4. threshold into a binary mask
 *   5. morphological open (erode then dilate)
 *   6. score as the fraction of set pixels
 *   7. trigger after min_frames consecutive frames over the threshold
 *   8. update the background as an exponential moving average
 *
 * Step 8 is the part that matters. Comparing each frame to the previous
 * one instead fails twice over: a subject who stops moving disappears
 * immediately, and gradual lighting changes trigger constantly. An
 * adaptive background gives a reference that drifts with the scene but
 * not with a stationary subject in it.
 */
#include "motion.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* stage 1: downscale                                                  */
/* ------------------------------------------------------------------ */

static void downscale(const uint8_t *src, unsigned sw, unsigned sh,
                      uint8_t *dst, unsigned dw, unsigned dh, int n)
{
    unsigned x, y;
    int i, j;
    unsigned area = (unsigned)n * (unsigned)n;

    if (n == 1) {
        memcpy(dst, src, (size_t)sw * sh);
        return;
    }

    for (y = 0; y < dh; y++) {
        for (x = 0; x < dw; x++) {
            unsigned sum = 0;

            for (j = 0; j < n; j++) {
                unsigned sy = y * n + j;
                const uint8_t *row;

                if (sy >= sh)
                    sy = sh - 1;
                row = src + (size_t)sy * sw;

                for (i = 0; i < n; i++) {
                    unsigned sx = x * n + i;
                    if (sx >= sw)
                        sx = sw - 1;
                    sum += row[sx];
                }
            }
            dst[(size_t)y * dw + x] = (uint8_t)(sum / area);
        }
    }
}

/* ------------------------------------------------------------------ */
/* stage 2: separable 5x5 Gaussian, kernel [1 4 6 4 1] / 16            */
/* ------------------------------------------------------------------ */

static void blur5(const uint8_t *src, uint8_t *tmp, uint8_t *dst,
                  unsigned w, unsigned h)
{
    static const int k[5] = { 1, 4, 6, 4, 1 };
    unsigned x, y;
    int t;

    /* horizontal */
    for (y = 0; y < h; y++) {
        const uint8_t *srow = src + (size_t)y * w;
        uint8_t       *trow = tmp + (size_t)y * w;

        for (x = 0; x < w; x++) {
            int sum = 0;

            for (t = -2; t <= 2; t++) {
                long sx = (long)x + t;

                if (sx < 0)            sx = 0;
                if (sx >= (long)w)     sx = (long)w - 1;
                sum += k[t + 2] * srow[sx];
            }
            trow[x] = (uint8_t)(sum >> 4);
        }
    }

    /* vertical */
    for (y = 0; y < h; y++) {
        uint8_t *drow = dst + (size_t)y * w;

        for (x = 0; x < w; x++) {
            int sum = 0;

            for (t = -2; t <= 2; t++) {
                long sy = (long)y + t;

                if (sy < 0)            sy = 0;
                if (sy >= (long)h)     sy = (long)h - 1;
                sum += k[t + 2] * tmp[(size_t)sy * w + x];
            }
            drow[x] = (uint8_t)(sum >> 4);
        }
    }
}

/* ------------------------------------------------------------------ */
/* stage 5: morphology on a binary (0 / 255) mask                      */
/* ------------------------------------------------------------------ */

static void morph3x3(const uint8_t *src, uint8_t *dst,
                     unsigned w, unsigned h, int erode)
{
    unsigned x, y;
    int i, j;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            uint8_t out = erode ? 255 : 0;

            for (j = -1; j <= 1 && (erode ? out : !out); j++) {
                long sy = (long)y + j;

                if (sy < 0)        sy = 0;
                if (sy >= (long)h) sy = (long)h - 1;

                for (i = -1; i <= 1; i++) {
                    long sx = (long)x + i;
                    uint8_t v;

                    if (sx < 0)        sx = 0;
                    if (sx >= (long)w) sx = (long)w - 1;

                    v = src[(size_t)sy * w + sx];
                    if (erode) {
                        if (!v) { out = 0; break; }
                    } else {
                        if (v)  { out = 255; break; }
                    }
                }
            }
            dst[(size_t)y * w + x] = out;
        }
    }
}

/* ------------------------------------------------------------------ */
/* setup                                                               */
/* ------------------------------------------------------------------ */

int motion_init(motion_t *m, unsigned in_w, unsigned in_h, const config_t *cfg)
{
    size_t n;

    memset(m, 0, sizeof *m);

    m->in_w          = in_w;
    m->in_h          = in_h;
    m->downscale     = cfg->downscale;
    m->threshold     = cfg->threshold;
    m->sensitivity   = cfg->sensitivity;
    m->min_frames    = cfg->min_frames;
    m->learning_rate = cfg->learning_rate;

    m->w = in_w / (unsigned)cfg->downscale;
    m->h = in_h / (unsigned)cfg->downscale;
    if (m->w == 0 || m->h == 0) {
        log_error("downscale factor %d is too large for %ux%u",
                  cfg->downscale, in_w, in_h);
        return -1;
    }

    n = (size_t)m->w * m->h;

    m->small      = malloc(n);
    m->tmp        = malloc(n);
    m->blurred    = malloc(n);
    m->mask       = malloc(n);
    m->scratch    = malloc(n);
    m->background = malloc(n * sizeof *m->background);

    if (!m->small || !m->tmp || !m->blurred || !m->mask ||
        !m->scratch || !m->background) {
        log_error("out of memory allocating detector buffers");
        motion_free(m);
        return -1;
    }

    m->ignore        = NULL;
    m->active_pixels = n;
    m->primed        = 0;
    m->consecutive   = 0;

    log_verbose("detector: %ux%u working resolution, threshold %d, "
                "sensitivity %.2f%%, alpha %.3f",
                m->w, m->h, m->threshold, m->sensitivity, m->learning_rate);
    return 0;
}

void motion_reset(motion_t *m)
{
    m->primed      = 0;
    m->consecutive = 0;
}

void motion_free(motion_t *m)
{
    free(m->small);
    free(m->tmp);
    free(m->blurred);
    free(m->mask);
    free(m->scratch);
    free(m->background);
    free(m->ignore);
    memset(m, 0, sizeof *m);
}

/* ------------------------------------------------------------------ */
/* PBM ignore mask                                                     */
/* ------------------------------------------------------------------ */

static int pbm_next_int(FILE *f, int *out)
{
    int c, val = 0, digits = 0;

    for (;;) {
        c = fgetc(f);
        if (c == EOF)
            return -1;
        if (c == '#') {                     /* comment to end of line */
            while (c != '\n' && c != EOF)
                c = fgetc(f);
            continue;
        }
        if (isspace(c)) {
            if (digits) break;
            continue;
        }
        if (!isdigit(c))
            return -1;
        val = val * 10 + (c - '0');
        digits = 1;
    }

    if (!digits)
        return -1;
    *out = val;
    return 0;
}

int motion_load_mask(motion_t *m, const char *path)
{
    FILE    *f;
    char     magic[3] = { 0 };
    int      mw, mh;
    uint8_t *full = NULL;
    size_t   n, i;
    unsigned x, y;

    f = fopen(path, "rb");
    if (!f) {
        log_error("cannot open mask '%s': %s", path, strerror(errno));
        return -1;
    }

    if (fread(magic, 1, 2, f) != 2 ||
        magic[0] != 'P' || (magic[1] != '1' && magic[1] != '4')) {
        log_error("mask '%s' is not a PBM (P1 or P4) file", path);
        fclose(f);
        return -1;
    }

    if (pbm_next_int(f, &mw) < 0 || pbm_next_int(f, &mh) < 0 ||
        mw <= 0 || mh <= 0) {
        log_error("mask '%s': bad header", path);
        fclose(f);
        return -1;
    }

    full = malloc((size_t)mw * mh);
    if (!full) {
        fclose(f);
        return -1;
    }

    if (magic[1] == '1') {
        /* ASCII: one '0' or '1' per pixel, whitespace-separated */
        for (i = 0; i < (size_t)mw * mh; i++) {
            int c;

            do {
                c = fgetc(f);
                if (c == '#')
                    while (c != '\n' && c != EOF)
                        c = fgetc(f);
            } while (c != EOF && isspace(c));

            if (c != '0' && c != '1') {
                log_error("mask '%s': truncated or malformed pixel data", path);
                free(full);
                fclose(f);
                return -1;
            }
            full[i] = (c == '1');
        }
    } else {
        /* Binary: rows padded to a byte boundary, MSB first. Exactly one
         * whitespace character separates the header from the data. */
        size_t row_bytes = ((size_t)mw + 7) / 8;
        uint8_t *row = malloc(row_bytes);

        if (!row) {
            free(full);
            fclose(f);
            return -1;
        }

        for (y = 0; y < (unsigned)mh; y++) {
            if (fread(row, 1, row_bytes, f) != row_bytes) {
                log_error("mask '%s': truncated pixel data", path);
                free(row);
                free(full);
                fclose(f);
                return -1;
            }
            for (x = 0; x < (unsigned)mw; x++)
                full[(size_t)y * mw + x] = (row[x / 8] >> (7 - (x % 8))) & 1;
        }
        free(row);
    }
    fclose(f);

    /* Nearest-neighbour resample to the detector's working resolution. */
    n = (size_t)m->w * m->h;
    free(m->ignore);
    m->ignore = malloc(n);
    if (!m->ignore) {
        free(full);
        return -1;
    }

    m->active_pixels = 0;
    for (y = 0; y < m->h; y++) {
        unsigned sy = (unsigned)((uint64_t)y * mh / m->h);

        for (x = 0; x < m->w; x++) {
            unsigned sx = (unsigned)((uint64_t)x * mw / m->w);
            /* In PBM, 1 is black. Black means ignore. */
            uint8_t consider = full[(size_t)sy * mw + sx] ? 0 : 255;

            m->ignore[(size_t)y * m->w + x] = consider;
            if (consider)
                m->active_pixels++;
        }
    }
    free(full);

    if (m->active_pixels == 0) {
        log_error("mask '%s' excludes the entire frame", path);
        return -1;
    }

    log_info("mask loaded: %ux%u, %.1f%% of the frame considered",
             mw, mh, 100.0 * (double)m->active_pixels / (double)n);
    return 0;
}

/* ------------------------------------------------------------------ */
/* the pipeline                                                        */
/* ------------------------------------------------------------------ */

int motion_process(motion_t *m, const uint8_t *gray, double *score_out,
                   int freeze_background)
{
    size_t n = (size_t)m->w * m->h;
    size_t i, changed = 0;
    double score;
    float  alpha = (float)m->learning_rate;

    /* 1. downscale */
    downscale(gray, m->in_w, m->in_h, m->small, m->w, m->h, m->downscale);

    /* 2. blur - without this, sensor noise fluctuating by a few levels
     *    per frame is indistinguishable from real movement */
    blur5(m->small, m->tmp, m->blurred, m->w, m->h);

    /* First frame: prime the model and report nothing. */
    if (!m->primed) {
        for (i = 0; i < n; i++)
            m->background[i] = (float)m->blurred[i];
        m->primed      = 1;
        m->consecutive = 0;
        if (score_out)
            *score_out = 0.0;
        return 0;
    }

    /* 3 + 4. difference against the model, then threshold */
    for (i = 0; i < n; i++) {
        float diff = (float)m->blurred[i] - m->background[i];

        if (diff < 0.0f)
            diff = -diff;
        m->mask[i] = (diff > (float)m->threshold) ? 255 : 0;
    }

    /* 5. open: erode removes isolated speckle, dilate restores the
     *    surviving regions to roughly their original extent */
    morph3x3(m->mask, m->scratch, m->w, m->h, 1);
    morph3x3(m->scratch, m->mask, m->w, m->h, 0);

    /* 6. score over the considered pixels only */
    if (m->ignore) {
        for (i = 0; i < n; i++)
            if (m->mask[i] && m->ignore[i])
                changed++;
    } else {
        for (i = 0; i < n; i++)
            if (m->mask[i])
                changed++;
    }

    score = 100.0 * (double)changed / (double)m->active_pixels;
    if (score_out)
        *score_out = score;

    /* 8. background update, unless a recording is in progress */
    if (!freeze_background && alpha > 0.0f) {
        for (i = 0; i < n; i++)
            m->background[i] = (1.0f - alpha) * m->background[i] +
                               alpha * (float)m->blurred[i];
    }

    /* 7. debounce */
    if (score >= m->sensitivity) {
        if (m->consecutive < m->min_frames)
            m->consecutive++;
    } else {
        m->consecutive = 0;
    }

    return m->consecutive >= m->min_frames;
}
