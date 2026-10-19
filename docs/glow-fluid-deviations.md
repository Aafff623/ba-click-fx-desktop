# GlowFluid 与 Web 参考实现的偏差清单

对照 `temp/glow-handoff/BAFX-GlowCursor-Handoff/source/GlowCursor.jsx`（React Bits / MOTDES GlowCursor，WebGL2）与本机 D3D11 移植实现的已知偏差。凡未在此列出的行为，均按参考实现逐条移植。

## 1. 混合语义（screen blend）

- **Web**：canvas 以 `mix-blend-mode: screen` 叠在网页绘制上下文上，直接读取下层页面像素做屏幕混合。
- **本机**：桌面透明覆盖层无法读取下层窗口内容。display pass 输出**预乘 alpha** 光效帧（`rgb` 已按 `1-exp(-(light+bloom)*grain)` 计算，`alpha = max(r,g,b)`），由 DirectComposition 与桌面合成。
- **后果**：深色背景下视觉接近 screen 混合；**白色/高亮背景下本机更亮**（screen 混合在白底上趋近无变化，预乘叠加不会）。深浅背景差异需实机确认，当前**未验证**。

## 2. 边缘渐隐（物理像素）

- **Web**：`smoothstep(0.0, 0.02, min(...))`，即容器每边 2% 的 uv 宽度。
- **本机**：改为**物理像素**宽度（默认 2px，按视口高/宽分别换算 uv）。全屏覆盖层上 2% 相当于 20-40px，会把贴边的光迹齐切掉。
- **后果**：贴屏幕边缘的尾迹不再被提前淡出；2px 仅用于消除边缘锯齿。如需完全关闭可把该值设 0。

## 3. 设备像素比（DPR）

- **Web**：`maxDevicePixelRatio`（默认 2）决定 canvas 后备缓冲分辨率；quality 档位（sim/dye 基准尺寸）与 DPR 共同决定实际纹理尺寸。
- **本机**：无 DPR 概念；Win32 视口像素即后备缓冲。quality 三档（low 96/384、medium 128/576、high 160/768）乘宽高比后，再按 `min(1, 视口/染料)` shrink，染料永不超过交换链分辨率。
- **后果**：高 DPI 屏上本机染料分辨率可能低于 Web 同设置的观感细腻度（quality=high 时 768 基准 vs Web 的 768×DPR）。属预期取舍，未做逐屏校准。

## 4. 休眠窗口与静止条件

- **Web**：`quiet = now - active > linger*1200+900ms` 且 `resting = |emitter.v| < 1`，两条件同时满足才清场、提交透明帧并停 rAF。
- **本机**：同一公式（linger 0.3 → 1260ms），`resting` 由 emitter 速度判定；emitter 未静止时样本不过期，渲染循环保持活跃。
- **偏差**：`followSpeed>=1` 时 Web 的 emitter 速度是弹簧积分残留值（指针停后可能长时间非零，延迟 idle）；本机在吸附路径下用帧间位移作为速度，指针停立即静止。这是**刻意修正**，避免参考实现里 pointer 离开后残留速度导致的延迟休眠。

## 5. 主题（theme）分支

- **Web**：`theme='light'` 走 DISPLAY 的 ink/density 分支（深色墨迹+密度合成），适配浅色页面。
- **本机**：配置无 theme 字段，display 恒走 dark 分支（`uLight=0`）。light 分支保留在 shader 中但不可达。
- **后果**：无。浅色桌面下的表现见第 1 条。

## 6. 指针离开与 emitter 复位

- **Web**：pointer 离开容器设 `ready=false`，保留 emitter 速度；重新进入时吸附到新位置。
- **本机**：空闲（quiet+resting）后 `pointerInside=false`，下一个样本把 emitter 吸附到该样本位置并清零速度，不会从上一会话位置拉出假轨迹。

## 7. 爆发（clickBurst）随机源

- **Web**：`Math.random()`。
- **本机**：确定性 xorshift32（每渲染器固定种子）。视觉上仍是随机分布；测试可复现。

## 8. 跨屏与多屏

- **Web**：单容器，无跨屏概念。
- **本机**：每屏独立模拟场（每 DisplaySession 一个渲染器/管线）。流体**不跨屏传递**：旧屏自然淡出，新屏从新位置起迹。第一版仅承诺单主屏 SDR（见 handoff P4 计划）。

## 9. 背景捕获（WGC）

- **Web**：不涉及。
- **本机**：Glow 不依赖 WGC 背景读取；BA 的 WGC 相关路径保持原样可用。深浅背景合成只走 DComp 透明通道（见第 1 条）。

## 10. 时间源与帧率

- **Web**：`requestAnimationFrame`，dt 钳制 1/30s。
- **本机**：Host 渲染循环单调时钟，dt 钳制 [1/240, 1/30]s；弹簧积分按参考实现子步进（≤4ms/步）。

## 11. 压力求解迭代次数

- **Web 与本机一致**：20 次 Jacobi。对**低频** divergence 收敛慢（约每步 ×0.72），这是参考实现的固有特性，非移植偏差；WARP 测试按「收敛趋势」验收。

## 未验证项（实机）

- 上述第 1、3 条的深浅背景/高 DPI 观感；贴手程度与真实回报率鼠标下的注入频率；长时运行的 GPU 计时与能耗。本机无交互桌面，均未验证。
