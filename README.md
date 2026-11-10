# aurora-ave-vaapi (placeholder)

**Status: experimental placeholder.** This repo exists so the code can be reviewed
while the Aurora maintainers decide where it should live upstream. It may be
merged into another project, renamed, or moved.

Hardware video **encoding** through VA-API on Apple Silicon M4 (tested on the
MacBook Air M4, J713 / T8132), for Chrome and FFmpeg, using the `apple-ave`
stateful V4L2 encoder kernel driver.

| Directory | What |
|---|---|
| `ave-vaapi/` | `ave_drv_video.so`: VA-API encoder driver. H.264 Constrained Baseline/Main/High and HEVC Main/Main 10, `VAEntrypointEncSlice`, CBR/VBR. Port of [snowf14k3/venus-vaapi-driver](https://github.com/snowf14k3/venus-vaapi-driver) (MIT) at 37e1030. VA object IDs have bit 31 set. |
| `va-dispatch/` | `j713_drv_video.so`: one VA driver that loads the decoder ([libva-v4l2_request](https://github.com/omacom/libva-v4l2_request), Apple AVD) and this encoder and routes every call. Needed because libva and Chrome load exactly one VA driver per process. Routing rules are documented at the top of `j713_dispatch.c`. |
| `nix/` | The Nix packages used for testing. |

## Build and test (host only, opens no device)

```sh
cd ave-vaapi && meson setup build && meson test -C build
make -C va-dispatch check
make -C va-dispatch DEC=/path/to/v4l2_request_drv_video.so ENC=/path/to/ave_drv_video.so
```

Use: `LIBVA_DRIVER_NAME=j713` with `j713_drv_video.so` in the libva driver path.
Chrome also needs `--enable-features=AcceleratedVideoEncoder,AcceleratedVideoDecoder,VaapiVideoDecoder`
and a Mesa with GBM NV12 allocation support (aurora-silicon/mesa change, pending).

## Tested on M4 Air (2026-11-10)

- FFmpeg `h264_vaapi` / `hevc_vaapi` (Main and Main 10): encoded output decoded and compared by PSNR against the source.
- Chrome 156 hardware encode, H.264 High 720p 44.7 dB, Constrained Baseline 43.6 dB, Main 1080p 45.5 dB (PSNR vs raw input).
- Concurrent hardware decode and encode in one Chrome process through the dispatcher.

## Known gaps

- No VUI colour description in the H.264 output.
- The encoder driver still carries the Venus decode entry points (hidden by the dispatcher).
- FFmpeg HEVC encode through the dispatcher is not supported (FFmpeg creates unhinted surfaces); use `LIBVA_DRIVER_NAME=ave` directly.
- "j713" naming is historical; the dispatcher is SoC-agnostic.

## Open question for maintainers

Should encoding go into libva-v4l2_request (one driver, no dispatcher), or stay a
separate driver plus this dispatcher installed as `aurora_drv_video.so`?

Developed with AI assistance (Claude). Tested on the author's hardware.

## License

MIT, see `LICENSE`. Copyright notices of the original venus-vaapi-driver are kept.
