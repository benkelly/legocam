# legocam

A user-space driver for the LEGO Studios camera on macOS. This camera came with LEGO set 9647, the LEGO Education Studios Camera and Software Kit. It is a Logitech QuickCam Web inside, USB ID `046d:0850`. The driver uses libusb, so it needs no kernel extension. The USB protocol comes from the Linux `gspca_stv06xx` driver.

`legocam` writes raw 356x292 `bayer_grbg8` frames to stdout. `ffplay` can show them, and `obscam` sends them to the OBS Virtual Camera.

## Build

```
brew install libusb ffmpeg
make
```

## Show the picture

```
make view
```

## Use it as a webcam

macOS adds a new camera only through a signed camera extension. A free Apple developer account cannot sign one. OBS ships a signed camera extension, so `obscam` sends the frames straight into it. The OBS app does not run.

You do these steps one time:

1. Install OBS 30 or later from obsproject.com or with `brew install --cask obs`.
2. Open OBS one time. In System Settings, go to General > Login Items & Extensions. Click the info button next to Camera Extensions, and turn on OBS Virtual Camera. On macOS 13 and 14, click Allow in Privacy & Security instead.
3. Quit OBS.

After that, run this command:

```
make webcam
```

In Zoom, FaceTime, or the browser, select "OBS Virtual Camera". To stop, press Ctrl-C. The OBS placeholder picture shows when legocam is not running.

## Exposure and gain

The camera has no automatic exposure. If the picture is too dark or too bright, give the exposure (0 to 32768) and the gain (0 to 15) as arguments. The defaults are 20000 and 10.

```
./legocam 30000 12 | ffplay -f rawvideo -pixel_format bayer_grbg8 -video_size 356x292 -
```

## Test

Plug in the camera, then run `make check`. The test reads 10 frames and makes sure that legocam stops when the pipe closes.

## Limits

- The frame rate is about 5 frames per second. USB 1.1 bandwidth and the sensor clock set this limit.
- The microphone and the snapshot button do not work.
- Apps show the camera as "OBS Virtual Camera".
- Tested on an Intel MacBook Pro with macOS 15.7.3 and OBS 33.0.1. Apple Silicon is not tested.

## License

GPL-2.0-or-later. See `LICENSE`. The USB protocol and register values come from the Linux `gspca_stv06xx` driver by Jean-Fredric Clere, Nikolas Zimmermann, Georg Acher, Mark Cave-Ayland, Carlo E Prelz, Dick Streefland, Tuukka Toivonen and Erik Andrén.
