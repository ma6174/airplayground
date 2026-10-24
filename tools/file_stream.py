#!/usr/bin/env python3
"""Stream a video file to the device, sound included.

The device is a thin sender: it encrypts and forwards whatever it is handed. So
the file is split on this side instead of on the chip:

  * video keeps its H.264 bitstream (no re-encode): the access units go out in
    the same APVN protocol capture-cli speaks, paced at the file's frame rate;
  * audio is transcoded to ALAC - the codec the mirror audio channel declares -
    and sent one frame per packet on the same connection.

    ./file_stream.py /path/lf.mp4 --tcp 192.168.1.137:8090

Then start a session on the device with audio enabled:

    curl 'http://<device>:8080/start?audio=1'
"""

import argparse
import itertools
import json
import urllib.request
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

MAGIC = b"APVN"
TYPE_CONFIG = 1
TYPE_FRAME = 2
TYPE_AUDIO = 3
VERSION = 1
# 352 samples per packet is what AirPlay 1 audio uses: 8 ms of sound, small
# enough to stay inside one WiFi datagram. The device declares the same number
# in SETUP, and schedules its RTP timestamps from it.
AUDIO_SPF = 352  # must match APP_AUDIO_SPF on the device
AUDIO_RATE = 44100
AUDIO_CHANNELS = 2


def log(message):
    print(message, file=sys.stderr, flush=True)


def probe(path):
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-show_streams", "-show_format", "-of", "json", path],
        check=True, stdout=subprocess.PIPE).stdout
    info = json.loads(out)
    video = next((s for s in info["streams"] if s["codec_type"] == "video"), None)
    audio = next((s for s in info["streams"] if s["codec_type"] == "audio"), None)
    if video is None:
        raise SystemExit("no video stream")
    if video["codec_name"] != "h264":
        raise SystemExit(f"video is {video['codec_name']}, the mirror channel takes H.264")
    rate = video.get("avg_frame_rate") or video.get("r_frame_rate") or "25/1"
    num, _, den = rate.partition("/")
    fps = float(num) / float(den or 1)
    return video, audio, fps


def first_keyframe(path):
    """Timestamp of the first keyframe: video can only start there, so audio has
    to start there too or the two drift apart by the length of the leading GOP."""
    out = subprocess.run(
        ["ffprobe", "-v", "error", "-select_streams", "v:0", "-show_entries",
         "packet=pts_time,flags", "-of", "csv=p=0", path],
        check=True, stdout=subprocess.PIPE).stdout.decode()
    for line in out.splitlines():
        fields = line.split(",")
        if len(fields) >= 2 and "K" in fields[1]:
            return float(fields[0])
    return 0.0


def session_running(host):
    try:
        with urllib.request.urlopen(f"http://{host}:8080/status", timeout=2) as reply:
            return "mirroring" in reply.read().decode("utf-8", "replace")
    except OSError:
        return False


# ------------------------------------------------------------------ PCM → ALAC frames

class BitWriter:
    """ALAC headers are 55 bits wide, so the PCM that follows starts mid-byte."""

    def __init__(self):
        self.data = bytearray()
        self.acc = 0
        self.pending = 0

    def write(self, value, bits):
        self.acc = (self.acc << bits) | (value & ((1 << bits) - 1))
        self.pending += bits
        while self.pending >= 8:
            self.pending -= 8
            self.data.append((self.acc >> self.pending) & 0xFF)
        self.acc &= (1 << self.pending) - 1

    def finish(self, aligned_to=1):
        if self.pending:
            self.data.append((self.acc << (8 - self.pending)) & 0xFF)
            self.pending = 0
        if aligned_to > 1:
            while len(self.data) % aligned_to:
                self.data.append(0)
        return bytes(self.data)


def alac_frame(pcm):
    """One ALAC frame in its uncompressed form: a stereo element tag, the sample
    count, then the samples interleaved (libavcodec/alac.c decode_element).

    The frame is padded with zeros to a 16-byte boundary: the device encrypts
    whole AES blocks, and a receiver that decrypts the entire UDP payload
    (rather than stopping at the last complete block of the original length)
    would otherwise garble the last few samples of every frame - a periodic
    crackle at 125 frame/s. Decoders stop after the declared sample count, so
    the padding is never heard."""
    bits = BitWriter()
    bits.write(1, 3)          # element: stereo pair
    bits.write(0, 4)          # element instance tag
    bits.write(0, 12)         # unused
    bits.write(1, 1)          # has_size: the sample count follows
    bits.write(0, 2)          # extra bits field
    bits.write(1, 1)          # is not compressed
    bits.write(AUDIO_SPF, 32)
    for sample in range(AUDIO_SPF):
        for channel in range(AUDIO_CHANNELS):
            at = 2 * (sample * AUDIO_CHANNELS + channel)
            value = int.from_bytes(pcm[at:at + 2], "little", signed=True)
            bits.write(value & 0xFFFF, 16)
    return bits.finish(aligned_to=16)


def read_exactly(stream, count):
    buffer = b""
    while len(buffer) < count:
        chunk = stream.read(count - len(buffer))
        if not chunk:
            return None
        buffer += chunk
    return buffer


def audio_frames(path, skip_frames=0):
    """Yields ALAC frames of AUDIO_SPF samples, straight from the decoded PCM."""
    proc = subprocess.Popen(
        ["ffmpeg", "-v", "error", "-i", path, "-vn", "-f", "s16le", "-ac",
         str(AUDIO_CHANNELS), "-ar", str(AUDIO_RATE), "pipe:1"],
        stdout=subprocess.PIPE)
    want = AUDIO_SPF * AUDIO_CHANNELS * 2
    if skip_frames:
        read_exactly(proc.stdout, skip_frames * want)
    try:
        while True:
            pcm = read_exactly(proc.stdout, want)
            if pcm is None:
                break
            yield alac_frame(pcm)
    finally:
        proc.kill()
        proc.wait()


# ------------------------------------------------------------------ H.264 Annex-B → access units

def stream_nals(stream):
    """Yields complete NAL units out of an Annex-B byte stream.

    A NAL ends where the next start code begins, so the tail of the buffer is
    held back until that start code shows up - splitting on read boundaries
    would hand the caller half a NAL and inflate the access-unit count.
    """
    buffer = b""
    for chunk in iter(lambda: stream.read(65536), b""):
        buffer += chunk
        starts = []
        pos = 0
        while True:
            hit = buffer.find(b"\x00\x00\x01", pos)
            if hit < 0:
                break
            starts.append(hit)
            pos = hit + 3
        for index in range(len(starts) - 1):
            nal = buffer[starts[index] + 3:starts[index + 1]].rstrip(b"\x00")
            if nal:
                yield nal
        if starts:
            buffer = buffer[starts[-1]:]
    tail = buffer[3:].rstrip(b"\x00") if buffer else b""
    if tail:
        yield tail


def first_mb_in_slice(nal):
    """Reads ue(v) first_mb_in_slice from a slice header: 0 means the slice
    starts a new picture, which is where one access unit ends."""
    data = nal[1:]
    bit = 0

    def read_bit():
        nonlocal bit
        if bit >> 3 >= len(data):
            raise IndexError
        value = (data[bit >> 3] >> (7 - (bit & 7))) & 1
        bit += 1
        return value

    zeros = 0
    try:
        while read_bit() == 0:
            zeros += 1
            if zeros > 32:
                return -1
        value = 0
        for _ in range(zeros):
            value = (value << 1) | read_bit()
    except IndexError:
        return -1
    return (1 << zeros) - 1 + value


def avcc(nals):
    out = bytearray()
    for nal in nals:
        out += struct.pack(">I", len(nal)) + nal
    return bytes(out)


def video_access_units(path, bitrate=None, height=None):
    """Yields (access_unit_bytes, is_idr, sps, pps).

    Without a bitrate the H.264 stream is passed through untouched. With one it
    is re-encoded: the mirror channel carries roughly 500 KB/s, so a file whose
    keyframes run to 50 KB cannot be forwarded as it is - the device's frame
    queue overflows and the picture stops.
    """
    command = ["ffmpeg", "-v", "error", "-i", path, "-an"]
    if bitrate is None:
        command += ["-c:v", "copy", "-bsf:v", "h264_mp4toannexb"]
    else:
        if height is not None:
            command += ["-vf", f"scale=-2:{height}"]
        command += ["-c:v", "libx264", "-preset", "veryfast", "-tune", "zerolatency",
                    "-profile:v", "baseline", "-bf", "0", "-g", "40",
                    "-b:v", str(bitrate), "-maxrate", str(bitrate),
                    "-bufsize", str(bitrate)]
    command += ["-f", "h264", "pipe:1"]
    proc = subprocess.Popen(command, stdout=subprocess.PIPE)
    pended = []
    sps = pps = None
    for nal in stream_nals(proc.stdout):
        kind = nal[0] & 0x1F
        if kind == 7:
            sps = nal
            continue
        if kind == 8:
            pps = nal
            continue
        if kind in (1, 5) and first_mb_in_slice(nal) != 0 and pended:
            # A continuation slice belongs to the picture already in hand.
            pended.append(nal)
            continue
        if kind in (1, 5) and pended:
            yield avcc(pended), any(n[0] & 0x1F == 5 for n in pended), sps, pps
            pended = []
        pended.append(nal)
    if pended:
        yield avcc(pended), any(n[0] & 0x1F == 5 for n in pended), sps, pps
    proc.wait()


# ------------------------------------------------------------------ transport

class Sink:
    def __init__(self, host, port, audio_port=None):
        self.sock = socket.create_connection((host, port), timeout=5)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        # Audio rides its own connection: sharing one means every keyframe stalls
        # the sound long enough to be heard.
        self.audio_sock = None
        if audio_port is not None:
            try:
                self.audio_sock = socket.create_connection((host, audio_port), timeout=5)
                self.audio_sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            except OSError as error:
                log(f"audio connection failed ({error}); sharing the video one")
        self.audio_lock = threading.Lock()
        self.lock = threading.Lock()
        self.bytes = 0
        self.frames = 0
        self.audio = 0

    def send(self, payload, audio=False):
        if audio and self.audio_sock is not None:
            with self.audio_lock:
                self.audio_sock.sendall(payload)
            self.bytes += len(payload)
            return
        with self.lock:
            self.sock.sendall(payload)
            self.bytes += len(payload)

    def configure(self, width, height, fps, sps, pps):
        header = MAGIC + struct.pack(">BBHHHHHH", TYPE_CONFIG, VERSION, 0, width, height,
                                     int(round(fps)), len(sps), len(pps))
        self.send(header + sps + pps)

    def frame(self, au, idr, pts_ms):
        header = MAGIC + struct.pack(">BBBBII", TYPE_FRAME, VERSION, 1 if idr else 0, 0,
                                     len(au), pts_ms)
        self.send(header + au)
        self.frames += 1

    def audio_frame(self, payload, pts_ms):
        header = MAGIC + struct.pack(">BBBBII", TYPE_AUDIO, VERSION, 0, 0, len(payload), pts_ms)
        self.send(header + payload, audio=True)
        self.audio += 1


# The device drains one frame per packet on its own clock, so the client has to
# stay slightly ahead: a lead of a few frames absorbs both the socket's bursty
# writes and the drift between two clocks. It must stay well inside the device's
# ring, though - a lead longer than the ring means the ring drops frames, and
# every dropped frame is a click.
AUDIO_LEAD_SEC = 0.20


def audio_worker(sink, frames, stop):
    """One ALAC frame per AUDIO_SPF/44100 s, the cadence the device sends at."""
    interval = AUDIO_SPF / AUDIO_RATE
    started = time.monotonic() - AUDIO_LEAD_SEC
    index = 0
    for payload in frames:
        if stop.is_set():
            break
        due = started + index * interval
        while True:
            now = time.monotonic()
            if now >= due:
                break
            time.sleep(min(due - now, 0.02))
        sink.audio_frame(payload, int(due * 1000))
        index += 1
    log(f"audio: {index} frame(s) sent")


def main():
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("file")
    parser.add_argument("--tcp", default="192.168.1.137:8090")
    parser.add_argument("--no-audio", action="store_true")
    parser.add_argument("--no-wait", action="store_true",
                        help="start sending even when no session is running yet")
    parser.add_argument("--video-bitrate", default=None,
                        help="re-encode the video to this bitrate (e.g. 900k) instead of "
                             "passing it through")
    parser.add_argument("--height", type=int, default=None,
                        help="with --video-bitrate, scale the picture to this height")
    args = parser.parse_args()

    host, _, port = args.tcp.partition(":")
    video, audio, fps = probe(args.file)
    log(f"{args.file}: {video['width']}x{video['height']} @ {fps:g} fps"
        + (f", audio {audio['codec_name']} {audio.get('sample_rate')} Hz "
           f"{audio.get('channels')} ch" if audio else ", no audio"))
    want_audio = audio is not None and not args.no_audio

    # Wait for the first keyframe: nothing before it can be decoded, and the
    # audio has to start at that same instant.
    keyframe_at = first_keyframe(args.file)
    units = video_access_units(args.file, args.video_bitrate, args.height)
    leading = []
    sps = pps = None
    for au, idr, sps, pps in units:
        if idr:
            leading.append((au, idr))
            break
        if sps is not None:
            leading.append((au, idr))
    if sps is None or pps is None:
        raise SystemExit("no SPS/PPS in the stream")

    sink = Sink(host, int(port), int(port) + 1 if want_audio else None)
    sink.configure(video["width"], video["height"], fps, sps, pps)
    log(f"audio starts at {keyframe_at:.3f}s, the first keyframe")

    # The device picks its content source when the session starts, so the frame
    # stream waits for /start: beginning earlier only means the receiver joins
    # in the middle of a GOP, with the sound already ahead of the picture.
    if not args.no_wait and not session_running(host):
        log("waiting for a session (curl '<device>:8080/start')")
        deadline = time.monotonic() + 60
        while not session_running(host):
            if time.monotonic() > deadline:
                raise SystemExit("no session started")
            time.sleep(0.2)

    stop = threading.Event()
    worker = None
    skip = int(round(keyframe_at * AUDIO_RATE / AUDIO_SPF))
    if want_audio:
        worker = threading.Thread(target=audio_worker,
                                  args=(sink, audio_frames(args.file, skip), stop),
                                  daemon=True)
        worker.start()

    started = time.monotonic() - AUDIO_LEAD_SEC
    index = 0
    def rest():
        for au, idr, _sps, _pps in units:
            yield au, idr

    for au, idr in itertools.chain(leading, rest()):
        due = started + index / fps
        now = time.monotonic()
        if due > now:
            time.sleep(due - now)
        sink.frame(au, idr, int(due * 1000))
        index += 1
        if index % 20 == 0:
            log(f"video: {index} frame(s), {sink.bytes // 1024} KB"
                + (f", audio: {sink.audio} frame(s)" if want_audio else ""))

    log(f"done: {index} video frame(s), {sink.audio} audio frame(s), "
        f"{sink.bytes // 1024} KB")
    stop.set()
    if worker is not None:
        worker.join(timeout=5)
    time.sleep(0.5)
    if sink.audio_sock is not None:
        sink.audio_sock.close()
    sink.sock.close()


if __name__ == "__main__":
    main()
