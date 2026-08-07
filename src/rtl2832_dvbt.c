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
#include <stdlib.h>
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
		const char *env = getenv("RTL_DVBT_GAINMODE");
		int mode = env ? (int)strtol(env, NULL, 0) : 0;

		rtlsdr_set_tuner_gain_mode(dev, mode);
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
