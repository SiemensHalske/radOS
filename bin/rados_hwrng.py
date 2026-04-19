#!/usr/bin/env python3
"""
radOS TRNG → Linux kernel entropy bridge

Reads random bytes from the radOS Geiger counter TRNG over serial
and feeds them into the Linux kernel entropy pool via /dev/random.

Usage:
    sudo python3 rados_hwrng.py /dev/ttyCH343USB0
    sudo python3 rados_hwrng.py /dev/ttyACM0

Requires: pyserial (pip install pyserial)
"""

import sys
import time
import struct
import fcntl
import serial

RNDADDENTROPY = 0x40085203  # ioctl to add entropy to kernel pool
BLOCK_SIZE = 32             # Request 32 bytes at a time
ENTROPY_BITS = 256          # 32 bytes = 256 bits of entropy per block
INTERVAL = 2.0              # Seconds between requests (match TRNG output rate)


def add_entropy(data, entropy_bits):
    """Write entropy to /dev/random via ioctl."""
    # struct rand_pool_info { int entropy_count; int buf_size; __u32 buf[]; }
    buf = struct.pack('ii', entropy_bits, len(data)) + data
    with open('/dev/random', 'wb') as f:
        fcntl.ioctl(f.fileno(), RNDADDENTROPY, buf)


def main():
    if len(sys.argv) < 2:
        print(f"Usage: sudo {sys.argv[0]} <serial_port>")
        sys.exit(1)

    port = sys.argv[1]
    print(f"[radOS-hwrng] Connecting to {port}...")

    ser = serial.Serial(port, 115200, timeout=5)
    time.sleep(2)  # Wait for ESP32 boot

    # Drain any boot messages
    ser.reset_input_buffer()

    total = 0
    print(f"[radOS-hwrng] Feeding entropy to /dev/random")

    try:
        while True:
            # Request raw binary bytes
            cmd = f"RANDBIN {BLOCK_SIZE}\n"
            ser.write(cmd.encode())
            data = ser.read(BLOCK_SIZE)

            if len(data) == BLOCK_SIZE:
                add_entropy(data, ENTROPY_BITS)
                total += BLOCK_SIZE
                print(f"\r[radOS-hwrng] Fed {total} bytes to kernel pool", end='', flush=True)
            elif data:
                # Might be an error message, print it
                rest = ser.readline()
                print(f"\n[radOS-hwrng] Device: {data.decode(errors='replace')}{rest.decode(errors='replace').strip()}")

            time.sleep(INTERVAL)
    except KeyboardInterrupt:
        print(f"\n[radOS-hwrng] Stopped. Total: {total} bytes fed to kernel.")
    finally:
        ser.close()


if __name__ == '__main__':
    main()
