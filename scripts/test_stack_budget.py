#!/usr/bin/env python3
"""Real GCC O0/O2 integration tests; all artifacts stay under an ignored build dir."""

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
import stack_budget as sb


ROOT = Path(__file__).resolve().parents[1]
FIXTURES = ROOT / "unit_test" / "stack_budget_fixtures"


class StackBudgetTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        parent = Path(os.environ.get("STACK_BUDGET_TEST_ROOT", ROOT / "build-stack-budget-python"))
        parent.mkdir(parents=True, exist_ok=True)
        cls.work = Path(tempfile.mkdtemp(prefix="run-", dir=parent))
        cls.tmp = cls.work / "tmp"
        cls.tmp.mkdir()
        os.environ.update(TMPDIR=str(cls.tmp), TMP=str(cls.tmp), TEMP=str(cls.tmp))
        cls.compiler = str(Path(os.environ.get("STACK_BUDGET_TEST_COMPILER")
                               or shutil.which("g++") or "/missing/g++").resolve())
        cls.tc = sb.toolchain(cls.compiler)
        cls.commands = []
        cls.cases = {}
        for mode in ("O0", "O2"):
            directory = cls.work / mode
            directory.mkdir()
            shutil.copytree(FIXTURES, directory / "source")
            objects = []
            for name in ("scenarios", "left", "right"):
                obj = directory / (name + ".o")
                cls.compile(directory / "source" / (name + ".cc"), obj, mode)
                objects.append({"id": name, "path": str(obj)})
            prebuilt = directory / "prebuilt.o"
            cls.command([cls.compiler, "-" + mode, "-c", directory / "source/prebuilt.cc",
                         "-o", prebuilt])
            probe = directory / "probe"
            link_map = directory / "probe.map"
            probe_object = directory / "probe.o"
            cls.command([cls.compiler, "-c", directory / "source/probe.cc", "-o", probe_object])
            cls.command([cls.compiler, probe_object,
                         *[o["path"] for o in objects], prebuilt,
                         "-Wl,-Map=" + str(link_map), "-o", probe])
            cls.cases[mode] = {"directory": directory, "objects": objects,
                               "prebuilt": prebuilt, "elf": probe, "map": link_map}
        print("stack-budget evidence:", cls.work, flush=True)

    @classmethod
    def command(cls, argv, expected=0):
        argv = [str(x) for x in argv]
        proc = subprocess.run(argv, text=True, capture_output=True, timeout=120,
                              env={**os.environ, "LC_ALL": "C"})
        cls.commands.append({"argv": argv, "cwd": str(Path.cwd()), "exit_code": proc.returncode,
                             "stdout": proc.stdout, "stderr": proc.stderr})
        sb.save(cls.work / "commands.json", cls.commands)
        if proc.returncode != expected:
            raise AssertionError(f"{argv!r}: expected {expected}, got {proc.returncode}\n"
                                 + proc.stdout + proc.stderr)
        return proc

    @classmethod
    def compile(cls, source, obj, mode="O2", extra=()):
        cls.command([sys.executable, ROOT / "scripts/stack_budget.py", "record", "--",
                     cls.compiler, "-std=c++17", "-fno-rtti", "-" + mode,
                     "-fstack-usage", "-fcallgraph-info=su", "-Wall", "-Wextra",
                     *extra, "-c", source, "-o", obj])

    def manifest(self, symbol="DirectEntry", mode="O2", owner="scenarios", capacity=65536):
        case = self.cases[mode]
        path = case["directory"] / ("manifest-" + self._testMethodName + ".json")
        value = {"schema_version": 1, "target": "fixtures", "objects": case["objects"],
                 "entries": [{"name": "Entry", "object": owner, "symbol": symbol,
                              "usable_bytes": capacity}], "page_size": 4096, "margin_bytes": 4096}
        sb.save(path, value)
        return path, value

    def analyze(self, symbol="DirectEntry", mode="O2", owner="scenarios", capacity=65536):
        path, _ = self.manifest(symbol, mode, owner, capacity)
        case = self.cases[mode]
        return sb.analyze(path, case["elf"], case["map"])

    def assertion(self, path, manifest):
        case = self.cases["O2"]
        template = path.with_suffix(".assertion-template.json")
        output = path.with_suffix(".assertion.json")
        value = {
            "provenance": "user_asserted", "source": "test-only mathematical premises, not measured bounds",
            "assumptions": ["Test fixture premise only; no production stack guarantee.",
                            "Exact bound binaries, no preload/interposition, ordinary SysV ABI."],
            "dependency_versions": {"gcc": self.tc["version"], "fixture": "1"},
            "configuration": "C++17 non-LTO O2 fixture", "compiler_version": self.tc["version"],
            "machine": self.tc["machine"], "required_flags": ["-O2", "-std=c++17"],
            "binding_files": [str(case["prebuilt"]),
                              sb.run([self.compiler, "-print-file-name=libc.so.6"]).strip(),
                              "/lib64/ld-linux-x86-64.so.2"],
            "covers": sorted(sb.REQUIRED_COVERAGE),
            "runtime_extra_bytes": 8192, "abi_extra_per_frame_bytes": 256,
            "external": {"__stack_chk_fail": 2048},
        }
        sb.save(template, value)
        manifest["assertion"] = str(output)
        sb.save(path, manifest)
        sb.bind_assertion(template, path, output)
        return output

    def simple_case(self):
        directory = self.work / self._testMethodName
        directory.mkdir()
        dependency = directory / "dep.hpp"
        dependency.write_text("#define VALUE 1\n")
        source, obj = directory / "entry.cc", directory / "entry.o"
        source.write_text('#include "dep.hpp"\n#ifndef ENTRY_MACRO\n#define ENTRY_MACRO 1\n#endif\n'
                          'extern "C" int Entry(int x) { return x + VALUE + ENTRY_MACRO; }\n')
        self.compile(source, obj)
        probe_source = directory / "probe.cc"
        probe_source.write_text('extern "C" int Entry(int);\n'
                                'int main(int argc, char**) { return Entry(argc) == 0; }\n')
        probe_object = directory / "probe.o"
        self.command([self.compiler, "-c", probe_source, "-o", probe_object])
        elf, link_map = directory / "probe", directory / "probe.map"
        self.command([self.compiler, probe_object, obj, self.cases["O2"]["prebuilt"],
                      "-Wl,-Map=" + str(link_map), "-o", elf])
        path, header, output = directory / "manifest.json", directory / "budget.hpp", directory / "report.json"
        value = {"schema_version": 1, "target": "fixtures", "objects": [{"id": "entry", "path": str(obj)}],
                 "entries": [{"name": "Entry", "object": "entry", "symbol": "Entry",
                              "usable_bytes": 65536}], "page_size": 4096, "margin_bytes": 4096,
                 "generated_header": str(header)}
        self.assertion(path, value)
        report = sb.analyze(path, elf, link_map)
        self.assertEqual(report["status"], "WITHIN_MODEL", report["entries"])
        sb.save(output, report)
        header.write_text(sb.header_text(report))
        return directory, source, dependency, obj, path, value, elf, link_map, output, report

    def test_direct_chain_real_O0_O2(self):
        for mode in self.cases:
            with self.subTest(mode=mode):
                report = self.analyze(mode=mode)
                entry = report["entries"][0]
                obj = sb.read_object(Path(self.cases[mode]["objects"][0]["path"]))
                expected = sum(obj["nodes"][s]["stack"]["bytes"] for s in
                               ("DirectEntry", "DirectMiddle", "DirectLeaf"))
                self.assertEqual(entry["static_known_path_bytes"], expected)
                self.assertEqual(entry["status"], "UNKNOWN")
                self.assertIsNone(entry["conditional_budget_bytes"])
                self.assertEqual([p["symbol"] for p in entry["critical_path"]],
                                 ["DirectEntry", "DirectMiddle", "DirectLeaf"])

    def test_branch_max_not_sum(self):
        for mode in self.cases:
            entry = self.analyze("BranchEntry", mode)["entries"][0]
            obj = sb.read_object(Path(self.cases[mode]["objects"][0]["path"]))
            sizes = {s: obj["nodes"][s]["stack"]["bytes"]
                     for s in ("BranchEntry", "BranchLeft", "BranchRight")}
            self.assertEqual(entry["static_known_path_bytes"],
                             sizes["BranchEntry"] + max(sizes["BranchLeft"], sizes["BranchRight"]))
            self.assertLess(entry["static_known_path_bytes"], sum(sizes.values()))

    def test_cross_TU_and_static_object_identity(self):
        for mode in self.cases:
            cross = self.analyze("CrossEntry", mode)["entries"][0]
            self.assertTrue(any(p["object"] == "left" for p in cross["critical_path"]))
            left = self.analyze("LeftEntry", mode, "left")["entries"][0]
            right = self.analyze("RightEntry", mode, "right")["entries"][0]
            self.assertTrue(any(p["symbol"] == "_ZL9LocalSamei" and p["object"] == "left"
                                for p in left["critical_path"]))
            self.assertTrue(any(p["symbol"] == "_ZL9LocalSamei" and p["object"] == "right"
                                for p in right["critical_path"]))
            self.assertNotEqual(left["static_known_path_bytes"], right["static_known_path_bytes"])

    def test_unknowns_never_become_zero_or_constants(self):
        for mode in self.cases:
            for symbol, reason in (("RecursiveEntry", "recursive"), ("DynamicEntry", "dynamic"),
                                   ("VirtualEntry", "indirect"), ("PrebuiltEntry", "ThirdPartyOpaque"),
                                   ("RuntimeWrapper", "indirect"),
                                   ("ExceptionEntry", "coverage")):
                with self.subTest(mode=mode, symbol=symbol):
                    entry = self.analyze(symbol, mode)["entries"][0]
                    self.assertEqual(entry["status"], "UNKNOWN")
                    self.assertIsNone(entry["conditional_budget_bytes"])
                    self.assertIsNone(entry["recommended_usable_bytes"])
                    self.assertGreater(entry["static_known_path_bytes"], 0)
                    self.assertTrue(any(reason in x for x in entry["unknown_edges"]))
                    with self.assertRaises(sb.BudgetError):
                        sb.header_text({"status": "UNKNOWN", "entries": [entry]})

    def test_overload_selection_is_exact_linkage(self):
        path, value = self.manifest("_Z8Overloadi")
        self.assertGreater(sb.analyze(path)["entries"][0]["static_known_path_bytes"], 0)
        value["entries"][0]["symbol"] = "Overload"
        sb.save(path, value)
        with self.assertRaisesRegex(sb.BudgetError, "exact emitted linkage"):
            sb.analyze(path)
        value["entries"][0].pop("object")
        sb.save(path, value)
        with self.assertRaisesRegex(sb.BudgetError, "explicit object"):
            sb.analyze(path)

    def test_inline_and_tail_follow_emitted_code(self):
        o0 = sb.read_object(Path(self.cases["O0"]["objects"][0]["path"]))
        o2 = sb.read_object(Path(self.cases["O2"]["objects"][0]["path"]))
        self.assertIn("_Z15InlineCandidatei", o0["nodes"])
        self.assertNotIn("_Z15InlineCandidatei", o2["nodes"])
        tail = o2["blocks"]["TailEntry"]["transfers"]
        self.assertTrue(any(t["instruction"].startswith("j") and t["symbol"] == "TailTarget"
                            for t in tail))
        for mode in self.cases:
            entry = self.analyze("TailEntry", mode)["entries"][0]
            obj = o0 if mode == "O0" else o2
            self.assertEqual(entry["static_known_path_bytes"],
                             sum(obj["nodes"][s]["stack"]["bytes"] for s in ("TailEntry", "TailTarget")))

    def test_switch_jump_table_indirect_prefix_is_not_silently_ignored(self):
        # Actual GCC O0 emits a comparison tree; O2 emits the CET-prefixed
        # jump table. Assert the transfer on the build where it really exists.
        obj = sb.read_object(Path(self.cases["O2"]["objects"][0]["path"]))
        self.assertTrue(any(t["indirect"] and t["instruction"] == "notrack"
                            for t in obj["blocks"]["SwitchEntry"]["transfers"]))
        entry = self.analyze("SwitchEntry", "O2")["entries"][0]
        self.assertTrue(any("indirect" in edge for edge in entry["unknown_edges"]))
        self.assertEqual(entry["status"], "UNKNOWN")

    def test_runtime_boundary_and_final_ELF_are_required(self):
        path, value = self.manifest()
        partial = sb.analyze(path)["entries"][0]
        self.assertTrue(any("runtime/ABI" in x for x in partial["unknown_edges"]))
        self.assertTrue(any("final ELF" in x for x in partial["unknown_edges"]))
        self.assertIsNone(partial["conditional_budget_bytes"])
        self.assertIsNone(partial["observed_sample_bytes"])
        self.assertEqual(partial["usable_bytes"], value["entries"][0]["usable_bytes"])

    def test_integer_overflow_and_rounding(self):
        for bad in (True, -1, "4096", sb.LIMIT + 1):
            with self.assertRaises(sb.BudgetError):
                sb.integer(bad, "test")
        with self.assertRaisesRegex(sb.BudgetError, "overflow"):
            sb.align(sb.LIMIT, 4096)
        with self.assertRaisesRegex(sb.BudgetError, "overflow"):
            sb.add(sb.LIMIT, 1)
        self.assertEqual(sb.align(4097, 4096), 8192)
        path, value = self.manifest()
        value["margin_bytes"] = sb.LIMIT + 1
        sb.save(path, value)
        with self.assertRaises(sb.BudgetError):
            sb.analyze(path)

    def test_conditional_model_and_EXCEEDS_exit_codes(self):
        case = self.cases["O2"]
        path, value = self.manifest()
        self.assertion(path, value)
        report = sb.analyze(path, case["elf"], case["map"])
        self.assertEqual(report["status"], "WITHIN_MODEL", report["entries"])
        entry = report["entries"][0]
        self.assertGreater(entry["conditional_budget_bytes"], entry["static_known_path_bytes"])
        self.assertEqual(entry["recommended_usable_bytes"] % 4096, 0)
        self.assertEqual(entry["observed_sample_bytes"], None)
        self.assertEqual(report["inputs"]["assertion"]["provenance"], "user_asserted")
        self.assertEqual(sum(p["bytes"] for p in entry["static_critical_path"]),
                         entry["static_known_path_bytes"])
        self.command([sys.executable, ROOT / "scripts/stack_budget.py", "analyze",
                      "--manifest", path, "--elf", case["elf"], "--map", case["map"],
                      "--output", path.with_suffix(".within.json")])
        value["entries"][0]["usable_bytes"] = 4096
        sb.save(path, value)
        self.assertion(path, value)
        output = path.with_suffix(".report.json")
        self.command([sys.executable, ROOT / "scripts/stack_budget.py", "analyze",
                      "--manifest", path, "--elf", case["elf"], "--map", case["map"],
                      "--output", output], expected=1)
        exceeded = sb.load(output)
        self.assertEqual(exceeded["status"], "EXCEEDS")
        # A known path can exceed U even while runtime/other edges remain unknown.
        value.pop("assertion")
        value["page_size"] = 1
        value["entries"][0]["usable_bytes"] = 1
        sb.save(path, value)
        exceeded = sb.analyze(path)
        self.assertEqual(exceeded["status"], "EXCEEDS")
        self.assertTrue(exceeded["entries"][0]["unknown_edges"])
        self.assertIsNone(exceeded["entries"][0]["conditional_budget_bytes"])

    def test_malformed_unknown_dialect_and_missing_edge(self):
        case = self.cases["O2"]
        obj = Path(case["objects"][0]["path"])
        original = obj.with_suffix(".ci").read_text()
        bad = case["directory"] / "bad.ci"
        for text in (original[:-2], original.replace("node: {", "newnode: {", 1)):
            bad.write_text(text)
            with self.assertRaises(sb.BudgetError):
                sb.read_ci(bad)
        su = case["directory"] / "bad.su"
        su.write_text(obj.with_suffix(".su").read_text().replace("\tstatic", "\tbogus", 1))
        with self.assertRaises(sb.BudgetError):
            sb.read_su(su)
        # Corrupt only the real VCG edge, retaining independent machine evidence.
        parsed = sb.read_object(obj)
        parsed["graph"].remove(("DirectEntry", "DirectMiddle"))
        known, _, _, unknown, *_ = sb.aggregate(
            {"object": "scenarios", "symbol": "DirectEntry"}, {"scenarios": parsed}, None)
        self.assertGreater(known, 0)
        self.assertTrue(any("VCG/machine edge mismatch" in edge for edge in unknown))

    def test_stale_source_header_object_and_missing_metadata(self):
        directory = self.work / self._testMethodName
        directory.mkdir()
        source, header, obj = directory / "entry.cc", directory / "dep.hpp", directory / "entry.o"
        header.write_text("#define VALUE 1\n")
        source.write_text('#include "dep.hpp"\nextern "C" int Entry(int x) { return x + VALUE; }\n')
        self.compile(source, obj)
        self.assertTrue(sb.check_receipt(obj))
        header.write_text("#define VALUE 2\n")
        with self.assertRaisesRegex(sb.BudgetError, "dependencies"):
            sb.check_receipt(obj)
        header.write_text("#define VALUE 1\n")
        source.write_text(source.read_text() + "// changed\n")
        with self.assertRaisesRegex(sb.BudgetError, "dependencies"):
            sb.check_receipt(obj)
        self.compile(source, obj)
        data = obj.read_bytes()
        obj.write_bytes(data)
        with self.assertRaisesRegex(sb.BudgetError, "rebuilt/replaced"):
            sb.check_receipt(obj)
        self.compile(source, obj)
        obj.with_suffix(".ci").rename(directory / "saved.ci")
        with self.assertRaises(sb.BudgetError) as cm:
            sb.check_receipt(obj)
        self.assertEqual(cm.exception.status, "STALE")
        prebuilt = self.cases["O2"]["prebuilt"]
        with self.assertRaises(sb.BudgetError) as cm:
            sb.check_receipt(prebuilt)
        self.assertEqual(cm.exception.status, "UNKNOWN")

    def test_actual_source_alias_resolution_is_fingerprinted(self):
        directory = self.work / self._testMethodName
        directory.mkdir()
        a, b, alias, obj = (directory / name for name in ("a.cc", "b.cc", "alias.cc", "entry.o"))
        a.write_text('extern "C" int Entry(int x) { return x + 1; }\n')
        b.write_text('extern "C" int Entry(int x) { return x + 2; }\n')
        alias.symlink_to(a)
        self.compile(alias, obj)
        self.assertTrue(sb.check_receipt(obj))
        alias.unlink()
        alias.symlink_to(b)
        with self.assertRaises(sb.BudgetError) as cm:
            sb.check_receipt(obj)
        self.assertEqual(cm.exception.status, "STALE")

    def test_LTO_and_alternate_ABI_are_UNSUPPORTED(self):
        directory = self.work / self._testMethodName
        directory.mkdir()
        source = directory / "entry.cc"
        source.write_text('extern "C" int Entry(int value) { return value + 1; }\n')
        obj = directory / "entry.o"
        self.command([sys.executable, ROOT / "scripts/stack_budget.py", "record", "--",
                      self.compiler, "-O2", "-flto", "-fstack-usage", "-fcallgraph-info=su",
                      "-c", source, "-o", obj], expected=2)
        self.assertFalse(sb.receipt_path(obj).exists())
        self.command([self.compiler, "-O2", "-flto", "-c", source, "-o", obj])
        self.assertIn(".gnu.lto_", sb.run(["objdump", "-h", str(obj)]))
        with self.assertRaises(sb.BudgetError) as cm:
            sb.compile_spec([self.compiler, "-m32", "-c", str(source), "-o", str(obj)])
        self.assertEqual(cm.exception.status, "UNSUPPORTED")

    def test_two_stage_identity_glue_change_and_old_header(self):
        case = self.cases["O2"]
        directory = self.work / self._testMethodName
        directory.mkdir()
        path, value = self.manifest()
        header = directory / "budget.hpp"
        value["generated_header"] = str(header)
        self.assertion(path, value)
        report = sb.analyze(path, case["elf"], case["map"])
        self.assertEqual(report["status"], "WITHIN_MODEL", report["entries"])
        header.write_text(sb.header_text(report))
        report_path = directory / "report.json"
        sb.save(report_path, report)
        repeated = sb.analyze(path, case["elf"], case["map"])
        self.assertEqual(report["fingerprint"], repeated["fingerprint"])
        glue_source, glue = directory / "glue.cc", directory / "glue.o"
        elf, link_map = directory / "consumer", directory / "consumer.map"
        output = directory / "consumption.json"
        glue_source.write_text('#include "budget.hpp"\nextern "C" int DirectEntry(int);\n'
                               'int main() { return DirectEntry(EntryNotUsed); }\n'.replace(
                                   "EntryNotUsed", "static_cast<int>(bbt_stack_budget::Entry)"))
        self.compile(glue_source, glue)

        def link():
            self.command([self.compiler, glue, *[o["path"] for o in case["objects"]],
                          case["prebuilt"], "-Wl,-Map=" + str(link_map), "-o", elf])

        link()
        first = sb.consume(report_path, path, header, elf, link_map, [glue], output)
        glue_source.write_text(glue_source.read_text().replace("return DirectEntry(",
                                                             "return 1 + DirectEntry("))
        self.compile(glue_source, glue)
        link()
        second = sb.consume(report_path, path, header, elf, link_map, [glue], output)
        self.assertEqual(first["analysis_fingerprint"], second["analysis_fingerprint"])
        self.assertNotEqual(first["consumption_fingerprint"], second["consumption_fingerprint"])
        header.write_text(header.read_text() + "// stale header\n")
        with self.assertRaises(sb.BudgetError) as cm:
            sb.consume(report_path, path, header, elf, link_map, [glue], output)
        self.assertEqual(cm.exception.status, "STALE")

    def test_generated_header_in_analysis_closure_rejected(self):
        directory = self.work / self._testMethodName
        directory.mkdir()
        header, source, obj = directory / "budget.hpp", directory / "entry.cc", directory / "entry.o"
        header.write_text("#pragma once\ninline constexpr int OldBudget = 4096;\n")
        source.write_text('#include "budget.hpp"\nextern "C" int Entry() { return OldBudget; }\n')
        self.compile(source, obj)
        manifest = {"schema_version": 1, "target": "bad_closure",
                    "objects": [{"id": "entry", "path": str(obj)}],
                    "entries": [{"name": "Entry", "object": "entry", "symbol": "Entry",
                                 "usable_bytes": 65536}], "page_size": 4096, "margin_bytes": 4096,
                    "generated_header": str(header)}
        path = directory / "manifest.json"
        sb.save(path, manifest)
        with self.assertRaises(sb.BudgetError) as cm:
            sb.analyze(path)
        self.assertEqual(cm.exception.status, "UNSUPPORTED")

    def test_cli_corruption_never_succeeds(self):
        directory = self.work / self._testMethodName
        directory.mkdir()
        manifest, output = directory / "manifest.json", directory / "report.json"
        manifest.write_text("{malformed")
        self.command([sys.executable, ROOT / "scripts/stack_budget.py", "analyze",
                      "--manifest", manifest, "--output", output], expected=2)
        self.assertEqual(sb.load(output)["status"], "ERROR")
        self.assertIsNone(sb.load(output)["conditional_budget_bytes"])

    def test_macro_dependency_and_recompile_invalidate_frozen_F(self):
        directory, source, dep, obj, path, _, elf, link_map, output, frozen = self.simple_case()
        dep.write_text("#define VALUE 2\n")
        with self.assertRaises(sb.BudgetError) as cm:
            sb.verify_report(output, path)
        self.assertEqual(cm.exception.status, "STALE")
        error_output = directory / "stale-error.json"
        self.command([sys.executable, ROOT / "scripts/stack_budget.py", "analyze",
                      "--manifest", path, "--elf", elf, "--map", link_map,
                      "--output", error_output], expected=2)
        self.assertEqual(sb.load(error_output)["status"], "STALE")
        dep.write_text("#define VALUE 1\n")
        self.assertEqual(sb.verify_report(output, path)[0]["fingerprint"], frozen["fingerprint"])
        self.compile(source, obj, extra=["-DENTRY_MACRO=2"])
        with self.assertRaises(sb.BudgetError) as cm:
            sb.verify_report(output, path)
        self.assertEqual(cm.exception.status, "STALE")

    def test_identical_object_recompile_is_not_phase_two_reuse(self):
        _, source, _, obj, path, _, _, _, output, frozen = self.simple_case()
        original_bytes = sb.digest(obj)
        old_event = sb.load(sb.receipt_path(obj))["compile_event"]
        self.compile(source, obj)
        self.assertEqual(sb.digest(obj), original_bytes)
        self.assertNotEqual(sb.load(sb.receipt_path(obj))["compile_event"], old_event)
        with self.assertRaises(sb.BudgetError) as cm:
            sb.verify_report(output, path)
        self.assertEqual(cm.exception.status, "STALE")
        self.assertIsNotNone(frozen["fingerprint"])

    def test_manual_assertion_gaps_versions_and_actual_link_binding(self):
        directory, _, _, _, path, value, elf, link_map, _, _ = self.simple_case()
        bound = sb.load(value["assertion"])
        template_path = Path(bound["template"])
        template = sb.load(template_path)
        template["covers"].remove("exception_unwind")
        sb.save(template_path, template)
        sb.bind_assertion(template_path, path, value["assertion"])
        partial = sb.analyze(path, elf, link_map)["entries"][0]
        self.assertEqual(partial["status"], "UNKNOWN")
        self.assertGreater(partial["static_known_path_bytes"], 0)
        self.assertIsNone(partial["conditional_budget_bytes"])
        self.assertIn("manual coverage gap: exception_unwind", partial["unknown_edges"])
        template["compiler_version"] = "14.2.0"
        sb.save(template_path, template)
        with self.assertRaises(sb.BudgetError) as cm:
            sb.bind_assertion(template_path, path, value["assertion"])
        self.assertEqual(cm.exception.status, "STALE")
        template["compiler_version"] = self.tc["version"]
        template["covers"] = sorted(sb.REQUIRED_COVERAGE)
        unlinked = directory / "not-linked.o"
        shutil.copy2(self.cases["O2"]["prebuilt"], unlinked)
        template["binding_files"].append(str(unlinked))
        sb.save(template_path, template)
        sb.bind_assertion(template_path, path, value["assertion"])
        with self.assertRaises(sb.BudgetError) as cm:
            sb.analyze(path, elf, link_map)
        self.assertEqual(cm.exception.status, "STALE")

    def test_static_external_provider_must_match_final_selection(self):
        directory, source, _, obj, path, value, elf, link_map, _, _ = self.simple_case()
        source.write_text('extern "C" int Opaque(int);\n'
                          'extern "C" int Entry(int x) { return Opaque(x); }\n')
        self.compile(source, obj)
        weak_source, strong_source = directory / "weak.cc", directory / "strong.cc"
        weak_source.write_text('extern "C" __attribute__((weak,noinline)) '
                               'int Opaque(int x) { return x + 7; }\n')
        strong_source.write_text('extern "C" __attribute__((noinline)) '
                                 'int Opaque(int x) { return x + 1; }\n')
        weak, strong, archive = directory / "weak.o", directory / "strong.o", directory / "opaque.a"
        self.command([self.compiler, "-O2", "-c", weak_source, "-o", weak])
        self.command([self.compiler, "-O2", "-c", strong_source, "-o", strong])
        self.command(["ar", "rcs", archive, strong])
        bound = sb.load(value["assertion"])
        template_path = Path(bound["template"])
        template = sb.load(template_path)
        template["external"]["Opaque"] = 128
        original_bindings = template["binding_files"][:]
        for label, inputs, kind, status in (
                ("strong-object", [strong], "T", "WITHIN_MODEL"),
                ("weak-before-archive", [weak, archive], "W", "UNKNOWN"),
                ("archive-only", [archive], "T", "UNKNOWN")):
            with self.subTest(label=label):
                self.command([self.compiler, directory / "probe.o", obj,
                              self.cases["O2"]["prebuilt"], *inputs,
                              "-Wl,-Map=" + str(link_map), "-o", elf])
                self.assertEqual(sb.read_nm(elf)["Opaque"]["kind"], kind)
                output, header = directory / (label + ".json"), directory / (label + ".hpp")
                value["generated_header"] = str(header)
                sb.save(path, value)
                template["binding_files"] = original_bindings + [str(p) for p in inputs]
                sb.save(template_path, template)
                sb.bind_assertion(template_path, path, value["assertion"])
                self.command([sys.executable, ROOT / "scripts/stack_budget.py", "analyze",
                              "--manifest", path, "--elf", elf, "--map", link_map,
                              "--output", output, "--header", header],
                             expected=0 if status == "WITHIN_MODEL" else 2)
                self.assertEqual(sb.load(output)["status"], status)
                self.assertEqual(header.exists(), status == "WITHIN_MODEL")

    def test_conditional_arithmetic_overflow_and_compile_error(self):
        directory, source, _, obj, path, value, elf, link_map, _, _ = self.simple_case()
        value["margin_bytes"] = sb.LIMIT
        sb.save(path, value)
        with self.assertRaisesRegex(sb.BudgetError, "overflow"):
            sb.analyze(path, elf, link_map)
        source.write_text("not valid C++;\n")
        failed = directory / "failed.o"
        proc = self.command([sys.executable, ROOT / "scripts/stack_budget.py", "record", "--",
                             self.compiler, "-O2", "-fstack-usage", "-fcallgraph-info=su",
                             "-c", source, "-o", failed], expected=2)
        self.assertIn("ERROR", proc.stderr)
        self.assertFalse(sb.receipt_path(failed).exists())
        self.assertNotEqual(obj, failed)

    def test_UNKNOWN_cli_has_no_header_or_capacity(self):
        path, value = self.manifest()
        header = path.with_suffix(".hpp")
        output = path.with_suffix(".unknown.json")
        value["generated_header"] = str(header)
        sb.save(path, value)
        self.command([sys.executable, ROOT / "scripts/stack_budget.py", "analyze",
                      "--manifest", path, "--output", output, "--header", header], expected=2)
        report = sb.load(output)
        self.assertEqual(report["status"], "UNKNOWN")
        self.assertFalse(header.exists())
        self.assertIsNone(report["entries"][0]["conditional_budget_bytes"])

    def test_final_map_must_select_the_analyzed_input_section(self):
        case = self.cases["O2"]
        path, value = self.manifest()
        self.assertion(path, value)
        original = sb.analyze(path, case["elf"], case["map"])
        self.assertEqual(original["status"], "WITHIN_MODEL")
        obj = case["objects"][0]["path"]
        lines = case["map"].read_text().splitlines()
        changed = [line.replace(obj, str(case["prebuilt"])) if "0x" in line and obj in line
                   else line for line in lines]
        self.assertNotEqual(changed, lines)
        bad_map = case["directory"] / "wrong-selected-section.map"
        bad_map.write_text("\n".join(changed) + "\n")
        report = sb.analyze(path, case["elf"], bad_map)
        self.assertEqual(report["status"], "UNKNOWN")
        self.assertTrue(any("map does not uniquely select" in edge
                            for edge in report["entries"][0]["unknown_edges"]))

    def test_callable_thunk_cannot_hide_in_generated_header_glue(self):
        directory, _, _, _, path, _, elf, link_map, output, _ = self.simple_case()
        source, obj = directory / "glue.cc", directory / "glue.o"
        source.write_text('#include "budget.hpp"\n#include <functional>\n'
                          'extern "C" int Entry(int);\n'
                          'extern void Accept(const std::function<int(int)>&);\n'
                          'int main() { std::function<int(int)> f = &Entry; Accept(f);'
                          ' return bbt_stack_budget::Entry == 0; }\n')
        self.compile(source, obj)
        self.assertTrue(any("_M_invoke" in s for s in sb.read_nm(obj)))
        with self.assertRaisesRegex(sb.BudgetError, "glue may emit main only") as cm:
            sb.consume(output, path, directory / "budget.hpp", elf, link_map, [obj],
                       directory / "rejected-consumption.json")
        self.assertEqual(cm.exception.status, "UNSUPPORTED")

    def test_unregistered_application_link_object_is_not_outside_F(self):
        directory, _, _, obj, path, _, _, _, _, _ = self.simple_case()
        hidden_source, hidden = directory / "hidden.cc", directory / "hidden.o"
        hidden_source.write_text('extern "C" int HiddenAdapter(int x) { return x + 1; }\n')
        self.command([self.compiler, "-O2", "-c", hidden_source, "-o", hidden])
        elf, link_map = directory / "extra-object-probe", directory / "extra-object.map"
        self.command([self.compiler, directory / "probe.o", obj, hidden, self.cases["O2"]["prebuilt"],
                      "-Wl,-Map=" + str(link_map), "-o", elf])
        report = sb.analyze(path, elf, link_map)
        self.assertEqual(report["status"], "UNKNOWN")
        self.assertTrue(any("unregistered application object" in edge
                            for edge in report["entries"][0]["unknown_edges"]))
        self.assertIsNone(report["entries"][0]["conditional_budget_bytes"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
