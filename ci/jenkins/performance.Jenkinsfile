// 性能与基线 Pipeline（任务 7）
// 契约来源：.superpowers/sdd/task-7-brief.md + scripts/ci_perf_check.py / scripts/record_baseline.py 的实际 argparse。
// 偏差说明：简报示例脚本名 run_pr_performance.py 为计划占位，本仓实际入口是
// scripts/ci_perf_check.py（--dur/--threads/--baseline/--output/--gate-enabled）与
// scripts/record_baseline.py record；Jenkinsfile 直接调用现有 CLI。
// 构建目录用 scripts 硬编码的 build/（简报的 build-ci-perf 属于占位脚本的 --build-dir 语义）。

// 阶段失败跟踪：失败阶段写入 env.FAILED_STAGE，供 post{failure} 邮件引用。
def runStageTracked(Closure body) {
    try {
        body()
    } catch (e) {
        env.FAILED_STAGE = env.STAGE_NAME
        throw e
    }
}

// ---- 统一告警：故障指纹 + 去重 + 恢复（任务 9）----
// 指纹 = sha256("|".join([job, stage, test_name, returncode, category]))[:16]，
// 与 scripts/ci/ci_common.py::failure_fingerprint 同构，由固定 python 行计算
// （chr(124) 即 '|'，避免引号转义）；只接收 Jenkins 生成值与受控字符串，
// 绝不 eval 用户输入。指纹文件在 workspace 固定相对路径，只记录时间戳与
// 类别，不含任何凭据；1 小时内同一指纹不重复发同一邮件。
def ALERT_DIR() { 'tests/reports/alerts' }
def ALERT_WINDOW_SECONDS() { 3600 }

def failureFingerprint(stageName, testName, returncode, category) {
    // argv 值可能来自外部（env.JOB_NAME 等），拼接进 sh 前必须白名单过滤；
    // 非法值（含单引号等）统一降级为 '?'，防止引号闭合/注入使告警路径静默失效。
    def safe = { v -> (v ==~ /^[A-Za-z0-9_ .:?+-]+$/) ? v : '?' }
    def job = safe(env.JOB_NAME ?: '')
    def stage = safe(stageName ?: '')
    def test = safe(testName ?: '')
    def rc = safe(returncode ?: '')
    def cat = safe(category ?: '')
    return sh(
        returnStdout: true,
        script:
            "python3 -c 'import hashlib,sys;print(hashlib.sha256(chr(124).join(sys.argv[1:]).encode()).hexdigest()[:16])' " +
            "'${job}' '${stage}' '${test}' '${rc}' '${cat}'"
    ).trim()
}

// 指纹文件里的最近告警时间戳；文件缺失/内容非法返回 null。
def lastAlertTs(fp) {
    def marker = "${ALERT_DIR()}/${fp}.alert"
    if (!fileExists(marker)) { return null }
    def raw = sh(returnStdout: true, script:
        "python3 -c 'import sys;print(open(sys.argv[1]).read().split()[0])' '${marker}'").trim()
    return (raw ==~ /^\d+$/) ? raw.toLong() : null
}

// 去重判定：1 小时内同一指纹已发过告警 → true（不重复发同一邮件）。
def alertSuppressed(fp) {
    def last = lastAlertTs(fp)
    if (last == null) { return false }
    def now = sh(returnStdout: true, script:
        "python3 -c 'import time;print(int(time.time()))'").trim().toLong()
    return (now - last) < ALERT_WINDOW_SECONDS()
}

// 记录本次告警：只写时间戳与类别（类别仅接受受控字符，防注入），供去重与恢复引用。
def recordAlert(fp, category) {
    def cat = (category ==~ /^[A-Za-z0-9_:-]+$/) ? category : 'unknown'
    def now = sh(returnStdout: true, script:
        "python3 -c 'import time;print(int(time.time()))'").trim()
    if (!(now ==~ /^\d+$/)) { now = '0' }
    sh "mkdir -p '${ALERT_DIR()}'"
    sh "echo '${now}' '${cat}' > '${ALERT_DIR()}/${fp}.alert'"
}

// 恢复：本构建成功（FAIL→PASS）且存在未恢复告警 → 发恢复邮件（含原故障指纹）
// 并清理指纹文件；WARN→PASS 的告警同样按恢复处理。
def notifyRecoveries() {
    if (currentBuild.result != null && currentBuild.result != 'SUCCESS') { return }
    sh "mkdir -p '${ALERT_DIR()}'"
    def markers = sh(returnStdout: true, script:
        "ls '${ALERT_DIR()}' 2>/dev/null || true").trim().split(/\s+/)
    def pending = markers.findAll { it && it ==~ /^[0-9a-f]{16}\.alert$/ }
    if (pending.isEmpty()) { return }
    def recovered = []
    for (m in pending) {
        def cat = sh(returnStdout: true, script:
            "python3 -c 'import sys;print(open(sys.argv[1]).read().split()[-1])' " +
            "'${ALERT_DIR()}/${m}'").trim()
        recovered << m.replaceAll(/\.alert$/, '') + " (" + cat + ")"
    }
    def commit = env.GIT_COMMIT ?: 'unknown'
    echo "[RECOVERED] ${env.JOB_NAME} #${env.BUILD_NUMBER} fingerprint(s)=${recovered.join(', ')}"
    emailext(
        to: '$DEFAULT_RECIPIENTS',
        subject: "RECOVERED: ${env.JOB_NAME} #${env.BUILD_NUMBER}",
        body: """Pipeline recovered.

Job: ${env.JOB_NAME}
Build: ${env.BUILD_NUMBER}
Build URL: ${env.BUILD_URL}
Commit: ${commit}
Recovered fingerprint(s): ${recovered.join(', ')}
Report: ${env.BUILD_URL}artifact/
Build log: ${env.BUILD_URL}console
""",
        mimeType: 'text/plain'
    )
    for (m in pending) {
        sh "rm -f '${ALERT_DIR()}/${m}'"
    }
}

// 分支上下文：PR（CHANGE_ID 存在）在 cpp-perf Agent 上对比 main 基线；
// main 生成并归档新基线，作为后续 PR 的唯一候选基线来源。其他上下文 fail-closed。
def isPullRequest() { (env.CHANGE_ID ?: '') != '' }
def isMainBranch() { (env.BRANCH_NAME ?: '') == 'main' }

// Shell 注入防护：Jenkins string 参数在进入 sh 前必须先通过白名单 / 数字校验；
// 校验失败即 error() 终止构建，绝不把未校验的用户值拼进 shell。
// PERF_GATE_ENABLED 为 booleanParam（原生布尔），只作为固定 flag 常量进入命令。
def validatePerfParameters() {
    if (!(params.PERF_DURATION_SECONDS ==~ /^\d+$/)) {
        error("PERF_DURATION_SECONDS 非法: '${params.PERF_DURATION_SECONDS}'（必须为正整数秒）")
    }
    def duration = params.PERF_DURATION_SECONDS.toInteger()
    if (duration < 1 || duration > 3600) {
        error("PERF_DURATION_SECONDS 必须在 [1, 3600] 秒: '${params.PERF_DURATION_SECONDS}'")
    }
    if (!(params.PERF_THREADS ==~ /^\d+$/)) {
        error("PERF_THREADS 非法: '${params.PERF_THREADS}'（必须为正整数）")
    }
    def threads = params.PERF_THREADS.toInteger()
    if (threads < 1 || threads > 256) {
        error("PERF_THREADS 必须在 [1, 256]: '${params.PERF_THREADS}'")
    }
    // PERF_BASELINE_BUILD 仅允许空串或纯数字（Jenkins 构建号），且只作为
    // Copy Artifact selector 参数使用，绝不进入 shell。
    if (params.PERF_BASELINE_BUILD != '' && !(params.PERF_BASELINE_BUILD ==~ /^\d+$/)) {
        error("PERF_BASELINE_BUILD 非法: '${params.PERF_BASELINE_BUILD}'（必须为空或纯数字构建号）")
    }
}

// 基线来源 Job：multibranch 拓扑下 PR Job 的兄弟 main 分支 Job
// （JOB_NAME 末段 'PR-xxx' → 'main'）；独立 Job 使用自身 JOB_NAME，
// 由操作者用 PERF_BASELINE_BUILD 显式指定基线构建号。
// JOB_NAME 由 Jenkins 生成，仅作 Groovy API 参数，不进入 shell。
def baselineProjectName() {
    def job = env.JOB_NAME ?: ''
    def branch = env.BRANCH_NAME ?: ''
    if (branch != '' && branch != 'main') {
        def idx = job.lastIndexOf('/')
        if (idx > 0) {
            return job.substring(0, idx + 1) + 'main'
        }
    }
    return job
}

// 固定路径常量：全部为仓库内受控路径，不接收任何用户输入。
def PERF_RESULT_DIR() { 'tests/reports/performance' }
def PERF_BASELINE_DIR() { 'tests/baselines/jenkins' }
def PERF_RESULT_JSON() { "${PERF_RESULT_DIR()}/result.json" }
def PERF_BASELINE_JSON() { "${PERF_BASELINE_DIR()}/baseline.json" }

pipeline {
    agent { label 'cpp-perf' }
    options {
        timestamps()
        disableConcurrentBuilds()
        timeout(time: 120, unit: 'MINUTES')
    }
    parameters {
        string(name: 'PERF_DURATION_SECONDS', defaultValue: '45',
               description: '单模块压测时长（秒，正整数，1..3600）')
        string(name: 'PERF_THREADS', defaultValue: '2',
               description: '压测线程数（正整数，1..256）')
        booleanParam(name: 'PERF_GATE_ENABLED', defaultValue: false,
               description: '启用 FAIL 门禁（默认关闭：>20% 退化仅报 WARN，不阻塞 PR）')
        string(name: 'PERF_BASELINE_BUILD', defaultValue: '',
               description: 'main 基线构建号（空 = 最近成功 main 构建；仅纯数字）')
    }
    stages {
        stage('Checkout') {
            steps {
                runStageTracked { checkout scm }
            }
        }
        stage('Validate Parameters') {
            steps {
                runStageTracked { validatePerfParameters() }
            }
        }
        stage('Resolve Context') {
            steps {
                runStageTracked {
                    script {
                        if (isPullRequest()) {
                            env.PIPELINE_MODE = 'pr'
                        } else if (isMainBranch()) {
                            env.PIPELINE_MODE = 'main'
                        } else {
                            error("不支持的构建上下文：BRANCH_NAME='${env.BRANCH_NAME ?: ''}' " +
                                  "CHANGE_ID='${env.CHANGE_ID ?: ''}'（仅支持 main 或 PR）")
                        }
                        echo "PIPELINE_MODE=${env.PIPELINE_MODE}"
                    }
                }
            }
        }
        stage('Build Performance Target') {
            steps {
                runStageTracked {
                    // 固定构建：Release + Ninja，仅 unified_stress 目标；
                    // scripts 硬编码 build/ 目录（ci_perf_check.py / record_baseline.py）。
                    sh '''
                        rm -rf build
                        cmake -S . -B build -G Ninja \
                          -DNEED_TEST=OFF -DNEED_BENCHMARK=ON \
                          -DCMAKE_BUILD_TYPE=Release
                        cmake --build build --target unified_stress --parallel
                    '''
                }
            }
        }
        stage('Acquire Baseline') {
            when { expression { env.PIPELINE_MODE == 'pr' } }
            steps {
                runStageTracked {
                    script {
                        sh "mkdir -p '${PERF_BASELINE_DIR()}'"
                        // 必须先清工作区残留：同一 Job 先跑 main 再跑 PR 时，
                        // 本地 leftover 会让 Copy Artifact 失败仍显示 COPIED=true（build #3 已复现）。
                        sh "rm -f '${PERF_BASELINE_JSON()}'"
                        try {
                            // copyartifact 795：LastSuccessfulBuildSelector / failIfNoArtifacts
                            // 已移除；Pipeline 用 copyArtifacts + lastSuccessful()/specific() + optional。
                            // 独立 Job 的 LastSuccessful 可能是 PR 构建（无基线产物），
                            // 操作者应用 PERF_BASELINE_BUILD 显式指定 main 基线构建号。
                            if (params.PERF_BASELINE_BUILD != '') {
                                copyArtifacts(
                                    projectName: baselineProjectName(),
                                    selector: specific(params.PERF_BASELINE_BUILD),
                                    filter: 'tests/baselines/jenkins/baseline.json',
                                    target: PERF_BASELINE_DIR(),
                                    flatten: true,
                                    optional: true,
                                    fingerprintArtifacts: true)
                            } else {
                                copyArtifacts(
                                    projectName: baselineProjectName(),
                                    selector: lastSuccessful(),
                                    filter: 'tests/baselines/jenkins/baseline.json',
                                    target: PERF_BASELINE_DIR(),
                                    flatten: true,
                                    optional: true,
                                    fingerprintArtifacts: true)
                            }
                        } catch (e) {
                            echo "Copy Artifact 失败（按无基线处理，不阻塞 PR）: ${e}"
                        }
                        env.PERF_BASELINE_COPIED = fileExists(PERF_BASELINE_JSON()) ? 'true' : 'false'
                        echo "PERF_BASELINE_COPIED=${env.PERF_BASELINE_COPIED}"

                        if (env.PERF_BASELINE_COPIED == 'true') {
                            // 基线 Schema 预检：不可解析 / 缺 modules / 缺 verdict 视为
                            // Schema 无效 → FAILURE；环境指纹一致性由 ci_perf_check.py 的
                            // compare_environment 判定（不一致 → NO_COMPARABLE_BASELINE）。
                            def schema = sh(returnStdout: true, script:
                                "python3 -c 'import json,sys;d=json.load(open(sys.argv[1]));" +
                                "print(\"OK\" if isinstance(d.get(\"modules\"),dict) and d.get(\"verdict\") else \"BAD\")' " +
                                "'${PERF_BASELINE_JSON()}'").trim()
                            if (schema != 'OK') {
                                currentBuild.result = 'FAILURE'
                                error("基线 Schema 无效（不可解析或缺少 modules/verdict）：${PERF_BASELINE_JSON()}")
                            }
                        } else {
                            // 无可用基线：写显式“无基线”标记，禁止 ci_perf_check 自动查找
                            // 工作区历史基线（基线只允许来自 main 构建产物）；标记令其按
                            // NO_COMPARABLE_BASELINE 处理 → UNSTABLE + 邮件，绝不绿色通过。
                            sh "echo '{\"environment\":{},\"modules\":{}}' > '${PERF_BASELINE_JSON()}'"
                        }
                    }
                }
            }
        }
        stage('Run Performance Check') {
            when { expression { env.PIPELINE_MODE == 'pr' } }
            steps {
                runStageTracked {
                    script {
                        sh "mkdir -p '${PERF_RESULT_DIR()}'"
                        def gateArg = params.PERF_GATE_ENABLED ? '--gate-enabled' : ''
                        // 参数已校验（数字/布尔白名单），独立 argv 传给 Python；
                        // gate 为固定 flag 常量，baseline/output 为受控固定路径，不拼接未校验值。
                        def rc = sh(returnStatus: true, script: """
                            python3 scripts/ci_perf_check.py \\
                              --dur "\${PERF_DURATION_SECONDS}" \\
                              --threads "\${PERF_THREADS}" \\
                              ${gateArg} \\
                              --baseline "${PERF_BASELINE_JSON()}" \\
                              --output "${PERF_RESULT_JSON()}" \\
                              > "${PERF_RESULT_DIR()}/perf-run.log" 2>&1
                        """)
                        env.PERF_RC = "${rc}"
                        // 报告由 ci_perf_check.py 校验通过后 write_json_atomic（.tmp + os.replace）
                        // 原子落盘；这里只在进程结束后读取，且读取失败按 FAILURE 处理，绝不消费半写文件。
                        def verdict = ''
                        if (fileExists(PERF_RESULT_JSON())) {
                            verdict = sh(returnStdout: true, script:
                                "python3 -c 'import json,sys;print(json.load(open(sys.argv[1])).get(\"verdict\",\"\"))' " +
                                "'${PERF_RESULT_JSON()}'").trim()
                        }
                        env.PERF_VERDICT = verdict ?: 'UNKNOWN'
                        echo "perf rc=${rc} verdict=${verdict}"
                        if (verdict == 'NO_COMPARABLE_BASELINE') {
                            // 无可比基线：UNSTABLE + 邮件，不阻塞 PR，但绝不绿色通过。
                            unstable("NO_COMPARABLE_BASELINE：无可用且环境指纹匹配的 main 基线，本次不比较")
                        } else if (verdict == 'WARN') {
                            // 轻度退化：UNSTABLE + 邮件，不阻塞 PR。
                            unstable("性能轻度退化（WARN）：详见 ${PERF_RESULT_DIR()}/result.md")
                        } else if (rc == 0 && verdict == 'PASS') {
                            echo "性能检查通过（PASS）"
                        } else {
                            // crash / timeout / zero ops / Schema 无效 / 硬错误 → FAILURE + 邮件；
                            // 本阶段不设置任何 PR 阻塞门禁（如 githubChecks），初期不阻塞合入。
                            currentBuild.result = 'FAILURE'
                            error("性能检查失败：rc=${rc} verdict=${verdict}（详情见 ${PERF_RESULT_DIR()}/perf-run.log）")
                        }
                    }
                }
            }
        }
        stage('Record Baseline') {
            when { expression { env.PIPELINE_MODE == 'main' } }
            steps {
                runStageTracked {
                    script {
                        sh "mkdir -p '${PERF_BASELINE_DIR()}'"
                        env.PERF_BASELINE_OK = 'false'
                        def rc = sh(returnStatus: true, script: """
                            python3 scripts/record_baseline.py record \\
                              --threads "\${PERF_THREADS}" \\
                              --dur "\${PERF_DURATION_SECONDS}" \\
                              --output "${PERF_BASELINE_JSON()}" \\
                              > "${PERF_BASELINE_DIR()}/record.log" 2>&1
                        """)
                        env.PERF_RC = "${rc}"
                        def ok = (rc == 0)
                        if (!ok) {
                            echo "record_baseline 退出码非零：rc=${rc}（见 ${PERF_BASELINE_DIR()}/record.log）"
                        }
                        if (ok && fileExists(PERF_BASELINE_JSON())) {
                            // 半成品判定：verdict != PASS 或任一模块 error → 不发布基线。
                            def check = sh(returnStdout: true, script:
                                "python3 -c 'import json,sys;d=json.load(open(sys.argv[1]));" +
                                "broken=[k for k,v in (d.get(\"modules\") or {}).items() if v.get(\"error\")];" +
                                "print(\"OK\" if d.get(\"verdict\")==\"PASS\" and not broken else \"BAD \"+str(broken))' " +
                                "'${PERF_BASELINE_JSON()}'").trim()
                            ok = check == 'OK'
                            if (!ok) {
                                echo "基线内容无效：${check}"
                            }
                        } else {
                            ok = false
                        }
                        if (!ok) {
                            // main 基线失败 → 构建失败，删除本地文件，不归档半成品。
                            sh "rm -f '${PERF_BASELINE_JSON()}'"
                            currentBuild.result = 'FAILURE'
                            error("main 基线记录/校验失败：构建失败，不发布半成品基线")
                        }
                        env.PERF_BASELINE_OK = 'true'
                        // 提取 environment.json 与 Markdown 报告。必须单行 python -c：
                        // Groovy 三引号会把源码缩进带进 -c 字符串，导致 IndentationError
                        //（build #1 已复现：基线 PASS 后仍 FAILURE）。
                        sh """
                            PYTHONPATH=scripts/ci python3 -c 'import json,sys;from perf_contract import write_markdown_report;d=json.load(open(sys.argv[1]));json.dump(d[\"environment\"],open(sys.argv[2],\"w\"),ensure_ascii=False,indent=2);open(sys.argv[2],\"a\").write(\"\\n\");write_markdown_report(sys.argv[3],d)' '${PERF_BASELINE_JSON()}' '${PERF_BASELINE_DIR()}/environment.json' '${PERF_BASELINE_DIR()}/baseline.md'
                        """
                        echo "main 基线已生成并校验：${PERF_BASELINE_JSON()}"
                    }
                }
            }
        }
    }
    post {
        always {
            script {
                // 恢复检测：本构建成功且存在未恢复告警 → 发恢复邮件并清理指纹文件。
                notifyRecoveries()
                if (env.PIPELINE_MODE == 'main') {
                    if (env.PERF_BASELINE_OK == 'true') {
                        // main 只归档通过校验的基线产物：baseline/environment/Markdown。
                        archiveArtifacts artifacts: 'tests/baselines/jenkins/baseline.json,tests/baselines/jenkins/environment.json,tests/baselines/jenkins/baseline.md', fingerprint: true
                        echo "main 基线已归档"
                    } else {
                        // 基线失败或未执行：绝不归档半成品基线。
                        echo "main 基线未通过校验，未归档（禁止发布半成品基线）"
                    }
                } else {
                    // PR 归档候选完整结果（JSON+Markdown）与原始运行日志。
                    archiveArtifacts artifacts: 'tests/reports/performance/**', fingerprint: true, allowEmptyArchive: true
                }
            }
        }
        unstable {
            script {
                def commit = env.GIT_COMMIT ?: 'unknown'
                def reportUrl = "${env.BUILD_URL}artifact/tests/reports/performance/result.md"
                def stageName = 'Run Performance Check'
                def cat = env.PERF_VERDICT ?: 'unknown'
                def rc = env.PERF_RC ?: '?'
                // 最短决定性错误：优先取 result.json 的 issues[0]，取不到用 category。
                def decisiveError = cat
                if (fileExists(PERF_RESULT_JSON())) {
                    // 最短决定性错误：优先取 issues[0]，取不到或用 category 兜底；
                    // JSON 损坏/解析异常时降级为 category，绝不因告警路径异常吞掉邮件。
                    decisiveError = sh(returnStdout: true, script:
                        "python3 -c 'import json,sys\n" +
                        "try:\n" +
                        " d=json.load(open(sys.argv[1]))\n" +
                        " i=d.get(\"issues\") or []\n" +
                        " print(i[0] if i else sys.argv[2])\n" +
                        "except Exception:\n" +
                        " print(sys.argv[2])' " +
                        "'${PERF_RESULT_JSON()}' '${cat}'").trim()
                }
                def fp = failureFingerprint(stageName, 'perf', rc, cat)
                // 去重：1 小时内同一指纹不重复发同一邮件（指纹文件记录最近告警时间）。
                if (alertSuppressed(fp)) {
                    echo "Alert dedup: fingerprint ${fp} alerted within ${ALERT_WINDOW_SECONDS()}s, skip email"
                } else {
                    recordAlert(fp, cat)
                    // 告警摘要先打印到 build log（SMTP 未配置前的告警出口）；emailext 保留待启用。
                    echo "[ALERT] ${env.JOB_NAME} #${env.BUILD_NUMBER} ${stageName} fingerprint=${fp} category=${cat} decisive=${decisiveError}"
                    // 正文只含 Job/Build URL/commit/阶段/类别/指纹/最短决定性错误/报告 URL，
                    // 不含任何凭据或敏感配置。
                    emailext(
                        to: '$DEFAULT_RECIPIENTS',
                        subject: "UNSTABLE: ${env.JOB_NAME} #${env.BUILD_NUMBER} (${cat})",
                        body: """Performance pipeline unstable.

Job: ${env.JOB_NAME}
Build: ${env.BUILD_NUMBER}
Build URL: ${env.BUILD_URL}
Commit: ${commit}
Mode: ${env.PIPELINE_MODE ?: 'unknown'}
Stage: ${stageName}
Category: ${cat} (rc=${rc})
Failure fingerprint: ${fp}
Decisive error: ${decisiveError}
Report: ${reportUrl}
Build log: ${env.BUILD_URL}console
""",
                        mimeType: 'text/plain'
                    )
                }
            }
        }
        failure {
            script {
                def commit = env.GIT_COMMIT ?: 'unknown'
                def reportUrl = "${env.BUILD_URL}artifact/tests/reports/performance/result.md"
                def stageName = env.FAILED_STAGE ?: 'unknown'
                def cat = (env.PERF_VERDICT ?: '') in ['FAIL', 'WARN', 'NO_COMPARABLE_BASELINE']
                    ? env.PERF_VERDICT : 'failure'
                def rc = env.PERF_RC ?: '?'
                // 最短决定性错误：优先取 result.json 的 issues[0]，取不到用 category。
                def decisiveError = cat
                if (fileExists(PERF_RESULT_JSON())) {
                    // 最短决定性错误：优先取 issues[0]，取不到或用 category 兜底；
                    // JSON 损坏/解析异常时降级为 category，绝不因告警路径异常吞掉邮件。
                    decisiveError = sh(returnStdout: true, script:
                        "python3 -c 'import json,sys\n" +
                        "try:\n" +
                        " d=json.load(open(sys.argv[1]))\n" +
                        " i=d.get(\"issues\") or []\n" +
                        " print(i[0] if i else sys.argv[2])\n" +
                        "except Exception:\n" +
                        " print(sys.argv[2])' " +
                        "'${PERF_RESULT_JSON()}' '${cat}'").trim()
                }
                def fp = failureFingerprint(stageName, 'perf', rc, cat)
                // 去重：1 小时内同一指纹不重复发同一邮件（指纹文件记录最近告警时间）。
                if (alertSuppressed(fp)) {
                    echo "Alert dedup: fingerprint ${fp} alerted within ${ALERT_WINDOW_SECONDS()}s, skip email"
                } else {
                    recordAlert(fp, cat)
                    // 告警摘要先打印到 build log（SMTP 未配置前的告警出口）；emailext 保留待启用。
                    echo "[ALERT] ${env.JOB_NAME} #${env.BUILD_NUMBER} ${stageName} fingerprint=${fp} category=${cat} decisive=${decisiveError}"
                    // 正文只含 Job/Build URL/commit/阶段/类别/指纹/最短决定性错误/报告 URL，
                    // 不含任何凭据或敏感配置。
                    emailext(
                        to: '$DEFAULT_RECIPIENTS',
                        subject: "FAILED: ${env.JOB_NAME} #${env.BUILD_NUMBER} (${stageName})",
                        body: """Performance pipeline failed.

Job: ${env.JOB_NAME}
Build: ${env.BUILD_NUMBER}
Build URL: ${env.BUILD_URL}
Commit: ${commit}
Mode: ${env.PIPELINE_MODE ?: 'unknown'}
Stage: ${stageName}
Category: ${cat} (rc=${rc})
Failure fingerprint: ${fp}
Decisive error: ${decisiveError}
Report: ${reportUrl}
Build log: ${env.BUILD_URL}console
""",
                        mimeType: 'text/plain'
                    )
                }
            }
        }
    }
}
