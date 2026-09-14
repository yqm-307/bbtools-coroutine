#!/usr/bin/env python3
"""Devin C++ Core Review 编排脚本（review_contract: hermes-repo-review/v1）

由 .github/workflows/devin-review.yml 在 pull_request(opened/reopened/synchronize)
触发时执行。职责：

  1. 计算 PR diff 中命中 bbt/**/*.{cc,cpp,h,hpp} 的核心 C++ 变更；
  2. 维护名为 "Devin C++ Core Review" 的 check run（checks API）；
  3. 通过 Devin Sessions API 创建只读审查会话并轮询至终止态；
  4. 按契约解析 verdict：仅当审查针对当前 head 完成且无 Critical/Important
     时才置 success；BLOCKED/错误/超时/旧 head/回写失败均为非成功；
  5. 将中文审查结果以 PR review(COMMENT) 形式回写。

环境变量：
  GITHUB_TOKEN            必填，Actions 注入的 GITHUB_TOKEN
  GITHUB_REPOSITORY       owner/repo（Actions 提供）
  GITHUB_EVENT_PATH       事件 payload 路径（Actions 提供）
  GITHUB_API_URL          默认 https://api.github.com
  DEVIN_API_KEY           必填 secret；缺失时 check run 置 BLOCKED 失败
  DEVIN_API_BASE          默认 https://api.devin.ai
  DEVIN_ORG_ID            可选；设置后走 v3 /organizations/{org}/sessions，
                          否则走 v1 /sessions（legacy，service/personal key）
  DEVIN_TIMEOUT_SECONDS   轮询总超时，默认 1500
  DEVIN_POLL_INTERVAL     轮询间隔秒，默认 20
  DEVIN_REVIEW_MAX_DIFF_BYTES  diff 注入 prompt 的字节上限，默认 200000
  DEVIN_REVIEW_WORKSPACE  仓库 checkout 根目录，默认 GITHUB_WORKSPACE 或 git root

适配点：DevinClient 封装了 session 创建/轮询/输出提取的全部 API 细节；
Devin API 变更时只改这个类。脚本不 mock 审查结果——任何无法获得真实
verdict 的路径都以非成功退出。
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
import time
import urllib.error
import urllib.request

CHECK_NAME = "Devin C++ Core Review"
REVIEW_CONTRACT = "hermes-repo-review/v1"
VERDICT_SCHEMA = "hermes-repo-review/v1-verdict"
CORE_EXTS = {".cc", ".cpp", ".h", ".hpp"}
CORE_PREFIX = "bbt/"

DEVIN_TERMINAL_STATES = {"finished", "blocked", "expired", "errored", "stopped", "suspended"}
DEVIN_SUCCESS_STATES = {"finished"}

GITHUB_OUTPUT_LIMIT = 60000  # check run summary 上限 65535，留余量


class ReviewError(Exception):
    pass


# ────────────────────────────── 工具 ──────────────────────────────


def env(name: str, default: str = "") -> str:
    return os.environ.get(name, default)


def run_git(args: list[str], cwd: str) -> str:
    proc = subprocess.run(
        ["git", *args], cwd=cwd, capture_output=True, text=True, timeout=120
    )
    if proc.returncode != 0:
        raise ReviewError(f"git {' '.join(args)} 失败: {proc.stderr.strip()}")
    return proc.stdout


def http_json(method: str, url: str, token: str, body: dict | None = None) -> dict:
    req = urllib.request.Request(url, method=method)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("X-GitHub-Api-Version", "2022-11-28")
    if token:
        req.add_header("Authorization", f"Bearer {token}")
    data = None
    if body is not None:
        data = json.dumps(body).encode()
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, data=data, timeout=60) as resp:
            raw = resp.read().decode()
            return json.loads(raw) if raw else {}
    except urllib.error.HTTPError as e:
        detail = e.read().decode(errors="replace")[:2000]
        raise ReviewError(f"GitHub API {method} {url} -> HTTP {e.code}: {detail}")
    except urllib.error.URLError as e:
        raise ReviewError(f"GitHub API {method} {url} 网络错误: {e.reason}")


def is_core_cpp(path: str) -> bool:
    """等价于 GitHub Actions paths glob bbt/**/*.{cc,cpp,h,hpp}（** 含零层目录）。"""
    if not path.startswith(CORE_PREFIX):
        return False
    _, dot, ext = path.rpartition(".")
    return bool(dot) and f".{ext}" in CORE_EXTS


# ────────────────────────── Devin API 适配点 ──────────────────────────


class DevinClient:
    """Devin Sessions API 客户端。v1（legacy）与 v3 两种入口。

    v1: POST {base}/v1/sessions          Bearer key
    v3: POST {base}/v3/organizations/{org}/sessions
    """

    def __init__(self, api_key: str, base: str, org_id: str):
        self.api_key = api_key
        self.base = base.rstrip("/")
        self.org_id = org_id

    def _request(self, method: str, path: str, body: dict | None = None) -> dict:
        url = f"{self.base}{path}"
        req = urllib.request.Request(url, method=method)
        req.add_header("Authorization", f"Bearer {self.api_key}")
        data = None
        if body is not None:
            data = json.dumps(body).encode()
            req.add_header("Content-Type", "application/json")
        try:
            with urllib.request.urlopen(req, data=data, timeout=60) as resp:
                raw = resp.read().decode()
                return json.loads(raw) if raw else {}
        except urllib.error.HTTPError as e:
            detail = e.read().decode(errors="replace")[:2000]
            raise ReviewError(f"Devin API {method} {url} -> HTTP {e.code}: {detail}")
        except urllib.error.URLError as e:
            raise ReviewError(f"Devin API {method} {url} 网络错误: {e.reason}")

    def _sessions_path(self) -> str:
        if self.org_id:
            return f"/v3/organizations/{self.org_id}/sessions"
        return "/v1/sessions"

    def create_session(self, prompt: str, title: str) -> dict:
        body = {
            "prompt": prompt,
            "title": title,
            "idempotent": False,
            "tags": ["hermes-repo-review", "bbtools-coroutine"],
        }
        return self._request("POST", self._sessions_path(), body)

    def get_session(self, session_id: str) -> dict:
        return self._request("GET", f"{self._sessions_path()}/{session_id}")

    @staticmethod
    def session_id_of(created: dict) -> str:
        sid = created.get("session_id") or created.get("devin_id") or created.get("id")
        if not sid:
            raise ReviewError(f"Devin create session 响应缺少 session id: {created}")
        return sid

    @staticmethod
    def session_url_of(created: dict) -> str:
        return created.get("url") or created.get("session_url") or ""

    @staticmethod
    def status_of(session: dict) -> str:
        return str(session.get("status_enum") or session.get("status") or "").lower()

    @staticmethod
    def output_text_of(session: dict) -> str:
        """提取 session 的文本输出。优先 structured_output，其次扫描 messages。

        Devin API 响应字段随版本演进，这里做宽容提取；找不到则返回空串，
        由上层按「verdict 缺失 → 非成功」处理。
        """
        so = session.get("structured_output")
        if isinstance(so, dict) and so.get("schema") == VERDICT_SCHEMA:
            return "```json\n" + json.dumps(so, ensure_ascii=False) + "\n```"
        if isinstance(so, str) and VERDICT_SCHEMA in so:
            return so
        messages = session.get("messages") or []
        for msg in reversed(messages):
            text = msg.get("message") or msg.get("text") or msg.get("content") or ""
            if isinstance(text, str) and VERDICT_SCHEMA in text:
                return text
        for key in ("output", "result", "last_message"):
            text = session.get(key)
            if isinstance(text, str) and VERDICT_SCHEMA in text:
                return text
        return ""


# ────────────────────────── verdict 解析 ──────────────────────────


JSON_BLOCK_RE = re.compile(r"```json\s*(\{.*?\})\s*```", re.DOTALL)


def parse_verdict(text: str, head_sha: str) -> dict:
    """从 Devin 输出文本中解析 verdict JSON。返回规范化 dict。

    校验：schema 匹配、head_sha 与当前 head 一致（防旧 head）、issues 为列表。
    counts 由 issues 实际内容重算，不信任模型自报计数。
    """
    if not text:
        raise ReviewError("Devin session 未产出可解析的 verdict 文本")
    candidates = []
    for m in JSON_BLOCK_RE.finditer(text):
        try:
            candidates.append(json.loads(m.group(1)))
        except json.JSONDecodeError:
            continue
    verdict = None
    for cand in reversed(candidates):  # 取最后一个匹配 schema 的块
        if isinstance(cand, dict) and cand.get("schema") == VERDICT_SCHEMA:
            verdict = cand
            break
    if verdict is None:
        # 兜底：输出本身可能就是一个裸 JSON
        try:
            cand = json.loads(text)
            if isinstance(cand, dict) and cand.get("schema") == VERDICT_SCHEMA:
                verdict = cand
        except json.JSONDecodeError:
            pass
    if verdict is None:
        raise ReviewError("未找到 schema=hermes-repo-review/v1-verdict 的 JSON 结论块")
    got_sha = str(verdict.get("head_sha") or "")
    if got_sha != head_sha:
        raise ReviewError(
            f"verdict head_sha 不匹配（旧 head？）: verdict={got_sha} 当前={head_sha}"
        )
    issues = verdict.get("issues")
    if not isinstance(issues, list):
        raise ReviewError("verdict.issues 缺失或不是列表")
    counts = {"critical": 0, "important": 0, "minor": 0, "suggestion": 0, "other": 0}
    for it in issues:
        sev = str(it.get("severity", "")).lower() if isinstance(it, dict) else ""
        counts[sev if sev in counts else "other"] += 1
    verdict["counts"] = counts
    verdict["decision"] = "FAIL" if (counts["critical"] or counts["important"]) else "PASS"
    return verdict


# ────────────────────────── check run / PR review ──────────────────────────


class GitHubClient:
    def __init__(self, token: str, repo: str, api_url: str):
        self.token = token
        self.repo = repo
        self.api_url = api_url.rstrip("/")

    def _url(self, path: str) -> str:
        return f"{self.api_url}/repos/{self.repo}{path}"

    def create_check_run(self, head_sha: str) -> int:
        resp = http_json(
            "POST",
            self._url("/check-runs"),
            self.token,
            {
                "name": CHECK_NAME,
                "head_sha": head_sha,
                "status": "in_progress",
                "output": {"title": CHECK_NAME, "summary": "Devin 审查进行中…"},
            },
        )
        return int(resp["id"])

    def complete_check_run(
        self, check_id: int, conclusion: str, summary: str, details_url: str = ""
    ) -> None:
        body = {
            "status": "completed",
            "conclusion": conclusion,
            "completed_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
            "output": {
                "title": CHECK_NAME,
                "summary": summary[:GITHUB_OUTPUT_LIMIT],
            },
        }
        if details_url:
            body["details_url"] = details_url
        http_json("PATCH", self._url(f"/check-runs/{check_id}"), self.token, body)

    def pr_head_sha(self, pr_number: int) -> str:
        resp = http_json("GET", self._url(f"/pulls/{pr_number}"), self.token)
        return resp["head"]["sha"]

    def post_pr_review_comment(self, pr_number: int, body: str) -> None:
        http_json(
            "POST",
            self._url(f"/pulls/{pr_number}/reviews"),
            self.token,
            {"event": "COMMENT", "body": body[:GITHUB_OUTPUT_LIMIT]},
        )


# ────────────────────────── prompt 组装 ──────────────────────────


def build_review_package(
    workspace: str, repo: str, pr_number: int, base_sha: str, head_sha: str,
    core_files: list[str], max_diff: int,
) -> dict:
    diff = run_git(["diff", "--unified=40", f"{base_sha}...{head_sha}", "--", "bbt"], cwd=workspace)
    truncated = False
    diff_bytes = diff.encode("utf-8", errors="replace")
    if len(diff_bytes) > max_diff:
        diff = diff_bytes[:max_diff].decode("utf-8", errors="replace")
        diff += "\n\n[... diff truncated by orchestrator ...]"
        truncated = True
    return {
        "review_contract": REVIEW_CONTRACT,
        "repo": repo,
        "pr_number": pr_number,
        "base_sha": base_sha,
        "head_sha": head_sha,
        "core_files": core_files,
        "diff_unified": diff,
        "diff_truncated": truncated,
        "repo_url": f"https://github.com/{repo}",
    }


def build_prompt(workspace: str, package: dict) -> str:
    # 审查提示资产：从上游 team-agent-repo 的 profiles/reviewer/{SOUL.md,
    # review-contract.md} 迁移合并的仓库内版本化副本，见文件头来源说明。
    prompt_path = os.path.join(workspace, "agent-docs/devin-review-prompt.md")
    prompt_asset = open(prompt_path, encoding="utf-8").read()
    return (
        "# 任务：bbtools-coroutine PR 核心 C++ 只读审查\n\n"
        "你是本仓库的只读代码审查者。严格遵守以下版本化审查提示资产（它包含本次审查的"
        "完整身份规则与输入/输出契约），然后对 review_package 中的 diff 执行审查。\n\n"
        "硬性要求：\n"
        "- 只读：不修改、不提交、不推送任何文件。\n"
        "- 结论只针对 review_package.head_sha；最后一条消息必须且仅包含一个\n"
        "  ```json fenced 块，schema 为 hermes-repo-review/v1-verdict。\n"
        "- issues 只放有真实证据（文件+行号/代码片段）的问题；"
        "severity 取 Critical|Important|Minor|Suggestion。\n"
        "- 所有面向人的文字用中文。\n\n"
        "## agent-docs/devin-review-prompt.md\n\n" + prompt_asset + "\n\n"
        "## review_package\n\n```json\n"
        + json.dumps(package, ensure_ascii=False) + "\n```\n"
    )


def render_verdict_markdown(verdict: dict, package: dict, session_url: str) -> str:
    lines = [
        f"## Devin C++ Core Review — {verdict['decision']}",
        "",
        f"- head SHA：`{verdict['head_sha']}`",
        f"- 范围：{len(package['core_files'])} 个核心 C++ 文件"
        + ("（diff 已截断）" if package.get("diff_truncated") else ""),
        f"- 严重度统计：Critical={verdict['counts']['critical']} "
        f"Important={verdict['counts']['important']} "
        f"Minor={verdict['counts']['minor']} "
        f"Suggestion={verdict['counts']['suggestion']}",
    ]
    if session_url:
        lines.append(f"- Devin session：{session_url}")
    lines += ["", "### 总体结论", "", verdict.get("summary_cn", ""), ""]
    if verdict["issues"]:
        lines += ["### 问题列表", ""]
        for i, it in enumerate(verdict["issues"], 1):
            lines += [
                f"{i}. **[{it.get('severity')}]** `{it.get('file')}` "
                f"{it.get('lines', '')}",
                f"   - 证据：{it.get('evidence', '')}",
                f"   - 描述：{it.get('description_cn', '')}",
                f"   - 建议：{it.get('suggestion_cn', '')}",
            ]
        lines.append("")
    uncovered = verdict.get("uncovered") or []
    if uncovered:
        lines += ["### 未覆盖项", ""]
        lines += [f"- {u}" for u in uncovered]
        lines.append("")
    if verdict.get("residual_risk_cn"):
        lines += ["### 残余风险", "", verdict["residual_risk_cn"], ""]
    return "\n".join(lines)


# ────────────────────────── 主流程 ──────────────────────────


def fail_check(gh: GitHubClient | None, check_id: int | None,
               conclusion: str, summary: str, details_url: str = "") -> None:
    if gh is None or check_id is None:
        return
    try:
        gh.complete_check_run(check_id, conclusion, summary, details_url)
    except ReviewError as e:
        # 回写失败本身即非成功——记录到 stderr 供 Actions 日志取证
        print(f"check run 回写失败: {e}", file=sys.stderr)


def main() -> int:
    token = env("GITHUB_TOKEN") or env("GH_TOKEN")
    repo = env("GITHUB_REPOSITORY")
    event_path = env("GITHUB_EVENT_PATH")
    api_url = env("GITHUB_API_URL", "https://api.github.com")
    if not token or not repo or not event_path:
        print("缺少 GITHUB_TOKEN/GITHUB_REPOSITORY/GITHUB_EVENT_PATH", file=sys.stderr)
        return 1

    event = json.load(open(event_path, encoding="utf-8"))
    pr = event.get("pull_request")
    if not pr:
        print("事件不含 pull_request，退出", file=sys.stderr)
        return 1
    pr_number = int(pr["number"])
    head_sha = pr["head"]["sha"]
    base_sha = pr["base"]["sha"]

    workspace = env("DEVIN_REVIEW_WORKSPACE") or env("GITHUB_WORKSPACE") or os.getcwd()
    gh = GitHubClient(token, repo, api_url)
    check_id: int | None = None
    session_url = ""

    try:
        check_id = gh.create_check_run(head_sha)
    except ReviewError as e:
        print(f"创建 check run 失败: {e}", file=sys.stderr)
        return 1

    try:
        changed = run_git(
            ["diff", "--name-only", f"{base_sha}...{head_sha}"], cwd=workspace
        ).splitlines()
        core_files = sorted(p for p in changed if is_core_cpp(p))

        if not core_files:
            gh.complete_check_run(
                check_id, "success",
                "SKIPPED: no core C++ changes\n\n"
                "本次 PR diff 未命中 bbt/**/*.{cc,cpp,h,hpp}，未触发 Devin 审查。",
            )
            print("SKIPPED: no core C++ changes")
            return 0

        devin_key = env("DEVIN_API_KEY")
        if not devin_key:
            gh.complete_check_run(
                check_id, "failure",
                "BLOCKED: DEVIN_API_KEY secret not configured。\n\n"
                f"命中核心文件 {len(core_files)} 个，但无法创建 Devin 审查会话。"
                "请在仓库 Settings → Secrets 中配置 DEVIN_API_KEY。",
            )
            print("BLOCKED: DEVIN_API_KEY secret not configured", file=sys.stderr)
            return 1

        package = build_review_package(
            workspace, repo, pr_number, base_sha, head_sha, core_files,
            int(env("DEVIN_REVIEW_MAX_DIFF_BYTES", "200000")),
        )
        prompt = build_prompt(workspace, package)

        devin = DevinClient(
            devin_key,
            env("DEVIN_API_BASE", "https://api.devin.ai"),
            env("DEVIN_ORG_ID"),
        )
        created = devin.create_session(
            prompt, f"Devin C++ Core Review PR#{pr_number} {head_sha[:8]}"
        )
        session_id = DevinClient.session_id_of(created)
        session_url = DevinClient.session_url_of(created)
        print(f"Devin session created: {session_id} {session_url}")

        deadline = time.time() + int(env("DEVIN_TIMEOUT_SECONDS", "1500"))
        interval = int(env("DEVIN_POLL_INTERVAL", "20"))
        session: dict = {}
        status = ""
        while time.time() < deadline:
            session = devin.get_session(session_id)
            status = DevinClient.status_of(session)
            if status in DEVIN_TERMINAL_STATES:
                break
            time.sleep(interval)
        if status not in DEVIN_TERMINAL_STATES:
            fail_check(gh, check_id, "timed_out",
                       f"Devin session 轮询超时（{int(env('DEVIN_TIMEOUT_SECONDS','1500'))}s），"
                       f"session={session_id}", session_url)
            return 1
        if status not in DEVIN_SUCCESS_STATES:
            fail_check(gh, check_id, "failure",
                       f"Devin session 终止于非正常状态：{status}，session={session_id}",
                       session_url)
            return 1

        verdict = parse_verdict(DevinClient.output_text_of(session), head_sha)

        # 旧 head 防护：写回前确认 PR head 未移动
        current_head = gh.pr_head_sha(pr_number)
        if current_head != head_sha:
            fail_check(gh, check_id, "cancelled",
                       f"STALE HEAD: 审查基于 {head_sha}，当前 head 已变为 {current_head}。"
                       "等待新一轮审查。", session_url)
            return 1

        body = render_verdict_markdown(verdict, package, session_url)
        gh.post_pr_review_comment(pr_number, body)

        if verdict["decision"] == "PASS":
            gh.complete_check_run(
                check_id, "success",
                "Devin 审查完成：无 Critical/Important 问题。\n\n" + body,
                session_url,
            )
            return 0
        gh.complete_check_run(
            check_id, "failure",
            "Devin 审查完成：存在 Critical/Important 问题。\n\n" + body,
            session_url,
        )
        return 1

    except ReviewError as e:
        fail_check(gh, check_id, "failure", f"审查编排失败：{e}", session_url)
        print(f"ReviewError: {e}", file=sys.stderr)
        return 1
    except Exception as e:  # 兜底：任何异常都必须落到非成功 check
        fail_check(gh, check_id, "failure", f"审查编排异常：{type(e).__name__}: {e}",
                   session_url)
        print(f"unexpected error: {type(e).__name__}: {e}", file=sys.stderr)
        return 1


# ────────────────────────── 本地自检（无网络） ──────────────────────────


def selftest() -> int:
    """静态 smoke：路径匹配、verdict 解析、gate 逻辑。不触网。"""
    ok = True

    def check(name: str, cond: bool):
        nonlocal ok
        print(("PASS" if cond else "FAIL"), name)
        ok = ok and cond

    check("bbt/a/b/c.hpp 命中", is_core_cpp("bbt/a/b/c.hpp"))
    check("bbt/x.cc 命中", is_core_cpp("bbt/x.cc"))
    check("bbt/x.cpp 命中", is_core_cpp("bbt/x.cpp"))
    check("bbt/x.h 命中", is_core_cpp("bbt/x.h"))
    check("unit_test/x.cc 不命中", not is_core_cpp("unit_test/x.cc"))
    check("bbt/x.py 不命中", not is_core_cpp("bbt/x.py"))
    check("bbtx/x.cc 不命中", not is_core_cpp("bbtx/x.cc"))

    head = "a" * 40
    good = {"schema": VERDICT_SCHEMA, "head_sha": head, "summary_cn": "ok",
            "issues": [{"severity": "Minor", "file": "bbt/a.cc", "lines": "1",
                        "evidence": "e", "description_cn": "d",
                        "suggestion_cn": "s"}],
            "uncovered": [], "residual_risk_cn": ""}
    v = parse_verdict("前缀文字\n```json\n" + json.dumps(good) + "\n```\n后缀", head)
    check("PASS verdict 解析", v["decision"] == "PASS" and v["counts"]["minor"] == 1)

    bad = dict(good, issues=[dict(good["issues"][0], severity="Critical")])
    v = parse_verdict("```json\n" + json.dumps(bad) + "\n```", head)
    check("Critical -> FAIL", v["decision"] == "FAIL" and v["counts"]["critical"] == 1)

    try:
        parse_verdict("```json\n" + json.dumps(dict(good, head_sha="b" * 40)) + "\n```", head)
        check("旧 head 被拒绝", False)
    except ReviewError:
        check("旧 head 被拒绝", True)

    try:
        parse_verdict("no json here", head)
        check("无 verdict 被拒绝", False)
    except ReviewError:
        check("无 verdict 被拒绝", True)

    return 0 if ok else 1


if __name__ == "__main__":
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    sys.exit(main())
