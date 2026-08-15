// 分段持续疲劳（Soak）Pipeline（任务 8）
// 契约来源：.superpowers/sdd/task-8-brief.md + scripts/ci/run_soak.py 的实际 argparse。
// 独占 cpp-soak agent；CPU quota / MemoryHigh / MemoryMax 在 Agent cgroup/容器层配置，
// Pipeline 只读取 summary.json 并归档报告，不自行修改主机资源限制。
//
// 分段续跑：每段是一个明确 Jenkins stage（script 内 while + stage("Segment N")），
// 每段独立 build log；段内失败（构建/运行/归档/判定）立即 error() 传播终止构建，
// 绝不使用无限 Shell 循环吞掉失败。续跑只发生在"当前段完整归档且结果为通过"之后，
// 且受 SOAK_CONTINUE 参数与段数上限控制。
//
// 判段依据 summary.json：PASS→续跑；WARN→归档+邮件+续跑由 SOAK_CONTINUE 控制；
// FAIL/报告缺失/JSON 无效→结束+邮件+不续跑（fail-closed）。

// 阶段失败跟踪：失败阶段写入 env.FAILED_STAGE，供 post{failure} 邮件引用。
def runStageTracked(Closure body) {
    try {
        body()
    } catch (e) {
        env.FAILED_STAGE = env.STAGE_NAME
        throw e
    }
}

// 段数上限：防失控循环的硬护栏（真实段数由 SOAK_CONTINUE 与结果决定）。
def MAX_SOAK_SEGMENTS() { 8 }

// 固定路径常量：全部为仓库内受控路径，不接收任何用户输入。
def SOAK_REPORT_ROOT() { 'tests/reports/soak' }
def SOAK_BUILD_DIR() { 'build-soak' }

// Shell 注入防护：duration/threads/两个 interval 均为 string 参数，在进入 sh 前
// 必须通过数字格式白名单 + 范围校验；校验失败即 error() fail-closed，绝不把
// 未校验的用户值拼进 shell。SOAK_CONTINUE 是 booleanParam 原生布尔，只作 Groovy
// 布尔分支（不进入 shell、不 eval、无字符串求值）。
def validateSoakParameters() {
    def numRe = /^\d+(\.\d+)?$/
    def intRe = /^\d+$/
    if (!(params.SOAK_DURATION_SECONDS ==~ numRe)) {
        error("SOAK_DURATION_SECONDS 非法: '${params.SOAK_DURATION_SECONDS}'（必须为正数秒，如 21600 或 30.5）")
    }
    def duration = params.SOAK_DURATION_SECONDS.toDouble()
    if (duration <= 0 || duration > 28800) {
        error("SOAK_DURATION_SECONDS 必须在 (0, 28800] 秒（≤8h timeout）: '${params.SOAK_DURATION_SECONDS}'")
    }
    if (!(params.SOAK_THREADS ==~ intRe)) {
        error("SOAK_THREADS 非法: '${params.SOAK_THREADS}'（必须为正整数）")
    }
    def threads = params.SOAK_THREADS.toInteger()
    if (threads < 1 || threads > 256) {
        error("SOAK_THREADS 必须在 [1, 256]: '${params.SOAK_THREADS}'")
    }
    if (!(params.SOAK_RESOURCE_INTERVAL_SECONDS ==~ numRe)) {
        error("SOAK_RESOURCE_INTERVAL_SECONDS 非法: '${params.SOAK_RESOURCE_INTERVAL_SECONDS}'（必须为正数秒）")
    }
    def resInterval = params.SOAK_RESOURCE_INTERVAL_SECONDS.toDouble()
    if (resInterval <= 0 || resInterval > 3600) {
        error("SOAK_RESOURCE_INTERVAL_SECONDS 必须在 (0, 3600] 秒: '${params.SOAK_RESOURCE_INTERVAL_SECONDS}'")
    }
    if (!(params.SOAK_METRIC_INTERVAL_SECONDS ==~ numRe)) {
        error("SOAK_METRIC_INTERVAL_SECONDS 非法: '${params.SOAK_METRIC_INTERVAL_SECONDS}'（必须为正数秒）")
    }
    def metricInterval = params.SOAK_METRIC_INTERVAL_SECONDS.toDouble()
    if (metricInterval <= 0 || metricInterval > 3600) {
        error("SOAK_METRIC_INTERVAL_SECONDS 必须在 (0, 3600] 秒: '${params.SOAK_METRIC_INTERVAL_SECONDS}'")
    }
    // SOAK_CONTINUE：booleanParam 原生布尔，无需字符串校验，仅映射为 Groovy 布尔分支。
    echo "Soak 参数校验通过: duration=${duration}s threads=${threads} " +
         "resourceInterval=${resInterval}s metricInterval=${metricInterval}s continue=${params.SOAK_CONTINUE}"
}

pipeline {
    agent { label 'cpp-soak' }
    options {
        timestamps()
        disableConcurrentBuilds()
        timeout(time: 8, unit: 'HOURS')
    }
    parameters {
        string(name: 'SOAK_DURATION_SECONDS', defaultValue: '21600',
               description: '单段持续时长（秒，正数，≤28800；默认 6 小时）')
        string(name: 'SOAK_THREADS', defaultValue: '1',
               description: '压测线程数（正整数，1..256）')
        string(name: 'SOAK_RESOURCE_INTERVAL_SECONDS', defaultValue: '10',
               description: '资源采样间隔（秒，正数，≤3600）')
        string(name: 'SOAK_METRIC_INTERVAL_SECONDS', defaultValue: '60',
               description: '六模块指标采样间隔（秒，正数，≤3600）')
        booleanParam(name: 'SOAK_CONTINUE', defaultValue: true,
               description: '段通过后是否续跑下一段（PASS/WARN 均受此控制；FAIL/报告缺失不续跑）')
    }
    stages {
        stage('Validate Parameters') {
            steps {
                runStageTracked { validateSoakParameters() }
            }
        }
        stage('Run Soak Segments') {
            steps {
                runStageTracked {
                    script {
                        // 段循环：Jenkins stage 循环而非 Shell 循环。每段独立 stage 与 build log；
                        // 段内任何失败（构建/运行/归档/判定）都 error() 抛出并终止构建，不吞失败。
                        // 续跑 = 本段完整归档且通过（PASS/WARN）&& SOAK_CONTINUE && 未达段数上限。
                        def seg = 1
                        def continueSoak = true
                        while (continueSoak && seg <= MAX_SOAK_SEGMENTS()) {
                            stage("Segment ${seg}") {
                                echo "=== Soak Segment ${seg} 开始 ==="
                                // 固定 commit：checkout scm 锁定本构建的 SCM revision（main 固定 commit），
                                // 段间重复 checkout 幂等，工作区不漂移。
                                checkout scm

                                // 构建 unified_stress 到 build-soak（首次全量，段间增量；二进制路径
                                // build-soak/bin/benchmark_test/unified_stress 与 run_soak.py 默认一致）。
                                sh '''
                                    cmake -S . -B build-soak -G Ninja \
                                      -DNEED_TEST=OFF -DNEED_BENCHMARK=ON \
                                      -DCMAKE_BUILD_TYPE=Release
                                    cmake --build build-soak --target unified_stress --parallel
                                '''

                                // 运行一段 Soak：stdout/stderr 落入 segment-N.log 保留异常证据；
                                // returnStatus 捕获退出码（PASS/WARN=0，FAIL=1，参数错误=2），
                                // 判段不依赖退出码而以 summary.json 为准，报告缺失/坏 JSON 按 FAIL。
                                def rc = sh(returnStatus: true, script: """
                                    mkdir -p '${SOAK_REPORT_ROOT()}'
                                    python3 scripts/ci/run_soak.py \\
                                      --build-dir '${SOAK_BUILD_DIR()}' \\
                                      --duration-seconds "\\${SOAK_DURATION_SECONDS}" \\
                                      --threads "\\${SOAK_THREADS}" \\
                                      --resource-interval-seconds "\\${SOAK_RESOURCE_INTERVAL_SECONDS}" \\
                                      --metric-interval-seconds "\\${SOAK_METRIC_INTERVAL_SECONDS}" \\
                                      --report-dir '${SOAK_REPORT_ROOT()}' \\
                                      > '${SOAK_REPORT_ROOT()}/segment-${seg}.log' 2>&1
                                """)
                                env.SOAK_RC = "${rc}"
                                echo "Segment ${seg} run_soak rc=${rc}"

                                // 归档五件套（resource/metrics/summary/raw + Markdown）：
                                // 续跑只允许发生在当前段完整归档之后；归档失败同样 error() 传播。
                                archiveArtifacts artifacts: 'tests/reports/soak/**', fingerprint: true, allowEmptyArchive: true

                                // 判段：run_id 从本段日志的 "report:" 行解析（run_soak 在五件套
                                // write_reports 之后才打印该行，故日志含 report 行 = 本段报告已完整落盘；
                                // 无 report 行 = 未创建/未写完整，按报告缺失 → FAIL）。
                                // 输出 STATUS=<PASS|WARN|FAIL> RUN=<runId>；报告缺失或 JSON 无效一律 FAIL。
                                def judgeOut = sh(returnStdout: true, script: """python3 - <<'PY' '${SOAK_REPORT_ROOT()}' '${SOAK_REPORT_ROOT()}/segment-${seg}.log'
import json, os, re, sys
root, log = sys.argv[1], sys.argv[2]
run_id = ""
if os.path.isfile(log):
    with open(log, errors="replace") as f:
        for line in f:
            mm = re.search(r"report: .*?([0-9]{8}T[0-9]{6}Z(-[0-9]{2})?)", line.strip())
            if mm:
                run_id = mm.group(1)
                break
status = "FAIL"
path = os.path.join(root, run_id, "summary.json") if run_id else ""
if run_id and os.path.isfile(path):
    try:
        with open(path) as f:
            status = json.load(f).get("status", "")
        if status not in ("PASS", "WARN", "FAIL"):
            status = "FAIL"
    except Exception:
        status = "FAIL"
print("STATUS=" + status + " RUN=" + run_id)
PY
""").trim()
                                def m = (judgeOut =~ /STATUS=(\S+)\s+RUN=(\S*)/)
                                if (!m.find()) {
                                    error("无法解析 Soak 判段输出: '${judgeOut}'")
                                }
                                def segmentStatus = m.group(1)
                                def runId = m.group(2)
                                // runId 来自脚本生成的 UTC 目录名（^\d{8}T\d{6}Z(-NN)?$），
                                // 仅用于归档路径/报告 URL 拼接，不进入 shell；异常值直接失败。
                                if (runId != '' && !(runId ==~ /^\d{8}T\d{6}Z(-\d{2})?$/)) {
                                    error("异常 Soak run 目录名: '${runId}'")
                                }
                                env.SOAK_SEGMENT = "${seg}"
                                env.SOAK_STATUS = segmentStatus
                                env.SOAK_RUN_ID = runId
                                echo "Segment ${seg} 判定: status=${segmentStatus} run=${runId}"

                                if (segmentStatus == 'FAIL') {
                                    // FAIL/缺失/坏 JSON：结束本次 Build、发邮件、不续跑。
                                    currentBuild.result = 'FAILURE'
                                    error("Segment ${seg} 失败（status=FAIL 或报告缺失/JSON 无效），不续跑；" +
                                          "详见 ${SOAK_REPORT_ROOT()}/segment-${seg}.log")
                                } else if (segmentStatus == 'WARN') {
                                    // 资源/功能告警：标记 UNSTABLE 触发邮件；是否续跑由 SOAK_CONTINUE 控制。
                                    unstable("Segment ${seg} 资源/功能告警（WARN），详见 " +
                                             "${env.BUILD_URL}artifact/${SOAK_REPORT_ROOT()}/${runId}/summary.json")
                                } else {
                                    echo "Segment ${seg} 通过（PASS）"
                                }
                            }
                            // 本段已完整归档且通过（PASS 或 WARN；FAIL 已在段内 error 抛出）：
                            // 续跑仅当 SOAK_CONTINUE=true 且未达段数上限。
                            continueSoak = params.SOAK_CONTINUE && seg < MAX_SOAK_SEGMENTS()
                            seg++
                        }
                        echo "Soak 段循环结束：完成 ${seg - 1} 段，最后状态 ${env.SOAK_STATUS ?: '?'}"
                    }
                }
            }
        }
    }
    post {
        always {
            script {
                // 兜底归档：段内已逐段归档；此处覆盖构建早期失败的残留证据（空目录安全）。
                archiveArtifacts artifacts: 'tests/reports/soak/**', fingerprint: true, allowEmptyArchive: true
            }
        }
        unstable {
            script {
                def commit = env.GIT_COMMIT ?: 'unknown'
                def runId = env.SOAK_RUN_ID ?: ''
                def reportUrl = runId != ''
                    ? "${env.BUILD_URL}artifact/${SOAK_REPORT_ROOT()}/${runId}/summary.json"
                    : "${env.BUILD_URL}artifact/${SOAK_REPORT_ROOT()}/"
                // 正文只含 Job/Build/commit/段号/状态/报告 URL，不含任何凭据或敏感配置。
                emailext(
                    to: '$DEFAULT_RECIPIENTS',
                    subject: "UNSTABLE: ${env.JOB_NAME} #${env.BUILD_NUMBER} (soak segment ${env.SOAK_SEGMENT ?: '?'})",
                    body: """Soak pipeline unstable (WARN segment).

Job: ${env.JOB_NAME}
Build: ${env.BUILD_NUMBER}
Commit: ${commit}
Segment: ${env.SOAK_SEGMENT ?: 'unknown'}
Status: ${env.SOAK_STATUS ?: 'unknown'}（资源/功能告警）
Report: ${reportUrl}
Build log: ${env.BUILD_URL}console
""",
                    mimeType: 'text/plain'
                )
            }
        }
        failure {
            script {
                def commit = env.GIT_COMMIT ?: 'unknown'
                def runId = env.SOAK_RUN_ID ?: ''
                def reportUrl = runId != ''
                    ? "${env.BUILD_URL}artifact/${SOAK_REPORT_ROOT()}/${runId}/summary.json"
                    : "${env.BUILD_URL}artifact/${SOAK_REPORT_ROOT()}/"
                emailext(
                    to: '$DEFAULT_RECIPIENTS',
                    subject: "FAILED: ${env.JOB_NAME} #${env.BUILD_NUMBER} (soak segment ${env.SOAK_SEGMENT ?: '?'})",
                    body: """Soak pipeline failed.

Job: ${env.JOB_NAME}
Build: ${env.BUILD_NUMBER}
Commit: ${commit}
Segment: ${env.SOAK_SEGMENT ?: 'unknown'}
Status: ${env.SOAK_STATUS ?: 'unknown'}（FAIL / 报告缺失 / JSON 无效，不续跑）
Failed stage: ${env.FAILED_STAGE ?: 'unknown'}
Report: ${reportUrl}
Build log: ${env.BUILD_URL}console
""",
                    mimeType: 'text/plain'
                )
            }
        }
    }
}
