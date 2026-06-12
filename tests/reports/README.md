# 疲劳测试报告

此目录存放所有疲劳测试的运行报告。每次运行生成一个时间戳子目录。

## 目录结构

```
tests/reports/
├── README.md
├── latest -> 2026-06-12_16-30-00/    # 符号链接指向最近一次
└── 2026-06-12_16-30-00/              # 单次运行
    ├── summary.json                  # 总览（JSON）
    ├── summary.md                    # 可读报告（Markdown）
    ├── comutex.json                  # comutex 模块完整指标时间序列
    ├── coroutine.json                # ...
    ├── chan.json
    ├── corwmutex.json
    ├── cocond.json
    ├── copool.json
    └── raw/                          # 原始 stdout 日志
        ├── comutex.log
        ├── coroutine.log
        └── ...
```

## 运行

```bash
# 1 小时全模块疲劳测试
python3 scripts/run_fatigue.py --duration 3600

# 30 分钟只跑 comutex
python3 scripts/run_fatigue.py --duration 1800 --filter comutex

# 查看最近报告
python3 scripts/run_fatigue.py --show-latest

# 对比最近两次
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
