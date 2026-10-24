/*
 * rtl_dvbt.c - dump a DVB-T transport stream from an RTL2832U on macOS
 *
 * Copyright (C) 2026  yayayouyou
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <libusb.h>

#include "rtl2832_dvbt.h"

/* Realtek RTL2832U. Same pair tools/usbreset.c uses. */
#define RTL_VID 0x0bda
#define RTL_PID 0x2838

/*
 * Reset the dongle at the USB level. Returns 0 if a reset was issued.
 *
 * Only works when the device is enumerated - a wedged dongle, not an absent
 * one. See the caller for why that distinction matters.
 */
static int usb_reset_device(void)
{
	libusb_context *ctx = NULL;
	libusb_device_handle *h;
	int r = -1;

	if (libusb_init(&ctx) < 0)
		return -1;

	h = libusb_open_device_with_vid_pid(ctx, RTL_VID, RTL_PID);
	if (h) {
		r = libusb_reset_device(h);
		libusb_close(h);
	}

	libusb_exit(ctx);
	return r;
}

#define DEFAULT_BUF_LEN (16 * 16384)

static volatile int do_exit = 0;
static rtlsdr_dev_t *dev = NULL;

/* -s: seconds between signal reports while capturing; 0 reports once. */
static int report_every = 0;
static volatile int capturing = 0;
static uint64_t bytes_written = 0;
static volatile int write_failed = 0;
static FILE *out = NULL;

static void sighandler(int signum)
{
	(void)signum;
	fprintf(stderr, "\nStopping...\n");
	do_exit = 1;
	if (dev)
		rtlsdr_cancel_async(dev);
}

static void print_signal(const rtl2832_signal_t *sig)
{
	fprintf(stderr,
		"\n  stage=%d  agc=0x%02x  constellation=%s  "
		"hierarchy=%d  TPS=%s\n  CNR=%.2f dB  EVM=%d  "
		"BER=%d/1e6\n",
		sig->fsm_stage, sig->digital_agc,
		sig->constellation == 0 ? "QPSK" :
		sig->constellation == 1 ? "16QAM" :
		sig->constellation == 2 ? "64QAM" : "INVALID",
		sig->hierarchy,
		sig->tps_valid ? "valid" : "GARBAGE",
		sig->cnr_millidb / 1000.0, sig->evm, sig->ber);
}

/*
 * Signal reports during capture (-s).
 *
 * Reception changes under a running capture - weather, interference, an
 * antenna someone nudged - and a single report taken at lock says nothing
 * about the hours after it. A front end showing that one report as current
 * was showing a number that could be hours stale, and could not tell a
 * channel that had faded from one that was fine.
 *
 * Reading the demodulator's status registers while streaming is what the
 * Linux driver does all along. They are control transfers on endpoint 0 and
 * read-only, so they do not disturb the bulk transfers carrying the stream.
 * BER is the error count since the previous read, so each report covers the
 * interval since the last one.
 */
static void *reporter(void *arg)
{
	(void)arg;

	while (capturing && !do_exit) {
		for (int i = 0; i < report_every * 10; i++) {
			if (!capturing || do_exit)
				return NULL;
			usleep(100000);
		}

		rtl2832_signal_t sig;

		if (rtl2832_dvbt_read_signal(dev, &sig) == 0)
			print_signal(&sig);
	}
	return NULL;
}

static void usage(void)
{
	fprintf(stderr,
"rtl_dvbt - DVB-T transport stream capture for RTL2832U on macOS\n"
"\n"
"Usage: rtl_dvbt -f <freq> [-b <bandwidth>] [-d <index>] [-n <seconds>]\n"
"                [-s <seconds>] [file]\n"
"\n"
"  -f <freq>       channel centre frequency in Hz (accepts 545M, 545000000)\n"
"  -b <bandwidth>  channel bandwidth in Hz, default 6000000 (Taiwan)\n"
"  -d <index>      device index, default 0\n"
"  -n <seconds>    stop after this many seconds, default run until Ctrl-C\n"
"  -s <seconds>    report signal quality every N seconds while capturing,\n"
"                  default only once, at lock\n"
"  file            output file, default stdout\n"
"\n"
"Example - capture 10 seconds and inspect what came back:\n"
"  rtl_dvbt -f 545M -n 10 out.ts && ffprobe out.ts\n"
"\n"
"A valid capture shows MPEG-2 TS with PAT/PMT tables. Anything else means\n"
"the demodulator did not lock - see docs/PORTING.md.\n");
	exit(1);
}

/* Accept 545M / 545000k / 545000000 */
static uint32_t parse_freq(const char *s)
{
	char *end;
	double v = strtod(s, &end);

	switch (*end) {
	case 'g': case 'G': v *= 1e9; break;
	case 'm': case 'M': v *= 1e6; break;
	case 'k': case 'K': v *= 1e3; break;
	default: break;
	}
	return (uint32_t)v;
}

static void ts_callback(unsigned char *buf, uint32_t len, void *ctx)
{
	(void)ctx;

	if (do_exit)
		return;

	if (fwrite(buf, 1, len, out) != len) {
		/* The common case by far is piping into a player that is not
		 * installed, where the shell reports "command not found" and
		 * we inherit the broken pipe. Say so, rather than dying
		 * silently right after announcing a good lock. */
		if (errno == EPIPE)
			fprintf(stderr,
				"\nOutput pipe closed - is the program you "
				"piped into installed and still running?\n");
		else
			perror("\nWrite failed");

		write_failed = 1;
		do_exit = 1;
		rtlsdr_cancel_async(dev);
		return;
	}
	bytes_written += len;
}

int main(int argc, char **argv)
{
	uint32_t freq = 0, bw = 6000000;
	int dev_index = 0, seconds = 0;
	struct sigaction sa;
	int opt, r, status;

	while ((opt = getopt(argc, argv, "f:b:d:n:s:h")) != -1) {
		switch (opt) {
		case 'f': freq = parse_freq(optarg); break;
		case 'b': bw = parse_freq(optarg); break;
		case 'd': dev_index = atoi(optarg); break;
		case 'n': seconds = atoi(optarg); break;
		case 's': report_every = atoi(optarg); break;
		default: usage();
		}
	}

	if (!freq)
		usage();

	if (optind < argc) {
		out = fopen(argv[optind], "wb");
		if (!out) {
			fprintf(stderr, "Cannot open %s for writing\n",
				argv[optind]);
			return 1;
		}
	} else {
		out = stdout;
	}

	if (rtlsdr_get_device_count() == 0) {
		/*
		 * Zero devices means nothing matching is on the USB bus at all,
		 * which no amount of resetting will change - libusb cannot open
		 * what it cannot find. Say what to check rather than just what
		 * failed, because "plugged in but not detected" is common with
		 * these dongles and the fix is physical.
		 */
		fprintf(stderr,
			"No RTL-SDR devices found.\n"
			"The dongle is not on the USB bus. Confirm with:\n"
			"  ioreg -p IOUSB -l | grep RTL2838\n"
			"If that prints nothing, unplug and replug it - and "
			"prefer a port on the machine itself over a hub.\n");
		return 1;
	}

	r = rtlsdr_open(&dev, dev_index);
	if (r < 0) {
		/*
		 * A dongle that is present but wedged will refuse to open, and
		 * stays that way until something resets it at the USB level -
		 * a previous run killed at the wrong moment, or a bad register
		 * write to the output-routing fields, will both do it. Since
		 * we can fix that without troubling the user, do.
		 *
		 * This cannot help a dongle that is not enumerated at all.
		 * Nothing in userspace can: macOS exposes no way to power-cycle
		 * a port. If it is not on the bus, it needs a hand.
		 */
		fprintf(stderr, "Could not open device %d, resetting USB...\n",
			dev_index);

		if (usb_reset_device() == 0) {
			usleep(1500000); /* let it re-enumerate */
			r = rtlsdr_open(&dev, dev_index);
		}

		if (r < 0) {
			fprintf(stderr,
				"Still could not open device %d. If it is "
				"plugged in but never appears, unplug and "
				"replug it - check with:\n"
				"  ioreg -p IOUSB -l | grep RTL2838\n",
				dev_index);
			return 1;
		}
		fprintf(stderr, "Recovered.\n");
	}

	fprintf(stderr, "Device: %s\n", rtlsdr_get_device_name(dev_index));

	{
		enum rtlsdr_tuner t = rtlsdr_get_tuner_type(dev);

		if (t == RTLSDR_TUNER_R828D) {
			/* Not tested here - nobody involved has held one. But
			 * it is the tuner in most sticks actually sold as
			 * DVB-T receivers, and librtlsdr gives R820T and R828D
			 * the same driver and the same demodulator init path,
			 * so there is no reason to believe it cannot work.
			 * Saying "it will not lock" would be a claim with
			 * nothing behind it. */
			fprintf(stderr,
				"R828D detected - untested, but it shares the "
				"R820T driver and register tables upstream, so "
				"it may well work. Please report the result.\n");
		} else if (t != RTLSDR_TUNER_R820T) {
			fprintf(stderr,
				"This dongle's tuner is not an R820T or R828D.\n"
				"The demodulator register table, spectrum "
				"inversion and IF frequency in this driver are "
				"all R82xx values; they will not match your "
				"tuner and it will not lock.\n"
				"Set RTL_DVBT_FORCE_TUNER=1 to try anyway.\n");
			if (!getenv("RTL_DVBT_FORCE_TUNER"))
				goto fail;
		}
	}

	/* librtlsdr's open() leaves the chip in SDR mode; undo that. */
	fprintf(stderr, "Switching to DVB-T mode...\n");
	if (rtl2832_dvbt_init(dev) < 0) {
		fprintf(stderr, "DVB-T init failed\n");
		goto fail;
	}

	fprintf(stderr, "Tuning to %.3f MHz, %.0f MHz bandwidth...\n",
		freq / 1e6, bw / 1e6);
	if (rtl2832_dvbt_set_params(dev, freq, bw) < 0) {
		fprintf(stderr, "Tuning failed\n");
		goto fail;
	}

	/*
	 * Wait for lock, then start capturing immediately.
	 *
	 * This matters more than it looks. On a marginal signal the
	 * demodulator locks and drops repeatedly, so any time spent between
	 * observing lock and opening the endpoint is time the lock can be lost
	 * in - and what lands in the file is then whatever the chip emits
	 * while unlocked, which is indistinguishable from noise.
	 */
	fprintf(stderr, "Waiting for lock (FSM stage, 11 = lock): ");
	status = 0;
	for (int i = 0; i < 100; i++) {
		int st = rtl2832_dvbt_get_fsm_stage(dev);

		if (i % 5 == 0)
			fprintf(stderr, "%d ", st);

		if (st == 11) {
			status = 1;
			break;
		}
		usleep(100000);
	}

	if (status)
	{
		rtl2832_signal_t sig;

		if (rtl2832_dvbt_read_signal(dev, &sig) == 0)
			print_signal(&sig);
	}
	else
		fprintf(stderr,
			"\nNever reached lock in 10s (last stage %d). "
			"Capturing anyway.\n",
			rtl2832_dvbt_get_fsm_stage(dev));

	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sa.sa_handler = sighandler;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	/* Take SIGPIPE ourselves. Its default action is to kill the process
	 * outright, which meant piping into a missing player looked like the
	 * driver crashing immediately after reporting a clean lock. */
	sa.sa_handler = SIG_IGN;
	sigaction(SIGPIPE, &sa, NULL);

	if (seconds > 0) {
		sa.sa_handler = sighandler;
		sigaction(SIGALRM, &sa, NULL);
		alarm((unsigned)seconds);
	}

	if (getenv("RTL_DVBT_DUMP"))
		rtl2832_dvbt_dump_regs(dev, "just before capture");

	rtlsdr_reset_buffer(dev);

	fprintf(stderr, "Capturing... (Ctrl-C to stop)\n");

	pthread_t reporter_thread;
	int reporting = 0;

	capturing = 1;
	if (report_every > 0)
		reporting = pthread_create(&reporter_thread, NULL,
					   reporter, NULL) == 0;

	rtlsdr_read_async(dev, ts_callback, NULL, 0, DEFAULT_BUF_LEN);

	/* Stop reporting before anything below closes the device. */
	capturing = 0;
	if (reporting)
		pthread_join(reporter_thread, NULL);

	fprintf(stderr, "Wrote %llu bytes (%.1f MB)\n",
		(unsigned long long)bytes_written, bytes_written / 1048576.0);

	/*
	 * Report failure honestly. rtlsdr_read_async() returns 0 whether the
	 * capture ended cleanly or the device vanished mid-stream - libusb's
	 * callback sets a private "device lost" flag and cancels, and the
	 * return value cannot tell the two apart. Probing the demodulator
	 * afterwards can: reg_read fails and get_fsm_stage returns negative
	 * once the device is gone.
	 */
	if (write_failed) {
		r = 1;
	} else if (bytes_written == 0) {
		fprintf(stderr,
			"Nothing was captured. The device is not producing "
			"data on the bulk endpoint.\n");
		r = 1;
	} else if (rtl2832_dvbt_get_fsm_stage(dev) < 0) {
		fprintf(stderr,
			"Device stopped responding - the capture is "
			"truncated.\n");
		r = 1;
	} else {
		r = 0;
	}

	rtlsdr_close(dev);
	if (out != stdout)
		fclose(out);
	return r;

fail:
	rtlsdr_close(dev);
	if (out != stdout)
		fclose(out);
	return 1;
}
