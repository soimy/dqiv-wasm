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

**M3a 已落地（实测）**：静态初始化顺序。MSVC 给每个 `/main/` 源文件一个 `init_seg` 头，抑制 CRT
启动构造，所以游戏的全局 ctor 在 `main()` 里加载 overlay 38 时才跑，那时 `pc::NitroVideo` 已经存在。
wasm 没有 `/FI` 也没有 `init_seg`，所以 `UnkDisplay` 的 ctor 在 `__wasm_call_ctors` 里先于 `main()`
跑了，`pc::current` 是 null。clang 在 wasm 上真正给的杠杆只有两个（都实测过）：**链接顺序**决定静态
初始化顺序（exe 的 object 先于 library 的 object），以及给单个全局变量加 `__attribute__((section))`
确实能造出那个段。`#pragma init_seg/data_seg/bss_seg` 和 `init_priority` 在 clang/wasm 上**全部被忽略**
（`-Wunknown-pragmas`、`-Wignored-attributes`），`__declspec(allocate)` 是 MSVC-only。所以 M3a 用链接顺序。

**M3b（真 snapshot/restore）需要地址，所以选了两遍链接**：第一遍带 `--Map` 跑，读地址，用读到的地址
生成表，再链接一遍。`wasm-ld` 有 `--Map=` ✓，没有段排序选项。

- [x] M3a：用链接顺序把静态初始化顺序修对（`wasm/src/early-contexts.cpp`）
- [ ] M3b：两遍链接拿到地址，生成真正的边界符号
- [ ] 让 `dqiv_overlay_registry()` 的生成内容与 MSVC 路径并存（同一份 CMake 函数，按编译器分支）
- [ ] 单元测试：连续 load/unload 两个 overlay，确认可写数据被正确恢复、析构被正确调用

**验收**：在 wasm 上 `FS_LoadOverlay`/`FS_UnloadOverlay` 往返 N 次后，全局数据与首次加载一致。

## M4 WebGL2 / GLES 3.0 渲染后端 + Emscripten 启动壳（1–2 周，最大的一块）

**状态（2026-10-18，`a6691ba`，CI 绿 ✓）：跑通了。** 画面到画布了——合成面几乎每个像素都有
内容、逐帧变化，画布和它逐点一致；帧率 ~30 fps；游戏自己的 `while(nativeStep()) OS_Wait()`
循环能过 boot logo、进开始动画。这一轮是三件事：SDK 线程的 park 用一对 `setjmp/longjmp`
真的交回控制权（`emscripten_sleep(0)` 实测不能 unwind，线程体永不返回）——音频路径因此通了；
帧边界由 frame callback 装填，每帧恰好一次（原来是每次 VCount 读数都跑一次边界，所以 `pace()`
每帧等 28 次，只有 ~3 fps）；`BlitFramebuffer` 到默认 framebuffer 在 WebGL2 上什么都不写，
所以 wasm 上改用 overlay 程序全屏贴图。读数见 `05-HANDOFF.md`。

- [x] `GlLoader`：按 GLES 3.0 逐个核过入口点表，5 个不在表里的已从表里拿掉，现在保留 `gl::load()` 的"缺一即抛"
- [x] 着色器：8 处 `#version 330 core` → `#version 300 es` + 精度限定符。逐条核过，
      ES 3.00 唯一缺的是 `gl_ClipDistance`，改成 varying + 片元 `discard`，开关用一个
      uniform 承载（这是 Windows 侧唯一一处行为改动，是有意的：硬件内建的插值方式
      规范明说 implementation-dependent，varying 是透视正确的）
- [x] **字节序**（原计划漏了，11 处）：游戏的约定是 `0xAARRGGBB`，代码按 BGRA 交给 GL。
      WebGL2 的上传和回读都没有 BGRA 格式，所以 wasm 上先把字节 swizzle 成 RGBA，
      读回来的 channel 位移相应互换。两个后端现在产出和消费同一套值。
- [x] 性能剖析路径（GPU 计时）用 `#ifdef` 隔离：`GL_TIME_ELAPSED` 不在 GLES 3.0 里。
      代价说清楚——`settings.autoQuality` 在 wasm 上会失效，直到接上
      `EXT_disjoint_timer_query_webgl2`。
- [x] Windows 依赖的 wasm 替身（实测这 4 个文件编不过）：`TextCanvas`（整套 GDI，只用来
      画 FPS 角标）、`CrashHandler`（SEH/SymDB）、`GameBoot`（`windows.h`/`_fsopen`/
      `MessageBoxA`）、`GameWindow`（`SDL_SavePNG`，emscripten 的 SDL3 port 里没有；
      走 `emscripten_run_script` 的 `canvas.toDataURL()` + 下载链接，顺带留一个看画面的手段）
- [x] `GameWindow`：改为请求 GLES 3.0；Emscripten 下用 `emscripten_set_main_loop` 替代 Win32 消息循环
- [x] `FramePresenter` / `Display`：适配 canvas 尺寸、`devicePixelRatio`、resize 事件。
      **这条几乎没有代码改动**——SDL3 3.2.4 的 emscripten driver 已经把四件事都实现了：
      `GetWindowSizeInPixels` 返回 logical × DPR、`HIGH_PIXEL_DENSITY` 读 DPR、
      `SetWindowSize` 设 canvas 元素尺寸和 CSS 尺寸并发 `RESIZED`、全屏是真的 HTML5 全屏。
      `GameWindow` 每帧重新读 drawable 尺寸，所以 resize 自动生效，不需要显式处理事件。
      真正改的只有一处：wasm 上把分辨率预设钳到屏幕可用区域，否则笔记本上 2560x1440 会溢出页面。
- [x] `GameBoot` 需要 `gameOverlayImages()`，那是 M3 的 registry。本轮先用最小 stub
      （构造自然跑，不做 snapshot/restore），M3 落地后重测标题画面
- [x] `index.html`：canvas 的 id 必须是 `#canvas`（SDL 的默认 selector），CSS 让它填满页面，
      SDL 就会拿 CSS 尺寸 × DPR 当绘制缓冲，并在窗口变化时发 resize。页面把 stdout 镜像到底部，
      因为没有浏览器自动化，自检那行得靠人看。`.data` 还没有——那是 M5。

**验收**（按实测拆开——标题画面归 M5，因为 `GameBoot::openResources()` 没有资产就直接抛）：
浏览器里渲染管线真的跑起来——GLES 3.0 context、`emscripten_set_main_loop`、canvas 尺寸 /
`devicePixelRatio` / resize，稳定 60 fps；`Settings > Display` 的分辨率与拉伸选项可用。

字节序和 clip 的第一份真 GPU 证据走 `wasm/src/gpu-selfcheck.cpp`，它在 context 建好、
`gl::load()` 成功之后跑一次，往控制台打两行：
- **字节序**：4 个已知的 recipe 字进 plane 0，同一个字必须原样回来。这是整条路径——
  `uploadOrder` → 上传 → ES compose shader → `ReadPixels` → `fromReadback`——端到端，不是其中某一环。
- **clip**：一个三角形完全在屏幕内要画出来、完全在外要画不出来、跨边的要画出它的一部分。
  说清楚它能证明什么、不能证明什么：w=1 时这四个 clip 值恰好就是 NDC，所以跨边的三角形两种实现
  给出同一个答案。这个改动是为了**跨边时的确定性**（硬件内建的插值规范明说 implementation-dependent，
  varying 是透视正确的），所以自检是"和桌面路径画一样的图"，不是"证明是 varying 起的作用"。

不依赖浏览器的机器门：8 个 shader 两个 profile 都过 glslang（`wasm/check-shaders.py`）、字节序
往返在 layout probe 里钉死、真游戏在 node 里跑到底并停在 GL 边界那一行
（`std::runtime_error: No native video context is active`）——这条现在 CI 里守着，它证明整条
decompiled 启动流程（每个 overlay、display plugin、sound system）都跑对了。

**wasm 上已核实的 SDL3 / Emscripten 事实**（从 emsdk 里 SDL 3.2.4 的源码、emscripten 的
`settings.js`、`library_pthread_stub.c` 和 musl patch 读出来的，不是推测）：

- `MAX_WEBGL_VERSION` 默认是 **1**。SDL 的 driver 只在请求的 GL 主版本是 3 时才把 context 升到
  WebGL 2，所以 `-sMAX_WEBGL_VERSION=2` 必须显式加，否则 8 个 `#version 300 es` 全挂。
  `dqiv_wasm_game` 的 link options 里加了。
- `GL_ENABLE_GET_PROC_ADDRESS` 默认是 true，所以 `GlLoader` 走 `SDL_GL_GetProcAddress` 没问题。
- `SDL_SetWindowPosition` 在 emscripten driver 的函数表里是**注释掉的** → no-op，居中靠 CSS。
- `SDL_GL_SetSwapInterval(-1)`（adaptive，`applySettings` 先试的那个）在 emscripten 会失败，报
  "Late swap tearing currently unsupported"；现有代码正好兜回 1。0 → `EM_TIMING_SETTIMEOUT`、
  1 → `EM_TIMING_RAF`，所以 wasm 上真正的 pacer 是 RAF。
- `SDL_SyncWindow` 在 emscripten 是 no-op（driver 没有 `SyncWindow`）。
- **`SDL_DelayPrecise` 在没有 pthreads 时是忙等**：`library_pthread_stub.c` 里
  `emscripten_thread_sleep` 是个 spin loop。协作调度（无 SharedArrayBuffer、无 COOP/COEP）
  意味着这条对我们是生效的——`pace()` 里的等待在浏览器里是烧 CPU，不是睡觉。
- `interpolating()` 在 wasm 上基本不触发：emscripten 的 display mode `refresh_rate` 是 0，
  `> 61` 恒假。所以 120/144/165/240 这些帧率设置在浏览器里被显示器刷新率封顶。

**链接期签名不匹配：13 个，其中 7 个在 spike 里就有。** 全是反编译源码里同一个函数在不同 TU 被
猜成不同签名（`void` vs `i32` 返回、`(i32)` vs `(i32,i32)`）。MSVC 上同样的不匹配是**静默**的
（C 语言不同 TU 之间不检查），wasm 只是把它说出来。新增的 6 个全在声音系统里
（`UnkSoundSystem.cpp` / `snd_arcplayer` / `snd_arcstrm` / `snd_player` / `snd_bank`）。wasm-ld
会插 thunk 适配所以能链接，但返回类型和参数个数不一致的那些**已经在执行路径上 trap 了**（实测）：
浏览器里游戏打完自己那行 `Original application initialization`，下一个动作就是
`RuntimeError: unreachable` at `signature_mismatch:func_02063920`（spike 里那 7 个之一），调用链是
`unkfunc_0207e934` ← `UnkApplication::unkfunc_02057f8c(0)` ← `main()`。所以它不是"以后再看的风险"，
是当前的挡路点，排在 M3b 前面。

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

`M2(✅) → M4(跑通✅) → M3a(✅) → 签名不匹配 → M3b → M5 → M6 → M7 → M8`，M9/M10 视需求另开分支。

**顺序改过两次，都是实测逼的，不是计划。** 第一次：静态初始化——MSVC 的 `init_seg` 头让游戏的
全局 ctor 在 `main()` 里加载 overlay 38 时才跑，wasm 上没有这个机制，所以 M3 从"以后的里程碑"
变成了浏览器路线的挡路项。第二次：那 13 个链接期签名不匹配**已经在执行路径上 trap 了**（实测，
见下），所以它排在 M3b 前面。

M4 先行的理由当时是：它是唯一"看得见进展"的里程碑，也能尽早暴露渲染相关的隐藏假设。它做到了——
而且真的暴露了：一个渲染器真 bug（无纹理 batch 沿用上一个 cycle 留在纹理单元 0 上的绑定，而那是
挂在 framebuffer 上的纹理，所以是反馈环）。这条不修的话卡的是帧循环，不只是自检。

**2026-10 决定（已确认）**：目标定为**只要浏览器**。因此 GPU 走 **WebGL2**（M4 原样），
WebGPU 降级为 M4b 不排期；M9/M10 归档——浏览器路线不用付 263 个指针字段的偏移化改造，
而那是原生路线的真实门票，不是 GPU API。

如果哪天改主意要原生，顺序变成 `M2(✅) → M9 的 263 字段审计（纯审计、不写代码、1–2 天）→ 定 GPU → …`：
审计结果决定 M9 是 3 周还是 3 个月，而那时 WebGPU 反而是对的（它把 M9 里
"GLES 3.0（已弃用）或新写 Metal"的二选一塌缩成一个后端）。
