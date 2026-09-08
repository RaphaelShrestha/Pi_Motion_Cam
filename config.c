/* config.c - defaults, ini parsing, argument parsing */
#include "config.h"
#include "log.h"

#include <ctype.h>
#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void config_defaults(config_t *c)
{
    memset(c, 0, sizeof *c);

    snprintf(c->device, sizeof c->device, "/dev/video0");
    c->width  = 640;
    c->height = 480;
    c->fps    = 15;

    c->threshold     = 25;
    c->sensitivity   = 1.5;
    c->min_frames    = 3;
    c->downscale     = 2;
    c->learning_rate = 0.05;
    c->mask[0]       = '\0';

    c->mode         = MODE_BOTH;
    snprintf(c->output, sizeof c->output, "./captures");
    c->preroll      = 3;
    c->cooldown     = 5;
    c->max_duration = 120;

    c->verbose = 0;
}

/* ------------------------------------------------------------------ */
/* ini file                                                            */
/* ------------------------------------------------------------------ */

static char *trim(char *s)
{
    char *end;

    while (*s && isspace((unsigned char)*s))
        s++;
    if (!*s)
        return s;
    end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end))
        *end-- = '\0';
    return s;
}

static int parse_resolution(const char *s, int *w, int *h)
{
    int a, b;

    if (sscanf(s, "%dx%d", &a, &b) != 2 && sscanf(s, "%d*%d", &a, &b) != 2)
        return -1;
    if (a <= 0 || b <= 0)
        return -1;
    *w = a;
    *h = b;
    return 0;
}

static int parse_mode(const char *s, capture_mode_t *out)
{
    if (!strcmp(s, "video"))  { *out = MODE_VIDEO;  return 0; }
    if (!strcmp(s, "stills")) { *out = MODE_STILLS; return 0; }
    if (!strcmp(s, "both"))   { *out = MODE_BOTH;   return 0; }
    return -1;
}

static int parse_bool(const char *s)
{
    return !strcmp(s, "1") || !strcasecmp(s, "true") ||
           !strcasecmp(s, "yes") || !strcasecmp(s, "on");
}

static int apply_kv(config_t *c, const char *key, const char *val, int lineno)
{
    if      (!strcmp(key, "device"))        snprintf(c->device, sizeof c->device, "%s", val);
    else if (!strcmp(key, "output"))        snprintf(c->output, sizeof c->output, "%s", val);
    else if (!strcmp(key, "mask"))          snprintf(c->mask,   sizeof c->mask,   "%s", val);
    else if (!strcmp(key, "fps"))           c->fps           = atoi(val);
    else if (!strcmp(key, "threshold"))     c->threshold     = atoi(val);
    else if (!strcmp(key, "sensitivity"))   c->sensitivity   = atof(val);
    else if (!strcmp(key, "min_frames"))    c->min_frames    = atoi(val);
    else if (!strcmp(key, "downscale"))     c->downscale     = atoi(val);
    else if (!strcmp(key, "learning_rate")) c->learning_rate = atof(val);
    else if (!strcmp(key, "preroll"))       c->preroll       = atoi(val);
    else if (!strcmp(key, "cooldown"))      c->cooldown      = atoi(val);
    else if (!strcmp(key, "max_duration"))  c->max_duration  = atoi(val);
    else if (!strcmp(key, "verbose"))       c->verbose       = parse_bool(val);
    else if (!strcmp(key, "resolution")) {
        if (parse_resolution(val, &c->width, &c->height) < 0) {
            log_error("config line %d: bad resolution '%s'", lineno, val);
            return -1;
        }
    } else if (!strcmp(key, "mode")) {
        if (parse_mode(val, &c->mode) < 0) {
            log_error("config line %d: bad mode '%s'", lineno, val);
            return -1;
        }
    } else {
        log_warn("config line %d: unknown key '%s', ignoring", lineno, key);
    }
    return 0;
}

int config_load_file(config_t *c, const char *path, int required)
{
    FILE *f;
    char  line[1024];
    int   lineno = 0;

    f = fopen(path, "r");
    if (!f) {
        if (errno == ENOENT && !required)
            return 0;
        log_error("cannot open config '%s': %s", path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof line, f)) {
        char *s, *eq, *key, *val, *hash;

        lineno++;
        s = trim(line);
        if (!*s || *s == ';' || *s == '#')
            continue;
        if (*s == '[')      /* section headers are decorative here */
            continue;

        /* strip trailing comments */
        hash = strchr(s, ';');
        if (hash) *hash = '\0';
        hash = strchr(s, '#');
        if (hash) *hash = '\0';

        eq = strchr(s, '=');
        if (!eq) {
            log_warn("config line %d: no '=', ignoring", lineno);
            continue;
        }
        *eq = '\0';
        key = trim(s);
        val = trim(eq + 1);

        if (apply_kv(c, key, val, lineno) < 0) {
            fclose(f);
            return -1;
        }
    }

    fclose(f);
    return 0;
}

/* ------------------------------------------------------------------ */
/* command line                                                        */
/* ------------------------------------------------------------------ */

enum {
    OPT_DEVICE = 1000, OPT_RESOLUTION, OPT_FPS, OPT_OUTPUT,
    OPT_THRESHOLD, OPT_SENSITIVITY, OPT_MINFRAMES, OPT_COOLDOWN,
    OPT_PREROLL, OPT_MAXDUR, OPT_MODE, OPT_DOWNSCALE, OPT_MASK,
    OPT_LEARNRATE, OPT_VERBOSE, OPT_CONFIG, OPT_HELP
};

static const struct option long_opts[] = {
    { "device",        required_argument, NULL, OPT_DEVICE      },
    { "resolution",    required_argument, NULL, OPT_RESOLUTION  },
    { "fps",           required_argument, NULL, OPT_FPS         },
    { "output",        required_argument, NULL, OPT_OUTPUT      },
    { "threshold",     required_argument, NULL, OPT_THRESHOLD   },
    { "sensitivity",   required_argument, NULL, OPT_SENSITIVITY },
    { "min-frames",    required_argument, NULL, OPT_MINFRAMES   },
    { "cooldown",      required_argument, NULL, OPT_COOLDOWN    },
    { "preroll",       required_argument, NULL, OPT_PREROLL     },
    { "max-duration",  required_argument, NULL, OPT_MAXDUR      },
    { "mode",          required_argument, NULL, OPT_MODE        },
    { "downscale",     required_argument, NULL, OPT_DOWNSCALE   },
    { "mask",          required_argument, NULL, OPT_MASK        },
    { "learning-rate", required_argument, NULL, OPT_LEARNRATE   },
    { "config",        required_argument, NULL, OPT_CONFIG      },
    { "verbose",       no_argument,       NULL, OPT_VERBOSE     },
    { "help",          no_argument,       NULL, OPT_HELP        },
    { NULL, 0, NULL, 0 }
};

const char *config_peek_path(int argc, char **argv)
{
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--config") || !strcmp(argv[i], "-c")) {
            if (i + 1 < argc)
                return argv[i + 1];
        } else if (!strncmp(argv[i], "--config=", 9)) {
            return argv[i] + 9;
        }
    }
    return NULL;
}

int config_parse_args(config_t *c, int argc, char **argv)
{
    int opt;

    optind = 1;
    opterr = 1;

    while ((opt = getopt_long(argc, argv, "c:vh", long_opts, NULL)) != -1) {
        switch (opt) {
        case OPT_DEVICE:
            snprintf(c->device, sizeof c->device, "%s", optarg);
            break;
        case OPT_RESOLUTION:
            if (parse_resolution(optarg, &c->width, &c->height) < 0) {
                log_error("bad --resolution '%s' (expected WxH)", optarg);
                return -1;
            }
            break;
        case OPT_FPS:           c->fps           = atoi(optarg); break;
        case OPT_OUTPUT:
            snprintf(c->output, sizeof c->output, "%s", optarg);
            break;
        case OPT_THRESHOLD:     c->threshold     = atoi(optarg); break;
        case OPT_SENSITIVITY:   c->sensitivity   = atof(optarg); break;
        case OPT_MINFRAMES:     c->min_frames    = atoi(optarg); break;
        case OPT_COOLDOWN:      c->cooldown      = atoi(optarg); break;
        case OPT_PREROLL:       c->preroll       = atoi(optarg); break;
        case OPT_MAXDUR:        c->max_duration  = atoi(optarg); break;
        case OPT_DOWNSCALE:     c->downscale     = atoi(optarg); break;
        case OPT_LEARNRATE:     c->learning_rate = atof(optarg); break;
        case OPT_MASK:
            snprintf(c->mask, sizeof c->mask, "%s", optarg);
            break;
        case OPT_MODE:
            if (parse_mode(optarg, &c->mode) < 0) {
                log_error("bad --mode '%s' (video|stills|both)", optarg);
                return -1;
            }
            break;
        case 'c':
        case OPT_CONFIG:
            break;                          /* handled by config_peek_path */
        case 'v':
        case OPT_VERBOSE:
            c->verbose = 1;
            break;
        case 'h':
        case OPT_HELP:
            config_usage(argv[0]);
            return 1;
        default:
            return -1;
        }
    }

    if (optind < argc) {
        log_error("unexpected argument '%s'", argv[optind]);
        return -1;
    }
    return 0;
}

int config_validate(const config_t *c)
{
    int ok = 1;

    if (c->width < 32 || c->height < 32) {
        log_error("resolution %dx%d is too small", c->width, c->height);
        ok = 0;
    }
    if (c->width % 2 || c->height % 2) {
        log_error("resolution must be even in both dimensions (YUYV)");
        ok = 0;
    }
    if (c->fps < 1 || c->fps > 120) {
        log_error("--fps must be between 1 and 120");
        ok = 0;
    }
    if (c->threshold < 0 || c->threshold > 255) {
        log_error("--threshold must be between 0 and 255");
        ok = 0;
    }
    if (c->sensitivity <= 0.0 || c->sensitivity > 100.0) {
        log_error("--sensitivity must be between 0 and 100 (percent)");
        ok = 0;
    }
    if (c->min_frames < 1) {
        log_error("--min-frames must be at least 1");
        ok = 0;
    }
    if (c->downscale < 1 || c->downscale > 16) {
        log_error("--downscale must be between 1 and 16");
        ok = 0;
    }
    if (c->learning_rate < 0.0 || c->learning_rate > 1.0) {
        log_error("--learning-rate must be between 0 and 1");
        ok = 0;
    }
    if (c->preroll < 0 || c->preroll > 30) {
        log_error("--preroll must be between 0 and 30 seconds");
        ok = 0;
    }
    if (c->cooldown < 0) {
        log_error("--cooldown cannot be negative");
        ok = 0;
    }
    if (c->max_duration < 1) {
        log_error("--max-duration must be at least 1 second");
        ok = 0;
    }
    if (c->width / c->downscale < 16 || c->height / c->downscale < 16) {
        log_error("--downscale %d leaves too little detail at %dx%d",
                  c->downscale, c->width, c->height);
        ok = 0;
    }
    return ok ? 0 : -1;
}

void config_usage(const char *prog)
{
    printf(
"pi-motion-cam - motion-detection camera for the Raspberry Pi\n"
"\n"
"usage: %s [options]\n"
"\n"
"capture:\n"
"  --device PATH          capture device            (default /dev/video0)\n"
"  --resolution WxH       capture size              (default 640x480)\n"
"  --fps N                requested frame rate      (default 15)\n"
"\n"
"detection:\n"
"  --threshold N          per-pixel delta, 0-255    (default 25)\n"
"  --sensitivity F        %% of pixels to trigger    (default 1.5)\n"
"  --min-frames N         consecutive frames        (default 3)\n"
"  --downscale N          detect at 1/N resolution  (default 2)\n"
"  --learning-rate F      background EMA alpha      (default 0.05)\n"
"  --mask FILE            PBM ignore mask           (default none)\n"
"\n"
"recording:\n"
"  --output DIR           output directory          (default ./captures)\n"
"  --mode MODE            video|stills|both         (default both)\n"
"  --preroll N            seconds kept before event (default 3)\n"
"  --cooldown N           seconds of quiet to stop  (default 5)\n"
"  --max-duration N       cap on one clip, seconds  (default 120)\n"
"\n"
"other:\n"
"  -c, --config FILE      ini file to read first\n"
"  -v, --verbose          print per-frame scores\n"
"  -h, --help             this message\n"
"\n"
"Flags override the config file. With no --config, ~/.config/pi-motion-cam/\n"
"config.ini is read if it exists.\n", prog);
}
