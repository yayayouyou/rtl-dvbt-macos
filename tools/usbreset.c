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
