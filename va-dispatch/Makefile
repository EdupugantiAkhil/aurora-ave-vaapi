# Build: make DEC=/path/v4l2_request_drv_video.so ENC=/path/ave_drv_video.so
# Test:  make check   (host only; uses fake inner drivers, opens no device)
DEC ?= /usr/lib/dri/v4l2_request_drv_video.so
ENC ?= /usr/lib/dri/ave_drv_video.so
CC ?= cc
CFLAGS ?= -O2
WFLAGS = -std=gnu11 -Wall -Wextra -Werror -Wno-unused-parameter -fPIC
all: j713_drv_video.so
j713_drv_video.so: j713_dispatch.c
	$(CC) $(CFLAGS) $(WFLAGS) -shared -fvisibility=hidden \
	  -DJ713_DEC_DRIVER='"$(DEC)"' -DJ713_ENC_DRIVER='"$(ENC)"' \
	  -o $@ $< -ldl -lva
check: j713_drv_video.so
	$(CC) -Wall -O2 -fPIC -shared -DFAKE_ENC=0 -o fake_dec.so tests/fake_drv.c
	$(CC) -Wall -O2 -fPIC -shared -DFAKE_ENC=1 -o fake_enc.so tests/fake_drv.c
	$(CC) -Wall -O2 -rdynamic -o test_dispatch tests/test_dispatch.c -ldl
	J713_VA_DEC_DRIVER=$$PWD/fake_dec.so J713_VA_ENC_DRIVER=$$PWD/fake_enc.so \
	  ./test_dispatch $$PWD/j713_drv_video.so
clean:
	rm -f *.so test_dispatch
.PHONY: all check clean
