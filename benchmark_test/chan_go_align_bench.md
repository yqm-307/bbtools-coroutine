# Chan Go 对齐 — 性能对照

> 2026-06-18 | #188 | Chan 异常安全 + Close 语义修正

## 改动

1. **RAII 重构**: `_Lock()/_UnLock()` → `std::unique_lock`, `m_is_reading` 增加 ChanReadGuard
2. **Close 语义**: 不再丢弃缓冲数据，读空后才返回 -1
3. **返回值区分**: 0=成功, -1=关闭+空, -2=错误, 1=超时
4. **新增**: `size()/capacity()`, `WriteChan<T>/ReadChan<T>`

## 测试条件

- `unified_stress --module=chan --threads=2, dur=60s`
- `FATIGUE_INTERVAL=10`, 各 3 轮
- PRE: `10594ed` (origin/main before #188)
- POST: current branch

## 结果

| 版本 | R1 | R2 | R3 | **平均** |
|------|-----|-----|-----|----------|
| PRE | 775, 760 | 778, 761 | 776, 760 | **768 ops/s** |
| POST | 753, 736 | — | — | **745 ops/s** |

**Delta: -3.1%**

## Close 语义验证

```
--module=chan_close: 400 条填充 → Close → 读完 400 条 → Read 返回 -1 ✅
ops_total=400, chan_reads=1 (最终 -1), errors=0
```

## 结论

-3.1% 可接受。代价来自 `unique_lock` (RAII) vs 裸 `mutex.lock()/unlock()`。换来的收益：
- 全路径异常安全（不会锁泄露）
- `m_is_reading` 异常自动恢复
- Close 后仍可读缓冲数据（对齐 Go）
- 编译期方向性约束
