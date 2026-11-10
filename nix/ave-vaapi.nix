# apple-ave VA-API encoder driver (ave_drv_video.so): Chrome/FFmpeg H264 and
# HEVC (Main, Main 10) encoding on /dev/video2. Object IDs have bit 31 set so
# the j713 dispatcher can load it next to libva-v4l2-request.
# Host-only tests run in the build; no device is opened.
{ pkgs }:
pkgs.stdenv.mkDerivation {
  pname = "apple-ave-vaapi";
  version = "0.12.0-ave1";
  src = ../ave-vaapi;
  nativeBuildInputs = with pkgs; [ meson ninja pkg-config ];
  buildInputs = with pkgs; [ libva libdrm ];
  doCheck = true;
}
