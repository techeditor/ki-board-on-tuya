# 本地 OTA（方案 A：Companion 经 USB）—— 包格式、密钥与发布

> 状态：批 1（签名工具与发布流程）+ 批 2（固件接收端，协议见 `shared/protocol/protocol.md`「固件升级（仅 USB）」）。
> Companion 发送端见批 3。不经 Companion 发包：`firmware/app/tools/ota_send.py`。
> 设计依据：内部 OTA 设计记录（2026-11-17 定案：只走 USB，不走涂鸦云 OTA）。

## 1. 规则

- 只经 USB（UART0）升级。BLE 来源的 OTA 命令一律丢弃。
- 升级包必须验签（ECDSA P-256 / SHA-256）。键盘验签，Companion 也验签。
- 不允许降级：包的版本必须高于键盘当前版本，也高于键盘记录过的最高版本。
- debug 固件不可经 OTA 发给用户：正式（release）公钥只签 release profile 的构建；
  debug profile 的固件只认测试公钥，测试公钥签的包 release 固件一律拒绝。
  也就是说：debug 板子之间可以用测试包做 OTA 试验，但 debug 镜像永远到不了 release 设备。
  `CONFIG_AK_DEBUG_PROBES` / `CONFIG_AK_OTA_TEST_KEY` / `CONFIG_AK_LICENSE_DEBUG_READ` 任一为 `y` 的构建都算 debug（`ota_pack.py` 的 `profile_of` 按这三个 Kconfig 名判断）。
- 开始升级前必须在键盘上确认（K6 确认 / K1 取消，默认取消，10 s 超时）。
- 开始升级前和写完、重启前都要满足供电条件：USB 供电，或电量读数可信且不低于 30%（`AK_OTA_MIN_BATTERY_PERCENT`）。不满足时报 `power`，不写入 KV、不重启。
- KV（License、激活数据、BLE 配对密钥、keymap、设置）全部保留。
- 涂鸦控制台**永远不建固件升级任务**（SDK 里的云 OTA 代码没有删，见设计报告 §1）。

## 2. 包格式（`.akota`）

```
[manifest 192 B][signature 64 B][UG 文件，原样]
```

manifest，全部小端：

| 偏移 | 长度 | 字段 | 说明 |
|---|---|---|---|
| 0 | 4 | magic | `AKOT` |
| 4 | 2 | format | 1 |
| 6 | 2 | manifest_len | 192 |
| 8 | 16 | product | `ki-board`，其余补 0 |
| 24 | 8 | hw | `v0.3`，其余补 0 |
| 32 | 1 | profile | 1 = release，2 = debug（`CONFIG_AK_LICENSE_DEBUG_READ=y` 或 `CONFIG_AK_OTA_TEST_KEY=y` 的构建） |
| 33 | 3 | 保留 | 0 |
| 36 | 6 | version | major、minor、patch，各 uint16 |
| 42 | 2 | 保留 | 0 |
| 44 | 4 | image_size | UG 文件字节数，不超过 ota 区 2940K 的 85% |
| 48 | 32 | image_sha256 | 整个 UG 文件的 SHA-256 |
| 80 | 8 | key_id | 签名公钥（65 B 非压缩点）SHA-256 的前 8 字节 |
| 88 | 40 | 保留 | 0 |
| 128 | 64 | ug_header | UG 文件前 64 字节的副本（键盘据此在收数据前算好写入地址并擦除） |

signature：对 manifest 192 字节做 ECDSA P-256 / SHA-256，`r‖s` 各 32 字节大端。

UG 文件要求：magic `0x4D4D4D`、类型 `0x4C5A4350`（LZMA 全量压缩）、`patch_length + 64 == 文件长度`。
`ota_pack.py` 会拒绝 QIO/UA 文件和其他类型。

另有一个平台限制：T5AI 平台在管理块下方为包预留 `align4k(patch_length + 28)` 字节，而文件长 `patch_length + 64`。
文件大小正好落在这 36 字节的扇区缺口里时，包会压到管理块上；键盘拒收（`ota_status` 原因 `flash`），`ota_pack.py` 签名时也会拒绝。
任何改变固件大小的重新构建都能避开（概率约 1%）。

## 3. 密钥

| 密钥 | 私钥在哪 | 公钥在哪 | 用途 |
|---|---|---|---|
| 正式（release） | **只在你本人的离线介质上**（加密 PEM），不进仓库、不进 CI、不进 Companion | `firmware/app/common/include/ak_ota_key_release.h`（现在是占位） | 签发给用户的 release 固件 |
| 测试（test） | 开发机临时目录，不提交 | `firmware/app/common/include/ak_ota_key_test.h` | debug 构建和实板试验 |

占位期间：release profile 的固件编译时打印警告，运行时拒绝所有 OTA 包（`ota_status` 原因 `no_key`）；
设置环境变量 `AK_RELEASE_BUILD=1` 构建会直接失败；`ota_pack.py` 拒绝用 release 密钥签名。

依赖：`pip install cryptography`。

### 3.1 生成正式密钥（离线，只做一次）

在一台不联网、或至少不同步云盘的电脑上：

```
python3 firmware/app/tools/ota_pack.py keygen --out-dir <U 盘目录> --name ki-board-ota-release
```

- 会要求两次输入口令（至少 12 个字符）。私钥用口令加密写入 `ki-board-ota-release_private.pem`（权限 0600），公钥写入 `..._public.pem`。
- 命令只打印文件路径和 `key_id`，不打印私钥。
- 私钥和口令分开保存，各备份一份（例如两只 U 盘放在不同地方）。私钥丢了就无法再给已出厂的键盘发 OTA，只能 USB + tyutool 重刷。
- 私钥泄露：任何人都能给键盘装固件。处理办法是发一版换新公钥的固件（见 §3.3）。

然后把**公钥**（只有公钥）带回开发机：

```
python3 firmware/app/tools/ota_pack.py pubkey-header --kind release --pub ki-board-ota-release_public.pem
```

这会覆盖 `ak_ota_key_release.h`。提交这个文件，走正常 MR。

### 3.2 测试密钥

开发者自己生成即可，不需要口令：

```
python3 firmware/app/tools/ota_pack.py keygen --out-dir <临时目录> --name test --no-passphrase
python3 firmware/app/tools/ota_pack.py pubkey-header --kind test --pub <临时目录>/test_public.pem
```

换测试公钥后，旧测试密钥签的包对新 debug 固件无效。仓库里当前的测试公钥 `key_id` 见头文件注释。

### 3.3 换正式密钥

固件只认编进去的那一把公钥。换钥匙时：先用**旧**私钥签一版内含**新**公钥的固件并发布；之后的版本用新私钥签。
跳过这一步，已装旧公钥的键盘只能 USB + tyutool 重刷。

## 4. 发布流程（每次发版）

1. 按 `docs/versioning.md` §5 发版清单完成检查。另加两项：
   - KV 兼容：本版若改了任何 KV 值的结构，确认能读旧值（或带版本字段）。
   - `ak_ota_key_release.h` 不是占位。
2. 用 release profile 清空重编（例如 `TUYA_T5AI_CORE_dual_cloudasr.config`），环境变量 `AK_RELEASE_BUILD=1`：
   ```
   tos.py config choice   # 选 release profile
   tos.py clean -f && AK_RELEASE_BUILD=1 tos.py build
   ```
3. 在离线机上签名（把 `.build/bin/app_UG_<ver>.bin`、`app_UA_<ver>.bin`、`.build/cache/using.config` 和仓库的 `VERSION`、两个公钥头文件拷过去）：
   ```
   python3 firmware/app/tools/ota_pack.py sign \
       --ug app_UG_0.3.60.bin --ua app_UA_0.3.60.bin \
       --config using.config --key <U 盘>/ki-board-ota-release_private.pem
   ```
   工具会拒绝：版本与 `VERSION` 或文件名不一致；debug 构建；UA 里没有正式公钥（用占位构建的固件，装上后再也收不了 OTA）；UA 里带测试公钥；UG 不是全量 LZMA 包或超过 85%。
   输出 `ki-board_0.3.60_release_release.akota`，并打印包的 SHA-256。
4. 在开发机上复核：
   ```
   python3 firmware/app/tools/ota_pack.py verify ki-board_0.3.60_release_release.akota
   ```
5. 把 `.akota` 和打印的 SHA-256 一起发布（发布渠道另定）。用户在 Companion 里选这个文件升级。

## 5. 防降级与回退

- 键盘只接受版本号**严格大于**当前版本和 KV `ak_ota_hi`（装过的最高版本）的包。
- 回退一个坏版本：用旧代码、**更高的版本号**重新构建并签名发布。
- 起不来的键盘：用 tyutool 整片重刷（芯片 ROM 下载模式，始终可用）。

## 6. 工具清单

| 文件 | 作用 |
|---|---|
| `firmware/app/tools/ota_pack.py` | keygen / pubkey-header / sign / verify |
| `firmware/app/tools/test_ota_pack.py` | 单元测试（`firmware/app/test/run_host_tests.sh` 会跑；没装 `cryptography` 时跳过并提示） |
| `firmware/app/common/include/ak_ota_key_release.h` | 正式公钥（当前占位） |
| `firmware/app/common/include/ak_ota_key_test.h` | 测试公钥 |

## 7. ota_status 原因码

固件 `ak_ota.c` 的 `ota_status` 失败原因（`reason` 字段）：

| 原因 | 含义 |
|---|---|
| `no_key` | 正式公钥仍是占位，拒绝所有包 |
| `bad_sig` | 签名校验失败 |
| `downgrade` | 版本不高于当前版本或 `ak_ota_hi` |
| `power` | 供电条件不满足（§1） |
| `busy` | 已有升级会话 |
| `flash` | 写入失败，包压到管理块等（§2） |
| `hash` | UG 数据 SHA-256 不符 |
| `timeout` / `cancelled` | 确认超时（10 s）或键盘上取消 |
| `sequence` / `incomplete` / `aborted` / `state` | 数据序号、中途不完整、中止、状态机错误 |

