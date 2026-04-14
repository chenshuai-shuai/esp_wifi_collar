#!/usr/bin/env python3

import argparse
import subprocess
import sys
import time
from pathlib import Path

import serial


PROJECT_ROOT = Path(__file__).resolve().parents[1]
BUILD_DIR = PROJECT_ROOT / "build"
APP_BIN = BUILD_DIR / "esp_wifi_collar.bin"
BOOTLOADER_BIN = BUILD_DIR / "bootloader" / "bootloader.bin"
PARTITION_BIN = BUILD_DIR / "partition_table" / "partition-table.bin"


def run_cmd(cmd):
    print("+", " ".join(cmd))
    subprocess.run(cmd, cwd=PROJECT_ROOT, check=True)


def build_payload(command, line_ending):
    endings = {
        "lf": "\n",
        "crlf": "\r\n",
        "cr": "\r",
        "none": "",
    }
    return f"{command}{endings[line_ending]}".encode("ascii")


def send_control(port, baud, command, settle_s, readback_s, line_ending, char_delay_s):
    response = b""
    with serial.Serial(port=port, baudrate=baud, timeout=0.05, write_timeout=1) as ser:
        ser.reset_input_buffer()
        ser.reset_output_buffer()
        payload = build_payload(command, line_ending)
        if char_delay_s > 0:
            for byte in payload:
                ser.write(bytes([byte]))
                ser.flush()
                time.sleep(char_delay_s)
        else:
            ser.write(payload)
            ser.flush()

        deadline = time.monotonic() + readback_s
        while time.monotonic() < deadline:
            chunk = ser.read(ser.in_waiting or 1)
            if chunk:
                response += chunk

    if settle_s > 0:
        time.sleep(settle_s)

    text = response.decode("utf-8", errors="ignore").strip()
    if text:
        print(text)


def ensure_build(skip_build):
    if skip_build:
        return
    run_cmd(["idf.py", "build"])


def ensure_artifacts():
    missing = [path for path in (BOOTLOADER_BIN, PARTITION_BIN, APP_BIN) if not path.exists()]
    if missing:
        joined = ", ".join(str(path.relative_to(PROJECT_ROOT)) for path in missing)
        raise FileNotFoundError(f"missing build artifacts: {joined}")


def run_esptool(port, baud, extra_args):
    cmd = [
        "python3",
        "-m",
        "esptool",
        "--chip",
        "esp32c3",
        "--port",
        port,
        "--baud",
        str(baud),
        "--before",
        "no_reset",
        "--after",
        "no_reset_stub",
    ]
    cmd.extend(extra_args)
    run_cmd(cmd)


def flash(port, control_baud, flash_baud, settle_s, readback_s, skip_build, monitor, line_ending, char_delay_s):
    ensure_build(skip_build)
    ensure_artifacts()

    send_control(port, control_baud, "NRF:ESP_DL", settle_s, readback_s, line_ending, char_delay_s)
    try:
        run_esptool(
            port,
            flash_baud,
            [
                "write_flash",
                "--flash_mode",
                "dio",
                "--flash_freq",
                "80m",
                "--flash_size",
                "4MB",
                "0x0",
                str(BOOTLOADER_BIN),
                "0x8000",
                str(PARTITION_BIN),
                "0x10000",
                str(APP_BIN),
            ],
        )
    finally:
        send_control(port, control_baud, "NRF:ESP_BOOT", settle_s, readback_s, line_ending, char_delay_s)

    if monitor:
        run_cmd(["idf.py", "-p", port, "monitor"])


def probe(port, control_baud, flash_baud, settle_s, readback_s, line_ending, char_delay_s):
    send_control(port, control_baud, "NRF:ESP_DL", settle_s, readback_s, line_ending, char_delay_s)
    try:
        run_esptool(port, flash_baud, ["chip_id"])
    finally:
        send_control(port, control_baud, "NRF:ESP_BOOT", settle_s, readback_s, line_ending, char_delay_s)


def one_shot_command(port, control_baud, command, settle_s, readback_s, line_ending, char_delay_s):
    send_control(port, control_baud, command, settle_s, readback_s, line_ending, char_delay_s)


def parse_args():
    parser = argparse.ArgumentParser(
        description="Control nRF boot pins and flash ESP32-C3 over the shared UART0 line."
    )
    parser.add_argument("--port", required=True, help="Shared UART serial port, for example /dev/ttyUSB0")
    parser.add_argument("--control-baud", type=int, default=115200, help="Baud used to send NRF:ESP_* commands")
    parser.add_argument("--flash-baud", type=int, default=115200, help="Baud used by esptool when flashing")
    parser.add_argument("--settle-ms", type=int, default=400, help="Delay after each NRF command before the next step")
    parser.add_argument("--readback-ms", type=int, default=250, help="How long to read optional nRF debug responses")
    parser.add_argument("--line-ending", choices=("lf", "crlf", "cr", "none"), default="lf", help="Line ending appended to raw commands")
    parser.add_argument("--char-delay-ms", type=int, default=0, help="Optional delay between transmitted command bytes")

    subparsers = parser.add_subparsers(dest="action", required=True)

    flash_parser = subparsers.add_parser("flash", help="Build if needed, enter download mode, flash, then boot")
    flash_parser.add_argument("--skip-build", action="store_true", help="Use existing build artifacts")
    flash_parser.add_argument("--monitor", action="store_true", help="Open idf monitor after reboot")

    subparsers.add_parser("probe", help="Enter download mode and run esptool chip_id")
    subparsers.add_parser("boot", help="Send NRF:ESP_BOOT")
    subparsers.add_parser("download", help="Send NRF:ESP_DL")
    subparsers.add_parser("reset", help="Send NRF:ESP_RST")
    send_parser = subparsers.add_parser("send", help="Send a raw ASCII line command to nRF")
    send_parser.add_argument("command", help="ASCII line to send, for example NRF:ESP_DL")

    return parser.parse_args()


def main():
    args = parse_args()
    settle_s = args.settle_ms / 1000.0
    readback_s = args.readback_ms / 1000.0
    char_delay_s = args.char_delay_ms / 1000.0

    try:
        if args.action == "flash":
            flash(
                port=args.port,
                control_baud=args.control_baud,
                flash_baud=args.flash_baud,
                settle_s=settle_s,
                readback_s=readback_s,
                skip_build=args.skip_build,
                monitor=args.monitor,
                line_ending=args.line_ending,
                char_delay_s=char_delay_s,
            )
        elif args.action == "probe":
            probe(
                port=args.port,
                control_baud=args.control_baud,
                flash_baud=args.flash_baud,
                settle_s=settle_s,
                readback_s=readback_s,
                line_ending=args.line_ending,
                char_delay_s=char_delay_s,
            )
        elif args.action == "boot":
            one_shot_command(args.port, args.control_baud, "NRF:ESP_BOOT", settle_s, readback_s, args.line_ending, char_delay_s)
        elif args.action == "download":
            one_shot_command(args.port, args.control_baud, "NRF:ESP_DL", settle_s, readback_s, args.line_ending, char_delay_s)
        elif args.action == "reset":
            one_shot_command(args.port, args.control_baud, "NRF:ESP_RST", settle_s, readback_s, args.line_ending, char_delay_s)
        elif args.action == "send":
            one_shot_command(args.port, args.control_baud, args.command, settle_s, readback_s, args.line_ending, char_delay_s)
        else:
            raise ValueError(f"unsupported action: {args.action}")
    except KeyboardInterrupt:
        return 130
    except (subprocess.CalledProcessError, FileNotFoundError, serial.SerialException, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
