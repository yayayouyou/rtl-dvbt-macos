/*
 * rtl2832_dvbt.c - DVB-T mode for RTL2832U on macOS
 *
 * Copyright (C) 2026  <your name>
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

	return 0;
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

	/* Drive the tuner through librtlsdr's public API. r82xx_set_bandwidth()
	 * already carries the DVB-T IF values inherited from the kernel driver
	 * (3.57 MHz at 6 MHz bandwidth), so we do not need to reach into the
	 * tuner privately. */
	if ((r = rtlsdr_set_tuner_bandwidth(dev, bw_hz)) < 0) {
		fprintf(stderr, "rtl2832: set_tuner_bandwidth failed\n");
		return r;
	}
	if ((r = rtlsdr_set_center_freq(dev, freq_hz)) < 0) {
		fprintf(stderr, "rtl2832: set_center_freq failed\n");
		return r;
	}

	/*
	 * TODO(phase1): read the IF the tuner actually settled on rather than
	 * assuming. r82xx stores it in priv->int_freq, but that lives behind
	 * librtlsdr's opaque rtlsdr_dev_t, so getting at it means either a
	 * small accessor patch to the submodule or duplicating the bandwidth
	 * table here. Hardcoding the documented value keeps Phase 1 moving;
	 * revisit before trusting 7 and 8 MHz.
	 */
	{
		uint32_t if_hz = (bw_hz <= 6000000) ? 3570000 : 4570000;
		if ((r = set_if(dev, if_hz)) < 0)
			return r;
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

	return 0;
}

int rtl2832_dvbt_get_status(rtlsdr_dev_t *dev)
{
	uint16_t tmp;
	int status = 0;

	if (!dev)
		return -1;

	tmp = demod_rd(dev, 1, 0x3c, 1);

	/* Bit 3 of the FSM state register means the demodulator has reached
	 * sync. The kernel driver treats that as everything below it being
	 * true as well, since the stages lock in order. */
	if (tmp & 0x08) {
		status = RTL2832_HAS_SIGNAL | RTL2832_HAS_CARRIER |
			 RTL2832_HAS_VITERBI | RTL2832_HAS_SYNC |
			 RTL2832_HAS_LOCK;
	}

	return status;
}

int rtl2832_dvbt_read_snr(rtlsdr_dev_t *dev)
{
	if (!dev)
		return -1;

	/* Reported in units of 0.1 dB. */
	return (int)demod_rd(dev, 3, 0x3c, 1);
}
