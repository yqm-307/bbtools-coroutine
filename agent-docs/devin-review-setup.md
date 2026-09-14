# Devin C++ Core Review — 配置与运维

`/.github/workflows/devin-review.yml` 在 PR opened/reopened/synchronize 时对
`bbt/**/*.{cc,cpp,h,hpp}` 变更调用 Devin reviewer，维护 required check
`Devin C++ Core Review`。审查提示资产：`agent-docs/devin-review-prompt.md`
（review_contract: hermes-repo-review/v1；自上游 team-agent-repo 的
`profiles/reviewer/SOUL.md` 与 `profiles/reviewer/review-contract.md`
迁移合并的仓库内版本化副本，上游演进时需人工同步）；
编排：`scripts/devin_review/devin_review.py`。

## 必须配置的仓库设置

| 类型 | 名称 | 说明 |
|------|------|------|
| Secret | `DEVIN_API_KEY` | Devin API key（Personal/Service）。缺失时 check 以 `BLOCKED: DEVIN_API_KEY secret not configured` 失败。值只进 Settings → Secrets，不进任何文件 |
| Variable | `DEVIN_API_BASE` | 可选，默认 `https://api.devin.ai` |
| Variable | `DEVIN_ORG_ID` | 可选；设置后用 Devin API v3 `/v3/organizations/{org}/sessions`，否则用 v1 `/v1/sessions` |

## Branch protection

main 的 required check 需包含 context `Devin C++ Core Review`（check run 名）。
编排脚本用 `checks: write` 权限创建同名 check run；Actions job 自身的 check
名为 `devin-review-dispatch`，二者不冲突。

## 行为矩阵

| 场景 | check run 结论 |
|------|----------------|
| diff 无 bbt C++ 变更 | success，summary 含 `SKIPPED: no core C++ changes` |
| 审查完成且无 Critical/Important | success + PR review 中文结论 |
| 有 Critical/Important | failure |
| Devin session blocked/errored/expired | failure |
| 轮询超时（默认 1500s） | timed_out |
| PR head 在审查期间移动（旧 head） | cancelled |
| DEVIN_API_KEY 缺失 / fork PR | failure（BLOCKED） |
| check run / PR review 回写失败 | 非零退出，Actions job 失败 |

## 适配点

- Devin API 细节全部封装在 `DevinClient`（创建/轮询/输出提取）。API 变更只改该类。
- verdict 输出提取顺序：`structured_output` → `messages` 倒序扫描 →
  `output`/`result`/`last_message`。找不到合规 JSON 块即非成功，不 mock。
- fork PR 拿不到 secrets → 预期落 BLOCKED；如需覆盖 fork 审查需另行设计
  （例如 workflow_run 两级模型），当前不做。

## 本地验证

```
python3 -m py_compile scripts/devin_review/devin_review.py
python3 scripts/devin_review/devin_review.py --selftest
```
