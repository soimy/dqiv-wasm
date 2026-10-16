# 本 fork 相对上游的改动

- Fork: `soimy/dqiv-wasm`（原 `GoldieLeGenie/DQIV-Recompiled`，改名保留 fork 关系）
- 上游基线：`e1826f9`（`main`）
- `upstream` remote 已配置，同步上游：`git fetch upstream && git merge upstream/main`

## 原则

**上游的 Windows 构建保持逐字节等价，上游合并保持 trivial。**
因此根 `CMakeLists.txt`、`CMakePresets.json`、`build.ps1`、`cmake/NativeOverlays.cmake` 一律未改；
所有跨平台改动都是"在非 Windows 上才生效"的分支，或是纯新增目录。

## 源码改动（3 处，Windows 行为不变）

| 文件 | 改动 | 原因 |
| --- | --- | --- |
| `game/src/ov015/btl/BattleSelectTarget.cpp` | 给三个 `switch` case 加花括号 | case 跳过了变量初始化，MSVC 容忍，Clang/GCC 直接报错。语义完全不变 |
| `native/GameServices.cpp` | `isGameTextPointer()` 的 `VirtualQuery`/线程栈/PE 镜像判断用 `#ifdef _WIN32` 包起来，非 Windows 在分配表检查后返回 `false` | 该启发式依赖 Win32 地址空间；Windows 路径一字未动 |
| `native/Parallel.hpp`（新增）+ `native/Display.cpp`、`native/GpuRenderer.cpp` | 3 处 `std::for_each(std::execution::par, …)` 改用 `DQIV_PARALLEL_FOR_EACH` 宏 | libc++（macOS/iOS/Android/wasm32）没有并行算法实现；宏在 MSVC 上仍是 `std::execution::par`，可用 `DQIV_NO_PARALLEL` 强制串行 |

## 新增目录（纯加法）

| 路径 | 内容 |
| --- | --- |
| `wasm/` | Emscripten 交叉构建脚手架：`CMakeLists.txt`、`build.sh`、`README.md`、`src/`（可移植桩、spike main、布局探针） |
| `docs/porting/` | 本次会话的产物与经验：spike 报告、可移植性清单、路线图、本文件 |
| `PORTING.md` | 根目录入口，指向 `docs/porting/` |
| `.gitignore` | 追加 `/wasm/build/` |

## 未做的事（有意为之）

- 没有合并/重构根 `CMakeLists.txt`：`wasm/CMakeLists.txt` 目前重复了上游的源文件清单，
  这是已知债务，见 `docs/porting/03-ROADMAP.md` 的 M2。
- 没有实现渲染层、overlay 机制、资源管线。
- `native/SaveStore.cpp`、`NitroThreads.cpp`、`NitroIdentity.cpp` 仍保持上游的 Windows 实现，
  wasm 构建用 `wasm/src/port-stubs.cpp`（约 55 行）替代；
  `native/DssRuntime.cpp` 经复验是**完全可移植**的（只是上游把它放在 exe 目标里），wasm 构建直接编译它。
