---
label: wayfinder:grilling
blocked_by: [parameter-pool-compatibility]
assignee:
claim_session:
status: closed
outcome: resolved
archive:
---
# 确认最小路线与验收出口

## Question
兼容性事实明确后，是否以一个路径点完成多尺寸栈的入口接通、按尺寸复用及验证，且不扩展到 CoPool 分流、伙伴分配器或自动扩栈？

## Done when
- 依据前置核查结论，向用户展示覆盖整个 Destination 的最小路线、默认/显式与对齐语义、不包含事项及真实验证出口，不重问已经确定的目标。
- 获得用户一次整图路线确认；按 WFER 规则整理相应 draft waypoint 骨架并记录确认依据。
- 明确路线确认不等于允许实现、提交、发布或合并；本轮持续 explore，后续授权按实际用户意图核对。

## Checkpoint
- 本票已解决，释放认领；用户确认的是覆盖整个 Destination 的单路径点路线及 draft 骨架，不是完整计划或实现。
- 确认依据：本会话向用户展示入口传递、页对齐及保护、按实际属性复用、容量/安全、兼容与验证范围，随后用户在确认工具中选择「确认路线，仅建立 draft 路径点」。
- `multi-size-stack-runtime` 的 proposal.md、specs.md、design.md、tasks.md 均已通过 CLI 创建并回读；revision=1，包状态 draft，尚无实施任务完成记录。
- 路线已写入 MAP：route_status=confirmed、route_revision=1、route_waypoints=[multi-size-stack-runtime]。本轮只维护 WFER 工件与 scratch，未改运行时源码、未测试 C++、未提交或写远端。
- 无在途子代理、构建或后台运行；下一动作需用户明确选择，建议 `/wfer-continue plan`。不因本票关闭就自动规划、实施或归档整图。

## Resolution
- 用户明确确认了一个完整路径点 `multi-size-stack-runtime`，覆盖默认/显式 C++ 入口、Coroutine→Context→StackPool 大小传递、页对齐、保护属性匹配、统一容量与安全回收，以及直接兼容性和真实验证出口；不拆成调查/编码/测试阶段点。
- 边界沿已展示路线：不做伙伴分配器、自动扩栈、CoPool 改造、新调度器，不修改全局默认栈大小。路线确认不等于实现、提交、发布或合并许可。
- 本地验证为冒烟、功能和直接耦合测试；全量 CTest 按当前本地仓规交 CI，正式计划时核清实施基线与流程文档差异。必要独立审查及未验证的诊断/性能边界保留，不以结构校验冒充运行时测试。
- 本次仅建立 `../waypoint/multi-size-stack-runtime/` 下 canonical draft 四文件骨架，未细化成 ready 计划、未打勾实施任务；每份均已通过 CLI 回读确认身份与 revision=1。
- 本轮 explore 的整图路线确认出口已达到；运行时目的地尚未达成，地图保持 active，不自动归档。后续计划或实施等待新指令。
