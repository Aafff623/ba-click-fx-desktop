# GlowCursor 二开：环境与基线记录（P0）

- 记录日期：2026-10-19
- 分支：`feature/glow-fluid-style`（基线 commit `5b990e5`，即上游 v0.3.1）
- 关联：spec issue #1，本文件对应 ticket #2（P0）

## 1. 仓库与远端状态

| 项 | 值 |
|---|---|
| 本地路径 | `D:\code\fork\ba-click-fx-desktop` |
| 基线 HEAD | `5b990e54740d198e99a8f246ea6f7c26ed9fc9ff`（与 handoff 研究基线 SHA 一致） |
| 工作树 | 干净（治理骨架文件未提交，见第 5 节） |
| `origin` | `https://github.com/CialloKing/ba-click-fx-desktop.git`（上游） |
| `github` | `https://github.com/Aafff623/ba-click-fx-desktop.git`（2026-10-19 新建公开仓库，空，未推送） |

## 2. 交接包完整性

`temp/glow-handoff/BAFX-GlowCursor-Handoff/`（本地 Git-ignored）：21 个文件全部通过 `sha256sum -c SHA256SUMS.txt`。已读全部 7 篇文档、2 套预设 JSON、`source/GlowCursor.jsx`（943 行）与 CSS。

## 3. handoff 远端研究结论的本地核对（全部通过）

| handoff 声明 | 本地核对结果（HEAD = 5b990e5） |
|---|---|
| `currentSchemaVersion=20` | 成立（`src/config/include/bafx/config/config.hpp:15`） |
| `InputConfig::trailOnlyWhilePressed=true` | 成立（config.hpp:151） |
| `EffectsConfig::trailWidth` 默认 `1.0F`（倍率） | 成立（config.hpp:85） |
| `EffectsMode{Full,Core}` | 成立（config.hpp:47-51） |
| `FrameSnapshot::hasDrawableContent()` | 成立（`src/fx/include/bafx/fx/simulation.hpp:98`） |
| `CompositionRenderer` 持有 `FxGpuRenderer` | 成立（composition_renderer.hpp:471） |
| `embedded_fx_shaders.hpp` 存在 | 成立 |
| `FxProfileStore` 只存 effects | 成立（见深度分析 `docs/project-facts.md`） |

## 4. 构建环境与构建结果

| 工具 | 版本 |
|---|---|
| OS | Windows（win32 10.0.26300） |
| Visual Studio 2022 Build Tools | 17.14.41（x64，`vcvars64.bat` 可用） |
| CMake | 4.3.3 |
| Python | 3.11.9 |
| Node | 24.21.0 |

构建入口：`cmake --workflow --preset slim-release-verify`（Slim，无 Spout2，无需 vcpkg）。
Full 需 `VCPKG_ROOT` 指向独立 vcpkg checkout——本机未准备，**未运行**。

踩坑记录（本机特有，非项目缺陷）：
1. Git Bash 默认 shell 无 MSVC 环境，直接跑 preset 报 `CMAKE_CXX_COMPILER not set`；
2. preset 用 `architecture: set`（VS 生成器语义），环境缓存过 Ninja 生成器后报
   "Ninja does not support platform specification"——清掉 `build/x64-slim` 并用
   `CMAKE_GENERATOR=Visual Studio 17 2022` 可解；
3. 因此本机构建必须经 `temp/build-slim.bat`（先 `vcvars64.bat` 再跑 workflow）。

构建结果（2026-10-19，本会话，`slim-release-verify` 全流程）：

- **构建本身成功**：configure + compile + link 全部通过（Release，Slim）。
- **测试**：CTest 共 47+ 项，9 项失败，全部为需要交互式 Windows 桌面的
  desktop/integration 类：`desktop_smoke`、`desktop_timed_exit`、
  `desktop_device_recovery_probe`、`desktop_frame_pacing_stall`（均 SEGFAULT）、
  `desktop_input_dispatch`（SEGFAULT）、`host_control_startup`（SEGFAULT）、
  `control_center_activation`（退出码 0xc0000409）、`hotkey_process_boundary`
  （Failed，与上游 v0.3.1 已知本地失败项一致）、`active_fx_roi_ab_report_contract`
  （Timeout）。本会话无交互桌面，这些失败**不能**归因于 Glow 改动——它们正是 P0
  基线要区分的"环境/上游既有"类别。
- 非桌面契约/单元测试在该轮全部通过（该轮构建横跨 T2 编辑进行中，T2 结论以随后
  的定向重跑为准）。

## 5. 本会话已产生但未提交的改动

- 治理骨架（上一轮 tta-init / setup-matt-pocock-skills 产物）：`AGENTS.md`、`CLAUDE.md`、
  `docs/project-facts.md`、`docs/agents/`、`docs/analysis/2026-10-19-project-deep-analysis.md`、
  `.gitignore` 追加 `/temp/` 与 `.agents/mcp.json`。这些不属于 Glow 功能，需单独 commit。
- Glow T2 配置接缝（进行中）：`src/config/`、`tests/glow_style_config_tests.cpp`、`tests/CMakeLists.txt`。

## 6. 原版 BA 基线状态

**未验证。** 本会话未运行 Host/Control Center，未做任何交互桌面测试（自由拖尾、点击、穿透、
快捷键、预设）。区分"上游已有问题"与"Glow 引入问题"需要用户在本机执行：

```powershell
# 构建（Developer 环境或 temp/build-slim.bat）
cmake --workflow --preset slim-release-verify
# 运行 Host 与 Control Center，按 README「快速开始」验证原有效果
```

已知上游限制（来自 v0.3.1 CHANGELOG 与本机 CI 记录）：本地某快捷键进程边界测试曾超时
（远端 CI 通过，根因未确认）——复跑测试时若命中，先记录再归因。

## 7. 已知限制（本会话）

- Full/Spout2 构建未运行（无 vcpkg checkout）。
- 交互桌面 smoke 测试（`BAFX_ENABLE_DESKTOP_SMOKE_TESTS`）未运行——需要已登录、未锁屏的桌面。
- 未做任何实机视觉验证；所有视觉相关结论标注"未验证"。
