# rtl-dvbt-macos

DVB-T reception for RTL2832U dongles on macOS / Apple Silicon.

**It works.** Verified on Apple Silicon against Taiwan's terrestrial DVB-T:
locks, delivers a clean MPEG-2 transport stream at BER 0, and decodes to
1920x1080 H.264.

```
$ rtl_dvbt -f 581M -n 10 ttv.ts
Switching to DVB-T mode...
Tuning to 581.000 MHz, 6 MHz bandwidth...
Gain 22.9 dB (BER 0/1e6)
  stage=11  agc=0x20  constellation=64QAM  hierarchy=0  TPS=valid
  CNR=24.42 dB  EVM=606  BER=0/1e6
Wrote 14680064 bytes (14.0 MB)

$ ffprobe ttv.ts
  program 400: 台灣電視台 / TTV    h264 1920x1080
  program 401: 台視新聞台 / TTV    h264 1920x1080
  program 402: 台視財經台 / TTV    h264 1920x1080
  program 403: 台視綜合台 / TTV    h264 1920x1080
```

## Why this exists

RTL2832U dongles are sold as DVB-T receivers and contain a **hardware DVB-T
demodulator**. On Linux, Windows and Android that demodulator is used and you
can watch television. On macOS there was nothing — you could use the stick as
an SDR via `librtlsdr`, but nobody had written the code to put the chip into
its television mode.

`librtlsdr` already does the hard parts: libusb transport, demodulator
register access, the R820T tuner, async bulk reads. It simply spends its
initialisation *disabling* the demodulator so raw I/Q reaches the host. This
project is the code that switches it back.

## Status

| | |
|---|---|
| Builds on Apple Silicon | yes |
| Locks on real DVB-T | yes, BER 0 |
| Valid MPEG-2 TS out | yes, 100% packet sync |
| Plays in VLC / ffmpeg | yes |
| Tuners supported | **R820T only** (E4000 / FC0012 tables are generated but not wired up) |
| Bandwidths | 6 / 7 / 8 MHz — only 6 MHz verified on air |
| Channel scanning | not yet |
| DVB-T2 | no |

Tested on one dongle (`0bda:2838`, R820T) in one country. Reports from other
hardware and other broadcast standards are very welcome.

## Build

```bash
git clone --recursive https://github.com/<you>/rtl-dvbt-macos
cd rtl-dvbt-macos
brew install cmake pkg-config libusb
cmake -S . -B build && cmake --build build
```

If you cloned without `--recursive`: `git submodule update --init --recursive`

## Use

```bash
./build/rtl_dvbt -f 581M -n 10 out.ts
ffprobe out.ts
```

Watch it live. There is no player in this project and there does not need to
be — anything that reads MPEG-2 TS from a pipe works:

```bash
./build/rtl_dvbt -f 581M | ffplay -
```

`ffplay` ships with ffmpeg, which you probably already have if you ran
`ffprobe` above. `mpv -` and `vlc -` work the same way; install them first
(`brew install mpv`, `brew install --cask vlc`) — piping into a command that
does not exist gives you a broken pipe, and `rtl_dvbt` will say so and stop.

Bandwidth defaults to 6 MHz (Taiwan, Americas). Use `-b 7000000` or
`-b 8000000` elsewhere.

Don't know which frequencies carry signal? Scan the UHF band first with
librtlsdr's own tool:

```bash
rtl_power -f 470M:700M:250k -g 40 -i 3 -e 30 scan.csv
```

Channels appear as flat plateaus 6 MHz wide, on 6 MHz centres.

### Gain

The driver sweeps tuner gain on tune and keeps whatever decodes best, which
takes a second or two. Pin it with `RTL_DVBT_GAIN=<tenths of a dB>` to skip
that.

This is not a nicety. librtlsdr's automatic gain is optimised for SDR — it
maximises level into the ADC, which on a strong transmitter drives the R820T
into compression. The demodulator still locks, still reports valid TPS, and
still hands you bytes at the right rate — but Reed-Solomon cannot correct any
of them. Same channel, seconds apart:

| gain | CNR | BER | usable? |
|---|---|---|---|
| librtlsdr automatic | 16.1 dB | 65535/1e6 | no — indistinguishable from noise |
| 22.9 dB (swept) | 24.4 dB | 0/1e6 | yes |

### Diagnostics

`RTL_DVBT_DUMP=1` prints every relevant demodulator register before capture.

The per-tune status line is the kernel driver's own view of the signal:
`stage` 11 means full lock, `TPS=valid` means the transmission parameters
decoded (you cannot get that from noise), `BER` is post-Viterbi errors per
million bits. If `BER` is in the tens of thousands, suspect gain before
anything else.

If a dongle stops producing data entirely — even under plain `rtl_sdr` — it is
wedged, not bricked. `tools/usbreset.c` clears it without unplugging.

## Architecture

```
src/rtl_dvbt.c          CLI: open, init, tune, gain search, dump TS
src/rtl2832_dvbt.c      the driver: mode switch, registers, tuning, status
include/rtl2832_regs.h  GENERATED register tables — do not hand-edit
tools/gen_regs.py       regenerates the above from the Java source
tools/usbreset.c        recover a wedged dongle
third_party/librtlsdr   submodule; compiled in, not linked
```

`librtlsdr` is **compiled from source into the binary** rather than linked.
This is required, not stylistic: `rtlsdr_demod_write_reg()` is the basis of
this entire driver, it is not declared in the public header, and librtlsdr
builds with `-fvisibility=hidden`. A Homebrew `librtlsdr` does not export it.

Two values in the vendored `tuner_r82xx.c` are changed back to the Linux
kernel's DVB-T settings (`filt_gain` 0x30 → 0x10, `loop_through` 0x80 → 0x00);
librtlsdr had altered them for SDR use.

## Credit

This is a port, not original research. The hard work was done by others:

- **[AndroidDvbDriver](https://github.com/signalwareltd/AndroidDvbDriver)**
  (Signalware Ltd / Martin Marinov, GPLv2) — the userspace DVB-T driver this
  is transcribed from. It proved the approach: no kernel driver needed.
- **[librtlsdr](https://github.com/rtlsdrblog/rtl-sdr-blog)** (GPLv2) — USB
  transport, tuner drivers, demodulator register primitives.
- The **Linux kernel V4L2 rtl2832 driver** (GPLv2), common ancestor of both,
  and the reference used to settle every disagreement.

## License

GPLv2, inherited from both upstreams. See [COPYING](COPYING).
