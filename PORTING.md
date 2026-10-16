# dqiv-wasm — 把 DQIV-Recompiled 移植到 wasm32 / 移动平台的开发分支

这是 [`GoldieLeGenie/DQIV-Recompiled`](https://github.com/GoldieLeGenie/DQIV-Recompiled)
的 fork（已改名 `dqiv-wasm`），目标是把这个"Dragon Quest IV DS 反编译 + 原生 PC 版"带到
**浏览器（wasm32）**，并在此基础上探索 Android / iOS。

上游本身只支持 **Windows x86 32 位 + MSVC + 桌面 OpenGL 3.3**（见上游 `README.md` 的 Building 一节）。

## 现状

2026 年做了一轮可行性 spike，结论是**可行**，而且比预期干净：

| 目标 | 结果 |
| --- | --- |
| 反编译游戏本体（553 个翻译单元） | **553/553** 编成 wasm32 |
| 核心 + NitroSystem + 声音 + 平台层 | 全部编译通过，共 **679 个目标文件 0 报错** |
| 链接 + 运行 | 单个 **8.5 MB** `.wasm`（Debug），node 下执行真实反编译代码 |
| 需要改动的游戏代码 | 只有 **1 处**（一个 `switch` case 跳过了变量初始化） |

关键原因：上游"资源里有 32 位指针，必须 32 位构建"的硬约束，在 wasm32 上**天然成立**
（wasm32 的指针就是 4 GB 线性内存里的 32 位偏移），所以 263 个指针成员字段、144 个 DS 结构偏移注释
全部保持原样。详见 [`docs/porting/01-SPIKE-REPORT.md`](docs/porting/01-SPIKE-REPORT.md)。

还**没做**的：渲染层（WebGL2/GLES3 后端）、DS overlay 机制、资源管线。见路线图。

**M2 已完成**（2026-10）：源清单不再重复——`cmake/Sources.cmake` 是唯一登记处，根
`CMakeLists.txt` 和 `wasm/CMakeLists.txt` 都引用它。顺带加了双平台 CI（`windows-latest`
真编译上游 Windows 构建，`ubuntu-latest` 跑本文件的快速开始那三条命令并断言输出）。
目标定为**只要浏览器**，所以渲染层走 WebGL2，WebGPU 作为可选后端不排期（见路线图 M4b）。

## 快速开始

```bash
git clone https://github.com/soimy/dqiv-wasm
cd dqiv-wasm

# 需要一个激活过的 emsdk
source /path/to/emsdk/emsdk_env.sh
./wasm/build.sh
node wasm/build/dqiv_wasm_spike.js      # 跑通证明：真实反编译代码在 wasm 里执行
node wasm/build/dqiv_layout_probe.js    # 32 位布局断言
```

Windows 原生构建不受影响，仍按上游说明使用 `build.bat` / `build.ps1`（需要 VS2022 + 自备欧版 ROM）。

## 文档导航

| 文档 | 内容 |
| --- | --- |
| [`docs/porting/01-SPIKE-REPORT.md`](docs/porting/01-SPIKE-REPORT.md) | spike 实测报告：数字、证据、原始输出 |
| [`docs/porting/02-FINDINGS.md`](docs/porting/02-FINDINGS.md) | 可移植性清单与经验：哪里能编、哪里不能、为什么 |
| [`docs/porting/03-ROADMAP.md`](docs/porting/03-ROADMAP.md) | 里程碑与待办（含 overlay 机制和 64 位重构的具体做法） |
| [`docs/porting/04-FORK-DIFF.md`](docs/porting/04-FORK-DIFF.md) | 本 fork 相对上游的改动清单 |
| [`wasm/README.md`](wasm/README.md) | 交叉构建脚手架说明 |

文档为中文，代码注释为英文；需要英文版可以直接翻译，不影响构建。

## 同步上游

```bash
git fetch upstream
git merge upstream/main     # 本 fork 的改动都是"非 Windows 才生效"或纯新增，冲突面很小
```

## 法律

仓库**不含任何游戏数据**。上游的设计就是从使用者自备的正版欧版 ROM（游戏代码 `YIVP`）中
在构建时抽取资源；`wasm/` 的后续计划同样如此（在浏览器里用 File API 读取玩家自己的 ROM）。
请勿把 ROM、`.obb` 或抽取出的 `assets/` 提交进仓库。
