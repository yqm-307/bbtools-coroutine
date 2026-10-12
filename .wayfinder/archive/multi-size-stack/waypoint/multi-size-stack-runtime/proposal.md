---
schema: wfer-execution/v1
plan_id: multi-size-stack-runtime
revision: 2
artifact: proposal
status: blocked
depends_on: []
---
# 多尺寸固定栈端到端支持 · 提案

## 目的
让同一 coroutine 运行时同时使用不同大小的固定栈，不再要求所有协程共享一种栈大小。不带大小参数使用全局默认；显式请求使用指定字节数并向上按系统页对齐。已有栈不扩缩或换栈。

## 范围
- 唯一路径点 `multi-size-stack-runtime`，目标仓 `bbtools-coroutine`；覆盖 C++ 创建入口、Coroutine→Context→StackPool→Stack 的参数与所有权链、尺寸/保护属性分桶、统一容量、预热/回收、错误清理、直接兼容性和验证。
- 保留 `bbtco`、`bbtco_desc` 和原有默认注册调用；增加大小在前的 C++ 注册重载，不新增宏或配置项。
- 分桶键为页对齐后的实际可用字节数及保护属性，不强制二次幂；继续 `memalign + mprotect`，保护页额外分配。
- 不包含伙伴分配器、自动扩栈、CoPool 按任务换栈或分流、新调度器/线程、infra/framework 接入、默认 12 KiB 调整、跨平台整体验收、CI 重构与发布。

## 影响与依据
- 冻结技术依据为已获取提交 `b172f53e66ee8c7e5008272d3a4eaccc23206647`，不是实时远端 HEAD。owner 当前 HEAD 为 `ddfa93c8edad9ce1e6c68b3006dce32df9130d1c`，落后该引用；已有 `AGENTS.md` 改动须保留，不在此轮同步、切分支或覆盖。
- 该提交 `Scheduler.cc:57–94` 的默认注册读配置；`Coroutine.hpp:68–73` 仍用 int 大小；`Context.cc:93–103` 丢掉大小及保护参数；`StackPool.cc` 仍单队列，借出数由分配数减近似队列长度得出，检查与借出不是原子操作。
- `Stack.cc` 已页对齐与逐栈保护，但零值/整数溢出、memalign 空返回、mprotect 失败主要靠 assert/日志；必须补运行时错误及清理。`Context.cc:105–116` 的回调先释放、再归还栈顺序保留。
- `GlobalConfig.hpp:26–65`：配置非线程安全、启动前设置；默认 12 KiB；max/min/preload 是全池数量，不是每桶容量或内存字节上限。
- 原先 `Coroutine::Create(4096, ...)` 等大小被忽略，接通后实际生效；所有直接调用及 debug/example 必须核查，重型测试显式给够栈，不能仅重跑旧测试。大小与内部统计类型变化涉及 detail ABI，调用方需重新编译，不保证旧二进制兼容。
- 运行时为 ASIO EventLoop、process-lifetime、单次初始化；C++17，不引入 Stop/restart 或 C++20 协程。依据为该提交 `agent-docs/2026-09-07-core-runtime-contract.md`。
- 原研究及用户路线确认见 `../../tickets/parameter-pool-compatibility.md#resolution`、`../../tickets/minimal-route-confirmation.md#resolution`。codebase 图谱 generation 为 2026-09-17，部分文件 stale/partial；本计划以实际 Git blob 为准。
- revision=2 为首轮独立计划审查 MSR-PR-01～07 的修订：明确释放侧 mprotect 故障测试、诊断宏准确文件落点、Release 拒绝及生产无注入证明、窄 RAII 和单锁性能注释。只细化既定机制与证据，不改变 R/S 需求、范围或出口；首版审查不自动覆盖修订后的候选，相关增量须复审。

## 交付约定
实现、独立审查、PR与main集成已经交付；诊断未齐，不降低原规格、不标done。本图已获用户明确确认留缺口归档，无发布授权。详细施工完成项与缺口见tasks.md；后续补验须明确恢复。

## 当前归档口径
### 合并后交付及留缺口归档（当前事实，优先于下方历史）
用户已批准同runner base/head性能CI简化修复，并明确授权合并；PR #385 已合并到 main `b36f5c4cd4406162c093f4e886ee5cab3915e9c8`。PR最终head `b60860a22c559a9eed5cb12241fb089f25832509` 与merge树一致；main push CI `38225087248`、Docs Check `38225086317` 均success。main CTest首轮57/57、smoke14/reliability23 cases无错误；六模块各45s、2线程base/head真实比较全部PASS，未改正式基线/required/仓库变量。独立实现审查链及7文件CI增量审查GO已完成。

稳定证据：
- https://github.com/yqm-307/bbtools-coroutine/pull/385
- https://github.com/yqm-307/bbtools-coroutine/actions/runs/38225087248
- https://github.com/yqm-307/bbtools-coroutine/actions/runs/38225086317
- https://github.com/yqm-307/bbtools-coroutine/pull/385#issuecomment-6120962390

用户在看到缺口后明确「没事留缺口归档吧」。本次归档是incomplete，不撤销S13/S15诊断要求，不把6-2或完整7-3标为通过；不重开开发/压测、不安装工具、不发布版本。残留：STRINGENT_DEBUG基线shared_from_this编译失败，Valgrind工具/头缺未测；PR阶段Test_hook_fd_epoch首轮FD快照15→14、重试后通过，main首轮通过不代表根因已修；历史Poller首发abort未定位。无自有子代理/构建或CI在途，用户原AGENTS修改保留。以后需要补诊断时必须明确恢复本图并核对应候选与环境。
