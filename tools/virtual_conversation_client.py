#!/usr/bin/env python3
"""Virtual client for the collar realtime ConversationService.

It mirrors the ESP32 conversation path:
  1. open StreamConversation with user-id/session-id metadata
  2. wait a few seconds
  3. upload generated PCM16 noise for a fixed duration
  4. half-close the stream and call EndConversation
"""

import argparse
import importlib
import os
import random
import struct
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import grpc


DEFAULT_HOST = "traini-grpc-collar-dev-nlb-06c70e7556ee7ab5.elb.us-east-1.amazonaws.com"
DEFAULT_PORT = 50051
DEFAULT_USER_ID = "CollarOne"
DEFAULT_SAMPLE_RATE = 24000
DEFAULT_CHANNELS = 1
DEFAULT_BIT_DEPTH = 16
DEFAULT_ENCODING = "pcm16"


def log(message: str):
    print(message, flush=True)


class Esp32c3ResourceModel:
    """Coarse resource gate for the current ESP32-C3 conversation path.

    This is intentionally conservative: it models the firmware's static buffers,
    tiny TX queue, frame caps, and the observed free heap at the point where
    nghttp2_session_client_new() currently fails on-device. It does not try to
    emulate Xtensa/RISC-V execution or the ESP-IDF allocator byte-for-byte.
    """

    CONV_TASK_STACK = 6144 * 4
    DIALOG_UL_STACK = 6144 * 4
    TX_QUEUE_DEPTH = 4
    TX_AUDIO_MAX = 3200
    TX_FRAME_MAX = 6144
    RX_BUFFER = 32768
    PENDING_ITEM = 3216
    TX_QUEUE_STORAGE = TX_QUEUE_DEPTH * PENDING_ITEM

    def __init__(self, args):
        self.enabled = args.esp32c3_sim
        self.free_heap = args.esp_free_heap
        self.largest_block = args.esp_largest_block or args.esp_free_heap
        self.nghttp2_session_min = args.nghttp2_session_min
        self.audio_max = args.esp_tx_audio_max
        self.frame_max = args.esp_tx_frame_max
        self.queue_depth = args.esp_tx_queue_depth
        self.queue_used = 0

    def banner(self):
        if not self.enabled:
            return
        static_conversation = (
            self.CONV_TASK_STACK
            + self.DIALOG_UL_STACK
            + self.TX_QUEUE_STORAGE
            + self.RX_BUFFER
            + self.PENDING_ITEM
            + self.TX_FRAME_MAX
        )
        log("[esp32c3] resource model enabled")
        log(
            "[esp32c3] firmware static conversation/uplink footprint estimate="
            f"{static_conversation} bytes "
            f"(conv_stack={self.CONV_TASK_STACK}, ul_stack={self.DIALOG_UL_STACK}, "
            f"rx={self.RX_BUFFER}, queue={self.TX_QUEUE_STORAGE}, tx_frame={self.TX_FRAME_MAX})"
        )
        log(
            "[esp32c3] runtime heap gate: "
            f"free_heap={self.free_heap} largest_block={self.largest_block} "
            f"nghttp2_session_min={self.nghttp2_session_min}"
        )

    def check_audio_chunk(self, pcm_len: int, grpc_frame_len: int, seq: int):
        if not self.enabled:
            return
        if pcm_len > self.audio_max:
            raise MemoryError(
                f"ESP CONV_TX_AUDIO_MAX_BYTES exceeded at seq={seq}: "
                f"pcm={pcm_len} cap={self.audio_max}"
            )
        if grpc_frame_len > self.frame_max:
            raise MemoryError(
                f"ESP CONV_TX_FRAME_MAX_BYTES exceeded at seq={seq}: "
                f"grpc_frame={grpc_frame_len} cap={self.frame_max}"
            )
        if self.queue_used >= self.queue_depth:
            raise MemoryError(
                f"ESP TX queue full at seq={seq}: depth={self.queue_depth}"
            )
        self.queue_used += 1

    def mark_chunk_sent(self):
        if self.enabled and self.queue_used > 0:
            self.queue_used -= 1

    def check_nghttp2_session_new(self):
        if not self.enabled:
            return
        log(
            "[esp32c3] h2_connect_if_needed: attempting nghttp2_session_client_new "
            f"with largest_block={self.largest_block}"
        )
        if self.largest_block < self.nghttp2_session_min:
            raise MemoryError(
                "nghttp2_session_client_new would fail on ESP32-C3: "
                f"largest_block={self.largest_block} < required~={self.nghttp2_session_min}"
            )


def compile_proto(repo_root: Path, out_dir: Path):
    proto = repo_root / "protocol" / "traini.proto"
    cmd = [
        sys.executable,
        "-m",
        "grpc_tools.protoc",
        f"-I{proto.parent}",
        f"--python_out={out_dir}",
        f"--grpc_python_out={out_dir}",
        str(proto),
    ]
    subprocess.run(cmd, check=True)
    sys.path.insert(0, str(out_dir))
    traini_pb2 = importlib.import_module("traini_pb2")
    traini_pb2_grpc = importlib.import_module("traini_pb2_grpc")
    return traini_pb2, traini_pb2_grpc


def noise_pcm16(sample_count: int, rng: random.Random, amplitude: int) -> bytes:
    samples = (rng.randint(-amplitude, amplitude) for _ in range(sample_count))
    return struct.pack("<" + ("h" * sample_count), *samples)


def request_iter(args, traini_pb2, resources: Esp32c3ResourceModel):
    fmt = traini_pb2.AudioFormat(
        sample_rate=args.sample_rate,
        channels=args.channels,
        bit_depth=args.bit_depth,
        encoding=args.encoding,
    )
    samples_per_chunk = int(args.sample_rate * args.channels * args.chunk_ms / 1000)
    rng = random.Random(args.seed)

    log(f"[client] stream opened; waiting {args.pre_delay:.1f}s before audio")
    time.sleep(args.pre_delay)

    total_chunks = int(args.audio_seconds * 1000 / args.chunk_ms)
    next_send = time.monotonic()
    for seq in range(1, total_chunks + 1):
        now_ms = int(time.time() * 1000)
        pcm = noise_pcm16(samples_per_chunk, rng, args.amplitude)
        # Approximate protobuf payload overhead for AudioChunk with format,
        # sequence number, and timestamp. ESP adds a 5-byte gRPC message prefix.
        grpc_frame_len = len(pcm) + 96
        resources.check_audio_chunk(len(pcm), grpc_frame_len, seq)
        yield traini_pb2.AudioChunk(
            audio_data=pcm,
            format=fmt,
            sequence_number=seq,
            timestamp=traini_pb2.Timestamp(
                seconds=now_ms // 1000,
                nanos=(now_ms % 1000) * 1_000_000,
            ),
        )
        resources.mark_chunk_sent()
        if seq == 1 or seq % 50 == 0 or seq == total_chunks:
            log(f"[client] TX seq={seq}/{total_chunks} bytes={len(pcm)}")

        next_send += args.chunk_ms / 1000.0
        sleep_s = next_send - time.monotonic()
        if sleep_s > 0:
            time.sleep(sleep_s)

    log("[client] audio upload complete; half-closing StreamConversation")


def run(args) -> int:
    repo_root = Path(__file__).resolve().parents[1]
    session_id = args.session_id or f"codex-session-{int(time.time() * 1000)}"
    resources = Esp32c3ResourceModel(args)
    resources.banner()

    with tempfile.TemporaryDirectory(prefix="traini_grpc_py_") as td:
        traini_pb2, traini_pb2_grpc = compile_proto(repo_root, Path(td))

        target = f"{args.host}:{args.port}"
        metadata = (("user-id", args.user_id), ("session-id", session_id))
        options = [
            ("grpc.max_receive_message_length", 16 * 1024 * 1024),
            ("grpc.max_send_message_length", 16 * 1024 * 1024),
        ]

        log(f"[client] target={target} plaintext user-id={args.user_id} session-id={session_id}")
        log(
            "[client] audio="
            f"{args.sample_rate}Hz/{args.channels}ch/{args.bit_depth}bit/"
            f"{args.encoding} raw bytes chunk_ms={args.chunk_ms}"
        )

        with grpc.insecure_channel(target, options=options) as channel:
            grpc.channel_ready_future(channel).result(timeout=args.connect_timeout)
            stub = traini_pb2_grpc.ConversationServiceStub(channel)

            rx_events = 0
            rx_audio_bytes = 0
            stream_attempted = False
            try:
                resources.check_nghttp2_session_new()
                stream_attempted = True
                deadline = args.pre_delay + args.audio_seconds + args.response_grace
                responses = stub.StreamConversation(
                    request_iter(args, traini_pb2, resources),
                    metadata=metadata,
                    timeout=deadline,
                )
                for event in responses:
                    rx_events += 1
                    which = event.WhichOneof("event")
                    if which == "audio_output":
                        n = len(event.audio_output.audio_data)
                        rx_audio_bytes += n
                        log(
                            "[client] RX audio_output "
                            f"seq={event.audio_output.sequence_number} bytes={n}"
                        )
                    elif which == "audio_complete":
                        log(f"[client] RX audio_complete total_chunks={event.audio_complete.total_chunks}")
                    elif which == "error":
                        log(f"[client] RX error code={event.error.code} message={event.error.message}")
                    else:
                        log(f"[client] RX event={which}")
            except MemoryError as exc:
                log(f"[esp32c3] FAIL before/during StreamConversation: {exc}")
                log("[esp32c3] matching firmware behavior: abort session and send standalone EndConversation")
            except grpc.RpcError as exc:
                code = exc.code()
                if code == grpc.StatusCode.DEADLINE_EXCEEDED:
                    log("[client] StreamConversation deadline reached after upload; continuing to EndConversation")
                else:
                    log(f"[client] StreamConversation RPC ended: code={code.name} details={exc.details()}")

            if args.esp32c3_sim and not stream_attempted:
                log("[esp32c3] no audio was sent because simulated C3 resources blocked stream creation")
            log(f"[client] calling EndConversation session-id={session_id}")
            req = traini_pb2.EndConversationRequest(session_id=session_id)
            summary = stub.EndConversation(req, metadata=metadata, timeout=args.end_timeout)
            log(f"[client] EndConversation OK summary.session_id={summary.session_id!r}")
            log(f"[client] done rx_events={rx_events} rx_audio_bytes={rx_audio_bytes}")
    return 0


def parse_args():
    parser = argparse.ArgumentParser(description="Run a virtual ESP32 ConversationService session.")
    parser.add_argument("--host", default=os.getenv("COLLAR_CONV_HOST", DEFAULT_HOST))
    parser.add_argument("--port", type=int, default=int(os.getenv("COLLAR_CONV_PORT", DEFAULT_PORT)))
    parser.add_argument("--user-id", default=os.getenv("COLLAR_CONV_USER_ID", DEFAULT_USER_ID))
    parser.add_argument("--session-id", default=os.getenv("COLLAR_CONV_SESSION_ID"))
    parser.add_argument("--pre-delay", type=float, default=3.0)
    parser.add_argument("--audio-seconds", type=float, default=10.0)
    parser.add_argument("--chunk-ms", type=int, default=20)
    parser.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE)
    parser.add_argument("--channels", type=int, default=DEFAULT_CHANNELS)
    parser.add_argument("--bit-depth", type=int, default=DEFAULT_BIT_DEPTH)
    parser.add_argument("--encoding", default=DEFAULT_ENCODING)
    parser.add_argument("--amplitude", type=int, default=1200)
    parser.add_argument("--seed", type=int, default=0xC011A4)
    parser.add_argument("--connect-timeout", type=float, default=8.0)
    parser.add_argument("--response-grace", type=float, default=15.0)
    parser.add_argument("--end-timeout", type=float, default=8.0)
    parser.add_argument(
        "--esp32c3-sim",
        action="store_true",
        help="Enable the ESP32-C3 resource model before/during the gRPC flow.",
    )
    parser.add_argument(
        "--esp-free-heap",
        type=int,
        default=13256,
        help="Observed free heap at StreamConversation transport bring-up.",
    )
    parser.add_argument(
        "--esp-largest-block",
        type=int,
        default=0,
        help="Largest allocatable heap block. Defaults to --esp-free-heap.",
    )
    parser.add_argument(
        "--nghttp2-session-min",
        type=int,
        default=18000,
        help="Estimated minimum contiguous bytes needed for nghttp2_session_client_new.",
    )
    parser.add_argument("--esp-tx-audio-max", type=int, default=3200)
    parser.add_argument("--esp-tx-frame-max", type=int, default=6144)
    parser.add_argument("--esp-tx-queue-depth", type=int, default=4)
    return parser.parse_args()


if __name__ == "__main__":
    raise SystemExit(run(parse_args()))
