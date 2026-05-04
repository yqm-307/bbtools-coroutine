## 1. Hook 收敛

- [x] 1.1 清点 Hook 路径中的宿主环境依赖，优先替换为 `pipe`、自建 listener 与确定性 timeout 场景。
- [x] 1.2 收敛 Hook 的 fd / timeout / retry 行为，使其 failure path 与上下文前置条件符合 `event-hooking` spec。

## 2. CoPool 收敛

- [x] 2.1 收敛 `Submit` 重载的等价外部行为，消除立即唤醒与仅入队之间的语义漂移。
- [x] 2.2 收敛 `Release` 的 drain / shutdown 协议，明确 worker 唤醒、残留任务和完成条件。

## 3. 测试出口

- [x] 3.1 新增 Hook 专项测试，覆盖 fd-ready、timeout、retry 与非 coroutine 线程上下文。
- [x] 3.2 新增 `CoPool` 专项测试，覆盖 `Submit` 等价性、worker 唤醒、monitor 行为与 `Release` shutdown。
- [x] 3.3 确认本阶段测试不再依赖宿主 `ssh` 服务或固定外部环境。

## 4. 衔接

- [x] 4.1 验证阶段 4 的实现产物可以作为 `clean-public-api-and-syntax-surface` 的稳定依赖层。
