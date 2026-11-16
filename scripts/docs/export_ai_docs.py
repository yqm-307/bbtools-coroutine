#!/usr/bin/env python3
"""Doxygen XML -> 分层 Markdown（给 AI 渐进式阅读）。

用法: python3 -I export_ai_docs.py <xml_dir> <out_dir> <modules.json>

分层（读到够用就停）:
  index.md            L0  仓库入口：模块表 + 阅读方式 + 说明覆盖率
  modules/<m>.md      L1  模块职责 + 符号清单（名字 + 一句话）
  symbols/<id>.md     L2  单个类 / 头文件内宏与函数的完整说明
  源码 文件:行号       L3  只给指针，不复制源码

退出码非 0：XML 缺失，或自检失败（断链、重复符号页、输出里带绝对路径）。
"""
import json
import pathlib
import re
import sys
import xml.etree.ElementTree as ET

SECTION_LABEL = {"note": "注意", "warning": "警告", "pre": "前置条件", "post": "后置条件",
                 "see": "参见", "remark": "备注"}
KIND_LABEL = {"class": "类", "struct": "结构体", "file": "头文件（宏 / 自由函数 / 类型）"}


def text_of(node):
    """把 doxygen 描述节点展平成 Markdown 文本。"""
    if node is None:
        return ""
    out = []

    def walk(n):
        tag = n.tag
        if tag == "computeroutput":
            out.append("`" + "".join(n.itertext()).strip() + "`")
        elif tag == "ref":
            out.append("".join(n.itertext()))
        elif tag in ("itemizedlist", "orderedlist"):
            out.append("\n")
            for i, li in enumerate(n.findall("listitem")):
                bullet = "-" if tag == "itemizedlist" else f"{i + 1}."
                out.append(f"{bullet} " + " ".join(text_of(li).split()) + "\n")
        elif tag == "programlisting":
            lines = ["".join(h.itertext()) for h in n.findall("codeline")]
            out.append("\n```cpp\n" + "\n".join(lines) + "\n```\n")
        elif tag == "verbatim":
            # @verbatim 块里每行开头的 " * " 是注释前缀，去掉后表格才对齐
            body = "\n".join(re.sub(r"^\s*\*\s?", "", ln) for ln in "".join(n.itertext()).strip("\n").splitlines())
            out.append("\n```\n" + body.strip("\n") + "\n```\n")
        elif tag == "parameterlist":
            return  # 参数单独处理
        elif tag == "simplesect":
            kind = n.get("kind")
            if kind == "return":
                return  # 返回值单独处理
            body = " ".join(text_of(n).split())
            out.append(f"\n**{SECTION_LABEL.get(kind, kind)}**：{body}\n")
        else:
            if tag == "para" and out and not out[-1].endswith("\n"):
                out.append("\n\n")
            if n.text:
                out.append(n.text)
            for c in n:
                walk(c)
                if c.tail:
                    out.append(c.tail)

    if node.text and node.text.strip():
        out.append(node.text)
    for c in node:
        walk(c)
        if c.tail and c.tail.strip():
            out.append(c.tail)
    return re.sub(r"\n{3,}", "\n\n", "".join(out)).strip()


def brief_of(el):
    return " ".join(text_of(el.find("briefdescription")).split())


def params_of(member):
    """返回 ([(name, direction, desc)], returns_text)。"""
    params, ret = [], ""
    det = member.find("detaileddescription")
    if det is None:
        return params, ret
    for pl in det.iter("parameterlist"):
        if pl.get("kind") not in ("param", "templateparam"):
            continue
        prefix = "模板参数 " if pl.get("kind") == "templateparam" else ""
        for pi in pl.findall("parameteritem"):
            names = [" ".join(n.itertext()).strip() for n in pi.iter("parametername")]
            direction = next((n.get("direction") for n in pi.iter("parametername") if n.get("direction")), "")
            desc = " ".join(text_of(pi.find("parameterdescription")).split())
            params.append((prefix + ", ".join(names), direction, desc))
    for ss in det.iter("simplesect"):
        if ss.get("kind") == "return":
            ret = " ".join(text_of(ss).split())
    return params, ret


def sig_of(m, scope=""):
    kind = m.get("kind")
    name = (m.findtext("name") or "").strip()
    if kind == "define":
        args = [p.findtext("defname") for p in m.findall("param")]
        return f"#define {name}" + (f"({', '.join(a for a in args if a)})" if args else "")
    definition = (m.findtext("definition") or name).strip()
    if scope:
        definition = definition.replace(scope + "::", "")
    tpl = m.find("templateparamlist")
    prefix = ""
    if tpl is not None:
        ps = [" ".join(p.itertext()).strip() for p in tpl.findall("param")]
        prefix = f"template <{', '.join(ps)}> "
    if kind == "function":
        return prefix + definition + (m.findtext("argsstring") or "").strip()
    return prefix + definition


def loc_of(el):
    loc = el.find("location")
    if loc is None:
        return ""
    f = loc.get("declfile") or loc.get("file") or ""
    return f"{f}:{loc.get('line')}" if loc.get("line") else f


def safe(s):
    return re.sub(r"[^A-Za-z0-9_.-]+", "_", s).strip("_")


def module_of(path):
    """bbt/<pkg>/<module>/X.hpp -> <module>；bbt/<pkg>/X.hpp -> <pkg>。"""
    parts = pathlib.PurePosixPath(path).parts
    if "bbt" in parts:
        parts = parts[parts.index("bbt") + 1:]
    if len(parts) >= 3:
        return parts[1]
    return parts[0] if len(parts) == 2 else "misc"


def collect_symbols(xml_dir):
    symbols = []
    for f in sorted(xml_dir.glob("*.xml")):
        if f.name == "index.xml":
            continue
        for cd in ET.parse(f).getroot().findall("compounddef"):
            kind = cd.get("kind")
            name = cd.findtext("compoundname") or ""
            if kind in ("class", "struct"):
                # 模板特化是实现细节，主模板页已覆盖
                if cd.get("prot") != "public" or "<" in name:
                    continue
                members = [m for sd in cd.findall("sectiondef") if sd.get("kind", "").startswith("public")
                           for m in sd.findall("memberdef")]
                sid = safe(name)
            elif kind == "file":
                members = [m for sd in cd.findall("sectiondef")
                           if sd.get("kind") in ("define", "func", "enum", "typedef", "var")
                           for m in sd.findall("memberdef")]
                if not members and not brief_of(cd):
                    continue
                sid = "file_" + safe(name)
            else:
                continue
            symbols.append(dict(id=sid, title=name, kind=kind, el=cd, members=members,
                                module=module_of(loc_of(cd) or name), brief=brief_of(cd), loc=loc_of(cd)))
    return symbols


def visible(m):
    n = (m.findtext("name") or "").strip()
    return bool(n) and not n.startswith("~") and not n.startswith("__")


def write_symbol(s, out_dir):
    L = [f"# {s['title']}", "",
         f"- 种类：{KIND_LABEL[s['kind']]}  ·  模块：[{s['module']}](../modules/{s['module']}.md)  ·  源码：`{s['loc']}`", ""]
    tpl = s["el"].find("templateparamlist")
    if tpl is not None:
        ps = [" ".join(p.itertext()).strip() for p in tpl.findall("param")]
        L += [f"```cpp\ntemplate <{', '.join(ps)}>\n```", ""]
    L += [s["brief"] or "> 该符号缺少 `@brief`，请回头文件补注释。", ""]
    det = text_of(s["el"].find("detaileddescription"))
    if det:
        L += ["## 说明", "", det, ""]
    bases = [b.text for b in s["el"].findall("basecompoundref")]
    if bases:
        L += ["继承：" + "、".join(f"`{b}`" for b in bases), ""]
    members = [m for m in s["members"] if (m.findtext("name") or "").strip() and not (m.findtext("name") or "").startswith("~")]
    if members:
        L += ["## 公开成员", ""]
    for m in members:
        L += [f"### `{m.findtext('name').strip()}`", "",
              "```cpp\n" + sig_of(m, s["title"] if s["kind"] != "file" else "") + "\n```", "",
              brief_of(m) or "> 缺少说明。"]
        d = text_of(m.find("detaileddescription"))
        if d:
            L += ["", d]
        params, ret = params_of(m)
        if params:
            L += ["", "参数："] + [f"- `{pn}`{f' [{dr}]' if dr else ''}：{ds}" for pn, dr, ds in params]
        if ret:
            L += ["", f"返回：{ret}"]
        if loc_of(m):
            L += ["", f"源码：`{loc_of(m)}`"]
        L.append("")
    (out_dir / "symbols" / f"{s['id']}.md").write_text("\n".join(L).rstrip() + "\n", encoding="utf-8")


def write_module(mod, syms, desc, out_dir):
    L = [f"# 模块 {mod}", "", desc.get(mod, ""), "",
         "按需点进符号页；未标注“缺说明”的条目可以只读这里的一句话。", "",
         "| 符号 | 一句话 | 详情 |", "|---|---|---|"]
    for s in sorted(syms, key=lambda x: x["title"]):
        L.append(f"| `{s['title']}` | {(s['brief'] or '（缺说明）').replace('|', chr(92) + '|')} | [L2](../symbols/{s['id']}.md) |")
        if s["kind"] == "file":  # 头文件页里的宏逐个列出，AI 不必打开 L2 就能知道有哪些
            for m in s["members"]:
                if m.get("kind") == "define" and visible(m):
                    mb = (brief_of(m) or "（缺说明）").replace("|", "\\|")
                    L.append(f"| ↳ `{m.findtext('name')}` | {mb} | [L2](../symbols/{s['id']}.md) |")
    (out_dir / "modules" / f"{mod}.md").write_text("\n".join(L) + "\n", encoding="utf-8")


def coverage(syms):
    total = doc = 0
    for s in syms:
        total += 1
        doc += bool(s["brief"])
        for m in s["members"]:
            if visible(m):
                total += 1
                doc += bool(brief_of(m))
    return doc, total


def write_index(project, by_mod, desc, start_here, out_dir):
    rows, doc_all, total_all = [], 0, 0
    for mod, syms in sorted(by_mod.items()):
        d, t = coverage(syms)
        doc_all, total_all = doc_all + d, total_all + t
        rows.append(f"| [{mod}](modules/{mod}.md) | {desc.get(mod, '')} | {len(syms)} | {d} / {t} |")
    L = [f"# {project} · AI 文档入口（L0）", "",
         "读法：先读本页选模块 → 读 `modules/<模块>.md` 看符号清单与一句话 → 需要细节再读 "
         "`symbols/<符号>.md`。每个符号页末尾给出 `文件:行号`，要看实现再去读源码，不要一上来读源码。", "",
         "## 模块", "", "| 模块 | 说明 | 符号页数 | 有说明的条目 / 总条目 |", "|---|---|---|---|", *rows, "",
         f"整体说明覆盖：{doc_all} / {total_all}（{100 * doc_all // max(total_all, 1)}%）。"
         "标有“缺说明”的条目不要当作契约使用，先补头文件注释。", ""]
    if start_here:
        L += ["## 优先阅读", ""] + [f"- [{m}](modules/{m}.md)：{why}" for m, why in start_here] + [""]
    (out_dir / "index.md").write_text("\n".join(L), encoding="utf-8")
    return doc_all, total_all


def self_check(out_dir):
    """断链、绝对路径、空输出都判失败：AI 读到坏链接比没有文档更糟。"""
    errors = []
    mds = sorted(out_dir.rglob("*.md"))
    if not (out_dir / "index.md").is_file() or len(mds) < 3:
        errors.append("index.md 缺失或符号页过少")
    for p in mds:
        t = p.read_text(encoding="utf-8")
        # 只校验本脚本生成的 .md 链接；代码示例里的 `[=](int fd)` 之类 lambda 不是链接
        for target in re.findall(r"\]\(([^)#\s]+\.md)\)", t):
            if not (p.parent / target).resolve().is_file():
                errors.append(f"{p.relative_to(out_dir)}: 断链 {target}")
        if re.search(r"/(home|Users|tmp|workspace)/", t):
            errors.append(f"{p.relative_to(out_dir)}: 出现绝对路径")
    return errors


def main():
    xml_dir, out_dir, cfg_path = (pathlib.Path(a) for a in sys.argv[1:4])
    cfg = json.loads(cfg_path.read_text(encoding="utf-8"))
    if not any(xml_dir.glob("*.xml")):
        sys.exit(f"没有 Doxygen XML: {xml_dir}")
    (out_dir / "modules").mkdir(parents=True, exist_ok=True)
    (out_dir / "symbols").mkdir(parents=True, exist_ok=True)

    symbols = collect_symbols(xml_dir)
    ids = [s["id"] for s in symbols]
    dup = sorted({i for i in ids if ids.count(i) > 1})
    if dup:
        sys.exit(f"符号页 id 重复: {dup}")

    by_mod = {}
    for s in symbols:
        write_symbol(s, out_dir)
        by_mod.setdefault(s["module"], []).append(s)
    desc = cfg.get("modules", {})
    for mod, syms in sorted(by_mod.items()):
        write_module(mod, syms, desc, out_dir)
    doc, total = write_index(cfg["project"], by_mod, desc, cfg.get("start_here", []), out_dir)
    (out_dir / "manifest.json").write_text(json.dumps(
        {"project": cfg["project"], "modules": {m: [s["id"] for s in v] for m, v in sorted(by_mod.items())},
         "coverage": {"documented": doc, "total": total}}, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")

    errors = self_check(out_dir)
    print(f"symbols={len(symbols)} modules={len(by_mod)} coverage={doc}/{total}")
    if errors:
        print("\n".join(errors[:20]), file=sys.stderr)
        sys.exit(f"自检失败 {len(errors)} 项")


if __name__ == "__main__":
    main()
