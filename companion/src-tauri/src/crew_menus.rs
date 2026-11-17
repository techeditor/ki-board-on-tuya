//! Which LCD menu the keyboard shows, and what a `menu_result` means
//! (FR-04 approvals, FR-06 model, FR-07 options / questions / follow-ups,
//! FR-10 approval mode).
//!
//! Pure state machine: no IO, no clock. The app layer feeds it Crew slot
//! snapshots, question cards, follow-up cards and board frames, writes the
//! returned frames to the keyboard, and runs the returned [`Outcome`] against
//! the Crew API. Rules are in `shared/protocol/protocol.md`, section
//! "LCD 菜单与按键灯光".
//!
//! Security: an [`Outcome`] is produced only from a `menu_result` whose id is
//! the menu currently shown, that has not expired, whose indexes are in range,
//! and (for an approval) whose request is still pending with the same
//! `request_id` / `request_mid` in the latest snapshot. Each menu answers once.

use std::collections::{HashMap, HashSet};

use serde_json::Value;

use crate::crew_api::{
    ApprovalAction, ApprovalMode, CrewApiClient, CrewModel, CrewSlot, FollowupCard, QuestionCard,
};
use crate::kirocrew_control::sanitize_device_text;
use crate::menu_wire::{
    encode_menu, menu_close_payload, menu_parked_payload, notice_payload, validate_selection, MenuIdGen, MenuItem,
    MenuKind, MenuRole, MenuSection, MenuSelect, MenuSpec, NoticeLevel,
};
use crate::lcd_text::lcd;
use crate::protocol::MenuResultPayload;

/// A request menu older than this is refused even if the keyboard answers it.
pub const REQUEST_MENU_TTL_MS: u64 = 30 * 60 * 1000;
/// Model/mode menu: keyboard timeout, plus a grace on the Companion side.
pub const MODEL_MENU_TIMEOUT_S: u32 = 30;
const GRACE_MS: u64 = 5_000;
/// Follow-up cards are not stored by Crew; drop ours after this long.
pub const FOLLOWUP_TTL_MS: u64 = 15 * 60 * 1000;
/// Context shown on the LCD: one short line. Anything longer (diffs, code,
/// multi-line questions) stays on the PC; the keyboard says "See screen".
pub const CTX_SHOW_MAX: usize = 80;
/// Label of the purple "answer by voice" item on question cards.
pub const DICTATE_LABEL: &str = "Dictate answer";

/// LCD context for `text`: `(ctx, more)`. Short single-line text is shown;
/// longer text is left to the PC screen (`more`).
pub fn screen_ctx(text: &str) -> (String, bool) {
    let t = text.trim();
    if t.is_empty() {
        return (String::new(), false);
    }
    if t.contains('\n') || t.len() > CTX_SHOW_MAX {
        return (String::new(), true);
    }
    (compact(t), false)
}

/// "Look at the diff first"-type choices: after sending the choice, the
/// Companion brings the Crew window to the front so the content can be read
/// there (the keyboard never shows it).
pub fn wants_screen(label: &str) -> bool {
    let l = label.to_lowercase();
    ["diff", "review", "show", "view", "look", "open", "inspect", "查看", "看看", "先看", "预览"]
        .iter()
        .any(|w| l.contains(w))
}

/// What the app layer must do with the Crew API after a valid answer.
#[derive(Clone, Debug, PartialEq)]
pub enum Outcome {
    Approve { slot: String, request_id: String, request_mid: String, action: ApprovalAction },
    /// An `[OPTIONS:]` choice, a follow-up prompt, a dictated answer, or the
    /// answer to a non-blocking card: sent as an ordinary user message.
    SendText { slot: String, text: String },
    AnswerAsk { slot: String, ask_id: String, answers: Vec<(String, String)> },
    /// Native card: steer into the running turn; fall back to `SendText`.
    SteerNative { slot: String, card_id: String, message: String },
    SetModel { slot: String, model: String },
    SetMode { slot: String, mode: ApprovalMode },
}

/// The result of one `menu_result`: frames to write now, and at most one
/// outcome to run. After running it, call [`MenuBoard::finish`].
#[derive(Clone, Debug, Default, PartialEq)]
pub struct ResultReply {
    pub frames: Vec<Value>,
    pub outcome: Option<(String, Outcome)>,
    /// The chosen option asks to look at something (a diff): bring the Crew
    /// window to the front.
    pub front_crew: bool,
}

#[derive(Clone, Debug, PartialEq)]
enum Source {
    Approval { slot: String, request_id: String, request_mid: String },
    Options { slot: String, ts: String, options: Vec<String> },
    Followup { slot: String, ts: String, prompts: Vec<String> },
    Question { slot: String, card: QuestionCard, index: usize, answers: Vec<(String, String)> },
    Model { slot: String, models: Vec<String> },
}

impl Source {
    fn key(&self) -> String {
        match self {
            Source::Approval { slot, request_id, request_mid } => format!("ap:{slot}:{request_id}:{request_mid}"),
            Source::Options { slot, ts, .. } => format!("op:{slot}:{ts}"),
            Source::Followup { slot, ts, .. } => format!("fu:{slot}:{ts}"),
            Source::Question { slot, card, .. } => format!("q:{slot}:{}{}", card.ask_id, card.card_id),
            Source::Model { slot, .. } => format!("model:{slot}"),
        }
    }
    fn priority(&self) -> u8 {
        match self {
            Source::Approval { .. } => 0,
            Source::Question { .. } => 1,
            Source::Options { .. } | Source::Followup { .. } => 2,
            Source::Model { .. } => 3,
        }
    }
    fn slot(&self) -> &str {
        match self {
            Source::Approval { slot, .. }
            | Source::Options { slot, .. }
            | Source::Followup { slot, .. }
            | Source::Question { slot, .. }
            | Source::Model { slot, .. } => slot,
        }
    }
    fn user_opened(&self) -> bool {
        matches!(self, Source::Model { .. })
    }
}

#[derive(Clone, Debug)]
struct Active {
    id: String,
    spec: MenuSpec,
    source: Source,
    expires_ms: u64,
    voice_index: Option<usize>,
}

/// The menu coordinator. One per Companion process.
#[derive(Debug)]
pub struct MenuBoard {
    ids: MenuIdGen,
    /// In-scope slots in order: K1..K3 bound chats, then the current chat.
    scope: Vec<(String, char)>,
    slots: HashMap<String, CrewSlot>,
    questions: HashMap<String, Vec<QuestionCard>>,
    followups: HashMap<String, (FollowupCard, u64)>,
    first_seen: HashMap<String, u64>,
    /// ESC'd requests: kept, not re-shown until K4 recalls them or they change.
    dismissed: HashSet<String>,
    /// Answered requests: not re-shown while Crew still reports them pending.
    handled: HashSet<String>,
    active: Option<Active>,
    /// ESC'd count last sent to the keyboard (`menu_parked`).
    parked_sent: Option<usize>,
    /// Dictated answers need keyboard-side (cloud) ASR.
    voice_ok: bool,
    /// Per chat: the model last picked by hand (target of the Auto toggle).
    /// Seeded from the Companion config file; the app layer persists changes.
    last_models: HashMap<String, String>,
}

fn letter_prefix(letter: char) -> String {
    if letter == '*' {
        String::new()
    } else {
        format!("{letter} ")
    }
}

fn compact(text: &str) -> String {
    text.split_whitespace().collect::<Vec<_>>().join(" ")
}


impl MenuBoard {
    pub fn new(seed: u64) -> Self {
        MenuBoard {
            ids: MenuIdGen::new(seed),
            scope: Vec::new(),
            slots: HashMap::new(),
            questions: HashMap::new(),
            followups: HashMap::new(),
            first_seen: HashMap::new(),
            dismissed: HashSet::new(),
            handled: HashSet::new(),
            active: None,
            // A freshly booted keyboard has nothing parked; reset_display()
            // forces a resend after a (re)connect.
            parked_sent: Some(0),
            voice_ok: false,
            last_models: HashMap::new(),
        }
    }

    /// Set the in-scope chats: `bound` = slot keys of K1..K3 (empty = unbound),
    /// `current` = the chat selected in the Companion.
    pub fn set_scope(&mut self, bound: &[String], current: Option<&str>) {
        let mut scope = Vec::new();
        for (i, key) in bound.iter().take(3).enumerate() {
            let key = key.trim();
            if !key.is_empty() && !scope.iter().any(|(k, _)| k == key) {
                scope.push((key.to_string(), (b'A' + i as u8) as char));
            }
        }
        if let Some(cur) = current.map(str::trim).filter(|c| !c.is_empty()) {
            if !scope.iter().any(|(k, _)| k == cur) {
                scope.push((cur.to_string(), '*'));
            }
        }
        self.scope = scope;
    }

    pub fn set_voice_ok(&mut self, ok: bool) {
        self.voice_ok = ok;
    }

    fn in_scope(&self, slot: &str) -> bool {
        self.scope.iter().any(|(k, _)| k == slot)
    }

    fn letter(&self, slot: &str) -> char {
        self.scope.iter().find(|(k, _)| k == slot).map(|(_, l)| *l).unwrap_or('*')
    }

    /// Feed a full `slots` snapshot. Returns the in-scope slots whose question
    /// cards must be (re)fetched (`needs_input` just turned on).
    pub fn update_slots(&mut self, slots: &[CrewSlot]) -> Vec<String> {
        let mut fetch = Vec::new();
        let mut next = HashMap::new();
        for slot in slots {
            let was = self.slots.get(&slot.key).map(|s| s.needs_input).unwrap_or(false);
            if slot.needs_input && !was && self.in_scope(&slot.key) {
                fetch.push(slot.key.clone());
            }
            if !slot.needs_input {
                self.questions.remove(&slot.key);
            }
            next.insert(slot.key.clone(), slot.clone());
        }
        self.questions.retain(|k, _| next.contains_key(k));
        self.followups.retain(|k, _| next.contains_key(k));
        self.slots = next;
        fetch
    }

    /// Scope slots with `needs_input` set (for a refetch after a scope change).
    pub fn slots_needing_questions(&self) -> Vec<String> {
        self.scope
            .iter()
            .filter(|(k, _)| self.slots.get(k).is_some_and(|s| s.needs_input))
            .map(|(k, _)| k.clone())
            .collect()
    }

    pub fn set_questions(&mut self, slot: &str, cards: Vec<QuestionCard>) {
        let cards: Vec<QuestionCard> = cards.into_iter().filter(|c| c.slot.is_empty() || c.slot == slot).collect();
        if cards.is_empty() {
            self.questions.remove(slot);
        } else {
            self.questions.insert(slot.to_string(), cards);
        }
    }

    pub fn add_followup(&mut self, card: FollowupCard, now: u64) {
        if card.items.iter().any(|i| !i.prompt.trim().is_empty()) {
            self.followups.insert(card.slot.clone(), (card, now));
        }
    }

    /// K4 in standby (`menu_recall`): show the ESC'd requests again.
    pub fn recall(&mut self) {
        self.dismissed.clear();
    }

    /// The keyboard (re)connected: forget what it was showing.
    pub fn reset_display(&mut self) {
        self.active = None;
        self.parked_sent = None;
    }

    /// Current menu's id and frames (UI mirror).
    pub fn active_frames(&self) -> Option<(String, Vec<Value>)> {
        let a = self.active.as_ref()?;
        Some((a.id.clone(), encode_menu(&a.spec).unwrap_or_default()))
    }

    fn question_answerable(&self, card: &QuestionCard, index: usize) -> bool {
        card.questions.get(index).is_some_and(|q| !q.options.is_empty() || self.voice_ok)
    }

    /// Pending requests in scope, sorted by priority then first sight.
    fn candidates(&mut self, now: u64) -> Vec<Source> {
        let mut out = Vec::new();
        for (key, _) in &self.scope {
            let Some(slot) = self.slots.get(key) else { continue };
            if let Some(info) = slot.pending_approval_info.as_ref() {
                // Only native requests can be decided through the mid-bound
                // `/approve` form the Companion uses.
                if slot.pending_approval
                    && info.origin == "native"
                    && !info.request_id.is_empty()
                    && !info.request_mid.is_empty()
                {
                    out.push(Source::Approval {
                        slot: key.clone(),
                        request_id: info.request_id.clone(),
                        request_mid: info.request_mid.clone(),
                    });
                }
            }
            for card in self.questions.get(key).into_iter().flatten() {
                if self.question_answerable(card, 0) {
                    out.push(Source::Question { slot: key.clone(), card: card.clone(), index: 0, answers: Vec::new() });
                }
            }
            if slot.has_options && !slot.options.is_empty() {
                out.push(Source::Options {
                    slot: key.clone(),
                    ts: slot.options_ts.clone(),
                    options: slot.options.clone(),
                });
            }
            if let Some((card, at)) = self.followups.get(key) {
                if now.saturating_sub(*at) < FOLLOWUP_TTL_MS {
                    out.push(Source::Followup {
                        slot: key.clone(),
                        ts: card.ts.clone(),
                        prompts: card.items.iter().map(|i| i.prompt.clone()).collect(),
                    });
                }
            }
        }
        for c in &out {
            self.first_seen.entry(c.key()).or_insert(now);
        }
        let live: HashSet<String> = out.iter().map(Source::key).collect();
        self.first_seen.retain(|k, _| live.contains(k));
        self.dismissed.retain(|k| live.contains(k));
        self.handled.retain(|k| live.contains(k));
        let seen = &self.first_seen;
        out.sort_by_key(|c| (c.priority(), seen.get(&c.key()).copied().unwrap_or(now)));
        out
    }

    fn showable<'a>(&self, cands: &'a [Source]) -> Vec<&'a Source> {
        cands
            .iter()
            .filter(|c| !self.dismissed.contains(&c.key()) && !self.handled.contains(&c.key()))
            .collect()
    }

    fn queue_label(&self, key: &str, showable: &[&Source]) -> String {
        if showable.len() < 2 {
            return String::new();
        }
        match showable.iter().position(|c| c.key() == key) {
            Some(i) => format!("{}/{}", i + 1, showable.len()),
            None => String::new(),
        }
    }

    /// Re-evaluate what the keyboard should show. Returns frames to write.
    pub fn reconcile(&mut self, now: u64) -> Vec<Value> {
        let mut frames = Vec::new();
        let cands = self.candidates(now);
        let showable = self.showable(&cands);

        if let Some(a) = &self.active {
            let gone = if a.source.user_opened() {
                !self.slots.contains_key(a.source.slot())
            } else {
                !cands.iter().any(|c| c.key() == a.source.key())
            };
            if gone || now >= a.expires_ms {
                frames.push(menu_close_payload(&a.id, lcd(if gone { "Done" } else { "Expired" })));
                self.active = None;
            }
        }
        if let (Some(a), Some(best)) = (&self.active, showable.first()) {
            if !a.source.user_opened() && best.priority() < a.source.priority() {
                frames.push(menu_close_payload(&a.id, ""));
                self.active = None;
            }
        }
        if self.active.is_none() {
            if let Some(best) = showable.first() {
                let queue = self.queue_label(&best.key(), &showable);
                let source = (*best).clone();
                frames.extend(self.show(source, queue, now));
            }
        } else if let Some(key) = self.active.as_ref().filter(|a| !a.source.user_opened()).map(|a| a.source.key()) {
            let queue = self.queue_label(&key, &showable);
            if let Some(a) = self.active.as_mut() {
                if queue != a.spec.queue {
                    a.spec.queue = queue;
                    frames.extend(encode_menu(&a.spec).unwrap_or_default());
                }
            }
        }
        // K4 breathes on the keyboard while ESC'd requests wait.
        let parked = self.dismissed.iter().filter(|k| !self.handled.contains(*k)).count();
        if self.parked_sent != Some(parked) {
            self.parked_sent = Some(parked);
            frames.push(menu_parked_payload(parked));
        }
        frames
    }

    /// ESC'd requests waiting for K4.
    pub fn parked(&self) -> usize {
        self.parked_sent.unwrap_or(0)
    }

    fn build_spec(&self, id: &str, source: &Source) -> Option<(MenuSpec, Option<usize>, u32)> {
        let letter = self.letter(source.slot());
        let prefix = letter_prefix(letter);
        match source {
            Source::Approval { slot, .. } => {
                let info = self.slots.get(slot)?.pending_approval_info.clone()?;
                let tool = if info.tool.is_empty() { "tool".to_string() } else { compact(&info.tool) };
                // No default answer: the keyboard ignores K6 on approvals, so
                // only the key of "Approve once" approves.
                let mut spec = MenuSpec::single(
                    id,
                    MenuKind::Approval,
                    format!("{prefix}{tool}"),
                    vec![
                        MenuItem::new(lcd("Approve once"), MenuRole::Confirm),
                        MenuItem::new(lcd("Reject"), MenuRole::Cancel),
                        MenuItem::new(lcd("Reject once"), MenuRole::Danger),
                    ],
                );
                (spec.ctx, spec.more) =
                    screen_ctx(if info.tool_input.is_empty() { &info.tool_purpose } else { &info.tool_input });
                Some((spec, None, 0))
            }
            Source::Options { slot, options, .. } => {
                let items = options.iter().map(|o| MenuItem::new(compact(o), MenuRole::Normal)).collect();
                let mut spec = MenuSpec::single(id, MenuKind::Options, format!("{prefix}{}", lcd("Reply")), items);
                (spec.ctx, spec.more) =
                    screen_ctx(&self.slots.get(slot).map(|s| s.prompt_preview.clone()).unwrap_or_default());
                Some((spec, None, 0))
            }
            Source::Followup { slot, .. } => {
                let (card, _) = self.followups.get(slot)?;
                let items = card.items.iter().map(|i| MenuItem::new(compact(&i.title), MenuRole::Normal)).collect();
                Some((MenuSpec::single(id, MenuKind::Options, format!("{prefix}{}", lcd("Next step")), items), None, 0))
            }
            Source::Question { card, index, .. } => {
                let q = card.questions.get(*index)?;
                let title = if q.header.is_empty() { lcd("Question").to_string() } else { compact(&q.header) };
                let items = q.options.iter().map(|o| MenuItem::new(compact(&o.label), MenuRole::Normal)).collect();
                let mut spec = MenuSpec::single(id, MenuKind::Question, format!("{prefix}{title}"), items);
                (spec.ctx, spec.more) = screen_ctx(&q.question);
                if q.multi_select {
                    spec.select = MenuSelect::Multi;
                }
                if card.questions.len() > 1 {
                    spec.sections[0].title = format!("{prefix}{title} {}/{}", index + 1, card.questions.len());
                }
                // Purple "Dictate answer": the keyboard records, confirms and
                // returns the text in menu_result (cloud ASR only).
                let voice = if self.voice_ok && spec.select == MenuSelect::Single {
                    spec.sections[0].items.push(MenuItem::new(lcd(DICTATE_LABEL), MenuRole::Voice));
                    Some(q.options.len())
                } else {
                    None
                };
                if spec.item_count() == 0 {
                    return None;
                }
                Some((spec, voice, 0))
            }
            Source::Model { .. } => None, // built by open_model_menu
        }
    }

    fn show(&mut self, source: Source, queue: String, now: u64) -> Vec<Value> {
        let id = self.ids.next_id();
        let Some((mut spec, voice_index, timeout_s)) = self.build_spec(&id, &source) else {
            return Vec::new();
        };
        spec.queue = queue;
        spec.timeout_s = timeout_s;
        self.install(id, spec, source, voice_index, now)
    }

    fn install(&mut self, id: String, spec: MenuSpec, source: Source, voice_index: Option<usize>, now: u64) -> Vec<Value> {
        match encode_menu(&spec) {
            Ok(frames) => {
                let ttl = if spec.timeout_s > 0 {
                    spec.timeout_s as u64 * 1000 + GRACE_MS
                } else {
                    REQUEST_MENU_TTL_MS
                };
                self.active = Some(Active { id, spec, source, expires_ms: now + ttl, voice_index });
                frames
            }
            Err(_) => {
                // Cannot be drawn within the frame limits: leave it to the
                // dashboard and keep it from blocking the queue.
                self.handled.insert(source.key());
                Vec::new()
            }
        }
    }

    /// K6 long press: open the model menu (page 1+ models, last page modes).
    pub fn open_model_menu(&mut self, slot: &str, models: &[CrewModel], now: u64) -> Vec<Value> {
        let Some(snapshot) = self.slots.get(slot).cloned() else {
            return vec![notice_payload(lcd("No chat selected"), NoticeLevel::Warn)];
        };
        let mut names: Vec<(String, String)> = Vec::new();
        names.push(("auto".into(), lcd("Auto").into()));
        for m in models {
            let name = m.model_name.trim();
            // "auto:jev" arms owner-only routing; not offered on the keyboard.
            if name.is_empty() || name == "auto" || name.starts_with("auto:") {
                continue;
            }
            let label = if m.display_name.trim().is_empty() { name } else { m.display_name.trim() };
            names.push((name.to_string(), label.to_string()));
        }
        let current = if snapshot.model.trim().is_empty() { "auto" } else { snapshot.model.trim() };
        let current_idx = names.iter().position(|(n, _)| n == current).unwrap_or(0);
        let mode = ApprovalMode::of_slot(&snapshot);
        let model_items: Vec<MenuItem> = names
            .iter()
            .enumerate()
            .map(|(i, (_, label))| MenuItem::new(label.clone(), MenuRole::Normal).checked(i == current_idx))
            .collect();
        let mode_items = vec![
            MenuItem::new(lcd("Normal"), MenuRole::Normal).checked(mode == ApprovalMode::Normal),
            MenuItem::new(lcd("Reads"), MenuRole::Normal).checked(mode == ApprovalMode::Reads),
            MenuItem::new(lcd("Trust"), MenuRole::Attention).double_confirm().checked(mode == ApprovalMode::Trust),
        ];
        let prefix = letter_prefix(self.letter(slot));
        let mut frames = Vec::new();
        if let Some(a) = self.active.take() {
            frames.push(menu_close_payload(&a.id, ""));
        }
        let id = self.ids.next_id();
        let spec = MenuSpec {
            id: id.clone(),
            ctx: String::new(),
            more: false,
            queue: String::new(),
            select: MenuSelect::Single,
            default: current_idx,
            timeout_s: MODEL_MENU_TIMEOUT_S,
            sections: vec![
                MenuSection { kind: MenuKind::Model, title: format!("{prefix}{}", lcd("Model")), items: model_items },
                MenuSection { kind: MenuKind::Mode, title: format!("{prefix}{}", lcd("Mode")), items: mode_items },
            ],
        };
        let source = Source::Model { slot: slot.to_string(), models: names.into_iter().map(|(n, _)| n).collect() };
        let shown = self.install(id, spec, source, None, now);
        if shown.is_empty() {
            frames.push(notice_payload(lcd("Model list too long"), NoticeLevel::Warn));
        }
        frames.extend(shown);
        frames
    }

    /// K6 double: the model to switch `slot` to (Auto <-> last hand-picked).
    pub fn toggle_target(&self, slot: &str) -> Result<String, &'static str> {
        let model = self.slots.get(slot).ok_or(lcd("No chat selected"))?.model.trim().to_string();
        if model.is_empty() || model == "auto" || model.starts_with("auto:") {
            self.last_models.get(slot).cloned().ok_or(lcd("No model picked yet"))
        } else {
            Ok("auto".into())
        }
    }

    /// Load the remembered per-chat models (from the Companion config file).
    pub fn set_last_models(&mut self, models: impl IntoIterator<Item = (String, String)>) {
        self.last_models = models.into_iter().collect();
    }

    /// Record a model change on `slot` (keyboard menu or toggle) so the toggle
    /// can return to the hand-picked one. `previous` is the slot's model before
    /// the change. Returns the model to persist for `slot`, if it changed.
    pub fn note_model_change(&mut self, slot: &str, previous: &str, chosen: &str) -> Option<String> {
        let is_manual = |m: &str| !m.is_empty() && m != "auto" && !m.starts_with("auto:");
        let remember = if is_manual(chosen) {
            chosen
        } else if is_manual(previous) {
            previous
        } else {
            return None;
        };
        if self.last_models.get(slot).map(String::as_str) == Some(remember) {
            return None;
        }
        self.last_models.insert(slot.to_string(), remember.to_string());
        Some(remember.to_string())
    }

    pub fn slot_model(&self, slot: &str) -> String {
        self.slots.get(slot).map(|s| s.model.clone()).unwrap_or_default()
    }

    /// Handle a `menu_result` from the keyboard.
    pub fn on_result(&mut self, r: &MenuResultPayload, now: u64) -> ResultReply {
        let mut reply = ResultReply::default();
        let Some(active) = self.active.clone() else {
            reply.frames.push(menu_close_payload(&r.id, lcd("Expired")));
            return reply;
        };
        if active.id != r.id {
            reply.frames.push(menu_close_payload(&r.id, lcd("Expired")));
            return reply;
        }
        self.active = None;
        if now >= active.expires_ms {
            reply.frames.push(menu_close_payload(&r.id, lcd("Expired")));
            reply.frames.extend(self.reconcile(now));
            return reply;
        }
        let multi_empty = active.spec.select == MenuSelect::Multi && r.selected.is_empty() && !r.dismissed;
        if r.dismissed || multi_empty {
            // ESC keeps the request (K4 brings it back); a user-opened model
            // menu is simply closed.
            if !active.source.user_opened() {
                self.dismissed.insert(active.source.key());
            }
            reply.frames.extend(self.reconcile(now));
            return reply;
        }
        let Some(sel) = validate_selection(&active.spec, &r.selected) else {
            reply.frames.push(menu_close_payload(&r.id, lcd("Invalid")));
            reply.frames.extend(self.reconcile(now));
            return reply;
        };
        // The voice item answers only with the text dictated and confirmed on
        // the keyboard; text on any other item is ignored.
        let dictated = match active.voice_index {
            Some(v) if sel.contains(&v) => match sanitize_device_text(&r.text) {
                Some(text) => Some(text),
                None => {
                    reply.frames.push(menu_close_payload(&r.id, lcd("Invalid")));
                    reply.frames.extend(self.reconcile(now));
                    return reply;
                }
            },
            _ => None,
        };
        let mut chosen = String::new();
        let outcome = match active.source.clone() {
            Source::Approval { slot, request_id, request_mid } => {
                let key = active.source.key();
                let still = self.candidates(now).iter().any(|c| c.key() == key);
                if !still {
                    reply.frames.push(menu_close_payload(&r.id, lcd("Expired")));
                    reply.frames.extend(self.reconcile(now));
                    return reply;
                }
                let action = match sel[0] {
                    0 => ApprovalAction::Approved,
                    1 => ApprovalAction::Rejected,
                    _ => ApprovalAction::RejectedOnce,
                };
                Outcome::Approve { slot, request_id, request_mid, action }
            }
            Source::Options { slot, options, .. } => {
                chosen = options[sel[0]].clone();
                Outcome::SendText { slot, text: chosen.clone() }
            }
            Source::Followup { slot, prompts, .. } => {
                chosen = active.spec.item(sel[0]).map(|i| i.label.clone()).unwrap_or_default();
                Outcome::SendText { slot, text: prompts[sel[0]].clone() }
            }
            Source::Question { slot, card, index, mut answers } => {
                let q = &card.questions[index];
                let text = match dictated {
                    Some(text) => text,
                    None => sel.iter().map(|&i| q.options[i].label.clone()).collect::<Vec<_>>().join(", "),
                };
                chosen = text.clone();
                answers.push((q.question.clone(), text));
                reply.front_crew = wants_screen(&chosen);
                match self.advance_question(slot, card, index, answers, now) {
                    Ok(frames) => {
                        reply.frames.extend(frames);
                        return reply;
                    }
                    Err(o) => o,
                }
            }
            Source::Model { slot, models } => {
                let i = sel[0];
                if i < models.len() {
                    Outcome::SetModel { slot, model: models[i].clone() }
                } else {
                    let mode = match i - models.len() {
                        0 => ApprovalMode::Normal,
                        1 => ApprovalMode::Reads,
                        _ => ApprovalMode::Trust,
                    };
                    Outcome::SetMode { slot, mode }
                }
            }
        };
        reply.front_crew = wants_screen(&chosen);
        if !active.source.user_opened() {
            self.handled.insert(active.source.key());
        }
        reply.outcome = Some((active.id, outcome));
        reply
    }

    /// Next question of a multi-question card, or the final outcome (`Err`).
    fn advance_question(
        &mut self,
        slot: String,
        card: QuestionCard,
        index: usize,
        answers: Vec<(String, String)>,
        now: u64,
    ) -> Result<Vec<Value>, Outcome> {
        if index + 1 < card.questions.len() && self.question_answerable(&card, index + 1) {
            let source = Source::Question { slot, card, index: index + 1, answers };
            return Ok(self.show(source, String::new(), now));
        }
        let key = format!("q:{slot}:{}{}", card.ask_id, card.card_id);
        self.handled.insert(key);
        if !card.ask_id.is_empty() {
            return Err(Outcome::AnswerAsk { slot, ask_id: card.ask_id, answers });
        }
        let message = if answers.len() == 1 {
            answers[0].1.clone()
        } else {
            answers.iter().map(|(q, a)| format!("{q}: {a}")).collect::<Vec<_>>().join("\n")
        };
        if card.native {
            Err(Outcome::SteerNative { slot, card_id: card.card_id, message })
        } else {
            Err(Outcome::SendText { slot, text: message })
        }
    }

    /// After running an outcome: close the menu with a short status, and show
    /// the next one. On failure the request may be shown again.
    pub fn finish(&mut self, id: &str, outcome: &Outcome, result: Result<&str, &str>, now: u64) -> Vec<Value> {
        let mut frames = Vec::new();
        match result {
            Ok(msg) => frames.push(menu_close_payload(id, msg)),
            Err(err) => {
                frames.push(menu_close_payload(id, lcd("Failed")));
                if let Some(key) = outcome_key(outcome) {
                    self.handled.remove(&key);
                }
                frames.extend(self.reconcile(now));
                frames.push(notice_payload(&format!("{}{err}", lcd("Failed: ")), NoticeLevel::Error));
                return frames;
            }
        }
        frames.extend(self.reconcile(now));
        frames
    }
}

fn outcome_key(o: &Outcome) -> Option<String> {
    match o {
        Outcome::Approve { slot, request_id, request_mid, .. } => Some(format!("ap:{slot}:{request_id}:{request_mid}")),
        Outcome::AnswerAsk { slot, ask_id, .. } => Some(format!("q:{slot}:{ask_id}")),
        Outcome::SteerNative { slot, card_id, .. } => Some(format!("q:{slot}:{card_id}")),
        // Options/follow-ups/models: a failed send leaves nothing to retry by key.
        _ => None,
    }
}

/// Run an outcome against the Crew API. `Ok(true)` = a model change that
/// applies after the running turn.
pub fn run_outcome(client: &CrewApiClient, o: &Outcome) -> Result<bool, String> {
    match o {
        Outcome::Approve { slot, request_id, request_mid, action } => {
            client.approve(slot, request_id, request_mid, *action).map(|_| false)
        }
        Outcome::SendText { slot, text } => client.send_message(slot, text).map(|_| false),
        Outcome::AnswerAsk { ask_id, answers, .. } => client.answer_ask(ask_id, answers).map(|_| false),
        Outcome::SteerNative { slot, card_id, message } => {
            if !client.steer_question(slot, card_id, message)? {
                // No turn running: the card's answer goes as a normal message.
                client.send_message(slot, message)?;
            }
            Ok(false)
        }
        Outcome::SetModel { slot, model } => client.set_model(slot, model).map(|r| r.deferred),
        Outcome::SetMode { slot, mode } => client.set_mode(slot, *mode).map(|_| false),
    }
}

/// Log line for an outcome: kind and chat, never answer text.
pub fn describe_outcome(o: &Outcome) -> String {
    match o {
        Outcome::Approve { slot, action, .. } => format!("approve {} {slot}", action.wire()),
        Outcome::SendText { slot, .. } => format!("send option {slot}"),
        Outcome::AnswerAsk { slot, .. } => format!("answer question {slot}"),
        Outcome::SteerNative { slot, .. } => format!("answer native question {slot}"),
        Outcome::SetModel { slot, model } => format!("set model {model} {slot}"),
        Outcome::SetMode { slot, mode } => format!("set mode {} {slot}", mode.wire()),
    }
}

/// Short `menu_close` text for a successful outcome.
pub fn success_message(o: &Outcome, deferred: bool) -> &'static str {
    lcd(match o {
        Outcome::Approve { action: ApprovalAction::Approved, .. } => "Approved",
        Outcome::Approve { .. } => "Rejected",
        Outcome::SendText { .. } | Outcome::AnswerAsk { .. } | Outcome::SteerNative { .. } => "Sent",
        Outcome::SetModel { .. } if deferred => "Model: next turn",
        Outcome::SetModel { .. } => "Model set",
        Outcome::SetMode { mode: ApprovalMode::Normal, .. } => "Mode: Normal",
        Outcome::SetMode { mode: ApprovalMode::Reads, .. } => "Mode: Reads",
        Outcome::SetMode { mode: ApprovalMode::Trust, .. } => "Mode: Trust",
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::crew_api::{CardOption, CardQuestion, FollowupItem, PendingApprovalInfo};

    fn slot(key: &str) -> CrewSlot {
        CrewSlot { key: key.into(), ..Default::default() }
    }

    fn approval_slot(key: &str, rid: &str, mid: &str) -> CrewSlot {
        CrewSlot {
            pending_approval: true,
            pending_approval_info: Some(PendingApprovalInfo {
                origin: "native".into(),
                tool: "shell".into(),
                tool_input: "git  push\norigin".into(),
                request_id: rid.into(),
                request_mid: mid.into(),
                ..Default::default()
            }),
            ..slot(key)
        }
    }

    fn board(slots: &[CrewSlot]) -> MenuBoard {
        let mut b = MenuBoard::new(7);
        b.set_scope(&["a".into(), "b".into(), "".into()], Some("cur"));
        b.update_slots(slots);
        b
    }

    fn menu_id(frames: &[Value]) -> String {
        frames.iter().find(|f| f["type"] == "menu").expect("menu frame")["payload"]["id"]
            .as_str()
            .unwrap()
            .to_string()
    }

    fn result(id: &str, selected: &[u32]) -> MenuResultPayload {
        MenuResultPayload { id: id.into(), selected: selected.to_vec(), dismissed: false, reason: String::new(), text: String::new() }
    }

    fn esc(id: &str) -> MenuResultPayload {
        MenuResultPayload { id: id.into(), selected: vec![], dismissed: true, reason: "esc".into(), text: String::new() }
    }

    #[test]
    fn approval_menu_and_decision() {
        let mut b = board(&[approval_slot("a", "r1", "m1")]);
        let frames = b.reconcile(0);
        let p = &frames[0]["payload"];
        assert_eq!(p["kind"], "approval");
        assert_eq!(p["title"], "A shell");
        // Multi-line tool input stays on the PC: "See screen" on the LCD.
        assert!(p.get("ctx").is_none());
        assert_eq!(p["more"], true);
        assert_eq!(p["items"].as_array().unwrap().len(), 3);
        let id = menu_id(&frames);
        let reply = b.on_result(&result(&id, &[2]), 10);
        let (rid, outcome) = reply.outcome.unwrap();
        assert_eq!(rid, id);
        assert_eq!(
            outcome,
            Outcome::Approve { slot: "a".into(), request_id: "r1".into(), request_mid: "m1".into(), action: ApprovalAction::RejectedOnce }
        );
        // Answered: not shown again while Crew still reports it pending.
        assert!(b.reconcile(20).is_empty());
        // A second result for the same menu is refused.
        let again = b.on_result(&result(&id, &[0]), 30);
        assert!(again.outcome.is_none());
        assert_eq!(again.frames[0]["payload"]["msg"], "Expired");
    }

    #[test]
    fn stale_or_foreign_results_are_refused() {
        let mut b = board(&[approval_slot("a", "r1", "m1")]);
        let id = menu_id(&b.reconcile(0));
        assert!(b.on_result(&result("other", &[0]), 1).outcome.is_none());
        // The request changed (new mid) before the key press: refused.
        b.update_slots(&[approval_slot("a", "r1", "m2")]);
        let r = b.on_result(&result(&id, &[0]), 2);
        assert!(r.outcome.is_none());
        // Out-of-range index is refused.
        let mut b = board(&[approval_slot("a", "r1", "m1")]);
        let id = menu_id(&b.reconcile(0));
        assert!(b.on_result(&result(&id, &[3]), 1).outcome.is_none());
        // Expired menu is refused.
        let mut b = board(&[approval_slot("a", "r1", "m1")]);
        let id = menu_id(&b.reconcile(0));
        assert!(b.on_result(&result(&id, &[0]), REQUEST_MENU_TTL_MS + 1).outcome.is_none());
    }

    #[test]
    fn coordinator_and_out_of_scope_approvals_are_ignored() {
        let mut coord = approval_slot("a", "r1", "m1");
        coord.pending_approval_info.as_mut().unwrap().origin = "coordinator".into();
        let mut b = board(&[coord, approval_slot("zz", "r2", "m2")]);
        assert!(b.reconcile(0).is_empty());
    }

    #[test]
    fn approval_preempts_options_and_queue_is_shown() {
        let mut opts = slot("b");
        opts.has_options = true;
        opts.options = vec!["Yes".into(), "No".into()];
        opts.options_ts = "5.0".into();
        let mut b = board(&[opts.clone()]);
        let frames = b.reconcile(0);
        let opt_id = menu_id(&frames);
        assert_eq!(frames[0]["payload"]["kind"], "options");
        b.update_slots(&[opts, approval_slot("a", "r1", "m1")]);
        let frames = b.reconcile(5);
        assert_eq!(frames[0]["type"], "menu_close");
        assert_eq!(frames[0]["payload"]["id"], opt_id.as_str());
        assert_eq!(frames[1]["payload"]["kind"], "approval");
        assert_eq!(frames[1]["payload"]["queue"], "1/2");
    }

    #[test]
    fn esc_parks_until_k4_recall_and_resolution_closes() {
        let mut b = board(&[approval_slot("a", "r1", "m1")]);
        let id = menu_id(&b.reconcile(0));
        let r = b.on_result(&esc(&id), 1);
        assert!(r.outcome.is_none());
        assert_eq!(r.frames.len(), 1);
        assert_eq!(r.frames[0]["type"], "menu_parked");
        assert_eq!(r.frames[0]["payload"]["count"], 1);
        assert_eq!(b.parked(), 1);
        assert!(b.reconcile(2).is_empty(), "an ESC'd request is not shown again by itself");
        b.recall();
        let frames = b.reconcile(3);
        let id2 = menu_id(&frames);
        assert_ne!(id, id2);
        assert!(frames.iter().any(|f| f["type"] == "menu_parked" && f["payload"]["count"] == 0));
        // Approved on the dashboard: the menu is revoked.
        b.update_slots(&[slot("a")]);
        let frames = b.reconcile(4);
        assert_eq!(frames[0]["type"], "menu_close");
        assert_eq!(frames[0]["payload"]["msg"], "Done");
        // A parked request resolved elsewhere drops the K4 count.
        b.update_slots(&[approval_slot("a", "r2", "m2")]);
        let id3 = menu_id(&b.reconcile(5));
        b.on_result(&esc(&id3), 6);
        b.update_slots(&[slot("a")]);
        let frames = b.reconcile(7);
        assert_eq!(frames.last().unwrap()["type"], "menu_parked");
        assert_eq!(frames.last().unwrap()["payload"]["count"], 0);
        // A reconnect resends the count.
        b.reset_display();
        assert_eq!(b.reconcile(8).last().unwrap()["type"], "menu_parked");
    }

    #[test]
    fn long_context_stays_on_screen_and_view_choices_front_crew() {
        assert_eq!(screen_ctx("git push origin feat/x"), ("git push origin feat/x".to_string(), false));
        assert_eq!(screen_ctx("a\nb"), (String::new(), true));
        assert_eq!(screen_ctx(&"x".repeat(CTX_SHOW_MAX + 1)), (String::new(), true));
        assert_eq!(screen_ctx("  "), (String::new(), false));
        // The approval fixture's input has a newline: the LCD says "See screen".
        let mut b = board(&[approval_slot("a", "r1", "m1")]);
        let f = b.reconcile(0);
        assert_eq!(f[0]["payload"]["more"], true);
        assert!(f[0]["payload"].get("ctx").is_none());
        assert!(f[0]["payload"].get("default").is_none(), "no default answer on approvals");

        assert!(wants_screen("Show me the diff") && wants_screen("先看 diff") && wants_screen("Review first"));
        assert!(!wants_screen("Merge it now"));
        let mut s = slot("cur");
        s.has_options = true;
        s.options = vec!["Show me the diff".into(), "Merge it now".into()];
        s.options_ts = "1.5".into();
        let mut b = board(&[s]);
        let id = menu_id(&b.reconcile(0));
        let r = b.on_result(&result(&id, &[0]), 1);
        assert!(r.front_crew && r.outcome.is_some());
    }

    #[test]
    fn options_and_followup_send_full_text() {
        let long = "Merge the change and also update the release notes".to_string();
        let mut s = slot("cur");
        s.has_options = true;
        s.options = vec![long.clone(), "Skip".into()];
        s.options_ts = "1.5".into();
        let mut b = board(&[s]);
        let id = menu_id(&b.reconcile(0));
        let (_, o) = b.on_result(&result(&id, &[0]), 1).outcome.unwrap();
        assert_eq!(o, Outcome::SendText { slot: "cur".into(), text: long });

        let mut b = board(&[slot("a")]);
        b.add_followup(
            FollowupCard { slot: "a".into(), items: vec![FollowupItem { title: "Ship".into(), prompt: "Ship it now".into() }], ts: "9.0".into() },
            0,
        );
        let id = menu_id(&b.reconcile(0));
        let (_, o) = b.on_result(&result(&id, &[0]), 1).outcome.unwrap();
        assert_eq!(o, Outcome::SendText { slot: "a".into(), text: "Ship it now".into() });
        // Follow-ups expire.
        let mut b = board(&[slot("a")]);
        b.add_followup(
            FollowupCard { slot: "a".into(), items: vec![FollowupItem { title: "x".into(), prompt: "y".into() }], ts: "1.0".into() },
            0,
        );
        assert!(b.reconcile(FOLLOWUP_TTL_MS + 1).is_empty());
    }

    fn card(ask: &str, card_id: &str, native: bool, qs: &[(&str, &[&str], bool)]) -> QuestionCard {
        QuestionCard {
            ask_id: ask.into(),
            card_id: card_id.into(),
            slot: "a".into(),
            native,
            ts: "1".into(),
            questions: qs
                .iter()
                .map(|(q, opts, multi)| CardQuestion {
                    question: q.to_string(),
                    header: String::new(),
                    options: opts.iter().map(|l| CardOption { label: l.to_string() }).collect(),
                    multi_select: *multi,
                })
                .collect(),
        }
    }

    #[test]
    fn multi_question_card_collects_answers_then_answers_once() {
        let mut s = slot("a");
        s.needs_input = true;
        let mut b = MenuBoard::new(1);
        b.set_scope(&["a".into()], None);
        assert_eq!(b.update_slots(&[s]), vec!["a".to_string()]);
        b.set_questions("a", vec![card("ask1", "", false, &[("Which db?", &["pg", "sqlite"], false), ("Extras?", &["x", "y", "z"], true)])]);
        let f = b.reconcile(0);
        assert_eq!(f[0]["payload"]["kind"], "question");
        assert_eq!(f[0]["payload"]["title"], "A Question 1/2");
        let id = menu_id(&f);
        let r = b.on_result(&result(&id, &[1]), 1);
        assert!(r.outcome.is_none());
        let id2 = menu_id(&r.frames);
        assert_eq!(r.frames[0]["payload"]["select"], "multi");
        let (_, o) = b.on_result(&result(&id2, &[0, 2]), 2).outcome.unwrap();
        assert_eq!(
            o,
            Outcome::AnswerAsk {
                slot: "a".into(),
                ask_id: "ask1".into(),
                answers: vec![("Which db?".into(), "sqlite".into()), ("Extras?".into(), "x, z".into())]
            }
        );
    }

    #[test]
    fn native_card_steers_and_question_beats_options() {
        let mut s = slot("a");
        s.needs_input = true;
        s.has_options = true;
        s.options = vec!["o".into()];
        let mut b = board(&[s]);
        b.set_questions("a", vec![card("", "c9", true, &[("Go?", &["yes", "no"], false)])]);
        let f = b.reconcile(0);
        assert_eq!(f[0]["payload"]["kind"], "question");
        assert_eq!(f[0]["payload"]["queue"], "1/2");
        let (_, o) = b.on_result(&result(&menu_id(&f), &[0]), 1).outcome.unwrap();
        assert_eq!(o, Outcome::SteerNative { slot: "a".into(), card_id: "c9".into(), message: "yes".into() });
    }

    #[test]
    fn model_menu_maps_models_and_modes_without_yolo() {
        let mut s = slot("cur");
        s.model = "claude-opus".into();
        s.trust_reads = true;
        let mut b = board(&[s]);
        let models = vec![
            CrewModel { model_name: "auto".into(), display_name: "Auto".into() },
            CrewModel { model_name: "auto:jev".into(), display_name: "Auto (Jev)".into() },
            CrewModel { model_name: "claude-sonnet".into(), display_name: "".into() },
            CrewModel { model_name: "claude-opus".into(), display_name: "Opus".into() },
        ];
        let f = b.open_model_menu("cur", &models, 0);
        assert_eq!(f.len(), 2);
        assert_eq!(f[0]["payload"]["default"], 2);
        assert_eq!(f[0]["payload"]["timeout_s"], MODEL_MENU_TIMEOUT_S);
        let all = serde_json::to_string(&f).unwrap();
        assert!(!all.to_lowercase().contains("yolo") && !all.contains("Jev"));
        assert_eq!(f[1]["payload"]["kind"], "mode");
        assert_eq!(f[1]["payload"]["items"][1]["checked"], true);
        assert_eq!(f[1]["payload"]["items"][2]["needs_double_confirm"], true);
        let id = menu_id(&f);
        let (_, o) = b.on_result(&result(&id, &[1]), 1).outcome.unwrap();
        assert_eq!(o, Outcome::SetModel { slot: "cur".into(), model: "claude-sonnet".into() });
        let f = b.open_model_menu("cur", &models, 2);
        let (_, o) = b.on_result(&result(&menu_id(&f), &[5]), 3).outcome.unwrap();
        assert_eq!(o, Outcome::SetMode { slot: "cur".into(), mode: ApprovalMode::Trust });
        // Model menus time out on the Companion side too.
        let f = b.open_model_menu("cur", &models, 10);
        let late = (MODEL_MENU_TIMEOUT_S as u64) * 1000 + GRACE_MS + 11;
        assert!(b.on_result(&result(&menu_id(&f), &[0]), late).outcome.is_none());
    }

    #[test]
    fn live_sized_model_list_fits_in_pages() {
        // 24 entries as returned by a 0.12.2 gateway (model_name only).
        let names = [
            "auto", "claude-opus-5.5", "claude-sonnet-5.5", "claude-haiku-5.5", "gpt-5.6-sol", "gpt-5.6-terra",
            "gpt-5.6-luna", "grok-4.7", "kimi-k3", "claude-opus-5", "claude-sonnet-5", "claude-opus-4.8",
            "claude-opus-4.7", "claude-sonnet-4.6", "claude-opus-4.5", "claude-sonnet-4.5", "claude-fable-5.1",
            "claude-haiku-4.5", "deepseek-3.2", "minimax-m2.5", "minimax-m2.1", "glm-5", "qwen3-coder-next",
            "agi-nova-beta-1m",
        ];
        let models: Vec<CrewModel> =
            names.iter().map(|n| CrewModel { model_name: n.to_string(), display_name: String::new() }).collect();
        let mut s = slot("cur");
        s.model = "claude-sonnet-4.5".into();
        let mut b = board(&[s]);
        let f = b.open_model_menu("cur", &models, 0);
        assert_eq!(f.len(), 7, "6 model pages (4 per page) + 1 mode page");
        assert_eq!(f[0]["payload"]["default"], 15);
        assert_eq!(f[3]["payload"]["items"][3]["checked"], true);
        for frame in &f {
            assert!(serde_json::to_vec(frame).unwrap().len() <= crate::menu_wire::MAX_FRAME_BYTES);
        }
        let (_, o) = b.on_result(&result(&menu_id(&f), &[15]), 1).outcome.unwrap();
        assert_eq!(o, Outcome::SetModel { slot: "cur".into(), model: "claude-sonnet-4.5".into() });
    }

    #[test]
    fn model_toggle_target() {
        let mut s = slot("cur");
        let mut b = board(&[s.clone()]);
        assert_eq!(b.toggle_target("cur"), Err("No model picked yet"));
        assert_eq!(b.note_model_change("cur", "", "claude-opus"), Some("claude-opus".into()));
        assert_eq!(b.note_model_change("cur", "", "claude-opus"), None, "unchanged: nothing to persist");
        assert_eq!(b.toggle_target("cur"), Ok("claude-opus".into()));
        // Per chat: another chat has its own memory.
        let mut other = slot("a");
        other.model = String::new();
        b.update_slots(&[s.clone(), other]);
        assert_eq!(b.toggle_target("a"), Err("No model picked yet"));
        b.set_last_models([("a".to_string(), "gpt-5".to_string())]);
        assert_eq!(b.toggle_target("a"), Ok("gpt-5".into()));
        b.note_model_change("cur", "", "claude-opus");
        s.model = "claude-opus".into();
        b.update_slots(&[s]);
        assert_eq!(b.toggle_target("cur"), Ok("auto".into()));
        assert_eq!(b.note_model_change("cur", "claude-x", "auto"), Some("claude-x".into()));
        assert_eq!(b.last_models.get("cur").map(String::as_str), Some("claude-x"));
    }

    #[test]
    fn dictated_answer_flow() {
        // Reply options never get a dictate item; question cards do.
        let mut s = slot("a");
        s.has_options = true;
        s.options = vec!["Yes".into(), "No".into()];
        let mut b = board(&[s]);
        b.set_voice_ok(true);
        let f = b.reconcile(0);
        assert_eq!(f[0]["payload"]["items"].as_array().unwrap().len(), 2);

        let mut b = board(&[slot("a")]);
        b.set_voice_ok(true);
        b.set_questions("a", vec![card("ask1", "", false, &[("Which branch?", &["main", "dev"], false)])]);
        let f = b.reconcile(0);
        let items = f[0]["payload"]["items"].as_array().unwrap().clone();
        assert_eq!(items.len(), 3);
        assert_eq!(items[2]["role"], "voice");
        assert_eq!(items[2]["label"], DICTATE_LABEL);
        let id = menu_id(&f);
        // The voice item without text is refused.
        let r = b.on_result(&result(&id, &[2]), 1);
        assert!(r.outcome.is_none());
        assert_eq!(r.frames[0]["payload"]["msg"], "Invalid");
        let id = menu_id(&r.frames); // shown again right away
        let mut with_text = result(&id, &[2]);
        with_text.text = "use the\nrelease branch".into();
        let (_, o) = b.on_result(&with_text, 3).outcome.unwrap();
        assert_eq!(
            o,
            Outcome::AnswerAsk {
                slot: "a".into(),
                ask_id: "ask1".into(),
                answers: vec![("Which branch?".into(), "use the release branch".into())]
            }
        );
        // Text on a normal item is ignored: the label is the answer.
        let mut b = board(&[slot("a")]);
        b.set_voice_ok(true);
        b.set_questions("a", vec![card("ask2", "", false, &[("Which?", &["main"], false)])]);
        let id = menu_id(&b.reconcile(0));
        let mut r = result(&id, &[0]);
        r.text = "ignored".into();
        let (_, o) = b.on_result(&r, 1).outcome.unwrap();
        assert!(matches!(o, Outcome::AnswerAsk { answers, .. } if answers[0].1 == "main"));
        // No cloud ASR: no dictate item.
        let mut b = board(&[slot("a")]);
        b.set_questions("a", vec![card("ask3", "", false, &[("Which?", &["main"], false)])]);
        assert_eq!(b.reconcile(0)[0]["payload"]["items"].as_array().unwrap().len(), 1);
    }

    #[test]
    fn failure_allows_retry_and_reports() {
        let mut b = board(&[approval_slot("a", "r1", "m1")]);
        let id = menu_id(&b.reconcile(0));
        let (_, o) = b.on_result(&result(&id, &[0]), 1).outcome.unwrap();
        let f = b.finish(&id, &o, Err("HTTP 404"), 2);
        assert_eq!(f[0]["payload"]["msg"], "Failed");
        assert_eq!(f[1]["payload"]["kind"], "approval");
        assert_eq!(f.last().unwrap()["type"], "notice");
        assert_eq!(success_message(&o, false), "Approved");
    }
}
