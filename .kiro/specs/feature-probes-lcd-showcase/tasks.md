# FP-lcd-showcase · Tasks

按顺序实现，逐条回填状态。复用件从 FP-1 `agent_display` 软复制（probe 自包含）。

- [ ] **T1 目录 + 复用件落位**
  - 建 `firmware/feature-probes/lcd_showcase/{src,include,include/assets,config,test}`。
  - 从 FP-1 复制：`ak_panel.c/.h`、`ak_scene.c/.h`、`ak_notify_ui.c/.h`、`app_notification.c/.h`、
    `app_rle_sprite.h`、`assets/generated/*`（全部 `sprite_mascot_*.h` + registry）。
  - `CMakeLists.txt`、`app_default.config`、`config/TUYA_T5AI_CORE.config` 照 FP-1 改 probe 名。
  - 验收：目录齐、config `CONFIG_DISPLAY_NAME="ak_lcd"` + LVGL 字体开关在。

- [ ] **T2 showcase_map（纯函数）+ host 单测**
  - `include/showcase_map.h` + `src/showcase_map.c`：按键/摇杆 → `showcase_intent_t`。
  - `test/test_lcd_showcase.c` + `run_host_tests.sh`：覆盖 R3.1 全部映射。
  - 验收：host 单测 map 部分 PASS，无 SDK 依赖。

- [ ] **T3 showcase_state（状态机）+ host 单测**
  - `include/showcase_state.h`（`showcase_view_t` + 6 场景表）+ `src/showcase_state.c`。
  - `showcase_state_apply(intent)` 改 view；`showcase_state_view()` 取只读快照。
  - 吉祥物/通知副作用通过弱回调或函数指针注入，使 host 测不依赖 LVGL。
  - host 单测：SCENE_STEP 环回 0..5；FOCUS/PTT/CONN toggle 改对字段。
  - 验收：host 单测 state 部分 PASS。

- [ ] **T4 ak_hud（LVGL 富信息布局）**
  - `include/ak_hud.h` + `src/ak_hud.c`：`ak_hud_init()` 建四区静态骨架；
    `ak_hud_render(const showcase_view_t*)` 按快照刷新文本/高亮/颜色。
  - 顶栏 / 3 槽位卡片 / 上下文主区 / 底栏；配色见 design。
  - 验收：编译过（真机观感留 T7）。

- [ ] **T5 角落吉祥物接入**
  - `showcase_state.c` 里按聚焦槽状态/场景调 `ak_scene_set_anim` / `play_oneshot`。
  - `ak_scene` 的 image 对齐右下角，尺寸不放大。
  - 验收：编译过；串口日志能看到 `scene set_anim -> ...`。

- [ ] **T6 tuya_app_main 串联 + 构建**
  - 入口：panel → LVGL → hud_init → scene_init → notify_ui_init → state_init（首屏场景 0）
    → input_thread。
  - `cd /data/projects/TuyaOpen && . ./export.sh`；`tos.py config choice -c TUYA_T5AI_CORE.config`；
    `tos.py clean -f && tos.py build`。
  - 验收：**BUILD SUCCESS**，产出 `lcd_showcase_QIO_0.1.0.bin`，AP < 3.5MB；`clang-format` clean。

- [ ] **T7 README + 真机验收说明（留用户执行）**
  - `README.md`：管线图、按键映射表、6 场景说明、构建/烧录命令（COM6）、归属。
  - 真机：`.\flash.ps1 -Port COM6 -App firmware/feature-probes/lcd_showcase -Monitor`。
  - 验收：文档齐；真机观感由用户在扩展板上确认（EC2 只保证编译+逻辑）。

## 附带交付（方案 A 的 Kiro 吉祥物脚手架，独立于上面的 T1–T7）

- [ ] **K1 Kiro 吉祥物管线脚手架**
  - `firmware/feature-probes/lcd_showcase/tools/kiro_mascot/`（或项目级 `tools/kiro_mascot/`）：
    - `manifest.json`（照 CodeTank 结构，character 换 Kiro 幽灵，动作集对齐 19 个）。
    - `palette.json`（Kiro 主题色：紫/青/白/深底）。
    - `prompts/prompts.json`（canonical + 每动作提示词，角色=Kiro 幽灵）。
    - `mascot_pipeline.py`（改编自 CodeTank，输出到 `include/assets/generated/`，透明键、
      RGB565 RLE、round-trip 校验保持一致）。
  - 验收：`python3 mascot_pipeline.py --help` 跑通；无源图时报缺图而非崩溃。

- [ ] **K2 手工出图 SOP 文档**
  - `docs/internal/kiro-mascot-authoring.md`：从提示词 → 出精灵表 → 抠色/切帧 → 跑 pipeline →
    换进 probe → 编译 → 烧录 的**逐步操作手册**（供用户晚上手工执行）。
  - 验收：步骤可照做，命令可复制粘贴。
