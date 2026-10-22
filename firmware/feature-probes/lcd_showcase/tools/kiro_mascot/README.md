# Kiro Ghost Mascot 资产管线（脚手架）

把角落吉祥物从 CodeTank 浣熊换成 **Kiro 幽灵**的资产管线。**这是脚手架**：源精灵表由你
手工出图（详见内部文档），
本目录只提供 manifest / 调色板 / 提示词 / 打包脚本。

## 内容

| 文件 | 作用 |
|---|---|
| `manifest.json` | 19 个动作定义（enum 对齐 `ak_scene.h` 的 `mascot_anim_id_t`，grid/帧数/fps/maxBox 沿用 CodeTank 起点），character 换成 Kiro 幽灵 |
| `palette.json` | Kiro 主题 32 色板（紫/青/白/深底/琥珀红） |
| `prompts/prompts.json` | canonical + 每动作出图提示词（角色=Kiro 幽灵，#00ff00 绿幕，16-bit 像素风） |
| `mascot_pipeline.py` | 切帧→抠绿幕→套调色板→RGB565 RLE→round-trip 校验→写头文件，改编自 CodeTank |
| `source/` | **你放手工出图的精灵表**（`idle.png`/`thinking.png`/… 共 18 张，`mini_mascot` 由 walking 派生不用单独出） |

## 用法

```bash
python3 -m pip install Pillow        # 一次性依赖
cd firmware/feature-probes/lcd_showcase/tools/kiro_mascot

python3 mascot_pipeline.py validate  # 只校验 manifest + 源图是否齐（缺图会清晰报缺哪几张）
python3 mascot_pipeline.py all       # 处理→预览→打包，输出到 ../../include/assets/generated/
```

- 缺 `source/*.png` 时 `all`/`validate` 会**优雅报缺图并退出（非零）**，不 traceback。
- `--help` 无需装 Pillow 即可运行。
- 输出直接覆盖 lcd_showcase 现有的 `sprite_mascot_*.h` + `sprite_mascot_registry.h`
  （即当前的 CodeTank 浣熊），随后 `tos.py build` 即换肤。
- 中间产物在 `build/mascot/`（帧 + 预览 GIF/gallery），可 gitignore。

> 输出头文件与 `include/app_rle_sprite.h` 的 RLE 格式、`sprite_mascot_registry.h` 的
> `mascot_anim_def_t` 结构完全兼容，透明键固定 `0x18C5`。

## 归属
改编自 CodeTank `tools/mascot_pipeline.py`（其切帧/抠色/RLE 改编自 clawd-tank，MIT）。
角色形象为 AWS Kiro 幽灵。
