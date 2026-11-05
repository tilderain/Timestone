// lobby-watcher/src/main.rs
// Headless Timestone lobby watcher (private dev tool). Polls all Timestone
// lobbies every 15 s through the public Steamworks lobby-search API and emits
// structured tracing events (target "lobby_watcher", event "created"/"updated"/
// "deleted"/"poll") describing lobby lifecycle changes. Every created or
// deleted lobby is also appended to a plain-text history log (LOBBY_LOG_FILE,
// default ./lobby-watcher.log) - the seed of a future database. Runs as the
// logged-in Steam user; the account must own UMVC3 (appid 357190).
// Optional Discord integration (bot.rs): set DISCORD_TOKEN (+ optional
// DISCORD_GUILD_ID for instant command registration) to enable /mm.
mod bot;

use std::collections::HashMap;
use std::fs::OpenOptions;
use std::io::Write;
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::time::{Duration, Instant, SystemTime, UNIX_EPOCH};

use parking_lot::Mutex;
use steamworks::{
    Client, DistanceFilter, LobbyId, LobbyKey, LobbyListFilter, SResult, StringFilter,
    StringFilterKind,
};

pub(crate) const APP_ID: u32 = 357190; // Ultimate Marvel vs. Capcom 3
const POLL_INTERVAL: Duration = Duration::from_secs(15);
const SEARCH_DEADLINE: Duration = Duration::from_secs(20);
const INIT_RETRY: Duration = Duration::from_secs(5);
const MAX_RESULTS: u64 = 50;
pub(crate) const EVENT_TARGET: &str = "lobby_watcher";
/// Env var for where the lifecycle log file is written.
const LOBBY_LOG_ENV: &str = "LOBBY_LOG_FILE";
/// Default lifecycle log path (current working directory).
const DEFAULT_LOBBY_LOG: &str = "lobby-watcher.log";

#[derive(Clone, Debug)]
pub(crate) struct Lobby {
    pub(crate) id: u64,
    pub(crate) name: String,
    pub(crate) host: String,
    pub(crate) protocol: String,
    pub(crate) members: usize,
    pub(crate) owner: u64, // SteamID64 of the lobby owner - join links need it
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
            owner: mm.lobby_owner(id).raw(),
        }
    }

    // Structural comparison (id is always the same key). Ping is not tracked:
    // steamworks 0.13.1 exposes no lobby ping estimate.
    fn same(&self, other: &Lobby) -> bool {
        self.name == other.name
            && self.host == other.host
            && self.protocol == other.protocol
            && self.members == other.members
            && self.owner == other.owner
    }
}

pub(crate) struct WatcherState {
    pub(crate) prev: HashMap<u64, Lobby>,
    absent: Vec<u64>, // ids absent from the immediately previous poll
    pub(crate) poll_index: u64,
    pending: bool, // a lobby-list request is in flight
    request_sent: Instant,
    next_poll: Instant,
    pub(crate) last_poll: Instant,
    lobby_log: PathBuf, // created/deleted events are appended here as text
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
        lobby.owner = lobby.owner,
        "Timestone lobby {event}",
    );
}

/// Append a created/deleted lobby to the lifecycle text log.
fn append_lobby_event(path: &Path, event: &str, lobby: &Lobby) {
    let line = format!("{}\n", lobby_log_line(&utc_now(), event, lobby));
    append_log_line(path, &line);
}

/// One human-readable log line: UTC timestamp, event, then every lobby field.
fn lobby_log_line(now: &str, event: &str, lobby: &Lobby) -> String {
    format!(
        "{} {} id={} name={:?} host={:?} protocol={:?} members={} owner={}",
        now, event, lobby.id, lobby.name, lobby.host, lobby.protocol, lobby.members, lobby.owner,
    )
}

/// Append a single line to the lifecycle log. The file is opened append-only
/// per call, so restarts, log rotation, or a mid-run delete are tolerated; an
/// unwritable path only logs a warning and never stops the watcher.
fn append_log_line(path: &Path, line: &str) {
    if let Err(e) = (|| -> std::io::Result<()> {
        let mut f = OpenOptions::new().create(true).append(true).open(path)?;
        f.write_all(line.as_bytes())?;
        f.flush()
    })() {
        tracing::warn!(
            target: EVENT_TARGET,
            event = "log_failed",
            path = %path.display(),
            error = %e,
            "could not write lobby log",
        );
    }
}

/// Current UTC time as "YYYY-MM-DDTHH:MM:SSZ" (RFC 3339, no sub-seconds).
fn utc_now() -> String {
    let secs = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_secs();
    utc_from_epoch(secs)
}

/// Format seconds since the Unix epoch as an RFC 3339 UTC timestamp.
fn utc_from_epoch(secs: u64) -> String {
    let days = secs / 86_400;
    let rem = secs % 86_400;
    let (h, m, s) = (rem / 3600, (rem % 3600) / 60, rem % 60);
    let (y, mo, d) = civil_from_days(days as i64);
    format!("{y:04}-{mo:02}-{d:02}T{h:02}:{m:02}:{s:02}Z")
}

/// Days since 1970-01-01 -> (year, month, day) in the proleptic Gregorian
/// calendar. Howard Hinnant's civil_from_days algorithm.
fn civil_from_days(z: i64) -> (i64, u32, u32) {
    let z = z + 719_468;
    let era = (if z >= 0 { z } else { z - 146_096 }) / 146_097;
    let doe = (z - era * 146_097) as u64; // [0, 146096]
    let yoe = (doe - doe / 1_460 + doe / 36_524 - doe / 146_096) / 365; // [0, 399]
    let y = yoe as i64 + era * 400;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100); // [0, 365]
    let mp = (5 * doy + 2) / 153; // [0, 11]
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32; // [1, 31]
    let m = if mp < 10 { mp + 3 } else { mp - 9 }; // [1, 12]
    let y = if m <= 2 { y + 1 } else { y };
    (y, m as u32, d)
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
                append_lobby_event(&state.lobby_log, "created", lobby);
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
            append_lobby_event(&state.lobby_log, "deleted", lobby);
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
    state.last_poll = Instant::now();
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

    // Plain-text history of created/deleted lobbies. Override the location
    // with LOBBY_LOG_FILE; the watcher never needs the log to keep polling.
    let lobby_log = PathBuf::from(match std::env::var(LOBBY_LOG_ENV) {
        Ok(path) if !path.trim().is_empty() => path,
        _ => DEFAULT_LOBBY_LOG.to_string(),
    });

    let state = Arc::new(Mutex::new(WatcherState {
        prev: HashMap::new(),
        absent: Vec::new(),
        poll_index: 0,
        pending: false,
        request_sent: Instant::now(),
        next_poll: Instant::now(), // first search fires immediately
        last_poll: Instant::now(),
        lobby_log: lobby_log.clone(),
    }));

    match std::env::var("DISCORD_TOKEN") {
        Ok(token) => {
            let state2 = Arc::clone(&state);
            std::thread::spawn(move || bot::run(&token, state2));
            tracing::info!(
                target: EVENT_TARGET,
                event = "discord_start",
                "discord bot enabled (token from DISCORD_TOKEN)",
            );
        }
        Err(_) => tracing::warn!(
            target: EVENT_TARGET,
            event = "discord_disabled",
            "DISCORD_TOKEN not set - running watcher only",
        ),
    }

    tracing::info!(
        target: EVENT_TARGET,
        event = "start",
        "watching for Timestone lobbies every 15 s",
    );
    append_log_line(
        &lobby_log,
        &format!("{} start watcher pid={}\n", utc_now(), std::process::id()),
    );
    loop {
        client.run_callbacks();

        let mut st = state.lock();
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
                let mut st = state2.lock();
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

#[cfg(test)]
mod tests {
    use super::{lobby_log_line, utc_from_epoch, Lobby};

    #[test]
    fn utc_from_epoch_is_rfc3339_utc() {
        assert_eq!(utc_from_epoch(0), "1970-01-01T00:00:00Z");
        // 2024 is a leap year.
        assert_eq!(utc_from_epoch(1_709_210_096), "2024-02-29T12:34:56Z");
        // 2100 is a leap-century year (divisible by 100, not 400): Feb has 28 days.
        assert_eq!(utc_from_epoch(4_107_456_000), "2100-02-28T00:00:00Z");
        assert_eq!(
            utc_from_epoch(4_107_456_000 + 86_400),
            "2100-03-01T00:00:00Z"
        );
    }

    #[test]
    fn log_line_is_one_escaped_row() {
        let lobby = Lobby {
            id: 109775999888744469,
            name: "Party \"Up\"".to_string(),
            host: "John\tDoe".to_string(),
            protocol: "1".to_string(),
            members: 2,
            owner: 76561198000000000,
        };
        assert_eq!(
            lobby_log_line("2026-09-08T19:40:01Z", "created", &lobby),
            "2026-09-08T19:40:01Z created id=109775999888744469 \
             name=\"Party \\\"Up\\\"\" host=\"John\\tDoe\" \
             protocol=\"1\" members=2 owner=76561198000000000"
        );
    }
}
