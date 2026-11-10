CFLAGS += -O2 -Wall -Wextra $(shell pkg-config --cflags libusb-1.0)
LDLIBS += $(shell pkg-config --libs libusb-1.0)
RAW = -f rawvideo -pixel_format bayer_grbg8 -video_size 356x292

all: legocam obscam

legocam: legocam.c

obscam: obscam.m
	$(CC) -O2 -Wall -Wextra -fobjc-arc $(shell pkg-config --cflags libswscale) $< \
		$(shell pkg-config --libs libswscale) \
		-framework Foundation -framework CoreMedia -framework CoreMediaIO -framework CoreVideo -o $@

view: legocam
	./legocam | ffplay -loglevel warning -fflags nobuffer $(RAW) -

webcam: legocam obscam
	./legocam | ./obscam

# Needs the camera plugged in. Reads 10 frames, then legocam must exit when the pipe closes.
check: legocam
	test "$$(./legocam | head -c 1039520 | wc -c)" -eq 1039520

.PHONY: all view webcam check
