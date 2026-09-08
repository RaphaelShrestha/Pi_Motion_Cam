/* ringbuf.c - fixed-capacity circular frame buffer */
#include "ringbuf.h"
#include "log.h"

#include <stdlib.h>
#include <string.h>

int ringbuf_init(ringbuf_t *rb, size_t frame_size, size_t capacity)
{
    memset(rb, 0, sizeof *rb);

    if (frame_size == 0)
        return -1;
    if (capacity == 0) {
        rb->frame_size = frame_size;   /* pre-roll disabled */
        return 0;
    }

    /* Guard against overflow before asking for the allocation. */
    if (capacity > SIZE_MAX / frame_size) {
        log_error("pre-roll buffer size overflows");
        return -1;
    }

    rb->data = malloc(capacity * frame_size);
    if (!rb->data) {
        log_error("cannot allocate %zu MB for the pre-roll buffer",
                  (capacity * frame_size) / (1024 * 1024));
        return -1;
    }

    rb->frame_size = frame_size;
    rb->capacity   = capacity;
    return 0;
}

void ringbuf_free(ringbuf_t *rb)
{
    free(rb->data);
    memset(rb, 0, sizeof *rb);
}

void ringbuf_clear(ringbuf_t *rb)
{
    rb->count = 0;
    rb->head  = 0;
}

void ringbuf_push(ringbuf_t *rb, const uint8_t *frame)
{
    if (rb->capacity == 0)
        return;

    memcpy(rb->data + rb->head * rb->frame_size, frame, rb->frame_size);
    rb->head = (rb->head + 1) % rb->capacity;
    if (rb->count < rb->capacity)
        rb->count++;
}

const uint8_t *ringbuf_at(const ringbuf_t *rb, size_t i)
{
    size_t oldest, slot;

    if (i >= rb->count)
        return NULL;

    /* Once full, head points at the oldest frame as well as the next
     * write slot; before that the oldest is simply slot 0. */
    oldest = (rb->count == rb->capacity) ? rb->head : 0;
    slot   = (oldest + i) % rb->capacity;

    return rb->data + slot * rb->frame_size;
}
