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
验收：同一帧下 CPU 读到的字节与纹理内容逐块相等。

**B2 2D 合成着色器（3–4 天，主体）**
一个全屏 pass 复刻 `Display::engine` 的合成子集：BG0–3（文本/仿射，4bpp/8bpp，扩展调色板）、
256 个 OBJ、窗口 0/1/OBJ、优先级、半透明、BLDCNT 四种混合、BLDY、马赛克、主亮度。
输出 MRT：`screen` 颜色 + `coverage`（3D 覆盖/优先级/混合码）。**保留 CPU 路径**，用谓词选择：
GPU 不支持的组合（含 `mode==3`、FIFO、捕获源的特殊态）自动落回 CPU。

验收：`?gpucompose=diff` 下两条路对同一帧逐像素比对，城镇/战斗/菜单/标题四场景差异为 0（或
仅已知的 1 LSB）；不支持的组合计数为 0 且落回正确。

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
