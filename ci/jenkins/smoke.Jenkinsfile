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
                def reportUrl = "${env.BUILD_URL}artifact/tests/reports/smoke/${env.SANITIZED_BUILD_TAG}/summary.md"
                // 正文只含 Job/Build/commit/失败阶段/报告 URL，不含任何凭据或敏感配置。
                emailext(
                    to: '$DEFAULT_RECIPIENTS',
                    subject: "FAILED: ${env.JOB_NAME} #${env.BUILD_NUMBER} (${env.FAILED_STAGE ?: 'unknown stage'})",
                    body: """Smoke pipeline failed.

Job: ${env.JOB_NAME}
Build: ${env.BUILD_NUMBER}
Commit: ${commit}
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
