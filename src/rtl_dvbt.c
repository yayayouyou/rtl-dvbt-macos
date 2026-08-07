/*
 * rtl_dvbt.c - dump a DVB-T transport stream from an RTL2832U on macOS
 *
 * Copyright (C) 2026  <your name>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rtl2832_dvbt.h"

#define DEFAULT_BUF_LEN (16 * 16384)

static volatile int do_exit = 0;
static rtlsdr_dev_t *dev = NULL;
static uint64_t bytes_written = 0;
static FILE *out = NULL;

static void sighandler(int signum)
{
	(void)signum;
	fprintf(stderr, "\nStopping...\n");
	do_exit = 1;
	if (dev)
		rtlsdr_cancel_async(dev);
}

static void usage(void)
{
	fprintf(stderr,
"rtl_dvbt - DVB-T transport stream capture for RTL2832U on macOS\n"
"\n"
"Usage: rtl_dvbt -f <freq> [-b <bandwidth>] [-d <index>] [-n <seconds>] [file]\n"
"\n"
"  -f <freq>       channel centre frequency in Hz (accepts 545M, 545000000)\n"
"  -b <bandwidth>  channel bandwidth in Hz, default 6000000 (Taiwan)\n"
"  -d <index>      device index, default 0\n"
"  -n <seconds>    stop after this many seconds, default run until Ctrl-C\n"
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
		fprintf(stderr, "Short write, stopping\n");
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

	while ((opt = getopt(argc, argv, "f:b:d:n:h")) != -1) {
		switch (opt) {
		case 'f': freq = parse_freq(optarg); break;
		case 'b': bw = parse_freq(optarg); break;
		case 'd': dev_index = atoi(optarg); break;
		case 'n': seconds = atoi(optarg); break;
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
		fprintf(stderr, "No RTL-SDR devices found.\n");
		return 1;
	}

	r = rtlsdr_open(&dev, dev_index);
	if (r < 0) {
		fprintf(stderr, "Failed to open device %d\n", dev_index);
		return 1;
	}

	fprintf(stderr, "Device: %s\n", rtlsdr_get_device_name(dev_index));

	if (rtlsdr_get_tuner_type(dev) != RTLSDR_TUNER_R820T) {
		fprintf(stderr,
			"Warning: only the R820T tuner is implemented. This "
			"dongle reports a different tuner, so the register "
			"tables below will not match and it will not lock.\n");
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
		fprintf(stderr, "\nLOCKED - capturing now.\n");
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

	if (seconds > 0) {
		sa.sa_handler = sighandler;
		sigaction(SIGALRM, &sa, NULL);
		alarm((unsigned)seconds);
	}

	if (getenv("RTL_DVBT_DUMP"))
		rtl2832_dvbt_dump_regs(dev, "just before capture");

	rtlsdr_reset_buffer(dev);

	fprintf(stderr, "Capturing... (Ctrl-C to stop)\n");
	rtlsdr_read_async(dev, ts_callback, NULL, 0, DEFAULT_BUF_LEN);

	fprintf(stderr, "Wrote %llu bytes (%.1f MB)\n",
		(unsigned long long)bytes_written, bytes_written / 1048576.0);

	if (bytes_written == 0)
		fprintf(stderr,
			"Nothing was captured. The device is not producing "
			"data on the bulk endpoint.\n");

	rtlsdr_close(dev);
	if (out != stdout)
		fclose(out);
	return 0;

fail:
	rtlsdr_close(dev);
	if (out != stdout)
		fclose(out);
	return 1;
}
