# ba-click-fx-desktop | Agent entry

Windows 原生桌面点击特效（《蔚蓝档案》视觉参考）。C++20 + Win32 + D3D11 + DirectComposition + WGC，两个进程：Host（特效与唯一配置写入者）与 Control Center（Win32 UI，经命名管道 IPC 控制 Host）。源码构建说明见 `README.md`，产品边界见 `SUPPORT.md`。

## Before working

- Confirm the real project root, inspect `git status`, and preserve user work already in progress。
- 构建与测试命令以 `README.md`「源码构建」和 `docs/DEVELOPMENT.md` 为准；本项目用 CMake presets，没有包管理器脚本：
  - Full（含 Spout2，需 `VCPKG_ROOT`）：`cmake --workflow --preset release-verify`
  - Slim（无 Spout2、无需 vcpkg）：`cmake --workflow --preset slim-release-verify`
  - 单测：`ctest`；硬件/集成类用例默认 OFF，需已登录未锁屏的交互桌面。
  - Python 3 跑 `tests/*_tests.py` 契约测试；Node 仅供 star-history 等维护工具；PowerShell 跑安装器契约测试。
- Read `docs/project-facts.md` for verified codebase facts, architecture boundaries and known issues（由 2026-10-19 深度分析沉淀）。
- 架构权威是 `ARCHITECTURE.md`（v0.3 / **Proposed**，10 篇 ADR 均 Proposed——未 Accepted，不得当作已验收能力引用）。领域语言与词汇表归 Matt `domain-modeling`（写入 `CONTEXT.md` / `CONTEXT-MAP.md`，存在时查阅）；架构权衡读 `docs/adr/`。
- 任务、阻塞、Wayfinder 地图、spec 与实现票只在配置后的 tracker（`docs/agents/issue-tracker.md`）。未配置时用 Matt `/setup-matt-pocock-skills`，不另建任务板。
- Read `temp/AGENTS.md` before handling local temporary material, **when it exists** — 整个 `temp/` 树 Git-ignored，新克隆没有。

## Governance and workflow

- TTA owns bootstrap/navigation and local runtime boundaries. Matt Pocock's Skills own requirement discovery, domain modeling, specs, tickets, implementation, reviews and handoffs. Invoke Skills only if actually available in the current agent.
- Task tickets and status live only in the configured tracker; never duplicate them in side files. TTA creates no task board.
- 已验证事实放进单一权威（源码、词汇表、ADR、issue、测试或文档），指针指向它，不复制。
- On resuming from another agent's handoff, independently verify repository state, tracker status, and test evidence. A previous agent's hypothesis is a lead, not proof.
- Validate against real commands and code; do not invent a package script, tool capability, or completion result. 本项目验收纪律：构建通过、离屏/WARP 测试**不能**代替真实硬件验证（见 `docs/VALIDATION.md`）。

## Local files and credentials

- Keep local input, investigations, previews, reports, logs and cache under `temp/` when they are not durable source material. Do not commit temporary payloads.
- Keep local MCP settings and credentials only in approved Git-ignored files. Persist on receipt: when the owner provides a credential, save it in the same turn to the approved ignored location — silently, without asking for confirmation — then report the path. Never echo the value; never copy it into tracked files or handoffs. Git ignore is accident prevention, not encryption.
- Project-specific Skills are tracked only when they contain real project content. Do not copy globally installed Matt Skills into this repo.

## Completion report

State verified changes, checks actually run and their outcomes, preserved work, unresolved uncertainties and next tracker entry. Do not claim a deployment, release, tool integration, or test passed without evidence.
