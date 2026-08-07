# Porting notes

Findings from reading the two upstreams side by side, plus what the first
hardware session actually taught us.

## First hardware session — what happened

Dongle: `0bda:2838` RTL2838UHIDIR, R820T tuner, indoor antenna, Taiwan.

Spectrum scan found real DVB-T at 533, 545, 557, 569 and 581 MHz — every
other 6 MHz channel, flat across the channel, 7–9 dB above noise. Weak but
present.

**Result: the demodulator now locks (FSM stage 11) and the endpoint delivers
~1.4 MB/s. It did not before — it delivered nothing at all.** The bytes are
not yet valid transport-stream packets, so this is not finished, but the
demodulator genuinely acquires Taiwan's DVB-T signal from macOS.

Four defects were found and fixed. In order of how much they mattered:

### 1. Tuner call order — the one that actually mattered

`rtl2832_dvbt_set_params()` called `rtlsdr_set_tuner_bandwidth()` before
`rtlsdr_set_center_freq()`. That is fatal, for a reason that is invisible
unless you read librtlsdr's tuner shims:

```c
/* librtlsdr.c:253, r820t_set_bw() */
return rtlsdr_set_center_freq(devt, devt->freq);
```

Upstream osmocom guards this with `if (devt->freq > 0)`. This fork does not.
Straight out of `rtlsdr_open()`, `dev->freq` is still 0 — the struct is
memset and the only assignments to it live inside `rtlsdr_set_center_freq`
itself. So the bandwidth call re-tunes to **0 Hz**, which trips librtlsdr's
automatic direct-sampling switch (`freq < 24 MHz` on an R820T), and
`_rtlsdr_set_direct_sampling()` calls `r82xx_standby()` on the way in.

The tuner goes to sleep, and the excursion rewrites demod registers back to
their SDR values — dismantling the DVB-T configuration set up moments
earlier. Every register write still returns success.

Tune first, then set bandwidth. As a bonus this also leaves the I2C repeater
closed: `set_center_freq` opens it and never closes it (librtlsdr.c:913 is
commented out) whereas `set_tuner_bandwidth` does.

**The misleading part:** this prints `Enabled direct sampling mode, input 2`
and `[R82XX] PLL not locked!`. Stock `rtl_sdr` prints those too, at the same
frequency, and works fine — so they read as harmless startup noise. They are
harmless *in SDR mode*. Here they are the sound of your configuration being
undone.

### 2. AGC loops left disabled

librtlsdr's `rtlsdr_init_baseband()` does:

```c
rtlsdr_demod_write_reg(dev, 1, 0x04, 0x00, 1);  /* disable RF and IF AGC loop */
```

Page 1 register 0x04 packs `EN_IF_AGC` (bit 7), `EN_RF_AGC` (bit 6),
`AAGC_HOLD` (bit 5) and `LOOP_GAIN2_3_0` (bits 4:1). The register tables do
write `LOOP_GAIN2_3_0`, but as a read-modify-write on the low bits — the two
enable bits stay zero.

The Java driver has no equivalent code because it meets a freshly powered
chip where these default to on. **Any state librtlsdr turns off for SDR that
the Java assumes is at power-on default is a bug waiting in this port.** That
class of defect is worth sweeping for deliberately.

### 3. The status register was invented

`rtl2832_dvbt_get_status()` originally read page 1 register 0x3c and tested
bit 3. Page 1 0x3c has no named field at all — the address was made up.

The real thing is `DVBT_FSM_STAGE` at **page 3, 0x51, bits 6:3**, and it is a
*stage number*, not a bitmask: 11 is full lock, 10 is signal/carrier/Viterbi
without sync, lower means still hunting.

This mattered more than a cosmetic bug: with the wrong register everything
reported "status 0x00, nothing", so there was no way to tell a demodulator
that was almost locking from one that was not running at all. The moment it
read the real register, the failure went from opaque to obvious — the chip
was reaching stage 9 the whole time.

**Read the diagnostic registers correctly before trusting any conclusion
drawn from them.**

### 4. librtlsdr's tuner helpers write demod registers

Unlike the Java's bare `R820tTuner`, librtlsdr's tuner wrappers touch demod
registers as a side effect. The DVB-T datapath therefore has to be
re-asserted *after* the tuner calls, not just once during init.

## SOLVED: the blocker was tuner gain

**Root cause: librtlsdr's automatic gain mode.** It is optimised for SDR - it
maximises signal level into the ADC. On a strong transmitter that drives the
R820T front end into compression. The demodulator then locks completely
convincingly: FSM stage 11, TPS decodes to 64QAM/hierarchy 0, output arrives
at the correct transport-stream rate. And every packet is uncorrectable.

Same channel (TTV, 581 MHz), seconds apart:

| gain | CNR | EVM | BER | result |
|---|---|---|---|---|
| librtlsdr automatic | 16.1 dB | 4084 | 65535/1e6 | uniform noise, 2% sync |
| 14.7 dB fixed | - | 0 | 39/1e6 | 100% sync, valid TS |
| 22.9 dB (swept) | 24.4 dB | 606 | **0/1e6** | 100% sync, H.264 1920x1080 |

Fixed by `auto_gain()` in rtl2832_dvbt.c: sweep the tuner's gain table, keep
the value with the lowest post-Viterbi BER, stop early once it is under 100.

**The lesson that would have saved hours:** "the demodulator reports lock" and
"the demodulator is producing usable data" are different claims, and the gap
between them is almost always gain. Read BER before believing a lock. We spent
a long time hunting a routing bug that did not exist because stage 11 looked
so much like success.

The corollary is about diagnostics. Three of the four things that actually
moved this forward were measurements, not fixes: reading the real FSM stage
register, porting `readBer()`, and porting the kernel's full `read_status()`
(constellation, hierarchy, CNR). Each one turned an opaque failure into an
obvious one. Port the instruments before the features.


## The proper fix: give the VGA back to the AGC loop

The gain sweep above works, but it is a workaround — it hunts for a fixed
operating point that happens to decode. Both stacks that are known to work on
this hardware do something better, and a later comparison against the kernel
found it.

**R820T register 0x0c bit 4 selects where the VGA takes its gain from:**
1 means follow the VAGC pin, which the RTL2832's IF-AGC loop drives; 0 means
"vga code mode", a fixed gain from bits 3:0.

The kernel leaves it at 1 throughout DVB-T operation: the init array has
`0x0c = 0xf5` (r820t.c:120) and `r820t_set_tv_standard()` writes only the low
nibble (r820t.c:1067, mask `0x0f`). The only kernel writes that clear bit 4
are in `r820t_set_gain_mode()` — the explicit manual-gain path — and
`r820t_standby()`. Neither is on the tuning path. AndroidDvbDriver agrees.

librtlsdr clears it on **every** tune. `r82xx_set_freq()` calls
`r82xx_set_vga_gain()` (tuner_r82xx.c:1181), which unconditionally writes
`0x0c` mask `0x9f` value `0x08` — 16.3 dB fixed, bit 4 = 0. That is right for
SDR, where the demodulator's AGC loops are off and the host wants a known
stable gain. It is wrong for DVB-T, where that loop is the only gain control
in the system: `restore_dvbt_defaults()` switches the IF-AGC loop on, and then
nothing is listening to its output.

`r820t_reconnect_agc_pin()` writes `0x0c = 0xf0` over I2C as the last tuner
write of the tune. It must be last — the write bypasses librtlsdr's register
shadow, so any later masked write would recompute from a stale cache.

**How to tell it worked:** read `DVBT_IF_AGC_VAL` (page3 0x59, 14-bit two's
complement, in `RTL_DVBT_DUMP=1`). Pinned at a rail across repeated reads
means the loop has no actuator. Settling mid-scale means it is tracking.
Before: rail. After: 2183.

Measured over five local channels, loop versus sweep:

| channel | AGC loop | gain sweep |
|---|---|---|
| 581 (TTV) | **100% sync, BER 0** | 100% sync, BER 57 |
| 557 | 96%, BER 109 | 100%, BER 93 |
| 569 | 97%, BER 19616 | 97%, BER 19616 |
| 545 | 59%, BER 19616 | 66%, BER 19616 |
| 533 | 75%, BER 19616 | 69%, BER 19616 |

The loop wins outright on the strongest channel and ties elsewhere, while
costing no sweep time and adapting to conditions changing under it. It is now
the default; `RTL_DVBT_AUTOGAIN=sweep` forces the old behaviour, and the
driver falls back to sweeping on its own if the loop never locks.

Note the three weak channels are unusable under both strategies — CNR around
21 dB, right at the 64-QAM threshold. That is a signal limit, not a driver
one, and sweeping does not rescue it. Which is why the fallback triggers on
"never locked" rather than "high BER": spending two seconds to arrive at the
same bad answer helps nobody.

### Two tuner values also corrected

While comparing against the kernel's `r820t_set_tv_standard()`, librtlsdr's
`r82xx_set_tv_standard()` turned out to differ on two DVB-T parameters:

| | kernel | librtlsdr |
|---|---|---|
| `filt_gain` | 0x10 | 0x30 |
| `loop_through` | 0x00 (lt on) | 0x80 (lt off) |

Changed back to the kernel's values in the vendored copy. Neither was the
blocker on its own - BER stayed at 65535 after the change - but they are the
kernel's tested settings for this delivery system.

Also worth knowing: librtlsdr calls `r82xx_set_tv_standard()` exactly once,
from `r82xx_init()`, with a hardcoded `bw=3`. The kernel calls it on **every**
tune with the real bandwidth. That has not mattered for 6 MHz, but it will
matter if 7 and 8 MHz ever need to work.

## Historical: what was ruled out before the gain finding



The demodulator locks and the endpoint delivers data at a plausible TS rate,
but the bytes are uniformly distributed with no 188-byte structure.

At this point the conclusion drawn was **"signal quality, not configuration"**
— `readBer()` was returning 65535 on every channel, the local signal measured
only 7–9 dB above noise on a stock whip indoors, and 64-QAM DVB-T wants around
20 dB. The recommendation was to get a better antenna.

**That conclusion was wrong**, and the correction is worth recording. The user
pointed out they had already received this exact channel with this exact
dongle and antenna, in this location, using the Linux kernel driver in a VM.
That single fact killed the hypothesis outright: the signal was fine, so the
7–9 dB figure was measuring something other than what mattered, and the
saturated BER had to be our own doing.

It was: the gain was wrong (see above). The lesson is that a plausible
physical explanation is not evidence, and one report of the same hardware
working elsewhere outweighs a lot of internally consistent reasoning.

Ruled out along the way, so nobody redoes them:

- register configuration — full readback before capture shows every DVB-T
  field at its intended value (`RTL_DVBT_DUMP=1`)
- capture timing — capturing the instant stage 11 is observed changes nothing
- `DVBT_SERIAL` 0/1, IF 0–5 MHz, `SPEC_INV` 0/1, tuner gain mode auto/manual
- bit rotations, bit inversion, bit-order reversal, 204-byte packets — no
  transform reveals TS structure
- USB endpoint choice — the Java uses interface 0, endpoint 0x81, same as us

One genuinely interesting observation: with `MPEG_IO_OPT_1_0 = 2` the
endpoint runs at ~45 MB/s and the data repeats **exactly every 3760 bytes =
20 × 188**, which is precisely the `USB_EPA_FIFO_CFG` value of `0x14` (20)
this driver writes. So that register demonstrably takes effect and the FIFO
really is 20 TS packets deep — it is simply being replayed rather than flow
controlled, because the demodulator is not filling it. Useful confirmation
that the USB side is wired correctly.

## Recovering a wedged dongle

Writing bad values to the MPEG output-routing fields will hang the endpoint
so hard that even plain `rtl_sdr` stops producing data and blocks forever.
It is not bricked. `libusb_reset_device()` on `0bda:2838` clears it without
unplugging:

```c
libusb_device_handle *h = libusb_open_device_with_vid_pid(ctx, 0x0bda, 0x2838);
libusb_reset_device(h);
```

Worth doing between experiments that sweep output-path registers.

---

## The gap being filled

`librtlsdr` and `AndroidDvbDriver` are both descendants of the Linux kernel
V4L2 `rtl2832` driver. They diverged in purpose:

- `librtlsdr` **disables** the DVB-T demodulator so raw I/Q reaches the host.
- `AndroidDvbDriver` **configures** it so an MPEG-2 TS reaches the host.

Everything below the demodulator — USB transport, I2C to the tuner, the R820T
itself — is common to both. So the port is not "write a driver", it is "write
the demodulator configuration that librtlsdr deliberately omits, and reuse
everything else".

## Register access maps 1:1

Java `wr(addr, page, val, len)` and librtlsdr `rtlsdr_demod_write_reg(dev,
page, addr, val, len)` are the same operation. The Java goes through an I2C
adapter with an explicit page-select write; librtlsdr encodes the page in the
USB control transfer index (`0x10 | page`) and the address as `(addr << 8) |
0x20`, where `0x20` is the demodulator's I2C address `0x10` shifted left one
bit. Same bytes on the wire.

This is why the port is mostly transcription.

## Known hazards

### 1. librtlsdr's 16-bit demod read is byte-swapped

`rtlsdr_demod_write_reg()` writes big-endian:

```c
data[0] = val >> 8;      /* MSB first */
data[1] = val & 0xff;
```

`rtlsdr_demod_read_reg()` reassembles little-endian:

```c
reg = (data[1] << 8) | data[0];   /* LSB first */
```

These disagree. Nothing in librtlsdr notices, because there is exactly one
call site and it uses `len = 1` and discards the result. We read 16-bit fields
routinely, so `demod_rd()` in `rtl2832_dvbt.c` swaps to compensate rather than
patching the submodule.

**Symptom if this is ever wrong:** a status or BER value that looks like a
plausible number with its bytes reversed.

### 2. Multi-byte writes are split — UNVERIFIED

The Java issues multi-byte register writes as single I2C transactions.
`unsetSdrMode()` alone needs a 3-byte and a 4-byte write. But
`rtlsdr_demod_write_reg()` tops out at two bytes — its buffer is
`unsigned char data[2]`, so a longer length would read past the end of the
stack array.

`demod_wr_bytes()` therefore issues one single-byte transaction per byte and
relies on the demodulator's register address auto-increment.

**This assumption has not been tested on hardware.** It should hold for plain
configuration registers, but any register that latches its effect on the final
byte of a burst would behave differently when split.

**If Phase 1 writes cleanly but never locks, suspect this first.** The fix is
a small accessor added to the submodule that does a real multi-byte control
transfer.

### 3. The tuner IF is assumed, not read back

Good news first: `librtlsdr`'s `r82xx_set_bandwidth()` **already contains the
DVB-T IF values** inherited from the kernel driver — 3.57 MHz for a 6 MHz
channel, 4.57 MHz for 7 and 8 MHz. `struct r82xx_priv` even still carries a
`delsys` field. The DVB-T paths survived the port to SDR-land; they are just
never exercised.

So the biggest risk I expected going in — "the R820T is set up for zero-IF SDR
and DVB-T needs a real IF" — is mostly already solved upstream.

What is *not* solved: we call `rtlsdr_set_tuner_bandwidth()` and then
**hardcode** the IF we expect it chose, because `priv->int_freq` sits behind
librtlsdr's opaque `rtlsdr_dev_t` and there is no accessor. That works as long
as our table matches theirs. It is fine for 6 MHz; verify before trusting 7 and
8 MHz.

The clean fix is a two-line accessor patch to the submodule.

### 4. Timing

The Java driver sleeps at points during initialisation. None of those delays
are reproduced here. If initialisation is flaky rather than consistently
broken, compare against `Rtl2832Frontend.java` and add them back.

## Debugging order when it doesn't lock

Work down this list — each step tells you which half of the problem to discard.

1. **Is there signal at all?** `rtl_power -f 470M:600M:1M -g 40 -i 10 scan.csv`.
   No energy at the channel means antenna or location, not code.
2. **Are bytes flowing?** If `rtl_dvbt` writes 0 bytes, the bulk endpoint is
   not producing — the mode switch failed outright.
3. **Are bytes flowing but garbage?** The mode switch partly worked. Check
   hazard 2 (split writes).
4. **Does status ever report lock?** Read `rtl2832_dvbt_get_status()` in a
   loop while sweeping frequency slightly. Lock that comes and goes points at
   IF or resampler configuration (hazard 3).
5. **Locks but ffprobe rejects the file?** Then the demodulator is fine and
   the problem is downstream — PID filtering or TS framing.

A correct capture is unambiguous: `ffprobe` identifies MPEG-2 TS and lists
PAT/PMT. There is no partial credit and no subjective judgement, which makes
this a pleasant thing to debug once bytes are moving.

## Regenerating the register tables

`include/rtl2832_regs.h` is generated. Do not hand-edit it.

```bash
git clone https://github.com/signalwareltd/AndroidDvbDriver /tmp/dvbdrv
./tools/gen_regs.py \
  /tmp/dvbdrv/drivers/src/main/java/info/martinmarinov/drivers/usb/rtl28xx/Rtl2832FrontendData.java \
  > include/rtl2832_regs.h
```

Expected output: 127 register descriptors, 4 init tables, 3 bandwidth tables.
If those counts change, upstream changed and the diff is worth reading.
