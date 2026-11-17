/**
 * @file ak_i18n.c
 * @brief Interface language and LCD string table. See ak_i18n.h.
 *
 * Source is UTF-8. tools/gen_font_ui.py reads the string literals in this file
 * to make sure the UI font has every character, so after adding a string here
 * regenerate common/src/ak_font_ui12_data.c and ak_font_ui16_data.c (the host test fails until you do).
 */

#include "ak_i18n.h"

#include <string.h>

static ak_lang_t s_lang = AK_LANG_DEFAULT;

/* [id] = {中文, English} */
static const char *const k_str[AK_STR_COUNT][AK_LANG_COUNT] = {
    [AK_STR_SET_TITLE]        = {"设置", "Settings"},
    [AK_STR_SET_VOLUME]       = {"音量", "Volume"},
    [AK_STR_SET_BRIGHTNESS]   = {"屏幕亮度", "Brightness"},
    [AK_STR_SET_VOICE_MODE]   = {"语音模式", "Voice mode"},
    [AK_STR_SET_LED_POWER]    = {"灯光电源", "LED power"},
    [AK_STR_SET_LED_TIMEOUT]  = {"灯光自动关闭", "LED auto-off"},
    [AK_STR_SET_LANGUAGE]     = {"语言", "Language"},
    [AK_STR_SET_FONT_SIZE]    = {"字号", "Font size"},
    [AK_STR_SET_CREDENTIALS]  = {"设备凭据", "Credentials"},
    [AK_STR_SET_PAIRING]      = {"配网模式", "Pairing mode"},
    [AK_STR_SET_TEST]         = {"硬件测试", "Test hardware"},
    [AK_STR_SET_EXIT]         = {"退出", "Exit"},
    [AK_STR_SET_APPLIED]      = {"已应用", "Applied"},
    [AK_STR_SET_OPEN]         = {"已打开", "OPEN"},
    [AK_STR_SET_HINT_ADJUST]  = {"旋转：调整数值", "Rotate: adjust value"},
    [AK_STR_SET_HINT_COMMIT]  = {"单击：确认并退出编辑", "Click: confirm & exit edit"},
    [AK_STR_SET_HINT_PAIRING] = {"单击：打开 / 关闭配网窗口", "Click: open / close pairing window"},
    [AK_STR_SET_HINT_PAIRING2] = {"允许 USB Companion 设置 Wi-Fi / License", "Lets USB Companion set Wi-Fi/License"},
    [AK_STR_SET_HINT_SELECT]  = {"旋转：选择项目", "Rotate: select item"},
    [AK_STR_SET_HINT_CLICK]   = {"单击：编辑 / 打开 / 退出", "Click: edit value / open / Exit"},
    [AK_STR_VOICE_ASR]        = {"语音输入", "ASR"},
    [AK_STR_VOICE_CHAT]       = {"对话", "Chat"},
    [AK_STR_LED_OFF]          = {"关", "Off"},
    [AK_STR_LED_ON]           = {"常开", "On"},
    [AK_STR_LED_AUTO]         = {"自动", "Auto"},
    [AK_STR_TIME_30S]         = {"30 秒", "30 s"},
    [AK_STR_TIME_60S]         = {"60 秒", "60 s"},
    [AK_STR_TIME_5MIN]        = {"5 分钟", "5 min"},
    [AK_STR_TIME_15MIN]       = {"15 分钟", "15 min"},
    [AK_STR_NOTE_CHAT]        = {"对话：回复显示在键盘上", "Chat: replies stay on the keyboard"},
    [AK_STR_NOTE_ASR]         = {"语音输入：文字发给 Kiro Crew", "ASR: speech goes to Kiro Crew"},
    [AK_STR_NOTE_LEDS_OFF]    = {"灯光关闭（断电）", "LEDs off (power cut)"},
    [AK_STR_NOTE_LEDS_ON]     = {"灯光常开", "LEDs always powered"},
    [AK_STR_NOTE_LEDS_AUTO]   = {"灯光：空闲自动断电", "LEDs: auto power-off"},
    [AK_STR_NOTE_LANGUAGE]    = {"语言：中文", "Language: English"},
    [AK_STR_NOTE_FONT_SIZE]   = {"字号：%u px", "Font size: %u px"},
    [AK_STR_ST_HOLD_TO_SPEAK] = {"按住语音键说话", "Hold the voice key and speak"},
    [AK_STR_ST_DICTATION_NEEDS_CLOUD] = {"语音回答需要涂鸦云 ASR", "Dictation needs Tuya cloud ASR"},
    [AK_STR_ST_READY] = {"就绪", "Ready"},
    [AK_STR_ST_INPUT_CANCELLED] = {"已取消输入", "Input cancelled"},
    [AK_STR_ST_INPUT_CLEARED] = {"已清空输入", "Input cleared"},
    [AK_STR_ST_INPUT_NOT_SENT] = {"未发送：K6 重发", "Not sent: K6 to retry"},
    [AK_STR_ST_INPUT_CONFIRMED] = {"已确认输入", "Input confirmed"},
    [AK_STR_ST_CHAT_NEEDS_CLOUD] = {"对话模式需要涂鸦云", "Chat needs Tuya cloud"},
    [AK_STR_ST_WIFI_CONNECTING] = {"正在连接 Wi-Fi…", "WiFi connecting..."},
    [AK_STR_ST_WIFI_CONNECTED] = {"Wi-Fi 已连接", "WiFi connected"},
    [AK_STR_ST_WIFI_FAILED] = {"警告：Wi-Fi 连接失败", "Warning: WiFi failed"},
    [AK_STR_ST_WIFI_RESTORED] = {"Wi-Fi 失败，已恢复原网络", "WiFi failed, restored previous"},
    [AK_STR_ST_WIFI_DISCONNECTED] = {"警告：Wi-Fi 已断开", "Warning: WiFi disconnected"},
    [AK_STR_ST_PAIR_WINDOW] = {"配网窗口 %u:%02u（USB）", "Pairing window %u:%02u (USB)"},
    [AK_STR_ST_PAIR_WRITE_LICENSE] = {"经 USB 写入 License %u:%02u", "Write License via USB %u:%02u"},
    [AK_STR_ST_REQUEST_WAITING] = {"有请求在等待（K6/K4 之后）", "Request waiting (after K6/K4)"},
    [AK_STR_ST_PAIRING_TUYA_APP] = {"配网模式：请用涂鸦 App", "Pairing mode: use Tuya app"},
    [AK_STR_ST_ASR_FALLBACK] = {"涂鸦 ASR 不可用，改用 PC ASR", "Tuya ASR unavailable -> PC ASR"},
    [AK_STR_ST_ASR_RESTORED] = {"涂鸦 ASR 已恢复", "Tuya ASR restored"},
    [AK_STR_ST_ASR_READY] = {"涂鸦 ASR 就绪", "Tuya ASR ready"},
    [AK_STR_ST_PC_ASR] = {"已选择 PC ASR", "PC ASR selected"},
    [AK_STR_ST_SESSION] = {"会话 %s：%s", "Session %s: %s"},
    [AK_STR_ST_STATUS] = {"状态：%s", "Status: %s"},
    [AK_STR_ST_SELECTED_SESSION] = {"已选择会话 %s", "Selected Session %s"},
    [AK_STR_ST_LISTENING] = {"正在听…", "Listening..."},
    [AK_STR_ST_RECOGNIZING] = {"识别中…", "Recognizing..."},
    [AK_STR_ST_DICTATED_HINT] = {"发送 K6 / 重说 语音键 / 返回 K1", "Send: K6 / Again: voice key / Back: K1"},
    [AK_STR_ST_CONFIRM_HINT] = {"发送 K6 / 取消 K1", "Send: K6 / Cancel: K1"},
    [AK_STR_ST_NO_SPEECH] = {"警告：没有听到语音", "Warning: no speech"},
    [AK_STR_ST_ASR_FAILED] = {"警告：语音识别失败", "Warning: ASR failed"},
    [AK_STR_ST_ASR_TIMEOUT] = {"警告：语音识别超时", "Warning: ASR timeout"},
    [AK_STR_ST_PAIRING_STORAGE] = {"配网模式失败：存储错误", "Pairing mode failed: storage"},
    [AK_STR_ST_PAIRING_RESTART] = {"正在重启进入配网模式", "Restarting into pairing mode"},
    [AK_STR_ST_BLE_PAIRED] = {"已与此 Companion 蓝牙配对", "BLE paired with this Companion"},
    [AK_STR_ST_BLE_CLEARED] = {"已清除蓝牙配对", "BLE pairing cleared"},
    [AK_STR_ST_LICENSE_EXISTS] = {"已有 License：请先允许覆盖", "License exists: allow overwrite first"},
    [AK_STR_ST_PAIRING_FIRST] = {"请先在设置中打开配网模式", "Settings > Pairing mode first"},
    [AK_STR_ST_USB_CONNECTED] = {"Companion 已通过 USB 连接", "Companion connected via USB"},
    [AK_STR_ST_USB_DISCONNECTED] = {"USB 已断开，请重新连接", "USB disconnected; reconnect USB"},
    [AK_STR_ST_USB_TIMEOUT] = {"USB 超时，请重新连接", "USB timed out; reconnect USB"},
    [AK_STR_ST_BLE_INIT_FAILED] = {"警告：蓝牙初始化失败", "Warning: BLE init failed"},
    [AK_STR_ST_BLE_CONNECTED] = {"Companion 已通过蓝牙连接", "Companion connected via BLE"},
    [AK_STR_ST_BLE_SUBSCRIBING] = {"蓝牙已连接，正在订阅…", "BLE connected; subscribing..."},
    [AK_STR_ST_BLE_ADVERTISING] = {"蓝牙广播中，等待 Companion", "BLE advertising for Companion"},
    [AK_STR_ST_BLE_DISCONNECTED] = {"蓝牙已断开，重新广播", "BLE disconnected; advertising"},
    [AK_STR_ST_LICENSE_SAVED] = {"License 已保存：重启后生效", "License saved: restart to apply"},
    [AK_STR_ST_LICENSE_WRITE_FAILED] = {"警告：License 写入失败", "Warning: License write failed"},
    [AK_STR_ST_LCD_READY] = {"屏幕就绪", "LCD ready"},
    [AK_STR_ST_CLOUD_STARTING] = {"正在启动云服务…", "Starting cloud..."},
    [AK_STR_ST_CLOUD_OK] = {"云服务已初始化", "Cloud initialized"},
    [AK_STR_ST_CLOUD_FAILED] = {"警告：云服务初始化失败", "Warning: cloud init failed"},
    [AK_STR_ST_ASR_STARTING] = {"正在启动语音识别…", "Starting ASR..."},
    [AK_STR_ST_ASR_OK] = {"语音识别已初始化", "ASR initialized"},
    [AK_STR_ST_ASR_INIT_FAILED] = {"警告：语音识别初始化失败", "Warning: ASR init failed"},
    [AK_STR_ST_WIFI_STARTING] = {"正在启动 Wi-Fi 监控…", "Starting WiFi monitor..."},
    [AK_STR_ST_WIFI_INIT_FAILED] = {"警告：Wi-Fi 初始化失败", "Warning: WiFi init failed"},
    [AK_STR_ST_BOOT_CHAT] = {"语音模式：对话（在设置中修改）", "Voice mode: Chat (Settings to change)"},
    [AK_STR_ST_BOOT_ASR] = {"语音模式：语音输入（在设置中修改）", "Voice mode: ASR (Settings to change)"},
    [AK_STR_ST_SCAN_QR] = {"请在 Companion 中扫码", "Scan QR in Companion"},
    [AK_STR_ST_ACTIVATING] = {"正在激活涂鸦设备…", "Activating Tuya device..."},
    [AK_STR_ST_ACTIVATED] = {"涂鸦设备已激活", "Tuya device activated"},
    [AK_STR_ST_CLOUD_READY] = {"涂鸦云就绪", "Tuya Cloud ready"},
    [AK_STR_ST_CLOUD_DISCONNECTED] = {"涂鸦云已断开", "Tuya Cloud disconnected"},
    [AK_STR_ST_RESETTING] = {"正在重置网络…", "Resetting network..."},
    [AK_STR_ST_PROVISIONED] = {"配网完成", "Provisioning complete"},
    [AK_STR_CHAT_YOU] = {"你：%s", "You: %s"},
    [AK_STR_CHAT_AI] = {"AI：%s", "AI: %s"},
    [AK_STR_CHAT_IDLE] = {"按住 K5 说话  K4 停止", "Hold K5: talk  K4: stop"},
    [AK_STR_CHAT_THINKING] = {"思考中…", "Thinking..."},
    [AK_STR_CHAT_REPLYING] = {"AI 正在回复（K4 停止）", "AI is replying (K4 stops)"},
    [AK_STR_CHAT_TITLE] = {"[对话] 涂鸦 AI 助手", "[Chat] Tuya AI agent"},
    [AK_STR_RST_TITLE] = {"重置网络？", "Reset network?"},
    [AK_STR_RST_L1] = {"Companion 请求清除 Wi-Fi", "Companion asks to clear Wi-Fi"},
    [AK_STR_RST_L2] = {"和云端绑定，然后", "and the cloud binding, then"},
    [AK_STR_RST_L3] = {"重启进入配网。", "reboot into pairing."},
    [AK_STR_RST_L4] = {"License 会保留。", "The License is kept."},
    [AK_STR_RST_KEYS] = {"K6：重置    K1：取消", "K6: reset    K1: cancel"},
    [AK_STR_RST_TIMEOUT] = {"10 秒内不操作即取消", "No answer in 10 s = cancel"},
    [AK_STR_UNPAIR_TITLE] = {"清除蓝牙配对？", "Clear BLE pairing?"},
    [AK_STR_UNPAIR_L1] = {"电脑请求清除本键盘的", "The PC asks to clear this"},
    [AK_STR_UNPAIR_L2] = {"蓝牙配对密钥。", "keyboard's BLE pairing key."},
    [AK_STR_UNPAIR_KEYS] = {"K6：清除    K1：取消", "K6: clear    K1: cancel"},
    [AK_STR_ST_PAIR_REQ_BUSY] = {"请先完成当前确认", "Finish the open prompt first"},
    [AK_STR_CRED_PID] = {"产品 ID", "Product ID"},
    [AK_STR_CRED_SET] = {"已设置", "SET"},
    [AK_STR_CRED_NOT_SET] = {"未设置", "NOT SET"},
    [AK_STR_CRED_SOURCE] = {"来源（调试）", "Source (debug)"},
    [AK_STR_CRED_OVERWRITE_ON] = {"允许覆盖一次（60 秒）", "Overwrite allowed once (60 s)"},
    [AK_STR_CRED_OVERWRITE_K6] = {"K6：允许覆盖一次 License", "K6: allow one License overwrite"},
    [AK_STR_CRED_NEED_USB] = {"连接 USB 并打开配网模式", "Connect USB, open Pairing mode"},
    [AK_STR_CRED_NEED_USB2] = {"以写入 License", "to write the License"},
    [AK_STR_CRED_BLE] = {"蓝牙配对", "BLE pairing"},
    [AK_STR_CRED_BACK] = {"K4 / 单击：返回", "K4 / Click: back"},
    [AK_STR_CRED_UNPAIR_ARMED] = {"再按一次 K1 清除蓝牙配对", "Press K1 again to clear BLE pairing"},
    [AK_STR_CRED_UNPAIR] = {"按两次 K1：清除蓝牙配对", "K1 twice: clear BLE pairing"},
    [AK_STR_SET_REBIND] = {"解绑设备", "Unbind device"},
    [AK_STR_SET_CHANGE_WIFI] = {"更换 Wi-Fi", "Change Wi-Fi"},
    [AK_STR_CHWIFI_TITLE] = {"更换 Wi-Fi？（绑定不变）", "Change Wi-Fi? (stays bound)"},
    [AK_STR_REBIND_TITLE] = {"解绑设备，重新配网？", "Unbind and pair again?"},
    [AK_STR_REBIND_L4] = {"只换 Wi-Fi 请用「更换 Wi-Fi」。", "Wi-Fi only? Use Change Wi-Fi."},
    [AK_STR_REBIND_L1] = {"清除 Wi-Fi 和涂鸦绑定，", "Clears Wi-Fi and the Tuya binding;"},
    [AK_STR_REBIND_L2] = {"保留 License 和已保存的 Wi-Fi。", "keeps the License and saved Wi-Fi."},
    [AK_STR_REBIND_L3] = {"重启后用涂鸦 App 配网。", "Then pair it in the Tuya app."},
    [AK_STR_REBIND_KEYS] = {"K6：解绑   K1：取消", "K6: unbind   K1: cancel"},
    [AK_STR_REBIND_RUNNING] = {"正在解绑，即将重启…", "Unbinding, restarting..."},
    [AK_STR_REBIND_FAILED] = {"解绑失败", "Unbind failed"},
    [AK_STR_REBIND_NEED_LICENSE] = {"未写 License，只能用 USB 配置", "No License: configure over USB"},
    [AK_STR_SET_BIND] = {"绑定会话", "Bind session"},
    [AK_STR_BIND_TITLE] = {"绑定会话：选择槽位", "Bind session: pick a slot"},
    [AK_STR_BIND_UNBOUND] = {"未绑定", "Not bound"},
    [AK_STR_BIND_HINT1] = {"K1-K3：选择槽位", "K1-K3: pick a slot"},
    [AK_STR_BIND_HINT2] = {"单击旋钮：返回  10 秒无操作退出", "Click: back  Leaves after 10 s idle"},
    [AK_STR_BIND_LOADING] = {"正在读取会话…", "Loading sessions..."},
    [AK_STR_BIND_NO_REPLY] = {"Companion 未响应", "No reply from the Companion"},
    [AK_STR_BIND_BOUND] = {"已绑定", "Bound"},
    [AK_STR_BIND_CLEARED] = {"已解除绑定", "Unbound"},
    [AK_STR_SET_WIFI] = {"已保存的 Wi-Fi", "Saved Wi-Fi"},
    [AK_STR_WIFI_EMPTY] = {"没有已保存的 Wi-Fi", "No saved Wi-Fi"},
    [AK_STR_WIFI_CURRENT] = {"当前", "In use"},
    [AK_STR_WIFI_HINT1] = {"旋转：选择  K6：删除", "Rotate: select  K6: delete"},
    [AK_STR_WIFI_HINT2] = {"K1：返回  新增请用 USB", "K1: back  Add via USB"},
    [AK_STR_WIFI_CONFIRM] = {"再按 K6 删除（3 秒内）", "Press K6 again to delete (3 s)"},
    [AK_STR_WIFI_CONFIRM_CUR] = {"再按 K6：断线后不再连接", "K6 again: not rejoined once lost"},
    [AK_STR_WIFI_DELETED] = {"已删除 Wi-Fi", "Wi-Fi deleted"},
    [AK_STR_WIFI_ADD_USB] = {"新增 Wi-Fi 只能经 USB", "Add Wi-Fi over USB only"},
    [AK_STR_PAIR_TITLE] = {"进入配网模式？", "Enter pairing mode?"},
    [AK_STR_PAIR_L1] = {"允许设置 Wi-Fi / License，", "Allows Wi-Fi / License setup"},
    [AK_STR_PAIR_L2] = {"仅 USB Companion，%u 秒内有效。", "from USB Companion for %u s."},
    [AK_STR_PAIR_L3] = {"只能经 USB，蓝牙永远", "Only over USB. BLE is never"},
    [AK_STR_PAIR_L4] = {"不能配网。", "allowed to provision."},
    [AK_STR_PAIR_CANCEL] = {"取消", "Cancel"},
    [AK_STR_PAIR_CONFIRM] = {"确认", "Confirm"},
    [AK_STR_PAIR_AUTO_CANCEL] = {"%u 秒后自动取消", "Auto cancel in %u s"},
    [AK_STR_PAIR_KEYS] = {"K6：确认   K1：取消", "K6: confirm   K1: cancel"},
    [AK_STR_PAIR_HINT] = {"旋转：选择  单击：执行", "Rotate: choose  Click: run choice"},
    [AK_STR_TEST_KEYS] = {"按键", "Keys"},
    [AK_STR_TEST_BATTERY_WAIT] = {"电池：…", "Battery: ..."},
    [AK_STR_TEST_WIFI_WAIT] = {"Wi-Fi：…", "WiFi: ..."},
    [AK_STR_TEST_BLE_WAIT] = {"蓝牙：…", "BLE: ..."},
    [AK_STR_TEST_HINT] = {"K1-K6 灯效  旋钮调节  单击：切换页", "K1-K6 effect  EC11 tune  click: view"},
    [AK_STR_TEST_EXIT] = {"双击旋钮退出", "Double-click EC11 to exit"},
    [AK_STR_TEST_BATTERY_NA] = {"电池：ADC 不可用", "Battery: ADC unavailable"},
    [AK_STR_TEST_BLE_NA] = {"蓝牙：本固件未包含", "BLE: not in this build"},
    [AK_STR_TEST_NO_SWITCH1] = {"lite / v0.3.1：本板没有", "lite / v0.3.1: no power switch on"},
    [AK_STR_TEST_NO_SWITCH2] = {"灯光电源开关（P18 未连接）。", "this board (P18 not connected)."},
    [AK_STR_TEST_LP_KEYS] = {"K1：上电   K2：断电", "K1: power on   K2: power off"},
    [AK_STR_TEST_LP_HINT] = {"单击：下一页  双击：退出", "Click: next view  Dbl-click: exit"},
    [AK_STR_FONT_TITLE] = {"字体测试", "Font test"},
    [AK_STR_FONT_HINT] = {"旋转：切换字体  单击：返回", "EC11 turn: font  click: back"},
    [AK_STR_MENU_OPTIONS] = {"选项", "Options"},
    [AK_STR_MENU_APPROVE] = {"审批", "Approve"},
    [AK_STR_MENU_QUESTION] = {"问题", "Question"},
    [AK_STR_MENU_MODEL] = {"模型", "Model"},
    [AK_STR_MENU_MODE] = {"模式", "Mode"},
    [AK_STR_MENU_INSTANCE] = {"实例", "Instance"},
    [AK_STR_MENU_SESSION] = {"会话", "Session"},
    [AK_STR_MENU_SEE_SCREEN] = {"详见电脑屏幕", "See screen"},
    [AK_STR_MENU_ARMED] = {"再按一次确认", "Press again to confirm"},
    [AK_STR_MENU_HINT_ANSWER] = {"K2-K5 回答  K1 返回", "K2-K5 answer  K1 Esc"},
    [AK_STR_MENU_HINT_MULTI] = {"K2-K5/旋钮 勾选  K6 提交  K1 返回", "K2-K5/knob tick  K6 submit  K1 Esc"},
    [AK_STR_MENU_HINT_SINGLE] = {"K2-K5 选择  旋钮/K6 确定  K1 返回", "K2-K5 pick  knob/K6 OK  K1 Esc"},
    [AK_STR_DICT_TITLE] = {"语音回答", "Dictate answer"},
    [AK_STR_DICT_HOLD] = {"按住语音键", "Hold the voice key"},
    [AK_STR_DICT_SPEAK] = {"说话", "and speak"},
    [AK_STR_DICT_CONFIRM_HINT] = {"K6 发送  语音键重说  K1 返回", "K6 send  voice key again  K1 back"},
    [AK_STR_DICT_BACK_HINT] = {"K1 返回菜单", "K1 back to the menu"},
    [AK_STR_INPUT_TITLE] = {"确认输入", "Confirm input"},
    [AK_STR_INPUT_HINT] = {"K6 发送 K4 删字 语音键重说 K1 取消", "K6 send K4 del voice:again K1 cancel"},
    [AK_STR_TEST_TITLE] = {"硬件测试", "Hardware Test"},
    [AK_STR_OTA_TITLE] = {"固件升级", "Firmware update"},
    [AK_STR_OTA_INSTALL] = {"安装 %s？", "Install %s?"},
    [AK_STR_OTA_RUNNING] = {"当前版本 %s", "Now running %s"},
    [AK_STR_OTA_KEEP_USB] = {"请保持 USB 连接。", "Keep the USB cable plugged in."},
    [AK_STR_OTA_KEPT] = {"设置和配对都会保留。", "Settings and pairing are kept."},
    [AK_STR_OTA_KEYS] = {"K6：安装    K1：取消", "K6: install    K1: cancel"},
    [AK_STR_OTA_PREPARING] = {"正在准备 %s", "Preparing %s"},
    [AK_STR_OTA_RECEIVING] = {"正在接收 %s：%u%%", "Receiving %s: %u%%"},
    [AK_STR_OTA_INSTALLING] = {"正在安装 %s", "Installing %s"},
    [AK_STR_OTA_NO_UNPLUG] = {"请勿拔出 USB。", "Do not unplug USB."},
    [AK_STR_OTA_RESTART] = {"完成后键盘自动重启。", "The keyboard restarts when done."},
    [AK_STR_OTA_FAILED] = {"升级失败：%s", "Update failed: %s"},
    [AK_STR_OTA_UPDATED] = {"已升级到 %s", "Updated to %s"},
};

ak_lang_t ak_lang_get(void)
{
    return s_lang;
}

void ak_lang_set(ak_lang_t lang)
{
    if ((unsigned)lang < (unsigned)AK_LANG_COUNT) {
        s_lang = lang;
    }
}

const char *ak_tr_lang(ak_str_id_t id, ak_lang_t lang)
{
    if ((unsigned)id >= (unsigned)AK_STR_COUNT || (unsigned)lang >= (unsigned)AK_LANG_COUNT ||
        k_str[id][lang] == NULL) {
        return "";
    }
    return k_str[id][lang];
}

const char *ak_tr(ak_str_id_t id)
{
    return ak_tr_lang(id, s_lang);
}

const char *ak_lang_name(ak_lang_t lang)
{
    return lang == AK_LANG_EN ? "English" : "中文";
}

const char *ak_lang_wire_str(ak_lang_t lang)
{
    return lang == AK_LANG_EN ? "en" : "zh";
}

bool ak_lang_from_wire(const char *s, ak_lang_t *out)
{
    if (s == NULL || out == NULL) {
        return false;
    }
    if (strcmp(s, "zh") == 0) {
        *out = AK_LANG_ZH;
    } else if (strcmp(s, "en") == 0) {
        *out = AK_LANG_EN;
    } else {
        return false;
    }
    return true;
}

uint8_t ak_lang_to_byte(ak_lang_t lang)
{
    return lang == AK_LANG_EN ? 1U : 0U;
}

bool ak_lang_from_blob(const uint8_t *blob, size_t len, ak_lang_t *out)
{
    if (blob == NULL || out == NULL || len != 1U || blob[0] >= (uint8_t)AK_LANG_COUNT) {
        return false;
    }
    *out = (ak_lang_t)blob[0];
    return true;
}

const char *ak_lang_agent_param(ak_lang_t lang)
{
    return lang == AK_LANG_EN ? "{\"lang\":\"en\",\"lang_name\":\"English\"}"
                              : "{\"lang\":\"zh\",\"lang_name\":\"Simplified Chinese\"}";
}
