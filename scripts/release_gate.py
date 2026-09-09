#!/usr/bin/env python3
"""Validate RC/Stable inputs and publish with explicit remote readback."""
from __future__ import annotations

import argparse
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request
from typing import NoReturn

VERSION_RE = re.compile(r"v(?P<major>0|[1-9][0-9]*)\.(?P<minor>0|[1-9][0-9]*)\.(?P<patch>0|[1-9][0-9]*)(?:-rc(?P<rc>0|[1-9][0-9]*))?")


def fail(message: str) -> NoReturn:
    print(f"release-gate: FAIL: {message}", file=sys.stderr)
    raise SystemExit(2)


def parse_version(version: str) -> re.Match[str]:
    match = VERSION_RE.fullmatch(version)
    if match is None:
        fail("invalid version; expected vX.Y.Z or vX.Y.Z-rcN")
    return match


def validate_inputs(kind: str, version: str, source_sha: str, rc_tag: str | None) -> None:
    version_match = parse_version(version)
    if not re.fullmatch(r"[0-9a-f]{40}", source_sha):
        fail("source_sha must be a full 40-character lowercase commit SHA")
    if kind == "rc":
        if version_match.group("rc") is None or rc_tag:
            fail("RC requires vX.Y.Z-rcN and an empty rc_tag")
    elif kind == "stable" and rc_tag:
        rc_match = parse_version(rc_tag)
        if version_match.group("rc") is not None or rc_match.group("rc") is None:
            fail("Stable requires vX.Y.Z and a vX.Y.Z-rcN source")
        if version_match.group("major", "minor", "patch") != rc_match.group("major", "minor", "patch"):
            fail("Stable and RC must have the same major/minor/patch")
    else:
        fail("invalid kind or missing rc_tag")


def git(*args: str) -> str:
    try:
        result = subprocess.run(["git", *args], text=True, capture_output=True, timeout=60)
    except (OSError, subprocess.TimeoutExpired):
        fail("git command unavailable or timed out")
    if result.returncode:
        fail("git command failed (check remote and repository access)")
    return result.stdout.strip()


def repository() -> str:
    value = os.environ.get("GITHUB_REPOSITORY", "")
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", value):
        fail("GITHUB_REPOSITORY must be owner/repository")
    return value


def api(path: str, method: str = "GET", payload: dict | None = None, *, allow_404: bool = False) -> dict:
    token = os.environ.get("GITHUB_TOKEN")
    if not token:
        fail("GITHUB_TOKEN is required")
    request = urllib.request.Request(
        f"https://api.github.com/{path}",
        data=None if payload is None else json.dumps(payload).encode(),
        method=method,
        headers={"Accept": "application/vnd.github+json", "Authorization": f"Bearer {token}",
                 "X-GitHub-Api-Version": "2022-11-28", "Content-Type": "application/json"},
    )
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            data = json.load(response)
        if not isinstance(data, dict):
            fail("unexpected GitHub API response")
        return data
    except urllib.error.HTTPError as exc:
        if allow_404 and method == "GET" and exc.code == 404:
            return {}
        fail(f"GitHub API {method} failed (HTTP {exc.code}); reconcile before retrying a write")
    except (urllib.error.URLError, TimeoutError, ValueError):
        fail("GitHub API unavailable or invalid response; reconcile before retrying a write")


def ensure_absent(version: str) -> None:
    repo = repository()
    if api(f"repos/{repo}/git/ref/tags/{version}", allow_404=True):
        fail("version tag already exists; never move or retry it blindly")
    if api(f"repos/{repo}/releases/tags/{version}", allow_404=True):
        fail("Release already exists; reconcile instead of recreating")


def validate_main_ci(source_sha: str) -> None:
    repo = repository()
    runs = api(f"repos/{repo}/actions/workflows/unit_test.yml/runs?event=push&branch=main&head_sha={source_sha}&per_page=100")
    candidates = [r for r in runs.get("workflow_runs", []) if r.get("head_sha") == source_sha
                  and r.get("event") == "push" and r.get("head_branch") == "main"]
    if not candidates:
        fail("no main CI run for source_sha")
    run = max(candidates, key=lambda r: (r["id"], r.get("run_attempt", 1)))
    if run.get("conclusion") != "success":
        fail("latest main CI for source_sha is not successful")
    jobs = api(f"repos/{repo}/actions/runs/{run['id']}/jobs?per_page=100")
    if jobs.get("total_count", 0) > len(jobs.get("jobs", [])):
        fail("incomplete CI jobs response")
    passed = {j["name"] for j in jobs.get("jobs", []) if j.get("conclusion") == "success"}
    if not {"编译 & 单元测试", "真实客户端验收", "性能回归检查", "1h 并行疲劳压测"} <= passed:
        fail("required main CI jobs missing, skipped or failed")


def validate_candidate(kind: str, version: str, source_sha: str, rc_tag: str | None) -> None:
    validate_inputs(kind, version, source_sha, rc_tag)
    ensure_absent(version)
    repo = repository()
    main_sha = api(f"repos/{repo}/git/ref/heads/main")["object"]["sha"]
    if kind == "rc" and source_sha != main_sha:
        fail("RC source_sha is not remote main HEAD")
    # Stable may use an older RC, but never a commit outside the main history.
    if git("merge-base", source_sha, main_sha) != source_sha:
        fail("source_sha is not in main history (or checkout is stale)")
    validate_main_ci(source_sha)
    if kind == "stable":
        tag = api(f"repos/{repo}/git/ref/tags/{rc_tag}")
        if tag.get("object", {}).get("type") != "commit" or tag["object"].get("sha") != source_sha:
            fail("RC must be a lightweight tag at source_sha")
        release = api(f"repos/{repo}/releases/tags/{rc_tag}")
        if release.get("prerelease") is not True or release.get("draft") is not False:
            fail("RC must be a published prerelease, not a draft")
        if release.get("tag_name") != rc_tag:
            fail("RC Release tag mismatch")
    print(f"release-gate: candidate validated: {version} @ {source_sha}")


def publish(kind: str, version: str, source_sha: str, rc_tag: str | None) -> None:
    # Runs again after Environment approval; no unvalidated input reaches a remote lookup.
    validate_candidate(kind, version, source_sha, rc_tag)
    run_id = os.environ.get("GITHUB_RUN_ID", "")
    if not re.fullmatch(r"[0-9]+", run_id):
        fail("publish requires a GitHub Actions run ID")
    repo = repository()
    prerelease = kind == "rc"
    body = f"Source SHA: `{source_sha}`\n\nEvidence: https://github.com/{repo}/actions/runs/{run_id}\n"
    if rc_tag:
        body += f"\nPromoted from: `{rc_tag}`\n"
    api(f"repos/{repo}/releases", method="POST", payload={
        "tag_name": version, "target_commitish": source_sha, "name": version,
        "body": body, "generate_release_notes": True, "prerelease": prerelease,
        "draft": False, "make_latest": "false" if prerelease else "true",
    })
    release = api(f"repos/{repo}/releases/tags/{version}")
    tag = api(f"repos/{repo}/git/ref/tags/{version}")
    if tag.get("object", {}).get("type") != "commit" or tag["object"].get("sha") != source_sha:
        fail("published tag SHA mismatch; reconcile without moving tag")
    if (release.get("tag_name") != version or release.get("prerelease") is not prerelease
            or release.get("draft") is not False or source_sha not in release.get("body", "")):
        fail("published Release metadata mismatch; reconcile before retry")
    print(json.dumps({"version": version, "sha": source_sha, "url": release.get("html_url")}, ensure_ascii=False))


def main() -> None:
    parser = argparse.ArgumentParser()
    sub = parser.add_subparsers(dest="command", required=True)
    check = sub.add_parser("validate-version")
    check.add_argument("--version", required=True)
    for name in ("validate", "publish"):
        command = sub.add_parser(name)
        command.add_argument("--kind", choices=("rc", "stable"), required=True)
        command.add_argument("--version", required=True)
        command.add_argument("--source-sha", required=True)
        command.add_argument("--rc-tag")
    args = parser.parse_args()
    if args.command == "validate-version":
        parse_version(args.version)
        print("release-gate: version valid")
    elif args.command == "validate":
        validate_candidate(args.kind, args.version, args.source_sha, args.rc_tag)
    else:
        publish(args.kind, args.version, args.source_sha, args.rc_tag)


if __name__ == "__main__":
    main()
