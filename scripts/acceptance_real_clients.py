#!/usr/bin/env python3
"""可复现的真实客户端验收：Echo + hiredis。

Echo 覆盖阻塞 POSIX socket 的变长/二进制 payload、碎片发送、并发、
空闲、半关闭、异常断连和重连。随机性固定 seed，失败可复现。
hiredis 复用仓内真实同步客户端示例，验证第三方库 FD 和协程并发路径。
"""

from __future__ import annotations

import argparse
import random
import socket
import subprocess
import sys
import threading
import time
from pathlib import Path

SKIP = 77


def recv_exact(sock: socket.socket, size: int) -> bytes:
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise RuntimeError(f"unexpected EOF after {len(data)}/{size} bytes")
        data.extend(chunk)
    return bytes(data)


def send_fragmented(sock: socket.socket, payload: bytes, rng: random.Random) -> None:
    offset = 0
    while offset < len(payload):
        size = min(len(payload) - offset, rng.randint(1, 12))
        sock.sendall(payload[offset:offset + size])
        offset += size
        if rng.random() < 0.25:
            time.sleep(0.001)


def echo_client(client_id: int, rounds: int, seed: int, errors: list[str]) -> None:
    rng = random.Random(seed)
    lengths = [1, 2, 7, 15, 31, 32, 33, 64, 127]
    try:
        for reconnect in range(rounds):
            with socket.create_connection(("127.0.0.1", 10010), timeout=5) as sock:
                sock.settimeout(5)
                for message_id in range(16):
                    length = rng.choice(lengths)
                    payload = bytes(rng.randrange(256) for _ in range(length))
                    send_fragmented(sock, payload, rng)
                    if recv_exact(sock, len(payload)) != payload:
                        raise RuntimeError(
                            f"echo mismatch client={client_id} round={reconnect}"
                        )
                if reconnect == rounds - 1:
                    payload = bytes(rng.randrange(256) for _ in range(23))
                    send_fragmented(sock, payload, rng)
                    if recv_exact(sock, len(payload)) != payload:
                        raise RuntimeError("half-close payload mismatch")
                    sock.shutdown(socket.SHUT_WR)
                    if sock.recv(1) != b"":
                        raise RuntimeError("server did not close after half-close")
            if rng.random() < 0.5:
                time.sleep(0.002)
    except Exception as exc:  # noqa: BLE001 - report client and scenario
        errors.append(f"client={client_id}: {exc!r}")


def run_echo(server: Path, clients: int, rounds: int, seed: int) -> None:
    with server_log() as log:
        proc = subprocess.Popen(
            [str(server)], stdout=log, stderr=subprocess.STDOUT
        )
        try:
            wait_port(10010)
            errors: list[str] = []
            threads = [
                threading.Thread(
                    target=echo_client,
                    args=(i, rounds, seed + i, errors),
                )
                for i in range(clients)
            ]
            for thread in threads:
                thread.start()

            # 异常断连：服务端应清理连接，不能影响后续客户端。
            for _ in range(8):
                with socket.create_connection(("127.0.0.1", 10010), timeout=5) as sock:
                    sock.sendall(b"partial-disconnect")

            for thread in threads:
                thread.join(timeout=30)
            if any(thread.is_alive() for thread in threads):
                raise RuntimeError("echo client thread did not finish")
            if errors:
                raise RuntimeError("; ".join(errors[:8]))

            # 断连后再验证一次新连接，覆盖重连入口。
            with socket.create_connection(("127.0.0.1", 10010), timeout=5) as sock:
                sock.settimeout(5)
                payload = b"reconnect-after-abort\x00\xff"
                sock.sendall(payload)
                if recv_exact(sock, len(payload)) != payload:
                    raise RuntimeError("reconnect echo mismatch")
            print(
                f"echo PASS clients={clients} rounds={rounds} "
                f"messages={clients * (rounds * 16 + 1) + 1} seed={seed}"
            )
        finally:
            proc.terminate()
            try:
                proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                proc.kill()
                proc.wait(timeout=5)


def server_log():
    return open("/tmp/bbtools-coroutine-echo-acceptance.log", "w", encoding="utf-8")


def wait_port(port: int) -> None:
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2):
                return
        except OSError:
            time.sleep(0.05)
    raise RuntimeError(f"echo server did not listen on {port}")


def redis_available(port: int) -> bool:
    try:
        with socket.create_connection(("127.0.0.1", port), timeout=1):
            return True
    except OSError:
        return False


def run_hiredis(binary: Path, timeout: int, redis_port: int) -> None:
    if not redis_available(redis_port):
        print(f"hiredis SKIP Redis unavailable on 127.0.0.1:{redis_port}")
        raise SystemExit(SKIP)
    result = subprocess.run(
        [str(binary)], capture_output=True, text=True, timeout=timeout
    )
    if result.returncode != 0:
        raise RuntimeError(
            f"hiredis exit={result.returncode}: "
            f"{result.stderr[-500:] or result.stdout[-500:]}"
        )
    output = result.stdout + result.stderr
    if "Total successful operations: 10000" not in output:
        raise RuntimeError(f"hiredis success count missing: {output[-1000:]!r}")
    if "Total errors: 0" not in output:
        raise RuntimeError(f"hiredis reported operation errors: {output[-1000:]!r}")
    if "Error:" in output or "Assertion failed" in output:
        raise RuntimeError("hiredis reported an error")
    print("hiredis PASS tasks=10000 commands>=40000 errors=0")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--echo-server", type=Path, required=True)
    parser.add_argument("--hiredis", type=Path, required=True)
    parser.add_argument("--clients", type=int, default=12)
    parser.add_argument("--rounds", type=int, default=3)
    parser.add_argument("--seed", type=int, default=20260909)
    parser.add_argument("--timeout", type=int, default=90)
    parser.add_argument("--redis-port", type=int, default=6379)
    args = parser.parse_args()

    run_echo(args.echo_server, args.clients, args.rounds, args.seed)
    run_hiredis(args.hiredis, args.timeout, args.redis_port)
    print("real-client acceptance PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
