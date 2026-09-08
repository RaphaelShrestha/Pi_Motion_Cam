/* config.h - runtime configuration: defaults, ini file, command line */
#ifndef PMC_CONFIG_H
#define PMC_CONFIG_H

typedef enum {
    MODE_VIDEO,
    MODE_STILLS,
    MODE_BOTH
} capture_mode_t;

typedef struct {
    /* capture */
    char device[256];
    int  width;
    int  height;
    int  fps;

    /* detection */
    int    threshold;        /* per-pixel delta, 0-255            */
    double sensitivity;      /* percent of pixels to trigger      */
    int    min_frames;       /* consecutive frames over threshold */
    int    downscale;        /* detect at 1/N resolution          */
    double learning_rate;    /* background EMA alpha              */
    char   mask[512];        /* PBM ignore mask, "" for none      */

    /* recording */
    capture_mode_t mode;
    char output[512];
    int  preroll;            /* seconds kept before an event */
    int  cooldown;           /* seconds of quiet to stop     */
    int  max_duration;       /* cap on one clip, seconds     */

    int verbose;
} config_t;

void        config_defaults(config_t *c);
int         config_load_file(config_t *c, const char *path, int required);
const char *config_peek_path(int argc, char **argv);
int         config_parse_args(config_t *c, int argc, char **argv);
int         config_validate(const config_t *c);
void        config_usage(const char *prog);

#endif /* PMC_CONFIG_H */
