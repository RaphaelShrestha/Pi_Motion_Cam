/* ringbuf.h - fixed-capacity circular frame buffer (the pre-roll) */
#ifndef PMC_RINGBUF_H
#define PMC_RINGBUF_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t *data;
    size_t   frame_size;
    size_t   capacity;      /* frames; 0 disables the buffer */
    size_t   count;         /* frames currently retained     */
    size_t   head;          /* next write slot               */
} ringbuf_t;

int  ringbuf_init(ringbuf_t *rb, size_t frame_size, size_t capacity);
void ringbuf_free(ringbuf_t *rb);
void ringbuf_clear(ringbuf_t *rb);
void ringbuf_push(ringbuf_t *rb, const uint8_t *frame);

/* Frame i in age order: 0 is the oldest retained frame. NULL if out of range. */
const uint8_t *ringbuf_at(const ringbuf_t *rb, size_t i);

static inline size_t ringbuf_count(const ringbuf_t *rb) { return rb->count; }

#endif /* PMC_RINGBUF_H */
