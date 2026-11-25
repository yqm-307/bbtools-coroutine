#!/usr/bin/env python3
"""只接受仓库配置中与当前 PR 性能比较完全一致的豁免。"""

import os
import re


def main():
    approval = re.fullmatch(
        r"([1-9][0-9]*) ([0-9a-f]{40}) ([0-9a-f]{40}) (.+)",
        os.environ.get("WAIVER_APPROVAL", ""),
    )
    if approval is None or approval.group(1, 2, 3) != (
        os.environ["PR_NUMBER"], os.environ["PR_BASE_SHA"], os.environ["PR_HEAD_SHA"]
    ):
        return

    pr, base, head, reason = approval.groups()
    if "0" * 40 in (base, head) or not reason.strip() or not reason.isprintable():
        return

    with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as output:
        output.write("perf_waived=true\n")
    reason = reason.strip().replace("%", "%25")
    print(f"::notice::perf-regression SKIPPED (approved): PR #{pr}, base={base}, head={head}, reason={reason}")


if __name__ == "__main__":
    main()
