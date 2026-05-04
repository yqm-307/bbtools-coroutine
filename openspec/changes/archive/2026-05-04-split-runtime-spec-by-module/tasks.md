## 1. 规范母版基线

- [x] 1.1 复核当前 runtime 源码布局，确认每个 capability 都能映射到稳定的职责边界。
- [x] 1.2 为 `runtime-scheduler`、`coroutine-lifecycle`、`event-hooking`、`sync-primitives`、`coroutine-pool-and-api` 和 `runtime-test-matrix` 建立长期维护归属。
- [x] 1.3 验证未来 runtime 变更能够引用一个主 capability，并仅在必要时附加跨模块依赖。

## 2. 路线图收敛

- [x] 2.1 将阶段 1～5 的内容明确为推荐拆分顺序与依赖关系，而不是在本 change 内顺序实现的编码任务。
- [x] 2.2 记录阶段 1～4 已通过独立 change 拆分推进，本 change 不再重复拥有这些实现任务。
- [x] 2.3 保留 `clean-public-api-and-syntax-surface` 作为后续独立 change 的推荐入口，而不是继续在本 change 中堆积实现待办。

## 3. 测试矩阵与后续流程

- [x] 3.1 明确后续 runtime 实现 change 需要引用对应 capability spec，并满足 `runtime-test-matrix` 的测试准入标准。
- [x] 3.2 明确当实现与当前 specs 存在实质偏差时，应新建或更新独立 change，而不是直接在本 change 中混入实现修补。
