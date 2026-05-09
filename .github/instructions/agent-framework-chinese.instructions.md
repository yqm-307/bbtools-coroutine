---
description: "Use when creating or editing Copilot framework files such as prompts, custom agents, skills, and instructions. Enforce Simplified Chinese for user-facing prose while preserving technical identifiers."
name: "Agent Framework Chinese"
applyTo:
  - ".github/prompts/**/*.prompt.md"
  - ".github/agents/**/*.agent.md"
  - ".github/skills/**/SKILL.md"
  - ".github/instructions/**/*.instructions.md"
  - ".github/copilot-instructions.md"
  - ".github/AGENTS.md"
---

# Agent Framework Chinese

- 这些框架文件中的正文说明、步骤、示例说明、输出模板与用户可见文案默认使用简体中文。
- 当前端 matter 的字段名、工具名、命令、路径、协议字段、API 名称、库名、文件名或其他技术标识需要保持原文时，不要强行翻译。
- 当自然语言与代码、命令或配置同时出现时，自然语言部分使用简体中文，代码与技术标识保持原样。
- 如果用户明确要求英文或其他语言，以用户要求为准。
- 修改此类文件时，优先保持结构、frontmatter 和工作流语义不变，只对语言表达做必要调整。