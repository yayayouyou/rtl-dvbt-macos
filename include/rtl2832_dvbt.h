/*
 * rtl2832_dvbt.h - DVB-T mode for RTL2832U on macOS
 *
 * Copyright (C) 2026  <your name>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * Ported from AndroidDvbDriver (GPLv2), which is itself a userspace port
 * of the Linux kernel V4L2 rtl2832 driver (GPLv2).
 */

#ifndef RTL2832_DVBT_H
#define RTL2832_DVBT_H

#include <stdint.h>
#include <rtl-sdr.h>

/* The RTL2832U's reference crystal. Every resampler and IF calculation
 * is derived from this, so a dongle with a non-standard crystal will not
 * lock until this matches. */
#define RTL2832_XTAL 28800000U

/* Demodulator lock status, cheapest-to-acquire first. In practice you watch
 * these in order: without SYNC there is no point looking at the rest. */
typedef enum {
	RTL2832_HAS_SIGNAL = 1 << 0,
	RTL2832_HAS_CARRIER = 1 << 1,
	RTL2832_HAS_VITERBI = 1 << 2,
	RTL2832_HAS_SYNC = 1 << 3,
	RTL2832_HAS_LOCK = 1 << 4,
} rtl2832_status_t;

/*
 * Take the chip out of SDR mode and initialise the DVB-T demodulator.
 *
 * The dongle must already be open via rtlsdr_open(). This is destructive to
 * SDR operation: after this call the device streams MPEG-2 TS, not I/Q, and
 * you must close and reopen to go back.
 *
 * Returns 0 on success, negative on failure.
 */
int rtl2832_dvbt_init(rtlsdr_dev_t *dev);

/*
 * Tune to a channel.
 *
 * freq_hz  - channel centre frequency
 * bw_hz    - channel bandwidth: 6000000 (Taiwan, Americas),
 *            7000000, or 8000000 (most of Europe)
 *
 * Returns 0 on success, negative on failure. Note that success here only
 * means the registers were written; call rtl2832_dvbt_get_status() after a
 * short settling delay to find out whether the demodulator actually locked.
 */
int rtl2832_dvbt_set_params(rtlsdr_dev_t *dev, uint32_t freq_hz, uint32_t bw_hz);

/*
 * Read the current lock status as a bitmask of rtl2832_status_t.
 * Returns negative on failure.
 */
int rtl2832_dvbt_get_status(rtlsdr_dev_t *dev);

/*
 * Raw acquisition state-machine stage, 0-15. This is the single most useful
 * diagnostic when nothing locks: it says how far the demodulator got.
 * 11 = full lock, 10 = signal/carrier/Viterbi but no sync, lower = still
 * hunting. Returns negative on failure.
 */
void rtl2832_dvbt_dump_regs(rtlsdr_dev_t *dev, const char *when);
int rtl2832_dvbt_get_fsm_stage(rtlsdr_dev_t *dev);

/*
 * Channel-estimator error-vector magnitude. Lower is better. Not yet
 * converted to dB - see the comment in the implementation. Returns negative
 * on failure.
 */
int rtl2832_dvbt_read_snr(rtlsdr_dev_t *dev);

#endif /* RTL2832_DVBT_H */
