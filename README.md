# rtl-dvbt-macos

DVB-T reception for RTL2832U dongles on macOS / Apple Silicon.

## Status: pre-alpha, unverified on hardware

The code builds and links. It has **never been run against a real dongle with
a real antenna**, so it has not yet received a single television frame. Treat
every claim below as "this is what the registers are supposed to do", not
"this works".

If you try it and it locks, please open an issue — that datapoint is currently
the most valuable thing this project could receive.

## Why this exists

RTL2832U dongles are sold as DVB-T receivers and contain a **hardware DVB-T
demodulator**. On Linux and Windows that demodulator is used and you can watch
television. On Android there is a userspace driver. On macOS there is nothing —
you can use the stick as an SDR via `librtlsdr`, but nobody has ever written the
code to put the chip into its television mode.

That gap is smaller than it looks. `librtlsdr` already does the hard parts:
libusb transport, demodulator register access, the R820T tuner, async bulk
reads. It simply spends its initialisation *disabling* the demodulator so raw
I/Q reaches the host. The missing piece is the code to switch back — and the
core of that is eight register writes.

This project is that missing piece.

## What works / what doesn't

| | |
|---|---|
| Builds on Apple Silicon | yes |
| Register tables transcribed | yes, all 127 descriptors + 4 init tables |
| Mode switch implemented | yes, untested |
| Tuning implemented | yes, untested |
| Verified signal lock | **no — not yet run on hardware** |
| Channel scanning | no |
| Anything resembling a TV app | no |

Only the **R820T** tuner is wired up. E4000 and FC0012 tables are generated and
present but not hooked into the init path.

## Build

```bash
git clone --recursive https://github.com/<you>/rtl-dvbt-macos
cd rtl-dvbt-macos
brew install cmake pkg-config libusb
cmake -S . -B build && cmake --build build
```

If you cloned without `--recursive`:

```bash
git submodule update --init --recursive
```

## Use

```bash
./build/rtl_dvbt -f 545M -n 10 out.ts
ffprobe out.ts
```

A working capture shows an MPEG-2 transport stream with PAT/PMT tables. If
`ffprobe` cannot identify the file, the demodulator did not lock — see
[docs/PORTING.md](docs/PORTING.md) for what to suspect and in what order.

Bandwidth defaults to 6 MHz (Taiwan, Americas). Use `-b 7000000` or
`-b 8000000` elsewhere.

Before blaming the code, confirm there is signal to receive at all:

```bash
rtl_power -f 470M:600M:1M -g 40 -i 10 scan.csv
```

## Architecture

```
src/rtl_dvbt.c        CLI: open, init, tune, dump TS to file/stdout
src/rtl2832_dvbt.c    the driver: mode switch, register access, tuning
include/rtl2832_regs.h  GENERATED register tables — do not hand-edit
tools/gen_regs.py     regenerates the above from the Java source
third_party/librtlsdr submodule; compiled into the binary, not linked
```

`librtlsdr` is **compiled from source into the binary** rather than linked.
This is required, not stylistic: `rtlsdr_demod_write_reg()` is the basis of
this entire driver, it is not declared in the public header, and librtlsdr
builds with `-fvisibility=hidden`. A Homebrew `librtlsdr` does not export it.

## Credit

This is a port, not original research. The hard work was done by others:

- **[AndroidDvbDriver](https://github.com/signalwareltd/AndroidDvbDriver)**
  (Signalware Ltd / Martin Marinov, GPLv2) — the userspace DVB-T driver this
  is transcribed from. It proved the whole approach: no kernel driver needed,
  it can all be done over plain USB from userspace.
- **[librtlsdr](https://github.com/rtlsdrblog/rtl-sdr-blog)** (GPLv2) — USB
  transport, tuner drivers, and the demodulator register primitives.
- The **Linux kernel V4L2 rtl2832 driver** (GPLv2), the common ancestor of
  both.

## License

GPLv2, inherited from both upstreams. See [COPYING](COPYING).
