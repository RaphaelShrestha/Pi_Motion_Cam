# Pi_Motion_Cam

A motion-detection camera for the Raspberry Pi, written in C using V4L2 for frame
capture and FFmpeg for encoding.

The program acquires frames directly from a USB webcam, detects movement by
comparing each frame against an adaptive background model, and records a clip when
motion is confirmed.

This is a personal project. I built it to understand video capture and image
processing at a lower level than a library normally exposes, so the capture and
detection paths are implemented by hand rather than delegated to OpenCV. It works
well enough for its purpose, but it isn't a security product and shouldn't be
treated as one.

## Motivation

I had previously used OpenCV in Python for a similar task. It worked, but
`cv2.VideoCapture(0)` was doing a great deal that I couldn't explain, and I wanted
to know what. This project reimplements the same idea with those layers removed:

- **Capture.** Frames come from V4L2 ioctls and `mmap`'d kernel buffers directly,
  rather than through a capture library.
- **Detection.** Blur, differencing, thresholding, and morphology are written as
  explicit loops over the pixel data — roughly 400 lines of C, no OpenCV dependency.
- **Encoding.** This is the one stage that is delegated. Raw frames are piped to an
  FFmpeg child process, which uses the Pi's hardware H.264 encoder.

## Overview

```
  USB webcam
      │
      │  VIDIOC_DQBUF — mmap'd buffer, YUYV 4:2:2
      ▼
  capture     extract the Y plane as grayscale (every other byte, no conversion)
      │
      ├──────────────► ring buffer — the last N seconds of frames
      │
      ▼
  detector    downscale → blur → |frame − background| → threshold
              → erode/dilate → count changed pixels
      │
      │  score above threshold for K consecutive frames?
      ▼
  recorder    fork/exec ffmpeg, write raw frames to its stdin
      │
      ▼
  clip.mp4 + still image
```

The ring buffer exists because detection is inherently late: by the time the score
crosses the threshold, the subject is already several frames into the shot. Buffering
a few seconds of frames and flushing them into FFmpeg first means clips begin before
the event rather than partway through it.

## Requirements

- Raspberry Pi 4 (developed on a 2GB model; also runs on a Zero 2 W at lower
  resolution)
- A UVC-compatible USB webcam exposing `YUYV`
- Raspberry Pi OS Bookworm or similar

```bash
sudo apt install build-essential ffmpeg v4l-utils
sudo usermod -aG video "$USER"   # log out and back in
```

## Building and running

```bash
git clone https://github.com/RaphaelShrestha/pi-motion-cam.git
cd pi-motion-cam
make
```

Confirm what the camera actually supports before choosing a resolution — mine
advertised modes it could not sustain:

```bash
v4l2-ctl -d /dev/video0 --list-formats-ext
```

Then:

```bash
./build/pi-motion-cam --device /dev/video0 --resolution 640x480 --fps 15 --output ./captures
```

Output is quiet apart from motion events:

```
[21:07:02] started — /dev/video0, 640x480 @ 15fps, YUYV
[21:09:41] motion  score=4.7%  → captures/2026-03-14/210941.mp4
[21:10:03] stopped after 22.1s
```

`SIGINT` and `SIGTERM` are handled so that the in-flight clip is finalized and
buffers released before exit.

## Options

| Flag | Default | Description |
|---|---|---|
| `--device` | `/dev/video0` | Capture device |
| `--resolution` | `640x480` | Capture size |
| `--fps` | `15` | Requested frame rate |
| `--output` | `./captures` | Output directory |
| `--threshold` | `25` | Per-pixel intensity delta that counts as changed (0–255) |
| `--sensitivity` | `1.5` | Percentage of changed pixels required to trigger |
| `--min-frames` | `3` | Consecutive triggering frames required |
| `--cooldown` | `5` | Seconds of quiet before recording stops |
| `--preroll` | `3` | Seconds retained from before the trigger |
| `--downscale` | `2` | Run detection at 1/N resolution; recording is unaffected |
| `--learning-rate` | `0.05` | Background adaptation rate (0–1) |
| `--mask` | — | PBM file marking regions to ignore |
| `--verbose` | off | Per-frame scores on stderr |

The same options can be set in `~/.config/pi-motion-cam/config.ini` under
`[capture]`, `[detection]`, and `[recording]`.

## Detection algorithm

For each frame:

1. **Extract luma.** YUYV packs pixels as `Y0 U Y1 V`, so the grayscale image is
   every other byte — no color conversion and no allocation.
2. **Downscale** with a box filter. At `--downscale 2` the remaining steps do a
   quarter of the work, and motion detection does not need the detail.
3. **Blur** with a separable 5×5 Gaussian. Without this, sensor noise fluctuating by
   a few intensity levels per frame is indistinguishable from real movement.
4. **Difference** against the background model: `D = |frame − background|`
5. **Threshold** into a binary mask: `M = D > threshold`
6. **Morphological open** (3×3 erode, then dilate) to remove isolated speckle while
   preserving the shape of larger moving regions.
7. **Score** as the fraction of set pixels in the mask.
8. **Trigger** when the score exceeds `sensitivity` for `min-frames` consecutive
   frames.
9. **Update the background** as an exponential moving average:
   `B ← (1−α)·B + α·frame`

Step 9 is the most consequential design decision. My first implementation compared
each frame to the previous one, which failed in two ways I hadn't anticipated: a
subject who stopped moving disappeared immediately, and gradual lighting changes
triggered constantly. An adaptive background gives a persistent reference that drifts
with the scene but not with a stationary subject in it.

Background updates are suspended while a recording is active, so a subject standing
still mid-clip is not absorbed into the model.

## Design notes and limitations

- **USB 2.0 bandwidth is a hard constraint.** Uncompressed YUYV at 720p silently
  negotiated down to roughly 8fps. The bus could not carry the data rate; the code
  was not at fault, though it took me a while to establish that.
- **Shadows remain unsolved.** A cloud crossing the sun alters a large fraction of
  pixels by a large amount, which is precisely the signature the detector looks for.
  A higher learning rate absorbs it faster, at the cost of holding stationary
  subjects for less time.
- **Low light is materially worse.** High sensor gain produces noise across the whole
  frame and effectively requires a different threshold after dark. Selecting that
  automatically is not implemented.
- **Pre-roll is memory-expensive.** Three seconds of raw 640×480 frames is roughly
  27MB, which is a real consideration on a 512MB board.
- **Tuning is scene-specific.** The defaults reflect one camera pointed at one
  window. Run with `--verbose` and observe the scores in your own scene before
  adjusting anything.
- Storage is not managed. I run `find ./captures -mtime +7 -delete` from cron.
- Tested against two webcams only; format negotiation is likely to fail on cameras
  that do not offer YUYV.

## Structure

```
src/
  main.c           argument parsing, main loop, signal handling
  v4l2_capture.c   device open, format negotiation, mmap buffer queue
  motion.c         blur, difference, threshold, morphology, scoring
  ringbuf.c        pre-roll frame buffer
  recorder.c       ffmpeg child process and pipe management
  config.c         ini parsing
```

`motion.c` has no dependency on V4L2 — it takes a grayscale buffer and its
dimensions. That separation was initially incidental, but it made the detector
testable against synthetic frame sequences rather than requiring me to move in front
of the camera for every change.

## Possible extensions

- MJPEG capture, to fit higher resolutions within the available USB bandwidth
- Connected-component labeling to identify and bound the regions that moved
- Notification on motion events rather than files on disk alone
- Automatic threshold adjustment for day and night conditions

## License

MIT. See [LICENSE](LICENSE).
