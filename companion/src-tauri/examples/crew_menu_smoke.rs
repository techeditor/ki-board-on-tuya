//! Read-only live check of the Crew fields the LCD menus use.
//!
//! Run from `companion/src-tauri`:
//!   cargo run --example crew_menu_smoke
//!
//! Only GET requests. Prints counts, field presence and model ids; never the
//! token, chat titles, messages, commands or answers.

use agentic_keyboard_companion_lib::crew_api::CrewApiClient;

fn main() -> Result<(), String> {
    let client = CrewApiClient::from_env()?;
    let slots = client.list_slots()?;
    let with_info = slots.iter().filter(|s| s.pending_approval_info.is_some()).count();
    let native = slots
        .iter()
        .filter_map(|s| s.pending_approval_info.as_ref())
        .filter(|i| i.origin == "native" && !i.request_id.is_empty() && !i.request_mid.is_empty())
        .count();
    let with_options = slots.iter().filter(|s| s.has_options && !s.options.is_empty()).count();
    let needs_input: Vec<&str> = slots.iter().filter(|s| s.needs_input).map(|s| s.key.as_str()).collect();
    let pinned = slots.iter().filter(|s| !s.model.is_empty()).count();
    let trust = slots.iter().filter(|s| s.trust).count();
    let reads = slots.iter().filter(|s| s.trust_reads).count();
    println!(
        "slots={} approval_info={with_info} native_bound={native} options={with_options} needs_input={} pinned_model={pinned} trust={trust} trust_reads={reads}",
        slots.len(),
        needs_input.len()
    );
    match client.list_models() {
        Ok(models) => {
            let ids: Vec<&str> = models.iter().map(|m| m.model_name.as_str()).collect();
            println!("models={} has_auto={} ids={ids:?}", models.len(), ids.contains(&"auto"));
        }
        Err(e) => println!("models error: {e}"),
    }
    for slot in needs_input.iter().take(3) {
        match client.slot_questions(slot) {
            Ok(cards) => println!(
                "questions slot=<redacted> cards={} blocking={} native={} first_question_options={}",
                cards.len(),
                cards.iter().filter(|c| !c.ask_id.is_empty()).count(),
                cards.iter().filter(|c| c.native).count(),
                cards.first().and_then(|c| c.questions.first()).map(|q| q.options.len()).unwrap_or(0)
            ),
            Err(e) => println!("questions error: {e}"),
        }
    }
    Ok(())
}
