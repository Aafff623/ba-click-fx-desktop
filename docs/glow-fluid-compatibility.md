# GlowFluid 兼容性回归矩阵

本矩阵记录 GlowFluid 样式在 Windows 兼容性维度上的验证状态。**凡未取得实测证据的项目一律标注「未验证」，不用静态审查冒充实机验收。** 本机（开发机）无交互桌面会话，桌面/集成类用例无法运行（与 BA 特效相同的环境限制，见 `docs/VALIDATION.md`）。

## 矩阵

| # | 维度 | 机制（代码路径） | 状态 | 证据 / 说明 |
| --- | --- | --- | --- | --- |
| 1 | 100% DPI（96dpi） | 视口像素即交换链尺寸；`setViewportSize` 驱动管线重分配 | 已验证（逻辑） | `glow_pipeline_allocates_reference_quality_dimensions`：256x256 下 sim 96²、dye 256²；WARP 实测 |
| 2 | 150% DPI | 同上；无 DPI 专属分支，缩放只改变视口像素数 | 已验证（逻辑） | 同 #1；高分辨率视口 1920x1080 下 sim 284x160、dye 1365x768，WARP 实测 |
| 3 | 200% DPI | 同上；染料分辨率受 `min(1, 视口/染料)` shrink 约束，永不超过交换链 | 已验证（逻辑） | 同 #1、#2；小视口 320x240 下染料 shrink 到 320x240，WARP 实测 |
| 4 | 双屏（含负坐标） | 每 DisplaySession 独立渲染器/管线；ScreenToClient 把物理屏幕坐标转到各 overlay 窗本地坐标 | 未验证 | 无第二显示器与交互桌面；跨屏流体不传递是已知限制（见下） |
| 5 | 分辨率/刷新率切换 | 交换链 resize → `setViewportSize` → 管线按键重分配（quality+尺寸） | 已验证（逻辑） | `glow_pipeline_quality_switching_reallocates_without_crash` 含 256→320→256 resize；Debug Layer 零冲突。真实热插拔未验证 |
| 6 | 睡眠/唤醒 | Host 既有设备恢复流程；管线绘制失败置 `renderFailed_` 停绘，样式降级 | 未验证 | 无睡眠/唤醒实测；失败路径有代码守卫（Map 失败即停） |
| 7 | 深色背景 | display 输出预乘 alpha，DComp 与桌面合成 | 未验证 | 参考 screen 混合在深色下接近；无实测 |
| 8 | 白色/高亮背景 | 同上；预乘叠加在白底比 Web 的 screen 混合更亮 | 未验证 | 偏差清单第 1 条；这是已知观感差异，需实机确认 |
| 9 | 高回报率鼠标（1000Hz） | Glow 直接消费同一 Raw Input 队列；每帧一个 emitter 段 splat，dt 钳制 [1/240, 1/30] | 未验证 | 采样合并是既有机制；未实测高频输入下的注入频率与手感 |
| 10 | Pause/Resume | 暂停/禁用时丢弃本帧输入（与 BA 路由同一守卫），不累积、不重放 | 已验证（逻辑） | main.cpp 守卫；恢复后无假迹理由见 #11 |
| 11 | 暂停期间无假轨迹 | 唤醒时 emitter 吸附到新样本位置并清零速度 | 已验证（逻辑） | `glow_renderer_settles_then_sleeps_without_phantom_trail`：idle 后唤醒只点亮新路径，旧位置全透明 |
| 12 | 设备移除恢复 | 管线随 CompositionRenderer 生命周期重建；创建失败降级占位/停用 | 未验证 | 无 GPU 设备移除实测；Debug Layer 下重分配路径零 ERROR/CORRUPTION |
| 13 | 质量热切换（低/中/高） | 补丁实时生效，下一帧重分配 | 已验证（逻辑） | 3 轮切换无崩溃；Debug Layer info queue 零错误（含 resize/quality 重分配） |
| 14 | 长时运行（小时级） | 空闲 1.26s 后停帧；无每帧 CPU 读回 | 未验证 | 无长时实测；机制见运维文档第 3 节 |
| 15 | BA 回归（样式共存） | BA 路径零改动；Glow 只在 `effectStyle=glowFluid` 时接管渲染 | 已验证（自动化） | 全量非桌面套件 36/37（唯一失败为改动前即超时的环境项）；FX 预设/profile 测试不受影响 |

## 承诺边界

- **第一版只承诺单主屏 SDR。** 多屏（含负坐标）、HDR、跨屏流体连续性不在承诺内。
- Glow 不依赖 WGC 背景捕获；BA 的 WGC 相关能力保持原样。
- 深浅背景下的观感差异见 `glow-fluid-deviations.md` 第 1 条。

## 已知限制汇总

1. 与 Web 参考的混合语义差异（screen blend vs 预乘 alpha 合成）——白底更亮，未实测。
2. 边缘渐隐为物理像素（2px），非参考的 2% 容器宽。
3. 高 DPI 下染料分辨率上限为 768 基准（quality=high），低于 Web 的 768×DPR。
4. 压力求解 20 次 Jacobi 对低频发散收敛慢（与参考一致），快速大范围划动可能有轻微体积伪影。
5. 跨屏流体不连续；每屏独立模拟场。
6. light 主题分支不可达（配置无 theme 字段）。
7. 点击爆发的随机源为确定性 xorshift32（非 Math.random），视觉等效、测试可复现。

## 回滚方案

见 `glow-fluid-operations.md` 第 4 节：切回 BA（日常）→ 管线失败降级占位 → 配置回滚（需先切 BA，schema 21 严格版本校验）。

## 复现方式

```powershell
# 全量构建（Slim，无需 vcpkg）
cmake --workflow --preset slim-release-verify

# 仅 Glow 相关
ctest -C Release -R "glow_fluid" --output-on-failure   # 17 例
ctest -C Release -R "^config$" --output-on-failure     # 36 例（含 Glow 配置）
```

本机无交互桌面时，桌面/集成类用例（`desktop_*` 标签）按既有策略跳过；`active_fx_roi_ab_report_contract` 的超时为改动前即存在的环境项。
