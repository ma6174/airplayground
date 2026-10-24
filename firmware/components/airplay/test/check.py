#!/usr/bin/env python3
"""Validates the binary plists produced by the C encoder.

Every body the sender puts on the wire is decoded here with plistlib, so a
layout mistake in ap_plist.c or a wrong SETUP field name fails the test with
the decoder's own view of the bytes.
"""

import os
import plistlib
import sys

FAILURES = []


def check(condition, message):
    if condition:
        print(f"  ok   {message}")
    else:
        print(f"  FAIL {message}")
        FAILURES.append(message)


def load(directory, name):
    path = os.path.join(directory, name)
    with open(path, "rb") as handle:
        return plistlib.load(handle)


def check_encoded(directory):
    value = load(directory, "encoded.bplist")
    print("encoded.bplist:")
    check(value["string"] == "hello", 'string == "hello"')
    check(value["empty"] == "", "empty string survives")
    check(value["small"] == 42, "small int == 42")
    check(value["big"] == 1234567890123, "64-bit int survives")
    check(value["negative"] == -7, "negative int == -7")
    check(value["flag"] is True, "true is a bool")
    check(value["off"] is False, "false is a bool")
    check(abs(value["ratio"] - 1.5) < 1e-12, "real == 1.5")
    check(value["blob"] == bytes([0x00, 0x01, 0x02, 0xFF]), "data bytes survive")
    check(value["list"][0] == 1 and value["list"][1] == "two" and value["list"][2] is False,
          "array keeps order and types")
    check(value["list"][3]["nested"] == 3, "nested dict inside an array")


def check_control(directory):
    value = load(directory, "control.bplist")
    print("control.bplist:")
    check(value["deviceID"] == "02:11:22:33:44:55", "deviceID")
    check(value["macAddress"] == "02:11:22:33:44:55", "macAddress repeats deviceID")
    check(value["sessionUUID"] == "0F0E0D0C-0B0A-4908-8706-050403020100", "sessionUUID")
    check(value["sourceVersion"] == "980.71.1", "sourceVersion is the encrypted-session value")
    check(value["isScreenMirroringSession"] is True, "isScreenMirroringSession")
    check(value["timingProtocol"] == "NTP", "timingProtocol is NTP")
    check(value["timingPort"] == 50000, "timingPort")
    check(value["osBuildVersion"] == "13F69", "osBuildVersion")
    check(value["model"] == "Linux", "model")
    check(value["name"] == "ESP32-S3 AirPlay", "name")
    check(value["updateSessionRequest"] is False, "updateSessionRequest")
    check(value["combinedGetInfoWithControlSetup"] is True, "combinedGetInfoWithControlSetup")
    check(len(value) == 12, f"exactly 12 keys (got {len(value)})")


def check_control_utf8(directory):
    """A non-ASCII sender name has to arrive as UTF-16BE code units.

    Written as ASCII bytes the receiver decodes them as single-byte characters,
    which is how "小智 TV" used to show up as "å°æ° TV".
    """
    value = load(directory, "control_utf8.bplist")
    print("control_utf8.bplist:")
    check(value["name"] == "小智 TV", f'name round-trips (got {value["name"]!r})')


def check_control_ptp(directory):
    value = load(directory, "control_ptp.bplist")
    print("control_ptp.bplist:")
    check(value["timingProtocol"] == "PTP", "timingProtocol is PTP")
    check("timingPort" not in value, "PTP descriptor carries no timingPort")
    peer = value["timingPeerInfo"]
    check(peer["ID"] == "9A8B7C6D-5E4F-4A3B-8C2D-1E0F9A8B7C6D", "timing peer ID")
    check(peer["SupportsClockPortMatchingOverride"] is True, "SupportsClockPortMatchingOverride")
    check(peer["DeviceType"] == 0, "DeviceType 0")
    check(peer["Addresses"] == ["10.0.0.7"], "peer address list")
    check(value["timingPeerList"] == [peer], "timingPeerList repeats the peer")


def check_audio(directory):
    value = load(directory, "audio.bplist")
    stream = value["streams"][0]
    print("audio.bplist:")
    check(stream["type"] == 96, "stream type 96")
    check(stream["streamConnectionID"] == 1111111111111111111, "audio streamConnectionID")
    check(stream["ct"] == 2, "ct == 2 (ALAC placeholder)")
    check(stream["spf"] == 352, "spf == 352")
    check(stream["sr"] == 44100, "sr == 44100")
    check(stream["audioFormat"] == 0x40000, "audioFormat == 0x40000")
    check(stream["audioMode"] == "default", "audioMode")
    check(stream["usingScreen"] is True, "usingScreen")
    check(stream["latencyMin"] == 0, "latencyMin == 0")
    check(stream["latencyMax"] == 3748, "latencyMax == 3748")
    check(stream["redundantAudio"] == 2, "redundantAudio == 2")
    check(stream["controlPort"] == 50001, "controlPort")
    check(len(value["streams"]) == 1, "one stream descriptor")


def check_audio_connections(directory):
    value = load(directory, "audio_connections.bplist")
    print("audio_connections.bplist:")
    stream = value["streams"][0]
    check("controlPort" not in stream, "modern layout drops controlPort")
    check(stream["isMedia"] is False, "isMedia is false")
    check(stream["supportsDynamicStreamID"] is True, "supportsDynamicStreamID")
    check("shk" not in stream, "no audio key is published")
    connections = stream["streamConnections"]
    check(connections["streamConnectionTypeRTP"]["streamConnectionKeyUseStreamEncryptionKey"] is False,
          "RTP connection is unencrypted")
    check(connections["streamConnectionTypeRTCP"]["streamConnectionKeyPort"] == 50001,
          "RTCP connection carries the control port")
    check(stream["usingScreen"] is True, "still marked as screen audio")


def check_video(directory):
    value = load(directory, "video.bplist")
    stream = value["streams"][0]
    print("video.bplist:")
    check(stream["type"] == 110, "stream type 110")
    check(stream["streamConnectionID"] == 2222222222222222222, "video streamConnectionID")
    check(stream["latencyMs"] == 75, "latencyMs == 75")
    check([entry["name"] for entry in stream["timestampInfo"]] ==
          ["SubSu", "BePxT", "AfPxT", "BefEn", "EmEnc"], "timestampInfo order")
    check(stream["shk"] == bytes(range(16)), "shk is the 16-byte stream key")
    check(stream["shiv"] == bytes(range(16, 32)), "shiv is the 16-byte IV")
    check(len(stream) == 6, f"six stream keys (got {len(stream)})")


def main():
    directory = sys.argv[1] if len(sys.argv) > 1 else "out"
    check_encoded(directory)
    check_control(directory)
    check_control_utf8(directory)
    check_control_ptp(directory)
    check_audio(directory)
    check_audio_connections(directory)
    check_video(directory)
    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed")
        return 1
    print("\nall plist checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
