# Kiro 吉祥物手工出图操作手册

把 `lcd_showcase` 角落吉祥物从 CodeTank 浣熊换成 **Kiro 幽灵**的**逐步操作手册**。产物是
RGB565 RLE 头文件，落到 `firmware/feature-probes/lcd_showcase/include/assets/generated/`，
换进固件后 `tos.py build` 即生效。

管线脚手架（manifest / 调色板 / 提示词 / 打包脚本）在：
`firmware/feature-probes/lcd_showcase/tools/kiro_mascot/`

> 全流程无需真机，只在最后一步（真机观感）才需要板子。EC2 上只保证出图→打包→编译通过。

---

## 0. 你要产出什么

- **18 张源精灵表 PNG**（`idle.png` / `thinking.png` / `typing.png` … 见下表），每张是一个
  动作的多帧网格图，放到 `tools/kiro_mascot/source/`。
- `mini_mascot` 由 `walking` 自动派生，**不用**单独出图。
- 跑 `mascot_pipeline.py all` 后自动生成 `sprite_mascot_*.h` + `sprite_mascot_registry.h`，
  覆盖当前的浣熊精灵。

18 个动作与网格（取自 `manifest.json`，可先照此，出图后按需微调 `maxBox`）：

| 动作 | 网格 grid | 帧数 | 动作 | 网格 grid | 帧数 |
|---|---|---|---|---|---|
| idle | 4×4 | 16 | dizzy | 4×2 | 8 |
| alert | 4×2 | 8 | sweeping | 4×2 | 8 |
| happy | 4×2 | 8 | walking | 4×2 | 8 |
| sleeping | 4×3 | 12 | going_away | 4×2 | 8 |
| disconnected | 4×4 | 16 | debugger | 4×3 | 12 |
| thinking | 4×2 | 8 | wizard | 4×4 | 16 |
| typing | 4×2 | 8 | conducting | 4×2 | 8 |
| juggling | 4×2 | 8 | beacon | 4×4 | 16 |
| building | 4×2 | 8 | | | |

> `grid=[列,行]`，例如 `idle` 是 4 列 × 4 行 = 16 帧，按**从左到右、从上到下**排列。

---

## 1. 环境准备

```bash
python3 -m pip install Pillow
cd /data/projects/tuya-powered-agentic-keyboard-for-kiro/firmware/feature-probes/lcd_showcase/tools/kiro_mascot
python3 mascot_pipeline.py --help          # 确认脚本能跑
```

---

## 2. 定角色（canonical reference）

Kiro 幽灵形象：**圆润的白→紫渐变幽灵身，平滑的圆顶、底部柔和波浪裙边，两只发光的青色 visor
眼睛，柔和的紫/青光晕；无手脚（个别动作可给一丝手的暗示）。** 参考轮廓见
`ref_projects/ki-board/assets/AWS_KIRO_Ghost_Outline.svg`。

先出一张 **canonical 角色总表**（8 姿势，4 列×2 行），作为后续所有动作的一致性锚。提示词从
`prompts/prompts.json` 的 `canonical` 段取，拼成：

> Pixel-art character reference sheet. Subject: the same AWS Kiro ghost — a rounded
> white-to-violet ghost with a smooth domed top, a gently scalloped wavy bottom hem,
> two glowing cyan visor eyes set close together, and a soft violet/cyan aura; no arms
> or legs. Layout: eight evenly spaced full-body poses in a strict 4 columns by 2 rows
> layout (front, three-quarter, side, back, happy, thinking, floating-still, drifting).
> Style: crisp hand-authored 16-bit-era pixel art, hard pixel clusters, limited palette,
> no antialiasing. Background: perfectly uniform #00ff00 chroma-key, no shadow, gradient,
> texture, floor, text, labels, watermark, border, grid lines, or green pixels on the ghost.

出图工具用 **ChatGPT Image**（或任意文生图工具）均可，关键三条：**纯 #00ff00 绿幕背景 +
16-bit 硬像素风 + 同一角色**。存为 `canonical-reference.png`（自己留档，非必须进管线）。

---

## 3. 逐动作出精灵表

每个动作出一张网格精灵表。提示词 = `prompts/prompts.json` 的
`actionTemplate`（identity/layout/background/constraints）+ 该动作在 `actions{}` 里的一句
描述，并说明网格布局。以 **typing（4 列×2 行=8 帧）** 为例：

> Using the Kiro ghost from the reference (preserve exact silhouette, cyan visor eyes,
> violet body, cyan glow, scale and palette): produce an 8-frame animation sprite sheet
> in a strict 4 columns by 2 rows grid, frames left-to-right then top-to-bottom, one
> complete mascot per equal cell with a consistent bottom-hem baseline.
> Action: hover behind one compact dark keyboard, faint hands alternate rapidly and the
> visor pulses tiny cyan indicator pixels.
> Style: crisp 16-bit pixel art, hard pixel clusters, no antialiasing.
> Background: perfectly uniform #00ff00, no shadow/gradient/text/labels/watermark/border/
> grid lines, no green pixels on the ghost. Do not crop any body part; keep every frame
> fully inside its cell with a small margin.

再以 **idle（4×4=16 帧）** 为例，把网格改成 "16-frame … 4 columns by 4 rows"，动作句换成
idle 的描述。其余 16 个动作同理，逐一从 `prompts.json` 取动作句替换即可。

出图要点（决定后续抠图/切帧成败）：
- **每帧完整居中、留边**，不要贴格子边（脚本会检查内容触边）。
- **底部裙边保持一致基线**（脚本按 baseline 归一）。
- 背景必须**纯绿 #00ff00**，幽灵身上别出现绿色像素。
- 帧数、行列**严格对齐上表**，否则脚本报"帧数不符"。

---

## 4. 放置源图

出好的 PNG 按动作命名，放进 `source/`：

```
tools/kiro_mascot/source/
├── idle.png          ├── dizzy.png
├── alert.png         ├── sweeping.png
├── happy.png         ├── walking.png
├── sleeping.png      ├── going_away.png
├── disconnected.png  ├── debugger.png
├── thinking.png      ├── wizard.png
├── typing.png        ├── conducting.png
├── juggling.png      └── beacon.png
├── building.png
├── confused.png
```

先校验齐不齐（缺哪张会清晰列出）：

```bash
python3 mascot_pipeline.py validate
```

---

## 5. 跑管线

```bash
cd firmware/feature-probes/lcd_showcase/tools/kiro_mascot
python3 mascot_pipeline.py all
```

`all` 依次做：
1. **process**：切帧 → 抠绿幕（含暗绿边）→ 按 baseline 归一缩放 → 套 32 色调色板 →
   帧存到 `build/mascot/frames/<action>/`。
2. **preview**：每动作生成一张预览 GIF + 一张 gallery.png，存 `build/mascot/previews/`。
3. **pack**：RGB565 编码 + RLE 压缩 + round-trip 校验 → 写 `sprite_mascot_*.h` 与
   `sprite_mascot_registry.h` 到 `../../include/assets/generated/`（覆盖浣熊）。

---

## 6. 检查预览

打开 `build/mascot/previews/gallery.png` 与各 `*.gif`，确认：
- 抠图干净（无绿边残留、无背景残块）；
- 各帧基线一致、居中；
- 32 色板下幽灵配色没崩（脚本会在 >32 色时报错）。

不满意就回第 3 步调提示词重出该动作，只需重放那张 PNG 再 `all`。

---

## 7. 换进固件并编译

精灵头已就地覆盖，直接编译验证：

```bash
cd /data/projects/TuyaOpen && . ./export.sh
cd /data/projects/tuya-powered-agentic-keyboard-for-kiro/firmware/feature-probes/lcd_showcase
tos.py clean -f && tos.py build
```

- 目标 **BUILD SUCCESS**；注意 **AP 分区上限 3.5MB**（当前浣熊约 2.28MB，余 ~1.4MB）。
  幽灵若比浣熊大很多导致超限，回第 3 步**减小 `maxBox`/减帧**，或降部分动作分辨率。
- host 单测不受影响（纯逻辑，与精灵无关）。

---

## 8. 真机验收（需扩展板）

```powershell
.\flash.ps1 -Port COM6 -App firmware/feature-probes/lcd_showcase -Monitor
```

看右下角 Kiro 幽灵动画是否正常（帧率、透明边缘、随聚焦槽状态切换）。

---

## 常见问题

| 现象 | 处理 |
|---|---|
| 抠图后有绿边残留 | 出图时确保纯 #00ff00 且无 JPEG 压缩；必要时调 `mascot_pipeline.py` 的 `chroma_to_alpha` 阈值 |
| "content touches cell-edge pixels" | 帧贴到了格子边，出图时每帧留更多边距 |
| "unexpected frame count" | 精灵表行列/帧数没对齐 `manifest.json` 的 grid |
| ">32 colors" | 出图色彩太杂；简化配色，或扩 `palette.json`（保持恰好 32 色） |
| 编译超 AP 3.5MB | 减 `maxBox`、减帧数、或对大动作降分辨率 |
| 透明键 | 固定 `0x18C5`（manifest `transparentKey`），与 `app_rle_sprite.h` 一致，勿改 |

## 归属
管线改编自 CodeTank `tools/mascot_pipeline.py`（切帧/抠色/RLE 改编自 clawd-tank，MIT）。
角色形象为 AWS Kiro 幽灵，参考 ki-board `AWS_KIRO_Ghost_Outline.svg`。
