---
description: 提出一个新的变更，并一步生成所有 artifacts
---

提出一个新的变更，一步创建 change 并生成所有 artifacts。

我会创建一个 change，并生成这些 artifacts：
- proposal.md（做什么，以及为什么做）
- design.md（如何做）
- tasks.md（实现步骤）

准备进入实现时，运行 /opsx:apply

---

**输入**：`/opsx:propose` 后的参数可以是 change 名（kebab-case），也可以是用户想构建内容的描述。

**步骤**

1. **如果没有提供输入，先询问用户想做什么**

   使用 **AskUserQuestion tool**（开放式问题，不带预设选项）提问：
   > "What change do you want to work on? Describe what you want to build or fix."

   根据用户的描述生成一个 kebab-case 名称（例如 "add user authentication" → `add-user-auth`）。

   **重要**：在真正理解用户想构建什么之前，不要继续。

2. **创建 change 目录**
   ```bash
   openspec new change "<name>"
   ```
   这会在 `openspec/changes/<name>/` 下创建一个带 `.openspec.yaml` 的脚手架目录。

3. **获取 artifact 构建顺序**
   ```bash
   openspec status --change "<name>" --json
   ```
   解析 JSON，获取：
   - `applyRequires`：进入实现前必须完成的 artifact ID 数组（例如 `["tasks"]`）
   - `artifacts`：所有 artifact 的状态与依赖列表

4. **按顺序创建 artifacts，直到达到 apply-ready**

   使用 **TodoWrite tool** 跟踪 artifact 创建进度。

   按依赖顺序循环处理 artifacts（优先处理没有待满足依赖的项）：

   a. **对于每个 `ready`（依赖已满足）的 artifact：**
      - 获取指令：
        ```bash
        openspec instructions <artifact-id> --change "<name>" --json
        ```
      - 指令 JSON 包含：
        - `context`：项目背景（这是给你的约束，不要写进输出）
        - `rules`：artifact 专属规则（这是给你的约束，不要写进输出）
        - `template`：输出文件应使用的结构
        - `instruction`：该 artifact 类型对应的 schema 指导
        - `outputPath`：artifact 的写入位置
        - `dependencies`：已经完成、需要读取的依赖 artifact
      - 读取已完成的依赖文件作为上下文
      - 按 `template` 的结构创建 artifact 文件
      - 将 `context` 和 `rules` 当作约束使用，但不要复制到文件中
      - 简要展示进度："Created <artifact-id>"

   b. **持续推进，直到所有 `applyRequires` artifacts 都已完成**
      - 每创建一个 artifact 后，重新运行 `openspec status --change "<name>" --json`
      - 检查 `applyRequires` 中的每个 artifact ID 是否都在 artifacts 数组中变为 `status: "done"`
      - 当所有 `applyRequires` artifacts 都完成时停止

   c. **如果某个 artifact 需要用户输入**（上下文不清晰）：
      - 使用 **AskUserQuestion tool** 进行澄清
      - 然后继续创建流程

5. **展示最终状态**
   ```bash
   openspec status --change "<name>"
   ```

**输出**

完成所有 artifacts 后，汇总说明：
- Change 名称与位置
- 已创建的 artifacts 列表及简要说明
- 当前状态："All artifacts created! Ready for implementation."
- 提示："Run `/opsx:apply` to start implementing."

**Artifact 创建指南**

- 遵循 `openspec instructions` 中每种 artifact 对应的 `instruction` 字段
- schema 已定义每种 artifact 应包含什么内容，应按其要求生成
- 创建新 artifact 前，先读取依赖 artifact 作为上下文
- 使用 `template` 作为输出文件结构，并将各节补充完整
- **重要**：`context` 和 `rules` 是给你的约束，不是写进文件的内容
  - 不要把 `<context>`、`<rules>`、`<project_context>` 这类块复制进 artifact
  - 它们只用于指导你写什么，不应出现在最终输出中

**护栏**
- 创建进入实现所需的全部 artifacts（由 schema 的 `apply.requires` 决定）
- 创建新 artifact 前，始终先读取依赖 artifact
- 如果上下文严重不清晰，就提问；但在合理范围内优先做出判断，保持推进
- 如果同名 change 已存在，询问用户是继续该 change 还是创建新变更
- 每写完一个 artifact 文件后，都先确认它确实存在，再继续下一步
