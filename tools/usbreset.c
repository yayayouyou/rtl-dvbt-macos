/*
 * usbreset.c - clear a wedged RTL2832U without unplugging it
 *
 * Copyright (C) 2026  yayayouyou
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Writing bad values to the demodulator's output-routing registers can hang
 * the USB endpoint hard enough that even plain rtl_sdr stops producing data
 * and blocks forever. The device is not bricked - a USB-level reset clears
 * it. Handy between experiments that sweep those registers.
 *
 * Build: clang -o usbreset tools/usbreset.c $(pkg-config --cflags --libs libusb-1.0)
 */

#include <stdio.h>
#include <libusb.h>
int main(void){
    libusb_context *ctx=NULL; libusb_device_handle *h;
    if(libusb_init(&ctx)) return 1;
    h=libusb_open_device_with_vid_pid(ctx,0x0bda,0x2838);
    if(!h){ printf("device not found\n"); return 1; }
    printf("reset: %d\n", libusb_reset_device(h));
    libusb_close(h); libusb_exit(ctx); return 0;
}
