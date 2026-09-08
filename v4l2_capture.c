/* v4l2_capture.c - device open, format negotiation, mmap buffer queue
 *
 * The streaming I/O flow is the standard one:
 *   VIDIOC_QUERYCAP -> VIDIOC_S_FMT -> VIDIOC_S_PARM -> VIDIOC_REQBUFS
 *   -> VIDIOC_QUERYBUF + mmap (per buffer) -> VIDIOC_QBUF (all)
 *   -> VIDIOC_STREAMON -> { select, VIDIOC_DQBUF, use, VIDIOC_QBUF }
 */
#include "v4l2_capture.h"
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <linux/videodev2.h>

static int xioctl(int fd, unsigned long request, void *arg)
{
    int r;

    do {
        r = ioctl(fd, request, arg);
    } while (r == -1 && errno == EINTR);

    return r;
}

static void fourcc_str(uint32_t f, char out[5])
{
    out[0] = (char)( f        & 0xff);
    out[1] = (char)((f >>  8) & 0xff);
    out[2] = (char)((f >> 16) & 0xff);
    out[3] = (char)((f >> 24) & 0xff);
    out[4] = '\0';
}

int capture_open(capture_t *cap, const char *device,
                 unsigned width, unsigned height, unsigned fps)
{
    struct v4l2_capability   vcap;
    struct v4l2_format       fmt;
    struct v4l2_streamparm   parm;
    struct v4l2_requestbuffers req;
    struct stat st;
    unsigned i;

    memset(cap, 0, sizeof *cap);
    cap->fd = -1;
    snprintf(cap->device, sizeof cap->device, "%s", device);

    if (stat(device, &st) == -1) {
        log_error("%s: %s", device, strerror(errno));
        return -1;
    }
    if (!S_ISCHR(st.st_mode)) {
        log_error("%s is not a character device", device);
        return -1;
    }

    cap->fd = open(device, O_RDWR | O_NONBLOCK, 0);
    if (cap->fd == -1) {
        log_error("open %s: %s", device, strerror(errno));
        if (errno == EACCES)
            log_error("hint: add yourself to the 'video' group and log back in");
        return -1;
    }

    /* --- capability check ---------------------------------------- */
    memset(&vcap, 0, sizeof vcap);
    if (xioctl(cap->fd, VIDIOC_QUERYCAP, &vcap) == -1) {
        if (errno == EINVAL)
            log_error("%s is not a V4L2 device", device);
        else
            log_errno("VIDIOC_QUERYCAP");
        goto fail;
    }
    if (!(vcap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        log_error("%s does not support video capture", device);
        goto fail;
    }
    if (!(vcap.capabilities & V4L2_CAP_STREAMING)) {
        log_error("%s does not support streaming I/O", device);
        goto fail;
    }

    /* --- format ---------------------------------------------------
     * YUYV only. The luma plane of a packed YUYV frame is every other
     * byte, so grayscale comes free; MJPEG would need a decode step
     * that this program does not have. */
    memset(&fmt, 0, sizeof fmt);
    fmt.type                = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width       = width;
    fmt.fmt.pix.height      = height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field       = V4L2_FIELD_ANY;

    if (xioctl(cap->fd, VIDIOC_S_FMT, &fmt) == -1) {
        log_errno("VIDIOC_S_FMT");
        log_error("hint: v4l2-ctl -d %s --list-formats-ext", device);
        goto fail;
    }

    if (fmt.fmt.pix.pixelformat != V4L2_PIX_FMT_YUYV) {
        char got[5];
        fourcc_str(fmt.fmt.pix.pixelformat, got);
        log_error("camera does not offer YUYV (driver chose %s)", got);
        log_error("hint: v4l2-ctl -d %s --list-formats-ext", device);
        goto fail;
    }

    if (fmt.fmt.pix.width != width || fmt.fmt.pix.height != height)
        log_warn("driver adjusted resolution to %ux%u",
                 fmt.fmt.pix.width, fmt.fmt.pix.height);

    cap->width          = fmt.fmt.pix.width;
    cap->height         = fmt.fmt.pix.height;
    cap->bytes_per_line = fmt.fmt.pix.bytesperline;
    cap->frame_size     = (size_t)cap->width * cap->height * 2;

    if (cap->bytes_per_line < (size_t)cap->width * 2) {
        log_error("driver reported an implausible stride (%zu)",
                  cap->bytes_per_line);
        goto fail;
    }

    /* --- frame rate ----------------------------------------------- */
    memset(&parm, 0, sizeof parm);
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator   = 1;
    parm.parm.capture.timeperframe.denominator = fps;

    if (xioctl(cap->fd, VIDIOC_S_PARM, &parm) == -1) {
        log_warn("VIDIOC_S_PARM failed, using whatever the driver defaults to");
        cap->fps = fps;
    } else if (parm.parm.capture.timeperframe.numerator > 0) {
        cap->fps = parm.parm.capture.timeperframe.denominator /
                   parm.parm.capture.timeperframe.numerator;
        if (cap->fps != fps)
            log_warn("driver negotiated %u fps rather than %u "
                     "(often a USB bandwidth limit)", cap->fps, fps);
    } else {
        cap->fps = fps;
    }
    if (cap->fps == 0)
        cap->fps = fps;

    /* --- buffers --------------------------------------------------- */
    memset(&req, 0, sizeof req);
    req.count  = CAPTURE_BUFFERS;
    req.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(cap->fd, VIDIOC_REQBUFS, &req) == -1) {
        if (errno == EINVAL)
            log_error("%s does not support mmap streaming", device);
        else
            log_errno("VIDIOC_REQBUFS");
        goto fail;
    }
    if (req.count < 2) {
        log_error("insufficient buffer memory on %s", device);
        goto fail;
    }

    for (i = 0; i < req.count && i < CAPTURE_BUFFERS; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof buf);
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (xioctl(cap->fd, VIDIOC_QUERYBUF, &buf) == -1) {
            log_errno("VIDIOC_QUERYBUF");
            goto fail;
        }

        cap->buffers[i].length = buf.length;
        cap->buffers[i].start  = mmap(NULL, buf.length,
                                      PROT_READ | PROT_WRITE, MAP_SHARED,
                                      cap->fd, buf.m.offset);
        if (cap->buffers[i].start == MAP_FAILED) {
            log_errno("mmap");
            cap->buffers[i].start = NULL;
            goto fail;
        }
        cap->n_buffers++;
    }

    log_info("started - %s, %ux%u @ %ufps, YUYV",
             cap->device, cap->width, cap->height, cap->fps);
    return 0;

fail:
    capture_close(cap);
    return -1;
}

int capture_start(capture_t *cap)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    unsigned i;

    for (i = 0; i < cap->n_buffers; i++) {
        struct v4l2_buffer buf;

        memset(&buf, 0, sizeof buf);
        buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index  = i;

        if (xioctl(cap->fd, VIDIOC_QBUF, &buf) == -1) {
            log_errno("VIDIOC_QBUF");
            return -1;
        }
    }

    if (xioctl(cap->fd, VIDIOC_STREAMON, &type) == -1) {
        log_errno("VIDIOC_STREAMON");
        return -1;
    }

    cap->streaming = 1;
    return 0;
}

void capture_stop(capture_t *cap)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    if (!cap->streaming || cap->fd == -1)
        return;
    if (xioctl(cap->fd, VIDIOC_STREAMOFF, &type) == -1)
        log_errno("VIDIOC_STREAMOFF");
    cap->streaming = 0;
}

void capture_close(capture_t *cap)
{
    unsigned i;

    capture_stop(cap);

    for (i = 0; i < cap->n_buffers; i++) {
        if (cap->buffers[i].start && cap->buffers[i].start != MAP_FAILED) {
            if (munmap(cap->buffers[i].start, cap->buffers[i].length) == -1)
                log_errno("munmap");
        }
        cap->buffers[i].start  = NULL;
        cap->buffers[i].length = 0;
    }
    cap->n_buffers = 0;

    if (cap->fd != -1) {
        close(cap->fd);
        cap->fd = -1;
    }
}

int capture_frame(capture_t *cap, const uint8_t **data, size_t *len,
                  unsigned *index)
{
    struct v4l2_buffer buf;
    struct timeval tv;
    fd_set fds;
    int r;

    FD_ZERO(&fds);
    FD_SET(cap->fd, &fds);
    tv.tv_sec  = 2;
    tv.tv_usec = 0;

    r = select(cap->fd + 1, &fds, NULL, NULL, &tv);
    if (r == -1) {
        if (errno == EINTR)
            return 0;              /* a signal arrived; caller re-checks */
        log_errno("select");
        return -1;
    }
    if (r == 0) {
        log_warn("no frame for 2s - is the camera still attached?");
        return 0;
    }

    memset(&buf, 0, sizeof buf);
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (xioctl(cap->fd, VIDIOC_DQBUF, &buf) == -1) {
        switch (errno) {
        case EAGAIN:
            return 0;
        case EIO:
            /* Some drivers signal a recoverable transfer error this way. */
            log_warn("VIDIOC_DQBUF returned EIO, continuing");
            return 0;
        default:
            log_errno("VIDIOC_DQBUF");
            return -1;
        }
    }

    if (buf.index >= cap->n_buffers) {
        log_error("driver returned buffer index %u out of range", buf.index);
        return -1;
    }

    *data  = (const uint8_t *)cap->buffers[buf.index].start;
    *len   = buf.bytesused ? buf.bytesused : cap->buffers[buf.index].length;
    *index = buf.index;
    return 1;
}

int capture_release(capture_t *cap, unsigned index)
{
    struct v4l2_buffer buf;

    memset(&buf, 0, sizeof buf);
    buf.type   = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index  = index;

    if (xioctl(cap->fd, VIDIOC_QBUF, &buf) == -1) {
        log_errno("VIDIOC_QBUF");
        return -1;
    }
    return 0;
}

void capture_pack(const capture_t *cap, const uint8_t *src, uint8_t *dst)
{
    size_t row_bytes = (size_t)cap->width * 2;
    unsigned y;

    if (cap->bytes_per_line == row_bytes) {
        memcpy(dst, src, cap->frame_size);
        return;
    }

    for (y = 0; y < cap->height; y++)
        memcpy(dst + y * row_bytes, src + y * cap->bytes_per_line, row_bytes);
}

void capture_extract_gray(const capture_t *cap, const uint8_t *packed,
                          uint8_t *dst)
{
    /* YUYV stores Y0 U Y1 V, so luma is every other byte. No conversion,
     * no allocation - this is why the format is worth insisting on. */
    size_t n = (size_t)cap->width * cap->height;
    size_t i;

    for (i = 0; i < n; i++)
        dst[i] = packed[i * 2];
}
