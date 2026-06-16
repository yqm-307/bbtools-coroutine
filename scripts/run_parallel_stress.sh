#!/bin/bash
# scripts/run_parallel_stress.sh
# 并行压测：6 模块同时跑，OS 公平调度
# 用法: ./scripts/run_parallel_stress.sh [duration_s] [threads_per_module]

set -e
ulimit -c unlimited
DUR=${1:-3600}
THREADS=${2:-$(nproc)}
BIN="./build/bin/benchmark_test/unified_stress"
OUTDIR="tests/reports/$(date +%Y-%m-%d_%H-%M-%S)"
MODULES=(comutex corwmutex cocond chan copool coroutine)

mkdir -p "$OUTDIR"
echo "[runner] dur=${DUR}s threads=${THREADS}/module cores=$(nproc)"
echo "[runner] out=$OUTDIR"

cleanup(){ for pid in "${PIDS[@]}";do kill $pid 2>/dev/null;done; }
trap cleanup EXIT

PIDS=()
for m in "${MODULES[@]}"; do
    log="$OUTDIR/${m}.log"
    FATIGUE_INTERVAL=${INTERVAL:-60} "$BIN" --module="$m" --threads="$THREADS" "$DUR" 0 0 > "$log" 2>&1 &
    PIDS+=($!)
    echo "[runner] $m → pid=$!"
done

echo "[runner] ${#PIDS[@]} running, waiting $(($DUR+30))s..."
FAIL=0
for pid in "${PIDS[@]}"; do
    wait "$pid" && rc=0 || rc=$?
    [ $rc -ne 0 ] && FAIL=1 && echo "[runner] pid=$pid FAIL (exit=$rc)"
done
trap - EXIT  # 正常完成不清除

# --- 汇总 ---
python3 -c "
import json, os
outdir='$OUTDIR'; mods=['comutex','corwmutex','cocond','chan','copool','coroutine']
all_ok=True
print(); print(f'{\"module\":>12} {\"ops\":>12} {\"errors\":>8}  status')
print('-'*45)
total=0; total_err=0
for m in mods:
    fp=os.path.join(outdir,f'{m}.log')
    if not os.path.exists(fp): print(f'{m:>12} {\"-\":>12} {\"-\":>8}  MISSING'); all_ok=False; continue
    with open(fp) as f: lines=[l for l in f if l.startswith('FATIGUE_METRIC:')]
    if not lines: print(f'{m:>12} {\"-\":>12} {\"-\":>8}  NO_DATA'); all_ok=False; continue
    d=json.loads(lines[-1].split('FATIGUE_METRIC:',1)[1])
    ops=d['ops_total']; errs=d.get('errors',0)
    total+=ops; total_err+=errs
    ok='OK' if ops>0 else 'ZERO'
    print(f'{m:>12} {ops:>12,} {errs:>8,}  {ok}')
print('-'*45)
print(f'{\"TOTAL\":>12} {total:>12,} {total_err:>8,}')
print(f'{\"result\":>12} {\"PASS\" if all_ok else \"FAIL\":>12}')
" > "$OUTDIR/summary.txt"
cat "$OUTDIR/summary.txt"
echo "[runner] done → $OUTDIR/"; exit $FAIL
