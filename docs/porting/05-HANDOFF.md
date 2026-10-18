# 交接（2026-07 会话结束时的落盘）

新会话从这里接手。本文只写**已经量到的东西**；凡是推断都标了依据，凡是量到但
和直觉相反的，都写明了它推翻了哪条早先的判断。

## 一句话状态

wasm 侧已经能跑到游戏自己的第一帧门口，卡在**声音系统的流线程永远等不到请求**，
所以协作调度器永远回不来，游戏画不出任何东西。画布上现在只有自检自己画的那个三角形。

## 分支与提交

- 分支 `wasm`（`origin/main` = 纯上游 `e1826f9`，未动）。
- 最新提交 `5c15a7f`（CI 两个 job 都绿 ✓，763 条 MSVC warning 逐字节和基线一致 ✓）。
- 提交链（新→旧）：`5c15a7f` → `ed621ca`（**windows 挂了**，被 `5c15a7f` 修好）→
  `c3f43ef` → `5d2a062` → `ad4276e` → `d6121c1` → `04418fa` → `6db7de4` → `295bcaf` →
  `290b0a7` → `bee68d3` → `5b0b373` → `4f8bde1` → `d6b7fa4` → `cca46bb` → `a32b275` →
  `22c498e` → `7c68d0f` → `cc41684` → `7d985dd` → `21d5d07` → `d967eec` → `785d870` →
  `cc28a7b` → `8e907dc` → `f9500e0` → `e1826f9`。

### 工作区里未提交的东西（接手第一件事）

三个文件：**1 是行为改动**（一个实验），**2、3 是诊断探针的收尾**。

1. **`wasm/src/port-stubs.cpp`** —— 真正的改动。`OS_PauseThread` 被拆成两半：
   ```cpp
   if (queue) { emscripten_sleep(0); return; }   // 真队列指针 = 有线程在等，让出控制权
   // null = 帧边界本身，pcWasmVCount() 要的就是这个
   ```
   外加 `#include <emscripten.h>`。**已经编好、已经量过，结论是否定**
   （见下面"本次量到的决定性结果"）。
2. `game/src/main/global/GlobalGamePart.cpp` —— 把 `vf0c` 探针的计数上限拆掉
   （`nativeCalls` 删了），因为每个前置调用都确认返回了，上限会藏住答案。
3. `game/src/main/menu/MenuManager.cpp` —— 同样把 `menuManagerCalls` 上限拆掉。

**2 和 3 动了反编译源文件，所以 Windows 是裁判** —— 提交前跑 CI，确认 763 条 warning
逐字节不变。`port-stubs.cpp` 是 wasm-only，不影响 Windows。

## 本次量到的决定性结果：`emscripten_sleep(0)` 没有 unwind

这是刚跑完的一次（`cdp-wide.mjs`，3 次采样、间隔 20s），**三次采样的 journal 逐字节相同**，
所以整条链现在是**完全静止**的，不是在忙循环里转。

- 拆分**之前**：journal 尾部是 `boundary: idle handler → returned → runPortableThreads: 1 threads`
  **永远重复** —— 因为流线程的 `OS_PauseThread(&thread->queue)` 走的是边界函数体，
  边界函数体又调 `runPortableThreads()`，它又跑线程 0，线程 0 又调 `OS_PauseThread`
  → 递归。所以它一直在说话。
- 拆分**之后**：journal 停在 `thread 0 running`，后面**什么都没有**，三个采样都一样。
  所以 `emscripten_sleep(0)` **进去了、没回来**，而且没有任何东西 resume。

**所以"Asyncify 会 unwind、所以 `runPortableThreads()` 能收尾"这个假设被测量否定了。**
原因和之前量到的另一条事实是同一件：`emscripten_set_main_loop(cb, 0, 1)` **实测只触发一次
就停**，因为 `main()` 返回了。sleep unwind 到主循环，而主循环不再驱动，所以没有东西可以
resume 进去。`emscripten_sleep(0)` 让 `main()` 留在循环里 —— **也试过，也是一帧**。

所以这不是"再试一次"的事，是一个设计岔路。三条路，各自的代价我量过/读过：

1. **让 `main()` 真的不返回**（`while(true){ step(); emscripten_sleep(0); }` 之类，
   配合 Asyncify）。之前试过的是"sleep 让 main 留在循环里"，结果还是一帧 —— 所以这条
   路要的是主循环**真的被 RAF 驱动**，不是只把 sleep 塞进 main。
2. **别让没活可做的线程占用这一轮** —— `runPortableThreads()` 在 `func_02076420` 会返回
   NULL（队列表空）时干脆不跑那个线程。这是改调度器语义（游戏的语义是"park 到请求来"），
   所以是设计决定，不是 bug 修复。最便宜，而且不需要 sleep。
3. **让流线程自己可暂停**（把它整个函数标成 Asyncify 可暂停），而不是靠 `OS_PauseThread`。

我倾向 2，因为它是唯一不需要"主循环真的转起来"这条前置条件的，而这条前置条件已经被
两次测量否定了。

## 决定性测量（这是接手时最该知道的事实）

浏览器实测（Chrome + CDP，真墙钟时间，不是 `--dump-dom`）：

- 自检两项都过 ✓：`selfcheck byte order: ok`、
  `selfcheck clip: inside 2436, straddling 1218, outside 0 -> ok`。
- 静态初始化顺序成立 ✓（`Original application initialization` 打印了，说明
  `openResources()` 跑完、`NitroVideo` 在 `UnkDisplay` 的 ctor 之前存在）。
- 协作调度器**工作正常** ✓（拆分**之前**量的）：帧边界反复执行，tick 在推进
  （`375984 → 377241 → 377974`）。拆分**之后**这次 tick 是 `479511 → 481711 → 483701`，
  而且**三个采样逐字节相同** —— 所以调度器现在不转了。这条对比本身就是证据。
- 完整 journal（`slice(0,200)`，窗口必须够大）**拆分之前**的顺序是：
  `byte order ok` → `clip ok` → `Original application initialization` →
  `unkfunc_0207e7e8: entered / clock answered tick 375984` → 第二次 tick 377241 →
  `Scene 11, frame 0` → `nativeStep call 0 currentTask_ set part_id_ 11 nextPartId_ 0` →
  `unkfunc_02058188: vf08 returned` → `vf0c: ENTERED` → `vf0c stage: tick` →
  `unkfunc_0207e7e8: entered / clock answered tick 377974` →
  `boundary: idle handler → returned` → `runPortableThreads: 1 threads` →
  `thread 0 running` → **然后永远只有 boundary 这三行**。
- **拆分之后**（刚量的）整条链一模一样，直到 `runPortableThreads: 1 threads` →
  `thread 0 running` → **然后什么都没有，三个采样都一样**。
- **`thread 0 running` 印了、`thread 0 returned` 没印** —— 所以线程 0 进了自己的函数
  就没回来。**拆分之前**那些 boundary 打印来自**它自己的**
  `OS_PauseThread(&thread->queue)`（当时它走的是边界函数体），不是来自 `pcWasmVCount()`。
- 线程 0 = 声音流线程（`snd_arcstrm.c:585` 的 `func_020771b4`）✓。它的形状是
  `while(TRUE){ OS_PauseThread(&thread->queue); while(TRUE){ OS_LockMutex; request =
  func_02076420(&thread->requestList); if(request==NULL){...break;} ...` ——
  `func_02076420` 空表返回 NULL ✓，所以**没有任何请求被排进队列**，所以它永远停在那。
- **画布像素量到**：`2940x1542 nonblack=2436 sumR=sumG=sumB=567012`，
  `567012 = 2436 × 232` —— 每个亮点都是同一个灰，所以画布上**恰好是自检那个三角形，
  游戏什么都没画**。
- 为什么游戏画不出东西（这条我早就知道）：`render3D` 在
  `if(!fogApplied_[slot&1])return;` 处提前返回 —— 默认 `GxRasterState` 下 fog pass
  根本不跑，`fogged_` 永远不被写。

## 这条链上被测量推翻的判断（别再犯）

1. 我说 "`currentTask_` 是空的" —— **错**。探针说 `currentTask_ set, part_id_ 11,
   nextPartId_ 0`。
2. 我说 "journal 不增长所以停着" —— **不成立**。journal 只在换场景时打印，而且 WebGL
   画布在同一场景会返回同一张图。
3. 我说 "画布开始画东西了"，因为 dataURL 从 `11834 → 119642 → 122966` —— **是 PNG
   编码噪声**，像素数说 `nonblack=2436`，就是自检三角形。
4. 我说 `unkfunc_02052464` 的第一行打印从没出现、而它前面每个调用都确认返回 ——
   我管这叫"自相矛盾"。**原因是我自己的 journal 窗口**：`slice(0,40)` 把它切掉了。
   `slice(0,200)` 能看到整条链。
5. 我说游戏"停在 `MenuManager::execute()` 里面" —— 但 `execute()` 自己的第一行打印
   从没出现，所以 `execute()` 根本没被到达。这条结论要撤回。
6. **"帧边界设一个让守卫过关的值"这个选择被下一次运行推翻了** —— 写 `REG_VCOUNT`
   没能解开 `vf10`。原因是量到的：自旋坐在 `OS_Wait()` **之前**，而帧边界只能通过
   `OS_Wait()` 到达，所以在边界写 VCount 永远不会发生。
   **"让自旋就是帧边界"才是有效的**（`gx.h:430` 的 `#ifdef __EMSCRIPTEN__` 分支）。

## 下一步（确切命令 + 成功判据）

**先拍上面那个设计岔路**（三条路里选一条）。`port-stubs.cpp` 的 yield 拆分已经编好、
已经量过、**结论是否定**（见"本次量到的决定性结果"那节）—— 它现在是一个"量到了、但没让
事情变好"的实验：要么按路 2 改 `runPortableThreads()`、把 sleep 撤掉；要么保留 sleep 并
解决"主循环真的被驱动"这条前置条件。

**改完之后的确切跑法**（都是跑通过的）：
```
source ../emsdk/emsdk_env.sh >/dev/null 2>&1 && cd wasm/build && cmake . && \
  make dqiv_wasm_game 2>&1 | grep -viE "^warning: function signature mismatch|^>>>" | tail -2 && \
  cp ../index.html . && cd /tmp && \
  node cdp-wide.mjs http://localhost:8111/index.html \
    /Users/sym/Repo/dqiv-wasm/ROMandOBBfileHere/DQ4chs.nds \
    "<新的 browser ws>"
```
（`CMakeLists.txt` 或头文件改过就先 `cmake .`；`wasm/build/index.html` 是**副本**，
改 `wasm/index.html` 之后必须 `cp ../index.html .`。）

**成功判据（没变）**：`unkfunc_02052464: ENTERED` 出现 → 然后
`MenuManager::execute stage: entered` → 然后 `frame 1` / `frame 2` →
然后 `MenuPart initialize: start`。

**判据的机理性说法**：`runPortableThreads()` 必须**能返回**，`unkfunc_02052464()` 才能被
到达，`MenuManager::execute()` 才能被到达。现在它返回不了。

**提交前记得**：`GlobalGamePart.cpp` 和 `MenuManager.cpp` 的探针收尾动了反编译源文件，
所以 Windows 是裁判 —— 走 CI，确认 763 条 warning 逐字节不变。`port-stubs.cpp` 是
wasm-only，不影响 Windows。

## 环境（都还活着，接手时先确认）

- **Chrome**：`--remote-debugging-port=9333 --user-data-dir=/tmp/chromeprofile --no-first-run`。
  browser endpoint 从 `http://127.0.0.1:9333/json/version` 读，**每次跑都重新读**
  （endpoint 会在两次运行之间死掉，症状是 `await new Promise(r=>ws.onopen=r)` 挂住 +
  `Detected unsettled top-level await`）。
- **http server**：`(python3 -m http.server 8111 >/tmp/http.log 2>&1 &)` 在 `wasm/build/`，
  现在 200 ✓。
- **`/tmp` 里的工具，都还能用**：
  - `cdp-wide.mjs` —— `slice(0,200)` + 像素计数，3 次采样间隔 20s。**主力。**
  - `cdp-tail.mjs` —— `slice(-40)`，看最新在干什么。
  - `cdp-count.mjs` —— 纯像素计数（`nonblack`/`sumR`/`sumG`/`sumB`）。
  - `cdp-close.mjs` —— 关掉遗留的测试 tab（`Target.closeTarget`）。
  - `cdp-full.mjs` —— `slice(0,40)`，**窗口太小，会切掉关键行**，别用它下结论。
  - `cdp-shot.mjs` —— **不可用**：本模型不能读图（`read_image` 报
    `model does not declare image input`）。截图不是这里的验证路径，从 JS 量才是。
- **`Runtime.evaluate` 之前必须先 `Runtime.enable`**，否则挂住。
- **必须开新 tab**（`Target.createTarget` + `Target.attachToTarget{flatten:true}` +
  每条命令带 `sessionId`）：被卡住的 tab 会毒化后面的运行，`1+1` 都挂。
- **Chrome 会缓存 `index.html`** —— 工具里已经带 `?cb=` cache-buster；改页面后如果
  看不到变化，先怀疑缓存。

## 用户定的两条长期指令

1. **"请使用 chrome dev tools 验证"** —— 需要真 GPU 的东西一律实测，不要假设。
   （这条推翻了我早先"这里没有浏览器自动化"的说法。）
2. **"排查阶段不用每轮都问我方向，直接按你计划和推荐的决策前进，直到有决定性成果。"**

## 待办

- **查谁本该给声音流线程排一个请求** —— 这是游戏自己第一帧的测量到的障碍。
  `func_02076420(&thread->requestList)` 空表返回 NULL，所以没东西排队。
- **定位到停止点之后把诊断探针拆掉**（`GlobalDQ4.cpp`、`GlobalGamePart.cpp`、
  `MenuManager.cpp`、`UnkMenuSystem.cpp`、`MenuPart.cpp`、`DssRuntime.cpp`、
  `port-stubs.cpp`）—— 它们是诊断，不是要发布的行为，而且现在 MSVC 上也会打印。
- **把 `GameInstall.cpp:136` 的 `YIVP` 要求放宽**，这样用户的中文 ROM 就能在他自己
  机器上打包。用户选的是 pack 路线（"你机器提取，浏览器加载"），所以这条现在是必需的。
  一行的事，等他拍板。
- **M3b** —— 真正的 snapshot/restore，用**两遍链接**（用户选的）：先带 `--Map` 链接、
  读地址、用读到的地址生成表、再链接一次。`--Map=` 已经在 `wasm/CMakeLists.txt` 里 ✓。
- **把自检钉成 CI 门禁** —— byte order 和 clip 现在都是真过的，headless Chrome + CDP
  可以进 CI。提过几次，还没拍板。
- `TextCanvas` 在 wasm 上：FPS overlay 保住矩形、丢掉文字。挂在它下面的开放决定：
  一个便携光栅器（stb_truetype + 自带字体）。
- `settings.autoQuality` 在 wasm 上是空操作（GPU-timing 路径被 `#ifdef` 掉了）——
  接受的代价，路线图里写了。
- `SDL_DelayPrecise` 在没有 pthreads 时是忙等 → RAF 是真正的节拍器；`pace()` 的等待
  在烧 CPU。`interpolating()` 在 wasm 上几乎不触发（emscripten display mode 的
  `refresh_rate` 是 0，所以 `> 61` 永远 false）。
- `index.html` + `.wasm` + `.data` 出标题画面 —— 还没做。
- **`03-ROADMAP.md` 和 `04-FORK-DIFF.md` 都欠更新**：签名批次、M5 路线、Asyncify 决定、
  MEMFS/heap 测量、Chrome 缓存、新 tab 要求、VCount 缺口、M5 浏览器侧已完成、
  协作调度器可用、画布只有自检三角形、capture 线程被移除、`--Map`。
  建议顺序写成 `M2(✅) → M4(大部分✅) → M3a(✅) → 签名(✅) → M5(浏览器侧✅) →
  M6/M7(现在卡这里，声音系统的线程) → M3b → ...`。

## 关键约束与已验证事实（省得重新推导）

- **硬约束：32 位指针**（263 个指针字段、144 处 DS 偏移注释）。wasm32 天然满足，
  原生 64 位不满足（M9）。
- Emscripten 4.0.13 pin 在 `/Users/sym/Repo/emsdk`；node v22；`glslang` 16.6.0
  （`/opt/homebrew/bin/glslangValidator`，**不需要 profile flag** —— `-P` 是"注入
  preamble"，不是"OpenGL ES profile"）。
- **WebGL2 就是 GLES 3.0** —— 要桌面 3.3 core 会让 SDL 在浏览器里建不出上下文。
  `MAX_WEBGL_VERSION` 默认是 **1**，SDL 只在请求的 GL major 是 3 时才把上下文抬到
  WebGL2，所以 `-sMAX_WEBGL_VERSION=2` 是强制的。
- **Asyncify 是解开 128MB ROM 的那一把钥匙** ✓。它和单线程协作调度兼容（没有
  SharedArrayBuffer、没有 COOP/COEP）。代价写在 CMake 注释里：它插桩每一个可暂停
  函数，所以**整个游戏**都会变慢。128MB ROM 在主线程上五种形状全部卡死，
  对照实验：1MB 文件每次 poll 都能回答 `1+1`。所以是大小，不是代码。
  Asyncify 也是让停下的线程能 yield 的原因 —— `emscripten_sleep(0)` unwind 线程栈。
- **`emscripten_set_main_loop(cb, 0, 1)` 实测只触发一次就停**，因为 `main()` 返回了。
  `emscripten_sleep(0)` 让 `main()` 留在循环里 —— **也试过，也是一帧**。
- **内存**：`INITIAL_MEMORY=64MB` + `ALLOW_MEMORY_GROWTH=1` 是量出来的可用值
  （320MB 实测更糟；页面会完全不响应）。
- **`MEMFS 的读会复制整个文件** —— 所以逐文件读大源文件是 O(文件数 × 文件大小)。
- **clang 在 wasm 上忽略** `#pragma init_seg` / `data_seg` / `bss_seg` /
  `__attribute__((init_priority))`（实测 `-Wignored-attributes`）。`__declspec(allocate)`
  是 MSVC-only。`wasm-ld` **没有 section 排序选项**。clang 给的两个杠杆（都量过）：
  链接顺序决定静态初始化顺序（**exe 的 object 排在 library object 之前**），
  以及 `__attribute__((section(".d38a")))` 真的会造出那个 section。这就是 M3a。
- **静态初始化顺序不是巧合，是游戏依赖的机制**：`CMakeLists.txt:91` 给每个 `/main/`
  源文件（包括声明全局 `UnkDisplay data_0211a7d0` 的 `UnkDisplay.cpp`）一个 MSVC 的
  `#pragma init_seg(".i38m", pcOverlayExit38)` 头，**压制 CRT 启动构造**，所以游戏的
  ctor 在 overlay registry 于 `main()` 内加载 overlay 38 时才跑。wasm 上靠链接顺序。
- **`0x502` 是 `GL_INVALID_OPERATION` (1282)**，在 WebGL 里就是 feedback loop 的码。
  `GL_INVALID_FRAMEBUFFER_OPERATION` 是 0x505。
- **`GlLoader` 52 项里恰好两项在 WebGL2 上不存在：`glMapBufferRange` 和
  `glUnmapBuffer`** ✓（Emscripten 实现了前者但直接拒绝 `MAP_READ`；`glGetBufferSubData`
  完全没实现）。桌面专属的三项在 `#ifndef __EMSCRIPTEN__` 里 —— `GetQueryObjectui64v`、
  `MapBufferRange`、`UnmapBuffer`。**`GlLoader.cpp` 的 define 段和头文件的 declare 段
  (`DQIV_GL_DECLARE`) 都要调用两张表**，忘了 declare 段会崩 MSVC（CI 38922160137 就是这么挂的）。
- **`GpuRenderer.cpp` 里有一处真 bug 修复（保留）**：draw loop 里
  `gl::BindTexture(GL_TEXTURE_2D,first.texture);` 现在**无条件**执行，从
  `if(first.texture)` 里提了出来。
- **`readPixels` 从 JS 里量游戏自己的 WebGL2 context 是可行的** ✓ ——
  `document.getElementById("canvas").getContext("webgl2")` 返回同一个 context。
  这是本环境的像素测量路径。
- **`std::cout` 在 `dqiv_game` 里能到 journal** ✓ —— 所以"探针没打印"是"代码没跑"
  的证据，不是"打印坏了"的证据。journal 路径 `/saves/journal.txt`，`main()` 把 stdout
  和 stderr 都 `freopen` 进去。
- **MSVC 要求任何放在反编译源或共享头里的探针也能在 MSVC 上编译** ——
  `#ifdef __EMSCRIPTEN__` 包住 `<iostream>` 但不包住打印本身，就是编译错误
  （CI 39084927697 就是这么挂的）。最省事的正确做法是让探针无条件编译。
- **`sed`/正则剥掉列 0 的 `#ifdef` 会把缩进的 `#endif` 变成孤儿** ——
  `error: #endif without #if`。数配对，别假设缩进对得上。
- **签名不匹配 13 处全部修完，重链接 0 warning ✓**（`5d2a062`，CI 绿）。13 处里 3 处是
  **参数个数**不匹配，那是会困住 wasm-ld thunk 的那一类。安全事实：**13 个函数的调用点
  全部丢弃返回值**（都是裸语句 `func_XXXX(a);`），所以"把声明向定义对齐"在 MSVC 上
  行为完全一致。规律：**有函数体的文件（`.c` 定义）几乎总是对的，错的都是声明表**
  （`nnsys/*.hpp`、`DssCore.hpp`、`snd_internal.h`、`.c` 里的 `extern`）—— 那些是从
  调用点猜出来的。
- **`NitroSymbols` 的 wrapper 必须留着，包在 `#ifdef _MSC_VER` 里** ✓。MSVC 的 mangling
  和 Itanium ABI 不同（`?unkfunc_02068ec8@@YAXH@Z` vs `_Z16unkfunc_02068ec8i`），所以
  MSVC 需要 adapter、wasm 上会撞。我早先用 clang 测 mangling，所以结论错了，CI 推翻的。
- **`-sDISABLE_EXCEPTION_CATCHING=0` 是必需的**（Emscripten 默认关掉异常捕获）。
- **`CrashHandler` 的 `divideLikeTheDs` 实现 DS 软件除法语义**（`x/0` 给 `x`、`x%0` 给 `0`），
  靠抓 x86 除零异常；游戏依赖它。wasm 上整数除零会 trap 且实例消失。
- **`assetHash` 是纯 FNV-1a**（`AssetPack.cpp:33`）—— 同字节 → 同 hash；它检测损坏/截断，
  不是真实性签名。`romGameCode()` 校验 **header CRC-16**（0x000–0x15d）✓，所以它是可靠的
  "这是不是一张合法 DS ROM"判别器，不会把 pack 认成 ROM。
- **`GameInstall.cpp` 的 `extractRom` 仍然要求 `YIVP`**（欧洲版）；`extractAnyRom` 加过、
  测过、发现会阻塞、**已删**（死代码）。`romRanges()` 对 ROM 源把 hash 留 0，注释写明
  "没有校验和是一个量到的决定"。`Assets::read()` 在 hash 为 0 时**跳过校验**（量到的决定，
  已声明）。但校验和检查是活的 ✓ —— 实测：hash 留 0 时游戏在 ROM 里找到 `bg.bgc`、
  读了、然后因校验和拒绝它。
- **用户的 ROM**：`/Users/sym/Repo/dqiv-wasm/ROMandOBBfileHere/DQ4chs.nds` —— 128MB、
  game code `YIVJ`（中文版），被 `.gitignore:4:/ROMandOBBfileHere/*` 忽略。
  `extractRom` 只认欧洲版；我新加的路径接受任何合法 DS ROM。
- **`index.html`**：`<canvas id="canvas" tabindex="0">` + `#log` 底栏 + `#pick`
  （`<input type="file" id="rom">` + `<a href="#" id="norom">`）。`DOM.setFileInputFiles`
  headless 免对话框可用 ✓ —— 所以测试走的是人走的那条路。页面自己掌握启动：
  `window.Module={preRun:[...]}` 在注入 script 之前设好，**preRun 回调跑在 `main()` 之前**，
  这正是"数据文件先进虚拟文件系统"需要的保证。MEMFS 不跨 reload 存活；IDBFS 故意没用
  （要 COOP/COEP）。
- **`GX_GetVCount()`（`game/libs/NitroSDK/include/nitro/gx.h:430`）现在有
  `#ifdef __EMSCRIPTEN__` 分支**，本地声明 `int pcWasmVCount(void);`（不 include
  `nitro/os/thread.h`，那会成环），返回 `pcWasmVCount()`。**这是共享头 → Windows 是裁判。**
  注释记录：自旋在 `OS_Wait()` 之前、帧边界只能通过 `OS_Wait()` 到达，所以在单线程协作
  调度器上，对硬件计数器的 spin-wait **必须就是**帧边界；而且游戏的 VCount 计时
  （`s_swapLine/s_drawLine/s_frameEndLine/s_endLine`）不再真的是扫描线。
- **`OS_PauseThread` 就是帧边界**，因为游戏本来就有显式的等帧点（`setWaitCallback` →
  `OS_PauseThread`）。这也是主线程被阻塞就是致命的原因 —— 没有别的线程能接手。
- **两个 portable thread 是声音系统的** ✓：`snd_arcstrm.c:585`（`func_020771b4`，流播放）
  和 `snd_capture.c:80`（`func_020744d0`，capture）。capture 现在在 wasm 上**不启动** ✓
  （`UnkSoundSystem.cpp` 里 `func_020741d0(3)` 包了 `#ifndef __EMSCRIPTEN__`），所以
  `runPortableThreads: 1 threads`。它等的消息来自 DS 声音采集硬件，浏览器没有 ✓。
- **`unkfunc_02081254()` 是 `return data_0211a7d0.frame_`**（`UnkDisplay.cpp:137`）——
  一行 return，不可能阻塞 ✓。`unkfunc_0207e7e8()` 是 `func_02079bf0()`
  （`native/DssRuntime.cpp:18`）—— 真 `std::chrono::steady_clock`，`denominator=64000000000ull`，
  所以它返回 ✓。
- **`nextPartId_ = 0` 是 `MENU_PART`，它确实注册了** ✓，所以 `nativeStep()` 不会 throw，
  和 `main()` 的 catch 保持沉默一致。`MenuPart::initialize()` **从未被调用** ✓。
- **`REG_VCOUNT` 在 wasm 上是只读的 —— 没人写它** ✓。
- **`render3D` 在 `if(!fogApplied_[slot&1])return;` 处提前返回**（无 fog 时）—— 所以
  默认 `GxRasterState` 下 fog pass 不跑、`fogged_` 不被写。这也是自检的计数依赖未定义
  纹理内容的原因。
- **`GpuRenderer::present` 要求 `recipes.size() == 256*704*4` (720896)**，否则 throw。
  `GxLayout::recipeRows=704`（两屏各 384 + inset 192）。`supersample()` 返回 4（默认
  `supersample=4, textureScale=4`）→ 场景 1024x768。
- **763 条 MSVC warning 是反编译源的历史噪声**（集中在 `MenuTemplate_materiel.cpp` ×347、
  `MenuDataCommon.cpp` ×77）—— 从 M2 起每个提交都逐字节一致，包括 `5c15a7f` ✓。
  **任何动到反编译源或共享头的改动，Windows 是裁判。**

## 常用命令（都是跑通过的）

```bash
# 重编 + 跑浏览器（node 那步可以丢后台 60s，用 job_output wait:true 收）
source ../emsdk/emsdk_env.sh >/dev/null 2>&1 && cd wasm/build && cmake . && \
  make dqiv_wasm_game 2>&1 | grep -viE "^warning: function signature mismatch|^>>>" | tail -2 && \
  cp ../index.html . && cd /tmp && node cdp-wide.mjs http://localhost:8111/index.html \
    /Users/sym/Repo/dqiv-wasm/ROMandOBBfileHere/DQ4chs.nds "<browser ws>"

# 单文件编译检查
em++ -Inative -Inative/native/compat -Igame/include -Igame/libs/NitroSDK/include \
  -Igame/libs/NitroSystem/include -DDQIV_NATIVE=1 -Wno-c++11-narrowing -std=c++20 \
  -sUSE_SDL=3 -c <file> -o /tmp/x.o

# CI（必须 -R soimy/dqiv-wasm，否则 gh 解析到上游、返回 []）
gh run list -R soimy/dqiv-wasm --limit 5 --json databaseId,headSha,status,conclusion
gh run watch <id> -R soimy/dqiv-wasm --exit-status          # 后台跑，job_output 收
gh run view <id> -R soimy/dqiv-wasm --log-failed
gh run view <id> -R soimy/dqiv-wasm --log | grep -oE "warning C[0-9]+" | sort | uniq -c
```

## 工具层的坑（都踩过，别再踩）

- `run_in_background`/`job_output` 拒绝字符串 `"True"` —— 必须是 JSON boolean。
- `cmake -P` 拒绝 `CONFIGURE_DEPENDS` → 验证 glob 要用真 configure。
- `ask_user_question` 每个 question 要 `id`、每个 option 要 `label`；JSON 里多一个 `"`
  就报 `invalid arguments: "questions" must be an array`（踩过两次）。
- `write` 工具**替换整个文件** —— 我曾把 `GameBoot.cpp` 写成只剩 `main()`，靠
  `git show HEAD:native/GameBoot.cpp` 找回前 95 行。
- `edit` 说 "file has not been read" —— 用 `bash cat` 读不算，必须用 `read` 工具。
- `ar rcs` 在 macOS 上不能归档 wasm object → 用 **`emar`**。
- `wasm-ld` 不在 PATH 上，在 `/Users/sym/Repo/emsdk/upstream/bin/wasm-ld`；
  `-Wl--map-file=` 是错的，正确写法是 **`-Wl,-Map,file`** / `--Map=` ✓。
- `grep` 把以 `->` 开头的 pattern 当选项 → 用真 tab 锚定，如 `grep $'^windows\tBuild\t'`。
- `sed -i ''` 批量重命名会造出**重复声明** → 一次改一处。
- `race(p, ms, label)` 三个参数，`s(method, params)` 两个 —— 把额外参数塞给 `s(` 会
  `SyntaxError: missing ) after argument list`。
- `Runtime.evaluate` 之前必须 `Runtime.enable`。
- `Page.navigate` 到 `about:blank` 再 `Runtime.evaluate` 会丢掉注入的 handler，用
  `Page.addImageToEvaluateOnNewDocument`（原文：`Page.addScriptToEvaluateOnNewDocument`）。
- `--dump-dom` + `--virtual-time-budget` 在主循环跑起来后**会挂** → 用 CDP + 真时间。
- `UTF8ToString(FS.readFile(...))` 会 `Aborted` —— `FS.readFile` 返回 `Uint8Array`，
  用 `new TextDecoder().decode(...)`。
- `EM_LOG_STACK_TRACE` 不存在；正确 flag 是 `EM_LOG_C_STACK` (8)，配
  `emscripten_get_callstack(int flags, char* out, int maxbytes)`。
- macOS `cat -A` → `illegal option`，用 `od -c` 看精确空白。
- `python3 -c "import yaml"` → `ModuleNotFoundError`，没有 YAML 校验器。
- `check-shaders.py`：`-P` 会把文件名当 preamble 吃掉 → 去掉；GLSL ES 3.00 里
  `isampler2D` **没有默认 precision qualifier** → 加 `precision highp isampler2D;`；
  header 从源码里解析两份，别硬编码。**现在 16/16 ✓。**
- namespace 作用域的 `const` 在 C++ 里是**内部链接** —— 跨 TU 必须 `extern`。
- 匿名 namespace 里的 `gpuTrace` 是内部链接 → 在 `GpuRenderer.hpp` 的 `namespace pc` 里
  `extern bool gpuTrace;`，cpp 里定义在匿名 namespace **外面**。
