#!/bin/bash
# 并行压测：六模块独立进程；进程退出与指标完整性同时决定结果。
# 用法: ./scripts/run_parallel_stress.sh [duration_s] [threads_per_module]
set -euo pipefail
ulimit -c unlimited
DUR=${1:-3600}
THREADS=${2:-$(nproc)}
[[ "$DUR" =~ ^[1-9][0-9]*$ && "$THREADS" =~ ^[1-9][0-9]*$ ]] || exit 2
ROOT="${PROJECT_ROOT:-$PWD}"
BIN="$ROOT/build/bin/benchmark_test/unified_stress"
OUTDIR=${OUTDIR:-"$ROOT/tests/reports/$(date +%Y-%m-%d_%H-%M-%S)-$$"}
MODULES=(comutex corwmutex cocond chan copool coroutine)
[[ -x "$BIN" && ! -e "$OUTDIR" ]] || { printf 'missing binary or existing output directory\n' >&2; exit 2; }
mkdir -p "$OUTDIR"
printf '[runner] dur=%ss threads=%s/module out=%s\n' "$DUR" "$THREADS" "$OUTDIR"

PIDS=()
cleanup() { for pid in "${PIDS[@]}"; do kill -TERM "$pid" 2>/dev/null || true; done; }
trap cleanup EXIT
trap 'exit 143' TERM
trap 'exit 130' INT
for m in "${MODULES[@]}"; do
    FATIGUE_INTERVAL=${INTERVAL:-60} timeout --signal=TERM --kill-after=30 "$((DUR+120))" \
        "$BIN" --module="$m" --threads="$THREADS" "$DUR" 0 0 > "$OUTDIR/$m.log" 2>&1 &
    PIDS+=("$!")
done
FAIL=0
for pid in "${PIDS[@]}"; do
    if wait "$pid"; then :; else FAIL=1; fi
done
trap - EXIT

# 复用指标校验，不能只看进程 rc 或已有的 PASS 文本。
python3 - "$ROOT" "$OUTDIR" "$DUR" "$FAIL" <<'PY' > "$OUTDIR/summary.txt" || FAIL=1
import json
import sys
from pathlib import Path
sys.path.insert(0, str(Path(sys.argv[1]) / 'scripts' / 'ci'))
from perf_contract import normalize_module_metrics
outdir = Path(sys.argv[2])
duration = int(sys.argv[3])
all_ok = sys.argv[4] == '0'
total = total_errors = 0
print(f'{"module":>12} {"ops":>12} {"errors":>8}  status')
print('-' * 45)
for module in ('comutex', 'corwmutex', 'cocond', 'chan', 'copool', 'coroutine'):
    try:
        records = [json.loads(line.split(':', 1)[1])
                   for line in (outdir / f'{module}.log').read_text().splitlines()
                   if line.startswith('FATIGUE_METRIC:')]
        metric = normalize_module_metrics([r for r in records if r.get('name') == module][-1])
        if metric is None:
            raise ValueError('invalid metric')
        ops, errors = metric['ops_total'], metric['errors']
        ok = ops > 0 and errors == 0 and metric['elapsed_s'] >= duration
        total += ops
        total_errors += errors
        print(f'{module:>12} {ops:>12,} {errors:>8,}  {"OK" if ok else "INVALID"}')
    except (OSError, ValueError, KeyError, IndexError, AttributeError, TypeError):
        ok = False
        print(f'{module:>12} {"-":>12} {"-":>8}  NO_DATA')
    all_ok = all_ok and ok
print('-' * 45)
print(f'{"TOTAL":>12} {total:>12,} {total_errors:>8,}')
print(f'{"result":>12} {"PASS" if all_ok else "FAIL":>12}')
sys.exit(0 if all_ok else 2)
PY
cat "$OUTDIR/summary.txt"
exit "$FAIL"
