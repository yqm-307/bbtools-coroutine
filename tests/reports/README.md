# 验证报告

唯一报告根。规范文档仍只在 `agent-docs/`。

## 目录

```text
tests/reports/
  README.md                 # 本文件，入仓
  archive/                  # 入仓：结论级
    <kind>/<YYYYMMDD>-<sha>/
      summary.md
      summary.json          # 结论与关键指标，不要完整时间序列
      *.png / *.svg         # 仅当这次运行已经产出图
  work/                     # gitignore：原始运行
    <kind>/<run-id>/        # raw 日志、完整序列、ctest.xml、stdout
```

`<kind>` 只允许：`smoke` `soak` `fatigue` `perf` `sanitizer`。不另开 kind。

脚本若仍写到 `tests/reports/<时间戳>/`（如 `run_fatigue.py`），视为 work，不入仓。不在本 Issue 改脚本默认路径。

CI 仍写 `tests/ci-reports/` 与 Actions artifact；基线仍只在 `perf-baseline` 的 `tests/baselines/`。

## 入仓

提交：

- `summary.md`、`summary.json`（结论、关键指标、风险/未覆盖）
- 已有 `*.png` / `*.svg`

不提交：

- `raw/`、`*.log`
- 完整 `FATIGUE_METRIC` 时间序列
- `ctest.xml`、覆盖率 `.gcda`、构建树

何时归档：支撑 Issue/PR/发布结论、无法稳定重现、或后续要对照。日常本地跑只进 `work/`。没有自动晋升脚本：Agent 或人拷 `summary.*`。

## 本地疲劳（现有入口，输出视为 work）

```bash
python3 scripts/run_fatigue.py --duration 3600
python3 scripts/run_fatigue.py --duration 1800 --filter comutex
python3 scripts/run_fatigue.py --show-latest
python3 scripts/run_fatigue.py --compare
```

## 指标说明

| 指标 | 含义 |
|------|------|
| ops_total | 总操作数 |
| ops_per_sec | 每秒操作数 |
| lock_ops | Lock/UnLock 完成次数 |
| lock_avg_us | 平均锁等待延迟（微秒） |
| trylock_success | TryLock 成功次数 |
| trylock_timeout | TryLock 超时次数 |
| rlock_ops / wlock_ops | 读锁/写锁操作数 |
| cond_waits / cond_signals | 条件变量等待/通知次数 |
| chan_reads / chan_writes | Channel 读写次数 |
| pool_tasks / pool_dropped | 协程池任务提交/丢弃数 |
