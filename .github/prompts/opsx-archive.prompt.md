---
description: 在实验性工作流中归档已完成的变更
---

在实验性工作流中归档一个已完成的变更。

**输入**：可选地在 `/opsx:archive` 后指定变更名（例如 `/opsx:archive add-auth`）。如果省略，需要检查是否能从当前对话上下文推断。如果含糊或存在歧义，你必须提示用户从可用变更中选择。

**步骤**

1. **如果没有提供变更名，提示用户选择**

   运行 `openspec list --json` 获取可用变更。使用 **AskUserQuestion tool** 让用户选择。

   只展示活跃变更（不包含已归档项）。
   如果可用，同时展示每个变更使用的 schema。

   **重要**：不要猜测或自动选择变更。始终让用户自己选择。

2. **检查 artifact 完成状态**

   运行 `openspec status --change "<name>" --json` 检查 artifact 完成情况。

   解析 JSON，理解：
   - `schemaName`：当前使用的工作流
   - `artifacts`：artifact 列表及其状态（`done` 或其他）

   **如果存在未完成的 artifact：**
   - 展示警告，并列出未完成项
   - 请求用户确认是否继续
   - 用户确认后再继续

3. **检查任务完成状态**

   读取 tasks 文件（通常为 `tasks.md`），检查是否存在未完成任务。

   统计 `- [ ]`（未完成）与 `- [x]`（已完成）的数量。

   **如果发现未完成任务：**
   - 展示警告并说明数量
   - 请求用户确认是否继续
   - 用户确认后再继续

   **如果不存在 tasks 文件：** 直接继续，不展示任务相关警告。

4. **评估 delta spec 的同步状态**

   检查 `openspec/changes/<name>/specs/` 下是否存在 delta specs。如果不存在，则直接继续，无需同步提示。

   **如果存在 delta specs：**
   - 将每个 delta spec 与其对应的主 spec `openspec/specs/<capability>/spec.md` 比较
   - 判断将要应用的变更类型（新增、修改、删除、重命名）
   - 在提示用户前先展示一份合并摘要

   **提示选项：**
   - 如果需要同步："现在同步（推荐）"、"不同步直接归档"
   - 如果已经同步："立即归档"、"仍然同步一次"、"取消"

   如果用户选择同步，使用 Task tool（subagent_type: "general-purpose"，prompt: "Use Skill tool to invoke openspec-sync-specs for change '<name>'. Delta spec analysis: <include the analyzed delta spec summary>"）。无论用户是否同步，最终都继续归档流程。

5. **执行归档**

   如果归档目录不存在，则先创建：
   ```bash
   mkdir -p openspec/changes/archive
   ```

   使用当前日期生成目标名：`YYYY-MM-DD-<change-name>`

   **检查目标是否已存在：**
   - 如果存在：报错，并建议重命名已有归档或改用其他日期
   - 如果不存在：将变更目录移动到归档目录

   ```bash
   mv openspec/changes/<name> openspec/changes/archive/YYYY-MM-DD-<name>
   ```

6. **展示摘要**

   展示归档完成摘要，包括：
   - 变更名
   - 使用的 schema
   - 归档位置
   - Spec 同步状态（已同步 / 跳过同步 / 无 delta specs）
   - 任何警告的说明（artifact 或任务未完成）

**成功时的输出**

```
## 归档完成

**Change:** <change-name>
**Schema:** <schema-name>
**Archived to:** openspec/changes/archive/YYYY-MM-DD-<name>/
**Specs:** ✓ Synced to main specs

所有 artifact 已完成。所有任务已完成。
```

**成功时的输出（无 Delta Specs）**

```
## 归档完成

**Change:** <change-name>
**Schema:** <schema-name>
**Archived to:** openspec/changes/archive/YYYY-MM-DD-<name>/
**Specs:** No delta specs

所有 artifact 已完成。所有任务已完成。
```

**成功时的输出（带警告）**

```
## 归档完成（带警告）

**Change:** <change-name>
**Schema:** <schema-name>
**Archived to:** openspec/changes/archive/YYYY-MM-DD-<name>/
**Specs:** Sync skipped (user chose to skip)

**Warnings:**
- Archived with 2 incomplete artifacts
- Archived with 3 incomplete tasks
- Delta spec sync was skipped (user chose to skip)

如果这不是你的预期，请检查归档结果。
```

**出错时的输出（归档已存在）**

```
## 归档失败

**Change:** <change-name>
**Target:** openspec/changes/archive/YYYY-MM-DD-<name>/

目标归档目录已存在。

**可选项：**
1. 重命名现有归档
2. 如果是重复项，删除现有归档
3. 等到其他日期再归档
```

**护栏**
- 如果未提供变更名，必须始终提示用户选择
- 使用 artifact graph（`openspec status --json`）检查完成情况
- 不要因为警告而阻止归档，只需说明并请求确认
- 移动到 archive 时保留 `.openspec.yaml`（它会随目录一并移动）
- 清晰展示发生了什么
- 如果请求同步，使用 Skill tool 调用 `openspec-sync-specs`（agent-driven）
- 如果存在 delta specs，必须先做同步评估并展示合并摘要，然后再提示用户
