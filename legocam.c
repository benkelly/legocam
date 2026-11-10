// SPDX-License-Identifier: GPL-2.0-or-later
// legocam: user-space driver for the LEGO Studios camera (Logitech QuickCam Web, 046d:0850).
// It writes raw 356x292 GRBG Bayer frames to stdout. Copyright (c) 2026 Ben Kelly.
// The protocol comes from the Linux gspca stv06xx driver (STV0610 bridge, VV6410 sensor):
// Copyright (c) 2001 Jean-Fredric Clere, Nikolas Zimmermann, Georg Acher, Mark Cave-Ayland,
//                    Carlo E Prelz, Dick Streefland
// Copyright (c) 2002, 2003 Tuukka Toivonen
// Copyright (c) 2008 Erik Andrén
#include <libusb.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define VID 0x046d
#define PID 0x0850
#define EP 0x81
#define W 356
#define H 292
#define NXFER 8
#define NPKT 32

static libusb_device_handle *dev;
static volatile sig_atomic_t stop;
static int active;
static unsigned char frame[W * H];
static int flen = -1; /* -1 means: wait for the next start of frame */

static int ctrl_out(uint16_t addr, unsigned char *buf, uint16_t len)
{
    int r = libusb_control_transfer(dev, 0x40, 0x04, addr, 0, buf, len, 5000);
    if (r < 0)
        fprintf(stderr, "legocam: write to 0x%04x failed: %s\n", addr, libusb_error_name(r));
    return r < 0 ? r : 0;
}

static int bridge_write(uint16_t addr, uint16_t val)
{
    unsigned char buf[2] = { val & 0xff, val >> 8 };
    return ctrl_out(addr, buf, val > 0xff ? 2 : 1);
}

/* Writes n (register, value) pairs to the sensor over the bridge I2C. n <= 16. */
static int sensor_write(const unsigned char (*regs)[2], int n)
{
    unsigned char buf[0x23] = { 0 };
    for (int i = 0; i < n; i++) {
        buf[i] = regs[i][0];
        buf[0x10 + i] = regs[i][1];
    }
    buf[0x20] = 0x20; /* VV6410 I2C address */
    buf[0x21] = n - 1;
    buf[0x22] = 1; /* write */
    int r = ctrl_out(0x0400, buf, sizeof buf);
    return r ? r : bridge_write(0x1704, 0); /* the STV0610 needs this after each I2C write */
}

static int sensor_set(uint8_t reg, uint8_t val)
{
    return sensor_write((const unsigned char[][2]){ { reg, val } }, 1);
}

static int sensor_read(uint8_t reg)
{
    unsigned char buf[0x23] = { 0 };
    if (bridge_write(0x1423, 5) < 0) /* I2C flush */
        return -1;
    buf[0] = reg;
    buf[0x20] = 0x20;
    buf[0x22] = 3; /* read */
    if (ctrl_out(0x1400, buf, sizeof buf) < 0)
        return -1;
    int r = libusb_control_transfer(dev, 0xc0, 0x04, 0x1410, 0, buf, 1, 5000);
    return r < 0 ? r : buf[0];
}

/* Same scale as the Linux driver: exposure 0..32768, gain 0..15. */
static int set_exposure_gain(int exposure, int gain)
{
    int v = (exposure * exposure >> 14) + exposure / 4;
    int fine = v % 415, coarse = v / 415 > 512 ? 512 : v / 415;
    const unsigned char regs[][2] = {
        { 0x20, fine >> 8 }, { 0x21, fine & 0xff },
        { 0x22, coarse >> 8 }, { 0x23, coarse & 0xff },
        { 0x24, 0xf0 | (gain & 0xf) },
    };
    return sensor_write(regs, 5);
}

static int init_camera(void)
{
    static const struct { uint16_t addr; uint8_t val; } bridge[] = {
        { 0x1620, 0x80 }, { 0x1620, 0x00 }, { 0x1443, 0x00 }, { 0x1423, 0x04 },
        { 0x1500, 0x0b }, { 0x1501, 0xa7 }, { 0x1502, 0xb7 }, { 0x1503, 0x00 },
        { 0x1504, 0x00 }, { 0x1536, 0x02 }, { 0x1537, 0x00 }, { 0x1538, 0x60 },
        { 0x1539, 0x01 }, { 0x153a, 0x20 }, { 0x153b, 0x01 },
    };
    static const unsigned char sensor[][2] = {
        { 0x10, 0x04 }, /* soft reset */
        { 0x10, 0x01 }, /* low power */
        { 0x11, 0x40 }, /* shuffled read-out */
        { 0x14, 0x55 }, { 0x15, 0x00 }, { 0x16, 0x81 },
        { 0x25, 0x02 }, /* clock divide by 2 */
        { 0x77, 0x5e }, { 0x78, 0x00 }, { 0x79, 0x11 },
    };
    for (size_t i = 0; i < sizeof bridge / sizeof bridge[0]; i++)
        if (bridge_write(bridge[i].addr, bridge[i].val))
            return -1;
    return sensor_write(sensor, sizeof sensor / sizeof sensor[0]);
}

/* Each packet holds chunks: 2-byte id, 2-byte length, then data. */
static void parse(const unsigned char *p, int len)
{
    while (len >= 4) {
        int id = p[0] << 8 | p[1], clen = p[2] << 8 | p[3];
        p += 4;
        len -= 4;
        if (clen > len) {
            flen = -1;
            return;
        }
        switch (id) {
        case 0x8001: case 0x8005: case 0xc001: case 0xc005: /* start of frame */
            flen = 0;
            break;
        case 0x0200: case 0x4200: /* image data */
            if (flen < 0)
                break;
            if (flen + clen > (int)sizeof frame) {
                flen = -1;
                break;
            }
            memcpy(frame + flen, p, clen);
            flen += clen;
            break;
        case 0x8002: case 0x8006: case 0xc002: /* end of frame */
            if (flen == (int)sizeof frame) {
                if (fwrite(frame, 1, sizeof frame, stdout) != sizeof frame || fflush(stdout))
                    stop = 1; /* the reader went away */
            } else if (flen >= 0) {
                fprintf(stderr, "legocam: dropped short frame, %d bytes\n", flen);
            }
            flen = -1;
            break;
        }
        p += clen;
        len -= clen;
    }
}

// NOTE: writes frames inside the USB callback. If the reader is slow, frames drop.
// Move the write to a separate thread if that becomes a problem.
static void LIBUSB_CALL on_transfer(struct libusb_transfer *t)
{
    if (t->status == LIBUSB_TRANSFER_COMPLETED) {
        for (int i = 0; i < t->num_iso_packets; i++) {
            if (t->iso_packet_desc[i].status != LIBUSB_TRANSFER_COMPLETED)
                flen = -1;
            else
                parse(libusb_get_iso_packet_buffer_simple(t, i), t->iso_packet_desc[i].actual_length);
        }
        if (!stop && libusb_submit_transfer(t) == 0)
            return;
    } else if (t->status != LIBUSB_TRANSFER_CANCELLED) {
        fprintf(stderr, "legocam: transfer failed, status %d\n", t->status);
    }
    stop = 1;
    active--;
}

static void on_signal(int sig)
{
    (void)sig;
    stop = 1;
}

int main(int argc, char **argv)
{
    int exposure = argc > 1 ? atoi(argv[1]) : 20000;
    int gain = argc > 2 ? atoi(argv[2]) : 10;
    struct libusb_transfer *xfer[NXFER] = { 0 };
    int r, cfg = 0, pkt;

    if (isatty(STDOUT_FILENO)) {
        fprintf(stderr, "usage: legocam [exposure 0-32768] [gain 0-15] | <reader>\n"
                        "Writes raw 356x292 bayer_grbg8 frames to stdout. Pipe it into ffplay or ffmpeg.\n");
        return 1;
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    if ((r = libusb_init(NULL)) < 0) {
        fprintf(stderr, "legocam: libusb_init: %s\n", libusb_error_name(r));
        return 1;
    }
    if (!(dev = libusb_open_device_with_vid_pid(NULL, VID, PID))) {
        fprintf(stderr, "legocam: camera %04x:%04x not found\n", VID, PID);
        return 1;
    }
    libusb_get_configuration(dev, &cfg);
    if (cfg != 1 && (r = libusb_set_configuration(dev, 1)) < 0)
        goto usb_fail;
    if ((r = libusb_claim_interface(dev, 0)) < 0)
        goto usb_fail;

    if ((r = sensor_read(0x00)) != 0x19) {
        fprintf(stderr, "legocam: VV6410 sensor not found (id %d)\n", r);
        return 1;
    }
    if (init_camera() || set_exposure_gain(exposure, gain)) {
        fprintf(stderr, "legocam: camera init failed\n");
        return 1;
    }

    if ((r = libusb_set_interface_alt_setting(dev, 0, 1)) < 0)
        goto usb_fail;
    if ((pkt = libusb_get_max_alt_packet_size(libusb_get_device(dev), 0, 1, EP)) <= 0) {
        r = pkt;
        goto usb_fail;
    }
    fprintf(stderr, "legocam: streaming, iso packet size %d\n", pkt);
    if (bridge_write(0x15c1, pkt) /* iso packet size */
        || bridge_write(0x15c3, 0x01) /* 288 lines. Column register 0x1680 stalls on the STV0610. */
        || bridge_write(0x1443, 0x00) /* scan rate */
        || bridge_write(0x1445, 1) /* LED on */
        || sensor_set(0x10, 0x00) /* sensor out of low power */
        || bridge_write(0x1440, 1)) { /* start iso stream */
        fprintf(stderr, "legocam: start failed\n");
        return 1;
    }

    for (int i = 0; i < NXFER; i++) {
        xfer[i] = libusb_alloc_transfer(NPKT);
        libusb_fill_iso_transfer(xfer[i], dev, EP, malloc(NPKT * pkt), NPKT * pkt, NPKT, on_transfer, NULL, 1000);
        libusb_set_iso_packet_lengths(xfer[i], pkt);
        if ((r = libusb_submit_transfer(xfer[i])) < 0) {
            fprintf(stderr, "legocam: submit: %s\n", libusb_error_name(r));
            stop = 1;
            break;
        }
        active++;
    }

    int cancelled = 0;
    while (active > 0) {
        if (stop && !cancelled) {
            for (int i = 0; i < NXFER; i++)
                if (xfer[i])
                    libusb_cancel_transfer(xfer[i]);
            cancelled = 1;
        }
        struct timeval tv = { 0, 100000 };
        libusb_handle_events_timeout_completed(NULL, &tv, NULL);
    }

    bridge_write(0x1440, 0); /* stop iso stream */
    bridge_write(0x1445, 0); /* LED off */
    sensor_set(0x10, 0x01); /* sensor low power */
    libusb_set_interface_alt_setting(dev, 0, 0);
    libusb_release_interface(dev, 0);
    libusb_close(dev);
    return 0;

usb_fail:
    fprintf(stderr, "legocam: %s\n", libusb_error_name(r));
    return 1;
}
