#!/usr/bin/env python3
"""
fatigue_monitor.py — Launch & monitor fatigue test processes with a real-time web dashboard.

Usage:
    python3 fatigue_monitor.py [--config fatigue_config.yaml] [--port 8888] [--host 0.0.0.0]

Dependencies:
    pip install psutil aiohttp pyyaml
"""

from __future__ import annotations

import argparse
import asyncio
import csv
import json
import os
import signal
import sys
import textwrap
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional

import psutil
import yaml
from aiohttp import web

SAMPLE_INTERVAL_S = 1.0
MAX_INMEM_HISTORY = 86400  # 24 hours at 1 sample/sec
DEFAULT_CONFIG = "fatigue_config.yaml"
DEFAULT_HOST = "0.0.0.0"
DEFAULT_PORT = 8888

SCRIPT_DIR = Path(__file__).resolve().parent
PROJECT_ROOT = SCRIPT_DIR.parent.parent
HISTORY_FILE = PROJECT_ROOT / "log/fatigue_history.jsonl"


@dataclass
class ProcessConfig:
    name: str
    binary: str
    args: List[str] = field(default_factory=list)
    enabled: bool = True


@dataclass
class MonitorConfig:
    build_dir: str = "build"
    processes: List[ProcessConfig] = field(default_factory=list)


@dataclass
class ProcessInfo:
    cfg: ProcessConfig
    pid: int = 0
    proc_handle: Optional[psutil.Process] = None
    popen: Optional[asyncio.subprocess.Process] = None
    alive: bool = False
    timestamps: List[float] = field(default_factory=list)
    cpu_values: List[float] = field(default_factory=list)
    mem_values: List[float] = field(default_factory=list)


def load_config(path: str) -> MonitorConfig:
    path = Path(path)
    if not path.is_absolute():
        path = SCRIPT_DIR / path
    with open(path) as f:
        raw = yaml.safe_load(f)
    procs = []
    for entry in raw.get("processes", []):
        procs.append(ProcessConfig(
            name=entry["name"],
            binary=entry["binary"],
            args=entry.get("args", []),
            enabled=entry.get("enabled", True),
        ))
    return MonitorConfig(
        build_dir=raw.get("build_dir", "build"),
        processes=procs,
    )


def load_history_file(infos: List[ProcessInfo]) -> int:
    """Load persisted samples from JSONL into ProcessInfo objects by name.
    Returns the number of samples loaded."""
    if not HISTORY_FILE.exists():
        return 0
    name_to_pi: Dict[str, ProcessInfo] = {pi.cfg.name: pi for pi in infos}
    count = 0
    with open(HISTORY_FILE, "r") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                rec = json.loads(line)
            except json.JSONDecodeError:
                continue
            name = rec.get("name")
            if name not in name_to_pi:
                continue
            pi = name_to_pi[name]
            pi.timestamps.append(rec["ts"])
            pi.cpu_values.append(rec["cpu"])
            pi.mem_values.append(rec["mem"])
            count += 1
    for pi in infos:
        overflow = len(pi.timestamps) - MAX_INMEM_HISTORY
        if overflow > 0:
            pi.timestamps = pi.timestamps[overflow:]
            pi.cpu_values = pi.cpu_values[overflow:]
            pi.mem_values = pi.mem_values[overflow:]
    return count


async def save_sample(name: str, pid: int, ts: float, cpu: float, mem: float):
    """Append a single sample to the JSONL history file asynchronously."""
    line = json.dumps({"name": name, "pid": pid, "ts": ts, "cpu": cpu, "mem": mem})
    try:
        with open(HISTORY_FILE, "a") as f:
            f.write(line + "\n")
            f.flush()
    except OSError as e:
        print(f"[WARN] failed to persist sample: {e}")


async def launch_processes(cfg: MonitorConfig) -> List[ProcessInfo]:
    bin_dir = PROJECT_ROOT / cfg.build_dir / "bin" / "benchmark_test"
    infos: List[ProcessInfo] = []

    for pc in cfg.processes:
        if not pc.enabled:
            continue
        exe = bin_dir / pc.binary
        if not exe.exists():
            print(f"[WARN] binary not found, skipping: {exe}")
            continue

        cmd = [str(exe)] + pc.args
        print(f"[LAUNCH] {pc.name}: {' '.join(cmd)}")

        proc = await asyncio.create_subprocess_exec(
            *cmd,
            stdout=asyncio.subprocess.PIPE,
            stderr=asyncio.subprocess.PIPE,
            preexec_fn=os.setsid,
        )

        pi = ProcessInfo(
            cfg=pc,
            pid=proc.pid,
            proc_handle=psutil.Process(proc.pid),
            popen=proc,
            alive=True,
        )
        infos.append(pi)
        print(f"  → pid={proc.pid}")

    return infos


async def drain_pipes(infos: List[ProcessInfo]):
    async def _reader(name: str, stream, prefix: str):
        while True:
            line = await stream.readline()
            if not line:
                break
            print(f"[{prefix}] {line.decode(errors='replace').rstrip()}")

    tasks = []
    for pi in infos:
        if pi.popen and pi.popen.stdout:
            tasks.append(asyncio.create_task(
                _reader(pi.cfg.name, pi.popen.stdout, pi.cfg.name)))
        if pi.popen and pi.popen.stderr:
            tasks.append(asyncio.create_task(
                _reader(pi.cfg.name, pi.popen.stderr, f"{pi.cfg.name}.ERR")))
    if tasks:
        await asyncio.gather(*tasks, return_exceptions=True)


async def kill_all(infos: List[ProcessInfo]):
    for pi in infos:
        if pi.alive:
            try:
                os.killpg(os.getpgid(pi.pid), signal.SIGTERM)
                pi.alive = False
            except ProcessLookupError:
                pass

    await asyncio.sleep(3.0)
    for pi in infos:
        try:
            os.killpg(os.getpgid(pi.pid), signal.SIGKILL)
        except (ProcessLookupError, OSError):
            pass


CONFIG: Optional[MonitorConfig] = None
DRAIN_TASKS: List[asyncio.Task] = []


async def start_single_process(name: str, bin_dir: Path) -> Optional[ProcessInfo]:
    """Launch a single fatigue process by config name. Returns ProcessInfo or None."""
    if not CONFIG:
        return None
    pc = next((p for p in CONFIG.processes if p.name == name), None)
    if not pc:
        return None
    exe = bin_dir / pc.binary
    if not exe.exists():
        print(f"[WARN] binary not found: {exe}")
        return None

    cmd = [str(exe)] + pc.args
    print(f"[LAUNCH] {pc.name}: {' '.join(cmd)}")
    proc = await asyncio.create_subprocess_exec(
        *cmd,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
        preexec_fn=os.setsid,
    )
    pi = ProcessInfo(
        cfg=pc,
        pid=proc.pid,
        proc_handle=psutil.Process(proc.pid),
        popen=proc,
        alive=True,
    )
    print(f"  → pid={proc.pid}")

    async def _reader(stream, prefix: str):
        while True:
            line = await stream.readline()
            if not line:
                break
            print(f"[{prefix}] {line.decode(errors='replace').rstrip()}")

    if proc.stdout:
        DRAIN_TASKS.append(asyncio.create_task(_reader(proc.stdout, pc.name)))
    if proc.stderr:
        DRAIN_TASKS.append(asyncio.create_task(_reader(proc.stderr, f"{pc.name}.ERR")))
    return pi


async def stop_single_process(pi: ProcessInfo):
    """Kill a single process and mark it dead."""
    if not pi.alive:
        return
    try:
        os.killpg(os.getpgid(pi.pid), signal.SIGTERM)
    except (ProcessLookupError, OSError):
        pass
    await asyncio.sleep(0.5)
    try:
        os.killpg(os.getpgid(pi.pid), signal.SIGKILL)
    except (ProcessLookupError, OSError):
        pass
    if pi.popen and pi.popen.returncode is None:
        try:
            pi.popen.terminate()
        except ProcessLookupError:
            pass
    pi.alive = False
    print(f"[STOP] {pi.cfg.name} pid={pi.pid}")


async def monitor_loop(infos: List[ProcessInfo],
                       ws_clients: List[web.WebSocketResponse]):
    while True:
        alive_any = False
        snapshots: List[dict] = []

        for pi in infos:
            if not pi.alive:
                continue

            returncode = pi.popen.returncode if pi.popen else None
            if returncode is not None:
                print(f"[EXIT] {pi.cfg.name} pid={pi.pid} rc={returncode}")
                pi.alive = False
                pi.timestamps.append(time.time())
                pi.cpu_values.append(0.0)
                pi.mem_values.append(0.0)
                continue

            alive_any = True
            try:
                cpu = pi.proc_handle.cpu_percent(interval=None)
                mem_info = pi.proc_handle.memory_info()
                mem_mb = mem_info.rss / (1024 * 1024)
            except (psutil.NoSuchProcess, psutil.AccessDenied):
                print(f"[DIED] {pi.cfg.name} pid={pi.pid}")
                pi.alive = False
                continue

            t = time.time()
            pi.timestamps.append(t)
            pi.cpu_values.append(round(cpu, 2))
            pi.mem_values.append(round(mem_mb, 2))

            overflow = len(pi.timestamps) - MAX_INMEM_HISTORY
            if overflow > 0:
                pi.timestamps = pi.timestamps[overflow:]
                pi.cpu_values = pi.cpu_values[overflow:]
                pi.mem_values = pi.mem_values[overflow:]

            await save_sample(pi.cfg.name, pi.pid, t, round(cpu, 2), round(mem_mb, 2))

            snapshots.append({
                "name": pi.cfg.name,
                "pid": pi.pid,
                "alive": True,
                "cpu": round(cpu, 2),
                "mem": round(mem_mb, 2),
                "time": t,
            })

        if snapshots:
            payload = json.dumps({"ts": time.time(), "data": snapshots})
            disconnected = []
            for ws in ws_clients:
                try:
                    await ws.send_str(payload)
                except (ConnectionResetError, ConnectionError):
                    disconnected.append(ws)
            for ws in disconnected:
                ws_clients.remove(ws)

        if not alive_any:
            await asyncio.sleep(SAMPLE_INTERVAL_S)
            continue

        await asyncio.sleep(SAMPLE_INTERVAL_S)

    for ws in ws_clients:
        try:
            await ws.send_str(
                json.dumps({"ts": time.time(), "data": [], "done": True}))
        except Exception:
            pass


WS_CLIENTS: List[web.WebSocketResponse] = []
INFOS_LIST: List[ProcessInfo] = []


async def index_handler(request: web.Request) -> web.Response:
    html_path = SCRIPT_DIR / "fatigue_dashboard.html"
    return web.FileResponse(html_path)


async def history_handler(request: web.Request) -> web.Response:
    since = request.query.get("since")
    since_ts = float(since) if since else None
    result = []
    for pi in INFOS_LIST:
        timestamps = pi.timestamps
        cpu = pi.cpu_values
        mem = pi.mem_values
        if since_ts is not None:
            start = 0
            for i, ts in enumerate(timestamps):
                if ts >= since_ts:
                    start = i
                    break
            timestamps = timestamps[start:]
            cpu = cpu[start:]
            mem = mem[start:]
        result.append({
            "name": pi.cfg.name,
            "pid": pi.pid,
            "alive": pi.alive,
            "timestamps": timestamps,
            "cpu": cpu,
            "mem": mem,
        })
    return web.json_response(result)


async def ws_handler(request: web.Request) -> web.WebSocketResponse:
    ws = web.WebSocketResponse(heartbeat=15.0)
    await ws.prepare(request)
    WS_CLIENTS.append(ws)
    print(f"[WS] client connected (total={len(WS_CLIENTS)})")
    try:
        async for _ in ws:
            pass
    finally:
        WS_CLIENTS.remove(ws)
        print(f"[WS] client disconnected (total={len(WS_CLIENTS)})")
    return ws


async def csv_download_handler(request: web.Request) -> web.Response:
    lines = ["process,pid,timestamp,cpu_percent,mem_mb"]
    for pi in INFOS_LIST:
        for i in range(len(pi.timestamps)):
            lines.append(
                f"{pi.cfg.name},{pi.pid},{pi.timestamps[i]:.3f},"
                f"{pi.cpu_values[i]:.2f},{pi.mem_values[i]:.2f}"
            )
    return web.Response(
        text="\n".join(lines),
        content_type="text/csv",
        headers={
            "Content-Disposition": "attachment; filename=fatigue_data.csv"},
    )


async def api_status_handler(request: web.Request) -> web.Response:
    result = []
    for pi in INFOS_LIST:
        result.append({
            "name": pi.cfg.name,
            "pid": pi.pid,
            "alive": pi.alive,
        })
    return web.json_response(result)


async def api_process_start(request: web.Request) -> web.Response:
    name = request.match_info["name"]
    existing = next((pi for pi in INFOS_LIST if pi.cfg.name == name), None)
    if existing and existing.alive:
        return web.json_response({"ok": False, "error": f"{name} is already running"}, status=409)

    bin_dir = PROJECT_ROOT / CONFIG.build_dir / "bin" / "benchmark_test"
    pi = await start_single_process(name, bin_dir)
    if not pi:
        return web.json_response({"ok": False, "error": "process config not found or binary missing"}, status=404)

    if existing:
        existing.pid = pi.pid
        existing.proc_handle = pi.proc_handle
        existing.popen = pi.popen
        existing.alive = True
    else:
        INFOS_LIST.append(pi)
    return web.json_response({"ok": True, "name": name, "pid": pi.pid})


async def api_process_stop(request: web.Request) -> web.Response:
    name = request.match_info["name"]
    existing = next((pi for pi in INFOS_LIST if pi.cfg.name == name), None)
    if not existing:
        return web.json_response({"ok": False, "error": "process not found"}, status=404)
    if not existing.alive:
        return web.json_response({"ok": False, "error": f"{name} is not running"}, status=409)

    await stop_single_process(existing)
    return web.json_response({"ok": True, "name": name})


async def api_process_restart(request: web.Request) -> web.Response:
    name = request.match_info["name"]
    existing = next((pi for pi in INFOS_LIST if pi.cfg.name == name), None)
    if not existing:
        return web.json_response({"ok": False, "error": "process not found"}, status=404)

    if existing.alive:
        await stop_single_process(existing)

    bin_dir = PROJECT_ROOT / CONFIG.build_dir / "bin" / "benchmark_test"
    pi = await start_single_process(name, bin_dir)
    if not pi:
        return web.json_response({"ok": False, "error": "failed to restart"}, status=500)

    existing.pid = pi.pid
    existing.proc_handle = pi.proc_handle
    existing.popen = pi.popen
    existing.alive = True
    return web.json_response({"ok": True, "name": name, "pid": pi.pid})


async def api_start_all(request: web.Request) -> web.Response:
    bin_dir = PROJECT_ROOT / CONFIG.build_dir / "bin" / "benchmark_test"
    started = []
    for pi in INFOS_LIST:
        if not pi.alive:
            new_pi = await start_single_process(pi.cfg.name, bin_dir)
            if new_pi:
                pi.pid = new_pi.pid
                pi.proc_handle = new_pi.proc_handle
                pi.popen = new_pi.popen
                pi.alive = True
                started.append(pi.cfg.name)
    return web.json_response({"ok": True, "started": started})


async def api_stop_all(request: web.Request) -> web.Response:
    stopped = []
    for pi in INFOS_LIST:
        if pi.alive:
            await stop_single_process(pi)
            stopped.append(pi.cfg.name)
    return web.json_response({"ok": True, "stopped": stopped})


def parse_args():
    p = argparse.ArgumentParser(
        description="Fatigue test process monitor + dashboard")
    p.add_argument("--config", default=DEFAULT_CONFIG,
                   help="YAML config file path")
    p.add_argument("--host", default=DEFAULT_HOST, help="Bind address")
    p.add_argument("--port", type=int, default=DEFAULT_PORT, help="Bind port")
    p.add_argument("--no-launch", action="store_true",
                   help="Attach to existing PIDs instead of launching binaries")
    return p.parse_args()


def print_banner(host: str, port: int):
    print(textwrap.dedent(f"""
    ╔══════════════════════════════════════════════════════════════╗
    ║              Fatigue Test Monitor Dashboard                  ║
    ║                                                            ║
    ║   Dashboard  →  http://{host}:{port}                        ║
    ║   CSV Export →  http://{host}:{port}/csv                     ║
    ║   JSON Data  →  http://{host}:{port}/history                 ║
    ║                                                            ║
    ║   Press Ctrl+C to stop monitoring and exit.                 ║
    ╚══════════════════════════════════════════════════════════════╝
    """))


async def main():
    args = parse_args()
    cfg = load_config(args.config)

    enabled = [p for p in cfg.processes if p.enabled]
    if not enabled:
        print("[ERROR] No enabled processes in config. Exiting.")
        sys.exit(1)

    global CONFIG
    CONFIG = cfg

    if not args.no_launch:
        infos = await launch_processes(cfg)
    else:
        infos = [ProcessInfo(cfg=pc) for pc in enabled]

    if not infos:
        print("[ERROR] No processes launched. Exiting.")
        sys.exit(1)

    loaded = load_history_file(infos)
    if loaded:
        print(f"[INFO] Loaded {loaded} persisted samples from {HISTORY_FILE}")

    global INFOS_LIST
    INFOS_LIST = infos

    asyncio.create_task(drain_pipes(infos))
    monitor_task = asyncio.create_task(monitor_loop(infos, WS_CLIENTS))

    stop_event = asyncio.Event()

    def _sig_handler():
        print("\n[INFO] Received interrupt signal.")
        stop_event.set()

    loop = asyncio.get_running_loop()
    try:
        loop.add_signal_handler(signal.SIGINT, _sig_handler)
        loop.add_signal_handler(signal.SIGTERM, _sig_handler)
    except NotImplementedError:
        pass

    app = web.Application()
    app.router.add_get("/", index_handler)
    app.router.add_get("/history", history_handler)
    app.router.add_get("/csv", csv_download_handler)
    app.router.add_get("/ws", ws_handler)
    app.router.add_get("/api/status", api_status_handler)
    app.router.add_post("/api/process/{name}/start", api_process_start)
    app.router.add_post("/api/process/{name}/stop", api_process_stop)
    app.router.add_post("/api/process/{name}/restart", api_process_restart)
    app.router.add_post("/api/processes/start-all", api_start_all)
    app.router.add_post("/api/processes/stop-all", api_stop_all)

    runner = web.AppRunner(app)
    await runner.setup()
    site = web.TCPSite(runner, args.host, args.port)
    await site.start()

    print_banner(args.host, args.port)

    try:
        await stop_event.wait()
    finally:
        monitor_task.cancel()
        try:
            await monitor_task
        except asyncio.CancelledError:
            pass

    print("\n[INFO] Shutting down...")
    await kill_all(infos)
    await runner.cleanup()

    csv_path = PROJECT_ROOT / "fatigue_results.csv"
    with open(csv_path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["process", "pid", "timestamp", "cpu_percent", "mem_mb"])
        for pi in infos:
            for i in range(len(pi.timestamps)):
                w.writerow([
                    pi.cfg.name, pi.pid, f"{pi.timestamps[i]:.3f}",
                    f"{pi.cpu_values[i]:.2f}", f"{pi.mem_values[i]:.2f}"])
    print(f"[INFO] CSV saved to {csv_path}")
    print("[INFO] Done.")


if __name__ == "__main__":
    asyncio.run(main())
