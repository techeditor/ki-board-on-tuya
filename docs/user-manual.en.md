# Ki-board User Manual

Ki-board is an AI keyboard for Kiro Crew users. It moves control of your Kiro Crew agents from the screen to your fingertips. It is both a fast way to type prompts (hold a key to talk, confirm on the keyboard, send) and a physical control for the agents (switch sessions, approve, pick replies, change the model). It supports Kiro Crew only.

This manual is for people who have a keyboard in hand: how to set it up, connect it, use it, and fix common problems. Key, light, menu and screen rules are all here too. Developer notes are in the [firmware README](../firmware/app/README.md).

> Scope: firmware 0.3 series, Companion for Windows. Screen texts below are quoted from the English interface. The Chinese edition is [user-manual.md](user-manual.md).

## Contents

1. [Know your keyboard](#1-know-your-keyboard)
2. [Getting started](#2-getting-started)
3. [Everyday use](#3-everyday-use)
4. [The keyboard Settings page](#4-the-keyboard-settings-page)
5. [The Companion app](#5-the-companion-app)
6. [Wi-Fi and pairing](#6-wi-fi-and-pairing)
7. [Updates](#7-updates)
8. [Troubleshooting and FAQ](#8-troubleshooting-and-faq)

---

## 1. Know your keyboard

### 1.1 Keys and knob

The keyboard has 6 keys (K1–K6), 1 knob and 1 screen. Each key has a light under it. A seventh light sits next to the knob.

```text
  ┌──────────────────────────────────┐
  │ Ki-board v0.3.x   Wi-Fi USB BT bat │  top bar
  ├────┬─────────────────────────────┤
  │ A  │                             │
  │ B  │    chat and status area     │
  │ C  │                             │
  ├────┴─────────────────────────────┤
  │ status bar                       │
  └──────────────────────────────────┘
     K1   K2   K3        ( knob )
     K4   K5   K6
```

| Part | Default job |
|---|---|
| K1 / K2 / K3 | Single click: select chat A / B / C, effective on release. Long press for about 0.5 s: switch to Kiro Crew 1 / 2 / 3 (this computer or a remote one). Neither touches the text in the input bar |
| K4 | With text in the input bar: single click deletes the last character, long press clears it. With a request you dismissed (ESC): single click brings it back |
| K5 | Voice key: hold to talk, release to finish |
| K6 | Single click: send the text in the input bar (only when the bar has text or the computer has been filled in). Double click: toggle between Auto and the model you last picked by hand. Long press: open the model / permission-mode menu |
| Knob | Turn: scroll in the Kiro Crew window on your computer. Click: Enter on the computer. Long press: open Settings |

Normal keys and the knob button recognise three gestures:

| Gesture | How |
|---|---|
| Single click | Press and release once. **If the key has no double click set, it takes effect on release.** A key with a double click (K6 by default) waits an extra 0.25 s to be sure you do not press a second time |
| Double click | Press again and release within 0.25 s of the first release |
| Long press | Hold until the time is up. It takes effect at once, you do not have to release. K1–K3 need 0.5 s, other keys and the knob 0.8 s |

One action counts as one gesture. When you hold K1–K3 to 0.5 s, that key's light turns blue, the status bar shows "Switching to Instance N" and it switches at once. Releasing before 0.5 s is only a single click and does not switch.

K5 has no gestures: pressing starts talking and releasing ends it. **Hold it down** and release when you are done.

Apart from the knob long press (always Settings), key functions can be changed in the Companion under "Key mapping". The display names of the three chat slots (A / B / C by default) can be set in the Companion too.

### 1.2 Screen

- **Start-up screen**: when the keyboard starts, it shows a start-up screen for about 1.5 seconds, or about 4 seconds on a keyboard with a custom start-up screen. Press any key or turn the knob to skip it; that key press does nothing else.
- **Top bar**: "Ki-board" and the firmware version on the left. Wi-Fi, USB and Bluetooth icons and the battery on the right. The bar is purple while the Companion is connected and black otherwise.
- **Three cards on the left**: the chats bound to K1–K3, named A / B / C. The selected card is purple. The dot at the top right of a card shows the chat state:

  | Dot | State |
  |---|---|
  | Dark grey | Not bound |
  | Green | Idle |
  | Red | Busy |
  | Orange | Waiting for you |
  | Light purple | New information, nothing to do |

- **Status bar**: the last line of the screen, for example "WiFi connected" or "Listening...". Errors are red, warnings yellow, success green.
  - While the keyboard joins a saved Wi-Fi at start-up, or rejoins after a drop, it shows "Connecting Wi-Fi...", then "WiFi connected" once it is on.
  - If it is still not connected after 30 s (and no other saved network took over), it shows "Warning: WiFi disconnected". The keyboard keeps retrying in the background.
  - While the Bluetooth pairing window is open, the status bar keeps the pairing prompt and is not covered by the Wi-Fi state. The Wi-Fi state shows again after the window closes.

### 1.3 Light colours

When a menu is up, **only the keys you can press are lit**. The others are dark. When the menu closes, the lights go back to showing chat state.

| Colour | Meaning |
|---|---|
| Blue | A normal choice |
| Green | Confirm |
| Red | Cancel or reject. The red K1 is "back / ESC" |
| Yellow | Attention. Steady: an option to watch. Fast flash: press again to confirm. Breathing: a chat is waiting for you, or a dismissed request is parked (K4) |
| Purple | Voice |

Also:

- K1–K3 breathing yellow: that chat is waiting for you.
- With text in the input bar, or after the computer has been filled in: K4 breathes red (delete), K6 flashes green (send).
- In a menu the highlighted item is at full brightness and the rest at about 40%. The light next to the knob shows the colour of the item a knob press would pick.
- In Settings you can set the lights to "Off", "On" or "Auto". The default is "Auto": the lights go out after 60 s idle (30 s, 60 s, 5 min or 15 min can be chosen) and come back on any key press or knob turn.

### 1.4 Power and battery

- The keyboard has a built-in lithium battery. Switch it on with the slide switch on the **left side** of the keyboard (on the dev board). You can also power and charge it through USB from a computer or charger.
- The battery icon at the top right shows the charge in percent. Below 20% the fill turns red.
- The percentage is estimated from the battery voltage, not measured exactly. When you plug in or unplug the charger the reading is re-calibrated and may jump. That is normal.
- At 15% or less, the small animation on the chat cards pauses. It resumes when you plug in the charger.

### 1.5 Working states

The keyboard is in exactly one of these states at any time.

| State | When it starts | How it ends |
|---|---|---|
| Home | Power-on; after the other states end | Hold K5 to talk; a request arrives; long-press K6; long-press the knob |
| Voice input | Hold K5 | Back to Home once the text is recognised (it goes into the input bar); also back to Home if nothing was recognised, it failed or timed out |
| Voice-answer confirm | You chose the voice-answer item in a menu and finished speaking | K6 sends; K1 or 20 s without a key goes back to the menu |
| Menu | A permission approval, question or reply options arrive | A choice, ESC (K1), the request was handled elsewhere, a timeout |
| Model / mode menu | Long press K6 | Same as above |
| Settings / test | Long-press the knob | Choose "Exit" |
| System confirm | Opening the pairing window or Change Wi-Fi, unbinding, resetting the system, clearing Bluetooth pairing, the Bluetooth pairing code, a firmware update | Confirm, cancel, or 10 s without a key (30 s for the pairing code) |

**What covers what**: a system confirm covers everything, then Settings, then the other states.

For a system confirm, **releasing K6 confirms** (pressing does not), and K1 cancels. It rests on "Cancel" so a stray press does nothing.

---

## 2. Getting started

### 2.1 What you need

- A Windows computer with Kiro Crew installed and running.
- The USB cable that came with the keyboard. It must carry data. A charge-only cable does not work.
- A **2.4 GHz** Wi-Fi network. The keyboard cannot join 5 GHz.
- The Tuya app (Tuya Smart or SmartLife) on your phone, for the first binding.
- A product on the Tuya IoT platform (it gives you the Tuya PID), and the Tuya UUID and AuthKey for this keyboard. You do not need them if you only control Kiro Crew. See 2.5.

Without internet, the keyboard and the Companion can still control Kiro Crew. You need internet for the keyboard's own cloud speech recognition and for the "Chat" voice mode.

### 2.2 Get the Companion and the firmware

**No prebuilt Companion or firmware is offered for now** (they are not stable yet). Build them from source as the repository README describes:

- Firmware: see the Build and Flash sections of the [firmware README](../firmware/app/README.md). Build with the release profile (`TUYA_T5AI_CORE_dual_cloudasr`). The debug profile is for development only. Do not use it on a keyboard you use every day.
- Companion (Windows): see the Build section of the repository [README](../README.md).

Close the Companion before flashing. It holds the serial port.

### 2.3 Connect the keyboard

**By USB (simplest, and required the first time)**

1. Plug the keyboard into the computer with the USB cable.
2. Open the Companion. "Auto-connect" is on by default, so it finds the keyboard and connects.
3. The top right shows "Online · USB", the keyboard's top bar turns purple, and the status bar says "Companion connected via USB".

**By Bluetooth (use it unplugged)**

Bluetooth must be paired with this computer once. **You need no USB cable, and the keyboard does not have to be bound to Tuya first.** Pair directly over Bluetooth:

1. Turn on Bluetooth on the computer. In the Companion's "Connection" page, set "Transport" to "Auto" or "BLE", click "Scan" and pick your keyboard. Its Bluetooth name is `Ki-board-XXXXXX`. The last six characters are the last six digits of the keyboard's Wi-Fi MAC address, which you can check on the keyboard under Settings > Device info.
2. On the keyboard, hold the knob to open Settings, choose "BLE pairing" and click the knob. The item shows "OPEN m:ss". The window lasts 120 s.
3. Back in the Companion, click "Pair over Bluetooth" under the Bluetooth device. The keyboard shows "Pair over BLE?" with a 6-digit code, and the Companion shows a code too.
4. **Check that the two codes are the same.** If they are, click "Codes match" in the Companion and press **K6** on the keyboard. If they differ, or it was not you, press K1 on the keyboard to cancel. 30 s without an answer cancels too.
5. The keyboard says "BLE paired with this Companion". Click "Connect" and you can use Bluetooth. You do not need to pair again.

Notes:

- One window pairs one computer, and the window closes after 3 failed attempts. To try again, open it again on the keyboard.
- A new pairing replaces the old one. A computer paired earlier has to pair again.
- If the window is not open when you click "Pair over Bluetooth", the keyboard does not react and the Companion tells you to open Settings > BLE pairing first.
- You can also pair over USB: open the "Pairing mode" window on the keyboard first (see 6.3), then click "Pair" under "Bluetooth pairing" in the Companion's "Advanced settings".

Auto-connect tries USB first, then the paired Bluetooth keyboard. After a drop it retries at 2, 4, 8 and 16 s, then every 30 s. If you click "Disconnect" yourself, it stops retrying until you click "Connect" or restart the Companion.

The keyboard also turns Bluetooth on when it is not bound to Tuya. Only while the 180 s "Pairing mode" window is open is Bluetooth left to the Tuya app, and the Companion cannot connect until the window closes. While USB is connected, the keyboard prefers USB.

### 2.4 Connect to Kiro Crew

1. Open Kiro Crew and sign in. Keep it running.
2. The Companion connects to Kiro Crew on this computer by itself, through Kiro Crew's local interface (loopback only): it reads chat state and sends prompts and approval replies there, after checking that the other end is the Kiro Crew running under your own user. On the "Control" page, "Live link" shows "Connected" and your chats are listed.
3. On the "Input" page, check that "Board keys drive Kiro Crew" is "On". When it is off, the keys do nothing on the computer.

If the Companion says "Crew identity check failed", see the [FAQ](#the-companion-says-crew-identity-check-failed).

### 2.5 Write the Tuya PID and UUID/AuthKey (needed for the Tuya cloud)

A new keyboard ships **without** the Tuya PID (product ID), Tuya UUID and Tuya AuthKey. Without them the Tuya cloud is off: the status bar says "No product ID: write it over USB", and cloud speech recognition and "Chat" mode do not work. The keyboard itself, the screen, the Companion (USB and Bluetooth) and PC speech recognition are not affected. If you only control Kiro Crew, skip this section.

To use the Tuya cloud, write them over USB first (USB only, never Bluetooth). The UUID and AuthKey are this device's own authorization. Keep them private and never paste them into an issue. In the steps below, `<...>` stands for your own value:

1. Connect the keyboard to the Companion over USB.
2. **Write the PID**: on the Companion's "Advanced settings" page, find the "Tuya PID / UUID / AuthKey" card. Enter `<your Tuya PID>` in "Tuya PID" (16 lowercase letters and digits, from the product you created on the Tuya IoT platform) and click "Write PID". The keyboard accepts a PID only **while it has none**. It takes effect after you **restart the keyboard** (the status bar tells you).
3. **Open the pairing window**: on the keyboard, hold the knob to open Settings, choose "Pairing mode" and press K6. A keyboard without a Tuya UUID/AuthKey opens the 180 s window at once.
4. **Write the UUID and AuthKey**: while the window is open, enter `<your Tuya UUID>` (20 characters) and `<your Tuya AuthKey>` (32 characters) on the same card and click "Write Tuya UUID/AuthKey". The keyboard says "Tuya UUID/AuthKey saved: restart". Restart the keyboard.
5. Open Settings > Device info. If Tuya PID, Tuya UUID and Tuya AuthKey all show "SET", you are done. The screen never shows the values.
6. Continue with 2.6 to get online and bind to Tuya.

Notes:

- If the keyboard already holds a Tuya UUID/AuthKey, writing a new one is rejected with "Tuya UUID/AuthKey set: allow overwrite". To replace it, with the window open go to Settings > Device info and press K6 to allow one overwrite (valid for 60 s).
- The PID can be written once and cannot be changed on the keyboard. To use another PID, first clear it with "Settings > Reset system" (see section 4), then write the new one.
- **"Reset system" clears the Tuya PID, UUID and AuthKey together.** The Tuya cloud is off again afterwards, and you must write them again as above. "Unbind device" and the K1+K6 recovery combination do not clear them.

### 2.6 Get online and bind to Tuya

The keyboard needs the Tuya PID and UUID/AuthKey from 2.5, and then a Tuya binding, for cloud speech recognition and "Chat" mode. There are two ways. Pick one. The keyboard joins 2.4 GHz Wi-Fi only.

**Way 1: Tuya app over Bluetooth (no computer needed)**

1. On the keyboard, hold the knob to open Settings, choose "Pairing mode", and press K6. The keyboard restarts into a 180 s pairing window. The status bar says "Pairing mode: use Tuya app".
2. On the phone, turn on Bluetooth, open the Tuya app, and choose "Add device". Find the keyboard among nearby devices, follow the prompts, and enter the name and password of your 2.4 GHz Wi-Fi.
3. The screen shows "Provisioning complete". The keyboard is bound.

**Way 2: USB and the Companion**

1. Connect the Companion over USB, open Settings on the keyboard, choose "Pairing mode", and press K6.
2. In the Companion's "Connection" page, find "Wi-Fi and SmartLife". Enter the Wi-Fi name (2.4 GHz) and password, and click "Send to keyboard".
3. When the keyboard has joined the Wi-Fi, the Companion shows a QR code. Scan it with the Tuya app to finish binding.

Every Wi-Fi network the keyboard joins successfully is remembered, including the one set up through the Tuya app. See [section 6](#6-wi-fi-and-pairing).

> If the status bar says "No Tuya UUID/AuthKey: use USB" or "No product ID: write it over USB", they are not written yet. See 2.5.

You can use the keyboard without the Tuya cloud (control Kiro Crew, PC speech recognition, Bluetooth), and no binding is needed. The Tuya PID, Tuya UUID and Tuya AuthKey rows in Settings > Device info are for the Tuya cloud only. Ignore them if you do not use it.

---

## 3. Everyday use

### 3.1 Switch sessions

The three cards A / B / C on the left are three Kiro Crew sessions.

| Action | Effect |
|---|---|
| Single click K1 / K2 / K3 | Select chat A / B / C, effective on release. Text you speak next goes to this chat |
| Long press K1 / K2 / K3 (about 0.5 s) | Switch to Kiro Crew 1 / 2 / 3 (this computer, or a remote Kiro Crew you set up). The key turns blue and the status bar shows "Switching to Instance N" |

Neither action changes text that is in the input bar.

After a single click, the Companion brings the Kiro Crew window to the front by default. To stop that, turn off "Bring Kiro Crew to the front when K1–K3 picks a chat" on the Companion's "Input" page.

### 3.2 Bind a session

To make K1–K3 point at the sessions you want:

1. Hold the knob to open Settings and choose "Bind session".
2. The screen lists slots A / B / C and K1–K3 light up blue. Press K1 / K2 / K3 to pick a slot.
3. If you have remote Kiro Crews, first choose "This computer" or a remote one.
4. In the session list press K2–K5 to pick, turn the knob to change page, and press K1 to go back. Bots (names starting with `@`) come first, then normal sessions. The last item, "Unbind", clears the slot.
5. When it works, the slot's key light flashes green and you return to Settings.

You can also do this on the Companion's "Control" page under "A / B / C keys". Worker sessions started by a Bot are hidden by default. To list them, turn on "List worker sessions when binding on the keyboard" on the "Input" page.

Sessions of a remote Kiro Crew show only their state colour. A single click on such a slot says "Remote chat: status only". Send messages and approve on the computer.

### 3.3 Voice input

1. **Hold K5** and speak. The status bar says "Listening...".
2. Release when done. The keyboard shows "Recognizing...", and within a few seconds the recognised text appears in the **input bar** at the bottom of the screen.
3. Look at the text. If it is right, press **K6** to send.

The text is never sent by itself. You must press K6.

**The input bar**

- It sits at the bottom of the screen, above the status bar. It has 2 lines by default and 3 for longer text. Beyond that, scroll with the knob (a thin scroll bar shows on the right). It fits at 12, 14 and 16 px.
- You edit only at the end, there is no cursor. Hold K5 and speak again to add more text at the end (a space is added between two English words or digits).
- **K4 single click** deletes the last character, **K4 long press** clears the bar, and the bar closes when it is empty. **K6 single click** sends. There is no separate "cancel" key: to drop the text, long-press K4.
- K1–K3 keep their meaning: a single click switches to that chat and leaves the text alone. If you notice you picked the wrong chat after recording, pick another and press K6. Switching Kiro Crew with a K1–K3 long press keeps the text too.
- There is no timeout. The text stays until you send or clear it. The limit is about 126 Chinese characters or 380 English characters. Extra text is not kept, and the status bar says "Input full: extra words dropped".
- If sending fails (Kiro Crew is closed or signed out, or this key has no chat bound), the text stays in the bar, the status bar shows the reason in red, and you can press K6 again ("Not sent: K6 to retry"). When the keyboard is not connected to the Companion, nothing is sent either and the text is kept ("Companion not connected; text kept").
- A request that arrives pops up at once. The text in the bar is kept and shown again after the menu ends. Settings and system confirmations do not lose the text either.

**Where the text goes**

Each chat slot has its own destination. You do not switch anything by hand:

| Destination | Which slots | Where the text goes | How to send |
|---|---|---|---|
| Keyboard input bar (default) | A slot with no Alt+N number set, or when "Voice text" on the Companion's "Input" page is set to "All chats use the keyboard" | The keyboard input bar | K6: sent through the Kiro Crew interface to the chat selected with K1–K3. It does not matter which chat is open on the computer, and the Kiro Crew window need not be in front |
| Computer input box | A slot with an Alt+N number set, while the Companion's "Board keys drive Kiro Crew" is on and the Kiro Crew window can be used | The Companion switches to that chat and types the text into the Kiro Crew input box without sending. The keyboard does not change page; the status bar shows "Filled in: K6 send / K4 clear" | K6, or Enter on the computer. K4 deletes a character, K4 long press clears the box. Valid for 20 s, and void if you pick another chat meanwhile |

If the computer path cannot work (the keys are switched off, the Kiro Crew window is not in front, typing fails), the text goes into the keyboard input bar instead and the status bar says why. The Alt+N number is set on the Companion's "Control" page.

**Two ways to recognise speech** (choose under "Voice recognition" on the Companion's "Input" page):

| Way | Recording | Notes |
|---|---|---|
| Keyboard cloud (Tuya) | Keyboard microphone | The keyboard must be on Wi-Fi, hold the Tuya UUID/AuthKey and be bound to Tuya. The text goes straight into the input bar |
| PC recognition | Computer microphone | Engines: Doubao, Alibaba Model Studio, or Local (offline). Doubao and Alibaba need your own API key. Local needs a one-time model download (about 245 MB). After release the keyboard stays at "Recognizing..."; when the result arrives it goes into the input bar and the status bar shows the engine name in green, for example "PC ASR: local" |

Details of PC recognition:

- While it waits for the result, **click K1 once to cancel**. The status bar shows "Input cancelled". With no result after 15 s it returns to the home screen with "Warning: ASR timeout". A result that arrives after a cancel or timeout never shows up.
- If nothing was heard or it failed, the status bar shows the reason in orange.
- With "PC recognition", the Companion can fall back to local offline recognition when the online engine has no key or fails. Tick "Use local offline recognition when the cloud fails or has no key". The local model must already be downloaded.

**When the keyboard's cloud recognition is unavailable, PC recognition is used automatically.** This happens with no Tuya UUID/AuthKey, a keyboard that is not activated or paired, an open pairing window, or Wi-Fi or the Tuya cloud being down. As long as the Companion is connected (USB or authenticated Bluetooth), holding K5 goes straight to PC recognition. With no Companion, holding K5 only shows "Voice needs Companion or pairing" and nothing is recorded. If the Companion is running but "Board keys drive Kiro Crew" is off, you see "Voice needs keyboard control".

**Two voice modes** (Settings > "Voice mode", or "Keyboard settings" on the Companion's "Device settings" page):

- "ASR": the recognised text goes into the input bar and you send it to Kiro Crew. This is the default.
- "Chat": you talk with the chat assistant in the Tuya cloud. The reply appears on the keyboard screen and is played aloud. It is not sent to Kiro Crew. There is no input bar. The keyboard must be online and bound to Tuya. If the Tuya cloud is briefly unavailable, that one press of K5 is treated as "ASR", the status bar says "Cloud down: speech input", and Chat comes back when the cloud does. The saved voice mode does not change.

Voice-related status messages (listening, recognizing, the recognition source, input cancelled) cover the standing "Pairing mode" and "PC ASR" hints and go away after about 4 s.

### 3.4 Approvals, questions and reply options

When Kiro Crew needs a decision, the keyboard shows a menu and takes over the keys. You can answer without touching the computer.

**How to choose**:

| Input | Single-choice menu | Multi-choice menu |
|---|---|---|
| K2 / K3 / K4 / K5 | Pick item 1–4 of the current page (the screen shows which key) | Tick / untick that item |
| Turn knob | Move the highlight, flip pages at the end | Same |
| Press knob | Pick the highlighted item | Tick / untick the highlighted item |
| K6 | Confirm the highlighted item | Submit all ticked items |
| K1 | ESC: close the menu without deciding | Same |

Keys are ignored for the first 0.3 s after a menu appears, to prevent accidents.

**Kinds of menu**:

- **Permission approval**: the options are "Approve once", "Reject" and "Reject once". An agent often asks for several tool calls at once (a batch). "Reject" refuses this call and also every other call in the same batch that is still waiting, and the agent is told you refused. "Reject once" refuses only this call. The other calls in the batch carry on as normal (each one still asks you if it needs approval). Neither choice is remembered: a later request asks again. **K6 and the knob button do nothing in an approval menu.** You must press the key of the option, so a slip cannot approve something.
- **Questions and reply options**: the options are the answers Kiro Crew offers. A purple voice-answer item appears only when you use the keyboard's cloud recognition. Select it, hold K5 and say the answer. A confirm page follows: press **K6** to send the text as the answer, or K1 (or wait 20 s) to go back to the menu.
- **Options that need a second press** (for example "Trust" in the permission-mode list): after the first press the key flashes yellow and the screen says "Press again to confirm". Press the same key again within 3 s. Pressing another key, turning the knob or waiting cancels it.
- **Long content** (code changes, long questions) is not shown on the keyboard. The screen says "See screen". Read it on the computer.

**Dismissed requests**: if you close a menu with K1, the request is still open. You just did not decide. Back on the home screen K4 breathes yellow. Click K4 to bring the menu back.

**Queue**: requests that arrive while you are speaking, waiting for a recognition result or on the voice-answer confirm page do not grab the screen. They wait and pop up afterwards, and K4 breathes yellow meanwhile. The input bar does not block requests: a request pops up at once, the text in the bar is kept and shown again when it is done. The title bar shows the position, such as `1/3`. When several wait, they show in this order: system confirm, permission approval, question, reply options, and in order of arrival within the same level.

### 3.5 Change model and permission mode

- **Double click K6**: toggle between Auto and the model you last picked by hand. If you never picked one, the screen says "No model picked yet".
- **Long press K6**: open one menu for the selected chat. The available models are on the first pages and the permission modes on the last page. The active item is ticked.
  - If the chat is busy, a model change shows "Model: next turn".
  - The three permission modes are "Normal", "Reads" and "Trust". "Trust" needs a second press.
  - The keyboard cannot set looser modes. Those can only be set in Kiro Crew.

### 3.6 Knob

| Action | Effect |
|---|---|
| Turn | Scroll up and down in the Kiro Crew window on the computer (clockwise is down). In a menu, move the highlight |
| Click | Press Enter on the computer |
| Long press | Open Settings |

In "Chat" voice mode, turning the knob scrolls the conversation on the keyboard screen. Other long text that does not fit scrolls line by line the same way: one notch clockwise is one line down, and a thin scroll bar shows on the right (not when the text fits). At the top the status bar briefly says "Top of text", at the bottom "End of text". The keyboard has no vibration motor, so nothing vibrates.

What turning the knob does on each screen:

| Screen | Knob turn |
|---|---|
| The "Chat" mode conversation page | Scrolls the conversation. While you look at the newest line, a longer reply follows along. After you scroll up, new content does not move the view and "New below" shows at the right of the title bar until you turn back to the bottom. A new round starts at the newest line |
| Home, with text in the input bar | Scrolls the input bar when the text is taller than the bar, and stops at the end. If it fits, it does what the next row says |
| Voice-answer confirm | Scrolls the text from the first line when it is taller than one screen. Does nothing if it fits |
| Home (ASR mode) | Scrolls on the computer. The history on the screen shows only the latest few lines |
| Menus | Moves the highlight and flips pages past the end. A question or reply description has at most 3 lines, with `...` when cut, and long content says "See screen" |
| Settings, Saved Wi-Fi | Moves the selection. The list follows it and a scroll bar shows on the right |

The number of lines per screen depends on the font size, and scrolling counts lines of the current size. After you change the font size, the conversation page returns to the newest line.

---

## 4. The keyboard Settings page

**Open**: long-press the knob. **Use**: turn the knob to choose, click to enter or confirm, choose "Exit" to leave. For a value, click to start editing, turn to change, click again to confirm. The screen shows "Applied".

From top to bottom:

| Item | What it does |
|---|---|
| Volume | Steps of 10% |
| Brightness | Screen brightness, steps of 10% |
| Voice mode | "ASR" or "Chat", see 3.3 |
| LED power | "Off", "On" or "Auto". Default "Auto" |
| LED auto-off | Only used in "Auto". Lights go out after 30 s, 60 s, 5 min or 15 min idle, and come back on a key, knob or menu. Default 60 s |
| Language | 中文 or English. The keyboard screen and the Companion both follow |
| Font size | 12, 14 or 16 px. Default 12 px. Choose a bigger one if the text is hard to read |
| Animation | On or off. The small ghost animation on the chat cards |
| Scene | Click to open the scene picker. The current scene name is shown on the right. A `*` after the name means you changed voice mode or lights after applying the scene. A scene is a set of key functions, voice mode and light settings, edited on the Companion's "Scenes" page |
| Bind session | Bind Kiro Crew sessions to K1–K3, see 3.2 |
| Saved Wi-Fi | The Wi-Fi networks the keyboard remembers, see below |
| Device info | Read-only status of the keyboard's connection, Wi-Fi, Bluetooth and device, see below |
| BLE pairing | Opens a 120 s window so you can pair with a computer over Bluetooth without USB, see 2.3 |
| Pairing mode / Change Wi-Fi | "Pairing mode" before the keyboard is bound to Tuya, "Change Wi-Fi" after. See [section 6](#6-wi-fi-and-pairing) |
| Unbind device | Clears the Tuya binding. See [section 6](#6-wi-fi-and-pairing). **It only appears once the keyboard is bound to Tuya.** A keyboard that is not bound has no such item in Settings. Use "Pairing mode" to set it up |
| Reset system | Turns the keyboard back into a new one: settings go back to default, and saved Wi-Fi, Tuya activation, Tuya PID/UUID/AuthKey and Bluetooth pairing are cleared. You press K6 twice to confirm, see below |
| Test hardware | Checks keys, knob, lights, battery, Wi-Fi and Bluetooth. Follow the page. Double-click the knob to exit |
| Exit | Back to the home screen |

**Reset system**

Use it when you hand the keyboard on, or want it back in the state you first received it.

1. In Settings select "Reset system". The bottom of the screen says what will be cleared. Click the knob.
2. The first red confirm box "Reset the system?" lists what will happen:
   - **Back to default**: volume, brightness, LEDs, voice mode, language, font size, animation, key bindings and scenes.
   - **Cleared**: saved Wi-Fi, Tuya activation, Tuya PID / UUID / AuthKey, BLE pairing, Companion pairing.
3. Press **K6** to go on, or K1 to cancel. A second red box "Clear all of this?" appears. Press **K6** ("reset") once more to really do it. Each box cancels after 10 s without a key press. Cancelling at any step changes nothing.
4. The screen shows "Resetting, restarting..." and the keyboard restarts.

Good to know:

- If the keyboard is activated and online, it first unbinds itself in the Tuya cloud. **When offline it only clears itself locally**, and you have to delete the old device in the Tuya app yourself.
- Afterwards the Tuya PID, UUID and AuthKey are gone and the Tuya cloud is off. Write them again as in 2.5 when you need it.
- Bluetooth pairing is cleared, so the Companion must pair again: use "Settings > BLE pairing", or connect USB (see 2.3).
- The firmware's anti-rollback version record is kept. A reset does not allow installing an older version.
- To change Wi-Fi or switch Tuya account you do not need a reset. See section 6.

**Saved Wi-Fi**

- Lists the networks the keyboard remembers. The one in use is marked "In use". The title shows how many are stored, for example `3/8`.
- Turn the knob to select and press **K6 to delete**. After the first K6 press, K6 flashes yellow. Press K6 again within 3 s to really delete. K1 goes back.
- Deleting the network in use keeps the connection until it drops. The keyboard does not rejoin it afterwards.
- New networks can only be added over USB (see section 6).

**Device info**

- A read-only page in four groups: **Connection** (whether the Companion is on USB, Bluetooth or not connected, and whether the handshake is done; whether the Tuya cloud is active, inactive or offline), **Wi-Fi** (name, IP address, signal strength, Wi-Fi MAC), **Bluetooth** (Bluetooth MAC, Bluetooth name, pairing state) and **Device** (firmware version, Tuya PID, Tuya UUID, Tuya AuthKey).
- It is longer than one screen. Turn the knob to scroll. There is a scroll bar on the right. It refreshes when something changes. K4 or a knob click goes back.
- With no Wi-Fi, the name, IP and signal show `--`. That is normal. The Wi-Fi MAC and Bluetooth MAC are still shown.
- Tuya PID, UUID and AuthKey only show "SET" or "NOT SET". The values are never shown. They are for the Tuya cloud only. Ignore them if you do not use it. The Bluetooth pairing key is never shown either.
- **Press K1 twice** to clear the Bluetooth pairing (when paired). Afterwards you must pair again.
- While the "Pairing mode" window is open and the keyboard already holds a Tuya UUID/AuthKey, K6 allows one overwrite (valid for 60 s). You rarely need it.

The Companion's "Device settings" page has the same settings under "Keyboard settings" (voice mode, key lights, auto-off time, screen font size, chat icon animation). Volume and brightness are in "Volume and brightness" above it, and "Device info" below shows the same as the keyboard. You can change them over USB or over paired Bluetooth. The keyboard and the Companion follow whichever changed last.

---

## 5. The Companion app

> This section describes the new Companion interface (single column, auto-connect USB then Bluetooth). Button positions in older versions differ.

The Companion is available for Windows only. At the top of the window you see:

- The title "Ki-board" and "Companion for Kiro Crew".
- The connection state, such as "Online · USB" or "Offline".
- The "Simple" and "Advanced" modes.
- A gear icon for theme and interface language.

### Simple mode

Just these: connect / disconnect the keyboard, the current voice mode, the chat being watched, and a quick scene switch. This is enough for everyday use.

### Advanced mode

Seven pages on the left:

| Page | What is on it |
|---|---|
| Connection | Transport (Auto / USB / BLE, board port, Bluetooth scan, auto-connect). With Bluetooth selected, "Pair over Bluetooth" sits under the device (pairing without USB, see 2.3). Pairing window. "Wi-Fi and SmartLife". Saved Wi-Fi |
| Input | The "Board keys drive Kiro Crew" master switch. Where voice text goes. Voice recognition way, engine, API key, local model, hotwords. Key mapping |
| Scenes | The scene list: create, switch, import and export, and the favourites on the keyboard |
| Control | Kiro Crew link state and chat list. "A / B / C keys" binding. Watched chats. The "legacy shortcut" part at the bottom (key injection into the Kiro Crew window, with Kiro Crew's status hooks for status) is a fallback for when Kiro Crew's local interface cannot be reached. You normally do not need it |
| Device settings | Volume and brightness. Keyboard settings. "Device info" (the same as the keyboard's Device info page, with copy buttons) |
| Advanced settings | Bluetooth pairing (over USB). Writing the Tuya PID, UUID and AuthKey. Unbind device. Firmware update. Rarely needed |
| Logs | The event stream between keyboard and Companion. Look here when troubleshooting |

**What needs USB**: writing Wi-Fi, writing the Tuya PID and UUID/AuthKey, opening the pairing window, Bluetooth pairing and clearing it over USB, unbinding, deleting saved Wi-Fi, and firmware update. (Pairing over Bluetooth without USB is the exception. You confirm it on the keyboard with a code.) Over Bluetooth these buttons are greyed out and only show state. Opening the pairing window, unbinding, clearing the keyboard's Bluetooth pairing and firmware update also need a K6 confirmation on the keyboard.

---

## 6. Wi-Fi and pairing

### 6.1 How the keyboard remembers Wi-Fi

- The keyboard remembers up to **8** networks. A network is stored only after the keyboard has joined it and received an address.
- The hotspot you used during Tuya app pairing is remembered as well.
- When the list is full, a new network replaces the one **not used for the longest time**.
- After a drop, the keyboard scans and joins the strongest saved network. Back in a place you have been, you do nothing.
- Passwords are stored on the keyboard only. Neither the Companion nor the screen shows them.
- 2.4 GHz only.

### 6.2 Moving to a different Wi-Fi: three ways

| Situation | What to do | Tuya binding |
|---|---|---|
| ① Back to a place you have been | Do nothing. The keyboard remembers the Wi-Fi networks it has joined and reconnects by itself | Kept |
| ② You have a computer and USB cable | Connect USB. On the keyboard: Settings > Change Wi-Fi, press K6. In the Companion's "Connection" page, "Wi-Fi and SmartLife", enter the new Wi-Fi and click "Send to keyboard". The keyboard goes online at once, no restart | **Kept** |
| ③ No USB | On the keyboard: Settings > Unbind device, press K6. After the restart, set up Wi-Fi through the Tuya app over Bluetooth | **The old binding is lost.** Add the device again in the Tuya app |

**Way ① needs no action**: the keyboard remembers up to 8 networks it has joined. When the list is full, the one not used for the longest time is replaced. The network used for Tuya app pairing counts too. Back in a place you have been, it reconnects by itself after a drop.

**Way ② in practice**: see the table. The "Change Wi-Fi" confirm box says "stays bound" and the window lasts 180 s. If the new Wi-Fi cannot be joined, the keyboard says "WiFi failed, restored previous".

**Only way ③ needs the unbind**, and only on a keyboard that is already bound to Tuya. "Unbind device" clears the Tuya binding and the current Wi-Fi, keeps the Tuya UUID/AuthKey and the saved Wi-Fi list, and restarts into the pairing window. You also use it to **switch to another Tuya account** or to start from scratch. A new keyboard that was never bound has no "Unbind device" in Settings. Use "Pairing mode".

**Steps to pair again after unbinding**:

1. In Settings choose "Unbind device" (or hold K1 + K6 for about 5 s) and press K6 to confirm. The keyboard restarts into the 180 s pairing window.
2. On the phone open the Tuya app, follow the prompts to add the device, pick the new Wi-Fi (2.4 GHz only) and enter its password.
3. When pairing succeeds the keyboard is ready again, and the new network goes into the saved list.

The keyboard has a short help page too: with "Change Wi-Fi" selected in Settings, or on the "Saved Wi-Fi" page, press **K4** to open "Change Wi-Fi environment". K1, K4 or a knob click goes back.

**Why no Wi-Fi over Bluetooth?** The Companion cannot write Wi-Fi over Bluetooth. Writing Wi-Fi and the Tuya UUID/AuthKey is accepted over USB only, because Bluetooth cannot prove you are next to the keyboard. The Tuya app's Bluetooth "change network" is not supported for an already-bound keyboard either.

### 6.3 The pairing window

The pairing window is a 180 s temporary channel on the keyboard that allows writing Wi-Fi and the Tuya UUID/AuthKey. While it is open, Bluetooth is left to the Tuya app. Open it on the keyboard: Settings > "Pairing mode" or "Change Wi-Fi" > K6. Or click "Open on the keyboard" in the Companion's "Pairing window" card, then press K6 on the keyboard. The screen counts down as "Pairing window m:ss (USB)". K1 cancels. 10 s without a key press cancels too.

---

## 7. Updates

There are no prebuilt firmware or Companion packages for now, and no signed `.akota` update files. To update, build again yourself.

### 7.1 Firmware

1. Pull the latest code and build with the release profile as in the Build section of the [firmware README](../firmware/app/README.md).
2. Close the Companion (it holds the serial port) and flash the new `app_QIO_<version>.bin` with `tos.py flash` or tyutool, as in the Flash section.
3. After the restart the top bar shows the new version.

The "Firmware update (USB)" card on the Companion's "Advanced settings" page is for signed update files that will be published later. You cannot use it now. When an official signing key is released, updating through the Companion with an `.akota` file comes back, and this section will be updated.

After flashing, check Settings > Device info to see whether Tuya PID, UUID and AuthKey still show "SET". If they show "NOT SET", write them again as in 2.5.

### 7.2 Companion

The Companion has no automatic update. Pull the latest code and build and run it again as in the Build section of the repository README.

Keep firmware and Companion both up to date. With older firmware, some Companion settings show "Waiting for the keyboard to sync" or are unavailable.

---

## 8. Troubleshooting and FAQ

Each entry is written as: symptom, cause, what to do.

### A single click on the keyboard reacts a moment late

- **Cause**: the keyboard waits 0.25 s to tell a single click from a double click, but only for a key that **has a double click set**. By default only K6 has one (model toggle), so a K6 single click takes effect about 0.25 s after you release. K1–K4 have no double click by default and answer on release.
- **What to do**: after pressing K6, wait a moment. Menu keys and the K5 voice key are not delayed: in a menu a key works on press and release, and K5 starts recording the instant you press it.

### I press a key and nothing happens on the computer

- **Cause**: (1) "Board keys drive Kiro Crew" is "Off". (2) The Kiro Crew window is not in front, and the keyboard says "Crew window not in front". (3) The Companion is not connected to the keyboard.
- **What to do**: on the Companion's "Input" page check the master switch is "On". Open the Kiro Crew window. Check that the top right of the Companion says "Online".

### "Session list unavailable" when binding a session

- **Cause**: the Companion could not get the session list from Kiro Crew. Usually Kiro Crew is not running, has just restarted, or the Companion is not connected to it. A related message is "Crew offline". "No reply from the Companion" means the keyboard is not connected to the Companion.
- **What to do**:
  1. Make sure Kiro Crew is running under the same Windows user as the Companion.
  2. On the Companion's "Control" page check that "Live link" shows "Connected". If not, click "Refresh". If it says "Crew identity check failed", see the next entry.
  3. Check that the keyboard and Companion are connected (top right says "Online").
  4. On the keyboard open Settings > Bind session again.

### The Companion says "Crew identity check failed"

- **Cause**: before connecting to Kiro Crew, the Companion checks that the program listening on the local port (5476 by default) really is Kiro Crew running under your own user. If the check fails, it does not connect and sends no secret. Kiro Crew may not be running, another program may hold the port, or Kiro Crew and the Companion may run under different Windows users. The keyboard also shows "Crew identity check failed" once.
- **What to do**: open the Kiro Crew desktop app and wait until it has started. Run Kiro Crew and the Companion as the same Windows user (not one as administrator and the other not). Close any other program using port 5476. Then restart the Companion. It retries every few seconds by itself.

### Bluetooth will not connect, or the status bar says "BLE linked; awaiting auth"

- **Cause**: Bluetooth is encrypted with a pairing key. Without that key, or with a key that does not match (for example the keyboard has since been paired with another computer), the keyboard ignores the computer's Bluetooth connection. "Awaiting auth" means something connected to the keyboard's Bluetooth but has not been authenticated yet. It may be the Tuya app on a phone, or a computer that is not paired yet. Also, while the 180 s "Pairing mode" window is open, Bluetooth is left to the Tuya app and the Companion cannot connect. Whether the keyboard is bound to Tuya does not matter for Bluetooth.
- **What to do**:
  1. Turn on Bluetooth on the computer. In the Companion set "Transport" to "Auto" or "BLE", click "Scan" and pick the keyboard (named like `Ki-board-XXXXXX`; the last six digits are the last six digits of the keyboard's Wi-Fi MAC, shown in Settings > Device info). If you cannot find it, make sure the keyboard is not inside a "Pairing mode" window (wait 180 s, or it closes after pairing completes).
  2. Never paired, or replaced by another computer: on the keyboard open Settings > BLE pairing with a click, then click "Pair over Bluetooth" in the Companion, compare the codes and press K6 (steps in 2.3). The window lasts 120 s and closes after 3 failures, so open it again if needed.
  3. Codes differ, no dialog appears, or the keyboard says "BLE pairing failed": open the window again on the keyboard and retry. "BLE is off: cannot pair" means Bluetooth is not running. A common reason is that the Companion had chosen USB.
  4. Still no luck: on the keyboard open Settings > Device info, press K1 twice to clear the pairing, and pair again.

### The Companion cannot find the keyboard (USB)

- **Cause**: the USB cable is charge-only, the serial driver is missing, or another program (for example a flashing tool) holds the serial port.
- **What to do**: use a data cable. Look in Windows Device Manager for a new serial device. If there is none, install the CH343 serial driver. Close other programs that use the serial port, then click "Connect" in the Companion.

### I changed Wi-Fi and the keyboard will not get online

- **Cause**: the new place is a network the keyboard has not joined before (or it was pushed out of the list by newer ones), and the keyboard cannot receive a new Wi-Fi over Bluetooth.
- **What to do**: in a place you have been before it reconnects by itself. In a new place use way ② or ③ in 6.2. With a USB cable use "Change Wi-Fi": you keep the binding. Without USB, use "Unbind device" and the Tuya app, and the old binding is lost. Also check that the router is 2.4 GHz, with no client isolation and no web-page login.

### Pairing fails, or the keyboard says pairing works over USB only

- **Cause**:
  - Writing Wi-Fi and the Tuya UUID/AuthKey is accepted only over USB and only inside the 180 s window. "Settings > Pairing mode first" means the window is not open or has run out.
  - A keyboard that already holds a Tuya UUID/AuthKey rejects a new one with "Tuya UUID/AuthKey set: allow overwrite".
  - The phone cannot see the keyboard: the window has expired, or the Tuya app has no Bluetooth or nearby-devices permission.
- **What to do**: connect USB, open the pairing window again on the keyboard (K6) and work inside it. To replace a Tuya UUID/AuthKey, with the window open go to Settings > Device info and press K6 to allow one overwrite. For phone pairing, check the keyboard screen says "Pairing mode: use Tuya app" and the Wi-Fi is 2.4 GHz.

### "Warning: WiFi failed" or "Warning: WiFi disconnected"

- **Cause**: wrong password, router out of range, or the network itself is down. When the keyboard joins at start-up or rejoins after a drop, the status bar first shows "Connecting Wi-Fi...", and only after 30 s without success becomes "Warning: WiFi disconnected". The keyboard keeps retrying the whole time. It is not stuck.
- **What to do**: wait a little first. If it still fails, open Settings > Saved Wi-Fi and look for the "In use" mark. If needed, write the correct password again over USB. A network with the same name gets its password updated.

### Voice does not respond, or recognition is poor

- **Causes and fixes**:

  | Screen message | Cause | What to do |
  |---|---|---|
  | "Warning: no speech" | Pressed too briefly, or too far from the microphone | Hold K5 down, finish speaking, then release. Move closer |
  | "Tuya ASR unavailable -> PC ASR" | The keyboard is offline, so cloud recognition is not available | Check Wi-Fi, or use PC recognition |
  | "PC ASR: no API key" | Doubao or Alibaba is selected but no API key is saved | Enter the key on the "Input" page for that engine and save it |
  | "PC ASR: key refused" | The key is wrong or the service is not enabled | Create a new key and make sure the service is enabled |
  | "PC ASR: network error" | The computer cannot reach the recognition service | Check the network, or tick the local fallback |
  | "Local model missing" | Local is selected, or the online engine failed and needs the fallback, but the model is not downloaded | On the "Input" page click "Download local model" (about 245 MB) and wait for "ready" |
  | "Microphone error" | The computer's microphone is busy or off | Check the Windows microphone permission and default recording device |
  | "Warning: ASR timeout" | No result within 15 s | Try again and check the network |
  | "Voice needs Companion or pairing" | Keyboard cloud recognition cannot be used and the Companion is not connected | Connect the Companion (USB or paired Bluetooth), or write the Tuya UUID/AuthKey and bind as in 2.5 and 2.6 |
  | "Voice needs keyboard control" | The Companion is running but that switch is off | Turn "Board keys drive Kiro Crew" on, on the Companion's "Input" page |
  | "Cloud down: speech input" | Voice mode is "Chat" but the Tuya cloud is briefly down | This one press is treated as "ASR" and the text goes into the input bar. Chat returns when the cloud does |
  | "Input full: extra words dropped" | The input bar is full (about 126 Chinese or 380 English characters) | Press K6 to send, or long-press K4 to clear |
  | "Not sent: K6 to retry" or "Companion not connected; text kept" | Kiro Crew is closed or signed out, this key has no chat bound, or the keyboard is not connected to the Companion | The text is still in the bar. Fix the cause and press K6 again |
  | "Chat needs Tuya cloud" | Voice mode is "Chat" but the keyboard is offline or not bound | Get online and bind, or switch back to "ASR" |

- **Poor accuracy**: speak close to the microphone at an even pace. Add names you use often under "Hotwords" on the "Input" page (supported by Doubao and Alibaba Fun-ASR, not by Local).

### The text on the screen is too small or too big

- **What to do**: Settings > Font size, pick 12, 14 or 16 px (default 12). You can also change "Screen font size" under "Keyboard settings" on the Companion's "Device settings" page.

### The battery reading looks wrong

- **Cause**: the percentage is estimated from voltage and is re-calibrated when you plug or unplug the charger, so it can jump. When no reading is available, the top bar shows no battery icon.
- **What to do**: treat it as a guide. To update firmware, use USB power or at least 30% battery.

### The lights are off

- **Cause**: "LED power" is "Auto" and the idle time has passed, or it is set to "Off".
- **What to do**: press any key or turn the knob and the lights come back. To keep them on, set "LED power" to "On" in Settings.

### A key in a menu does nothing

- **Cause**: keys are ignored for 0.3 s after a menu appears. In a permission approval menu K6 and the knob button do nothing, only the option's own key works. An option that needs a second press only flashes the light on the first press.
- **What to do**: wait until the lights settle. In an approval, press the key of the option. When a key flashes, press the same key again within 3 s.

### The keyboard is stuck, looks wrong, or I want to start over

- **What to do**: on any screen, **hold K1 and K6 together for about 5 seconds**. While you hold, K1 and K6 are steady yellow. After the time is up, K6 flashes yellow and K1 turns red, and the screen asks "Unbind and pair again?". Release both keys, then press K6 once to confirm or K1 to cancel. No answer within 10 s cancels. This works even with a broken screen: wait until K6 flashes yellow, release both keys, and press K6 once.
- **Keys are ignored for a moment**: once both keys are down, their single, double and long presses, menu choices and dialog confirmations do nothing until both are released. You may press them in either order. A K1 pressed first can only cancel or go back, and a dialog confirms only when you release K6, so nothing fires by accident.
- **Note**: confirming clears the Tuya binding and the current Wi-Fi and restarts into the pairing window. The Tuya UUID/AuthKey and the saved Wi-Fi list are kept. You bind again in the Tuya app. On a keyboard that is not bound to Tuya yet, the combination does the same as "Pairing mode". Without a Tuya UUID/AuthKey it only shows "No Tuya UUID/AuthKey: use USB". The combination does nothing on the hardware test page.
- **To go all the way back to a new keyboard** (clearing the Tuya PID, UUID/AuthKey and all settings): use "Settings > Reset system", see section 4.

---

If your problem is not covered here, open an issue on the project page. Include the firmware version (shown in the top bar), the Companion version (shown at the bottom of its window) and a few relevant lines from the Companion "Logs" page. Do not paste Wi-Fi passwords or API keys.
