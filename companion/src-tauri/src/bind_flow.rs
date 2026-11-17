//! Settings > Bind session on the keyboard (`shared/protocol/protocol.md`
//! "绑定会话"): after `bind_session {slot}` the Companion shows an instance
//! menu (only when remote crews exist), then a session menu, and ends each
//! step with `bind_done`. This module builds those menus and says what an
//! answer means; the app layer does the Crew calls and the binding. Pure.

use crate::bind_store::{row_identity, LOCAL_INSTANCE};
use crate::crew_api::{CrewInstance, CrewSlot};
use crate::lcd_text::tr;
use crate::menu_wire::{encode_menu, MenuIdGen, MenuItem, MenuKind, MenuRole, MenuSpec, ITEMS_TOTAL_MAX};
use crate::protocol::MenuResultPayload;
use serde_json::Value;

/// Keyboard-side timeout of each menu; on expiry the flow ends (`closed`).
pub const MENU_TIMEOUT_S: u32 = 60;

#[derive(Clone, Debug, PartialEq, Eq)]
enum Step {
    /// Item 0 = this machine, then `instances[i - 1]`.
    Instance,
    /// `targets[i]`: the slot key, `None` for the closing "Unbind" item.
    Session { instance: String, targets: Vec<Option<(String, String)>> },
}

/// What the app layer does with an answer.
#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Next {
    /// Not this flow's menu.
    NotOurs,
    /// List this instance's sessions, then call [`BindFlow::session_menu`].
    ListSessions { instance: String },
    /// Back from the session menu to the instance menu: write these frames.
    Frames(Vec<Value>),
    /// Bind the slot to `row` (row identity) named `name`.
    Bind { row: String, name: String },
    /// Clear the slot's binding.
    Unbind,
    /// End the flow with `bind_done` `result` (`back` / `closed`).
    Done(&'static str),
}

#[derive(Debug)]
pub struct BindFlow {
    pub slot: u8,
    ids: MenuIdGen,
    zh: bool,
    instances: Vec<CrewInstance>,
    /// The id of the menu on the keyboard and what its items stand for.
    menu_id: String,
    step: Step,
}

fn letter(slot: u8) -> char {
    (b'A' + slot.saturating_sub(1).min(2)) as char
}

impl BindFlow {
    /// A flow for slot `slot` (1..3). `instances` = usable remote crews.
    pub fn new(slot: u8, seed: u64, zh: bool, instances: Vec<CrewInstance>) -> Self {
        BindFlow { slot, ids: MenuIdGen::new(seed), zh, instances, menu_id: String::new(), step: Step::Instance }
    }

    /// Whether the first step picks an instance (remote crews exist).
    pub fn needs_instance(&self) -> bool {
        !self.instances.is_empty()
    }

    pub fn menu_id(&self) -> &str {
        &self.menu_id
    }

    fn title(&self) -> String {
        format!("{} {}", tr("Bind", self.zh), letter(self.slot))
    }

    fn encode(&mut self, kind: MenuKind, items: Vec<MenuItem>, default: usize) -> Vec<Value> {
        self.menu_id = self.ids.next_id();
        let mut spec = MenuSpec::single(self.menu_id.clone(), kind, self.title(), items);
        spec.timeout_s = MENU_TIMEOUT_S;
        spec.default = default;
        encode_menu(&spec).unwrap_or_default()
    }

    /// Instance menu: this machine first, then the remote crews in the
    /// dashboard switcher's order. `bound` = the slot's row identity.
    pub fn instance_menu(&mut self, bound: &str) -> Vec<Value> {
        let bound_inst = bound.split_once(':').map(|(i, _)| i).unwrap_or("");
        let mut items = vec![MenuItem::new(tr("This computer", self.zh), MenuRole::Normal).checked(bound_inst == LOCAL_INSTANCE)];
        items.extend(
            self.instances.iter().map(|i| MenuItem::new(i.name.clone(), MenuRole::Normal).checked(bound_inst == i.id)),
        );
        let default = items.iter().position(|i| i.checked).unwrap_or(0);
        self.step = Step::Instance;
        self.encode(MenuKind::Instance, items, default)
    }

    /// Session menu for `instance` (`local` or a remote id): Bots first (by
    /// name), then the other sessions in Crew's order, workers only when
    /// `show_workers`; "Unbind" last. `bound` = the slot's row identity.
    pub fn session_menu(&mut self, instance: &str, slots: &[CrewSlot], show_workers: bool, bound: &str) -> Vec<Value> {
        let visible = |s: &&CrewSlot| show_workers || !s.is_worker();
        let mut bots: Vec<&CrewSlot> = slots.iter().filter(|s| s.is_bot()).filter(visible).collect();
        bots.sort_by_key(|s| display_name(s).to_lowercase());
        let others = slots.iter().filter(|s| !s.is_bot()).filter(visible);
        // shortcut: the keyboard takes 48 items (12 pages); beyond 47 sessions the
        // oldest-listed are cut. Add a search / filter step if users hit it.
        let rows: Vec<&CrewSlot> = bots.into_iter().chain(others).take(ITEMS_TOTAL_MAX - 1).collect();
        let mut items = Vec::with_capacity(rows.len() + 1);
        let mut targets = Vec::with_capacity(rows.len() + 1);
        for s in rows {
            let name = display_name(s);
            let label = if s.is_bot() { format!("@{name}") } else { name.clone() };
            let row = row_identity(&format!("{instance}:{}", s.key));
            items.push(MenuItem::new(label, MenuRole::Normal).checked(row == bound));
            targets.push(Some((s.key.clone(), name)));
        }
        items.push(MenuItem::new(tr("Unbind", self.zh), MenuRole::Cancel));
        targets.push(None);
        let default = items.iter().position(|i| i.checked).unwrap_or(0);
        self.step = Step::Session { instance: instance.to_string(), targets };
        self.encode(MenuKind::Session, items, default)
    }

    /// The keyboard answered menu `r.id`.
    pub fn on_result(&mut self, r: &MenuResultPayload, bound: &str) -> Next {
        if r.id != self.menu_id || self.menu_id.is_empty() {
            return Next::NotOurs;
        }
        self.menu_id.clear(); // one answer per menu
        if r.dismissed || r.selected.is_empty() {
            let timed_out = r.reason == "timeout";
            return match (&self.step, timed_out) {
                (_, true) => Next::Done("closed"),
                (Step::Session { .. }, false) if self.needs_instance() => Next::Frames(self.instance_menu(bound)),
                _ => Next::Done("back"),
            };
        }
        let pick = r.selected[0] as usize;
        match &self.step {
            Step::Instance => match pick {
                0 => Next::ListSessions { instance: LOCAL_INSTANCE.to_string() },
                n => match self.instances.get(n - 1) {
                    Some(i) => Next::ListSessions { instance: i.id.clone() },
                    None => Next::Done("back"),
                },
            },
            Step::Session { instance, targets } => match targets.get(pick) {
                Some(Some((key, name))) => Next::Bind { row: row_identity(&format!("{instance}:{key}")), name: name.clone() },
                Some(None) => Next::Unbind,
                None => Next::Done("back"),
            },
        }
    }
}

fn display_name(s: &CrewSlot) -> String {
    let t = s.title.split_whitespace().collect::<Vec<_>>().join(" ");
    if !t.is_empty() {
        t
    } else if !s.agent.is_empty() {
        s.agent.clone()
    } else {
        s.key.clone()
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use serde_json::json;

    fn slot(v: Value) -> CrewSlot {
        serde_json::from_value(v).unwrap()
    }

    fn labels(frames: &[Value]) -> Vec<String> {
        frames
            .iter()
            .flat_map(|f| f["payload"]["items"].as_array().cloned().unwrap_or_default())
            .map(|i| i["label"].as_str().unwrap_or("").to_string())
            .collect()
    }

    fn answer(id: &str, selected: &[u32]) -> MenuResultPayload {
        MenuResultPayload { id: id.into(), selected: selected.to_vec(), dismissed: false, reason: String::new(), text: String::new() }
    }

    fn esc(id: &str, reason: &str) -> MenuResultPayload {
        MenuResultPayload { id: id.into(), selected: vec![], dismissed: true, reason: reason.into(), text: String::new() }
    }

    fn slots() -> Vec<CrewSlot> {
        vec![
            slot(json!({"key": "chat-2-1", "title": "Fix the parser"})),
            slot(json!({"key": "chat-3-1", "title": "worker", "mate_root": "member-mate"})),
            slot(json!({"key": "member-zed", "title": "Zed", "mode": "member"})),
            slot(json!({"key": "member-mate", "title": "Ki-board Mate", "mode": "member"})),
            slot(json!({"key": "chat-4-1", "title": "", "agent": "planner"})),
        ]
    }

    #[test]
    fn local_only_skips_the_instance_step_and_binds_a_session() {
        let mut f = BindFlow::new(2, 7, false, vec![]);
        assert!(!f.needs_instance());
        let frames = f.session_menu("local", &slots(), false, "local:chat-4-1");
        assert_eq!(frames[0]["payload"]["kind"], "session");
        assert_eq!(frames[0]["payload"]["title"], "Bind B");
        assert_eq!(frames[0]["payload"]["timeout_s"], MENU_TIMEOUT_S);
        // Bots first by name, then sessions in Crew's order, workers hidden, Unbind last.
        assert_eq!(labels(&frames), ["@Ki-board Mate", "@Zed", "Fix the parser", "planner", "Unbind"]);
        assert_eq!(frames[0]["payload"]["default"], 3, "the bound session is highlighted");
        let id = f.menu_id().to_string();
        assert_eq!(f.on_result(&answer("other", &[0]), ""), Next::NotOurs);
        assert_eq!(
            f.on_result(&answer(&id, &[0]), ""),
            Next::Bind { row: "local:member-mate".into(), name: "Ki-board Mate".into() }
        );
        assert_eq!(f.on_result(&answer(&id, &[0]), ""), Next::NotOurs, "one answer per menu");

        let frames = f.session_menu("local", &slots(), true, "");
        assert!(labels(&frames).contains(&"worker".to_string()), "workers listed when enabled");
        let id = f.menu_id().to_string();
        assert_eq!(f.on_result(&answer(&id, &[5]), ""), Next::Unbind);
        f.session_menu("local", &slots(), false, "");
        let id = f.menu_id().to_string();
        assert_eq!(f.on_result(&esc(&id, "esc"), ""), Next::Done("back"), "K1 without instances: slot page");
        f.session_menu("local", &slots(), false, "");
        let id = f.menu_id().to_string();
        assert_eq!(f.on_result(&esc(&id, "timeout"), ""), Next::Done("closed"));
    }

    #[test]
    fn remote_crews_add_the_instance_step() {
        let inst = vec![CrewInstance { id: "remote1".into(), name: "Remote 1".into() }];
        let mut f = BindFlow::new(1, 9, true, inst);
        let frames = f.instance_menu("remote1:chat-9");
        assert_eq!(frames[0]["payload"]["kind"], "instance");
        assert_eq!(labels(&frames), ["本机", "Remote 1"]);
        assert_eq!(frames[0]["payload"]["default"], 1);
        let id = f.menu_id().to_string();
        assert_eq!(f.on_result(&answer(&id, &[1]), ""), Next::ListSessions { instance: "remote1".into() });

        let remote = vec![slot(json!({"key": "chat-9", "title": "Deploy"})), slot(json!({"key": "member-ops", "title": "Ops"}))];
        let frames = f.session_menu("remote1", &remote, false, "remote1:chat-9");
        assert_eq!(labels(&frames), ["@Ops", "Deploy", "解除绑定"]);
        let id = f.menu_id().to_string();
        // K1 goes back to the instance menu, a new menu id.
        let Next::Frames(back) = f.on_result(&esc(&id, "esc"), "remote1:chat-9") else { panic!("expected the instance menu") };
        assert_eq!(back[0]["payload"]["kind"], "instance");
        let id = f.menu_id().to_string();
        assert_eq!(f.on_result(&answer(&id, &[0]), ""), Next::ListSessions { instance: "local".into() });
        f.session_menu("remote1", &remote, false, "");
        let id = f.menu_id().to_string();
        assert_eq!(f.on_result(&answer(&id, &[1]), ""), Next::Bind { row: "remote1:chat-9".into(), name: "Deploy".into() });
        f.instance_menu("");
        let id = f.menu_id().to_string();
        assert_eq!(f.on_result(&esc(&id, "esc"), ""), Next::Done("back"), "K1 on the instance menu: slot page");
    }

    #[test]
    fn long_lists_fit_the_keyboard() {
        let many: Vec<CrewSlot> = (0..80).map(|i| slot(json!({"key": format!("chat-{i}-1"), "title": format!("Session {i}")}))).collect();
        let mut f = BindFlow::new(3, 1, false, vec![]);
        let frames = f.session_menu("local", &many, false, "");
        let l = labels(&frames);
        assert_eq!(l.len(), ITEMS_TOTAL_MAX);
        assert_eq!(l.last().map(String::as_str), Some("Unbind"));
        assert!(frames.iter().all(|fr| fr.to_string().len() <= crate::menu_wire::MAX_FRAME_BYTES));
    }
}
