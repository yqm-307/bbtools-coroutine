// 定时 Smoke Pipeline（任务 6）
// 契约来源：.superpowers/sdd/task-6-brief.md + scripts/ci/run_smoke.py 的实际 argparse。
// run_smoke.py 支持受限的 --test-label，Jenkins 参数经同一 grammar 校验后
// 作为独立 argv 值传入，不使用动态 Shell 求值或字符串执行。

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

// Shell 注入防护：Jenkins string 参数在进入 sh/Python 前必须先通过
// 白名单 / 数字 / CTest label grammar 校验；校验失败即 error() 终止构建，
// 绝不把未校验的用户值拼进 shell，也不对用户值做任何求值。
def validateSmokeParameters() {
    def validBuildTypes = ['Debug', 'Release', 'RelWithDebInfo', 'MinSizeRel']
    if (!(params.SMOKE_BUILD_TYPE in validBuildTypes)) {
        error("SMOKE_BUILD_TYPE 非法: '${params.SMOKE_BUILD_TYPE}'（允许: ${validBuildTypes.join(', ')}）")
    }
    if (!(params.SMOKE_TIMEOUT_SECONDS ==~ /^\d+(\.\d+)?$/)) {
        error("SMOKE_TIMEOUT_SECONDS 非法: '${params.SMOKE_TIMEOUT_SECONDS}'（必须为正数，如 900 或 900.5）")
    }
    def timeoutSeconds = params.SMOKE_TIMEOUT_SECONDS.toDouble()
    if (timeoutSeconds <= 0 || timeoutSeconds > 1200) {
        error("SMOKE_TIMEOUT_SECONDS 必须在 (0, 1200] 范围内: '${params.SMOKE_TIMEOUT_SECONDS}'")
    }
    if (!(params.SMOKE_TEST_LABEL ==~ /^[A-Za-z0-9_.-]+$/)) {
        error("SMOKE_TEST_LABEL 非法: '${params.SMOKE_TEST_LABEL}'（仅允许字母/数字/._-）")
    }
}

pipeline {
    agent { label 'cpp-fast' }
    triggers { cron('H/30 * * * *') }
    options {
        timestamps()
        disableConcurrentBuilds()
        timeout(time: 20, unit: 'MINUTES')
    }
    parameters {
        string(name: 'SMOKE_BUILD_TYPE', defaultValue: 'Debug',
               description: 'CMake build type（白名单: Debug/Release/RelWithDebInfo/MinSizeRel）')
        string(name: 'SMOKE_TIMEOUT_SECONDS', defaultValue: '900',
               description: '单条命令超时秒数（正数，如 900 或 900.5）')
        string(name: 'SMOKE_TEST_LABEL', defaultValue: 'smoke',
               description: 'CTest label（字母/数字/._-）')
    }
    environment {
        // BUILD_TAG 由 Jenkins 生成，再清洗一次防特殊字符进入路径。
        SANITIZED_BUILD_TAG = "${env.BUILD_TAG.replaceAll(/[^A-Za-z0-9_.-]/, '_')}"
    }
    stages {
        stage('Checkout') {
            steps {
                runStageTracked { checkout scm }
            }
        }
        stage('Run Smoke') {
            steps {
                runStageTracked {
                    validateSmokeParameters()
                    // 参数已校验（白名单/数字/CTest label grammar），双引号引用传给 Python；
                    // 不拼接未校验值，不对用户值做任何 shell 求值。
                    sh """
                        python3 scripts/ci/run_smoke.py \\
                          --build-dir build-ci-smoke \\
                          --build-type "\${SMOKE_BUILD_TYPE}" \\
                          --timeout-seconds "\${SMOKE_TIMEOUT_SECONDS}" \\
                          --test-label "\${SMOKE_TEST_LABEL}" \\
                          --report-dir "tests/reports/smoke/\${SANITIZED_BUILD_TAG}"
                    """
                }
            }
        }
    }
    post {
        always {
            script {
                // 恢复检测：本构建成功且存在未恢复告警 → 发恢复邮件并清理指纹文件。
                notifyRecoveries()
                def reportDir = "tests/reports/smoke/${env.SANITIZED_BUILD_TAG}"
                def xmlPath = "${reportDir}/ctest.xml"
                def publishFailure = null
                if (fileExists(xmlPath)) {
                    // 先用 Python 标准库验证 XML；畸形文件不交给 junit，避免 post
                    // 在归档前中断。路径只含已清洗 BUILD_TAG，不接收任意 Shell 输入。
                    def xmlStatus = sh(
                        returnStatus: true,
                        script: "python3 -c 'import sys, xml.etree.ElementTree as ET; ET.parse(sys.argv[1])' \"${xmlPath}\""
                    )
                    if (xmlStatus == 0) {
                        try {
                            junit testResults: xmlPath
                        } catch (publishError) {
                            publishFailure = "JUnit publish failed: ${publishError}"
                        }
                    } else {
                        publishFailure = "ctest.xml is not valid XML: ${xmlPath}"
                    }
                } else {
                    echo "ctest.xml not found under ${reportDir}; JUnit publish skipped"
                }

                // 无论 JUnit 是否有效/可发布，都先保留 summary 和失败日志。
                archiveArtifacts artifacts: 'tests/reports/smoke/**', fingerprint: true, allowEmptyArchive: true
                if (publishFailure != null) {
                    env.FAILED_STAGE = 'Publish JUnit'
                    error(publishFailure)
                }
            }
        }
        failure {
            script {
                def commit = env.GIT_COMMIT ?: 'unknown'
                def reportDir = "tests/reports/smoke/${env.SANITIZED_BUILD_TAG}"
                def reportUrl = "${env.BUILD_URL}artifact/${reportDir}/summary.md"
                def stageName = env.FAILED_STAGE ?: 'unknown'
                def cat = 'failure'
                def rc = '?'
                // 最短决定性错误：优先取 summary.json 的 issues[0]，取不到用 category。
                def decisiveError = cat
                def summaryPath = "${reportDir}/summary.json"
                if (fileExists(summaryPath)) {
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
                        "'${summaryPath}' '${cat}'").trim()
                }
                def fp = failureFingerprint(stageName, 'smoke', rc, cat)
                // 去重：1 小时内同一指纹不重复发同一邮件（指纹文件记录最近告警时间）。
                if (alertSuppressed(fp)) {
                    echo "Alert dedup: fingerprint ${fp} alerted within ${ALERT_WINDOW_SECONDS()}s, skip email"
                } else {
                    recordAlert(fp, cat)
                    // 告警摘要先打印到 build log（SMTP 未配置前的告警出口）；emailext 保留待启用。
                    echo "[ALERT] ${env.JOB_NAME} #${env.BUILD_NUMBER} ${stageName} fingerprint=${fp} category=${cat} decisive=${decisiveError} report=${reportUrl}"
                    // 正文只含 Job/Build URL/commit/阶段/类别/指纹/最短决定性错误/报告 URL，
                    // 不含任何凭据或敏感配置。
                    emailext(
                        to: '$DEFAULT_RECIPIENTS',
                        subject: "FAILED: ${env.JOB_NAME} #${env.BUILD_NUMBER} (${stageName})",
                        body: """Smoke pipeline failed.

Job: ${env.JOB_NAME}
Build: ${env.BUILD_NUMBER}
Build URL: ${env.BUILD_URL}
Commit: ${commit}
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
