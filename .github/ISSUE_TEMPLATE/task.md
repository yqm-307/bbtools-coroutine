---
name: 任务
about: 开发、文档或流程任务
labels: []
---

## 背景

<!-- 为什么要做。关联父 Issue / 契约 / 现有行为。 -->

## 目标

<!-- 做成什么样。 -->

## 范围

**做：**

- （填写）

**不做：**

- （填写）

## 验收标准

- [ ] （填写）
- [ ] （填写）

## 建议验证

对照 `agent-docs/development-and-release-process.md` 验证阶梯勾选，不发明新命令。

- [ ] 仅文档 / 模板 / gitignore：`git diff --check`；抽查链接
- [ ] `scripts/` 或 release_gate：对应 python 测试 / 离线校验
- [ ] 单测 / example：相关 ctest 或该 example
- [ ] 运行时 / 公开 API：全量 ctest
- [ ] Hook / 兼容行为：相关 hook 单测 + 全量 ctest
- [ ] 压测程序本身：短跑或相关 benchmark

## 风险与未覆盖

<!-- 已知缺口。不要把 WARN / 部分压测写成通过。 -->
