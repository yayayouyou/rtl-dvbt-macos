#!/usr/bin/env python3
"""
Transcribe the RTL2832 DVB-T register tables from AndroidDvbDriver (Java) into C.

Why a generator instead of hand-copying: there are 127 bit-field descriptors and
three register-init tables. Hand-transcription is exactly the kind of work that
silently introduces a one-hex-digit bug you then spend a weekend chasing with no
signal lock and no idea why. This also keeps provenance explicit and lets us
re-sync if upstream changes.

Source:  https://github.com/signalwareltd/AndroidDvbDriver  (GPLv2)
         drivers/src/main/java/info/martinmarinov/drivers/usb/rtl28xx/
         Rtl2832FrontendData.java

Usage:   ./tools/gen_regs.py <path-to-Rtl2832FrontendData.java> > include/rtl2832_regs.h
"""

import re
import sys

ENUM_ENTRY = re.compile(
    r"^\s+(DVBT_[A-Z0-9_]+)\(\s*(0x[0-9a-fA-F]+|\d+)\s*,"
    r"\s*(0x[0-9a-fA-F]+|\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)"
)
REGVALUE = re.compile(
    r"new\s+RegValue\(\s*DvbtRegBitName\.(DVBT_[A-Z0-9_]+)\s*,"
    r"\s*(0x[0-9a-fA-F]+|\d+)\s*\)"
)
TABLE_START = re.compile(r"static final RegValue\[\]\s+([A-Z0-9_]+)\s*=")
BW_BYTE = re.compile(r"\(byte\)\s*(0x[0-9a-fA-F]+)")


def parse(src: str):
    """Return (enum_entries, regvalue_tables, bw_params)."""
    lines = src.splitlines()

    # --- bit-field descriptors -------------------------------------------
    enum_entries = []
    in_enum = False
    for ln in lines:
        if "enum DvbtRegBitName" in ln:
            in_enum = True
            continue
        if in_enum:
            m = ENUM_ENTRY.match(ln)
            if m:
                name, page, addr, msb, lsb = m.groups()
                enum_entries.append(
                    (name, int(page, 0), int(addr, 0), int(msb), int(lsb))
                )
            elif re.match(r"^\s+DvbtRegBitName\(", ln):
                in_enum = False  # reached the constructor

    # --- RegValue[] init tables ------------------------------------------
    tables, current = {}, None
    for ln in lines:
        m = TABLE_START.search(ln)
        if m:
            current = m.group(1)
            tables[current] = []
            continue
        if current is not None:
            for rm in REGVALUE.finditer(ln):
                tables[current].append((rm.group(1), int(rm.group(2), 0)))
            if "};" in ln:
                current = None

    # --- BW_PARAMS --------------------------------------------------------
    bw_params, current_bw = [], None
    in_bw = False
    for ln in lines:
        if "BW_PARAMS" in ln:
            in_bw = True
            continue
        if in_bw:
            if "MHz bandwidth" in ln:
                if current_bw:
                    bw_params.append(current_bw)
                current_bw = []
                continue
            if current_bw is not None:
                current_bw.extend(int(b, 16) for b in BW_BYTE.findall(ln))
            if re.match(r"^\s+};", ln):
                if current_bw:
                    bw_params.append(current_bw)
                in_bw = False

    return enum_entries, tables, bw_params


def emit(enum_entries, tables, bw_params) -> str:
    out = []
    w = out.append

    w("/*")
    w(" * rtl2832_regs.h - RTL2832 DVB-T register tables")
    w(" *")
    w(" * GENERATED FILE - do not edit by hand.")
    w(" * Regenerate with: ./tools/gen_regs.py <Rtl2832FrontendData.java>")
    w(" *")
    w(" * Transcribed from AndroidDvbDriver (GPLv2):")
    w(" *   https://github.com/signalwareltd/AndroidDvbDriver")
    w(" * which is itself a userspace port of the Linux kernel V4L2 rtl2832")
    w(" * driver (drivers/media/dvb-frontends/rtl2832.c, GPLv2).")
    w(" *")
    w(" * This file is licensed GPLv2, inherited from both sources.")
    w(" */")
    w("")
    w("#ifndef RTL2832_REGS_H")
    w("#define RTL2832_REGS_H")
    w("")
    w("#include <stdint.h>")
    w("")
    w("/* A bit-field within a demodulator register page. */")
    w("typedef struct {")
    w("\tuint8_t  page;")
    w("\tuint16_t addr;")
    w("\tuint8_t  msb;")
    w("\tuint8_t  lsb;")
    w("} rtl2832_reg_t;")
    w("")

    # enum ids
    w("typedef enum {")
    for i, (name, *_rest) in enumerate(enum_entries):
        w(f"\t{name} = {i},")
    w(f"\tRTL2832_REG_COUNT = {len(enum_entries)}")
    w("} rtl2832_reg_id;")
    w("")

    # descriptor table
    w("static const rtl2832_reg_t rtl2832_regs[RTL2832_REG_COUNT] = {")
    for name, page, addr, msb, lsb in enum_entries:
        w(f"\t[{name}] = {{ 0x{page:02x}, 0x{addr:03x}, {msb:2d}, {lsb:2d} }},")
    w("};")
    w("")

    w("typedef struct {")
    w("\trtl2832_reg_id reg;")
    w("\tuint32_t       val;")
    w("} rtl2832_regval_t;")
    w("")

    # init tables
    for tname, entries in tables.items():
        cname = "rtl2832_" + tname.lower()
        w(f"static const rtl2832_regval_t {cname}[] = {{")
        for name, val in entries:
            w(f"\t{{ {name}, 0x{val:x} }},")
        w("};")
        w(f"#define {cname.upper()}_LEN "
          f"(sizeof({cname}) / sizeof({cname}[0]))")
        w("")

    # bandwidth params
    if bw_params:
        n, width = len(bw_params), len(bw_params[0])
        w("/* Coefficients for the resampler, indexed by bandwidth:")
        w(" *   [0] = 6 MHz  (Taiwan, Americas)")
        w(" *   [1] = 7 MHz")
        w(" *   [2] = 8 MHz  (most of Europe)")
        w(" */")
        w(f"static const uint8_t rtl2832_bw_params[{n}][{width}] = {{")
        for i, params in enumerate(bw_params):
            mhz = 6 + i
            w(f"\t{{ /* {mhz} MHz */")
            for off in range(0, len(params), 10):
                chunk = ", ".join(f"0x{b:02x}" for b in params[off:off + 10])
                w(f"\t\t{chunk},")
            w("\t},")
        w("};")
        w("")

    w("#endif /* RTL2832_REGS_H */")
    return "\n".join(out) + "\n"


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2

    with open(sys.argv[1], encoding="utf-8") as f:
        src = f.read()

    enum_entries, tables, bw_params = parse(src)

    if not enum_entries:
        print("error: no register descriptors found - has the Java "
              "source layout changed?", file=sys.stderr)
        return 1

    print(f"parsed {len(enum_entries)} register descriptors, "
          f"{len(tables)} init tables, {len(bw_params)} bandwidth tables",
          file=sys.stderr)
    for tname, entries in tables.items():
        print(f"  {tname}: {len(entries)} entries", file=sys.stderr)

    sys.stdout.write(emit(enum_entries, tables, bw_params))
    return 0


if __name__ == "__main__":
    sys.exit(main())
