# Apple AVE port (encode only) — experimental

This tree is a port of snowf14k3/venus-vaapi-driver
(37e1030aba11f66e303e4f5fb0c3a13e2a78265e) to the Apple AVE stateful V4L2
encoder (`apple-ave` kernel driver).  It builds `ave_drv_video.so`
(LIBVA_DRIVER_NAME=ave), exposes H.264 CB/Main/High and HEVC Main/Main10
VAEntrypointEncSlice with CBR/VBR, no packed headers, one slice, one
reference, and no decoding.  See ../README.md for the analysis and the
hardware test plan.  The original README follows.

# venus-vaapi-driver

VA-API hardware video driver for the Qualcomm Venus codec on the Xiaomi Redmi
K20 Pro / Mi 9T Pro (Raphael, SM8150).

## Support

| Codec | Decode | Encode |
| --- | --- | --- |
| H.264 8-bit | Baseline, Main, High | Baseline, Main, High |
| HEVC Main 8-bit | Experimental (opt-in) | Available |
| HEVC Main10 | Experimental (opt-in) | Available |
| VP8 | Available | Available |
| VP9 Profile 0 | Available | Not supported |

Raphael hardware tests decoded 45 consecutive HEVC Main and Main10 frames
pixel-identically to software. Main10 uses P010 surfaces. To expose HEVC
hardware decoding for one process, set
`VENUS_VAAPI_EXPERIMENTAL_HEVC_DECODE=1`. On the tested Raphael 7.1 kernel,
after an HEVC hardware decode session, Venus encoding can fail until reboot,
so HEVC decoding stays disabled by default. Encoding supports I/P frames;
B-frame encoding and VP9 encoding are not implemented. The driver supports
DMA-BUF export. FFmpeg may align a 1080-high HEVC encoding context to 1088
coded lines; clients should present the visible height.

## Install

On Debian 13:

```bash
sudo apt install build-essential meson ninja-build pkg-config libva-dev libdrm-dev vainfo
./scripts/install.sh
LIBVA_DRIVER_NAME=venus vainfo --display drm --device /dev/dri/renderD128
```

Licensed under MIT.
