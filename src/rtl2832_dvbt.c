/*
 * rtl2832_dvbt.c - DVB-T mode for RTL2832U on macOS
 *
 * Copyright (C) 2026  yayayouyou
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Ported from AndroidDvbDriver's Rtl2832Frontend.java (GPLv2):
 *   https://github.com/signalwareltd/AndroidDvbDriver
 * which is a userspace port of the Linux kernel V4L2 rtl2832 driver.
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <unistd.h>
#include <string.h>

#include "rtl2832_dvbt.h"
#include "rtl2832_regs.h"

/*
 * These are defined in librtlsdr.c but deliberately not declared in the
 * public rtl-sdr.h, and librtlsdr builds with -fvisibility=hidden, so they
 * are absent from a stock libr dylib's symbol table. We compile
 * librtlsdr's sources straight into this binary (see CMakeLists.txt), which
 * is why these resolve at link time. Linking against a Homebrew librtlsdr
 * will NOT work.
 */
extern int rtlsdr_demod_write_reg(rtlsdr_dev_t *dev, uint8_t page,
				  uint16_t addr, uint16_t val, uint8_t len);
extern uint16_t rtlsdr_demod_read_reg(rtlsdr_dev_t *dev, uint8_t page,
				      uint16_t addr, uint8_t len);
extern int rtlsdr_write_reg(rtlsdr_dev_t *dev, uint8_t block, uint16_t addr,
			    uint16_t val, uint8_t len);
extern uint16_t rtlsdr_read_reg(rtlsdr_dev_t *dev, uint8_t block,
				uint16_t addr, uint8_t len);
extern int rtlsdr_i2c_write_reg(rtlsdr_dev_t *dev, uint8_t i2c_addr,
				uint8_t reg, uint8_t val);
extern void rtlsdr_set_i2c_repeater(rtlsdr_dev_t *dev, int on);

/* third_party/librtlsdr/include/tuner_r82xx.h */
#define R820T_I2C_ADDR 0x34

/* Block selector for the system-register space (librtlsdr's USBB). */
#define RTL2832_BLOCK_USB 1

#define RTL2832_USB_SYSCTL       0x2000
#define RTL2832_USB_EPA_MAXPKT   0x2158
#define RTL2832_USB_EPA_FIFO_CFG 0x2160

/* ---------------------------------------------------------------------- */
/* Raw register access                                                     */
/* ---------------------------------------------------------------------- */

/*
 * librtlsdr's demod read is byte-swapped relative to its own write: the
 * write path emits big-endian (data[0] = val >> 8) but the read path
 * reassembles little-endian ((data[1] << 8) | data[0]). Nothing in
 * librtlsdr ever reads a 16-bit demod register - the single call site uses
 * len 1 and discards the result - so the bug has never had a chance to
 * bite. We read 16-bit fields constantly, so we compensate here rather than
 * patching the submodule.
 *
 * If you ever see a status or BER value that looks like a plausible number
 * with its bytes reversed, this is the first place to look.
 */
static uint16_t demod_rd(rtlsdr_dev_t *dev, uint8_t page, uint16_t addr,
			 uint8_t len)
{
	uint16_t raw = rtlsdr_demod_read_reg(dev, page, addr, len);

	if (len == 1)
		return raw & 0xff;

	return (uint16_t)(((raw & 0xff) << 8) | (raw >> 8));
}

/*
 * Write a run of consecutive byte registers.
 *
 * The Java driver issues these as single multi-byte I2C transactions, but
 * librtlsdr's demod_write_reg tops out at two bytes (its stack buffer is
 * uint8_t[2], so a longer length would read past the end). We therefore
 * split into one transaction per byte and rely on the demodulator's
 * register address auto-increment.
 *
 * ASSUMPTION, UNVERIFIED ON HARDWARE: that splitting is equivalent. It
 * should be for plain configuration registers, but any register that
 * latches on the final byte of a burst would behave differently. If Phase 1
 * writes cleanly yet never locks, this is suspect number one - see
 * docs/PORTING.md.
 */
static int demod_wr_bytes(rtlsdr_dev_t *dev, uint8_t page, uint16_t addr,
			  const uint8_t *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		int r = rtlsdr_demod_write_reg(dev, page, (uint16_t)(addr + i),
					       buf[i], 1);
		if (r < 0)
			return r;
	}
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Bit-field access                                                        */
/* ---------------------------------------------------------------------- */

/*
 * Read-modify-write a named bit-field. Mirrors wrDemodReg() in the Java.
 *
 * Fields are big-endian within their register run: the byte at the field's
 * start address holds the most significant bits.
 */
static int reg_write(rtlsdr_dev_t *dev, rtl2832_reg_id id, uint32_t val)
{
	const rtl2832_reg_t *reg = &rtl2832_regs[id];
	uint8_t len = (uint8_t)((reg->msb >> 3) + 1);
	uint32_t mask = (1u << (reg->msb - reg->lsb + 1)) - 1;
	uint32_t cur = 0;
	uint8_t buf[4];

	if (len > sizeof(buf))
		return -1;

	/* Read the whole run byte-by-byte so the byte order is ours, not
	 * librtlsdr's. */
	for (uint8_t i = 0; i < len; i++) {
		uint16_t b = demod_rd(dev, reg->page,
				      (uint16_t)(reg->addr + i), 1);
		cur |= (uint32_t)(b & 0xff) << ((len - 1 - i) * 8);
	}

	cur &= ~(mask << reg->lsb);
	cur |= (val & mask) << reg->lsb;

	for (uint8_t i = 0; i < len; i++)
		buf[i] = (uint8_t)(cur >> ((len - 1 - i) * 8));

	return demod_wr_bytes(dev, reg->page, reg->addr, buf, len);
}

/* Read a named bit-field. Mirrors rdDemodReg() in the Java. */
static int reg_read(rtlsdr_dev_t *dev, rtl2832_reg_id id, uint32_t *out)
{
	const rtl2832_reg_t *reg = &rtl2832_regs[id];
	uint8_t len = (uint8_t)((reg->msb >> 3) + 1);
	uint32_t mask = (1u << (reg->msb - reg->lsb + 1)) - 1;
	uint32_t cur = 0;

	if (len > 4)
		return -1;

	for (uint8_t i = 0; i < len; i++) {
		uint16_t b = demod_rd(dev, reg->page,
				      (uint16_t)(reg->addr + i), 1);
		cur |= (uint32_t)(b & 0xff) << ((len - 1 - i) * 8);
	}

	*out = (cur >> reg->lsb) & mask;
	return 0;
}

static int reg_write_table(rtlsdr_dev_t *dev, const rtl2832_regval_t *tbl,
			   size_t n)
{
	for (size_t i = 0; i < n; i++) {
		int r = reg_write(dev, tbl[i].reg, tbl[i].val);
		if (r < 0) {
			fprintf(stderr, "rtl2832: table write failed at "
					"entry %zu\n", i);
			return r;
		}
	}
	return 0;
}

/* ---------------------------------------------------------------------- */
/* Mode switching                                                          */
/* ---------------------------------------------------------------------- */

/*
 * Leave SDR mode and hand the signal path back to the hardware DVB-T
 * demodulator.
 *
 * This is the whole reason this project exists. librtlsdr spends its
 * initialisation doing the opposite of these writes - bypassing the
 * demodulator so raw I/Q reaches the host - and no macOS code has ever
 * undone it. Eight register writes.
 *
 * Transcribed from Rtl2832Frontend.unsetSdrMode().
 */
static int unset_sdr_mode(rtlsdr_dev_t *dev)
{
	static const uint8_t mode[]  = { 0x11, 0x10 };
	static const uint8_t fsm0[]  = { 0x00, 0x0f, 0xff };
	static const uint8_t fsm1[]  = { 0x40, 0x00 };
	static const uint8_t fsm2[]  = { 0x06, 0x3f, 0xce, 0xcc };
	int r;

	/* PID filter off - we want the full transport stream */
	if ((r = rtlsdr_demod_write_reg(dev, 0, 0x61, 0xe0, 1)) < 0) return r;

	/* mode */
	if ((r = rtlsdr_demod_write_reg(dev, 0, 0x19, 0x20, 1)) < 0) return r;
	if ((r = demod_wr_bytes(dev, 0, 0x17, mode, sizeof(mode))) < 0) return r;

	/* FSM */
	if ((r = demod_wr_bytes(dev, 1, 0x92, fsm0, sizeof(fsm0))) < 0) return r;
	if ((r = demod_wr_bytes(dev, 1, 0x3e, fsm1, sizeof(fsm1))) < 0) return r;
	if ((r = demod_wr_bytes(dev, 1, 0x15, fsm2, sizeof(fsm2))) < 0) return r;

	return 0;
}

/*
 * Undo the SDR-specific things librtlsdr switched OFF that DVB-T needs ON.
 *
 * This has no counterpart in the Java driver, and that is exactly why it was
 * missing. AndroidDvbDriver talks to a freshly powered chip where these bits
 * sit at their power-on defaults, so it never has to think about them. We
 * arrive after librtlsdr's rtlsdr_init_baseband(), which deliberately clears
 * them because SDR mode wants the raw ADC path with no automatic gain
 * control:
 *
 *   rtlsdr_demod_write_reg(dev, 1, 0x04, 0x00, 1);
 *       "disable RF and IF AGC loop"
 *
 * Page 1 register 0x04 packs EN_IF_AGC (bit 7), EN_RF_AGC (bit 6),
 * AAGC_HOLD (bit 5) and LOOP_GAIN2_3_0 (bits 4:1) together. Our register
 * tables do write LOOP_GAIN2_3_0, but as a read-modify-write on the low bits
 * only — the two enable bits stay zero. The demodulator then has no way to
 * drive the tuner's gain, the signal level into the ADC is never brought into
 * range, and it can never lock. Every register write succeeds and nothing
 * works.
 *
 * Call before the init tables so their LOOP_GAIN2 writes layer on top.
 */
static int restore_dvbt_defaults(rtlsdr_dev_t *dev)
{
	int r;

	if ((r = reg_write(dev, DVBT_EN_IF_AGC, 1)) < 0) return r;
	if ((r = reg_write(dev, DVBT_EN_RF_AGC, 1)) < 0) return r;
	if ((r = reg_write(dev, DVBT_AAGC_HOLD, 0)) < 0) return r;

	return 0;
}

/*
 * Configure the USB endpoint for transport-stream delivery.
 *
 * This is the device-level init() the Java driver runs after the frontend is
 * up (Rtl28xxDvbDevice.init()), and it is the one piece librtlsdr has no
 * equivalent for at all. librtlsdr sets USB_SYSCTL and USB_EPA_MAXPKT, but
 * it defines USB_EPA_FIFO_CFG and then never writes it - SDR mode does not
 * need the endpoint FIFO reconfigured, TS delivery does.
 *
 * Two further differences from librtlsdr, both deliberate:
 *  - USB_SYSCTL is read-modify-written here (val |= 0x09, "enable DMA and
 *    Full Packet Mode") rather than blindly assigned 0x09, so whatever else
 *    the chip has set survives.
 *  - byte order: librtlsdr's 2-byte write emits val >> 8 first, so the FIFO
 *    length byte 0x14 at offset 0 means writing 0x1400, not 0x0014.
 */
static int init_usb_endpoints(rtlsdr_dev_t *dev)
{
	uint16_t val;
	int r;

	/* enable DMA and Full Packet Mode */
	val = rtlsdr_read_reg(dev, RTL2832_BLOCK_USB, RTL2832_USB_SYSCTL, 1);
	val |= 0x09;
	r = rtlsdr_write_reg(dev, RTL2832_BLOCK_USB, RTL2832_USB_SYSCTL,
			     val & 0xff, 1);
	if (r < 0)
		return r;

	/* EPA maximum packet size 0x0200 */
	r = rtlsdr_write_reg(dev, RTL2832_BLOCK_USB, RTL2832_USB_EPA_MAXPKT,
			     0x0002, 2);
	if (r < 0)
		return r;

	/* EPA FIFO length */
	return rtlsdr_write_reg(dev, RTL2832_BLOCK_USB,
				RTL2832_USB_EPA_FIFO_CFG, 0x1400, 2);
}

/*
 * Tell the demodulator where the tuner is putting the IF.
 *
 * PSET_IFFREQ = -floor((if_hz % XTAL) * 2^22 / XTAL)
 *
 * en_bbin selects baseband input, used when the tuner is zero-IF. The R820T
 * in DVB-T mode is not - librtlsdr's r82xx_set_bandwidth() picks 3.57 MHz
 * for a 6 MHz channel - so this normally takes the IF path.
 */
static int set_if(rtlsdr_dev_t *dev, uint32_t if_hz)
{
	uint64_t pset;
	int r;

	r = reg_write(dev, DVBT_EN_BBIN, if_hz == 0 ? 1 : 0);
	if (r < 0)
		return r;

	pset = (uint64_t)(if_hz % RTL2832_XTAL) * 0x400000u / RTL2832_XTAL;
	pset = (uint64_t)(-(int64_t)pset) & 0x3fffff;

	return reg_write(dev, DVBT_PSET_IFFREQ, (uint32_t)pset);
}

/* ---------------------------------------------------------------------- */
/* Public API                                                              */
/* ---------------------------------------------------------------------- */

int rtl2832_dvbt_init(rtlsdr_dev_t *dev)
{
	int r;

	if (!dev)
		return -1;

	if ((r = unset_sdr_mode(dev)) < 0) {
		fprintf(stderr, "rtl2832: could not leave SDR mode\n");
		return r;
	}

	if ((r = reg_write(dev, DVBT_SOFT_RST, 0)) < 0)
		return r;

	if ((r = restore_dvbt_defaults(dev)) < 0) {
		fprintf(stderr, "rtl2832: could not re-enable AGC loops\n");
		return r;
	}

	/*
	 * In DVB-T the demodulator's AGC loop is what drives the tuner's gain;
	 * a tuner pinned to a manual gain fights it. RTL_DVBT_GAINMODE lets us
	 * sweep this by hand while acquisition is still stalling.
	 */
	{
		const char *g = getenv("RTL_DVBT_GAIN");

		if (g) {
			/* Manual tuner gain in tenths of a dB. With a CNR
			 * meter available (rtl2832_dvbt_read_signal) this is
			 * worth sweeping: the demodulator's own AGC optimises
			 * for level, not for carrier-to-noise, and on a
			 * marginal signal the two disagree. */
			rtlsdr_set_tuner_gain_mode(dev, 1);
			rtlsdr_set_tuner_gain(dev, (int)strtol(g, NULL, 0));
		} else {
			rtlsdr_set_tuner_gain_mode(dev, 0);
		}
	}

	r = reg_write_table(dev, rtl2832_initial_regs,
			    RTL2832_INITIAL_REGS_LEN);
	if (r < 0)
		return r;

	/* Only the R820T path is wired up. The generated header also carries
	 * the E4000 and FC0012 tables if you have one of those dongles. */
	r = reg_write_table(dev, rtl2832_tuner_init_r820t,
			    RTL2832_TUNER_INIT_R820T_LEN);
	if (r < 0)
		return r;

	if ((r = reg_write(dev, DVBT_SOFT_RST, 1)) < 0) return r;
	if ((r = reg_write(dev, DVBT_SOFT_RST, 0)) < 0) return r;

	/* Last, matching the Java's open(): frontend first, then the
	 * device-level USB endpoint setup. */
	if ((r = init_usb_endpoints(dev)) < 0) {
		fprintf(stderr, "rtl2832: USB endpoint setup failed\n");
		return r;
	}

	return 0;
}

/*
 * Hand the R820T's VGA back to the demodulator's AGC loop.
 *
 * R820T register 0x0c bit 4 selects where the VGA takes its gain from:
 *   1 = follow the VAGC pin, which the RTL2832's IF-AGC loop drives
 *   0 = "vga code mode", fixed gain from bits 3:0
 *
 * Both stacks known to work on this hardware leave it at 1 for DVB-T. The
 * kernel's init array has 0x0c = 0xf5 (r820t.c:120) and its
 * r820t_set_tv_standard() writes only the low nibble (r820t.c:1067, mask
 * 0x0f), so bit 4 survives every tune. The only kernel writes that clear it
 * live in r820t_set_gain_mode() - the explicit manual-gain path - and
 * r820t_standby(). Neither is on the DVB-T tuning path.
 *
 * librtlsdr clears it on EVERY tune: r82xx_set_freq() calls
 * r82xx_set_vga_gain() (tuner_r82xx.c:1181), which unconditionally writes
 * 0x0c mask 0x9f value 0x08 - 16.3 dB fixed, bit 4 = 0. Correct for SDR,
 * where the demodulator's AGC loops are disabled and the host wants a stable
 * known gain. Fatal for DVB-T, where that loop is the only gain control there
 * is: restore_dvbt_defaults() switches the IF-AGC loop on, and then nothing
 * is listening to its output pin.
 *
 * 0xf0 is the init-array byte with the low nibble zeroed, exactly what the
 * kernel's set_tv_standard writes when IMR calibration has not run - and
 * librtlsdr never runs IMR calibration.
 *
 * Written straight over I2C, bypassing librtlsdr's register shadow. That
 * shadow is therefore stale for 0x0c afterwards, so this MUST be the last
 * tuner write of the tune: any later masked write would recompute from the
 * stale cache and undo it.
 */
static int r820t_reconnect_agc_pin(rtlsdr_dev_t *dev)
{
	int r;

	rtlsdr_set_i2c_repeater(dev, 1);
	r = rtlsdr_i2c_write_reg(dev, R820T_I2C_ADDR, 0x0c, 0xf0);
	rtlsdr_set_i2c_repeater(dev, 0);

	return r;
}

/*
 * Pick the tuner gain that actually decodes, by measuring.
 *
 * librtlsdr's automatic gain mode is tuned for SDR: it maximises signal level
 * into the ADC, which for a strong nearby transmitter drives the R820T front
 * end into compression. The demodulator then locks perfectly well - carrier,
 * timing, even the TPS field all come out right - while the constellation is
 * smeared badly enough that Reed-Solomon cannot correct anything. The result
 * is a confident-looking lock delivering unusable bytes, which is a genuinely
 * confusing failure to debug.
 *
 * Measured on TTV, 581 MHz, seconds apart on the same channel:
 *   automatic gain -> CNR 16.1 dB, BER 65535/1e6, no sync bytes at all
 *   14.7 dB fixed  -> EVM 0,       BER    39/1e6, 100%% TS sync
 *
 * So sweep, and keep the gain with the lowest post-Viterbi BER. Costs about
 * two seconds on tune, and stops as soon as the error rate is low enough for
 * Reed-Solomon to mop up the rest.
 */
static int auto_gain(rtlsdr_dev_t *dev)
{
	int gains[64];
	int n, i, best_gain = 0, best_ber = -1;
	rtl2832_signal_t sig;

	n = rtlsdr_get_tuner_gains(dev, gains);
	if (n <= 0 || n > (int)(sizeof(gains) / sizeof(gains[0])))
		return rtlsdr_set_tuner_gain_mode(dev, 0);

	rtlsdr_set_tuner_gain_mode(dev, 1);

	for (i = 0; i < n; i++) {
		if (rtlsdr_set_tuner_gain(dev, gains[i]) < 0)
			continue;

		usleep(250000);

		if (rtl2832_dvbt_read_signal(dev, &sig) < 0)
			continue;
		if (sig.fsm_stage != 11)
			continue;

		if (best_ber < 0 || sig.ber < best_ber) {
			best_ber = sig.ber;
			best_gain = gains[i];
		}

		if (sig.ber < 100)
			break;
	}

	if (best_ber < 0) {
		/* Never locked at any gain - hand it back to the hardware. */
		return rtlsdr_set_tuner_gain_mode(dev, 0);
	}

	fprintf(stderr, "Gain %.1f dB (BER %d/1e6)\n",
		best_gain / 10.0, best_ber);

	return rtlsdr_set_tuner_gain(dev, best_gain);
}

int rtl2832_dvbt_set_params(rtlsdr_dev_t *dev, uint32_t freq_hz, uint32_t bw_hz)
{
	uint32_t bw_mode;
	int bw_idx;
	uint64_t num;
	uint32_t resamp_ratio, cfreq_off_ratio;
	int r;

	if (!dev)
		return -1;

	switch (bw_hz) {
	case 6000000: bw_idx = 0; bw_mode = 48000000; break;
	case 7000000: bw_idx = 1; bw_mode = 56000000; break;
	case 8000000: bw_idx = 2; bw_mode = 64000000; break;
	default:
		fprintf(stderr, "rtl2832: unsupported bandwidth %u Hz "
				"(use 6, 7 or 8 MHz)\n", bw_hz);
		return -1;
	}

	/*
	 * Drive the tuner through librtlsdr's public API. r82xx_set_bandwidth()
	 * already carries the DVB-T IF values inherited from the kernel driver
	 * (3.57 MHz at 6 MHz bandwidth), so we do not need to reach into the
	 * tuner privately.
	 *
	 * ORDER IS LOad-BEARING. rtlsdr_set_center_freq() must come FIRST.
	 *
	 * r820t_set_bw() ends with
	 *     return rtlsdr_set_center_freq(devt, devt->freq);
	 * (librtlsdr.c:253) and this fork has no `if (devt->freq > 0)` guard.
	 * Straight out of rtlsdr_open(), dev->freq is still 0 - the struct is
	 * memset and the only two assignments to it live inside
	 * rtlsdr_set_center_freq itself. So calling set_tuner_bandwidth first
	 * re-tunes to 0 Hz, which trips librtlsdr's automatic direct-sampling
	 * switch (freq < 24 MHz on an R820T), and _rtlsdr_set_direct_sampling()
	 * calls r82xx_standby() on the way in. The tuner goes to sleep and the
	 * excursion rewrites demod registers back to their SDR values,
	 * dismantling the DVB-T configuration we set up moments earlier.
	 *
	 * Tuning first leaves dev->freq set, the auto-switch evaluates false,
	 * and the excursion never happens. It also means the I2C repeater is
	 * closed on the way out: set_center_freq opens it and never closes it
	 * (librtlsdr.c:913 is commented out), whereas set_tuner_bandwidth does.
	 */
	if ((r = rtlsdr_set_center_freq(dev, freq_hz)) < 0) {
		fprintf(stderr, "rtl2832: set_center_freq failed\n");
		return r;
	}
	if ((r = rtlsdr_set_tuner_bandwidth(dev, bw_hz)) < 0) {
		fprintf(stderr, "rtl2832: set_tuner_bandwidth failed\n");
		return r;
	}

	/*
	 * librtlsdr's tuner helpers write demod registers as a side effect and
	 * leave the I2C repeater open. Re-assert the DVB-T datapath before
	 * configuring the rest of the demodulator.
	 *
	 * The Java reference does not need this: its tuner is a bare R820tTuner
	 * that never touches demod registers, so "configure tuner, then
	 * configure demod" holds trivially. Ours does not get that for free.
	 */
	if ((r = reg_write(dev, DVBT_IIC_REPEAT, 0)) < 0) return r;

	/* page0 0x08: librtlsdr leaves 0x4d (I-phase ADC only); DVB-T needs both. */
	if ((r = reg_write(dev, DVBT_AD_EN_REG, 1)) < 0) return r;
	if ((r = reg_write(dev, DVBT_AD_EN_REG1, 1)) < 0) return r;

	/* MPEG output routing, cleared by librtlsdr's page0 0x06 = 0x80. */
	if ((r = reg_write(dev, DVBT_MPEG_IO_OPT_2_2, 0)) < 0) return r;
	if ((r = reg_write(dev, DVBT_MPEG_IO_OPT_1_0, 0)) < 0) return r;

	/* page1 0x15: unsetSdrMode's value, plus SPEC_INV for the R820T. */
	if ((r = rtlsdr_demod_write_reg(dev, 1, 0x15, 0x06, 1)) < 0) return r;
	if ((r = reg_write(dev, DVBT_SPEC_INV, 1)) < 0) return r;

	/*
	 * And the mode bit itself. page0 0x19 is what actually selects which
	 * source feeds the USB FIFO: 0x05 is librtlsdr's "enable SDR mode"
	 * (raw ADC samples), 0x20 is DVB-T (the demodulator's transport
	 * stream). Without re-asserting it here the demodulator can lock
	 * perfectly and you still read uniformly random bytes off the
	 * endpoint, because you are reading the ADC, not the TS.
	 */
	if ((r = rtlsdr_demod_write_reg(dev, 0, 0x19, 0x20, 1)) < 0) return r;

	/* RTL_DVBT_SERIAL: TS output format select (serial vs parallel). The
	 * tables set 0; worth being able to flip while the demodulator locks
	 * but the endpoint still yields non-TS bytes. */
	{
		const char *env = getenv("RTL_DVBT_SERIAL");

		if (env) {
			r = reg_write(dev, DVBT_SERIAL,
				      (uint32_t)strtoul(env, NULL, 0));
			if (r < 0) return r;
		}
	}

	/* RTL_DVBT_MPEGIO="<opt22>:<opt10>" - which tap of the demod chain is
	 * routed out. Small enough space to sweep exhaustively. */
	{
		const char *env = getenv("RTL_DVBT_MPEGIO");
		unsigned a, b;

		if (env && sscanf(env, "%u:%u", &a, &b) == 2) {
			if ((r = reg_write(dev, DVBT_MPEG_IO_OPT_2_2, a)) < 0) return r;
			if ((r = reg_write(dev, DVBT_MPEG_IO_OPT_1_0, b)) < 0) return r;
		}
	}

	/* PID filter disabled (bits 7:6 = 11) - pass the whole stream. */
	if ((r = rtlsdr_demod_write_reg(dev, 0, 0x61, 0xe0, 1)) < 0) return r;

	/*
	 * TODO(phase1): read the IF the tuner actually settled on rather than
	 * assuming. r82xx stores it in priv->int_freq, but that lives behind
	 * librtlsdr's opaque rtlsdr_dev_t, so getting at it means either a
	 * small accessor patch to the submodule or duplicating the bandwidth
	 * table here. Hardcoding the documented value keeps Phase 1 moving;
	 * revisit before trusting 7 and 8 MHz.
	 *
	 * RTL_DVBT_IF overrides it, for sweeping by hand while the
	 * acquisition state machine is stalling and we are still guessing.
	 */
	{
		uint32_t if_hz = (bw_hz <= 6000000) ? 3570000 : 4570000;
		const char *env = getenv("RTL_DVBT_IF");

		if (env)
			if_hz = (uint32_t)strtoul(env, NULL, 0);

		if ((r = set_if(dev, if_hz)) < 0)
			return r;
	}

	/* RTL_DVBT_SPECINV: same idea. The R820T presents an inverted
	 * spectrum, so the tables set this to 1, but it is worth being able to
	 * flip. */
	{
		const char *env = getenv("RTL_DVBT_SPECINV");

		if (env) {
			r = reg_write(dev, DVBT_SPEC_INV,
				      (uint32_t)strtoul(env, NULL, 0));
			if (r < 0)
				return r;
		}
	}

	/* Resampler coefficients for this channel bandwidth. */
	if ((r = demod_wr_bytes(dev, 1, 0x1c, rtl2832_bw_params[bw_idx],
				sizeof(rtl2832_bw_params[bw_idx]))) < 0)
		return r;

	/* RSAMP_RATIO = floor(XTAL * 7 * 2^22 / bw_mode) */
	num = (uint64_t)RTL2832_XTAL * 7 * 0x400000u / bw_mode;
	resamp_ratio = (uint32_t)(num & 0x3ffffff);
	if ((r = reg_write(dev, DVBT_RSAMP_RATIO, resamp_ratio)) < 0)
		return r;

	/* CFREQ_OFF_RATIO = -floor(bw_mode * 2^20 / (XTAL * 7)) */
	num = ((uint64_t)bw_mode << 20) / ((uint64_t)RTL2832_XTAL * 7);
	cfreq_off_ratio = (uint32_t)((uint64_t)(-(int64_t)num) & 0xfffff);
	if ((r = reg_write(dev, DVBT_CFREQ_OFF_RATIO, cfreq_off_ratio)) < 0)
		return r;

	if ((r = reg_write(dev, DVBT_SOFT_RST, 1)) < 0) return r;
	if ((r = reg_write(dev, DVBT_SOFT_RST, 0)) < 0) return r;

	/*
	 * Gain, in order of preference.
	 *
	 * Default is what the kernel does: give the VGA back to the
	 * demodulator's AGC loop and let the hardware track the signal. That
	 * adapts to conditions changing under it, which a fixed gain cannot.
	 *
	 * RTL_DVBT_GAIN pins a manual gain instead. RTL_DVBT_AUTOGAIN=sweep
	 * forces the measured sweep, which is the fallback if the AGC pin
	 * turns out not to lock on some other dongle.
	 */
	{
		const char *g = getenv("RTL_DVBT_GAIN");
		const char *mode = getenv("RTL_DVBT_AUTOGAIN");

		if (g && *g) {
			/* already applied above */
		} else if (mode && !strcmp(mode, "sweep")) {
			auto_gain(dev);
		} else {
			rtl2832_signal_t sig;

			if (r820t_reconnect_agc_pin(dev) < 0)
				fprintf(stderr, "rtl2832: could not hand the "
						"VGA back to the AGC loop\n");

			/*
			 * Give the loop time to settle, then check it got
			 * anywhere. Fall back to sweeping only if it never
			 * locks at all.
			 *
			 * Deliberately not falling back on a high BER: on a
			 * marginal channel the loop locks and still cannot
			 * deliver, and sweeping does not rescue it either.
			 * Measured across five local channels, the two
			 * strategies agree within noise everywhere except the
			 * strongest, where the loop wins outright (BER 0 vs
			 * 57). Spending two seconds sweeping to arrive at the
			 * same bad answer helps nobody.
			 */
			int locked = 0, i;

			for (i = 0; i < 8; i++) {
				usleep(200000);
				if (rtl2832_dvbt_read_signal(dev, &sig) == 0 &&
				    sig.fsm_stage == 11) {
					locked = 1;
					break;
				}
			}

			if (!locked) {
				fprintf(stderr, "rtl2832: AGC loop never "
						"locked - sweeping instead\n");
				auto_gain(dev);
			}
		}
	}

	return 0;
}

/*
 * Full acquisition status, read the way the Linux kernel driver reads it
 * (rtl2832_read_status()). These are the numbers that distinguish "the
 * signal is poor" from "we decoded it wrong":
 *
 *   constellation / hierarchy come from the TPS field. If they are garbage,
 *   the demodulator misread the transmission parameters and everything
 *   downstream is doomed regardless of signal strength. The kernel treats
 *   out-of-range values as a hard error.
 *
 *   CNR is computed from the channel estimator's error-vector magnitude with
 *   the kernel's per-constellation constants, in units of 0.001 dB.
 */
int rtl2832_dvbt_read_signal(rtlsdr_dev_t *dev, rtl2832_signal_t *out)
{
	static const uint32_t cnr_const[3][4] = {
		{85387325, 85387325, 85387325, 85387325},
		{86676178, 86676178, 87167949, 87795660},
		{87659938, 87659938, 87885178, 88241743},
	};
	uint32_t stage;
	uint16_t tps, evm;

	if (!dev || !out)
		return -1;

	memset(out, 0, sizeof(*out));

	if (reg_read(dev, DVBT_FSM_STAGE, &stage) < 0)
		return -1;
	out->fsm_stage = (int)stage;

	/* digital AGC, page3 0x05 - how hard the demod is pulling gain */
	out->digital_agc = demod_rd(dev, 3, 0x05, 1) & 0xff;

	tps = demod_rd(dev, 3, 0x3c, 1) & 0xff;
	out->constellation = (tps >> 2) & 0x03;
	out->hierarchy     = (tps >> 4) & 0x07;
	out->tps_valid     = (out->constellation < 3 && out->hierarchy < 4);

	evm = demod_rd(dev, 4, 0x0c, 2);
	out->evm = evm;

	if (out->tps_valid && evm) {
		double l = log10((double)evm) * 16777216.0;
		out->cnr_millidb = (int)((cnr_const[out->constellation]
					  [out->hierarchy] - l)
					 / ((1 << 24) / 10000.0));
	}

	out->ber = demod_rd(dev, 3, 0x4e, 2);

	return 0;
}

void rtl2832_dvbt_dump_regs(rtlsdr_dev_t *dev, const char *when)
{
	static const struct { uint8_t page; uint16_t addr; const char *what; } r[] = {
		{0, 0x06, "MPEG_IO_OPT_2_2 (b7) / OPT_ADC_IQ"},
		{0, 0x07, "MPEG_IO_OPT_1_0 (b7:6)"},
		{0, 0x08, "AD_EN_REG (b7) / AD_EN_REG1 (b6)  SDR=0x4d"},
		{0, 0x19, "mode  SDR=0x05  DVB-T=0x20"},
		{0, 0x61, "PID filter  disabled=0xe0"},
		{1, 0x01, "SOFT_RST (b2) / IIC_REPEAT (b3)"},
		{1, 0x04, "AGC: EN_IF (b7) EN_RF (b6) HOLD (b5)"},
		{1, 0x15, "SPEC_INV (b0)  we want 0x07"},
		{1, 0xb1, "EN_BBIN (b0)  SDR=0x1a"},
	};

	fprintf(stderr, "\n--- demod registers %s ---\n", when);
	for (unsigned i = 0; i < sizeof(r)/sizeof(r[0]); i++)
		fprintf(stderr, "  page%d 0x%02x = 0x%02x   %s\n",
			r[i].page, r[i].addr,
			demod_rd(dev, r[i].page, r[i].addr, 1), r[i].what);
	{
		uint32_t if_agc = 0, rf_agc = 0;

		reg_read(dev, DVBT_IF_AGC_VAL, &if_agc);
		reg_read(dev, DVBT_RF_AGC_VAL, &rf_agc);

		/* 14-bit two's complement. Pinned at a rail across repeated
		 * reads means the loop has no actuator - see
		 * r820t_reconnect_agc_pin(). */
		fprintf(stderr, "  IF_AGC = %6d   RF_AGC = %6d\n",
			(if_agc & 0x2000) ? (int)if_agc - 0x4000 : (int)if_agc,
			(rf_agc & 0x2000) ? (int)rf_agc - 0x4000 : (int)rf_agc);
	}
	fprintf(stderr, "  FSM stage = %d\n\n", rtl2832_dvbt_get_fsm_stage(dev));
}

int rtl2832_dvbt_get_fsm_stage(rtlsdr_dev_t *dev)
{
	uint32_t stage;

	if (!dev || reg_read(dev, DVBT_FSM_STAGE, &stage) < 0)
		return -1;

	return (int)stage;
}

int rtl2832_dvbt_get_status(rtlsdr_dev_t *dev)
{
	int stage;

	if (!dev)
		return -1;

	/*
	 * The demodulator exposes its acquisition progress as a state-machine
	 * stage number, not a bitmask. Stage 11 is full lock; stage 10 means
	 * signal, carrier and Viterbi but no sync yet. Anything lower means it
	 * is still hunting, which is far more informative than a boolean -
	 * see rtl2832_dvbt_get_fsm_stage().
	 */
	stage = rtl2832_dvbt_get_fsm_stage(dev);
	if (stage < 0)
		return -1;

	if (stage == 11)
		return RTL2832_HAS_SIGNAL | RTL2832_HAS_CARRIER |
		       RTL2832_HAS_VITERBI | RTL2832_HAS_SYNC |
		       RTL2832_HAS_LOCK;

	if (stage == 10)
		return RTL2832_HAS_SIGNAL | RTL2832_HAS_CARRIER |
		       RTL2832_HAS_VITERBI;

	return 0;
}

/*
 * Bit errors per ~1 MB, straight from the demodulator's Reed-Solomon stage
 * (Java readBer(): page 3, 0x4e, 16 bits). This is the number that says
 * whether a weak signal is the problem: a demodulator can report lock and
 * still be handing you packets it could not correct.
 */
int rtl2832_dvbt_read_ber(rtlsdr_dev_t *dev)
{
	if (!dev)
		return -1;

	return (int)demod_rd(dev, 3, 0x4e, 2);
}

int rtl2832_dvbt_read_snr(rtlsdr_dev_t *dev)
{
	uint32_t constel, hier, evm;

	if (!dev)
		return -1;

	if (reg_read(dev, DVBT_RX_CONSTEL, &constel) < 0 ||
	    reg_read(dev, DVBT_RX_HIER, &hier) < 0 ||
	    reg_read(dev, DVBT_CE_EST_EVM, &evm) < 0)
		return -1;

	/*
	 * The real figure is
	 *   (SNR_CONST[constel][hier] - intlog10(evm)) / ((1 << 24) / 100)
	 * which needs the constant table and fixed-point log from the Java.
	 * Until the demodulator locks there is nothing to measure, so rather
	 * than port that now we return the raw error-vector magnitude: it is
	 * monotonic in the right direction (lower is better) and enough to
	 * tell whether acquisition is improving.
	 */
	(void)constel;
	(void)hier;

	return (int)evm;
}
