# 路线图

目标：把游戏真正跑起来（先浏览器/wasm32，再考虑原生移动端）。
每一项给出**验收标准**，避免"看起来做完了"。工作量是量级估计，不是承诺。

## M1 ✅ 可行性 spike（已完成）

- [x] 用 Emscripten 把 `dqiv_game`（553 TU）+ 核心 + NNS + 声音 + 平台层编成 wasm32
- [x] 验证 32 位布局断言（`sizeof(void*)==4`、`DataObject==0x10`、`UnkGameApplication==0xb4`、`OSThread==0xc0`）
- [x] 链接成单个 `.wasm` 并运行真实反编译函数
- [x] 产出 `wasm/` 脚手架与本文档

**验收**：`./wasm/build.sh` 从零构建 0 报错；`node wasm/build/dqiv_wasm_spike.js` 打印出 `arrayToIndex(...) = 3`。

## M2 收敛构建，消除源文件清单重复（0.5–1 天）

`wasm/CMakeLists.txt` 目前复制了上游的源文件清单（`dqiv_core` / `dqiv_game` / `dqiv_sound` / `dqiv_platform` / `dqiv_nitro_system`）。
上游一改文件，这里就会漂移。

- [ ] 把各目标的源文件清单抽成共享的 cmake 片段（例如 `cmake/Sources.cmake` 里的函数/变量），
      根 `CMakeLists.txt` 与 `wasm/CMakeLists.txt` 同时引用
- [ ] 保持上游 Windows 构建产物不变（用一次 `build.bat` 或 CI 验证；本机没有 Windows 时可只做配置层验证）
- [ ] `native/DragonQuestIV.rc`、overlay 生成等 MSVC 专属部分必须能被非 MSVC 工具链安全跳过

**验收**：新增一个上游文件后，只需要在一个地方登记；Windows 构建仍然 0 error。

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

`M2 → M4 → M3 → M5 → M6 → M7 → M8`，M9/M10 视需求另开分支。
M4 先行的理由：它是唯一"看得见进展"的里程碑，也能尽早暴露渲染相关的隐藏假设。
