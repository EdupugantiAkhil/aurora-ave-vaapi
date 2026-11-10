# j713_drv_video.so: one VA driver for decode (libva-v4l2-request, AVD) and
# encode (apple-ave VA driver, AVE). See j713_dispatch.c for the routing rules.
{ pkgs, decoder, encoder }:
let
  dec = "${decoder}/lib/dri/v4l2_request_drv_video.so";
  enc = "${encoder}/lib/dri/ave_drv_video.so";
  cflags = "-std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -O2 -fPIC";
in pkgs.stdenv.mkDerivation {
  pname = "j713-va-dispatch";
  version = "0.1";
  src = ../va-dispatch;
  buildInputs = [ pkgs.libva ];
  buildPhase = ''
    runHook preBuild
    $CC ${cflags} -shared -fvisibility=hidden \
      -DJ713_DEC_DRIVER='"${dec}"' -DJ713_ENC_DRIVER='"${enc}"' \
      -o j713_drv_video.so j713_dispatch.c -ldl -lva
    runHook postBuild
  '';
  doCheck = true;
  checkPhase = ''
    runHook preCheck
    $CC -Wall -O2 -fPIC -shared -DFAKE_ENC=0 -o fake_dec.so tests/fake_drv.c
    $CC -Wall -O2 -fPIC -shared -DFAKE_ENC=1 -o fake_enc.so tests/fake_drv.c
    $CC -Wall -O2 -rdynamic -o test_dispatch tests/test_dispatch.c -ldl
    J713_VA_DEC_DRIVER=$PWD/fake_dec.so J713_VA_ENC_DRIVER=$PWD/fake_enc.so \
      ./test_dispatch $PWD/j713_drv_video.so
    # The compiled-in inner driver paths must exist.
    test -e ${dec} && test -e ${enc}
    runHook postCheck
  '';
  installPhase = ''
    install -Dm755 j713_drv_video.so $out/lib/dri/j713_drv_video.so
  '';
}
