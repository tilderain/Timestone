// lobby-watcher/src/main.rs
// Headless Timestone lobby watcher (private dev tool). Polls all Timestone
// lobbies every 15 s through the public Steamworks lobby-search API and emits
// structured tracing events (target "lobby_watcher", event "created"/"updated"/
// "deleted"/"poll") describing lobby lifecycle changes. Runs as the logged-in
// Steam user; the account must own UMVC3 (appid 357190). Future Discord-bot and
// rich-presence integrations will consume these events; for now they are log
// lines via the default tracing_subscriber fmt layer.
use std::collections::HashMap;
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use steamworks::{
    Client, DistanceFilter, LobbyId, LobbyKey, LobbyListFilter, SResult, StringFilter,
    StringFilterKind,
};

const APP_ID: u32 = 357190; // Ultimate Marvel vs. Capcom 3
const POLL_INTERVAL: Duration = Duration::from_secs(15);
const SEARCH_DEADLINE: Duration = Duration::from_secs(20);
const INIT_RETRY: Duration = Duration::from_secs(5);
const MAX_RESULTS: u64 = 50;
const EVENT_TARGET: &str = "lobby_watcher";

#[derive(Clone, Debug)]
struct Lobby {
    id: u64,
    name: String,
    host: String,
    protocol: String,
    members: usize,
}

impl Lobby {
    fn read(mm: &steamworks::Matchmaking, id: LobbyId) -> Lobby {
        let data = |key: &str| mm.lobby_data(id, key).unwrap_or_default();
        Lobby {
            id: id.raw(),
            name: data("name"),
            host: data("username"),
            protocol: data("protocol"),
            members: mm.lobby_member_count(id),
        }
    }

    // Structural comparison (id is always the same key). Ping is not tracked:
    // steamworks 0.13.1 exposes no lobby ping estimate.
    fn same(&self, other: &Lobby) -> bool {
        self.name == other.name
            && self.host == other.host
            && self.protocol == other.protocol
            && self.members == other.members
    }
}

struct WatcherState {
    prev: HashMap<u64, Lobby>,
    absent: Vec<u64>, // ids absent from the immediately previous poll
    poll_index: u64,
    pending: bool, // a lobby-list request is in flight
    request_sent: Instant,
    next_poll: Instant,
}

fn emit_event(event: &str, lobby: &Lobby) {
    tracing::info!(
        target: EVENT_TARGET,
        event = event,
        lobby.id = lobby.id,
        lobby.name = %lobby.name,
        lobby.host = %lobby.host,
        lobby.protocol = %lobby.protocol,
        lobby.members = lobby.members,
        "Timestone lobby {event}",
    );
}

fn process_poll(state: &mut WatcherState, current: Vec<Lobby>) {
    let mut created = 0u32;
    let mut updated = 0u32;
    let mut deleted = 0u32;
    let mut absent_now: Vec<u64> = Vec::new();
    let mut next_prev: HashMap<u64, Lobby> = HashMap::new();

    for lobby in &current {
        next_prev.insert(lobby.id, lobby.clone());
        match state.prev.get(&lobby.id) {
            None => {
                emit_event("created", lobby);
                created += 1;
            }
            Some(prev) if !prev.same(lobby) => {
                emit_event("updated", lobby);
                updated += 1;
            }
            _ => {}
        }
    }
    for lobby in state.prev.values() {
        if next_prev.contains_key(&lobby.id) {
            continue;
        }
        absent_now.push(lobby.id);
        if state.absent.contains(&lobby.id) {
            emit_event("deleted", lobby);
            deleted += 1;
        }
    }

    tracing::info!(
        target: EVENT_TARGET,
        event = "poll",
        poll = state.poll_index,
        total = current.len(),
        created,
        deleted,
        updated,
        "lobby poll complete",
    );
    state.prev = next_prev;
    state.absent = absent_now;
}

fn main() {
    tracing_subscriber::fmt::init();

    let client = loop {
        match Client::init_app(APP_ID) {
            Ok(client) => break client,
            Err(e) => {
                tracing::error!(
                    target: EVENT_TARGET,
                    event = "init_failed",
                    "SteamAPI init failed: {e:?} - retrying in 5 s \
                     (Steam running and logged in, account owns UMVC3?)",
                );
                std::thread::sleep(INIT_RETRY);
            }
        }
    };
    let mm = client.matchmaking();

    let state = Arc::new(Mutex::new(WatcherState {
        prev: HashMap::new(),
        absent: Vec::new(),
        poll_index: 0,
        pending: false,
        request_sent: Instant::now(),
        next_poll: Instant::now(), // first search fires immediately
    }));

    tracing::info!(
        target: EVENT_TARGET,
        event = "start",
        "watching for Timestone lobbies every 15 s",
    );
    loop {
        client.run_callbacks();

        let mut st = state.lock().unwrap();
        let now = Instant::now();
        if !st.pending && now >= st.next_poll {
            st.pending = true;
            st.request_sent = now;
            st.next_poll = now + POLL_INTERVAL; // next scheduled search
            mm.set_lobby_list_filter(LobbyListFilter {
                string: Some(vec![StringFilter(
                    LobbyKey::new("Timestone"),
                    "1",
                    StringFilterKind::Equal,
                )]),
                open_slots: Some(1),
                distance: Some(DistanceFilter::Worldwide),
                count: Some(MAX_RESULTS),
                ..Default::default()
            });
            let state2 = Arc::clone(&state);
            let client2 = client.clone();
            mm.request_lobby_list(move |res: SResult<Vec<LobbyId>>| {
                let mut st = state2.lock().unwrap();
                st.pending = false;
                match res {
                    Ok(ids) => {
                        let mm = client2.matchmaking();
                        let current: Vec<Lobby> =
                            ids.iter().map(|id| Lobby::read(&mm, *id)).collect();
                        st.poll_index += 1;
                        process_poll(&mut st, current);
                    }
                    Err(e) => tracing::warn!(
                        target: EVENT_TARGET,
                        event = "search_failed",
                        "lobby search failed: {e:?}",
                    ),
                }
            });
        } else if st.pending && now.duration_since(st.request_sent) > SEARCH_DEADLINE {
            // A hung search: clear pending so the next iteration re-fires
            // (a new request cancels the previous one - documented crate behavior).
            st.pending = false;
            st.next_poll = now;
        }
        drop(st);

        std::thread::sleep(Duration::from_millis(50));
    }
}
