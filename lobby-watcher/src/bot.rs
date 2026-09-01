// lobby-watcher/src/bot.rs
// Discord integration for the lobby watcher. Runs inside the watcher process
// (shares its live lobby state) and exposes slash commands:
//   /mm  — post a complex embed listing every open Timestone lobby with a
//          steam://joinlobby link for each.
// The bot is optional: if DISCORD_TOKEN is unset the watcher runs without it.
use std::sync::Arc;
use std::time::Duration;

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
                Ok(mut client) => client.start().await,
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
    let (lobbies, poll_index, updated_ago) = {
        let st = ctx.data().state.lock();
        let mut lobbies: Vec<Lobby> = st.prev.values().cloned().collect();
        let poll_index = st.poll_index;
        let updated_ago = st.last_poll.elapsed();
        lobbies.sort_by(|a, b| b.members.cmp(&a.members));
        (lobbies, poll_index, updated_ago)
    };
    let embed = build_mm_embed(&lobbies, poll_index, updated_ago);
    ctx.send(poise::CreateReply::default().embed(embed)).await?;
    Ok(())
}

/// steam://joinlobby/<appid>/<lobby_id>/<owner_steamid> — opens/joins the lobby
/// through the Steam client (launching UMVC3 if needed).
pub fn join_link(l: &Lobby) -> String {
    format!("steam://joinlobby/{APP_ID}/{}/{}", l.id, l.owner)
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

#[cfg(test)]
mod tests {
    use super::*;

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
    fn join_link_uses_steam_protocol_with_owner() {
        let l = lobby("hi", 109775244078428410, 76561198027378405);
        assert_eq!(
            join_link(&l),
            "steam://joinlobby/357190/109775244078428410/76561198027378405"
        );
    }

    #[test]
    fn embed_has_one_field_per_lobby() {
        let lobbies = vec![lobby("a", 1, 2), lobby("b", 3, 4)];
        let e = build_mm_embed(&lobbies, 7, Duration::from_secs(15));
        let json = serde_json::to_value(&e).unwrap();
        assert_eq!(json["title"], "Open Timestone Lobbies — 2");
        assert_eq!(json["fields"].as_array().unwrap().len(), 2);
        assert!(json["fields"][0]["value"].as_str().unwrap().contains("steam://joinlobby/"));
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
}
