/* motion.h - the motion-detection pipeline */
#ifndef PMC_MOTION_H
#define PMC_MOTION_H

#include <stddef.h>
#include <stdint.h>

#include "config.h"

typedef struct {
    /* input geometry */
    unsigned in_w, in_h;

    /* parameters, copied from config at init */
    int    downscale;
    int    threshold;
    double sensitivity;
    int    min_frames;
    double learning_rate;

    /* working geometry (input / downscale) */
    unsigned w, h;

    /* scratch buffers, each w*h bytes unless noted */
    uint8_t *small;
    uint8_t *tmp;
    uint8_t *blurred;
    uint8_t *mask;
    uint8_t *scratch;
    float   *background;     /* w*h floats: the adaptive model */
    uint8_t *ignore;         /* w*h, 255 = consider, NULL = whole frame */

    size_t active_pixels;    /* pixels not excluded by the mask */
    int    primed;           /* background model initialised     */
    int    consecutive;      /* frames over threshold in a row   */
} motion_t;

int  motion_init(motion_t *m, unsigned in_w, unsigned in_h, const config_t *cfg);
void motion_reset(motion_t *m);
void motion_free(motion_t *m);
int  motion_load_mask(motion_t *m, const char *path);

/* Returns 1 when motion has been sustained for min_frames, else 0.
 * *score_out, if non-NULL, gets the percentage of changed pixels.
 * freeze_background stops the model adapting while a clip is recording. */
int  motion_process(motion_t *m, const uint8_t *gray, double *score_out,
                    int freeze_background);

#endif /* PMC_MOTION_H */
