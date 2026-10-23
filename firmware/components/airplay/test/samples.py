#!/usr/bin/env python3
"""Writes binary plists for the C decoder tests.

Apple receivers answer SETUP and GET /info with "bplist00" bodies. Generating
the samples with plistlib keeps the C decoder honest: the bytes come from a
different implementation than the one under test.
"""

import os
import plistlib
import sys


def write(directory, name, value):
    path = os.path.join(directory, name)
    with open(path, "wb") as handle:
        handle.write(plistlib.dumps(value, fmt=plistlib.FMT_BINARY))
    print(f"wrote {path} ({os.path.getsize(path)} bytes)")


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "out"
    os.makedirs(directory, exist_ok=True)

    write(
        directory,
        "sample_types.bplist",
        {
            "string": "hello",
            "empty": "",
            "small": 42,
            "large": 1234567890123,
            "negative": -7,
            "flag": True,
            "off": False,
            "ratio": 1.5,
            "blob": bytes([0x00, 0x01, 0x02, 0xFF]),
            "list": [1, "two", False, {"nested": 3}],
        },
    )

    # The values a real receiver reports for a 1080p display that wants a PIN.
    write(
        directory,
        "sample_info.bplist",
        {
            "features": 0x5A7FFFF7,
            "statusFlags": 0x200,
            "name": "Living Room",
            "displays": [
                {"widthPixels": 1920, "heightPixels": 1080},
                {"widthPixels": 3840, "heightPixels": 2160},
            ],
        },
    )

    # Control SETUP reply: event port, skipRecord and both stream ports.
    write(
        directory,
        "sample_setup.bplist",
        {
            "eventPort": 7001,
            "skipRecord": True,
            "streams": [
                {"type": 110, "dataPort": 7100},
                {"type": 96, "dataPort": 7200, "controlPort": 7300},
            ],
        },
    )


if __name__ == "__main__":
    main()
