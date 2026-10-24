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
| Valid MPEG-2 TS out | yes — 99.93% packet sync over a 3-minute continuous capture |
| Plays in ffplay / VLC / mpv | yes |
| Tuners | **R820T** verified. **R828D** accepted but untested. Others refused unless forced. |
| Bandwidths | 6 / 7 / 8 MHz implemented — **only 6 MHz verified on air** |
| Channel scanning | not yet — you must know the frequency |
| DVB-T2 | no |

### What has actually been tested, and what has not

Being specific about this so nobody wastes an evening:

**Tested:** one dongle (`0bda:2838`, R820T tuner) on one Apple Silicon Mac,
against Taiwan's terrestrial DVB-T — 6 MHz channels, 64-QAM, hierarchy 0.
Three minutes of continuous capture holds lock with 99.93% packet sync and no
phase drift. A clean recursive clone was built and run to confirm what you get
is what was tested.

**Not tested at all:**

- **7 and 8 MHz bandwidths.** Most of Europe is 8 MHz. The code paths exist
  and come from the same kernel tables as the working 6 MHz one, but no 8 MHz
  signal has ever been put through this. If you are in Europe, you are the
  first — please say how it goes.
- **The R828D tuner**, which is what most sticks sold as DVB-T receivers
  actually contain. It shares librtlsdr's R820T driver and demodulator init
  path, so there is reason to expect it works, but that is inference, not a
  measurement.
- Every other tuner (E4000, FC0012, FC0013). Register tables are generated for
  some of them but nothing is wired up; the driver refuses to run unless you
  set `RTL_DVBT_FORCE_TUNER=1`.
- QPSK and 16-QAM constellations, hierarchical modes, and any guard interval
  other than whatever Taiwan transmits.
- Intel Macs. Nothing here should be Apple-Silicon-specific, but it has not
  been run on one.
- Long captures beyond three minutes, and recovery from the device being
  unplugged mid-capture (it is detected and reported, but the path is only
  reasoned about, not exercised).

**Known limit:** channels arriving below roughly 22 dB CNR do not decode
usefully here, whichever gain strategy is used. On the author's antenna that
is four of the five local multiplexes. That looks like a signal limit rather
than a driver one, but it has not been proven.

## Build

```bash
git clone --recursive https://github.com/yayayouyou/rtl-dvbt-macos
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

### What you actually get

An MPEG-2 transport stream, exactly as broadcast. That is the point, but it
means two properties of the broadcast land in your lap rather than being
smoothed over here, and both surprised the author while building a viewer on
top of this:

- **The video is usually interlaced.** 1080i is normal for DVB-T — 59.94
  fields woven into 29.97 frames. `ffplay` and VLC deinterlace on request;
  **browsers do not deinterlace at all**, so anything you feed a `<video>`
  element will comb visibly on motion. If you are building something
  web-facing, plan to deinterlace (`-vf yadif=mode=1`); on Apple Silicon
  `h264_videotoolbox` makes that affordable in real time.
- **The audio is often AAC in LATM/LOAS framing.** ffmpeg, ffplay, VLC and mpv
  all decode it. **HLS does not accept it** — it wants ADTS. Remuxing with
  `-c copy` into HLS produces a stream browsers accept and then play in total
  silence, with no error anywhere. `-c:a aac` fixes it.

Neither is a driver bug and neither is worth "fixing" here: this tool's job is
to hand you the broadcast unaltered. But knowing beforehand saves an evening.

Check what your own transmitter sends with `ffprobe`:

```bash
ffprobe -show_entries stream=codec_name,field_order,avg_frame_rate out.ts
```

Don't know which frequencies carry signal? Scan the UHF band first with
librtlsdr's own tool:

```bash
rtl_power -f 470M:700M:250k -g 40 -i 3 -e 30 scan.csv
```

Channels appear as flat plateaus 6 MHz wide, on 6 MHz centres.

### Gain

By default the driver hands the tuner's VGA back to the demodulator's own AGC
loop — the same thing the Linux kernel does for DVB-T — and lets the hardware
track the signal. No configuration needed.

This is not a nicety, and it is the single thing that took longest to find.
librtlsdr is an SDR library: it deliberately disables the demodulator's AGC
loops and pins the tuner at a fixed gain, because an SDR host wants a known
stable gain. For DVB-T that loop is the *only* gain control in the system, and
with it disconnected the front end sits wherever librtlsdr left it. On a
strong transmitter that is deep in compression — at which point the
demodulator still locks, still reports valid TPS, still delivers bytes at the
right rate, and Reed-Solomon cannot correct a single one of them:

| gain source | CNR | BER | usable? |
|---|---|---|---|
| librtlsdr's fixed gain | 16.1 dB | 65535/1e6 | no — indistinguishable from noise |
| demodulator AGC loop | 24.0 dB | 0/1e6 | yes |

Two escape hatches if that does not suit your hardware:

- `RTL_DVBT_GAIN=<tenths of a dB>` pins a manual gain.
- `RTL_DVBT_AUTOGAIN=sweep` measures every gain in the tuner's table and keeps
  the one with the lowest bit error rate. Slower, but it is also what the
  driver falls back to on its own if the AGC loop never locks.

### Diagnostics

`RTL_DVBT_DUMP=1` prints every relevant demodulator register before capture.

The per-tune status line is the kernel driver's own view of the signal:
`stage` 11 means full lock, `TPS=valid` means the transmission parameters
decoded (you cannot get that from noise), `BER` is post-Viterbi errors per
million bits. If `BER` is in the tens of thousands, suspect gain before
anything else.

That line is printed once, at lock. `-s <seconds>` keeps printing it while
capturing — reception changes under a running capture, and a reading from an
hour ago says nothing about now:

```bash
./build/rtl_dvbt -f 593M -s 2 | ffplay -
```

Each report's `BER` then covers the interval since the previous one. The
reads are the same status registers the Linux driver polls during streaming,
on the control endpoint, so they do not touch the stream: 25 seconds with a
report every second had no more transport errors than 25 seconds without
(none, on that channel).

A level that looks fine with a stubbornly high `BER` is worth recognising.
One multiplex here sat at a steady 23 dB CNR — comfortably above what 64-QAM
needs — with 5,700–8,300 errors per million on every report and 1–5% of
packets beyond repair, while a neighbouring multiplex on the same antenna
read 25 dB and zero. That is interference or reflections on that frequency,
not a weak signal, and no gain setting fixes it: the driver's sweep tried
and did worse.

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
The driver depends on several librtlsdr functions that are deliberately absent
from `rtl-sdr.h` — `rtlsdr_demod_write_reg()` is the basis of the whole thing.
That is internal API with no stability guarantee, and whether a prebuilt
`librtlsdr` even exports it depends on how it was built (its autotools build
hides internal symbols on any compiler; its CMake build only under GCC).
Vendoring pins both the symbols and the exact demodulator behaviour this was
verified against.

The submodule is used **unmodified**. An earlier version of this project
patched two tuner constants back to the kernel's DVB-T values; once the AGC
loop was reconnected properly they turned out to make no measurable
difference, so the patch was dropped rather than carried. A clean recursive
clone is exactly what was tested.

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
