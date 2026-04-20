#!/usr/bin/env python3
import argparse
import shutil
import signal
import socket
import subprocess
import sys
import wave
from pathlib import Path


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Receive ESP32 microphone PCM over UDP, write a WAV file, and optionally play live."
    )
    parser.add_argument("--bind", default="0.0.0.0", help="Local bind address")
    parser.add_argument("--port", type=int, default=3333, help="Local UDP port")
    parser.add_argument("--rate", type=int, default=24000, help="PCM sample rate")
    parser.add_argument("--channels", type=int, default=1, help="PCM channel count")
    parser.add_argument("--bits", type=int, default=16, choices=(16,), help="PCM bit depth")
    parser.add_argument("--wav", default="mic_capture.wav", help="Output WAV path")
    parser.add_argument("--play", action="store_true", help="Play audio live via ffplay")
    return parser


def main() -> int:
    args = build_parser().parse_args()

    wav_path = Path(args.wav)
    wav_path.parent.mkdir(parents=True, exist_ok=True)

    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.bind((args.bind, args.port))
    sock.settimeout(1.0)

    ffplay = None
    if args.play:
        ffplay_path = shutil.which("ffplay")
        if ffplay_path is None:
            print("ffplay not found; continuing without live playback", file=sys.stderr)
        else:
            ffplay = subprocess.Popen(
                [
                    ffplay_path,
                    "-loglevel",
                    "warning",
                    "-nodisp",
                    "-fflags",
                    "nobuffer",
                    "-flags",
                    "low_delay",
                    "-f",
                    "s16le",
                    "-ar",
                    str(args.rate),
                    "-ac",
                    str(args.channels),
                    "-",
                ],
                stdin=subprocess.PIPE,
            )

    stop = False

    def handle_stop(signum, frame):
        del signum, frame
        nonlocal stop
        stop = True

    signal.signal(signal.SIGINT, handle_stop)
    signal.signal(signal.SIGTERM, handle_stop)

    total_bytes = 0
    packet_count = 0

    print(
        f"Listening on udp://{args.bind}:{args.port} -> {wav_path} "
        f"({args.rate} Hz, {args.channels} ch, {args.bits}-bit)"
    )

    with wave.open(str(wav_path), "wb") as wav_file:
        wav_file.setnchannels(args.channels)
        wav_file.setsampwidth(args.bits // 8)
        wav_file.setframerate(args.rate)

        while not stop:
            try:
                data, addr = sock.recvfrom(4096)
            except socket.timeout:
                continue

            if not data:
                continue

            packet_count += 1
            total_bytes += len(data)
            wav_file.writeframesraw(data)

            if ffplay is not None and ffplay.stdin is not None:
                try:
                    ffplay.stdin.write(data)
                    ffplay.stdin.flush()
                except BrokenPipeError:
                    ffplay = None

            if (packet_count % 50) == 0:
                frames = total_bytes // (args.channels * (args.bits // 8))
                seconds = frames / float(args.rate)
                print(
                    f"packets={packet_count} bytes={total_bytes} seconds={seconds:.2f} "
                    f"last_from={addr[0]}:{addr[1]}"
                )

    if ffplay is not None and ffplay.stdin is not None:
        ffplay.stdin.close()
        ffplay.wait(timeout=2)

    sock.close()
    frames = total_bytes // (args.channels * (args.bits // 8))
    seconds = frames / float(args.rate)
    print(f"Saved {wav_path} bytes={total_bytes} seconds={seconds:.2f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
