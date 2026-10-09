#!/usr/bin/env python3
"""perf_report_ingest.py — 消费 server3 可信控制端发布的性能报告（fail-closed）。

职责边界（信任边界）：
  本脚本是 **消费端**，运行在 hosted runner 上，只用标准库 `json` / `urllib` / `hashlib`：
    - 不执行、不 eval/exec 评论内容；评论正文只作 `json.loads`；
    - 不运行 unified_stress、不编译、不读 perf-baseline 工作树、不写仓库；
    - 不携带任何 server3 凭据；只用 CI 只读 token（`GITHUB_TOKEN`，contents: read）。
  数据来源：本仓绑定 source SHA 的 **commit comment**（
  `GET /repos/{repo}/commits/{sha}/comments`），评论正文含固定模板标记
  `bbtools-server3-perf/v1` 与一段 ```json 报告。

信任判定（全部为硬条件，任一不满足 → 非零，绝不置绿）：
  1. 评论 `author.id` 必须等于固定拥有者 API id（exact 整数比较，绝不比 login；
     同名/伪装评论 **忽略**，不因此拒绝整个 thread）；
  2. 报告 `repo` / `source_sha` 必须与本次 subject 完全一致（完整 40-hex）；
  3. `baseline.ref` 必须是已批准 ref（默认 `refs/heads/perf-baseline`）；
  4. 取 `baseline.commit:baseline.path` 的文件字节，sha256 必须等于 `baseline.sha256`；
  5. 用 baseline 的真实 JSON 独立复算：环境指纹必须可比
     （FINGERPRINT_KEYS 任一不一致 → NO_COMPARABLE_BASELINE，不置绿），
     六个模块必须齐全、`errors == 0`、`elapsed_s > 0`、吞吐退化必须与
     声明的 `verdict` 一致（声明不得比实测更轻，`severity(declared) >= severity(derived)`）；
  6. `measurement_sha256` 必须等于报告规范序列化（sort_keys，紧凑分隔符）的 sha256。

成功语义：只有 `verdict ∈ {PASS, WARN}` 且上述全部通过才 exit 0（WARN 走显式注解，
沿用既有 perf_contract：10% WARN / 20% FAIL、CoCond 30%/40%、gate-enabled 语义）。
其余（缺报告 / 身份不符 / 摘要不符 / 环境不可比 / METRIC_INVALID / FAIL / 未知）→ exit 2。

本文件为独立固定版本：不含 `perf_contract` 阈值/指纹键的复制真源，但通过
`scripts/ci/test_perf_report_ingest.py::ContractDriftGuardTest` 断言本文件内联常量
与 `perf_contract.py` 真源一致，防止阈值静默漂移。
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
import os
import re
import sys
import time
import urllib.error
import urllib.request

MARKER = "bbtools-server3-perf/v1"
SCHEMA_VERSION = 1

# 判定闭集（与 perf_contract.VALID_VERDICTS 一致，由 drift guard 断言）。
VALID_VERDICTS = {
    "PASS", "WARN", "FAIL", "UNSTABLE", "NO_COMPARABLE_BASELINE", "METRIC_INVALID",
}
SUCCESS_VERDICTS = {"PASS", "WARN"}
SEVERITY = {"FAIL": 5, "METRIC_INVALID": 4, "UNSTABLE": 3,
            "WARN": 2, "NO_COMPARABLE_BASELINE": 1, "PASS": 0}

# 六个必须模块（顺序无关，集合必须精确相等）。
MODULES = ("comutex", "corwmutex", "cocond", "chan", "copool", "coroutine")
REQUIRED_MODULE_FIELDS = ("ops_total", "ops_per_sec", "errors", "elapsed_s")

# 环境指纹比较键（与 perf_contract.FINGERPRINT_KEYS 一致，由 drift guard 断言）。
FINGERPRINT_KEYS = (
    "agent", "node", "cpu_model", "logical_cores", "mem_total_kb",
    "compiler", "cmake_version", "ninja_version", "build_type",
    "cmake_args", "threads",
)

# 阈值（与 perf_contract 一致，由 drift guard 断言）。
THRESHOLD_WARN = 10
THRESHOLD_FAIL = 20
THRESHOLD_COCOND = 30
THRESHOLD_COCOND_FAIL = 40

# 有界读取上限：防超大评论正文 / 无限分页 / 失控样本量。
MAX_COMMENT_BYTES = 256 * 1024
MAX_TOTAL_COMMENTS = 500
MAX_PAGES = 5
MAX_BASELINE_BYTES = 1024 * 1024
HTTP_TIMEOUT = 30
SHA40_RE = re.compile(r"^[0-9a-f]{40}$")
SHA256_RE = re.compile(r"^[0-9a-f]{64}$")

DEFAULT_BASELINE_REF = "refs/heads/perf-baseline"
BASELINE_PATH_PREFIX = "tests/baselines/"


class IngestError(Exception):
    """带闭集 reason 的失败：调用方据 reason 决定退出码与注解。"""

    def __init__(self, status: str, reason: str):
        super().__init__(f"{status}: {reason}")
        self.status = status
        self.reason = reason


def canonical_bytes(obj) -> bytes:
    """规范序列化：sort_keys + 紧凑分隔符 + UTF-8（measurement_sha256 基准）。"""
    return json.dumps(obj, sort_keys=True, separators=(",", ":"),
                      ensure_ascii=False).encode("utf-8")


def http_get_json(url: str, token: str, *, bounded_bytes: int, page: int | None = None):
    """GET 一个 JSON 端点，返回 (parsed, next_page)。

    只读、超时、响应体有界；不做任何写。token 为空则匿名（公开仓只读）。
    """
    headers = {"Accept": "application/vnd.github+json",
               "X-GitHub-Api-Version": "2022-11-28",
               "User-Agent": "bbtools-perf-ingest/1"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    request = urllib.request.Request(url, headers=headers, method="GET")
    try:
        with urllib.request.urlopen(request, timeout=HTTP_TIMEOUT) as response:
            raw = response.read(bounded_bytes + 1)
            link = response.headers.get("Link", "")
    except urllib.error.HTTPError as exc:
        raise IngestError("REPORT_MISSING", f"http {exc.code} for {url}") from exc
    except (urllib.error.URLError, TimeoutError, OSError) as exc:
        raise IngestError("REPORT_MISSING", f"http unavailable: {exc}") from exc
    if len(raw) > bounded_bytes:
        raise IngestError("REPORT_MALFORMED", f"response exceeds {bounded_bytes} bytes")
    try:
        parsed = json.loads(raw.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise IngestError("REPORT_MALFORMED", f"invalid JSON: {exc}") from exc
    next_page = None
    if page is not None and 'rel="next"' in link:
        next_page = page + 1
    return parsed, next_page


def fetch_comments(repo: str, sha: str, token: str) -> list[dict]:
    """读取绑定 sha 的 commit comments，分页有界、总量有界、单条正文有界。"""
    comments: list[dict] = []
    page = 1
    while page <= MAX_PAGES:
        url = (f"https://api.github.com/repos/{repo}/commits/{sha}/comments"
               f"?per_page=100&page={page}")
        batch, next_page = http_get_json(url, token, bounded_bytes=MAX_COMMENT_BYTES * 200,
                                         page=page)
        if not isinstance(batch, list):
            raise IngestError("REPORT_MALFORMED", "comments response is not a list")
        for entry in batch:
            body = entry.get("body") if isinstance(entry, dict) else None
            if isinstance(body, str) and len(body.encode("utf-8")) > MAX_COMMENT_BYTES:
                raise IngestError("REPORT_MALFORMED",
                                  f"comment body exceeds {MAX_COMMENT_BYTES} bytes")
        comments.extend(batch)
        if len(comments) > MAX_TOTAL_COMMENTS:
            raise IngestError("REPORT_MALFORMED",
                              f"comment count exceeds {MAX_TOTAL_COMMENTS}")
        if next_page is None:
            return comments
        page = next_page
    # 第 MAX_PAGES 页后仍返回 next link：分页被截断，不能信任（有界失败，
    # 防止用 >MAX_PAGES 页把冲突评论挤出可见范围）。
    raise IngestError("REPORT_MALFORMED",
                      f"comment pagination exceeds {MAX_PAGES} pages")


def fetch_baseline_bytes(repo: str, path: str, ref: str, token: str) -> bytes:
    """按 commit 绑定读取 baseline 文件字节（contents API，base64）。

    GitHub Contents API 的 `content` 每 60 字符插入换行，故先剥离全部 ASCII 空白
    再以 `validate=True` 严格解码（仍只接受合法 base64 字母表）；字节 sha256 与
    `report.baseline.sha256` 的比较继续作为完整性闸门。
    """
    url = f"https://api.github.com/repos/{repo}/contents/{path}?ref={ref}"
    payload, _ = http_get_json(url, token, bounded_bytes=MAX_BASELINE_BYTES * 2)
    if not isinstance(payload, dict) or payload.get("encoding") != "base64":
        raise IngestError("NO_COMPARABLE_BASELINE", "baseline content not base64")
    content = payload.get("content")
    if not isinstance(content, str):
        raise IngestError("NO_COMPARABLE_BASELINE", "baseline content missing/not text")
    try:
        raw = base64.b64decode("".join(content.split()), validate=True)
    except (ValueError, TypeError) as exc:
        raise IngestError("NO_COMPARABLE_BASELINE", f"baseline decode failed: {exc}") from exc
    if len(raw) > MAX_BASELINE_BYTES:
        raise IngestError("NO_COMPARABLE_BASELINE", "baseline file too large")
    return raw


def extract_report(body: str):
    """从评论正文提取报告 dict；无标记 / 无 JSON 段 → None（忽略该评论）。"""
    if MARKER not in body:
        return None
    text = body
    fence = re.search(r"```json\s*(\{.*?\})\s*```", body, re.DOTALL)
    if fence:
        text = fence.group(1)
    else:
        start, end = body.find("{"), body.rfind("}")
        if start < 0 or end <= start:
            return None
        text = body[start:end + 1]
    try:
        parsed = json.loads(text)
    except json.JSONDecodeError:
        return None
    return parsed if isinstance(parsed, dict) else None


def _require(cond: bool, status: str, reason: str) -> None:
    if not cond:
        raise IngestError(status, reason)


def metric_value_ok(value) -> bool:
    """模块吞吐/时长字段的合法性：bool 之外的非负有限数。

    与真源 perf_contract.normalize_module_metrics（math.isfinite、拒 bool/NaN/inf/负值）
    同判，由 ContractDriftGuardTest 断言一致，防止消费端闸门静默弱于契约。
    """
    return (isinstance(value, (int, float)) and not isinstance(value, bool)
            and math.isfinite(value) and value >= 0)


def select_candidate(comments: list[dict], repo: str, source_sha: str,
                     expected_author_id: int):
    """过滤出受控报告候选；伪装/身份不符的评论被忽略（不 fail 整个 thread）。"""
    matches = []
    for comment in comments:
        user = comment.get("user") or {}
        # 身份用 API id（整数）exact 比较，不接受 login 同名冒充。
        if user.get("id") != expected_author_id:
            continue
        report = extract_report(comment.get("body") or "")
        if report is None:
            continue
        if report.get("repo") != repo or report.get("source_sha") != source_sha:
            continue
        digest = hashlib.sha256(canonical_bytes(
            {k: v for k, v in report.items() if k != "measurement_sha256"})).hexdigest()
        matches.append((digest, report, comment.get("id")))
    if not matches:
        return None
    digests = {d for d, _, _ in matches}
    if len(digests) > 1:
        raise IngestError("CONFLICTING_REPORTS",
                          f"{len(digests)} distinct reports for same source SHA")
    return matches[0][1]


def validate_report(report: dict, repo: str, source_sha: str,
                    expected_baseline_ref: str, expected_baseline_commit: str):
    """报告结构 + 摘要完整性（schema 闭集、字段类型、measurement_sha256）。"""
    _require(report.get("schema") == MARKER, "REPORT_MALFORMED", "marker mismatch")
    _require(report.get("schema_version") == SCHEMA_VERSION, "REPORT_MALFORMED",
             "schema_version mismatch")
    _require(report.get("repo") == repo, "REPORT_MALFORMED", "repo mismatch")
    _require(report.get("source_sha") == source_sha, "REPORT_MALFORMED",
             "source_sha mismatch")

    declared = report.get("measurement_sha256")
    _require(isinstance(declared, str) and SHA256_RE.match(declared) is not None,
             "REPORT_MALFORMED", "measurement_sha256 missing/invalid")
    computed = hashlib.sha256(canonical_bytes(
        {k: v for k, v in report.items()
         if k != "measurement_sha256"})).hexdigest()
    _require(computed == declared, "REPORT_MALFORMED",
             "measurement_sha256 mismatch (tampered/edited report)")

    baseline = report.get("baseline")
    _require(isinstance(baseline, dict), "NO_COMPARABLE_BASELINE", "baseline missing")
    _require(baseline.get("ref") == expected_baseline_ref,
             "NO_COMPARABLE_BASELINE", "baseline.ref not the approved ref")
    commit = baseline.get("commit")
    _require(isinstance(commit, str) and SHA40_RE.match(commit) is not None,
             "NO_COMPARABLE_BASELINE", "baseline.commit not a full 40-hex SHA")
    if expected_baseline_commit:
        _require(commit == expected_baseline_commit, "NO_COMPARABLE_BASELINE",
                 "baseline.commit not the pinned approved commit")
    path = baseline.get("path")
    _require(isinstance(path, str) and path.startswith(BASELINE_PATH_PREFIX)
             and ".." not in path and not path.startswith("/") and "\\" not in path,
             "NO_COMPARABLE_BASELINE", "baseline.path outside approved prefix")
    _require(isinstance(baseline.get("sha256"), str)
             and SHA256_RE.match(baseline["sha256"]) is not None,
             "NO_COMPARABLE_BASELINE", "baseline.sha256 invalid")

    modules = report.get("modules")
    _require(isinstance(modules, dict) and set(modules) == set(MODULES),
             "METRIC_INVALID", "modules must be exactly the six modules")
    for name, metrics in modules.items():
        _require(isinstance(metrics, dict), "METRIC_INVALID", f"{name} not an object")
        for field in REQUIRED_MODULE_FIELDS:
            _require(field in metrics, "METRIC_INVALID", f"{name}.{field} missing")
        _require(metrics["errors"] == 0, "METRIC_INVALID",
                 f"{name}.errors != 0")
        for field in ("ops_total", "ops_per_sec", "elapsed_s"):
            value = metrics[field]
            _require(metric_value_ok(value),
                     "METRIC_INVALID", f"{name}.{field} not a finite number")
        _require(metrics["elapsed_s"] > 0, "METRIC_INVALID", f"{name}.elapsed_s <= 0")

    _require(report.get("verdict") in VALID_VERDICTS, "METRIC_INVALID",
             "verdict outside closed set")
    environment = report.get("environment")
    _require(isinstance(environment, dict), "NO_COMPARABLE_BASELINE",
             "environment missing")
    return report


def compare_environment(old_env: dict, new_env: dict):
    """内联环境指纹比较（语义同 perf_contract.compare_environment）。"""
    if not old_env or not new_env:
        return "NO_COMPARABLE_BASELINE", "missing_environment"
    diffs = [k for k in FINGERPRINT_KEYS if old_env.get(k) != new_env.get(k)]
    if diffs:
        return "NO_COMPARABLE_BASELINE", "environment_mismatch:" + ",".join(diffs)
    return "PASS", "ok"


def derive_module_status(old_ops, new_ops, module: str, gate_enabled: bool):
    """内联吞吐判定（语义同 perf_contract.compare_module），返回 (status, delta_pct)。"""
    old_ops = float(old_ops or 0)
    new_ops = float(new_ops or 0)
    if old_ops <= 0:
        return "NO_COMPARABLE_BASELINE", None
    delta = (new_ops - old_ops) / old_ops * 100
    threshold = THRESHOLD_COCOND if module == "cocond" else THRESHOLD_WARN
    escalate = THRESHOLD_COCOND_FAIL if module == "cocond" else THRESHOLD_FAIL
    if delta <= -escalate:
        status = "FAIL" if gate_enabled else "WARN"
    elif delta <= -threshold:
        status = "WARN"
    else:
        status = "PASS"
    return status, round(delta, 3)


def cross_check(report: dict, baseline: dict, gate_enabled: bool):
    """用基线真实 JSON 独立复算，返回 (derived_verdict, per_module, env_status)。

    环境不可比 → NO_COMPARABLE_BASELINE；模块缺基线或基线无效 → NO_COMPARABLE_BASELINE；
    其余按阈值门控。声明 verdict 不得比实测更轻（在 verify() 中强制）。
    """
    env_status, env_reason = compare_environment(
        (baseline.get("environment") or {}), report.get("environment") or {})
    if env_status != "PASS":
        return "NO_COMPARABLE_BASELINE", {}, env_reason
    baseline_modules = baseline.get("modules") or {}
    per_module = {}
    worst = "PASS"
    for name in MODULES:
        old_mod = baseline_modules.get(name)
        if not isinstance(old_mod, dict) or old_mod.get("error"):
            return "NO_COMPARABLE_BASELINE", per_module, f"baseline_module_missing:{name}"
        old_ops = old_mod.get("ops_per_sec")
        new_ops = report["modules"][name].get("ops_per_sec")
        if not isinstance(old_ops, (int, float)) or isinstance(old_ops, bool) or old_ops <= 0:
            return "NO_COMPARABLE_BASELINE", per_module, f"baseline_zero_ops:{name}"
        status, delta = derive_module_status(old_ops, new_ops, name, gate_enabled)
        per_module[name] = {"status": status, "delta_pct": delta, "old_ops": old_ops}
        if SEVERITY[status] > SEVERITY[worst]:
            worst = status
    return worst, per_module, "ok"


def verify(report: dict, baseline_bytes: bytes, repo: str, source_sha: str,
           expected_author_id: int, expected_baseline_ref: str,
           expected_baseline_commit: str, gate_enabled: bool) -> dict:
    """完整判定链；返回规范化结果 dict，失败抛 IngestError。"""
    validate_report(report, repo, source_sha, expected_baseline_ref,
                    expected_baseline_commit)

    baseline_sha = hashlib.sha256(baseline_bytes).hexdigest()
    declared_sha = report["baseline"]["sha256"]
    _require(baseline_sha == declared_sha, "NO_COMPARABLE_BASELINE",
             "baseline file sha256 mismatch (bytes != report.baseline.sha256)")
    try:
        baseline = json.loads(baseline_bytes.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise IngestError("NO_COMPARABLE_BASELINE", f"baseline JSON invalid: {exc}") from exc
    _require(isinstance(baseline, dict) and isinstance(baseline.get("modules"), dict),
             "NO_COMPARABLE_BASELINE", "baseline schema invalid")

    derived, per_module, reason = cross_check(report, baseline, gate_enabled)
    declared = report["verdict"]
    if derived == "NO_COMPARABLE_BASELINE":
        raise IngestError("NO_COMPARABLE_BASELINE", f"independent compare: {reason}")
    # 声明不得比实测更轻：控制器可因延迟/冻结等额外检查加重，但绝不能减轻。
    _require(SEVERITY[declared] >= SEVERITY[derived], "METRIC_INVALID",
             f"declared verdict {declared} lighter than derived {derived}")
    _require(declared in SUCCESS_VERDICTS, declared,
             f"declared verdict {declared} is not a success verdict")

    return {
        "status": declared,
        "source_sha": source_sha,
        "author_id": expected_author_id,
        "baseline_commit": report["baseline"]["commit"],
        "baseline_sha256": declared_sha,
        "measurement_sha256": report["measurement_sha256"],
        "derived_verdict": derived,
        "modules": {name: {"status": report["modules"][name].get("status", "PASS"),
                           **per_module.get(name, {})} for name in MODULES},
    }


def emit_summary(result: dict) -> None:
    lines = ["## 性能回归检查（server3 受控报告）", "",
             f"- source_sha: `{result['source_sha']}`",
             f"- controller author_id: {result['author_id']}",
             f"- baseline commit: `{result['baseline_commit']}`",
             f"- measurement_sha256: `{result['measurement_sha256']}`",
             f"- verdict: **{result['status']}** (independent derived: {result['derived_verdict']})",
             "", "| Module | Status | Delta |", "|---|---|---|"]
    for name, info in result["modules"].items():
        delta = info.get("delta_pct")
        delta_str = f"{delta:+.1f}%" if isinstance(delta, (int, float)) else "N/A"
        lines.append(f"| {name} | {info['status']} | {delta_str} |")
    path = os.environ.get("GITHUB_STEP_SUMMARY")
    if path:
        try:
            with open(path, "a") as handle:
                handle.write("\n".join(lines) + "\n")
        except OSError:
            pass
    if result["status"] == "WARN":
        print("::warning::性能回归检查 WARN（受控报告，按既有契约不阻塞，需人工复核）")
    else:
        print("::notice::性能回归检查 PASS（受控报告 + 基线独立复算一致）")


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description="Ingest trusted server3 perf report")
    parser.add_argument("--repo", required=True, help="owner/repository")
    parser.add_argument("--source-sha", required=True,
                        help="PR head.sha / push github.sha（完整 40-hex）")
    parser.add_argument("--expected-author-id", required=True,
                        help="受控报告作者的固定 GitHub API user id（整数）")
    parser.add_argument("--expected-baseline-ref", default=DEFAULT_BASELINE_REF)
    parser.add_argument("--expected-baseline-commit", default="")
    parser.add_argument("--gate-enabled", dest="gate_enabled", action="store_true",
                        default=True, help="与现役 workflow 一致（默认开启）")
    parser.add_argument("--gate-disabled", dest="gate_enabled", action="store_false")
    parser.add_argument("--attempts", type=int, default=1,
                        help="缺报告时有界等待次数（离线 comments-file 忽略）")
    parser.add_argument("--retry-delay", type=int, default=180)
    parser.add_argument("--comments-file", help="离线测试：从 JSON 文件读评论")
    parser.add_argument("--baseline-file", help="离线测试：直接给 baseline JSON 文件")
    parser.add_argument("--report-out", help="将规范化结果写入该路径")
    return parser


def _run_once(args) -> dict:
    expected_author_id = int(args.expected_author_id)
    if args.comments_file:
        with open(args.comments_file, encoding="utf-8") as handle:
            comments = json.load(handle)
    else:
        comments = fetch_comments(args.repo, args.source_sha,
                                  os.environ.get("GITHUB_TOKEN", ""))
    report = select_candidate(comments, args.repo, args.source_sha, expected_author_id)
    if report is None:
        raise IngestError("REPORT_MISSING",
                          "no approved controller report bound to this source SHA")
    # 先做结构与摘要校验（纯本地），避免用未校验的 baseline.path/commit 组 URL 发只读请求。
    validate_report(report, args.repo, args.source_sha, args.expected_baseline_ref,
                    args.expected_baseline_commit)
    baseline = report.get("baseline")
    _require(isinstance(baseline, dict), "NO_COMPARABLE_BASELINE", "baseline missing")
    if args.baseline_file:
        with open(args.baseline_file, "rb") as handle:
            baseline_bytes = handle.read(MAX_BASELINE_BYTES + 1)
    else:
        baseline_bytes = fetch_baseline_bytes(args.repo, baseline["path"],
                                              baseline["commit"],
                                              os.environ.get("GITHUB_TOKEN", ""))
    return verify(report, baseline_bytes, args.repo, args.source_sha,
                  expected_author_id, args.expected_baseline_ref,
                  args.expected_baseline_commit, args.gate_enabled)


def main() -> int:
    args = build_parser().parse_args()
    if not SHA40_RE.match(args.source_sha or ""):
        print("ERROR: --source-sha must be a full 40-hex commit SHA", file=sys.stderr)
        return 2
    if not re.fullmatch(r"[1-9][0-9]*", args.expected_author_id or ""):
        print("ERROR: --expected-author-id must be a positive integer API id",
              file=sys.stderr)
        return 2
    if "/" not in args.repo or args.repo.count("/") != 1:
        print("ERROR: --repo must be owner/repository", file=sys.stderr)
        return 2

    attempts = max(1, args.attempts if not args.comments_file else 1)
    last = None
    for attempt in range(1, attempts + 1):
        try:
            result = _run_once(args)
            break
        except IngestError as exc:
            last = exc
            # 只对「报告尚未出现」做有界等待；其余失败立即返回。
            if exc.status != "REPORT_MISSING" or attempt == attempts:
                print(f"PERF_INGEST_FAILED: {exc.status}: {exc.reason}", file=sys.stderr)
                print(f"::error::性能回归检查 {exc.status}: {exc.reason}")
                return 2
            print(f"[attempt {attempt}/{attempts}] report not yet present "
                  f"({exc.reason}); waiting {args.retry_delay}s", flush=True)
            time.sleep(args.retry_delay)
    else:  # pragma: no cover - loop always breaks or returns
        print(f"PERF_INGEST_FAILED: {last}", file=sys.stderr)
        return 2

    if args.report_out:
        path = os.path.dirname(os.path.abspath(args.report_out))
        os.makedirs(path, exist_ok=True)
        with open(args.report_out, "w", encoding="utf-8") as handle:
            json.dump(result, handle, ensure_ascii=False, indent=2, sort_keys=True)
    emit_summary(result)
    print(json.dumps({k: result[k] for k in
                      ("status", "source_sha", "baseline_commit", "derived_verdict")},
                     ensure_ascii=False))
    return 0


if __name__ == "__main__":
    sys.exit(main())
