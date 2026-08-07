/*
 * probe.c - verify that demodulator register access actually works
 *
 * Before believing anything about lock status, establish the boring thing:
 * do our writes land, and can we read them back? Everything downstream is
 * meaningless if this fails.
 *
 * Build: see tools/build-probe.sh
 */

#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <rtl-sdr.h>

extern int rtlsdr_demod_write_reg(rtlsdr_dev_t *dev, uint8_t page,
				  uint16_t addr, uint16_t val, uint8_t len);
extern uint16_t rtlsdr_demod_read_reg(rtlsdr_dev_t *dev, uint8_t page,
				      uint16_t addr, uint8_t len);

static uint8_t rd8(rtlsdr_dev_t *d, uint8_t page, uint16_t addr)
{
	return rtlsdr_demod_read_reg(d, page, addr, 1) & 0xff;
}

int main(void)
{
	rtlsdr_dev_t *dev = NULL;
	int r;

	if (rtlsdr_open(&dev, 0) < 0) {
		fprintf(stderr, "cannot open device\n");
		return 1;
	}

	printf("\n=== 1. Demodulator presence (Java attach() reads page0 reg0) ===\n");
	printf("  page0 0x00 = 0x%02x\n", rd8(dev, 0, 0x00));

	printf("\n=== 2. I2C repeater register (page1 0x01) ===\n");
	printf("  before        = 0x%02x   (expect 0x10 closed / 0x18 open)\n",
	       rd8(dev, 1, 0x01));

	printf("\n=== 3. Write/read-back test on page1 0x01 ===\n");
	r = rtlsdr_demod_write_reg(dev, 1, 0x01, 0x18, 1);
	printf("  wrote 0x18 (rc=%d) -> reads back 0x%02x %s\n", r,
	       rd8(dev, 1, 0x01), rd8(dev, 1, 0x01) == 0x18 ? "OK" : "MISMATCH");
	r = rtlsdr_demod_write_reg(dev, 1, 0x01, 0x10, 1);
	printf("  wrote 0x10 (rc=%d) -> reads back 0x%02x %s\n", r,
	       rd8(dev, 1, 0x01), rd8(dev, 1, 0x01) == 0x10 ? "OK" : "MISMATCH");

	printf("\n=== 4. State BEFORE DVB-T switch (librtlsdr SDR mode) ===\n");
	printf("  page0 0x08 = 0x%02x   (librtlsdr open wrote 0x4d)\n", rd8(dev, 0, 0x08));
	printf("  page1 0xb1 = 0x%02x   (librtlsdr open wrote 0x1a)\n", rd8(dev, 1, 0xb1));
	printf("  page1 0x15 = 0x%02x   (librtlsdr open wrote 0x01)\n", rd8(dev, 1, 0x15));
	printf("  page0 0x19 = 0x%02x\n", rd8(dev, 0, 0x19));
	printf("  page0 0x61 = 0x%02x\n", rd8(dev, 0, 0x61));

	printf("\n=== 5. Apply unsetSdrMode() writes, verifying each ===\n");
	struct { uint8_t page; uint16_t addr; uint8_t val; } w[] = {
		{0, 0x61, 0xe0},
		{0, 0x19, 0x20},
		{0, 0x17, 0x11}, {0, 0x18, 0x10},
		{1, 0x92, 0x00}, {1, 0x93, 0x0f}, {1, 0x94, 0xff},
		{1, 0x3e, 0x40}, {1, 0x3f, 0x00},
		{1, 0x15, 0x06}, {1, 0x16, 0x3f}, {1, 0x17, 0xce}, {1, 0x18, 0xcc},
	};
	int bad = 0;
	for (unsigned i = 0; i < sizeof(w)/sizeof(w[0]); i++) {
		r = rtlsdr_demod_write_reg(dev, w[i].page, w[i].addr, w[i].val, 1);
		uint8_t got = rd8(dev, w[i].page, w[i].addr);
		int ok = (got == w[i].val);
		if (!ok) bad++;
		printf("  page%d 0x%02x <- 0x%02x  rc=%2d  readback 0x%02x  %s\n",
		       w[i].page, w[i].addr, w[i].val, r, got, ok ? "OK" : "** MISMATCH **");
	}
	printf("  --> %d of %zu writes did not read back\n", bad, sizeof(w)/sizeof(w[0]));

	printf("\n=== 6. FSM / status registers after switch ===\n");
	printf("  page1 0x3c = 0x%02x   (our get_status reads this)\n", rd8(dev, 1, 0x3c));
	printf("  page3 0x3c = 0x%02x   (our read_snr reads this)\n", rd8(dev, 3, 0x3c));
	printf("  page0 0x0c = 0x%02x\n", rd8(dev, 0, 0x0c));

	rtlsdr_close(dev);
	printf("\n");
	return 0;
}
