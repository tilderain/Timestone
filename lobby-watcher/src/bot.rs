// lobby-watcher/src/bot.rs
// Discord integration for the lobby watcher. Runs inside the watcher process
// (shares its live lobby state) and exposes slash commands:
//   /mm  — post a complex embed listing every open Timestone lobby with a
//          https://mm.b.hero.rehab join link for each.
// The bot is optional: if DISCORD_TOKEN is unset the watcher runs without it.
use std::sync::Arc;
use std::time::{Duration, Instant};

use parking_lot::Mutex;
use poise::serenity_prelude as serenity;

use crate::{Lobby, WatcherState, APP_ID};

pub struct Data {
    pub state: Arc<Mutex<WatcherState>>,
}

pub type Error = Box<dyn std::error::Error + Send + Sync>;
pub type Context<'a> = poise::Context<'a, Data, Error>;

/// Run the bot until the client disconnects. Blocks the calling thread.
pub fn run(token: &str, state: Arc<Mutex<WatcherState>>) {
    // setup() below moves `state` into the framework's Data; the live-card
    // task gets its own handle on the same watcher state.
    let card_state = Arc::clone(&state);
    let framework = poise::Framework::builder()
        .options(poise::FrameworkOptions {
            commands: vec![mm()],
            ..Default::default()
        })
        .setup(move |ctx, _ready, framework| {
            Box::pin(async move {
                match std::env::var("DISCORD_GUILD_ID") {
                    Ok(guild) => {
                        let gid = serenity::GuildId::new(guild.parse::<u64>()?);
                        poise::builtins::register_in_guild(ctx, &framework.options().commands, gid)
                            .await?;
                    }
                    Err(_) => {
                        poise::builtins::register_globally(ctx, &framework.options().commands)
                            .await?;
                    }
                }
                Ok(Data { state })
            })
        })
        .build();

    let result = tokio::runtime::Builder::new_multi_thread()
        .enable_all()
        .build()
        .expect("tokio runtime")
        .block_on(async {
            let client = serenity::Client::builder(
                token.to_string(),
                serenity::GatewayIntents::non_privileged(),
            )
            .activity(serenity::ActivityData::watching("Timestone lobbies"))
            .framework(framework)
            .await;
            match client {
                Ok(mut client) => {
                    // Live lobby card: a background task renders once per poll
                    // into a single channel message. Independent of slash
                    // commands; it just reads the same shared state.
                    tokio::spawn(run_lobby_card(client.http.clone(), card_state.clone()));
                    client.start().await
                }
                Err(e) => Err(e),
            }
        });
    if let Err(e) = result {
        tracing::error!(
            target: crate::EVENT_TARGET,
            event = "discord_error",
            "discord client failed: {e:?}",
        );
    }
}

/// List all open Timestone lobbies as an embed with a join link per lobby.
#[poise::command(slash_command)]
async fn mm(ctx: Context<'_>) -> Result<(), Error> {
    let (lobbies, poll_index, updated_ago) = lobby_snapshot(&ctx.data().state.lock());
    let embed = build_mm_embed(&lobbies, poll_index, updated_ago);
    ctx.send(poise::CreateReply::default().embed(embed)).await?;
    Ok(())
}

const JOIN_BASE_URL: &str = "https://mm.b.hero.rehab/joinlobby";

/// https://mm.b.hero.rehab/joinlobby/<appid>/<lobby_id>/<owner_steamid> —
/// join-link landing page for the lobby on the matchmaking service.
pub fn join_link(l: &Lobby) -> String {
    format!("{JOIN_BASE_URL}/{APP_ID}/{}/{}", l.id, l.owner)
}

/// The /mm embed: one field per open lobby (busiest first), each with a join
/// link; empty-state description when no lobbies are up.
fn build_mm_embed(lobbies: &[Lobby], poll_index: u64, updated_ago: Duration) -> serenity::CreateEmbed {
    let mut e = serenity::CreateEmbed::new()
        .title(format!("Open Timestone Lobbies — {}", lobbies.len()))
        .color(0x9b59b6);
    if lobbies.is_empty() {
        e = e.description("No open lobbies right now. Try again in a bit.");
    } else {
        for l in lobbies.iter().take(25) {
            e = e.field(
                format!("{} — {}", l.name, l.host),
                format!(
                    "protocol {} • {}/8 players\n[Join lobby]({})",
                    l.protocol,
                    l.members,
                    join_link(l)
                ),
                false,
            );
        }
        if lobbies.len() > 25 {
            e = e.field(
                format!("+{} more", lobbies.len() - 25),
                "Showing the first 25 lobbies.",
                false,
            );
        }
    }
    e.footer(serenity::CreateEmbedFooter::new(format!(
        "poll #{poll_index} • updated {:.0} s ago",
        updated_ago.as_secs_f64()
    )))
}

// ── Live lobby card ─────────────────────────────────────────────────────

const LOBBY_CHANNEL_ENV: &str = "DISCORD_LOBBY_CHANNEL_ID";
/// After a failed card send/edit, wait this long before trying again.
const CARD_RETRY: Duration = Duration::from_secs(300);

/// The live card and /mm share one view of the watcher state: every lobby,
/// busiest first, plus the poll index and time since the last completed poll
/// (both shown in the embed footer).
fn lobby_snapshot(state: &WatcherState) -> (Vec<Lobby>, u64, Duration) {
    let mut lobbies: Vec<Lobby> = state.prev.values().cloned().collect();
    lobbies.sort_by(|a, b| b.members.cmp(&a.members));
    (lobbies, state.poll_index, state.last_poll.elapsed())
}

/// True when an embed title is the one [`build_mm_embed`] produces — used to
/// find the existing card after a restart instead of posting a duplicate.
fn is_lobby_card_title(title: &str) -> bool {
    title.starts_with("Open Timestone Lobbies")
}

fn is_lobby_card(msg: &serenity::Message) -> bool {
    msg.embeds
        .iter()
        .any(|e| e.title.as_deref().map(is_lobby_card_title).unwrap_or(false))
}

/// HTTP status of a serenity error when the failure was an unsuccessful
/// request (404 = the card message was deleted; 403 = permissions revoked).
fn http_status(e: &serenity::Error) -> Option<u16> {
    match e {
        serenity::Error::Http(err) => err.status_code().map(|s| s.as_u16()),
        _ => None,
    }
}

/// Read DISCORD_LOBBY_CHANNEL_ID and verify it names a text channel the bot
/// can see. Logs and returns None when the card cannot run (the watcher keeps
/// going either way).
async fn resolve_lobby_channel(http: &serenity::Http) -> Option<serenity::ChannelId> {
    let raw = match std::env::var(LOBBY_CHANNEL_ENV) {
        Ok(v) if !v.is_empty() => v,
        _ => {
            tracing::info!(
                target: crate::EVENT_TARGET,
                event = "card_disabled",
                "{LOBBY_CHANNEL_ENV} not set - no live lobby card",
            );
            return None;
        }
    };
    let channel = match raw.parse::<u64>() {
        Ok(id) => serenity::ChannelId::new(id),
        Err(_) => {
            tracing::error!(
                target: crate::EVENT_TARGET,
                event = "card_error",
                reason = "bad channel id",
                "{LOBBY_CHANNEL_ENV}={raw} is not a valid snowflake",
            );
            return None;
        }
    };
    match channel.to_channel(http).await {
        Ok(serenity::Channel::Guild(gc)) if gc.kind == serenity::ChannelType::Text => {
            tracing::info!(
                target: crate::EVENT_TARGET,
                event = "card_start",
                channel = channel.get(),
                channel_name = %gc.name,
                "lobby card will post in the configured channel",
            );
            Some(channel)
        }
        Ok(serenity::Channel::Guild(gc)) => {
            tracing::error!(
                target: crate::EVENT_TARGET,
                event = "card_error",
                reason = "not a text channel",
                channel_kind = ?gc.kind,
                "{LOBBY_CHANNEL_ENV}={raw} is not a text channel",
            );
            None
        }
        Ok(_) => {
            tracing::error!(
                target: crate::EVENT_TARGET,
                event = "card_error",
                reason = "not a guild channel",
                "{LOBBY_CHANNEL_ENV}={raw} is not in a guild",
            );
            None
        }
        Err(e) => {
            tracing::error!(
                target: crate::EVENT_TARGET,
                event = "card_error",
                reason = "channel lookup failed",
                "cannot resolve {LOBBY_CHANNEL_ENV}={raw}: {e:?}",
            );
            None
        }
    }
}

/// Background task (one per process): render the open-lobby embed after every
/// completed poll and keep a single channel message in sync with it. Failures
/// are logged and retried after [`CARD_RETRY`]; a deleted message (404) is
/// recreated on the next attempt. Never exits.
async fn run_lobby_card(http: Arc<serenity::Http>, state: Arc<Mutex<WatcherState>>) {
    let Some(channel) = resolve_lobby_channel(&http).await else {
        return;
    };

    // Reuse the card from a previous run so restarts don't leave duplicates.
    let mut message_id: Option<serenity::MessageId> = None;
    match channel.messages(&http, serenity::GetMessages::new().limit(10)).await {
        Ok(msgs) => {
            if let Some(existing) = msgs.iter().find(|m| is_lobby_card(m)) {
                tracing::info!(
                    target: crate::EVENT_TARGET,
                    event = "card_found",
                    message = existing.id.get(),
                    "reusing existing lobby card message",
                );
                message_id = Some(existing.id);
            }
        }
        Err(e) => tracing::warn!(
            target: crate::EVENT_TARGET,
            event = "card_error",
            phase = "discover",
            "could not look for an existing card: {e:?}",
        ),
    }

    let mut last_rendered: Option<u64> = None;
    let mut retry_after: Option<Instant> = None;
    loop {
        tokio::time::sleep(Duration::from_millis(500)).await;

        let (lobbies, poll_index, updated_ago) = lobby_snapshot(&state.lock());
        // Render only when a poll completed since the last render. The first
        // poll lands within seconds of startup.
        if poll_index == 0 || last_rendered == Some(poll_index) {
            continue;
        }
        if let Some(until) = retry_after {
            if Instant::now() < until {
                continue;
            }
            retry_after = None;
        }

        let embed = build_mm_embed(&lobbies, poll_index, updated_ago);
        let result = match message_id {
            Some(mid) => channel
                .edit_message(&http, mid, serenity::EditMessage::new().embed(embed.clone()))
                .await,
            None => channel
                .send_message(&http, serenity::CreateMessage::new().embed(embed))
                .await,
        };

        match result {
            Ok(msg) => {
                message_id = Some(msg.id);
                last_rendered = Some(poll_index);
            }
            Err(e) => {
                if http_status(&e) == Some(404) {
                    // Someone deleted the card (or cleared the channel): post a
                    // fresh one on the next tick.
                    message_id = None;
                    last_rendered = None;
                    retry_after = None;
                    tracing::info!(
                        target: crate::EVENT_TARGET,
                        event = "card_recreated",
                        "lobby card message is gone (404) - will repost",
                    );
                } else {
                    tracing::error!(
                        target: crate::EVENT_TARGET,
                        event = "card_error",
                        status = ?http_status(&e),
                        error = ?e,
                        "lobby card update failed - retrying in {} s",
                        CARD_RETRY.as_secs(),
                    );
                    retry_after = Some(Instant::now() + CARD_RETRY);
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::collections::HashMap;

    fn lobby(name: &str, id: u64, owner: u64) -> Lobby {
        Lobby {
            id,
            name: name.into(),
            host: "host".into(),
            protocol: "0.2.9".into(),
            members: 1,
            owner,
        }
    }

    #[test]
    fn join_link_points_at_mm_service() {
        let l = lobby("hi", 109775244078428410, 76561198027378405);
        assert_eq!(
            join_link(&l),
            "https://mm.b.hero.rehab/joinlobby/357190/109775244078428410/76561198027378405"
        );
    }

    #[test]
    fn embed_has_one_field_per_lobby() {
        let lobbies = vec![lobby("a", 1, 2), lobby("b", 3, 4)];
        let e = build_mm_embed(&lobbies, 7, Duration::from_secs(15));
        let json = serde_json::to_value(&e).unwrap();
        assert_eq!(json["title"], "Open Timestone Lobbies — 2");
        assert_eq!(json["fields"].as_array().unwrap().len(), 2);
        assert!(json["fields"][0]["value"].as_str().unwrap().contains("https://mm.b.hero.rehab/joinlobby/"));
    }

    #[test]
    fn embed_shows_empty_state() {
        let e = build_mm_embed(&[], 0, Duration::from_secs(1));
        let json = serde_json::to_value(&e).unwrap();
        assert!(json["description"].as_str().unwrap().contains("No open lobbies"));
        let fields = json.get("fields").and_then(|f| f.as_array()).map(|a| a.len()).unwrap_or(0);
        assert_eq!(fields, 0); // serenity omits the key entirely when empty
    }

    #[test]
    fn embed_caps_at_25_lobbies() {
        let lobbies: Vec<Lobby> = (0..30).map(|i| lobby(&format!("l{i}"), i, i + 1)).collect();
        let e = build_mm_embed(&lobbies, 1, Duration::from_secs(5));
        let json = serde_json::to_value(&e).unwrap();
        let fields = json["fields"].as_array().unwrap();
        assert_eq!(fields.len(), 26); // 25 lobbies + "+5 more"
        assert!(fields[25]["name"].as_str().unwrap().contains("+5 more"));
    }

    #[test]
    fn card_title_marker() {
        assert!(is_lobby_card_title("Open Timestone Lobbies — 3"));
        assert!(is_lobby_card_title("Open Timestone Lobbies — 0"));
        assert!(!is_lobby_card_title("Open Timestone"));
        assert!(!is_lobby_card_title(""));
    }

    #[test]
    fn lobby_snapshot_sorts_busiest_first() {
        let mut prev = HashMap::new();
        for (id, members) in [(3u64, 1usize), (1, 5), (2, 2)] {
            prev.insert(
                id,
                Lobby {
                    id,
                    name: format!("l{id}"),
                    host: "host".into(),
                    protocol: "0.2.9".into(),
                    members,
                    owner: 76561198027378405,
                },
            );
        }
        let st = WatcherState {
            prev,
            absent: Vec::new(),
            poll_index: 12,
            pending: false,
            request_sent: Instant::now(),
            next_poll: Instant::now(),
            last_poll: Instant::now(),
        };
        let (lobbies, poll_index, _updated_ago) = lobby_snapshot(&st);
        assert_eq!(poll_index, 12);
        let members: Vec<usize> = lobbies.iter().map(|l| l.members).collect();
        assert_eq!(members, vec![5, 2, 1]);
    }
}

