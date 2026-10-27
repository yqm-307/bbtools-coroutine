# #411 CFS运行时整合验证

本地集成回归与六模块有界稳定性检查通过。它不代表性能无退化或发布资格；最终commit的全量CTest与同runner base/head性能比较由PR CI验证。

## 范围和身份

- main基点：`206772b821ce6bd5ffc3afe547e11d21f5794b5a`。保留该基点的async/await API和测试。
- 本地被测实现为集成工作树源码快照：`0a3bfe7984464b588b6a3b002a471576127c2eeceb6c8f4f2a5f6d34b6c7e05d`（192个源/测试/构建文件）；不可把main基点SHA说成被测优化实现。
- 代码包含此前保留的栈缓存、Chan旧无超时读等待、R4-noformat等待路径和mutex CFS。账目随Coroutine迁移，READY本地回队，通知/park/yield/退役/driver契约保持。
- 新自旋锁、query/batch与libcds均未采纳；同步原语不再扩展。

## 本地结果

- Debug聚焦CTest：51/51，0 failure/error/skipped。覆盖CFS、ready、driver、scaling、cancel/exception，以及coroutine/API/stack/poller/chan/reliability/async。
- Release库和unified_stress实际构建成功，加载库路径与源码/二进制指纹核对。
- 无新增编译警告；Test_chan两条未使用局部变量警告已在同一main基点定向构建复现，不改动无关旧测试。
- 六模块分别45秒、2worker，最终有效指标elapsed为46.0–46.2秒（含既有退出收口）；逐模块无timeout/crash、errors=0、正ops、完整done。18份raw hash和原始最终指标已回放。

|模块|总操作计数|ops/s|指标elapsed_s|errors|
|---|---:|---:|---:|---:|
|comutex|1718449|37357.6|46.0|0|
|corwmutex|1401680|30471.3|46.0|0|
|cocond|22692|492.2|46.1|0|
|chan|859549|18645.3|46.1|0|
|copool|40956|886.5|46.2|0|
|coroutine|22320|483.1|46.2|0|

该表沿用unified_stress既有操作计数口径，不代表所有业务对象/栈已物理回收，也不将提交/活动计数说成端到端完成。

## 已知成本与未采纳结论

此前CFS/R4-noformat/Go三臂短测基于旧冻结源码，持续yield双worker吞吐配对中位+155.08%，短任务双worker吞吐-14.85%、CPU/操作+9.01%、P99+14.16%。用户以公平性和避免饥饿为目标选择保留CFS；这些数值不是本次main集成后的新性能比较，不能声称全场景加速。旧source/harness身份和完整四格变化保存在summary.json。

两个最小query/batch候选没有观察到值得采用的收益；libcds仅探针通过、运行时一致性组合未完成，未测性能；自旋锁信号不普遍，双worker CoMutex短测退化，均不进入实现。

## 升级与边界

旧priority预算字段沿用名称/默认值，已改作CFS相对服务权重；零值不再禁用优先级。内部布局变化需重编本库及直接依赖，详见使用手册和API配置表。

保持协作式执行边界，不强制抢占不yield的业务，不承诺跨P全局精确公平或等待硬上界。无ASan/TSan、跨平台、长soak或完整物理回收验收。

## 证据与失败记录

原始命令、CTest XML、源码清单、实际加载库/ldd及raw留本地忽略的`build-411-final-delivery/`。只归档本summary.md和summary.json；不提交raw、日志、构建树或会话状态。

首轮收集器被两个main已有警告提前挡住；main定向编译后确认其来源。第二轮51项CTest和构建全部通过，但尾部收据误用了旧实验core/lib路径；已按实际CMake的lib目录及ldd/hash补核，不靠重跑测试或负载凑绿。
