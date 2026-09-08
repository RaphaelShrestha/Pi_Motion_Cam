/* recorder.h - ffmpeg child process, clip and snapshot output */
#ifndef PMC_RECORDER_H
#define PMC_RECORDER_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#include "config.h"

typedef struct {
    int   active;
    int   fd;               /* write end of the pipe to ffmpeg stdin */
    pid_t pid;
    char  path[1024];       /* the .mp4 currently being written      */

    unsigned long frames;
    time_t started;
    time_t last_motion;
} recorder_t;

const char *recorder_encoder(void);
int  recorder_probe(void);
int  recorder_make_stem(const config_t *cfg, char *out, size_t outlen);

void recorder_init(recorder_t *r);
int  recorder_start(recorder_t *r, const config_t *cfg,
                    unsigned width, unsigned height, unsigned fps);
int  recorder_write(recorder_t *r, const uint8_t *frame, size_t len);
void recorder_stop(recorder_t *r);

int  recorder_snapshot(const config_t *cfg, const uint8_t *frame, size_t len,
                       unsigned width, unsigned height, const char *stem);

#endif /* PMC_RECORDER_H */
