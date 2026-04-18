#!/usr/bin/env python3
"""Read ESP32 serial into a log file with timestamps. Auto-reconnects."""
import os
import sys
import time

sys.path.insert(0, "/opt/homebrew/Cellar/platformio/6.1.18_3/libexec/lib/python3.13/site-packages")
import serial  # noqa: E402

PORT = "/dev/cu.usbserial-0001"
BAUD = 115200
LOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "logs", "serial.log")
os.makedirs(os.path.dirname(LOG), exist_ok=True)

def main():
    with open(LOG, "a", buffering=1) as f:
        f.write(f"\n=== serial_tail started {time.strftime('%H:%M:%S')} ===\n")
        while True:
            try:
                s = serial.Serial(PORT, BAUD, timeout=1)
                # Don't toggle DTR/RTS — leave the board running.
                f.write(f"=== opened {PORT} {time.strftime('%H:%M:%S')} ===\n")
                while True:
                    line = s.readline()
                    if not line:
                        continue
                    try:
                        txt = line.decode("utf-8", errors="replace").rstrip()
                    except Exception:
                        txt = repr(line)
                    f.write(f"{time.strftime('%H:%M:%S')} {txt}\n")
            except (serial.SerialException, OSError) as e:
                f.write(f"=== {time.strftime('%H:%M:%S')} reconnect: {e} ===\n")
                time.sleep(1)

if __name__ == "__main__":
    main()
