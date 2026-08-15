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
                        def selector = (params.PERF_BASELINE_BUILD != '')
                            ? [$class: 'SpecificBuildSelector', buildNumber: params.PERF_BASELINE_BUILD]
                            : [$class: 'LastSuccessfulBuildSelector']
                        try {
                            // 基线只从最近成功 main 构建（或显式构建号）Copy Artifact 获取；
                            // 不读 Git、不读工作区历史基线。获取失败按无基线处理，不阻塞。
                            step([$class: 'CopyArtifact',
                                  projectName: baselineProjectName(),
                                  selector: selector,
                                  filter: 'tests/baselines/jenkins/baseline.json',
                                  target: PERF_BASELINE_DIR(),
                                  flatten: true,
                                  fingerprintArtifacts: true,
                                  failIfNoArtifacts: false])
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
                        // 提取 environment.json 与 Markdown 报告（固定 python 代码，仅受控路径参数）。
                        sh """
                            PYTHONPATH=scripts/ci python3 -c '
                        import json, sys
                        from perf_contract import write_markdown_report
                        with open(sys.argv[1]) as f:
                            data = json.load(f)
                        with open(sys.argv[2], "w") as f:
                            json.dump(data["environment"], f, ensure_ascii=False, indent=2)
                            f.write("\\n")
                        write_markdown_report(sys.argv[3], data)
                        ' '${PERF_BASELINE_JSON()}' '${PERF_BASELINE_DIR()}/environment.json' '${PERF_BASELINE_DIR()}/baseline.md'
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
                // 正文只含 Job/Build/commit/mode/verdict/报告 URL，不含任何凭据或敏感配置。
                emailext(
                    to: '$DEFAULT_RECIPIENTS',
                    subject: "UNSTABLE: ${env.JOB_NAME} #${env.BUILD_NUMBER} (${env.PERF_VERDICT ?: 'unknown'})",
                    body: """Performance pipeline unstable.

Job: ${env.JOB_NAME}
Build: ${env.BUILD_NUMBER}
Commit: ${commit}
Mode: ${env.PIPELINE_MODE ?: 'unknown'}
Verdict: ${env.PERF_VERDICT ?: 'unknown'} (rc=${env.PERF_RC ?: '?'})
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
                def reportUrl = "${env.BUILD_URL}artifact/tests/reports/performance/result.md"
                emailext(
                    to: '$DEFAULT_RECIPIENTS',
                    subject: "FAILED: ${env.JOB_NAME} #${env.BUILD_NUMBER} (${env.FAILED_STAGE ?: 'unknown stage'})",
                    body: """Performance pipeline failed.

Job: ${env.JOB_NAME}
Build: ${env.BUILD_NUMBER}
Commit: ${commit}
Mode: ${env.PIPELINE_MODE ?: 'unknown'}
Verdict: ${env.PERF_VERDICT ?: 'unknown'} (rc=${env.PERF_RC ?: '?'})
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
