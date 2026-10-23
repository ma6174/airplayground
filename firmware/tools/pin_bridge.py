#!/usr/bin/env python3
"""Keeps the ESP32's console open: device logs stream to stdout, and bytes
written to a FIFO are forwarded to the device.

On boards whose console is a USB-Serial/JTAG port the host-write path is
read-only (bytes never reach the firmware's STDIN), so the AirPlay pairing
code has to be delivered over HTTP instead - see docs/runbook.md. The FIFO
here is for boards with a real UART console.
"""

import os
import sys
import time

import serial

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem1101"
FIFO = sys.argv[2] if len(sys.argv) > 2 else "/tmp/pin_fifo"

if not os.path.exists(FIFO):
    os.mkfifo(FIFO)
fifo_fd = os.open(FIFO, os.O_RDONLY | os.O_NONBLOCK)

link = serial.Serial(PORT, 115200, timeout=0.2)
sys.stdout.write("bridge ready\n")
sys.stdout.flush()

while True:
    data = link.read(4096)
    if data:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
    try:
        chunk = os.read(fifo_fd, 64)
    except BlockingIOError:
        chunk = b""
    if chunk:
        link.write(chunk)
        link.flush()
        sys.stdout.write("\n[sent to device: %r]\n" % chunk)
        sys.stdout.flush()
    time.sleep(0.01)
