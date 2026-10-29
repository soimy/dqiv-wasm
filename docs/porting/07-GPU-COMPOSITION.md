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
PA–PD 矩阵、28 位参考点、溢出回绕、256 色）、256 个文本 OBJ（两种字符映射、两个引擎、
4bpp/8bpp、翻转、优先级、半透明）、backdrop、BLDCNT 的 alpha/亮度、BLDALPHA、
BLDY、主亮度；由 `?gpucompose` 选择，未覆盖的态（仿射/位图精灵、窗口、马赛克、扩展调色板、
BF 模式 6–7、仿射位图）一律回退 CPU；BG0 画 3D 这条现在也做（把 3D 缩略图当一层：
BG0 的优先级与水平滚动、图顶字节里五位 alpha 的 `blend(...,5)`、透明像素让给下层），
没有 3D 图时才回退；显示模式 2 还有一条直通（B2d，见下：屏幕就是本帧捕获的那张 3D 图时，
采样图本身而不是 VRAM 里的副本）。另有两个态根本不在寄存器里，而是 `Display`
自己的：战斗 band（`setBand`，把底屏名牌挪到屏外）、菜单的世界背景（`setWorldBackground`，
用 3D 目标 0 顶替捕获副本），以及这一帧有没有高清图（`showsHdPictures`，即 `hd_`）。这三条
`composeSupported` 直接问 `Display`，拒绝理由是 `[battle band N]` / `[world background N]` /
`[hd pictures N]`。高清图那条尤其要紧：HD 的图块是在画布上盖上去的，VRAM 里仍然是旧图块，
所以"从 VRAM 合成"会安静地画出旧图——探针逐像素比对其实看不出来（CPU 的 `pixels()` 里也
没有 HD），只有真机画面会不对，属于必须提前拒绝、不能等 CI 发现的那类。
自检 `selfcheck gpu compose: ok (blend 0, plain 0, mode2 0/0, affine16 0, affine8 0, affine-sub 0, obj 0/1827, obj1d 0/554, obj-sub 0/360, bg0-3d 0 of 49152 differ, backdrop3d 0/0, canvas 0 of 196608 differ, graphics 0)`：
构造一个滚动的文本 BG（16 个调色板组、跨块边界的写、透明像素），同一帧分别用 `Display` 与
GPU 合成，两种寄存器配置（有/无混合+亮度）逐像素为 0 差异；仿射三组再覆盖两种地图宽度、
翻转、回绕与两个引擎，各自也是 0 差异。精灵三组：一屏七个对象（压过 BG 的实心 4bpp、
同优先级的半透明、跨屏角回绕、双向翻转、32×32 8bpp、被跳过的双倍尺寸位、同优先级下
较低 id 胜出），1D 字符映射一组，子引擎一组（自己的 `0x06600000`/OAM `0x07000400`/
调色板 `0x300`）。每组都在关掉对象层后再合成一次，两次之差必须非零——`obj 0/1827` 的前一个
数是「与 CPU 的差异」，后一个数是「对象真的画上去了多少像素」，只有前者为 0 的测试会空转。
BG0 画 3D 那一组给的是预乘纹理 + 反预乘后的 ARGB 两份，透明度取 0/128/255 三档，覆盖
透明让位、五位 alpha 混合与不透明直出，并额外钉住一个只可能来自 3D 图的像素（否则整屏
只有 backdrop 也照样「与 CPU 相同」）。backdrop 放 3D 通过那一组不进这串差异计数——它故意
与 `Display` 不同：整屏与 3D 图逐像素相同（透明像素留 backdrop），且同一个屏幕关掉 flag 后
整屏是 backdrop 色，两个数都必须是 0。
过程中抓到的真错：主引擎的 BG 调色板基址是 `0x05000000`（不是 `0x200`，那是 OBJ 调色板）；
着色器输出把 ARGB 写成了 BGR；DISPCNT 的 bit 24–29（字符/地图扩展基址）用 float 传会丢位，
改成低 24 位 + 顶字节分两段上传；引擎 B 的 BG 从 `0x06200000` 起读、四个 128 KB 窗口彼此
相隔 2 MB，而 `vramWindowBases` 原来从 `0x06000000` 起每 128 KB 枚举一档——窗口 1–3
永远是「未映射」，子引擎的合成只会读到 VRAM 0；OAM 纹理原来只建了 512 texel（引擎 A 那一半），
子引擎的精灵永远读不到属性——自检的 `obj-sub` 是红的才发现；精灵的 OAM 也要按
`Display::objects` 的规则读：先看属性 0/1 判断它是否被覆盖，覆盖了才读属性 2。

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
  `affine16 / affine8 / affine-sub 0 of 49152 differ`）；**精灵也已完成**（两种字符映射、
  两个引擎、4bpp/8bpp、翻转、优先级、半透明，自检 `obj 0/1827, obj1d 0/554, obj-sub 0/360`）。
  至此 B2c 的三块都齐了，GPU 合成的覆盖面已经覆盖城镇实机那 2520 帧里出现的全部组合——
  主引擎显示模式 2 占 2389 帧、子引擎 BG 模式 3 仿射 2389 帧、带精灵 131 帧。
  精灵这一段仍是子集：仿射（旋转缩放）精灵、位图精灵、马赛克精灵一律回退 CPU，真机上若某帧
  出现在此列，探针会报 `[affine sprites N]` 之类的更窄理由，而不是笼统的 `[sprites N]`。
  最后一次真机 `?gpucompose=probe` 要确认的是：城镇子引擎 2389 帧从 `[affine background N]`
  变成 `ok`，带精灵的 131 帧从 `[sprites N]` 变成 `ok`；本机没有 ROM，这里只有自检数据。
- **B2d 主屏直通（合成这一半已落地）**：主引擎是 mode 2 时，画面就是捕获进 VRAM 的那张 3D 图。
  `composeScreen(..., capture)` 现在可以直接采样那张图（`uCapture`，一个 256×192 的纹理），
  不再读 VRAM 里已经拷好的副本；捕获本身仍由 CPU 走到 VRAM，所以这里省掉的只是"再读回来"。
  判定在 `Display::showsThreeCapture`：捕获必须取的是 3D 源（DISPCAPCNT bit 24）、mode 0
  （不混）、整屏 256×192、写在 bank 开头，且显示的正是那个 bank——少一条就走 VRAM。
  自检 `mode2 0/0`：前一个是 VRAM 那条路（原样保留，仍与 CPU 逐像素相同），后一个是直通
  （对着已知答案，而不是对着同一帧的 CPU 结果）。为了让这个测试不可能空转，VRAM 里故意放了
  **另一张**图：直通若没生效、或读错了来源，会差 49152 个像素而不是悄悄通过；把 `captureShown`
  临时钉成 `false` 跑一遍，实测就是 49152。
  这一步踩到两件事。其一，捕获在帧尾写 VRAM、下一帧才显示，所以参照必须合成两帧，
  而 `native3d_` 本来就是"晚一帧"的（wasm 上读回只能这样，见 `takeNative` 的注释）；
  其二，为了让合成器拿到**同一张**图，`native_` 改成两个目标轮换（`nativeSlot_`），
  否则这一帧的 `queueNative` 会先把上一帧那张覆盖掉。采样时还要走一遍 `Display::color15`
  的五位量化与反预乘（`unpremultiplyNative` 的倒数表原样搬进着色器），否则每个通道的低位都不同。
  真正省掉 `readNative`/`display-capture` 还要等捕获变惰性（B4）。
- **B4 捕获上 GPU**：捕获 = 把 3D/画布纹理 blit 进捕获纹理；只有 CPU 真去读那段 VRAM
  （DMA、纹理上传）时才惰性读回一次。做完这一步，B2d 才能把整条 `readNative` 去掉。

**B3 画布着色器接上（1 天）**
`present()` 不再传 recipe（每帧 4 个平面、2.9 MB 的上传随之消失）；画布着色器采样
screen0/screen1，面板/内嵌/scale2x/HD/band 的布局逻辑保留。

计划里原来写的是"用 coverage 纹理表达 kind 1/2"，**改成不做了**：只要 `compose2d` 自己也把
"backdrop 放 3D 通过"（`Display::engine` 的 `recipe[1]` 第 3 位，配合 `layout.backdrop3D`）
画进去，合成出来的 screen 就是完整的，画布只剩"把两张 screen 按布局摆上去 + HD 覆盖 + band"，
不需要第二张输出、也不需要 MRT。这样少一张纹理、少一套语义，B3 从"重写画布"缩成"给画布
加一条采样路径"。

两个已知代价，写在这里而不是等着被发现：
1. `compose2d` 能拿到的 3D 只有 256×192 的缩略图（`nativeTexture()`），而画布那条路采样的是
   原始 3D 目标、放大会更清晰。宽屏战斗（backdrop 放 3D 通过）因此会比现在略软；如果真机
   上看得见，再给这条单独传高分辨率纹理，而不是为此保留整套 coverage。
2. band 仍在画布上画（`layout_.band`），不进取景合成——它本来就在屏外。

**B3 的这一半（合成里画 backdrop 3D）已落地**：`composeScreen(..., scene3d, backdrop3D)`
在显示模式 1、没有任何 2D 层盖住、`first==5` 的像素上采样 `uScreen3d`（屏幕自己那一片
256×192），反预乘后**不量化**（画布那条路也不量化），保留 3D 图自身的透明度语义：全透明就
留 backdrop，等同画布的"below"；3D 层是 BLDCNT 的第一目标且淡出/淡入（effect≥2）时再按
`Display` 的六位算术调一次亮度，最后才轮到总亮度。自检 `backdrop3d 0/0`：全层关闭时整屏
等于 3D 图（已知答案，不是 Display 的 recipe——CPU 没有 3D，那里是 backdrop 色），且**不带
flag 时整屏是 backdrop 色**，所以这个用例不可能因为"什么都没画"而通过。

顺带把边界写死：这条只有主引擎、且只有屏幕按 1:1 落在画布上时才对得上（`nativeTexture()`
就是源矩形取的是主屏那半张，见 `queueNative(source)`）。大战斗的 `topScale<1`、以及子引擎
（它的那半场景根本没有缩略图），`composeSupported` 一律以 `backdrop 3D` 回退给 `Display`；
探针的逐像素 diff 对烘焙过的帧跳过——那是**故意的**差异，比了只会天天报假警。

**画布那条采样路径已经有了，并且接上了 `Mode::single`（`?gpucompose=1`）**：
`GpuLayout::composedTop`（0/1，哪一屏在上）
一设，`present()` 就不再上传 4 个 recipe 平面（2.9 MB/帧），画布着色器的 `uComposed` 分支按
`cell` 的行号采样对应的 `screens_[…]`，`pixel()`/`hdOver` 都绕开 recipe。自检是把**同一帧**
用两条路各呈现一次再逐像素比：`canvas 0 of 196608 differ`（recipe 那条先按 presenter 的布局
把 `Display::recipes()` 铺成 256×704×4）。这一组把总亮度关掉了，理由写进代码：recipe 那条是
浮点亮度作用在 8 位色上，`Display` 是六位取整，两者本来就差 1–2 个 LSB（计划里说的"已知 1 LSB"
就是它），而这一组要验的是"哪一屏、哪一行、朝哪边"，不是亮度的舍入；亮度已经由 compose2d
自己的用例逐像素对过。

**但"接上"之前必须先补一件事**：`Mode::split / joined / field / effect` 呈现的不是
`Display::recipes()`（显示引擎的最终输出），而是 `mainGraphicsRecipes()`——**显示模式选择
capture 之前**的图形层（`Display::engine` 里 `graphics_=output` 就在模式 mux 之前）。
城镇就是这种帧：主引擎是显示模式 2 且每帧都在捕获，`FramePresenter` 判成 `Mode::split`。
也就是说，拿"最终显示输出"的合成屏去喂画布，在城镇会画错——模式 2 的最终输出是那张捕获位图，
而画布现在画的是"图形层 + 画布分辨率的 3D"。所以 compose2d 还要一个"图形变体"（跳过模式 mux，
即不做模式 2 的整屏取图），split 用它；`single`（不捕获的那些帧）才用最终输出那版。
另外 split 的记账是"每个屏各自保留自己那张"（`subEngine` 为假的帧不重算子屏），接到纹理上
就是"子屏不重算时保留它的纹理"，`screens_` 本来就一直在；只要把"这一帧有没有更新过"记清楚，
别把上一帧的屏当成这一帧的。

**现在接上的是 `Mode::single` 和 `Mode::split`**（`?gpucompose=1`，默认关）：
`FramePresenter` 在 `display_.frame()` 之后合成，成功就设 `layout_.composed[2]`，`present()`
不再传 recipe、画布直接采样那两张图；失败就退回 recipe，每 120 帧打一行
`compose use: fell back, <理由>`。

为此补了三件事：

1. **`screens_` 从两个目标变成四个**：0/1 是两个引擎自己的图（探针/自检读回来的就是它），
   2/3 是"画布上看到的上屏/下屏"。原来"引擎号 == 屏幕号"只在 `mainTop` 为真时成立，主引擎
   换到下屏时画布会取错图；现在 `composeScreen(..., target)` 明确写进 DS 屏槽位，画布永远
   上屏取 2、下屏取 3（`layout_.composed` 存的是槽位号，split 的两半可能指同一个槽）。
2. **compose2d 的图形变体**（`uGraphics`）：跳过显示模式 2 的整屏取图，其余照旧（模式 1 本来
   就是同一份）。自检把同一帧用"配方"和"合成屏"各呈现一次：`graphics 0`——那一帧故意是
   显示模式 2 + BG0 开着，所以两边的差别正好是"位图 vs 图形层"，图形变体要是没生效会差
   98304 个像素（这组还要求配方那半至少 1000 个非黑像素，空转不了）。
3. **`capture` 从 `scene3d` 里拆出来**成为独立参数：一张图可以喂 BG0-3D / backdrop，而不
   冒充"模式 2 显示的就是它"（那是 `Display::showsThreeCapture` 的判定，只有调用方知道）。

split 的记账照旧：`seen_[0]/seen_[1]` 决定两半各自取哪块槽位（主引擎当前那块，或它上一次
驱动的那块），主引擎的图形只合成进 `2+screen` 一个槽；`screenRecipes_` 在合成成功时不再拷贝。
探针那行现在也带上布局与合成帧数（`compose probe: N frames, split, screens M, main ok …`），
真机跑一次 `?gpucompose=probe` 就能确认城镇到底走 `single` 还是 `split`、以及有多少帧真的
从合成器呈现——这两个数就是 B3 的验收证据。
`joined/field/effect` 仍整条走 recipe（它们要 sides/inset/band，或三段拼起来的布局）。

因此 `?gpucompose=1` 现在只省 `pres` 的一半（拷贝 + 上传；`display_.frame()` 还在跑），
`dr`/`dc` 要等 B4 把 `hd_` 的判定从 `engine()` 里拆出来、能整条跳过 CPU 合成之后才省得掉。

**B4 捕获上 GPU（1–2 天）**
捕获 = 从 3D/画布纹理 blit 进捕获纹理；显示它（LCDC 直读 bank 的模式 2）时采样捕获纹理。
只有 CPU 真去读那段 VRAM（DMA/纹理上传）时才做一次惰性读回。readback 从此消失。

**B4 的第一半已经落地：合成成功的帧不再跑 `Display` 的 CPU 合成。** 既然画布要的那张图
GPU 已经合成好了，`display_.frame()`（`dr`+`dc` ≈ 3.5ms）就是白算的；但它还兼着捕获单元
（游戏会把捕获的 bank 当纹理读回去），所以拆出 `Display::captureOnly()`，这些帧只跑捕获。
两条边界写死：

1. **只要加载过任何 HD 图就整条不跳**（`HdPictures::hdPicturesLoaded()`）。判据不是
   `Display::showsHdPictures()`——那个只看"注册的图块"，不包含 `placeHdPicture` 画上去的那种，
   而后者同样只在 recipe 平面 3 里、不在 VRAM 里；跳了 CPU 合成又没有 recipe，画布就画不出它。
   顺带这一条也修了一个潜在 bug：原来的合成闸门只用 `showsHdPictures()`，place 型 HD 图会漏过去。
2. **捕获源不是 3D 的帧不跳**（`DISPCAPCNT` bit 24 清、且捕获开着）：那种捕获的像素来自
   `graphics_`，也就是 CPU 合成的产物，跳过就没有了。

代价写在明处：合成帧上 `display_.pixels()`/`recipes()` 是旧值，所以探针模式（`probe`/`diff`）
一概不跳；`empty_`（面板布局"另一屏是否空白"）也不再更新——没有诚实的来源，面板就保持上一次
的判断。探针的 diff 另外跳过捕获直通的帧：那条路采样的是本帧的图，而 Display 读的 bank 里是
上一帧捕获的，两者本来就不该相等。

**B4 的后半，第一步也已经落地：捕获本身上了 GPU。** 新增第 10 个着色器 `captureFragment`：
读那张 256×192 的 3D 缩略图（预乘、行从下往上），按 `unpremultiplyNative` 的倒数表反预乘、
按 `Display::color15` 取五位、源 alpha 非零就置位 15，输出成 VRAM 里那两个字节
（小端）。`capturePicture(picture)` 画进 `capture_`，`readCapture()` 只是把结果读回来
（自检用）。

自检 `capture 0`：同一帧里先让 `Display::capture` 用 CPU 写进 bank A（显示模式 2 + 捕获
开、3D 源、整屏、无偏移），再让 GPU 采同一张缩略图画一份，逐字比对 49152 个 16 位字；
并且要求 CPU 那份捕获里至少有 1000 个像素与首像素不同——否则"什么都没写"和"图里全是一个
颜色"会一起通过。

**惰性回填也已经接上了。** `Display::captureDeferrable()` 认那条唯一的路：捕获源是 3D 独立源
（`DISPCAPCNT` bit 29-30 为 0、bit 24 置位）、整屏 256×192、写在整个 bank 的开头、bank 正给
LCDC 用。这种帧里 `Display::deferCapture()` 只登记区域并清 bit 31，`FramePresenter` 先用
`capturePicture(nativeTexture())` 把图渲进捕获纹理。`NitroVideo` 记下这段"已写但字节还在 GPU"，
并在这些东西碰到它时惰性回填：`read` / `readTexture` / `readDisplay` / `lcdcSource`（任何读），
以及**写到同一区域**（先回填再覆盖，程序顺序不变）。同名区域的新捕获直接替换旧的、不触发回填——
旧字节从来没有可被观察的机会，因为任何读都会先把旧的拉回来。

自检 `lazy 0`：用假源（`0x1234+i`）登记一次，CPU 读 `0x06000000` 必须拿到这些字；同一区域再登记
两次，回调一次都不能被调；然后往区域头写两个字节，回调必须恰好被调一次，读回来是"新写的头 +
回填的身子"。这一步不需要 GPU 就能测，所以它不会因为显卡驱动而漏网。

于是合成帧上省掉的现在是：`pres` 的一半（2.9MB 拷贝 + 上传）、`dr`、`dc`、`capture` 的
256×192 循环、以及捕获 bank 那 128KB 的上传（不写 bank，stamp 不变，GPU 那边就不会收到它）。

**还剩的一步**：`readNative` 本身。它现在还在跑，因为 `Display` 的剩下几个消费者（非合成帧的
`engine()`、桌面端的即时读回路径）还要 CPU 像素。等这些帧也只剩 GPU 合成时，它才能整条去掉；
那一步做完，城镇 profile 里 `readNative 1483` 也一起消失。

**B5 收敛（1 天）**
四场景逐像素回归、真机 fps、Windows CI 三绿。

## 5. 不在这轮里
`prep` 1963（3500 三角形 CPU 变换）是独立的一条线：更少的三角形/更省的顶点路径，
与 B 正交，B 落地后若离 60 还差再动它。

## 6. 回退
B1–B4 全部有开关，任一阶段出问题都能退回现路径；CPU 合成代码在 B5 之前不删：
`?gpucompose=1` 用 GPU 合成的屏（当前只有 `Mode::single` 会走，其余帧自动退回），
`?gpucompose=probe` 只统计能接的帧与理由（不改画面），`?gpucompose=diff` 再加上每 60 帧
一次的逐像素比对。
