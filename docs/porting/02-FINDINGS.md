# 可移植性清单与经验

这份文档记录"哪些能编、哪些不能、为什么"，以及移植时必须知道的事实。
数据来自 2026 年的实测（上游基线 `e1826f95`），详见 `01-SPIKE-REPORT.md`。

## 1. 上游为什么只能 Windows

| 约束 | 位置 | 说明 |
| --- | --- | --- |
| 必须 32 位指针 | `CMakeLists.txt:3` | `if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4) message(FATAL_ERROR ...)` |
| 必须 MSVC | `CMakeLists.txt:131-160` | `add_executable(... WIN32 ... .rc)`、`/WHOLEARCHIVE:`、`/SECTION:.rdata,RW`、`/ENTRY:mainCRTStartup`、`/utf-8`、`/Gy /MP8` |
| Windows 资源 | `project(... LANGUAGES C CXX RC)` | `native/DragonQuestIV.rc` 需要 RC 语言，非 Windows 工具链没有 |
| overlay 段布局 | `cmake/NativeOverlays.cmake` | 用 `/FI` 注入 `#pragma init_seg / data_seg / bss_seg / section` + `__declspec(allocate(".d$NNa"))`，靠 MSVC 链接器的 `$` 段排序工作 |
| SDL3 | `FetchContent(SDL3 3.4.16)` | 本身跨平台，但上游只走桌面路径 |
| 渲染 | `native/GameWindow.cpp` | 显式请求 OpenGL 3.3 **core** profile |
| 构建入口 | `build.ps1` / `build.bat` | PowerShell + `vswhere` 找 VS2022 |

`CMakePresets.json` 里只有一个 `windows-game` 预设（Visual Studio 17 2022 / Win32）。

## 2. 规模与依赖分布

| 部分 | 规模 |
| --- | --- |
| `native/`（PC 层，全部要移植的代码） | 97 个文件 / 9,186 行 |
| `game/src`（反编译的游戏本体） | 560 个文件 / 162,904 行 |
| `game/libs`（NitroSDK + NitroSystem 重实现） | 165 个文件 / 23,720 行 |
| `game/include` | 449 个文件 / 24,136 行 |

**Windows 依赖的分布**（这是好消息：集中且小）

- 8 个文件 `#include <windows.h>`：
  - `AssetPack.cpp`：已自带 `#ifdef _WIN32` 守卫 → 非 Windows 直接编过
  - `GameServices.cpp`：本 fork 加了 `#ifdef _WIN32` 守卫（见 `04-FORK-DIFF.md`）
  - `SaveStore.cpp` / `NitroThreads.cpp` / `NitroIdentity.cpp`：**真正 Windows-only**，wasm 用 `wasm/src/port-stubs.cpp`（约 55 行）替代
  - `GameBoot.cpp`（`MessageBoxA` + 入口）/ `CrashHandler.cpp`（DbgHelp + `CONTEXT`）/ `TextCanvas.cpp`（`CreateFontW`）：属于必须重写的窗口/入口层
- 3 个文件只做 `_WIN32` / `__declspec` 条件判断：`game/include/globaldefs.h`、`game/libs/NitroSDK/include/nitro/types.h`、`game/src/main/object/UnkData_020b62c0.cpp`
- MSVC 专属语法极少：`_MSC_VER` 1 处、`#pragma comment` 2 处；没有 `intrin.h`、`__forceinline`、`stdcall`
- **没有 `std::thread`**（只有 3 处 `std::mutex`），所以 wasm 不需要 pthreads
- `native/DssRuntime.cpp`（上游放在 exe 目标里）经复验**完全可移植**，wasm 直接编译

## 3. 32 位指针模型（唯一的硬约束）

上游的资源（从 ROM 抽出的游戏数据）里含 32 位指针，因此整个游戏假定"指针值空间 = 32 位"：

- `game/include` 里 **263 个指针成员字段**，其中 **144 个带 `// 0x..` 的 DS 结构偏移注释** → 布局与 DS 内存/ROM 二进制一一对应
- 结构体大小断言：`DataObject==0x10`、`UnkGameApplication==0xb4`（`GameBoot.cpp`）、
  `OSThread==0xc0`、`OSMutex==0x18`（`NitroThreads.cpp`）、`FS_File==0x48` + `offsetof(currentPosition)==0x2c`（`NitroFilesystem.cpp`）、
  `SequencePlayerParams==32`、`SequenceTrackParams==32`（`SoundSequence.hpp`）
- `GameServices::isGameTextPointer()` 保留了原始 DS 代码的惯用法：**一个整数里要么是字符串指针、要么是消息 ID**，
  靠地址范围启发式区分。它依赖"指针值落在进程自己的地址空间里"这个前提
- 纹理对象里还有"relocated / layered"标记（`native/Texture.cpp` 明确拒绝带重定位指针的纹理对象）

### 各目标的处境

| 目标 | 指针模型 | 结论 |
| --- | --- | --- |
| **wasm32** | 4 GB 线性内存里的 32 位偏移 | **天然满足**，实测 553/553 编过、布局断言全部成立 |
| **Android arm32（armeabi-v7a）** | 32 位 | 天然满足；但 Pixel 7 / 新骁龙等 64-bit-only 机型装不了，Play 也要求 64 位 |
| **原生 64 位（macOS arm64、iOS、arm64 Android）** | 64 位 | **不满足**：263 个字段 / 144 个布局注释需要"指针 → 32 位偏移/句柄"的改造 |

### 为什么原生 macOS 连"取巧"都不行（实测）

理论上可以"把所有游戏内存分配进低 4 GB，让 32 位字段存截断指针"。在 macOS arm64 上这**不可行**：

```
mmap(MAP_FIXED) 0x10000000  -> Cannot allocate memory
mmap(MAP_FIXED) 0x80000000  -> Cannot allocate memory
mmap(MAP_FIXED) 0x100000000 -> Cannot allocate memory     (4 GB)
mmap(MAP_FIXED) 0x200000000 -> OK                          (8 GB)
```

即 macOS arm64 用户态地址空间的前 8 GB 拿不到（`__PAGEZERO` + dyld/共享缓存占了低区），
所以"低地址竞技场"只能在 Linux/Android arm64 上考虑，macOS/iOS 必须老老实实做偏移化改造。

## 4. 宿主平台事实（查证/实测）

- **Rosetta 2 不支持 32 位 x86**；macOS 早已不能运行 32 位程序。
- **CrossOver 27 起放弃 32 位游戏容器和 Intel Mac 支持**（全面转 ARM64），
  所以这个 32 位 exe 在当前的 CrossOver 上跑不了；Whisky 已停更。
  → Apple Silicon Mac 上"原生运行上游产物"这条路基本关闭。
- **Windows 11 on ARM** 通过 WOW64 + Prism 可以模拟运行 32 位 x86 应用（微软官方文档确认），
  且 VS2022 的 Arm64 原生版本可以交叉编译 x86 目标 → UTM 里装 Windows 11 ARM 构建是可行的。
  但虚拟机里没有硬件 OpenGL 3.3 驱动（虚拟 GPU 只提供基础显示/有限 D3D），
  这个项目要 GL 3.3 core，所以"在 ARM 虚拟机里玩"要额外折腾 Mesa 之类，帧率不保证。
- 上游 **Releases 里没有任何二进制附件**（1.1/1.2 都是空 assets），必然要自建。

## 5. Clang/GCC 比 MSVC 严格的地方（实测）

| 类别 | 数量 | 处理 |
| --- | --- | --- |
| `switch` case 跳过变量初始化 | **1 处**（`ov015/btl/BattleSelectTarget.cpp`） | 加花括号，已修（语义不变） |
| 花括号初始化的窄化（`-Wc++11-narrowing`） | **234 处 / 28 个文件** | 已是本意（`-1` 哨兵进 `unsigned char`、`0xa000002b` 形式的 DS 地址进 `int`），值与原 ARM 代码一致；在 `wasm/CMakeLists.txt` 里关闭该诊断，不去改 234 行反编译产物 |
| 函数原型跨 TU 不一致 | **11 处**（`func_02061228`、`func_02062040`、`func_0206e154`、`func_020732a0`、`func_0207343c`、`func_020738a4`、`func_02075780`、`func_02075aa4` 等） | wasm-ld 只给警告，不影响链接；原生链接器不检查这类问题 |
| libc++ 没有并行算法 | 3 处 `std::execution::par` | 已用 `DQIV_PARALLEL_FOR_EACH` 宏包装（MSVC 仍走 `par`） |

## 6. 渲染层：要写的后端有多大

- **8 个着色器**，全部 `#version 330 core`（`native/GpuRenderer.cpp`）：场景/雾/叠加/降采样/合成等
- 实际调用的 GL 入口点约 **60 个**，形态很"素"：VBO/VAO/FBO、纹理、uniform、`glDrawArrays`、模板、混合。
  没有 sampler object、没有 compute、没有 DSA、没有 instancing
- **WebGL2 / GLES 3.0 的缺口只有三处**：

| 缺口 | 位置 | 处理方案 |
| --- | --- | --- |
| `gl_ClipDistance[0..3]` | `GpuRenderer.cpp:41` 场景顶点着色器 | GLSL ES 3.00 没有这个内建。把 `aClip` 传成 varying，在片元着色器里 `discard` |
| `glMapBuffer/glUnmapBuffer`、`glBeginQuery/EndQuery/glGetQueryObjectui64v` | `GpuRenderer.cpp`（GPU 计时/回读，性能剖析路径） | WebGL2 无核心缓冲映射与计时查询：`#ifdef` 掉，或用 `EXT_disjoint_timer_query_webgl2` |
| 上下文与 GLSL 版本 | `GameWindow.cpp` 请求 3.3 core；shader 里 8 处 `#version` | 改请求 GLES 3.0；着色器改 `#version 300 es` + 精度限定符 |

- 其余全部 1:1 可用：`texelFetch`（14 处）、`gl_FragDepth`、`glBlitFramebuffer`、`glClearBufferfv/fi`、
  `glDrawBuffers`、`glVertexAttribIPointer`、`glReadPixels` 等
- SDL3 用到的子系统只有 VIDEO / GAMEPAD / AUDIO / GL，Emscripten 的 `sdl3` 端口全部提供

## 7. DS overlay 机制（唯一需要设计的地方）

上游把 38 个 DS overlay 编译成**原生代码段**，并保留 DS 的"load/unload overlay 时数据段被重置"语义：

- 生成侧（MSVC 专属）：`cmake/NativeOverlays.cmake` 为每个 overlay 生成一个被 `/FI` 强制包含的头，
  内容为 `#pragma init_seg(".i$NNm", pcOverlayExitNN)`、`#pragma data_seg(".d$NNm")`、`#pragma bss_seg(".b$NNm")`；
  再用 `#pragma section(".d$NNa"/".d$NNz")` + `__declspec(allocate(...))` 放两个边界符号，靠 MSVC 链接器的
  `$` 段排序把该 overlay 的所有数据夹在 `a` 和 `z` 之间
- 运行期（可移植）：`native/NitroOverlays.cpp` 拿 `OverlayImage{id, data 区间, bss 区间, constructors 区间}`
  做快照/恢复，并维护每模块的析构列表（`pcRegisterOverlayDestructor`）

**Clang / wasm 的等价方案**（可行性高，尚未实现）：

1. 用 `__attribute__((section("…")))` + 生成代码里的两个边界符号替代 `$` 段排序；
   `wasm-ld` **不支持** ELF 的 `__start_/__stop_` 合成符号，所以边界符号必须显式生成（生成器本来就在生成代码）。
2. `#pragma init_seg` 的"把构造调用路由到 `pcOverlayExit`"用两件事替代：
   Clang 的 `__attribute__((constructor(priority)))` 做顺序，**自己实现 `__cxa_atexit`**
   把析构回调收进 overlay 的析构列表（这正是 MSVC 那边 `init_seg` 干的事）。
3. 好消息：这套东西只在 `cmake/NativeOverlays.cmake` + `native/NitroOverlays.cpp` 两处，
   游戏代码本身不需要改；而且没有 overlay 时游戏仍能编译链接（本次 spike 就是这样）。

## 8. 资源、存档与输入

- **资源抽取**：`native/PackMain.cpp` 只用 `std::filesystem` + `argc/argv`，**本身可编成 wasm**，
  所以浏览器版可以"用 File API 读玩家自己的 ROM → 现场抽包 → 灌进 MEMFS/IDBFS"，不随包分发任何游戏数据。
  打包格式是自定义 `DQIVPK01` + 32 字节 footer + FNV-1a 校验。
- **存档**：原版是 64 KiB 卡带备份镜像，`SaveStore` 负责原子发布（Windows 用 `MoveFileExW(MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)`）。
  wasm 版需要把"原子发布"换成 IDBFS/OPFS 的等价做法（当前桩里是 TODO）。
- **输入**：桌面键位/手柄映射已在 `native/InputBindings.cpp`（纯 SDL3，**可移植**）；
  DS 触摸屏对应下屏点击，浏览器/移动端映射到指针事件很直接。

## 9. 法律

仓库不含游戏数据。上游构建时从使用者自备的欧版 ROM（`YIVP`）抽取资源；
移植版必须沿用同样的模型，且不要把 ROM / `.obb` / 抽取出的 `assets/` 提交进仓库。
