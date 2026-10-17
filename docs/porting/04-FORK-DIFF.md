# 本 fork 相对上游的改动

- Fork: `soimy/dqiv-wasm`（原 `GoldieLeGenie/DQIV-Recompiled`，改名保留 fork 关系）
- 上游基线：`e1826f9`
- **分支布局（2026-10 起）**：`main` 只镜像上游，本 fork 的全部工作都在 **`wasm` 分支**上。
  同步上游：`git fetch upstream && git merge upstream/main`，在 `wasm` 上执行；
  `main` 保持纯上游、可以无条件 fast-forward。

## 原则

**上游的 Windows 构建保持行为等价，上游合并保持 trivial。**
`CMakePresets.json`、`build.ps1`、`cmake/NativeOverlays.cmake` 一律未改；
所有跨平台改动都是"在非 Windows 上才生效"的分支，或是纯新增目录。

根 `CMakeLists.txt` 从 M2 起**不再逐字节等价**（这是 M2 的已知代价）。改动刻意保持最小：
6 处源清单换成共享函数的调用、1 处 MSVC 链接选项包进 `if(MSVC)`。
Windows 侧的**行为**逐条验过：两个平台各配置一次、和 HEAD 的清单逐条对比，14/14 一致，
且每个目标内部的源文件顺序（包括 Windows-only 文件的位置）保持原样，链接顺序不变。
唯一的可见变化是生成的 `.vcxproj` 里源文件路径变成绝对路径。

## 源码改动（3 处，Windows 行为不变）

| 文件 | 改动 | 原因 |
| --- | --- | --- |
| `game/src/ov015/btl/BattleSelectTarget.cpp` | 给三个 `switch` case 加花括号 | case 跳过了变量初始化，MSVC 容忍，Clang/GCC 直接报错。语义完全不变 |
| `native/GameServices.cpp` | `isGameTextPointer()` 的 `VirtualQuery`/线程栈/PE 镜像判断用 `#ifdef _WIN32` 包起来，非 Windows 在分配表检查后返回 `false` | 该启发式依赖 Win32 地址空间；Windows 路径一字未动 |
| `native/Parallel.hpp`（新增）+ `native/Display.cpp`、`native/GpuRenderer.cpp` | 3 处 `std::for_each(std::execution::par, …)` 改用 `DQIV_PARALLEL_FOR_EACH` 宏 | libc++（macOS/iOS/Android/wasm32）没有并行算法实现；宏在 MSVC 上仍是 `std::execution::par`，可用 `DQIV_NO_PARALLEL` 强制串行 |

## 构建改动（M2，Windows 行为不变）

| 文件 | 改动 |
| --- | --- |
| `cmake/Sources.cmake`（新增） | 各目标源清单的唯一登记处。Windows-only 文件（`SaveStore.cpp`、`NitroThreads.cpp`、`NitroIdentity.cpp`）在 `if(WIN32)` 里就地插入，位置不变；`dqiv_portable_sources()` 给出非 Windows 的桩；`dqiv_portable_platform_sources()` 给出上游放在 exe 目标里、但完全可移植的 `DssRuntime.cpp` |
| `CMakeLists.txt` | 6 处源清单换成共享函数调用；`/WHOLEARCHIVE`、`/SECTION:.rdata,RW`、`/ENTRY`、`/DEBUG:FULL`、`/INCREMENTAL:NO` 包进 `if(MSVC)` |
| `wasm/CMakeLists.txt` | 同上引用共享清单；`dqiv_sound` 的链接目标统一成上游的 `dqiv_platform`（原先指向 `dqiv_core` 是历史遗留） |
| `.github/workflows/ci.yml`（新增） | `windows-2022`（pin：`windows-latest` 和 `windows-2025` 现在都指向只装 VS 2026 的镜像，而 preset 硬编码 VS 2022 的 generator）真编译上游 Windows 构建（`windows-game` preset，Release）；`ubuntu-latest` 从零跑 `./wasm/build.sh` 并断言 spike 输出 `= 3` 与四条布局断言。Emscripten 钉在 4.0.13（spike 用的版本） |

`dqiv_sound` 那处改动值得单独说明：`gameOverlayImages()` 只在 `GameBoot.cpp`（exe 目标）里被引用，
`NitroOverlays.cpp` 不引用它，所以统一链接目标不会引入新的未定义符号。

## 源码改动（M2 之后，Windows 行为不变，除一处有意的）

| 文件 | 改动 | 原因 |
| --- | --- | --- |
| `native/GlLoader.hpp`/`.cpp` | 5 个入口点从表里拿掉（`glClearDepth`、`glBindFragDataLocation`、`glMapBuffer`→`MapBufferRange`、`GetQueryObjectiv`→`GetQueryObjectuiv`）；`GetQueryObjectui64v` 移到 `DQIV_GL_DESKTOP_FUNCTIONS` 条件段，**在 .cpp 的 define+load 和 header 的 declare 两处都要调用**（漏了 header 那次就是 CI 上那次 `C2039`） | 按 GLES 3.0 逐个核过；`load()` 缺一即抛，所以表必须正好是 GLES 3.0 有的 |
| `native/PixelOrder.hpp`（新增） | `kShiftR/G/B`、`uploadOrder()`、`fromReadback()` | WebGL2 的上传和回读都没有 BGRA 格式。放在 header 里是因为 layout probe 要在没有 GL context 的情况下把这个往返钉死 |
| `native/GpuRenderer.cpp` | `DQIV_GLSL_HEADER` 宏（ES 侧多两行精度限定符，外加 `precision highp isampler2D`——ES 里整型 sampler **没有默认精度**）；`gl_ClipDistance` → varying + `uClipEnabled` + `discard`；`kUploadFormat` wasm 上 `GL_RGBA`；GPU 计时 `#ifdef` 隔离 | **这是 Windows 侧唯一一处行为改动，是有意的**：ES 3.00 唯一缺的就是 `gl_ClipDistance`，硬件内建的插值规范明说 implementation-dependent，varying 是透视正确的 |
| `native/TextCanvas.cpp/.hpp` | `windows.h`、GDI 构造/析构、`text()` 全部 `#ifdef` 包起来 | 只用来画 FPS 角标。wasm 上角标保留矩形、失去文字——诚实的最小实现 |
| `native/CrashHandler.cpp` | `windows.h`/`dbghelp.h`/SEH 注册包起来；wasm 上 `trace()` 走 `emscripten_get_callstack(EM_LOG_C_STACK, …)` | `divideLikeTheDs`（DS 软件除法语义：`x/0` 得 `x`、`x%0` 得 `0`）是桌面专属；wasm 的整数除零是 trap，而且实例已经没了 |
| `native/GameBoot.cpp` | `<share.h>`/`MessageBoxA`/`_fsopen`/`"NUL"`/NVIDIA-AMD dllexport 包起来；`main()` 拆成 `GameRun`，wasm 上 `emscripten_set_main_loop` | 一份定义、两个驱动。两处顺序变化写在 commit 里、没有糊过去：`overlays.load(38)` 现在在 `FramePresenter` 构造之后；`GameRun` 是 static local，成员在程序退出时析构 |
| `native/GameWindow.cpp` | wasm 上请求 MINOR 0 + `SDL_GL_CONTEXT_PROFILE_ES`；`SDL_SavePNG` 走 `canvas.toDataURL()` + 下载链接；分辨率预设钳到屏幕可用区域 | WebGL2 就是 GLES 3.0——请求桌面 3.3 core 正是浏览器里建不出 context 的原因。笔记本上 2560x1440 会溢出页面 |
| `native/NitroSymbols.cpp` | 两个 wrapper 恢复，**包在 `#ifdef _MSC_VER` 里** | MSVC 的 mangling 和 Itanium ABI 不同（`?unkfunc_02068ec8@@YAXH@Z` vs `_Z16unkfunc_02068ec8i`），所以 adapter 在 MSVC 上是必需的、在 wasm 上会撞名 |
| `native/NitroThreads.hpp` | `runPortableThreads()`、`runIdle()` 加 `#ifdef __EMSCRIPTEN__` | 协作调度（用户选的路）：`OS_PauseThread` 就是帧边界，两行，不是调度器框架 |
| `native/GlLoader.hpp` | `MapBufferRange`/`UnmapBuffer` 移进 `DQIV_GL_DESKTOP_FUNCTIONS`（wasm 上为空） | Emscripten **实现了** `glMapBufferRange` 但**直接拒绝 `MAP_READ`**（`libwebgl.js`：`access does not support MAP_READ or MAP_UNSYNCHRONIZED`），而 `glGetBufferSubData` 根本没实现。机器核对：52 个入口里**正好这两个**在生成的 JS 里缺席 |
| `native/GpuRenderer.cpp`（M3a 期间） | `readNative` 开头 wasm 强制 `immediate=true`；`MapBufferRange` 调用点 `#ifndef __EMSCRIPTEN__`；**`BindTexture` 改成无条件**；`DQIV_TRACE` 逐句报 GL 错误（桌面空宏） | 异步 readback 在 wasm 上没有"读"这一半，所以每次读都是直读——双缓冲买到的流水线（上一帧的读和这一帧的画重叠）在这里没了，说清楚而不是糊过去。**无条件绑定是真 bug 修复**：无纹理 batch 会沿用上一个 cycle 留在单元 0 上的东西，而 `readNative` 在那儿留的是挂在 framebuffer 上的纹理，所以第二次画是反馈环 → `GL_INVALID_OPERATION`。不修的话卡的是帧循环 |
| `native/AssetPack.cpp` | `executablePath()` 加 `#elif defined(__EMSCRIPTEN__)` 返回 `/` | `read_symlink("/proc/self/exe")` 在 node 和 Chrome 里**都**抛（实测） |
| `native/GameBoot.cpp`（M3a 期间） | wasm 上 `openResources()` 允许空 asset pack（Windows 保持抛）；`early-*` 上下文以引用接进 `GameRun` | 让浏览器跑能过这一关，资产在 M5 由用户自己的 ROM 提供 |
| `wasm/src/early-contexts.cpp`（新增） | `gameOverlayImages()` 在**全局作用域**声明（和 `GameBoot.cpp` 一致），`namespace pc` 里定义 `earlySavePath`/`earlyIdentityPath`/`earlyIdentity`/`earlyVideo`/`earlyPower`/`earlyOverlays` | M3a 的机制：clang 在一个 target 内按源文件顺序发静态初始化器，且 exe 的 object 先于 library 的 object——两条都实测过。namespace-scope 的 `const` 在 C++ 里是**内部链接**，所以要 `extern` |
| `wasm/CMakeLists.txt`（M3a 期间） | `-sINITIAL_MEMORY=64MB -sALLOW_MEMORY_GROWTH=1` | 实测：heap 停在 18481152 字节、游戏在自己初始化的一个 `operator new` 上爆掉。代价是增长时复制整个 heap（一次停顿），起点拉高是为了少几次 |

`wasm/check-shaders.py`（新增，可执行）：从 `native/GpuRenderer.cpp` 把 8 个 shader 抽出来，
**两个 header 都从 `#define` 行读**（不是硬编码），写成 `.vert`/`.frag` 过 `glslangValidator`，
不需要 profile 参数——它从 `#version` 行读 flavour、从扩展名读 stage。16/16 过。

`wasm/src/gpu-selfcheck.cpp`（新增，只进 `dqiv_wasm_game`，所以 MSVC 看不到它、warning 分布不变）：
字节序和 clip 的第一份真 GPU 证据，跑在真浏览器（headless Chrome + CDP）上。字节序 `ok`；clip 的梯度
`0→2436, 0.5→2436, 0.8→2289, 1.0→1218, 1.5→0`，两三角形一次画正好是两倍，交换实验也交换。
自检的输出走 `journal.txt`（`main()` 把 stdout/stderr `freopen` 进去），所以它在浏览器的虚拟文件系统里、
不在控制台——CDP 读 `/saves/journal.txt` 才看得到。

`wasm/index.html`（新增）：canvas 的 id 必须是 `#canvas`（SDL 的默认 selector），CSS 让它填满页面。
页面把 stdout 镜像到底部。`--dump-dom` + `--virtual-time-budget` 在主循环跑起来之后会**挂住**，所以验证
走 CDP over `--remote-debugging-port` 用真实时间；Node 22 有全局 `fetch`/`WebSocket`，裸 CDP 客户端不需要
驱动库。`Page.addScriptToEvaluateOnNewDocument` 装 handler（在任何页面脚本之前），因为导航会清掉 `window`。

## 新增目录（纯加法）

| 路径 | 内容 |
| --- | --- |
| `cmake/Sources.cmake` | 共享源清单（见上） |
| `.github/workflows/ci.yml` | 双平台 CI，守住"Windows 仍 0 error"这条验收 |
| `wasm/` | Emscripten 交叉构建脚手架：`CMakeLists.txt`、`build.sh`、`README.md`、`check-shaders.py`、`index.html`、`src/`（可移植桩、spike main、布局探针、GPU 自检） |
| `docs/porting/` | 本次会话的产物与经验：spike 报告、可移植性清单、路线图、本文件 |
| `PORTING.md` | 根目录入口，指向 `docs/porting/` |
| `.gitignore` | 追加 `/wasm/build/` |

## 未做的事（有意为之）

- overlay 机制（M3）和资源管线（M5）还没实现。渲染层已经能编、能链接、能在 node 里跑到底，
  在浏览器里跑起来是 M4 的验收。`TextCanvas` 的文字、`autoQuality`、`divideLikeTheDs` 这三处
  在 wasm 上是诚实的最小实现，不是"实现了"。
- `native/SaveStore.cpp`、`NitroThreads.cpp`、`NitroIdentity.cpp` 仍保持上游的 Windows 实现，
  wasm 构建用 `wasm/src/port-stubs.cpp`（约 55 行）替代；
  `native/DssRuntime.cpp` 经复验是**完全可移植**的（只是上游把它放在 exe 目标里），wasm 构建直接编译它。
- `CMakePresets.json` 里仍然只有 `windows-game` 一个预设。wasm 侧走 `wasm/build.sh`，
  加预设的收益不大，而且会让 `cmake --preset` 的语义在两个平台间不一致。
