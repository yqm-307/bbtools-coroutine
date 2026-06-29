# Chan Go 对齐 — 性能对照

> 2026-06-29 | #207 (原 #188) | Chan 异常安全 + Close 语义修正 + Notify race fix

## 改动

1. **RAII 重构**: `_Lock()/_UnLock()` → `std::unique_lock`, `m_is_reading` ChanReadGuard
2. **Close 语义**: 不再丢弃缓冲数据，读空后才返回 -1
3. **返回值区分**: 0=成功, -1=关闭+空, -2=错误, 1=超时
4. **新增**: `size()/capacity()`, `WriteChan<T>/ReadChan<T>`
5. **YieldWithCallback 修复** (b0ae2c8): callback 在 Resume() 中执行导致 `unique_lock` 拥有权冲突 → 改为手动 unlock
6. **Notify 丢失安全网** (500ms): CAS 后 Notify 可能丢失 → `_WaitUntilEnableReadOrTimeout(500)` 兜底
7. **Review 修复**: `size()` 加锁, `TryRead` 超时递减, 文档补全

## 测试条件

- `unified_stress --module=chan --threads=2, FATIGUE_INTERVAL=10, timeout -s KILL 25`
- PRE: `10594ed` (origin/main before #188)
- MID: `44542e3` (纯 RAII, 不含 b0ae2c8 和安全网)
- POST: `1809187+` (最终版本: CAS-before-lock + 500ms 网 + b0ae2c8 fix)

## 结果 (各 3 轮快测)

| 版本 | R1 | R2 | R3 | **平均** |
|------|-----|-----|-----|----------|
| PRE | 839 | 847 | 854 | **846/s** |
| MID | 855 | 827 | 859 | **847/s** (+0.0%) |
| POST | 745 | 757 | 784 | **762/s** (-10.0%) |

### 代价拆解

- RAII + Close drain: **+0.0%**（零开销）
- b0ae2c8 lock fix + 500ms 安全网 + TryRead fix + size lock: **-10.0%**

## 1h 疲劳测试

- 6/6 模块线性增长，零冻结，零 crash
- chan: 2,465,475 ops @ 679/s (2T)
- 全通道 16.4M ops, exit=0

## Close 语义验证

```
--module=chan_close: 400 条填充 → Close → 读完 400 条 → Read 返回 -1 ✅
```

## 结论

-10% 来自两个不可省略的竞态修复 (b0ae2c8 + 安全网)，不是 RAII 本身。换来的收益：
- 全路径异常安全（不会锁泄露）
- `m_is_reading` 异常自动恢复
- Close 后仍可读缓冲数据（对齐 Go）
- 编译期方向性约束
- 无死锁（Resource deadlock avoided）
- 无永久阻塞（Notify 丢失兜底）
