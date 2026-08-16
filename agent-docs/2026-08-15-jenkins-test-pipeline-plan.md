# Jenkins 测试体系实现计划

> **面向 AI 代理的工作者：** 必需子技能：使用 `superpowers:subagent-driven-development` 逐任务实现此计划。步骤使用复选框（`- [ ]`）跟踪进度。

**目标：** 将 `bbtools-coroutine` 的 Smoke、PR 性能和持续疲劳测试统一迁移到 Jenkins，并建立可保存、可比较、可告警的测试证据链。

**架构：** 仓库提供可本地运行的测试 Harness 和版本化 Jenkins Pipeline。Smoke 使用 `cpp-fast`，PR 性能使用固定环境的 `cpp-perf`，持续疲劳使用独占 `cpp-soak`。Jenkins Controller 只负责任务编排、凭据、结果归档、GitHub 状态回写和邮件通知，不承担编译或长测。

**技术栈：** C++17、CMake/Ninja、CTest、Python 3 标准库、Bash、Jenkins Pipeline、JUnit/CTest XML、JSON/JSONL。

**设计依据：** `agent-docs/2026-08-15-jenkins-test-pipeline-design.md`

---

## 0. 实现边界和文件清单

实现只修改以下仓库内容；Jenkins Controller、Agent Label、Webhook、邮件凭据和 Job 参数在 Jenkins 管理面配置，不把凭据写入仓库。

### 将创建

- `scripts/ci/__init__.py`：让 CI Python 辅助模块可被单测导入。
- `scripts/ci/ci_common.py`：报告目录、命令执行、错误分类、环境摘要和 JSON 写入的无副作用公共函数。
- `scripts/ci/run_smoke.py`：Smoke 构建、CTest、退出检查和报告生成入口。
- `scripts/ci/perf_contract.py`：性能指标 Schema、环境指纹、基线读取和比较逻辑。
- `scripts/ci/run_pr_performance.py`：PR/main 性能测试入口，调用 `unified_stress` 并生成结构化结果。
- `scripts/ci/run_soak.py`：单进程六模块、资源采样、功能指标采集、分段报告和异常判定入口。
- `scripts/ci/test_ci_common.py`：公共函数单元测试。
- `scripts/ci/test_perf_contract.py`：性能 Schema、基线兼容性和阈值判定单元测试。
- `scripts/ci/test_soak.py`：指标解析、停滞判定、资源阈值和退出状态单元测试。
- `ci/jenkins/smoke.Jenkinsfile`：定时 Smoke Pipeline。
- `ci/jenkins/performance.Jenkinsfile`：PR 性能和 main 基线 Pipeline。
- `ci/jenkins/soak.Jenkinsfile`：6 小时分段自动续跑 Pipeline。

### 将修改

- `unit_test/CMakeLists.txt`：为 Smoke 测试设置 CTest Label 和显式 timeout。
- `scripts/ci_perf_check.py`：保留现有 CLI，改为调用统一性能契约，支持显式基线和结果路径。
- `scripts/record_baseline.py`：保留本地用法，输出统一 Schema 和环境指纹。
- `docs/ci-guide.md`：改写为 Jenkins 使用和故障排查指南。
- `.github/workflows/unit_test.yml`：Jenkins 验收后删除旧测试 Workflow。
- `.github/workflows/memery_test_info.yml`：Jenkins 验收后删除旧内存检测 Workflow。

### 不修改

- `bbt/coroutine/` 下运行时核心代码；
- `benchmark_test/unified_stress.cc` 的业务压测逻辑；除非实现阶段发现指标契约确实缺字段，并单独形成测试 Harness 变更；
- `scripts/fatigue_monitor/` Web Dashboard；它是交互式本地工具，不作为 Jenkins 无头长测入口；
- `.env`、凭据、SSH 配置和 Jenkins Controller 配置文件。

### 验证原则

仓库没有 `health/lint.sh`，因此不用虚构该命令。每个阶段使用真实命令验证：Python `unittest`、`bash -n`、CMake/CTest、短时 Smoke、短时性能和短时 Soak。实现完成前必须运行 `git diff --check`、相关测试和 Git 状态检查。

---

## 任务 1：建立 CI Python 公共契约和测试骨架

**文件：**
- 创建：`scripts/ci/__init__.py`
- 创建：`scripts/ci/ci_common.py`
- 创建：`scripts/ci/test_ci_common.py`

### 步骤 1：编写失败测试

在 `scripts/ci/test_ci_common.py` 中先覆盖：

```python
import json
import tempfile
import unittest
from pathlib import Path

from ci_common import classify_process_result, write_json_atomic


class CommonContractTest(unittest.TestCase):
    def test_classify_timeout(self):
        self.assertEqual(classify_process_result(None, timed_out=True), "timeout")

    def test_classify_signal(self):
        self.assertEqual(classify_process_result(-11, timed_out=False), "crash")

    def test_write_json_creates_parent_and_valid_json(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "nested" / "result.json"
            write_json_atomic(path, {"status": "ok"})
            self.assertEqual(json.loads(path.read_text()), {"status": "ok"})
```

运行：

```bash
PYTHONPATH=scripts/ci python3 -m unittest scripts/ci/test_ci_common.py -v
```

预期：因 `ci_common.py` 尚未提供函数而失败。

### 步骤 2：实现最小公共函数

`ci_common.py` 至少实现：

```python
def classify_process_result(returncode: int | None, timed_out: bool) -> str:
    if timed_out:
        return "timeout"
    if returncode is None:
        return "unknown_exit"
    if returncode < 0:
        return "crash"
    if returncode != 0:
        return "failure"
    return "pass"


def write_json_atomic(path: Path, value: object) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n")
    temporary.replace(path)
```

同时实现命令执行、时间戳、目录初始化和脱敏环境摘要函数；函数不得读取密钥环境变量的值。

### 步骤 3：运行测试确认通过

```bash
PYTHONPATH=scripts/ci python3 -m unittest scripts/ci/test_ci_common.py -v
python3 -m py_compile scripts/ci/ci_common.py scripts/ci/test_ci_common.py
```

预期：测试全部通过，Python 编译检查退出码为 0。

### 步骤 4：提交

```bash
git add scripts/ci/__init__.py scripts/ci/ci_common.py scripts/ci/test_ci_common.py
git commit -m "test(ci): 建立 Jenkins 测试公共契约"
```

---

## 任务 2：给 CTest 建立 Smoke 标签和 timeout

**文件：**
- 修改：`unit_test/CMakeLists.txt`
- 测试：`scripts/ci/run_smoke.py` 将通过 CTest 查询标签，不新增 C++ 测试。

### 步骤 1：确认当前测试清单

运行：

```bash
cmake -S . -B build-ci-catalog -G Ninja -DNEED_TEST=ON -DNEED_BENCHMARK=ON -DCMAKE_BUILD_TYPE=Debug
ctest --test-dir build-ci-catalog -N
```

预期：当前实际注册 19 个测试；记录名称，不把过期文档中的 16 个测试作为依据。

### 步骤 2：修改 CMake 测试属性

在现有 `add_test()` 注册后，为 Smoke 候选设置 `LABELS "smoke"`，并为可能无进展的测试设置明确 timeout。实现阶段必须按当前测试行为确认候选集合，初始集合为：

```cmake
set_tests_properties(
    Test_coroutine
    Test_coroutine_exception
    Test_poller
    Test_chan
    Test_comutex
    Test_coevent
    Test_copool
    Test_smoke
    PROPERTIES LABELS "smoke" TIMEOUT 60
)
```

不要给 `Test_reliability` 加入默认 Smoke Label；它保留为完整回归和独立 Reliability Job 的候选。

### 步骤 3：验证标签和 timeout

```bash
cmake -S . -B build-ci-catalog -G Ninja -DNEED_TEST=ON -DNEED_BENCHMARK=ON -DCMAKE_BUILD_TYPE=Debug
ctest --test-dir build-ci-catalog -N -L smoke
ctest --test-dir build-ci-catalog -N -V -L smoke
```

预期：只列出 8 个 Smoke 候选，且 CTest 文件显示 timeout 属性可用。

### 步骤 4：提交

```bash
git add unit_test/CMakeLists.txt
git commit -m "test(cmake): 标记 Smoke 测试并设置超时"
```

---

## 任务 3：实现定时 Smoke Harness

**文件：**
- 创建：`scripts/ci/run_smoke.py`
- 修改：`scripts/ci/ci_common.py`（仅在测试先失败后补充必要函数）
- 测试：`scripts/ci/test_ci_common.py` 增加错误分类测试。

### 步骤 1：定义命令契约

Smoke 入口支持以下参数：

```text
--source-dir /home/yqm/Code/owner/bbtools/bbtools-coroutine
--build-dir build-ci-smoke
--build-type Debug
--timeout-seconds 900
--report-dir tests/reports/smoke/20260815T120000Z
```

实际路径通过 Jenkins 工作区传入，不在脚本中写死当前开发机路径。

### 步骤 2：实现干净构建和测试流程

`run_smoke.py` 依次执行：

```bash
rm -rf build-ci-smoke
cmake -S . -B build-ci-smoke -G Ninja \
  -DNEED_TEST=ON -DNEED_BENCHMARK=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-ci-smoke --parallel
ctest --test-dir build-ci-smoke --output-on-failure -L smoke
```

先检查 CTest 是否支持 `--output-junit`：

```bash
ctest --help | grep -- '--output-junit'
```

支持时写入 `ctest.xml`；不支持时保留 `Testing/` 目录和文本结果，并让 Jenkins 归档文本报告，不得假装生成 JUnit XML。

脚本还必须：

- 记录每条命令、退出码和耗时；
- 记录 CMake cache 摘要、编译器版本和内核信息；
- 扫描工作区和构建目录中的 core/dump 文件；
- 扫描 stdout/stderr 中的 `Assert`、`Segmentation fault`、`AddressSanitizer`、`ERROR`；
- 测试超时后先发送 `SIGTERM`，等待有限时间，再发送 `SIGKILL`；
- 输出 `summary.json`、`summary.md`、`stdout.log`、`stderr.log` 和 `commands.json`；
- 出现 timeout、crash、dump 或错误扫描命中时返回非 0。

### 步骤 3：添加 Harness 单元测试

使用假的 `sys.executable` 子进程验证 timeout、退出码和 dump 文件分类，不启动真实 coroutine：

```python
def test_timeout_is_failure(self):
    result = run_command([sys.executable, "-c", "import time; time.sleep(2)"], timeout=0.05)
    self.assertEqual(result.category, "timeout")
    self.assertNotEqual(result.returncode, 0)
```

### 步骤 4：运行本地短 Smoke

```bash
PYTHONPATH=scripts/ci python3 -m unittest discover -s scripts/ci -p 'test_*.py' -v
python3 scripts/ci/run_smoke.py --build-dir build-ci-smoke --timeout-seconds 900
```

预期：Smoke 报告生成，退出码为 0；若真实测试失败，停止推进并记录失败测试，不用重试掩盖问题。

### 步骤 5：提交

```bash
git add scripts/ci/run_smoke.py scripts/ci/ci_common.py scripts/ci/test_ci_common.py
git commit -m "ci(smoke): 增加 Jenkins 定时 Smoke Harness"
```

---

## 任务 4：统一性能指标 Schema 和基线比较

**文件：**
- 创建：`scripts/ci/perf_contract.py`
- 创建：`scripts/ci/test_perf_contract.py`
- 修改：`scripts/ci_perf_check.py`
- 修改：`scripts/record_baseline.py`

### 步骤 1：编写基线比较失败测试

覆盖以下事实：

```python
class PerfContractTest(unittest.TestCase):
    def test_incompatible_agent_is_not_comparable(self):
        old = {"environment": {"agent": "perf-a", "compiler": "g++-13"}}
        new = {"environment": {"agent": "perf-b", "compiler": "g++-13"}}
        self.assertEqual(compare_environment(old, new).status, "NO_COMPARABLE_BASELINE")

    def test_ten_percent_drop_is_warning(self):
        result = compare_module(old_ops=1000, new_ops=890, module="comutex")
        self.assertEqual(result.status, "WARN")

    def test_twenty_percent_drop_is_failure_after_gate_enabled(self):
        result = compare_module(old_ops=1000, new_ops=790, module="comutex", gate_enabled=True)
        self.assertEqual(result.status, "FAIL")

    def test_missing_metric_is_failure(self):
        self.assertEqual(validate_module_metrics({}).status, "METRIC_INVALID")
```

运行：

```bash
PYTHONPATH=scripts/ci python3 -m unittest scripts/ci/test_perf_contract.py -v
```

预期：因 Schema 函数尚未实现而失败。

### 步骤 2：实现统一 Schema

`perf_contract.py` 定义固定字段：

```python
REQUIRED_TOP_LEVEL = {
    "schema_version", "repository", "commit", "base_commit",
    "environment", "build", "parameters", "modules", "verdict"
}

REQUIRED_MODULE_FIELDS = {
    "ops_total", "ops_per_sec", "errors", "elapsed_s"
}

VALID_VERDICTS = {
    "PASS", "WARN", "FAIL", "UNSTABLE",
    "NO_COMPARABLE_BASELINE", "METRIC_INVALID"
}
```

环境指纹至少包括：

- Jenkins Node 名和稳定 Agent 标识；
- CPU 型号和逻辑核数；
- 内存总量；
- 编译器版本；
- CMake 版本；
- Ninja 版本；
- Build Type；
- CMake 参数；
- Processer 线程数；
- 模块和运行时长。

指纹比较失败只能产生 `NO_COMPARABLE_BASELINE`，不能转成 `PASS`。

### 步骤 3：改造现有性能脚本

保留现有命令兼容性：

```bash
python3 scripts/ci_perf_check.py --threads=2 --dur=45
python3 scripts/record_baseline.py record --threads=2 --dur=60 --quick
```

新增参数：

```text
--baseline PATH
--output PATH
--environment-file PATH
--gate-enabled
--module MODULE
```

脚本职责调整为：

- 运行 `unified_stress`；
- 解析每个模块最后一个有效 `FATIGUE_METRIC`；
- 拒绝 timeout、crash、zero ops、缺字段；
- 写统一 JSON 和 Markdown；
- 无基线输出 `NO_COMPARABLE_BASELINE`；
- 初期 `--gate-enabled` 默认关闭；
- 不在 PR Job 中隐式写基线。

### 步骤 4：运行单元测试和短性能测试

```bash
PYTHONPATH=scripts/ci python3 -m unittest discover -s scripts/ci -p 'test_*.py' -v
cmake --build build --target unified_stress --parallel
python3 scripts/ci_perf_check.py --module=comutex --threads=1 --dur=10 --no-baseline-compare
```

预期：单模块输出结构化结果，`elapsed_s >= 10`，`ops_total > 0`，退出码为 0；输出文件能被 `json.load()` 重新读取。

### 步骤 5：提交

```bash
git add scripts/ci/perf_contract.py scripts/ci/test_perf_contract.py scripts/ci_perf_check.py scripts/record_baseline.py
git commit -m "ci(performance): 统一性能指标和基线契约"
```

---

## 任务 5：实现单进程六模块 Soak Harness

**文件：**
- 创建：`scripts/ci/run_soak.py`
- 创建：`scripts/ci/test_soak.py`
- 修改：`scripts/ci/ci_common.py`（仅补充通用采样函数）

### 步骤 1：编写指标和资源判定失败测试

```python
class SoakContractTest(unittest.TestCase):
    def test_parse_metric_line(self):
        line = 'FATIGUE_METRIC:{"name":"comutex","ops_total":10,"errors":0}'
        metric = parse_metric_line(line)
        self.assertEqual(metric["name"], "comutex")

    def test_stall_after_two_unchanged_samples(self):
        samples = [10, 10, 10]
        self.assertTrue(has_stall(samples, max_unchanged_intervals=2))

    def test_all_modules_required(self):
        result = evaluate_function_metrics({"comutex": {"ops_total": 1}}, REQUIRED_MODULES)
        self.assertEqual(result.status, "METRIC_MISSING")

    def test_rss_high_is_warning(self):
        self.assertEqual(evaluate_resource(rss_mib=140, cpu_pct=5, memory_high=128, memory_max=256).status, "WARN")

    def test_rss_max_is_failure(self):
        self.assertEqual(evaluate_resource(rss_mib=260, cpu_pct=5, memory_high=128, memory_max=256).status, "FAIL")
```

### 步骤 2：实现采样和运行器

`run_soak.py` 的默认命令：

```bash
python3 scripts/ci/run_soak.py \
  --build-dir build-soak \
  --duration-seconds 21600 \
  --threads 1 \
  --resource-interval-seconds 10 \
  --metric-interval-seconds 60 \
  --report-dir tests/reports/soak/20260815T120000Z
```

报告目录由脚本基于 UTC 时间生成；Jenkins 只提供 `tests/reports/soak` 根目录，不允许用户输入路径穿越目录。

运行命令固定为：

```bash
build-soak/bin/benchmark_test/unified_stress \
  --threads=1 21600 0 0
```

脚本必须：

- 单进程覆盖六模块；
- 以独立进程组启动并读取 stdout/stderr；
- 每 10 秒读取 `/proc/<pid>/stat`、`/proc/<pid>/status`；
- 每 60 秒解析 `FATIGUE_METRIC:` 行；
- 输出 `resource.jsonl`、`metrics.jsonl`、`summary.json`、`summary.md` 和 `raw.log`；
- 记录 PID、run ID、commit、Node、CPU、RSS、PSS（可读取时）、线程数和退出码；
- 检测连续两个以上采样周期无进展；
- 检测缺失模块、errors、timeout、crash、OOM、core/dump；
- 目标时长结束后发送 `SIGTERM`，等待正常退出；
- 只有异常不退出时才使用 `SIGKILL`，并把结果标记为 `forced_kill`；
- 对 `SIGTERM` 正常结束的退出码和目标时长做明确区分；
- 失败段保留完整日志，不能由下一段覆盖。

脚本只负责测量和判定。CPU、MemoryHigh、MemoryMax 的硬限制由 Jenkins `cpp-soak` Agent 的 cgroup/容器资源配置提供；脚本只能观测并报警，不能擅自修改主机资源限制。

### 步骤 3：运行短 Soak

```bash
PYTHONPATH=scripts/ci python3 -m unittest scripts/ci/test_soak.py -v
cmake -S . -B build-soak -G Ninja -DNEED_BENCHMARK=ON -DNEED_TEST=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build-soak --target unified_stress --parallel
python3 scripts/ci/run_soak.py \
  --build-dir build-soak \
  --duration-seconds 30 \
  --threads 1 \
  --resource-interval-seconds 2 \
  --metric-interval-seconds 5
```

预期：六个模块均有指标，进程正常结束，报告包含资源和功能 JSONL；如果出现 assert、crash、缺指标或异常退出，停止计划并记录证据。

### 步骤 4：提交

```bash
git add scripts/ci/run_soak.py scripts/ci/test_soak.py scripts/ci/ci_common.py
git commit -m "ci(soak): 增加低资源持续疲劳测试 Harness"
```

---

## 任务 6：编写 Jenkins Smoke Pipeline

**文件：**
- 创建：`ci/jenkins/smoke.Jenkinsfile`

### 步骤 1：定义 Pipeline 参数和调度

Pipeline 必须包含：

```groovy
pipeline {
    agent { label 'cpp-fast' }
    triggers { cron('H/30 * * * *') }
    options {
        timestamps()
        disableConcurrentBuilds()
        timeout(time: 20, unit: 'MINUTES')
    }
    parameters {
        string(name: 'SMOKE_BUILD_TYPE', defaultValue: 'Debug')
        string(name: 'SMOKE_TIMEOUT_SECONDS', defaultValue: '900')
        string(name: 'SMOKE_TEST_LABEL', defaultValue: 'smoke')
    }
    stages {
        stage('Checkout') { steps { checkout scm } }
        stage('Run Smoke') {
            steps {
                sh '''
                    python3 scripts/ci/run_smoke.py \\
                      --build-dir build-ci-smoke \\
                      --build-type "$SMOKE_BUILD_TYPE" \\
                      --timeout-seconds "$SMOKE_TIMEOUT_SECONDS" \\
                      --test-label "$SMOKE_TEST_LABEL" \\
                      --report-dir tests/reports/smoke/"$BUILD_TAG"
                '''
            }
        }
    }
}
```

实际 `sh` 命令必须把参数安全地传给 Python，不能使用未经校验的字符串拼接执行任意 Shell 内容。

### 步骤 2：归档测试结果和邮件

`post` 块必须：

- `junit` 仅在 `ctest.xml` 存在且格式有效时调用；
- `archiveArtifacts` 归档 `tests/reports/smoke/**`；
- `always` 保存失败日志；
- `failure` 调用 `emailext`；
- 不修改其他 Job 的状态；
- 失败通知包含 Job、Build、commit、失败阶段和报告 URL。

### 步骤 3：验证 Pipeline 静态语法和本地入口

```bash
bash -n scripts/ci/run_smoke.py 2>/dev/null || true
python3 -m py_compile scripts/ci/run_smoke.py
python3 scripts/ci/run_smoke.py --help
```

Jenkins 节点上使用 Jenkins Pipeline Linter 或等价的 Controller API 验证 Groovy；本地不能把普通 Bash `bash -n` 当成 Groovy 验证结果。

### 步骤 4：提交

```bash
git add ci/jenkins/smoke.Jenkinsfile
git commit -m "ci(jenkins): 增加定时 Smoke Pipeline"
```

---

## 任务 7：编写 Jenkins 性能和基线 Pipeline

**文件：**
- 创建：`ci/jenkins/performance.Jenkinsfile`
- 修改：`scripts/ci/perf_contract.py`（仅补充 Pipeline 需要的元数据字段）

### 步骤 1：定义分支行为

同一 Pipeline 根据构建上下文分两种行为：

- PR：在固定 `cpp-perf` Agent 上测试候选 commit，读取最近成功且环境指纹匹配的 main 基线。
- main：运行相同参数，生成并归档新的 baseline JSON，作为后续 PR 的唯一候选基线来源。

参数：

```groovy
parameters {
    string(name: 'PERF_DURATION_SECONDS', defaultValue: '45')
    string(name: 'PERF_THREADS', defaultValue: '2')
    booleanParam(name: 'PERF_GATE_ENABLED', defaultValue: false)
    string(name: 'PERF_BASELINE_BUILD', defaultValue: '')
}
```

### 步骤 2：实现固定构建和比较流程

```groovy
stage('Build Performance Target') {
    steps {
        sh '''
            rm -rf build-ci-perf
            cmake -S . -B build-ci-perf -G Ninja \
              -DNEED_TEST=OFF -DNEED_BENCHMARK=ON \
              -DCMAKE_BUILD_TYPE=Release
            cmake --build build-ci-perf --target unified_stress --parallel
        '''
    }
}
```

随后调用：

```bash
python3 scripts/ci/run_pr_performance.py \
  --build-dir build-ci-perf \
  --duration-seconds "$PERF_DURATION_SECONDS" \
  --threads "$PERF_THREADS" \
  --baseline "$PERF_BASELINE" \
  --output tests/reports/performance/result.json \
  --gate-enabled "$PERF_GATE_ENABLED"
```

### 步骤 3：处理 Jenkins 状态

初期行为：

- `NO_COMPARABLE_BASELINE`：构建 `UNSTABLE`，不阻塞 PR，发邮件；
- 轻度退化：构建 `UNSTABLE`，不阻塞 PR，发邮件；
- crash、timeout、zero ops、Schema 无效：构建 `FAILURE`，不设置 PR 阻塞门禁，但发邮件；
- main baseline：失败则构建失败，不能写入半成品基线。

性能指标必须先写临时文件，校验成功后再原子替换为最终 JSON，避免 Jenkins 读取半写文件。

### 步骤 4：归档和基线获取

- main 构建归档 `baseline.json`、`environment.json` 和 Markdown 报告；
- PR 构建归档候选完整结果、比较结果和原始日志；
- Jenkins 使用 Copy Artifact 或统一 Artifact Manager 获取最近成功 main 构建的基线；
- 获取到的基线必须通过环境指纹校验；
- 不把性能基线写入 Git 仓库；
- 禁止以“找不到基线”为绿色性能通过。

### 步骤 5：验证

```bash
PYTHONPATH=scripts/ci python3 -m unittest discover -s scripts/ci -p 'test_*.py' -v
cmake --build build-ci-perf --target unified_stress --parallel
python3 scripts/ci/run_pr_performance.py \
  --build-dir build-ci-perf --duration-seconds 10 --threads 1 \
  --no-baseline --output /tmp/bbt-perf-result.json
python3 -m json.tool /tmp/bbt-perf-result.json >/dev/null
```

### 步骤 6：提交

```bash
git add ci/jenkins/performance.Jenkinsfile scripts/ci/perf_contract.py
git commit -m "ci(jenkins): 增加 PR 性能和基线流水线"
```

---

## 任务 8：编写 Jenkins Soak Pipeline

**文件：**
- 创建：`ci/jenkins/soak.Jenkinsfile`

### 步骤 1：定义独占 Agent 和可调参数

```groovy
pipeline {
    agent { label 'cpp-soak' }
    options {
        timestamps()
        disableConcurrentBuilds()
        timeout(time: 8, unit: 'HOURS')
    }
    parameters {
        string(name: 'SOAK_DURATION_SECONDS', defaultValue: '21600')
        string(name: 'SOAK_THREADS', defaultValue: '1')
        string(name: 'SOAK_RESOURCE_INTERVAL_SECONDS', defaultValue: '10')
        string(name: 'SOAK_METRIC_INTERVAL_SECONDS', defaultValue: '60')
        booleanParam(name: 'SOAK_CONTINUE', defaultValue: true)
    }
}
```

Jenkins Job 配置保证 `cpp-soak` 独占 executor。CPU quota、MemoryHigh 和 MemoryMax 在 Agent cgroup/容器层配置，Pipeline 只读取并写入报告。

### 步骤 2：实现单段执行和续跑

每段流程：

```text
checkout main 固定 commit
  └─ 构建 unified_stress
      └─ run_soak.py 运行一段
          ├─ 采集资源
          ├─ 采集六模块指标
          ├─ 归档 summary/resource/metrics/raw
          └─ 判断段结果
              ├─ 成功且 SOAK_CONTINUE=true：进入下一段
              └─ 失败：停止续跑，发邮件并保留异常证据
```

续跑只能在当前段完整归档且结果为通过时发生。不能使用无限 Shell 循环吞掉失败；每段必须有明确 Jenkins stage 和 build log。

### 步骤 3：资源和功能告警

Pipeline 解析 `summary.json`：

- `status=PASS`：归档并启动下一段；
- `status=WARN`：归档、发邮件、是否续跑由参数控制；
- `status=FAIL`：结束本次 Build、发邮件、不续跑；
- 报告缺失或 JSON 无效：按 `FAIL` 处理。

### 步骤 4：运行短段验证

```bash
python3 -m py_compile scripts/ci/run_soak.py
python3 scripts/ci/run_soak.py --help
python3 scripts/ci/run_soak.py \
  --build-dir build-soak \
  --duration-seconds 30 \
  --threads 1 \
  --resource-interval-seconds 2 \
  --metric-interval-seconds 5
python3 -m json.tool tests/reports/soak/*/summary.json >/dev/null
```

Jenkins 验收时再运行完整 6 小时段；未完成完整段不得宣称 Soak 通过。

### 步骤 5：提交

```bash
git add ci/jenkins/soak.Jenkinsfile
git commit -m "ci(jenkins): 增加分段持续疲劳流水线"
```

---

## 任务 9：统一邮件告警和报告归档

**文件：**
- 修改：`ci/jenkins/smoke.Jenkinsfile`
- 修改：`ci/jenkins/performance.Jenkinsfile`
- 修改：`ci/jenkins/soak.Jenkinsfile`
- 修改：`scripts/ci/ci_common.py`
- 修改：各报告生成脚本

### 步骤 1：统一故障指纹

在 `ci_common.py` 中实现：

```python
def failure_fingerprint(job, stage, test_name, returncode, category):
    raw = "|".join([job, stage, test_name, str(returncode), category])
    return hashlib.sha256(raw.encode()).hexdigest()[:16]
```

邮件正文包含：

- Job 名和 Build URL；
- commit；
- 阶段；
- 错误类别；
- 故障指纹；
- 最短决定性错误；
- 报告下载地址。

不得在邮件、日志或 JSON 中输出 Token、密码、私钥或完整凭据环境变量。

### 步骤 2：验证告警路径

使用 Jenkins 参数或受控测试分支制造以下测试场景：

1. `run_smoke.py` 返回 timeout；
2. 性能结果返回 `NO_COMPARABLE_BASELINE`；
3. Soak 结果返回 RSS WARN；
4. 相同故障连续触发两次；
5. 故障恢复。

验收：首次失败发邮件，重复故障在 1 小时内不重复轰炸，恢复邮件包含原故障指纹。

### 步骤 3：提交

```bash
git add ci/jenkins scripts/ci
git commit -m "ci(jenkins): 统一测试报告和邮件告警"
```

---

## 任务 10：Jenkins 集成验收

**文件：**
- 不新增仓库文件；使用前述 Pipeline 和 Harness。

### 步骤 1：配置 Jenkins Job

由 Jenkins 管理面创建或更新：

- `bbtools-coroutine-smoke`：Pipeline Script Path `ci/jenkins/smoke.Jenkinsfile`，cron `H/30 * * * *`，Label `cpp-fast`；
- `bbtools-coroutine-pr-performance`：Pipeline Script Path `ci/jenkins/performance.Jenkinsfile`，GitHub PR Webhook，Label `cpp-perf`；
- `bbtools-coroutine-soak`：Pipeline Script Path `ci/jenkins/soak.Jenkinsfile`，Label `cpp-soak`，独占 executor。

Jenkins 凭据只配置：

- GitHub 仓库只读访问；
- GitHub PR 状态回写所需凭据；
- 邮件 SMTP 凭据。

不得把上游 API Key、生产 SSH Key、Docker Socket 或 sudo 凭据提供给这些测试 Job。

### 步骤 2：Smoke 端到端验收

触发一次手动 Smoke，检查：

- checkout 固定 commit；
- 干净编译；
- CTest Smoke 运行；
- JUnit/文本报告可下载；
- 成功结果不发失败邮件；
- 受控失败能发邮件且不影响其他 Job。

### 步骤 3：PR 性能端到端验收

顺序执行：

1. main 基线构建；
2. PR 性能构建；
3. 检查 PR 使用了相同环境指纹；
4. 检查完整 JSON/Markdown/原始日志归档；
5. 检查轻度退化为 `UNSTABLE` 且不阻塞 PR；
6. 检查无基线显示 `NO_COMPARABLE_BASELINE`，不显示 PASS。

### 步骤 4：Soak 端到端验收

先运行 30 秒和 5 分钟短段，再运行完整 6 小时段：

- 单进程六模块；
- `--threads=1`；
- 资源采样和功能指标可关联；
- 自然结束；
- 下一段自动启动；
- 受控失败停止续跑并发邮件；
- 异常段报告不被后续成功段覆盖。

### 步骤 5：提交验收记录

长期决策和稳定流程写入仓库；原始日志、core、JSONL 原始采样、Jenkins 临时缓存不提交。必要时新增简短阶段证据到 `agent-docs/`，只记录结论、证据窗口、未覆盖范围和限制。

---

## 任务 11：停用 GitHub Actions 并更新文档

**文件：**
- 删除：`.github/workflows/unit_test.yml`
- 删除：`.github/workflows/memery_test_info.yml`
- 修改：`docs/ci-guide.md`

### 步骤 1：确认 Jenkins 验收门全部通过

在删除旧 Workflow 前必须有真实证据：

```text
Smoke：至少 3 次定时/手动成功，且 1 次受控失败告警
PR 性能：至少 1 次 main 基线 + 1 次 PR 比较
Soak：至少 1 个完整 6 小时段成功
邮件：失败、重复抑制、恢复路径均已验证
PR 回写：GitHub 能看到 Jenkins 状态
```

### 步骤 2：删除旧 Workflow

只在上述验收完成后执行：

```bash
git rm .github/workflows/unit_test.yml .github/workflows/memery_test_info.yml
```

不删除 `.github/` 中与测试执行无关的配置。

### 步骤 3：更新 CI 指南

`docs/ci-guide.md` 必须改为真实内容：

- Jenkins Job 名；
- Smoke 默认频率和调节方式；
- PR 性能状态和基线规则；
- Soak 资源参数和 6 小时分段；
- 邮件告警规则；
- 本地等价命令；
- 报告字段和下载位置；
- 失败分诊；
- 明确 GitHub Actions 已停用。

文档中的测试数量以 CTest 当前输出为准，不再写死过期的 16 个。

### 步骤 4：提交

```bash
git add -u .github/workflows/unit_test.yml .github/workflows/memery_test_info.yml
git add docs/ci-guide.md
git commit -m "ci(jenkins): 下线 GitHub Actions 测试流水线"
```

---

## 任务 12：最终验证和交付审查

### 步骤 1：运行 Python 和 Shell 检查

```bash
PYTHONPATH=scripts/ci python3 -m unittest discover -s scripts/ci -p 'test_*.py' -v
python3 -m compileall -q scripts/ci scripts/ci_perf_check.py scripts/record_baseline.py
bash -n scripts/ci/*.sh 2>/dev/null || true
```

仓库当前没有 CI Bash Harness 时，`bash -n` 对空匹配不作为成功证据；以 Python 编译检查和实际 Jenkins Job 为准。

### 步骤 2：运行 CMake/CTest 等价验证

```bash
rm -rf build-ci-final
cmake -S . -B build-ci-final -G Ninja \
  -DNEED_TEST=ON -DNEED_BENCHMARK=ON \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build-ci-final --parallel
ctest --test-dir build-ci-final --output-on-failure
```

预期：19 个测试注册并运行，失败数为 0；若失败，记录具体测试和环境，不以计划完成为结论。

### 步骤 3：检查仓库产物和差异

```bash
git diff --check
git status --short
git diff --stat HEAD~12..HEAD
```

检查：

- 只包含 Jenkins 测试体系相关文件；
- 没有凭据、Token、私钥和完整环境敏感值；
- 没有提交 build、coverage、core、原始 JSONL 或 Jenkins 临时日志；
- 设计规格和实现计划位于 `agent-docs/`；
- GitHub Actions 已在 Jenkins 验收后删除；
- 文档与 Pipeline 参数一致。

### 步骤 4：一致性结论

对照设计规格检查四个维度：

- 数据格式：Smoke、性能、Soak 报告字段和 Schema 一致；
- 时序：Smoke 定时、PR 性能、main 基线、Soak 分段续跑顺序一致；
- 参数传递：Jenkins 参数完整传入 Python Harness，未在 Jenkinsfile 重复硬编码；
- 禁止项：未修改核心运行时代码，未引入新第三方依赖，未接入生产凭据。

输出结论之一：`consistent`、`deviation` 或 `needs-context`，并附文件和命令证据。

```text
ARCH_RISK: MEDIUM
```

计划完成后，所有“已通过”“已迁移”“已下线”结论必须对应新鲜 Jenkins 运行记录和本地验证输出，不接受仅凭 Jenkinsfile 静态检查或代理自报。
