# CPU 正式基线：执行中断，未完成

## 结论

- 状态：`INVALID`、`partial=true`；本次正式基线未完成，不作性能验收通过依据。
- 预检 10/10 有效；正式记录 19/26，缺少 s020–s026 及 completion.json。
- 中断原因：执行器使用 180 秒前台等待，短于冻结计划名义 226 秒，外层超时终止进程组；不是已记录 workload 样本的正确性失败。
- 按单次、不补采约定停止；没有重启负载或填补缺失样本。候选分析与实现不依赖本次基线完成；收益结论仍须来自后续完整、同协议三臂对照。

## 父会话核对

- 读回正式交接、冻结计划及实际执行日志；日志记录 terminal ERROR 180.2s。
- 直接核对 manifest 预期 26 项、samples.jsonl 19 个唯一 sample_id、replay 缺失清单与预检报告 10/10。
- 19 个已记录 driver 的 returncode 均为 0，未标 timed_out/execution_error；逐条重算全部 57 个 raw 流 SHA256，一致。
- 正式收集未生成 run/reports/summary.json；可用报告是从已存 raw 重算的 replay/summary.json，不能声称原正式报告与 replay 相等。
- replay/summary.json SHA256：`7e73e2db35b42aa3c5d0989742915fe25c56a395641a908c7994f07077d35aff`。

## 仅供候选选择的部分观察

表中均为同轮 base/Go 比值的中位数；吞吐比值越低，base 越慢；P99 与 CPU/op 比值越高，base 成本越高。不完整维度仅作线索。

| 场景 / worker | 完整配对轮数 | 吞吐比 | P99 比 | CPU/op 比 |
|---|---:|---:|---:|---:|
| cpu-short-v1 / 1 | 3/3 | 0.864 | 1.128 | 1.164 |
| cpu-short-v1 / 2 | 2/3 | 0.871 | 1.789 | 1.317 |
| yield-steady-v1 / 1 | 2/3 | 0.415 | 2.606 | 2.427 |
| yield-steady-v1 / 2 | 2/3 | 0.571 | 3.077 | 1.659 |

idle 两臂均缺失。短任务单 worker 子矩阵完整，但不使整个套件有效；其他维度未完成三轮，不给正式差距或收益结论。yield 差距可作为调度分析优先线索，不证明任何函数是热点或任何候选有效。

## 身份与证据

- base 为完整已采纳 R4 dirty，不是裸 HEAD；source_id：`93a7f6293a23852aa18d509be62d23b556d11d84c3fb7dd1d901c0889cb85062`。
- Go：1.27.3，原冻结 CPU 套件；没有 candidate 组。
- raw、manifest、样本索引及 replay：`tests/reports/work/perf/411-cpu-baseline/{run,replay}/`。原始数据不入归档、不修改。
- 交接：`<本地证据路径已脱敏>`。
- 冻结计划：`<本地证据路径已脱敏>`。
- 执行日志：`<本地证据路径已脱敏>`。

本轮未修改运行时、基准驱动或 perf-tools；父会话只新增本失败结论归档。未做 A/A、profile、全核矩阵或远端 CI；未提交、推送。

> 归档说明：本 PR 只收入该历史运行的结论级 Markdown 摘要；原文提及的 raw、JSON、构建树与诊断程序未随本 PR 交付。保留历史时点的状态，不以此声明当前版本已复测。
