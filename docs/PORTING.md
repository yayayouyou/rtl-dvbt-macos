# Porting notes

Findings from reading the two upstreams side by side, and the things most
likely to cost you a weekend. Written before any hardware test, so the
"suspects" list below is reasoning, not experience.

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
