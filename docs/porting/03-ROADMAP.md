# 路线图

目标：把游戏真正跑起来（先浏览器/wasm32，再考虑原生移动端）。
每一项给出**验收标准**，避免"看起来做完了"。工作量是量级估计，不是承诺。

## M1 ✅ 可行性 spike（已完成）

- [x] 用 Emscripten 把 `dqiv_game`（553 TU）+ 核心 + NNS + 声音 + 平台层编成 wasm32
- [x] 验证 32 位布局断言（`sizeof(void*)==4`、`DataObject==0x10`、`UnkGameApplication==0xb4`、`OSThread==0xc0`）
- [x] 链接成单个 `.wasm` 并运行真实反编译函数
- [x] 产出 `wasm/` 脚手架与本文档

**验收**：`./wasm/build.sh` 从零构建 0 报错；`node wasm/build/dqiv_wasm_spike.js` 打印出 `arrayToIndex(...) = 3`。

## M2 ✅ 收敛构建，消除源文件清单重复（已完成）

`cmake/Sources.cmake` 现在是各目标源清单的**唯一登记处**，根 `CMakeLists.txt` 与
`wasm/CMakeLists.txt` 都引用它。

- [x] 把各目标的源文件清单抽成共享的 cmake 片段（`cmake/Sources.cmake` 里的函数），
      根 `CMakeLists.txt` 与 `wasm/CMakeLists.txt` 同时引用
- [x] 保持上游 Windows 构建产物不变。本机没有 Windows，所以用"两个平台各配置一次、
      逐条对比 HEAD 的清单"来验：**14/14 一致**。真正的 Windows 编译放进 CI 的
      CI 的 `windows` job（runner pin 在 `windows-2022`），这条验收才有机器能守。
- [x] `native/DragonQuestIV.rc`、overlay 生成等 MSVC 专属部分必须能被非 MSVC 工具链安全跳过：
      `/WHOLEARCHIVE`、`/SECTION:.rdata,RW`、`/ENTRY`、`/DEBUG:FULL`、`/INCREMENTAL:NO` 已包进
      `if(MSVC)`。`LANGUAGES RC` 和 `WIN32` 保留原样——根项目在 Apple Silicon 上连配置都完不成
      （第 3 行的 32 位检查就是 fatal），所以这两处不构成实际障碍。

四处真实差异是这样参数化的（这是 M2 的全部难点，其余是搬运）：

| 位置 | 处理 |
| --- | --- |
| `native/SaveStore.cpp`（`dqiv_core`） | `if(WIN32)` 就地插入，位置不变，链接顺序不变 |
| `native/NitroThreads.cpp` / `native/NitroIdentity.cpp`（`dqiv_platform`） | 同上，非 Windows 走 `dqiv_portable_sources()` 给的桩 |
| `native/DssRuntime.cpp` | 独立函数 `dqiv_portable_platform_sources()`：上游把它放在 exe 目标里，wasm 把它编进 `dqiv_platform`，两边引用同一个函数 |
| SDL | 目标名当参数（根用 `SDL3::SDL3`，wasm 用 `-sUSE_SDL=3`），不进共享文件 |

顺带把 `dqiv_sound` 的链接目标统一成上游的 `dqiv_platform`（wasm 原先指向 `dqiv_core` 是历史遗留；
桩已提供所需符号，`gameOverlayImages()` 只在 `GameBoot.cpp` 里被引用，所以不会引入新的未定义符号）。

**验收**：新增一个上游文件后，只需要在一个地方登记；Windows 构建仍然 0 error。
→ 两条都已成立，而且都有机器守过。CI 的 `windows` job（runner pin `windows-2022`：`windows-latest`
和 `windows-2025` 现在都指向只装 VS 2026 的镜像，而 `windows-game` preset 硬编码 VS 2022 的
generator）实测 **0 error**，6 个产物全部链接成功，包括 `DragonQuestIV.exe`；`wasm` job 从干净
checkout 跑 `build.sh`，spike 输出 `= 3`、四条布局断言全过。764 条 MSVC warning 是反编译源码
原有的噪音（C4838 为主，集中在 `MenuTemplate_*.cpp`），不是 M2 引入的。

## M3 DS overlay 机制的可移植实现（3–5 天，需要设计）

目前缺失：overlay 模块与其可写数据段的边界符号在 Clang/wasm 上没有生成，导致运行时无法按 DS 语义
快照/恢复每个 overlay 的数据段。做法见 `02-FINDINGS.md` 第 7 节。

- [ ] 用 `__attribute__((section(...)))` + 显式边界符号替代 `#pragma data_seg/bss_seg` + `.d$NNa`/`.d$NNz` 段排序
      （`wasm-ld` 没有 ELF 的 `__start_/__stop_`，边界符号必须由生成器显式写出）
- [ ] 用 `__attribute__((constructor(priority)))` + 自己实现 `__cxa_atexit` 接管析构注册，
      替代 `#pragma init_seg(".i$NNm", pcOverlayExitNN)`
- [ ] 让 `dqiv_overlay_registry()` 的生成内容与 MSVC 路径并存（同一份 CMake 函数，按编译器分支）
- [ ] 单元测试：连续 load/unload 两个 overlay，确认可写数据被正确恢复、析构被正确调用

**验收**：在 wasm 上 `FS_LoadOverlay`/`FS_UnloadOverlay` 往返 N 次后，全局数据与首次加载一致。

## M4 WebGL2 / GLES 3.0 渲染后端 + Emscripten 启动壳（1–2 周，最大的一块）

- [ ] `GlLoader`：按目标裁剪入口点表（WebGL2 没有 `glMapBuffer`、计时查询），去掉 `gl::load()` 的"缺一即抛"
- [ ] 着色器：8 处 `#version 330 core` → `#version 300 es` + 精度限定符；
      `gl_ClipDistance` 改为 varying + 片元 `discard`
- [ ] `GameWindow`：改为请求 GLES 3.0；Emscripten 下用 `emscripten_set_main_loop` 替代 Win32 消息循环
- [ ] `FramePresenter` / `Display`：适配 canvas 尺寸、`devicePixelRatio`、resize 事件
- [ ] 性能剖析路径（GPU 计时/回读）用 `#ifdef` 隔离，必要时接 `EXT_disjoint_timer_query_webgl2`
- [ ] 用 `wasm/build.sh` 产出 `index.html` + `.wasm` + `.data`，浏览器里能看到**标题画面**

**验收**：浏览器里稳定 60 fps 显示标题/开场画面；`Settings > Display` 的分辨率与拉伸选项可用。

## M4b [可选] WebGPU 后端（不排期，等 M4 验收过了再量）

2026-10 的决定：**目标定为"只要浏览器"，所以 M4 走 WebGL2，WebGPU 不排期。**
理由不是 WebGPU 不好，而是它在这个目标下**不增加触达，只增加工作量和体积**。

实测的规模（`native/` 里 **281 个 `gl::` 调用点**、约 70 个入口点、8 个着色器）：

| | WebGL2（M4） | WebGPU（M4b） |
| --- | --- | --- |
| 形态 | **移植**：只有 3 个缺口（见第 6 节） | **重写**：约 180 个状态机调用要变成 bind group / pipeline 描述符 |
| 着色器 | `#version 300 es` + 精度限定符 | 8 个全部改写成 WGSL |
| 覆盖率 | 基本全员 | 87.8%，且 **Firefox 全线 disabled-by-default** |
| 体积 | — | `--use-port=emdawnwebgpu`（Dawn 的 `webgpu.h` 实现）给 7 MB 的 `.wasm` 再加几 MB |

WebGPU 唯一的真甜头是原生跨平台——但那条路的门票是第 3 节的 263 个指针字段（M9），
**不是 GPU API**。所以"上 WebGPU 就天然跨平台"是个错觉：它只让 GPU 层跨平台。

两点值得记录，因为它们对 WebGPU **有利**、且已经付过成本：

1. **回读路径已经是双缓冲异步的**（`GpuRenderer.cpp:1012` 起：`readPending_` + PBO +
   `GL_STREAM_READ`，"这帧收上一帧排队的"）——正好是 WebGPU 的 copy-to-buffer + async map 模型。
   WebGPU 最大的结构摩擦点，这里天然不存在。**M4 里别把它改成同步。**
2. `glFinish` 只在 profiling 开着时走；`GL_(UN)PACK_ALIGNMENT` 只有 alignment，
   WebGPU 的 `bytesPerRow` 能覆盖。

M4 里顺手做的 4 个 hedge（成本几乎为零，让 M4b 不用返工）——把后端敏感的点显式化：
回读（见上）、`gl_ClipDistance`（varying + 片元 `discard`，**WebGL2 和 WebGPU 都要这么写**，等于免费）、
`BlitFramebuffer` 2 处、`glFinish` 1 处。这 4 处就是 M4 全部的"语义重写"面。

**触发条件**：改主意要原生（macOS arm64 / iOS）；或者 WebGL2 撞上实际的性能/功能墙。
注意没有缓存的话，这种"每帧改一堆状态"的负载在 WebGPU 上会比 WebGL2 **更慢**。

## M5 资源管线：浏览器内抽取（3–5 天）

- [ ] 把 `native/PackMain.cpp` 编成 wasm 工具（它是纯 `std::filesystem` 代码），
      或用 File API + Emscripten 的 `WORKERFS` 直接让主程序读 ROM
- [ ] 首次运行引导：选择用户自己的欧版 ROM（`YIVP`）→ 抽取 → 写入 IDBFS/OPFS → 之后不再询问
- [ ] `.obb`（Android 版数据，可选，用于 HD 音乐）走同一条路
- [ ] 失败路径要给明确提示（ROM 区域不对、文件损坏、空间不足）

**验收**：全新浏览器配置 + 用户自备 ROM → 能进入游戏；刷新页面后不必重新抽取。

## M6 存档与设置（2–3 天）

- [ ] `SaveStore` 的原子发布：IDBFS/OPFS 里实现"写临时文件 → 校验 → 替换"，或改用 IndexedDB 事务
- [ ] `settings.ini` / `journal.txt` 落到同一持久化层
- [ ] 导出/导入存档（浏览器里给玩家一条逃生通道）

**验收**：刷新/关闭浏览器后存档与设置保留；导出再导入能复原。

## M7 输入（2–3 天）

- [ ] 键盘映射（沿用 `InputBindings.cpp`，纯 SDL3）、手柄（Gamepad API）
- [ ] DS 触摸屏 → 鼠标/触摸事件映射到下屏，含 `Battle display: DS screens` 布局
- [ ] 移动端：软键盘需求（起名界面）——先做"内嵌 HTML input"方案

**验收**：不使用鼠标也能完整走完开场到第一场战斗。

## M8 平台层收尾（3–5 天）

- [ ] `NitroThreads` / `NitroIdentity` / `SaveStore` 用真正的可移植实现替换 `wasm/src/port-stubs.cpp`
- [ ] `CrashHandler`：wasm 用 Emscripten 的异常/`abort` 钩子；原生端用各自平台的等价物
- [ ] `TextCanvas`（Win32 GDI 画调试字体）：移植到 SDL3 渲染或直接去掉
- [ ] `GameBoot`：启动流程与错误弹窗改成跨平台（`MessageBoxA` → 日志 + 页面提示）

**验收**：`wasm/src/port-stubs.cpp` 可以被删除，构建仍然通过。

## M9 [分支] 原生 64 位（macOS arm64 / iOS）——另一条路，成本高

只有在明确需要 App Store 或原生性能时才做。前置：`02-FINDINGS.md` 第 3 节的 263 个指针字段审计。

- [ ] 建立"guest 偏移"类型（`u32` 偏移 + 访问器），把 144 个带 DS 偏移注释的字段改为偏移语义
- [ ] 逐个处理 `isGameTextPointer()` 这类"整数编码指针"的惯用法（64 位下必须引入显式 tag 或句柄表）
- [ ] 资源装载器把 ROM 里的 32 位指针在装载时转换为宿主指针/偏移
- [ ] iOS：GLES 3.0（已弃用）或新写 Metal 后端；注意不能依赖 JIT
- [ ] macOS：可以保留 OpenGL 4.1 core（已弃用但可用），渲染改动最小

**验收**：macOS arm64 上原生跑起来；iOS 侧载可玩。

## M10 [分支] Android APK（复用 M4 的 GLES 3.0 后端，1–2 周）

- [ ] SDL3 Android 壳 + SAF 选择 ROM/`.obb`
- [ ] 触摸映射、生命周期、音频会话
- [ ] 32 位（`armeabi-v7a`）与 64 位（`arm64-v8a`，需要 M9）两条腿：先出 32 位版本验证，
      但注意 64-bit-only 机型装不上

**验收**：真机（含 64-bit-only 机型，走 M9）能玩。

## 建议顺序

`M2(✅) → M4 → M3 → M5 → M6 → M7 → M8`，M9/M10 视需求另开分支。
M4 先行的理由：它是唯一"看得见进展"的里程碑，也能尽早暴露渲染相关的隐藏假设。
M2 必须先做的理由：M4 会往两边清单里加文件（`GlLoader`/`GpuRenderer`/`GameWindow`/
`FramePresenter`/`Display`/`TextCanvas`/`GameBoot` 这 7 个现在都还进不了 wasm 清单）。

**2026-10 决定（已确认）**：目标定为**只要浏览器**。因此 GPU 走 **WebGL2**（M4 原样），
WebGPU 降级为 M4b 不排期；M9/M10 归档——浏览器路线不用付 263 个指针字段的偏移化改造，
而那是原生路线的真实门票，不是 GPU API。

如果哪天改主意要原生，顺序变成 `M2(✅) → M9 的 263 字段审计（纯审计、不写代码、1–2 天）→ 定 GPU → …`：
审计结果决定 M9 是 3 周还是 3 个月，而那时 WebGPU 反而是对的（它把 M9 里
"GLES 3.0（已弃用）或新写 Metal"的二选一塌缩成一个后端）。
