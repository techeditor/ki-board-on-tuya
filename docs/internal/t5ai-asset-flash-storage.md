# T5AI 大素材（精灵/表情）独立存储研究

> 备用研究记录 · 2026-09-14 · 基于本机 TuyaOpen 与 lcd_showcase 实况核实。
> 问题起点：表情/精灵帧数据体积大，能否**单独烧到 T5 Flash 某个位置、不占应用程序空间（AP 分区）**。

## 1. 背景：什么是 AP 分区，素材现在占在哪

T5-E1（BK7258）是**双核**：CP（Communication Processor，跑 Wi-Fi/BLE 系统固件）+
AP（Application Processor，跑 TuyaOpen 应用层 = 你写的 `tuya_app_main` + LVGL + 精灵）。
构建产出 `bk7258`(CP) 与 `bk7258_ap`(AP) 两套。

`lcd_showcase` 构建日志中三个 size 行的含义：

```
bl2 size 32480,   limit 65536,   partition size 69632      ← bootloader
cp  size 971156,  limit 1048576, partition size 1114112    ← CP 分区（系统固件）
ap  size 2277376, limit 3670016, partition size 3899392    ← AP 分区（你的应用）
```

- `ap size 2.28MB / limit 3.5MB`：**当前 19 个 CodeTank RLE 精灵（~1.17MB）是编进 AP 分区、
  和应用代码挤在一起的**（`sprite_mascot_*.h` 里的 `const uint16_t[]`）。余量 ~1.2MB。
- 结论：现状不溢出。此文档解决的是「换未压缩大帧 / 素材持续增长 / 想让素材独立迭代」时的问题。

## 2. T5AI-Core 真实分区表（8MB Flash）

来源：`firmware/feature-probes/lcd_showcase/.build/bin/debug/auto_partitions.csv`

| Name | Offset | Size | Type | R | W | 用途 |
|---|---|---|---|---|---|---|
| primary_bootloader | — | 68K | code | ✓ | ✗ | bootloader |
| primary_tuyaboot | — | 68K | code | ✓ | ✗ | tuya boot |
| primary_cp_app | — | 1088K | code | ✓ | ✗ | **CP 系统固件** |
| primary_ap_app | — | **3808K** | code | ✓ | ✗ | **AP 应用（精灵现在在这）** |
| ota | — | **2940K** | data | ✓ | ✓ | OTA 暂存区 |
| ota_mgr | — | 8K | data | ✓ | ✓ | OTA 管理 |
| usr_config | — | 8K | data | ✓ | ✓ | 用户配置 |
| tuya_data | 0x7cd000 | 196K | data | ✓ | ✓ | KV / 授权等 |
| sys_rf | 0x7fe000 | 4K | data | ✓ | ✓ | RF 校准 |
| sys_net | 0x7ff000 | 4K | data | ✓ | ✓ | 网络参数 |

8MB = 8192K，上表合计约 8192K，**基本占满**——新增独立资源分区需要**从现有区腾空间**
（最现实是压缩 `ota` 或缩小 `primary_ap_app`），不能凭空加。

## 3. 平台提供的能力（已核实）

### 3.1 裸 Flash 读写 API
`platform/T5AI/tuyaos/tuyaos_adapter/include/flash/tkl_flash.h`：
```c
OPERATE_RET tkl_flash_read (uint32_t addr, uint8_t *dst, uint32_t size);
OPERATE_RET tkl_flash_write(uint32_t addr, const uint8_t *src, uint32_t size);
OPERATE_RET tkl_flash_erase(uint32_t addr, uint32_t size);
OPERATE_RET tkl_flash_get_one_type_info(TUYA_FLASH_TYPE_E type, TUYA_FLASH_BASE_INFO_T *info);
```

### 3.2 分区类型枚举里已预留用户区
`tuya_cloud_types.h` 的 `TUYA_FLASH_TYPE_E` 含：
```
... TUYA_FLASH_TYPE_APP, TUYA_FLASH_TYPE_OTA,
    TUYA_FLASH_TYPE_USER0, TUYA_FLASH_TYPE_USER1,   ← 专门给用户素材/数据的分区抽象
    TUYA_FLASH_TYPE_UF (user file), TUYA_FLASH_TYPE_KV_* ...
```
`tkl_flash_get_one_type_info(TUYA_FLASH_TYPE_USER0, &info)` 可拿到该区的**起始地址+大小**，
免去硬编码偏移。

### 3.3 文件系统 API
`platform/T5AI/tuyaos/tuyaos_adapter/include/system/tkl_fs.h`：`tkl_fs_mkdir/open/read/...`，
设备类型含 `DEV_INNER_FLASH / DEV_EXT_FLASH / DEV_SDCARD / DEV_USB_DISK`。即平台自带
文件系统抽象，可把素材当文件读。

## 4. 三条可行方案

### 方案 A — 独立资源分区 + 偏移读取（最贴合"单独烧、不占程序空间"）
- 划一个资源分区（复用 `USER0`，或从 `ota`/`ap` 腾出自定义区），把精灵打包成**单一 .bin +
  索引头**烧进去。
- 运行时 `tkl_flash_get_one_type_info(USER0)` 拿基址 → `tkl_flash_read(base+off, buf, len)`
  按帧偏移读取；解 RLE 后交给 LVGL。
- **优点**：素材/固件解耦——改素材只重烧资源分区，不重编固件；AP 分区只剩代码。
- **代价**：改分区表（要腾空间）；写一个资源索引格式 + 运行时读取层；多一步"烧资源 bin"。
- **RAM**：只需 1~2 帧解码缓冲（最大 sprite ~136×97×3B ≈ 40KB），可接受。

### 方案 B — littlefs / tkl_fs 文件系统（最灵活）
- 资源分区上挂文件系统，精灵当文件 `tkl_fs_open`/读。
- **优点**：加/换动画=加文件，最省心，天然支持多素材。
- **代价**：文件系统读开销 + RAM 缓冲；逐帧动画要注意吞吐；需打包 fs 镜像并烧入。

### 方案 C — 保持编进 AP，靠 RLE 压缩扛（现状，零改动）
- 当前做法：RLE 让 19 动画仅 ~1.17MB，AP 2.28/3.5MB 余 ~1.2MB。
- **优点**：零工程改动，最稳。
- **适用**：素材规模不暴涨时的默认选择。

## 5. 对比与建议

| 维度 | A 资源分区+偏移 | B 文件系统 | C 编进 AP(现状) |
|---|---|---|---|
| 占 AP 程序空间 | 否 | 否 | 是 |
| 素材独立迭代（不重编固件） | ✓ | ✓ | ✗ |
| 工程改动量 | 中（分区表+读取层） | 中大（fs+打包） | 无 |
| 运行时 RAM | 小（帧缓冲） | 中（fs 缓冲） | 0（XIP 直读） |
| 加素材便利度 | 中（重打包 bin） | 高（加文件） | 低（重编） |
| 8MB 是否够腾 | 需从 ota/ap 腾 | 需从 ota/ap 腾 | 无需 |

**建议**：
- 素材维持当前规模（RLE 后 ~1.2MB）→ **留 C**，不折腾分区最稳。
- 素材会长到数 MB / 想独立迭代 / 要上未压缩大帧 → **走 A**，最符合"单独烧、不占程序空间"，
  且 `USER0` + `tkl_flash_*` 已是现成接口，落地路径清晰。
- 素材种类多且频繁增删 → **B**。

## 6. 方案 A 落地清单（备用）

1. **腾空间**：8MB 已近满，从 `ota`(2940K) 或 `primary_ap_app`(3808K) 划出资源区
   （e.g. 1~2MB）。改 T5 分区配置（`bk7258_ap/partitions/` 下 json/csv 体系）。
2. **打包器**：给 `tools/kiro_mascot/mascot_pipeline.py` 加一个 `pack-bin` 模式——
   输出单一 `assets.bin`（连续 RLE 帧）+ 一个 C 索引头（每动画的 offset/frame_count/w/h/
   frame_ms/looping），替代现在的 `sprite_mascot_*.h`。
3. **运行时读取层**：新增 `ak_asset_flash.c`——启动时 `tkl_flash_get_one_type_info(USER0)`
   拿基址；`ak_asset_load_frame(anim, idx, buf)` = 查索引 → `tkl_flash_read` → 解 RLE。
   `ak_scene.c` 从"指针直取 RLE"改为"从 flash 读一帧到 buf 再解"。
4. **烧录步骤**：`tos.py` 烧固件外，额外用 tyutool 把 `assets.bin` 烧到资源分区偏移
   （或做成 OTA 式独立升级）。
5. **验证**：host 侧对 assets.bin 做 round-trip；真机看动画帧率/透明边缘（COM6）。

## 7. 关键事实速查

- Flash 总量：**8MB**；AP 分区上限 **3808K(~3.5MB)**，当前用 2.28MB。
- 精灵现状：RLE 压缩后 ~1.17MB，**编进 AP 分区**。
- 读写接口：`tkl_flash_read/write/erase` + `tkl_flash_get_one_type_info`。
- 用户分区抽象：`TUYA_FLASH_TYPE_USER0/USER1`、`TUYA_FLASH_TYPE_UF`。
- 文件系统：`tkl_fs_*`（`DEV_INNER_FLASH` 等）。
- 8MB 已近占满，新分区需从 `ota`/`ap` 腾，非凭空新增。
- 透明键固定 `0x18C5`；最大 sprite ~136×97，单帧解码缓冲 ~40KB。

## 归属 / 参考
- 分区表：`firmware/feature-probes/lcd_showcase/.build/bin/debug/auto_partitions.csv`
- Flash API：`platform/T5AI/tuyaos/tuyaos_adapter/include/flash/tkl_flash.h`
- 分区类型：`platform/T5AI/tuyaos/.../tuya_cloud_types.h`（`TUYA_FLASH_TYPE_E`）
- 文件系统：`platform/T5AI/tuyaos/tuyaos_adapter/include/system/tkl_fs.h`
- 精灵管线：`firmware/feature-probes/lcd_showcase/tools/kiro_mascot/mascot_pipeline.py`
