# 性能 B：把合成搬到 GPU（WebGL2 还是 WebGPU）

> 记录于 2026-10-23。数字全部来自 §15.10 的真机 profile 与代码本身，不是估算。
> 结论先行：**走 WebGL2**，WebGPU 只作为"以后真需要 compute"时的备选。

## 1. 现在的账（15.10，城镇重场景，真机）

```
f 13683  cap 4240  dc 2540  dr 953  r3d 4853  prep 1963  tex 220  pres 1207
```

| 项 | 时间 | 是什么 |
|---|---|---|
| `dr` 953 | dip 前 | `Display::engine` 把 VRAM 窗口读进 `bg_`/`obj_`/调色板（808KB/引擎）并解成 `layers_[5][49152]` |
| `dc` 2540 | | 逐像素合成两个屏幕，产出 `output` + 4 张 recipe plane |
| `capture` 630 | | 捕获单元：逐像素混进 VRAM（`Display::capture`） |
| readback ~1.8 | | `glReadPixels` 取 3D 帧给 `Display` 用（`capturing` 时每帧） |
| `prep` 1963 | | 3500 三角形的 CPU 变换/剔除 |
| `pres` 1207 | | 2.9MB 的 CPU 字节交换 + 4 次 `TexSubImage2D` + 2 次全屏 draw |

真正卡住 60 的是 **`dr`+`dc`+`capture`+readback+`pres` ≈ 7.1ms**（13.7ms 里的一半），
全是"每帧把 2D 图在 CPU 上逐像素做一遍，再整张传上去"。`prep` 是另一条线（见 §5）。

## 2. B 要改的是什么

不是换 API，是换数据流：

```
现在：VRAM/寄存器 ──CPU 逐像素──> 256x192 color + 4x256x704 recipe ──2.9MB/帧──> GPU
以后：VRAM/寄存器 ──一次上传──> 纹理 ──GPU 着色器逐像素──> screen0/screen1 纹理 ──> 画布着色器
```

3D 层本来就已经是 GPU 纹理（`uThree0/1`），只是 `capture` 时被迫读回 CPU；B 之后
**3D 帧不再离开 GPU**，捕获单元改成把纹理 blit 进"捕获纹理"，显示它时直接采样。

预计省下：`dr` 953 + `dc` 2540 + `capture` 630 + readback ~1800 + `pres` 1207 ≈ **7.1ms**，
工作部分从 13.7ms 降到约 6.6ms；浏览器侧那 11ms 里的 2.9MB/帧上传与同步读也一起去掉。
按 15.9 的测量（关 3D → 60fps，说明浏览器/合成本身不是墙），B 之后**重场景应当到 60**。

## 3. 为什么是 WebGL2，不是 WebGPU

| | WebGL2 | WebGPU |
|---|---|---|
| 现有渲染器 | 直接改，3D 路径/HD 图集/呈现全部保留 | 整个重写（上下文、pipeline、bind group、顶点布局、采样器、上传、读回、overlay） |
| 2D 合成要的能力 | 全部具备：MRT（合成色 + 3D 覆盖/优先级两张图）、整数纹理、`texelFetch`、实例化、stencil | 同样具备，但没有任何一条是 WebGL2 缺的 |
| 我们的瓶颈在哪 | CPU 逐像素（`dr`/`dc`）、上传体积（2.9MB）、同步读回 | 同上——**一个都不是 API 开销** |
| 每帧 draw 数 | 2 次全屏 + 场景几十次 | WebGPU 的"低 CPU/次调用开销"在这个量级上测不出来 |
| Windows 端 | 同一份代码，继续共享（本来就在跑） | 要么保留两套后端，要么把 Windows 也迁走 |
| Emscripten 支持 | 现成 | 4.0.13 只有 `emdawnwebgpu` port（`USE_WEBGPU` 已弃用），且要走 SDL 之外的上下文 |
| 风险面 | 只动 2D 合成 + present | 上述 + WGSL 全部着色器 + 浏览器支持矩阵（Safari/Firefox 的 WebGPU 版本差异） |

**工作量差异**：B 本身（2D 合成 + 捕获 + 呈现重构）约 **7–9 天**；走 WebGPU 要在这之上
再加 **8–11 天**的渲染器移植与 WGSL 重写，总计 **16–20 天**，而测得的瓶颈一项都不会因此消失。

WebGPU 真正值得上的场景，是将来要做 compute（比如把 HD 图集的 `scale2x(scale2x())` 从 CPU
挪走，或做后处理）。到那时再评估，且 B 的着色器逻辑可以直接翻成 WGSL——设计不白做。

## 4. 分阶段（每阶段都能单独提交、单独测）

**B1 源数据上纹理（1 天）**
`NitroVideo` 加按 4KB 块的写脏位（现在只有 `revision_`/`textureRevision_`，粒度太粗：3D 每帧
都在写 VRAM，粗粒度永远"脏"）。`GpuRenderer::uploadDisplaySources` 把 `bg_`/`obj_`/两套调色板
按脏块 `TexSubImage2D` 上去（R8UI + RGBA8），无视觉变化。
验收（已落地，`05eb10f`）：`selfcheck compose sources: ok` ——提前面的图案写进 VRAM
（含跨 4KB 块边界的写）、调色板、OAM，再从纹理里 `texelFetch` 出来逐字节比对，走的是
与游戏相同的写入路径；行布局（1024 texel 一行、每块四行）错一个字节就会报出来。
写戳在 `NitroVideo::stampVram`，上传在 `GpuRenderer::uploadDisplaySources`。

**B2 2D 合成着色器（3–4 天，主体）**
一个全屏 pass 复刻 `Display::engine` 的合成子集：BG0–3（文本/仿射，4bpp/8bpp，扩展调色板）、
256 个 OBJ、窗口 0/1/OBJ、优先级、半透明、BLDCNT 四种混合、BLDY、马赛克、主亮度。
输出 MRT：`screen` 颜色 + `coverage`（3D 覆盖/优先级/混合码）。**保留 CPU 路径**，用谓词选择：
GPU 不支持的组合（含 `mode==3`、FIFO、捕获源的特殊态）自动落回 CPU。

验收：`?gpucompose=diff` 下两条路对同一帧逐像素比对，城镇/战斗/菜单/标题四场景差异为 0（或
仅已知的 1 LSB）；不支持的组合计数为 0 且落回正确。

B2 的**第一段已落地**（`compose2dFragment` + `GpuRenderer::composeScreen`）：文本 BG0–3
（4bpp/8bpp、5 种尺寸、优先级、倒置位）、仿射 BG2/BG3（8 位与 16 位图块地图、翻转位、
PA–PD 矩阵、28 位参考点、溢出回绕、256 色）、backdrop、BLDCNT 的 alpha/亮度、BLDALPHA、
BLDY、主亮度；由 `?gpucompose` 选择，未覆盖的态（精灵、窗口、马赛克、扩展调色板、
BF 模式 6–7、仿射位图、BG0 画 3D）一律回退 CPU。
自检 `selfcheck gpu compose: ok (blend 0, plain 0, mode2 0, affine16 0, affine8 0, affine-sub 0 of 49152 differ)`：
构造一个滚动的文本 BG（16 个调色板组、跨块边界的写、透明像素），同一帧分别用 `Display` 与
GPU 合成，两种寄存器配置（有/无混合+亮度）逐像素为 0 差异；仿射三组再覆盖两种地图宽度、
翻转、回绕与两个引擎，各自也是 0 差异。
过程中抓到的真错：主引擎的 BG 调色板基址是 `0x05000000`（不是 `0x200`，那是 OBJ 调色板）；
着色器输出把 ARGB 写成了 BGR；DISPCNT 的 bit 24–29（字符/地图扩展基址）用 float 传会丢位，
改成低 24 位 + 顶字节分两段上传；引擎 B 的 BG 从 `0x06200000` 起读、四个 128 KB 窗口彼此
相隔 2 MB，而 `vramWindowBases` 原来从 `0x06000000` 起每 128 KB 枚举一档——窗口 1–3
永远是「未映射」，子引擎的合成只会读到 VRAM 0。

**先量了一遍 DQ4 自己用什么**（`?gpucompose=probe`，标题画面 120 帧）：

```
compose probe: 120 frames, main ok 0 [display mode 2 119] [background mode 3 1],
                              sub ok 0 [background mode 3 119] [sprites 1]
```

主引擎几乎全程是 **显示模式 2（VRAM 位图显示）** —— 也就是"3D 画面被捕获进 VRAM 再显示"
那条路；子引擎全程是 **BG 模式 3（BG3 仿射）**。因此 B2 的文本 BG 只是很小的一块，
下一段应当先做：显示模式 2（采样 VRAM 位图）、仿射 BG、精灵。这也说明 B4（捕获上 GPU）
不是收尾项，而是主引擎画面本身。

**城镇实机（2520 帧，用户机）证实了同一件事，而且更彻底**：

```
compose probe: 2520 frames, main ok 0 [background mode 3 131] [display mode 2 2389],
                              sub ok 0 [background mode 3 2389] [sprites 131]
frame 15690  r3d-prepare 3017  render3D 6023  display-compose 2800  display-read 997
             display-capture 683  readNative 1483  present 1247
```

主引擎 **95% 的帧是显示模式 2**——屏幕上那幅画就是"3D 帧被读回、捕获进 VRAM、再整屏当位图显示"。
`readNative 1483 + display-capture 683 + display-compose 2800 + display-read 997` 这 ~6ms
全是为这一条服务。子引擎是 BG 模式 3（BG3 仿射），另有 131 帧带精灵。

所以路线要改成：**主屏那条路整条重写**，而不是把 `Display::engine` 一行行搬过去。

- **B2c（新，最高优先）显示模式 2 + 仿射 BG + 精灵**：显示模式 2 已完成（`2d8c66d`，
  自检 `mode2 0 of 49152 differ`；这一段的测试原来把 bank 交回 CPU 可见（0x82）而不是交给
  LCDC，两边都读不到图、黑对黑也能过——现在写完后把 bank A 置 0x80，且位图第一点故意写成
  红色，读回必须真是红的，测试不能再空转）；仿射 BG 也已完成（两种地图宽度、两个引擎，自检
  `affine16 / affine8 / affine-sub 0 of 49152 differ`）。剩下精灵。把 GPU 合成的覆盖面补到
  游戏真正用的组合；另有 131 帧是 BG 模式 3 + 精灵，落在精灵这一块里。
  做完仿射后，城镇子引擎那 2389 帧的拒绝理由应当从 `[affine background N]` 变成 `ok`
  （带精灵的 131 帧仍会落 `[sprites N]`，直到精灵做完）——下一次真机 `?gpucompose=probe`
  要确认这一点；本机没有 ROM，这里只有自检数据。
- **B2d 主屏直通**：主引擎是 mode 2 时，画面就是 3D 帧本身——直接采样 3D 目标（捕获之后则采样
  捕获纹理），`dr`/`dc`/`display-capture`/`readNative` 整条不再需要。
- **B4 捕获上 GPU**：捕获 = 把 3D/画布纹理 blit 进捕获纹理；只有 CPU 真去读那段 VRAM
  （DMA、纹理上传）时才惰性读回一次。

**B3 画布着色器接上（1 天）**
`present()` 不再传 recipe；画布着色器采样 screen0/screen1 + 3D 纹理，面板/内嵌/band/HD 的
布局逻辑保留（配方里 kind 1/2 的语义用 coverage 纹理表达）。

**B4 捕获上 GPU（1–2 天）**
捕获 = 从 3D/画布纹理 blit 进捕获纹理；显示它（LCDC 直读 bank 的模式 2）时采样捕获纹理。
只有 CPU 真去读那段 VRAM（DMA/纹理上传）时才做一次惰性读回。readback 从此消失。

**B5 收敛（1 天）**
四场景逐像素回归、真机 fps、Windows CI 三绿。

## 5. 不在这轮里
`prep` 1963（3500 三角形 CPU 变换）是独立的一条线：更少的三角形/更省的顶点路径，
与 B 正交，B 落地后若离 60 还差再动它。

## 6. 回退
B1–B4 全部有开关（`?gpucompose=0/1/diff`），任一阶段出问题都能退回现路径；CPU 合成代码
在 B5 之前不删。
