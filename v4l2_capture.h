/* v4l2_capture.h - V4L2 device open, format negotiation, mmap buffer queue */
#ifndef PMC_V4L2_CAPTURE_H
#define PMC_V4L2_CAPTURE_H

#include <stddef.h>
#include <stdint.h>

#define CAPTURE_BUFFERS 4

typedef struct {
    int      fd;
    char     device[256];

    unsigned width, height;
    unsigned fps;
    size_t   bytes_per_line;   /* driver stride, >= width*2 */
    size_t   frame_size;       /* packed YUYV frame: width*height*2 */

    struct {
        void  *start;
        size_t length;
    } buffers[CAPTURE_BUFFERS];
    unsigned n_buffers;
    int      streaming;
} capture_t;

int  capture_open(capture_t *cap, const char *device,
                  unsigned width, unsigned height, unsigned fps);
int  capture_start(capture_t *cap);
void capture_stop(capture_t *cap);
void capture_close(capture_t *cap);

/* Returns 1 with a frame, 0 on timeout/EAGAIN/signal, -1 on error.
 * On 1, *data/*len point into an mmap'd buffer that must be handed back
 * with capture_release(*index) once copied out. */
int  capture_frame(capture_t *cap, const uint8_t **data, size_t *len,
                   unsigned *index);
int  capture_release(capture_t *cap, unsigned index);

/* Copy a captured buffer into a tightly packed frame, dropping stride. */
void capture_pack(const capture_t *cap, const uint8_t *src, uint8_t *dst);
/* Pull the luma plane out of packed YUYV into a width*height gray buffer. */
void capture_extract_gray(const capture_t *cap, const uint8_t *packed,
                          uint8_t *dst);

#endif /* PMC_V4L2_CAPTURE_H */
