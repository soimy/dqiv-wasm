# wasm32 可行性 spike 报告

- 日期：2026 年（本机 macOS 26.6.2 / Apple Silicon）
- 工具链：Emscripten 4.0.13
- 上游基线：`GoldieLeGenie/DQIV-Recompiled` `e1826f95`（`main`）
- 产物：`wasm/`（见 `wasm/README.md`），补丁见 `04-FORK-DIFF.md`
- 本文数字为 fork 里 `./wasm/build.sh` 从零构建复现所得（Debug 构建）

## 结论

**可以把整个反编译游戏编成 wasm32 并链接运行——已用证据确认。**
上游"资源里有 32 位指针，必须 32 位构建"的硬约束，在 wasm32 上**天然满足**，不需要改动任何游戏逻辑。

## 实测结果

| 目标 | 结果 |
| --- | --- |
| `dqiv_core`（PC 中性核心：DS GPU/VRAM/文件/设置） | **50/50** 编译通过 |
| `dqiv_game`（反编译游戏本体 553 个 TU） | **553/553** 编译通过 |
| `dqiv_nitro_system`（NitroSystem 库） | **53/53** 编译通过 |
| `dqiv_sound`（NitroSDK 声音） | **10/10** 编译通过 |
| `dqiv_platform`（平台层，排除 3 个 Windows 文件，含可移植的 `DssRuntime.cpp`） | **8/8** 编译通过 |
| `dqiv_portable` + `dqiv_vorbis` | **1/1** + **1/1** |
| 合计 | **679 个目标文件，0 报错** |
| `wasm-ld -r` 全量合并 | 成功，19 MB 合并对象 |
| **真实链接 + 运行** | 生成 8.5 MB `.wasm`（Debug），node 下运行成功 |

运行输出（真实反编译代码在 wasm 里执行）：

```
dqiv-wasm: sizeof(void*)=4
dqiv-wasm: decompiled dss::arrayToIndex([1,1,1,1], value=3, max=4) = 3 (expect 3)
dqiv-wasm: live game heap allocations = 0
dqiv-wasm: tick counter (unkfunc_0207e7e8) = 2131      # 真实实现（DssRuntime.cpp），值随运行变化
```

### 布局断言在 wasm32 下逐条成立（未改一行游戏代码）

```
sizeof(void*)=4   DataObject=0x10   UnkGameApplication=0xb4   OSThread=0xc0
FS_File=0x48  offsetof(FS_File,currentPosition)=0x2c
SequencePlayerParams=32 (offsetof tempo=24)   SequenceTrackParams=32
```

## 关键发现

1. **上游的 32 位假设在 wasm32 下自动成立。**
   `game/include` 里 263 个指针成员字段（其中 144 个带 `// 0x..` 的 DS 结构偏移注释）全部保持原布局。
   原因：wasm32 的"指针"是 4 GB 线性内存里的 32 位偏移，正好复刻 DS / 32 位进程的指针值空间——
   也就是 `GameServices::isGameTextPointer()` 那种"整数里编码指针"的旧写法所依赖的前提。
   （对比：macOS arm64 前 8 GB 地址空间无法 mmap，见 `02-FINDINGS.md`，所以原生 64 位拿不到这个红利。）
2. **Windows 依赖面极小且集中在边缘。** 真正 Windows-only 的只有 3 个文件
   （`SaveStore.cpp`、`NitroThreads.cpp`、`NitroIdentity.cpp`），约 55 行可移植桩即可补齐，随后就能链接运行。
   `DssRuntime.cpp`（上游放在 exe 目标里，一度被误判为平台层）复验后是完全可移植的，已直接编译。
3. **唯一的真实 C++ 可移植性 bug**：`game/src/ov015/btl/BattleSelectTarget.cpp` 的 switch case 跳过了变量初始化
   （MSVC 容忍，Clang/GCC 拒绝）。加花括号即可。
   553 个反编译 TU 里**只有这 1 个**需要改。
4. **libc++ 没有并行算法**：3 处 `std::execution::par`（`Display.cpp` ×1、`GpuRenderer.cpp` ×2）。
4b. **Clang 的窄化诊断**：反编译产物里有 **234 处 / 28 个文件**把 `-1` 之类的常量塞进 `unsigned char` 字段
   （`-Wc++11-narrowing`，MSVC 容忍）。这些值与原 ARM 代码一致、是刻意的哨兵，
   因此在 `wasm/CMakeLists.txt` 里关闭该诊断，而不是去改 234 行反编译代码。
5. **overlay 机制是 MSVC 专属，且集中在两处**：`cmake/NativeOverlays.cmake` 用 `/FI` 注入
   `#pragma init_seg / data_seg / bss_seg / section` + `__declspec(allocate(".d$NNa"))` 来界定每个 DS overlay 的可写数据区间。
   运行期（`NitroOverlays.cpp`：快照/恢复 + 构造/析构列表）本身可移植。详见 `02-FINDINGS.md`。
6. **wasm-ld 额外暴露 11 处函数原型不一致**（`func_02061228`、`func_02062040`、`func_0206e154`、`func_020732a0`、
   `func_0207343c`、`func_020738a4`、`func_02075780`、`func_02075aa4` 等）。原生链接器不检查，wasm 以警告报出；
   不影响链接，但移植时值得清理。

## 复现

```bash
source /path/to/emsdk/emsdk_env.sh
./wasm/build.sh
node wasm/build/dqiv_wasm_spike.js
node wasm/build/dqiv_layout_probe.js
```

首次配置会下载 `stb_vorbis.c`（校验和固定）并触发 Emscripten 的 SDL3 端口构建。
