#!/usr/bin/env python3
"""result job 的本仓内联汇总：把 needs.*.result 汇总为唯一 required check 判定。

替代已删除的共享 result callee（CI 不再引用 classify/result callee 链）。
只消费调用方以 JSON 传入的 needs 结果映射与必跑/可选/预期跳过集合；不读
GitHub 上下文、不执行任何命令。fail-closed 语义：

- `required` 集合必须非空（空即拒绝，防「全跳过变绿」）；
- 任一 required 为 failure / cancelled / 空值 / skipped → verdict=failure；
- results 出现 required∪optional 之外的 job 或非法状态值 → 拒绝；
- optional 非 success 时：skipped/缺失仅当该 job 被列入 `expected_skips`
  （由调用方按 job 级触发条件显式声明）才放行，其余一律判 failure；
- `expected_skips` 必须是 optional 的子集（不得给 required 或未知 job 放行）；
- 无任何真实 success 不得判绿。

输入（JSON，stdin 或 --file）：
  {"results": {"<job>": "success|failure|cancelled|skipped", ...},
   "required": ["<job>", ...],
   "optional": ["<job>", ...],             # 可省，默认 []
   "expected_skips": ["<job>", ...]}       # 可省，默认 []（不放行任何跳过）

输出（stdout 一行 JSON；并写 $GITHUB_OUTPUT，若在环境提供）：
  {"ok": true, "verdict": "success|failure", "failed": [...], "skipped": [...]}

退出码：0 verdict=success；1 verdict=failure；3 输入契约拒绝（非 JSON/缺键/
非法值，输出 {"ok": false, "code": ...}，不回显被拒输入值）。
"""
from __future__ import annotations

import json
import os
import sys

VALID_STATUSES = {"success", "failure", "cancelled", "skipped"}


class ContractError(Exception):
    def __init__(self, code):
        super().__init__(code)
        self.code = code


def _load(stream):
    try:
        payload = json.load(stream)
    except ValueError:
        raise ContractError("E_INPUT_JSON")
    if not isinstance(payload, dict):
        raise ContractError("E_INPUT_TYPE")
    for key in ("results", "required"):
        if key not in payload:
            raise ContractError("E_INPUT_MISSING")
    return payload


def evaluate(results, required, optional, expected_skips) -> dict:
    """返回 {verdict, failed, ran, skipped}；任何必跑非 success 即 failure。"""
    if not isinstance(results, dict) or not results:
        raise ContractError("E_RESULT_EMPTY")
    if not isinstance(required, list) or not required:
        raise ContractError("E_REQUIRED_EMPTY")
    if not isinstance(optional, list):
        raise ContractError("E_INPUT_TYPE")
    if not isinstance(expected_skips, list):
        raise ContractError("E_INPUT_TYPE")
    if set(required) & set(optional):
        raise ContractError("E_CHECK_OVERLAP")
    if not set(expected_skips) <= set(optional):
        raise ContractError("E_SKIP_NOT_OPTIONAL")

    allowed = set(required) | set(optional)
    unknown = set(results) - allowed
    if unknown:
        raise ContractError("E_RESULT_UNKNOWN_JOB")
    for status in results.values():
        if status not in VALID_STATUSES:
            raise ContractError("E_RESULT_STATUS")

    skip_ok = set(expected_skips)
    failed = []
    for job in required:
        if results.get(job, "") != "success":
            failed.append(job)
    for job in optional:
        status = results.get(job, "")
        if status == "success":
            continue
        if status in ("skipped", "") and job in skip_ok:
            continue
        failed.append(job)  # failure / cancelled / 未声明预期跳过的 skipped

    # 防御性不变量：无任何真实 success 不得判绿（即使调用方构造空 required 计划）。
    if not failed and not any(results.get(j, "") == "success" for j in allowed):
        raise ContractError("E_ALL_SKIPPED")

    return {
        "verdict": "success" if not failed else "failure",
        "failed": failed,
        "ran": [j for j in allowed if results.get(j, "") in ("success", "failure")],
        "skipped": [j for j in allowed if results.get(j, "") in ("skipped", "")],
    }


def main(argv=None) -> int:
    import argparse

    parser = argparse.ArgumentParser(prog="result.py")
    parser.add_argument("--file", default="-", help="输入 JSON 文件；默认 stdin")
    args = parser.parse_args(argv)

    try:
        if args.file in (None, "-"):
            payload = _load(sys.stdin)
        else:
            try:
                with open(args.file, "r", encoding="utf-8") as handle:
                    payload = _load(handle)
            except (OSError, UnicodeDecodeError):
                raise ContractError("E_INPUT_JSON")
        out = evaluate(
            payload["results"],
            payload["required"],
            payload.get("optional", []),
            payload.get("expected_skips", []),
        )
    except ContractError as err:
        print(json.dumps({"ok": False, "code": err.code}, ensure_ascii=False,
                         sort_keys=True))
        return 3

    result = {"ok": True, **out}
    print(json.dumps(result, ensure_ascii=False, sort_keys=True))
    github_output = os.environ.get("GITHUB_OUTPUT")
    if github_output:
        with open(github_output, "a", encoding="utf-8") as handle:
            handle.write("verdict=" + out["verdict"] + "\n")
            handle.write("result_json=" + json.dumps(result, sort_keys=True) + "\n")
    return 0 if out["verdict"] == "success" else 1


if __name__ == "__main__":
    sys.exit(main())
