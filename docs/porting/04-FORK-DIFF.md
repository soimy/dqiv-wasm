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

## 新增目录（纯加法）

| 路径 | 内容 |
| --- | --- |
| `cmake/Sources.cmake` | 共享源清单（见上） |
| `.github/workflows/ci.yml` | 双平台 CI，守住"Windows 仍 0 error"这条验收 |
| `wasm/` | Emscripten 交叉构建脚手架：`CMakeLists.txt`、`build.sh`、`README.md`、`src/`（可移植桩、spike main、布局探针） |
| `docs/porting/` | 本次会话的产物与经验：spike 报告、可移植性清单、路线图、本文件 |
| `PORTING.md` | 根目录入口，指向 `docs/porting/` |
| `.gitignore` | 追加 `/wasm/build/` |

## 未做的事（有意为之）

- 没有实现渲染层、overlay 机制、资源管线。
- `native/SaveStore.cpp`、`NitroThreads.cpp`、`NitroIdentity.cpp` 仍保持上游的 Windows 实现，
  wasm 构建用 `wasm/src/port-stubs.cpp`（约 55 行）替代；
  `native/DssRuntime.cpp` 经复验是**完全可移植**的（只是上游把它放在 exe 目标里），wasm 构建直接编译它。
- `CMakePresets.json` 里仍然只有 `windows-game` 一个预设。wasm 侧走 `wasm/build.sh`，
  加预设的收益不大，而且会让 `cmake --preset` 的语义在两个平台间不一致。
