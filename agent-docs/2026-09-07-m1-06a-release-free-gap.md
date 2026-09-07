# M1-06a Release 构建资源释放：实现与契约差异

契约真源：`agent-docs/2026-09-07-core-runtime-contract.md`
Issue：#279

## 根因

`Stack::_Release()` 原为 `assert(Free(m_mem_chunk, m_mem_chunk_size) == 0)`。
NDEBUG（Release 构建）下 assert 整体被编译掉，`Free()`（含 `free()`）不执行——
每块协程栈内存泄漏，且 `m_mem_chunk` 不置空。全仓 grep 确认这是唯一一处
把副作用塞进 assert 的释放语句。

## 修复

- 释放改为实际执行语句：`int ret = Free(...)`，随后置空 `m_mem_chunk/
  m_mem_chunk_size/m_useable_size` 回收到空态，最后 `assert(ret == 0)` 保留
  Debug 契约检查。
- 语义：`Clear()` 与析构共用 `_Release()`，现在幂等（重复调用不二次 free），
  释放后对象回到空态（`MemChunkBegin()`/`StackTop()` 返回 nullptr）。

## 验证

- 新增 `t_stack_clear_releases_and_is_idempotent`：Clear 后指针归零 + 二次 Clear
  不崩。Release 树（build/CMAKE_BUILD_TYPE=Release，即 NDEBUG）修复前 RED
  （MemChunkBegin 非空），修复后 GREEN。该探针直接锁 bug：不真正 free 就
  不可能安全置 nullptr。
- 全量 ctest 21/21 通过（-j2，无卡死）。

## 已知限制

- RSS 泄漏探针在 `stack_protect=false` 下无效：memalign 的栈内存从不被写入，
  只是虚拟预留、不驻留 RSS，free 与否都测不出增长；touch 它会占满内存违反
  机器内存约束。故未采用 RSS 方案，改由 idempotent 指针探针锁住。
- 构造器中 oom 路径 `assert(m_mem_chunk != nullptr)` 无副作用，不属本 Issue
  范围（Release 下 alloc 失败不崩，但后续会走空指针，属另一类问题）。
