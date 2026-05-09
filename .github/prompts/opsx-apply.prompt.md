---
description: 从 OpenSpec 变更实现任务（实验性）
---

从 OpenSpec 变更中实现任务。

**输入**：可选地在 `/opsx:apply` 后指定变更名（例如 `/opsx:apply add-auth`）。如果省略，需要检查是否能从当前对话上下文推断。如果含糊或存在歧义，你必须提示用户从可用变更中选择。

**步骤**

1. **选择变更**

   如果提供了名称，就直接使用。否则：
   - 如果用户在对话中提到过某个变更，则尝试从上下文推断
   - 如果只有一个活跃变更，则自动选择
   - 如果存在歧义，运行 `openspec list --json` 获取可用变更，并使用 **AskUserQuestion tool** 让用户选择

   始终明确告知："Using change: <name>"，并说明如何覆盖它（例如 `/opsx:apply <other>`）。

2. **检查状态以理解当前 schema**
   ```bash
   openspec status --change "<name>" --json
   ```
   解析 JSON，理解：
   - `schemaName`：当前使用的工作流（例如 `spec-driven`）
   - 哪个 artifact 包含任务内容（对 `spec-driven` 通常是 `tasks`，其他 schema 以状态结果为准）

3. **获取 apply 指令**

   ```bash
   openspec instructions apply --change "<name>" --json
   ```

   该命令会返回：
   - 上下文文件路径（取决于 schema）
   - 进度信息（总数、已完成、剩余）
   - 带状态的任务列表
   - 基于当前状态生成的动态说明

   **状态处理：**
   - 如果 `state: "blocked"`（缺少 artifact）：展示说明，并建议使用 `/opsx:continue`
   - 如果 `state: "all_done"`：告知任务已完成，并建议归档
   - 否则：进入实现流程

4. **读取上下文文件**

   读取 apply 指令输出中的 `contextFiles` 列表。
   这些文件取决于当前使用的 schema：
   - **spec-driven**：proposal、specs、design、tasks
   - 其他 schema：以 CLI 输出中的 `contextFiles` 为准

5. **展示当前进度**

   展示：
   - 当前使用的 schema
   - 进度："N/M tasks complete"
   - 剩余任务概览
   - CLI 返回的动态说明

6. **实现任务（循环直到完成或被阻塞）**

   对每个待完成任务：
   - 明确展示当前正在处理哪个任务
   - 做出所需代码修改
   - 保持改动最小且聚焦
   - 在 tasks 文件中将任务标记为完成：`- [ ]` → `- [x]`
   - 继续处理下一个任务

   **以下情况需要暂停：**
   - 任务不清晰：请求澄清
   - 实现过程中暴露设计问题：建议更新 artifacts
   - 遇到错误或阻塞：报告情况并等待指示
   - 用户主动打断

7. **完成或暂停时展示状态**

   展示：
   - 本次会话已完成的任务
   - 总体进度："N/M tasks complete"
   - 如果全部完成：建议归档
   - 如果暂停：说明原因并等待指示

**实现过程中的输出**

```
## 正在实现：<change-name>（schema: <schema-name>）

正在处理任务 3/7：<task description>
[...implementation happening...]
✓ Task complete

正在处理任务 4/7：<task description>
[...implementation happening...]
✓ Task complete
```

**完成时的输出**

```
## 实现完成

**Change:** <change-name>
**Schema:** <schema-name>
**Progress:** 7/7 tasks complete ✓

### 本次会话完成项
- [x] Task 1
- [x] Task 2
...

所有任务均已完成。你可以使用 `/opsx:archive` 归档这个变更。
```

**暂停时的输出（遇到问题）**

```
## 实现已暂停

**Change:** <change-name>
**Schema:** <schema-name>
**Progress:** 4/7 tasks complete

### 遇到的问题
<description of the issue>

**可选项：**
1. <option 1>
2. <option 2>
3. 其他方案

你希望如何处理？
```

**护栏**
- 持续推进任务，直到全部完成或遇到阻塞
- 开始前始终先读取上下文文件（来自 apply 指令输出）
- 如果任务存在歧义，先暂停并提问，不要直接实现
- 如果实现暴露出问题，先暂停并建议更新 artifact
- 保持代码改动最小，并限定在当前任务范围内
- 每完成一个任务后，立即更新任务复选框
- 遇到错误、阻塞或需求不清时暂停，不要猜测
- 使用 CLI 输出中的 `contextFiles`，不要自行假设具体文件名

**流式工作流集成**

这个 skill 支持 “actions on a change” 模型：

- **可在任意时机调用**：在所有 artifact 完成前（如果任务已存在）、部分实现之后、或与其他动作交错进行时都可以调用
- **允许更新 artifact**：如果实现暴露设计问题，应建议更新 artifact，而不是被固定阶段锁死，应保持流动式工作
