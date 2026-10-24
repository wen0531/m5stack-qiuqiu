#!/usr/bin/env python3
"""Check the built firmware against the device partition table.

The simulator has no flash, so the only place the 16 MB / 4.94 MB app slot limit
can be enforced is the build output.  Run after `idf.py build`.
"""
import os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))   # sim/
FW = os.path.dirname(ROOT)                                            # project root
PARTS = os.path.join(FW, "partitions.csv")
BIN = os.path.join(FW, "build", "StopWatch-UserDemo.bin")

WARN_RATIO = 0.90

def parse_parts():
    parts = {}
    for line in open(PARTS):
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        fields = [f.strip() for f in line.split(",")]
        if len(fields) >= 5 and fields[1] == "app":
            name, size = fields[0], fields[4]
            try:
                parts[name] = int(size, 0)
            except ValueError:
                pass
    return parts

def main():
    if not os.path.exists(BIN):
        print(f"firmware not built: {BIN}")
        return 1
    app_size = os.path.getsize(BIN)
    parts = parse_parts()
    if not parts:
        print("no app partitions found in partitions.csv")
        return 1

    slot_name = "ota_0"
    slot = parts.get(slot_name)
    # ota_0 size may be omitted in the csv (auto-filled); fall back to the value
    # used by the current layout
    if not slot:
        slot = 0x4F0000

    ratio = app_size / slot
    print(f"app image : {app_size:>10,} bytes ({app_size/1024/1024:.2f} MB)")
    for name, size in sorted(parts.items()):
        occupied = app_size if name == slot_name else None
        if occupied:
            print(f"partition : {name:<8} {size/1024/1024:.2f} MB  "
                  f"used {occupied/1024/1024:.2f} MB ({occupied/size*100:.1f}%)")
        else:
            print(f"partition : {name:<8} {size/1024/1024:.2f} MB")
    print(f"slot {slot_name:<5}: {slot/1024/1024:.2f} MB, free {(slot-app_size)/1024:.0f} KB")

    if ratio > 1.0:
        print("ERROR: firmware does not fit into the OTA slot")
        return 1
    if ratio > WARN_RATIO:
        print(f"WARNING: firmware uses {ratio*100:.1f}% of the OTA slot (> {WARN_RATIO*100:.0f}%)")
        return 0
    print("OK: fits comfortably")
    return 0

if __name__ == "__main__":
    sys.exit(main())
