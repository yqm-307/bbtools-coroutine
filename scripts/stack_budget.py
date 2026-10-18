#!/usr/bin/env python3
"""Conditional GCC stack models, not a C++ stack-safety prover (stdlib only)."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import re
import shutil
import subprocess
import sys
import uuid


SCHEMA = 1
LIMIT = (1 << 63) - 1
QUOTE = r'"(?:\\.|[^"\\])*"'
EXIT = {"WITHIN_MODEL": 0, "EXCEEDS": 1, "UNKNOWN": 2, "STALE": 2,
        "ERROR": 2, "UNSUPPORTED": 2}
REQUIRED_COVERAGE = {
    "coroutine_wrapper", "std_function", "fcontext", "exception_unwind",
    "destructors", "stack_chk", "red_zone", "return_address", "alignment",
    "runtime_callbacks", "no_interposition",
    "dynamic_loader",
}


class BudgetError(ValueError):
    def __init__(self, status, message):
        super().__init__(message)
        self.status = status


def require(condition, message, status="ERROR"):
    if not condition:
        raise BudgetError(status, message)


def digest(path):
    try:
        return hashlib.sha256(Path(path).read_bytes()).hexdigest()
    except OSError as exc:
        raise BudgetError("STALE", f"missing/unreadable input: {path}: {exc}") from exc


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True)


def fingerprint(value):
    return hashlib.sha256(canonical(value).encode()).hexdigest()


def load(path):
    try:
        return json.loads(Path(path).read_text())
    except (OSError, ValueError) as exc:
        raise BudgetError("ERROR", f"invalid JSON: {path}: {exc}") from exc


def save(path, value):
    Path(path).write_text(json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + "\n")


def integer(value, name, positive=False):
    require(type(value) is int and (1 if positive else 0) <= value <= LIMIT,
            f"{name} must be an integer in [{'1' if positive else '0'}, {LIMIT}]")
    return value


def add(*values):
    result = sum(values)
    require(0 <= result <= LIMIT, "stack-size arithmetic overflow")
    return result


def align(value, page):
    return add(value, (-value) % page)


def run(argv, cwd=None, input_text=None, echo=False):
    # No shell, compile_commands execution, or implicit compiler discovery.
    try:
        proc = subprocess.run([str(x) for x in argv], cwd=cwd, input=input_text,
                              text=True, capture_output=True, timeout=90,
                              env={**os.environ, "LC_ALL": "C"})
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise BudgetError("ERROR", f"command failed: {argv!r}: {exc}") from exc
    require(proc.returncode == 0,
            f"command exit {proc.returncode}: {argv!r}\n{proc.stderr}")
    if echo:
        sys.stdout.write(proc.stdout)
        sys.stderr.write(proc.stderr)
    return proc.stdout


def toolchain(compiler):
    require(sys.platform == "linux" and platform.machine() == "x86_64",
            "only Linux x86_64 is supported", "UNSUPPORTED")
    compiler = str(Path(compiler).resolve())
    version = run([compiler, "-dumpfullversion"]).strip()
    machine = run([compiler, "-dumpmachine"]).strip()
    verbose = run([compiler, "--version"])
    require(version.startswith("15.2.") and "Free Software Foundation" in verbose
            and machine == "x86_64-linux-gnu", "requires GCC 15.2 x86_64-linux-gnu",
            "UNSUPPORTED")
    return {"compiler": compiler, "compiler_sha256": digest(compiler),
            "version": version, "machine": machine, "banner": verbose,
            "nm": run(["nm", "--version"]).splitlines()[0],
            "objdump": run(["objdump", "--version"]).splitlines()[0],
            "nm_sha256": digest(shutil.which("nm")),
            "objdump_sha256": digest(shutil.which("objdump"))}


def compile_spec(argv):
    require(len(argv) >= 2, "record requires -- /absolute/compiler <compile arguments>")
    compiler, args = str(Path(argv[0]).resolve()), list(argv[1:])
    require(Path(compiler).is_file(), "compiler must be an explicit executable path")
    require("-c" in args and args.count("-o") == 1, "only single-source -c -o compilation")
    require(not any(x.startswith(("@", "-flto", "-fplugin", "-specs", "--specs",
                                   "-wrapper", "-fprofile", "-save-temps"))
                    or x in {"-m32", "-mx32", "-fwhole-program"}
                    for x in args), "LTO/plugins/response/profile/alternate ABI unsupported",
            "UNSUPPORTED")
    output_index = args.index("-o")
    require(output_index + 1 < len(args), "missing -o operand")
    output = Path(args[output_index + 1]).resolve()
    sources = [Path(x).resolve() for x in args if x.endswith((".cc", ".cpp", ".cxx", ".C"))]
    require(len(sources) == 1, "exactly one explicit C++ source required")
    source = sources[0]
    flags, index = [], 0
    while index < len(args):
        arg = args[index]
        if arg in {"-o", "-MF", "-MT", "-MQ"}:
            index += 2
            continue
        if arg in {"-c", "-MD", "-MMD", "-MP"} or Path(arg).resolve() == source:
            index += 1
            continue
        flags.append(arg)
        index += 1
    require(not any(x.startswith(("-march=", "-mabi=")) and x not in {
        "-march=x86-64", "-mabi=sysv"} for x in flags),
        "unvalidated architecture/ABI override", "UNSUPPORTED")
    return compiler, source, output, flags


def dependency_snapshot(compiler, source, flags, cwd=None):
    cwd = Path(cwd or Path.cwd())
    resolved_source = str((cwd / source).resolve())
    text = run([compiler, *flags, "-M", "-MT", "stack_budget_input", str(source)], cwd=cwd)
    require(text.startswith("stack_budget_input:"), "unexpected GCC dependency format")
    # GCC make escaping, not a shell grammar. Reject syntax not exercised here.
    text = text.replace("\\\n", "")
    tokens = re.findall(r"(?:\\.|[^\s])+", text.split(":", 1)[1])
    paths = []
    for token in tokens:
        require(not re.search(r"(?<!\\)[$#]", token), "unsupported make dependency escaping")
        name = re.sub(r"\\([\\ #:$])", r"\1", token)
        path = str((cwd / name).resolve())
        paths.append(path)
    require(resolved_source in paths and len(paths) == len(set(paths)), "invalid dependency list")
    macros = run([compiler, *flags, "-dM", "-E", str(source)], cwd=cwd)
    # Inspect the effective cc1 options without random assembler temporary names.
    proc = subprocess.run([compiler, *flags, "-###", "-S", "-o", os.devnull, str(source)],
                          capture_output=True, text=True, timeout=30, cwd=cwd,
                          env={**os.environ, "LC_ALL": "C"})
    require(proc.returncode == 0, "cannot inspect effective GCC driver options")
    abi_names = {"__GXX_ABI_VERSION", "__SIZEOF_POINTER__", "__x86_64__", "__LP64__",
                 "__BYTE_ORDER__", "__EXCEPTIONS", "__GNUC__", "__GNUC_MINOR__"}
    abi = {}
    for line in macros.splitlines():
        fields = line.split(maxsplit=2)
        if len(fields) == 3 and fields[1] in abi_names:
            abi[fields[1]] = fields[2]
    require(abi.get("__SIZEOF_POINTER__") == "8" and "__x86_64__" in abi,
            "effective preprocessing ABI is not supported", "UNSUPPORTED")
    return {"files": {path: digest(path) for path in sorted(paths)},
            "macros_sha256": fingerprint(macros),
            "abi_macros": abi, "driver_options": proc.stderr,
            "driver_sha256": fingerprint(proc.stderr)}


def receipt_path(obj):
    return Path(str(obj) + ".stack-budget.json")


def record(argv, launcher=None):
    compiler, source, obj, flags = compile_spec(argv)
    source_arg = next(arg for arg in argv[1:] if arg.endswith((".cc", ".cpp", ".cxx", ".C"))
                      and Path(arg).resolve() == source)
    tc = toolchain(compiler)
    require("-fstack-usage" in flags and "-fcallgraph-info=su" in flags,
            "record target needs -fstack-usage -fcallgraph-info=su")
    before = dependency_snapshot(compiler, source_arg, flags)
    run(([launcher] if launcher else []) + [compiler, *argv[1:]], echo=True)
    after = dependency_snapshot(compiler, source_arg, flags)
    require(Path(source_arg).resolve() == source, "source alias changed during compilation", "STALE")
    require(before == after, "dependencies changed during compilation", "STALE")
    su, ci = obj.with_suffix(".su"), obj.with_suffix(".ci")
    evidence = {"object": digest(obj), "su": digest(su), "ci": digest(ci)}
    value = {"schema_version": SCHEMA, "object": str(obj), "source": str(source), "source_arg": source_arg,
             "cwd": str(Path.cwd()), "flags": flags, "toolchain": tc,
             "dependencies": after, "artifacts": evidence,
             "compile_event": uuid.uuid4().hex,
             "object_mtime_ns": obj.stat().st_mtime_ns}
    value["identity"] = fingerprint(value)
    save(receipt_path(obj), value)


def check_receipt(obj):
    path = receipt_path(obj)
    require(path.is_file(), f"missing compilation receipt: {path}", "UNKNOWN")
    value = load(path)
    require(value.get("schema_version") == SCHEMA, "unsupported receipt schema", "UNSUPPORTED")
    identity = value.pop("identity", None)
    require(identity == fingerprint(value), "damaged compilation receipt")
    require(value["object"] == str(obj), "receipt object identity mismatch", "STALE")
    require(isinstance(value.get("source_arg"), str), "receipt needs an actual source argv; recapture", "STALE")
    require(str((Path(value["cwd"]) / value["source_arg"]).resolve()) == value["source"],
            "source alias resolution changed", "STALE")
    require(obj.is_file() and obj.stat().st_mtime_ns == value["object_mtime_ns"],
            "object was rebuilt/replaced after capture", "STALE")
    require(toolchain(value["toolchain"]["compiler"]) == value["toolchain"],
            "compiler/tool identity changed", "STALE")
    for key, path in (("object", obj), ("su", obj.with_suffix(".su")),
                      ("ci", obj.with_suffix(".ci"))):
        require(digest(path) == value["artifacts"][key], f"changed {key}: {path}", "STALE")
    current = dependency_snapshot(value["toolchain"]["compiler"],
                                  value["source_arg"], value["flags"], value["cwd"])
    require(current == value["dependencies"], "effective dependencies/macros/flags changed", "STALE")
    value["identity"] = identity
    return value


def read_su(path):
    rows = []
    for line in path.read_text().splitlines():
        fields = line.split("\t")
        require(len(fields) == 3 and fields[1].isdigit()
                and fields[2] in {"static", "dynamic", "dynamic,bounded"},
                f"unsupported .su row: {line}")
        location = re.fullmatch(r"(.*):(\d+):(\d+):(.+)", fields[0])
        require(location is not None, "missing .su location/signature")
        rows.append({"location": ":".join(location.groups()[:3]), "display": location[4],
                     "bytes": integer(int(fields[1]), "frame bytes"), "qualifier": fields[2]})
    require(rows, "empty .su does not prove zero stack")
    return rows


def read_ci(path):
    # Only the GCC 15.2 line-oriented VCG dialect exercised by the real fixtures.
    lines = path.read_text().splitlines()
    require(len(lines) >= 2 and lines[-1] == "}", "truncated .ci")
    opening = re.fullmatch(r"graph: \{ title: (" + QUOTE + ")", lines[0])
    require(opening is not None, "unknown VCG header")
    source, nodes, edges = json.loads(opening[1]), {}, []
    for line in lines[1:-1]:
        node = re.fullmatch(r"node: \{ title: (" + QUOTE + r") label: (" + QUOTE
                            + r")(?: shape : (ellipse|triangle))? \}", line)
        edge = re.fullmatch(r"edge: \{ sourcename: (" + QUOTE + r") targetname: ("
                            + QUOTE + r")(?: label: (" + QUOTE + r"))? \}", line)
        if node:
            title, label = json.loads(node[1]), json.loads(node[2]).split("\n")
            require(title not in nodes and 1 <= len(label) <= 3, "duplicate/unknown VCG node")
            symbol = title[len(source) + 1:] if title.startswith(source + ":") else title
            value = {"symbol": symbol, "display": label[0], "shape": node[3],
                     "location": label[1] if len(label) >= 2 else None, "stack": None}
            if len(label) == 3:
                stack = re.fullmatch(r"(\d+) bytes \((static|dynamic|dynamic,bounded)\)", label[2])
                require(stack is not None, "unknown GCC stack decoration")
                value["stack"] = {"bytes": integer(int(stack[1]), "frame bytes"),
                                  "qualifier": stack[2]}
            nodes[title] = value
        elif edge:
            edges.append((json.loads(edge[1]), json.loads(edge[2])))
        else:
            raise BudgetError("ERROR", f"unknown VCG syntax: {line}")
    require(nodes, "empty call graph")
    require(all(a in nodes and b in nodes for a, b in edges), "VCG edge without node")
    require(len({n["symbol"] for n in nodes.values()}) == len(nodes),
            "ambiguous normalized VCG linkage")
    return {v["symbol"]: v for v in nodes.values()}, {
        (nodes[a]["symbol"], nodes[b]["symbol"]) for a, b in edges}


def read_nm(path, allow_ambiguous=False):
    result = {}
    for line in run(["nm", "-P", "-S", str(path)]).splitlines():
        fields = line.split()
        require(2 <= len(fields) <= 4, "malformed nm input")
        if fields[0] in result:
            require(allow_ambiguous, "ambiguous nm linkage")
            result[fields[0]] = {"kind": "?", "address": None, "size": None}
            continue
        result[fields[0]] = {"kind": fields[1], "address": fields[2] if len(fields) > 2 else None,
                             "size": fields[3] if len(fields) > 3 else None}
    return result


def assembly(path):
    text = run(["objdump", "-drw", "--no-show-raw-insn", str(path)])
    blocks, symbol = {}, None
    for line in text.splitlines():
        header = re.fullmatch(r"([0-9a-f]+) <([^>]+)>:", line)
        if header:
            symbol = header[2]
            require(symbol not in blocks, "duplicate disassembly block")
            blocks[symbol] = {"transfers": [], "instructions": []}
            continue
        inst = re.match(r"^\s*([0-9a-f]+):\s+([a-z][a-z0-9.]*)\s*(.*)$", line)
        if not inst or symbol is None:
            continue
        _, mnemonic, operand = inst.groups()
        blocks[symbol]["instructions"].append((mnemonic, operand))
        # GNU objdump prints CET/MPX transfer prefixes as separate mnemonics.
        # Do not mistake a prefixed indirect transfer for an ordinary instruction.
        if mnemonic in {"notrack", "bnd"}:
            blocks[symbol]["transfers"].append({"symbol": None, "indirect": True,
                                                "instruction": mnemonic, "offset": False})
            continue
        # Conditional branches into other fragments matter too, not only calls.
        if mnemonic not in {"call", "callq"} and not mnemonic.startswith("j"):
            continue
        indirect = operand.lstrip().startswith("*")
        reloc = re.search(r"\b(R_X86_64_\w+)\s+(\S+)", operand)
        label = re.search(r"<([^>]+)>", operand)
        target, offset = None, False
        if not indirect and reloc:
            target = re.sub(r"[+-]0x[0-9a-f]+$", "", reloc[2])
        elif not indirect and label:
            target = label[1]
            if "+" in target:
                target, offset = target.split("+", 1)[0], True
        if target == symbol and offset and not mnemonic.startswith("call"):
            continue
        blocks[symbol]["transfers"].append({"symbol": target, "indirect": indirect,
                                            "instruction": mnemonic, "offset": offset})
    require(blocks, "missing machine code (LTO/unsupported object)", "UNSUPPORTED")
    return blocks


def read_object(obj):
    receipt = check_receipt(obj)
    sections = run(["objdump", "-h", str(obj)])
    require(".gnu.lto_" not in sections, "LTO object is unsupported", "UNSUPPORTED")
    nodes, graph = read_ci(obj.with_suffix(".ci"))
    rows, nm, blocks = read_su(obj.with_suffix(".su")), read_nm(obj), assembly(obj)
    for symbol, node in nodes.items():
        if node["stack"] is None:
            continue
        matches = [r for r in rows if (r["location"], r["display"]) ==
                   (node["location"], node["display"])]
        require(len(matches) == 1, f"missing/ambiguous .su association: {symbol}")
        require(node["stack"] == {k: matches[0][k] for k in ("bytes", "qualifier")},
                f".su/.ci mismatch: {symbol}")
        require(symbol in nm and nm[symbol]["kind"] in "TtWw",
                f"stack node has no emitted linkage: {symbol}")
    for row in rows:
        require(sum((n["location"], n["display"]) == (row["location"], row["display"])
                    and n["stack"] is not None for n in nodes.values()) == 1,
                f"unmapped/ambiguous .su row: {row['display']}")
    return {"path": obj, "receipt": receipt, "nodes": nodes, "graph": graph,
            "nm": nm, "blocks": blocks}


def link_evidence(elf, link_map, objects, entry_symbols, link_directory=None):
    require(elf.is_file() and link_map.is_file(), "final ELF and GNU ld map required", "UNKNOWN")
    header = run(["objdump", "-f", str(elf)])
    program = run(["objdump", "-p", str(elf)])
    require("file format elf64-x86-64" in header and "HAS_SYMS" in header
            and ("EXEC_P" in header or "INTERP" in program),
            "requires an unstripped x86_64 executable", "UNSUPPORTED")
    text = link_map.read_text()
    require("Linker script and memory map" in text, "unsupported linker map dialect", "UNSUPPORTED")
    link_directory = Path(link_directory or link_map.parent).resolve()
    loads = {str((link_directory / line[5:].strip()).resolve())
             for line in text.splitlines() if line.startswith("LOAD ")}
    require(loads and all(Path(p).is_file() for p in loads),
            "link input is missing, or link working directory does not match the map", "STALE")
    for obj in objects:
        require(str(obj) in loads, f"analyzed object not loaded by final link: {obj}", "STALE")
    nm = read_nm(elf, allow_ambiguous=True)
    for symbol in entry_symbols:
        require(symbol in nm and nm[symbol]["kind"] in "Tt",
                f"entry not uniquely emitted in ELF: {symbol}", "UNKNOWN")
    runtime, loader = {}, None
    if "INTERP" in program:
        require(not os.environ.get("LD_PRELOAD") and not os.environ.get("LD_AUDIT"),
                "LD_PRELOAD/LD_AUDIT interposition is unsupported", "UNSUPPORTED")
        interp = section_bytes(elf, ".interp")
        name = bytes(interp[k] for k in sorted(interp)).split(b"\0", 1)[0].decode("ascii")
        require(name == "/lib64/ld-linux-x86-64.so.2",
                "unvalidated ELF interpreter (not executed)", "UNSUPPORTED")
        loader_path = str(Path(name).resolve())
        # Trusted standard loader, dependency listing only: never run the entry/probe.
        listing = run([name, "--list", str(elf.resolve())])
        runtime[loader_path] = digest(loader_path)
        for line in listing.splitlines():
            if re.fullmatch(r"\s*linux-vdso\.so\.1 \(0x[0-9a-f]+\)", line):
                continue
            match = re.fullmatch(r"\s*(?:\S+ => )?(/.+?) \(0x[0-9a-f]+\)", line)
            require(match is not None, f"unresolved/unknown loader listing: {line}", "UNKNOWN")
            path = str(Path(match[1]).resolve())
            runtime[path] = digest(path)
        loader = {"path": loader_path, "sha256": digest(loader_path),
                  "LD_LIBRARY_PATH": os.environ.get("LD_LIBRARY_PATH", "")}
    return {"elf": str(elf), "elf_sha256": digest(elf),
            "map": str(link_map), "map_sha256": digest(link_map),
            "link_directory": str(link_directory),
            "program_headers_sha256": fingerprint(program),
            "runtime_files": runtime, "loader": loader,
            "loaded_files": {p: digest(p) for p in sorted(loads)}}


def symbol_table(path):
    result = {}
    for line in run(["objdump", "-t", str(path)]).splitlines():
        fields = line.split()
        if len(fields) >= 6 and "F" in fields:
            index = fields.index("F")
            name = fields[-1]
            value = (fields[index + 1], int(fields[0], 16), int(fields[index + 2], 16))
            result[name] = value if name not in result else None
    return result


def section_bytes(path, section):
    result = {}
    for line in run(["objdump", "-s", "-j", section, str(path)]).splitlines():
        match = re.match(r"^ ([0-9a-f]+) ((?:[0-9a-f]{2,8} ){1,4})", line)
        if match:
            data = bytes.fromhex(match[2].replace(" ", ""))
            result.update((int(match[1], 16) + i, b) for i, b in enumerate(data))
    return result


def verify_selection(obj, symbol, elf, link):
    """Match actual function bytes, masking only known ELF relocation fields."""
    left, right = symbol_table(obj), symbol_table(elf)
    require(left.get(symbol) is not None and right.get(symbol) is not None,
            f"missing/ambiguous final function: {symbol}", "UNKNOWN")
    section, address, size = left[symbol]
    final_section, final_address, final_size = right[symbol]
    require(size > 0 and size == final_size, f"link selection/size mismatch: {symbol}", "UNKNOWN")
    # Equal masked bytes alone do not identify a COMDAT provider. Independently
    # require the final GNU map to place THIS object's input section at the
    # address from the actual ELF symbol table.
    pending, placements = None, []
    for line in Path(link["map"]).read_text().splitlines():
        wrapped = re.fullmatch(r" (\.\S+)", line)
        if wrapped:
            pending = wrapped[1]
            continue
        full = re.fullmatch(r" (\.\S+)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.+)", line)
        continuation = re.fullmatch(r"\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(.+)", line)
        if full:
            input_section, base, length, input_path = full.groups()
        elif pending and continuation:
            input_section, (base, length, input_path) = pending, continuation.groups()
        else:
            pending = None
            continue
        pending = None
        input_path = str((Path(link["link_directory"]) / input_path).resolve())
        if input_section == section and input_path == str(obj.resolve()):
            placements.append((int(base, 16), int(length, 16)))
    require(sum(base + address == final_address and address + size <= length
                for base, length in placements) == 1,
            f"final map does not uniquely select this input section: {symbol}", "UNKNOWN")
    a, b = section_bytes(obj, section), section_bytes(elf, final_section)
    masks = set()
    for line in run(["objdump", "-r", "-j", section, str(obj)]).splitlines():
        reloc = re.fullmatch(r"([0-9a-f]+)\s+(R_X86_64_\w+)\s+(.+)", line)
        if not reloc:
            continue
        offset = int(reloc[1], 16)
        if not address <= offset < address + size:
            continue
        widths = {"R_X86_64_PC32": 4, "R_X86_64_PLT32": 4,
                  "R_X86_64_32": 4, "R_X86_64_32S": 4, "R_X86_64_64": 8}
        require(reloc[2] in widths, f"unvalidated final relocation: {reloc[2]}", "UNKNOWN")
        masks.update(range(offset - address, offset - address + widths[reloc[2]]))
    require(all(address + i in a and final_address + i in b for i in range(size)),
            f"incomplete final bytes: {symbol}", "UNKNOWN")
    require(all(a[address + i] == b[final_address + i] for i in range(size) if i not in masks),
            f"final ELF does not reuse analyzed function bytes: {symbol}", "STALE")
    return {"symbol": symbol, "size": size, "object_sha256": digest(obj),
            "input_section": section, "map_selected_object": str(obj.resolve()),
            "non_relocated_bytes_sha256": fingerprint(
                [a[address + i] for i in range(size) if i not in masks])}


def manifest_inputs(path):
    manifest = load(path)
    require(manifest.get("schema_version") == SCHEMA, "unsupported manifest schema", "UNSUPPORTED")
    require(isinstance(manifest.get("target"), str) and manifest["target"], "target name required")
    integer(manifest.get("page_size"), "page_size", True)
    integer(manifest.get("margin_bytes"), "margin_bytes")
    objects = {}
    for item in manifest.get("objects", []):
        require(isinstance(item.get("id"), str) and item["id"] not in objects, "duplicate object id")
        objects[item["id"]] = read_object(Path(item["path"]).resolve())
    require(objects, "explicit analysis objects required")
    names = set()
    for entry in manifest.get("entries", []):
        require(re.fullmatch(r"[A-Za-z_][A-Za-z_0-9]*", entry.get("name", "")) is not None,
                "entry name must be a stable C++ identifier")
        require(entry["name"] not in names, "duplicate stable entry name")
        names.add(entry["name"])
        require(entry.get("object") in objects, "entry requires an explicit object id")
        obj, symbol = objects[entry["object"]], entry.get("symbol")
        require(symbol in obj["nodes"] and obj["nodes"][symbol]["stack"] is not None,
                "entry must select one exact emitted linkage symbol")
        integer(entry.get("usable_bytes"), "usable_bytes", True)
        require(entry["usable_bytes"] % manifest["page_size"] == 0,
                "usable_bytes must be the current page-aligned U")
        add(entry["usable_bytes"], manifest["page_size"])
    require(names, "explicit entries required; demangled/overload matching is not supported")
    forbidden = manifest.get("generated_header")
    if forbidden:
        forbidden = str(Path(forbidden).resolve())
        require(all(forbidden not in o["receipt"]["dependencies"]["files"] for o in objects.values()),
                "generated header is inside the analysis closure", "UNSUPPORTED")
    return manifest, objects


def bind_assertion(template_path, manifest_path, output):
    manifest, objects = manifest_inputs(manifest_path)
    value = load(template_path)
    require(not {"object_identities", "bindings", "template", "template_sha256",
                 "schema_version", "target"}.intersection(value), "reserved assertion template field")
    require(value.get("provenance") == "user_asserted" and value.get("source")
            and value.get("assumptions") and value.get("dependency_versions")
            and value.get("configuration"), "manual assertion needs source/version/configuration/assumptions")
    for obj in objects.values():
        require(obj["receipt"]["toolchain"]["version"] == value.get("compiler_version")
                and obj["receipt"]["toolchain"]["machine"] == value.get("machine"),
                "assertion compiler/target mismatch", "STALE")
        require(all(f in obj["receipt"]["flags"] for f in value.get("required_flags", [])),
                "assertion configuration flags mismatch", "STALE")
    value["object_identities"] = {key: obj["receipt"]["identity"] for key, obj in objects.items()}
    value["target"] = manifest["target"]
    value["bindings"] = {str(Path(p).resolve()): digest(p) for p in value.get("binding_files", [])}
    require(value["bindings"], "manual runtime assertion must bind actual dependency files")
    value["template"] = str(Path(template_path).resolve())
    value["template_sha256"] = digest(template_path)
    value["schema_version"] = SCHEMA
    save(output, value)


def assertion_inputs(manifest, objects):
    if not manifest.get("assertion"):
        return None
    value = load(manifest["assertion"])
    require(value.get("schema_version") == SCHEMA and value.get("provenance") == "user_asserted",
            "invalid assertion provenance/schema")
    require(value.get("target") == manifest["target"] and value.get("object_identities") ==
            {key: obj["receipt"]["identity"] for key, obj in objects.items()},
            "assertion no longer describes these analysis objects", "STALE")
    require(digest(value["template"]) == value["template_sha256"],
            "manual assertion source changed; explicitly rebind/reanalyze", "STALE")
    template = load(value["template"])
    require(all(value.get(key) == item for key, item in template.items()),
            "bound assertion was edited independently of its source", "STALE")
    for path, expected in value["bindings"].items():
        require(digest(path) == expected, f"manual assertion dependency changed: {path}", "STALE")
    value["coverage_gaps"] = sorted(REQUIRED_COVERAGE - set(value.get("covers", [])))
    integer(value.get("runtime_extra_bytes"), "runtime_extra_bytes", True)
    integer(value.get("abi_extra_per_frame_bytes"), "abi_extra_per_frame_bytes", True)
    runtime_symbols = value.get("runtime_symbols", [])
    keys = [(item["object"], item["symbol"]) for item in runtime_symbols]
    require(len(keys) == len(set(keys)) and all(owner in objects and symbol in objects[owner]["nm"]
            for owner, symbol in keys), "runtime_symbols must select explicit frozen wrapper linkages")
    for symbol, count in value.get("external", {}).items():
        require(isinstance(symbol, str) and symbol, "invalid external linkage assertion")
        integer(count, "external model bytes", True)
    return value


def runtime_object_selection(assertion, objects, elf, link):
    result = {}
    if not assertion:
        return result
    for item in assertion.get("runtime_symbols", []):
        owner, symbol = item["object"], item["symbol"]
        result[f"{owner}::{symbol}"] = verify_selection(objects[owner]["path"], symbol, elf, link)
    return result


def aggregate(entry, objects, assertion, elf=None, link=None):
    unknown, visited, active, selected = set(), set(), set(), {}
    cache = {}
    external = assertion.get("external", {}) if assertion else {}

    def resolve(owner, symbol):
        local = objects[owner]["nm"].get(symbol)
        if local and local["kind"] in "TtWw":
            return owner, symbol
        matches = [(key, symbol) for key, obj in objects.items()
                   if obj["nm"].get(symbol, {}).get("kind") in {"T", "W"}]
        if len(matches) == 1:
            return matches[0]
        return None

    def walk(key):
        owner, symbol = key
        obj = objects[owner]
        node = obj["nodes"].get(symbol)
        visited.add(key)
        if key in active:
            unknown.add(f"{owner}::{symbol}: recursive SCC (no depth assertion supported)")
            return 0, 0, [], 0, []
        if key in cache:
            return cache[key]
        if not node or not node["stack"] or symbol not in obj["blocks"]:
            unknown.add(f"{owner}::{symbol}: missing stack/machine metadata (cold/alias gap)")
            return 0, 0, [], 0, []
        frame = node["stack"]["bytes"]
        if node["stack"]["qualifier"] != "static":
            unknown.add(f"{owner}::{symbol}: {node['stack']['qualifier']} stack")
        if obj["nm"][symbol]["kind"] in "Ww":
            unknown.add(f"{owner}::{symbol}: COMDAT/weak selection unsupported")
        graph = {b for a, b in obj["graph"] if a == symbol}
        machine = set()
        for transfer in obj["blocks"][symbol]["transfers"]:
            if transfer["indirect"] or not transfer["symbol"]:
                unknown.add(f"{owner}::{symbol}: indirect/unresolved machine transfer")
            else:
                machine.add(transfer["symbol"])
                if transfer["offset"]:
                    unknown.add(f"{owner}::{symbol}: non-entry call/fragment transfer")
        for missing in sorted(graph ^ machine):
            unknown.add(f"{owner}::{symbol} -> {missing}: VCG/machine edge mismatch")
        if elf:
            try:
                selected[f"{owner}::{symbol}"] = verify_selection(obj["path"], symbol, elf, link)
            except BudgetError as exc:
                if exc.status == "STALE":
                    raise
                unknown.add(f"{owner}::{symbol}: {exc}")
        active.add(key)
        children = []
        for target in sorted(graph | machine):
            resolved = resolve(owner, target)
            if resolved:
                children.append(walk(resolved))
            elif target in external:
                children.append((0, external[target], [
                    {"object": None, "symbol": target, "bytes": external[target],
                     "provenance": "user_asserted"}], 1, []))
            else:
                unknown.add(f"{owner}::{symbol} -> {target}: missing metadata/summary")
        active.remove(key)
        raw = max(children, key=lambda x: x[0], default=(0, 0, [], 0, []))
        best = max(children, key=lambda x: x[1], default=(0, 0, [], 0, []))
        abi = assertion["abi_extra_per_frame_bytes"] if assertion else 0
        path = [{"object": owner, "symbol": symbol, "bytes": frame, "provenance": "compiler"}]
        result = (add(frame, raw[0]), add(frame, abi, best[1]), path + best[2],
                  1 + best[3], path + raw[4])
        cache[key] = result
        return result

    known, conditional, path, depth, static_path = walk((entry["object"], entry["symbol"]))
    if not assertion:
        unknown.add("runtime/ABI/EH/stack_chk coverage is not asserted or compiler-proven")
    else:
        unknown.update("manual coverage gap: " + gap for gap in assertion["coverage_gaps"])
    return known, conditional, path, sorted(unknown), sorted(visited), selected, depth, static_path


def assertion_link_evidence(assertion, link, references):
    if not assertion or not link:
        return {}
    actual = {**link["loaded_files"], **link["runtime_files"]}
    if link["loader"]:
        require(link["loader"]["path"] in assertion["bindings"],
                "manual runtime condition must bind the selected dynamic loader", "UNKNOWN")
    for path, expected in assertion["bindings"].items():
        dynamic = "DYNAMIC" in run(["objdump", "-f", path])
        selected = link["runtime_files"] if dynamic else actual
        require(selected.get(path) == expected,
                f"asserted dependency is not an actual matching link input: {path}", "STALE")
    used = set(assertion.get("external", {})).intersection(references)
    providers = {symbol: [] for symbol in used}
    dynamic_files = {}
    for path in set(assertion["bindings"]) | set(link["runtime_files"]):
        dynamic = "DYNAMIC" in run(["objdump", "-f", path])
        dynamic_files[path] = dynamic
        text = run(["nm", *(["-D"] if dynamic else []), "-P", "--defined-only", path])
        seen = {}
        for line in text.splitlines():
            fields = line.split()
            if len(fields) not in {3, 4}:
                continue
            symbol = fields[0].split("@", 1)[0]
            if symbol in used:
                seen.setdefault(symbol, set()).add((fields[1], fields[2]))
        for symbol, emitted in seen.items():
            if len(emitted) == 1 and next(iter(emitted))[0] == "T":
                providers[symbol].append(path)
    require(all(len(paths) == 1 and paths[0] in assertion["bindings"]
                for paths in providers.values()),
            "external assertion lacks a unique strong emitted provider (IFUNC/weak/version ambiguity)",
            "UNKNOWN")
    executable_symbols = read_nm(Path(link["elf"]), allow_ambiguous=True)
    for symbol, paths in providers.items():
        provider = Path(paths[0])
        emitted = executable_symbols.get(symbol, {}).get("kind")
        if dynamic_files[paths[0]]:
            require(emitted not in {"T", "t", "W", "w", "I", "i", "?"},
                    "executable interposes an asserted dynamic runtime symbol", "UNKNOWN")
        else:
            require(emitted == "T",
                    f"final ELF lacks a strong static external provider: {symbol}", "UNKNOWN")
            # LOAD of an archive is not evidence that its defining member was
            # extracted. Until member selection is supported, keep it UNKNOWN.
            with provider.open("rb") as stream:
                archive = stream.read(8) in {b"!<arch>\n", b"!<thin>\n"}
            require(not archive,
                    f"static archive external provider selection unsupported: {symbol}", "UNKNOWN")
            verify_selection(provider, symbol, Path(link["elf"]), link)
    return {symbol: paths[0] for symbol, paths in providers.items()}


def link_dependencies(link, objects, assertion, glue=()):
    """Freeze support inputs too, without making the disposable main TU part of F."""
    analyzed = {str(o["path"]) for o in objects.values()}
    excluded = analyzed | {str(Path(p).resolve()) for p in glue}
    bound = set(assertion["bindings"]) if assertion else set()
    crt = set()
    for compiler in {o["receipt"]["toolchain"]["compiler"] for o in objects.values()}:
        for name in ("Scrt1.o", "crt1.o", "crti.o", "crtbegin.o", "crtbeginS.o",
                     "crtend.o", "crtendS.o", "crtn.o"):
            path = run([compiler, "-print-file-name=" + name]).strip()
            if Path(path).is_file():
                crt.add(str(Path(path).resolve()))
    result = {}
    for path, expected in {**link["loaded_files"], **link["runtime_files"]}.items():
        if path in excluded:
            continue
        if path.endswith(".o") and path not in crt and path not in bound:
            emitted = {s for s, value in read_nm(Path(path)).items() if value["kind"] in "TtWw"}
            if emitted == {"main"}:
                continue
            require(not emitted, f"unregistered application object in link closure: {path}", "UNKNOWN")
        result[path] = expected
    return result


def identity_inputs(manifest, objects, assertion, selection, dependencies=None):
    return {"schema_version": SCHEMA, "engine_sha256": digest(__file__),
            "python_version": platform.python_version(),
            "manifest": manifest, "objects": {key: obj["receipt"]["identity"]
                                            for key, obj in objects.items()},
            "assertion": assertion, "selection": selection, "link_dependencies": dependencies or {}}


def analyze(manifest_path, elf=None, link_map=None, link_directory=None):
    manifest, objects = manifest_inputs(manifest_path)
    assertion = assertion_inputs(manifest, objects)
    link, link_unknown = None, None
    if elf and link_map:
        try:
            link = link_evidence(elf, link_map, [o["path"] for o in objects.values()],
                                 [e["symbol"] for e in manifest["entries"]], link_directory)
        except BudgetError as exc:
            if exc.status == "STALE":
                raise
            link_unknown = str(exc)
    else:
        link_unknown = "final ELF/link-selection evidence is missing"
    entries, selection, references = [], {}, set()
    for entry in manifest["entries"]:
        known, conditional, path, unknown, visited, selected, depth, static_path = aggregate(
            entry, objects, assertion, elf if link else None, link)
        selection.update(selected)
        if link_unknown:
            unknown.append(link_unknown)
        for owner, symbol in visited:
            references.add(symbol)
            references.update(b for a, b in objects[owner]["graph"] if a == symbol)
        total = None
        recommendation = None
        if not unknown:
            total = add(conditional, assertion["runtime_extra_bytes"], manifest["margin_bytes"])
            recommendation = align(total, manifest["page_size"])
            add(recommendation, manifest["page_size"])
        status = ("EXCEEDS" if known > entry["usable_bytes"]
                  or (recommendation is not None and recommendation > entry["usable_bytes"])
                  else "UNKNOWN" if unknown else "WITHIN_MODEL")
        entries.append({**entry, "status": status, "static_known_path_bytes": known,
                        "conditional_budget_bytes": total, "recommended_usable_bytes": recommendation,
                        "observed_sample_bytes": None, "unknown_edges": sorted(set(unknown)),
                        "critical_path": path, "static_critical_path": static_path,
                        "modeled_frame_depth": depth,
                        "assumptions": [
                            "static_known_path_bytes is a compiler model, neither an actual lower nor upper bound",
                            "branches use max, not sum; emitted inline frames are never reconstructed",
                            "tail/conditional inter-function jumps conservatively add frames",
                            "only this exact non-LTO object/link selection and input identity apply",
                        ] + (assertion["assumptions"] if assertion else [])})
    dependencies = {}
    try:
        if link:
            selection.update(runtime_object_selection(assertion, objects, elf, link))
        external_providers = assertion_link_evidence(assertion, link, references)
        if link:
            dependencies = link_dependencies(link, objects, assertion)
    except BudgetError as exc:
        if exc.status != "UNKNOWN":
            raise
        external_providers = {}
        for entry in entries:
            entry["unknown_edges"].append(str(exc))
            entry["conditional_budget_bytes"] = None
            entry["recommended_usable_bytes"] = None
            entry["status"] = ("EXCEEDS" if entry["static_known_path_bytes"] > entry["usable_bytes"]
                               else "UNKNOWN")
    status = ("EXCEEDS" if any(e["status"] == "EXCEEDS" for e in entries)
              else "UNKNOWN" if any(e["status"] == "UNKNOWN" for e in entries) else "WITHIN_MODEL")
    inputs = identity_inputs(manifest, objects, assertion, selection, dependencies)
    report = {"schema_version": SCHEMA, "status": status, "entries": entries,
              "fingerprint": fingerprint(inputs), "inputs": inputs,
              "link_evidence": link, "reachable_references": sorted(references),
              "external_providers": external_providers,
              "provenance": "conditional_model_with_user_assertions" if assertion else "partial_compiler_model"}
    report["integrity"] = fingerprint(report)
    return report


def header_text(report):
    require(report["status"] == "WITHIN_MODEL"
            and all(e["status"] == "WITHIN_MODEL" for e in report["entries"]),
            "capacity constants require WITHIN_MODEL for every entry", "UNKNOWN")
    lines = ["#pragma once", "#include <cstddef>",
             "// Conditional model only; read the bound JSON assumptions before use.",
             "namespace bbt_stack_budget {",
             f'inline constexpr char kAnalysisFingerprint[] = "{report["fingerprint"]}";']
    for entry in report["entries"]:
        lines.append(f'inline constexpr std::size_t {entry["name"]} = '
                     f'{entry["recommended_usable_bytes"]}ULL;')
    return "\n".join(lines + ["}", ""])


def verify_report(report_path, manifest_path):
    report = load(report_path)
    integrity = report.pop("integrity", None)
    require(fingerprint(report) == integrity, "damaged model report")
    report["integrity"] = integrity
    manifest, objects = manifest_inputs(manifest_path)
    assertion = assertion_inputs(manifest, objects)
    dependencies = report["inputs"]["link_dependencies"]
    for path, expected in dependencies.items():
        require(digest(path) == expected, f"linked support dependency changed: {path}", "STALE")
    current = identity_inputs(manifest, objects, assertion, report["inputs"]["selection"], dependencies)
    require(fingerprint(current) == report["fingerprint"] and current == report["inputs"],
            "analysis inputs changed; explicitly reanalyze, do not rebuild a fixed-point loop", "STALE")
    return report, objects


def consume(report_path, manifest_path, header, elf, link_map, glue_objects, output, link_directory=None):
    report, objects = verify_report(report_path, manifest_path)
    require(str(header.resolve()) == report["inputs"]["manifest"].get("generated_header"),
            "header path differs from the analysis output identity", "STALE")
    require(header.read_text() == header_text(report), "generated header is old/damaged", "STALE")
    glue = {}
    for item in glue_objects:
        obj = Path(item).resolve()
        require(obj not in [o["path"] for o in objects.values()], "glue must not be an analyzed object")
        receipt = check_receipt(obj)
        require(str(header.resolve()) in receipt["dependencies"]["files"],
                "glue does not bind the generated header", "STALE")
        emitted = {s for s, v in read_nm(obj).items() if v["kind"] in "TtWw"}
        require(not emitted.intersection(report["reachable_references"]),
                "registration glue entered the coroutine-reachable closure", "UNSUPPORTED")
        require(emitted == {"main"},
                "generated-header glue may emit main only; freeze callable/template helpers "
                "in first-stage runtime objects instead", "UNSUPPORTED")
        glue[str(obj)] = receipt["identity"]
    require(glue, "actual glue compilation receipts required")
    link = link_evidence(elf, link_map, [o["path"] for o in objects.values()] + list(map(Path, glue)),
                         [e["symbol"] for e in report["entries"]], link_directory)
    require(assertion_link_evidence(report["inputs"]["assertion"], link,
                                    report["reachable_references"]) == report["external_providers"],
            "final runtime provider identity changed", "STALE")
    require(link_dependencies(link, objects, report["inputs"]["assertion"], glue) ==
            report["inputs"]["link_dependencies"],
            "consumer changed runtime/support link inputs outside frozen F", "STALE")
    for qualified, expected in report["inputs"]["selection"].items():
        owner, symbol = qualified.split("::", 1)
        require(verify_selection(objects[owner]["path"], symbol, elf, link) == expected,
                "consumer recompiled/replaced a reachable function", "STALE")
    credential = {"schema_version": SCHEMA, "status": "WITHIN_MODEL",
                  "analysis_fingerprint": report["fingerprint"], "header_sha256": digest(header),
                  "glue": glue, "final_link": link,
                  "assumptions": "same user_asserted runtime/ABI conditions; not a safety certificate"}
    credential["consumption_fingerprint"] = fingerprint(credential)
    save(output, credential)
    return credential


def parser():
    result = argparse.ArgumentParser(description=__doc__, epilog=(
        "Exit: 0 WITHIN_MODEL, 1 EXCEEDS, 2 UNKNOWN/STALE/ERROR/UNSUPPORTED. "
        "No SAFE status. GCC 15.2 Linux x86_64, non-LTO only. "
        "record executes an explicit local compiler argv; no compile_commands shell strings."))
    commands = result.add_subparsers(dest="command", required=True)
    p = commands.add_parser("record", help="compiler launcher: capture actual flags, dependencies, .su/.ci/object")
    p.add_argument("--launcher", help="optional explicit single executable (e.g. ccache), never a shell")
    p.add_argument("argv", nargs=argparse.REMAINDER)
    p = commands.add_parser("bind-assertion", help="explicitly bind a user_asserted template to exact built inputs")
    p.add_argument("--template", required=True, type=Path)
    p.add_argument("--manifest", required=True, type=Path)
    p.add_argument("--output", required=True, type=Path)
    p = commands.add_parser("analyze", help="report compiler model and unknowns; optionally emit guarded constants")
    p.add_argument("--manifest", required=True, type=Path)
    p.add_argument("--elf", type=Path)
    p.add_argument("--map", type=Path)
    p.add_argument("--link-directory", type=Path, help="actual link cwd (default: map parent)")
    p.add_argument("--output", required=True, type=Path)
    p.add_argument("--header", type=Path)
    p = commands.add_parser("consume", help="validate frozen F against actual final ELF, header and glue; write C")
    for name in ("report", "manifest", "header", "elf", "map", "output"):
        p.add_argument("--" + name, required=True, type=Path)
    p.add_argument("--glue-object", action="append", required=True, type=Path)
    p.add_argument("--link-directory", type=Path, help="actual link cwd (default: map parent)")
    return result


def main(argv=None):
    args = parser().parse_args(argv)
    try:
        if args.command == "record":
            record(args.argv[1:] if args.argv[:1] == ["--"] else args.argv, args.launcher)
            return 0
        if args.command == "bind-assertion":
            bind_assertion(args.template, args.manifest, args.output)
            return 0
        if args.command == "consume":
            value = consume(args.report, args.manifest, args.header, args.elf, args.map,
                            args.glue_object, args.output, args.link_directory)
        else:
            value = analyze(args.manifest, args.elf, args.map, args.link_directory)
            save(args.output, value)
            if args.header:
                require(str(args.header.resolve()) ==
                        value["inputs"]["manifest"].get("generated_header"),
                        "--header must match manifest generated_header")
                if value["status"] == "WITHIN_MODEL":
                    args.header.write_text(header_text(value))
        print(f'{value["status"]}: ' + value.get("fingerprint", value.get("analysis_fingerprint", "")))
        for entry in value.get("entries", []):
            print(f'  {entry["name"]}: model={entry["static_known_path_bytes"]} '
                  f'conditional={entry["conditional_budget_bytes"]} U={entry["usable_bytes"]} '
                  f'unknown={len(entry["unknown_edges"])}')
        return EXIT[value["status"]]
    except (BudgetError, OSError, ValueError, KeyError, TypeError, AttributeError,
            IndexError, RecursionError, subprocess.TimeoutExpired) as exc:
        status = exc.status if isinstance(exc, BudgetError) else "ERROR"
        value = {"schema_version": SCHEMA, "status": status, "error": str(exc),
                 "entries": [], "fingerprint": None, "conditional_budget_bytes": None,
                 "static_known_path_bytes": None, "observed_sample_bytes": None,
                 "unknown_edges": [str(exc)] if status == "UNKNOWN" else [],
                 "assumptions": [], "critical_path": [], "usable_bytes": None}
        if getattr(args, "output", None):
            save(args.output, value)
        print(f"{status}: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main())
