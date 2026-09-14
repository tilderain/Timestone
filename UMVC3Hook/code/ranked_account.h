#pragma once

// Tier 4 "Ranked" online mode: links the local Steam account to the hosted
// Timestone account service (server code lands in a later change). The whole
// v1 link flow (browser page + status/completion API) runs on the login
// service, login.hero-rehab.xyz; the ranked API (mvc3-ranked.hero-rehab.xyz)
// is a separate service with no v1 endpoints. Both origins are overridable
// from Timestone.ini for local testing. No Discord SDK involvement: transport
// is WinHTTP loaded at runtime, and the poll runs on a background thread so
// the game thread never blocks on network I/O.
//
// The link only ever starts from an explicit selection of tier 4 through
// DiscordRichPresence_SetMode(3) -> RankedAccount_SetEnabled(true). A boot-time
// ini value of 3 without a completed link NEVER opens a browser or sends a
// request; [Settings]RankedLinked = 1 (written once, after the service accepts
// the single completion request) suppresses the whole flow on later sessions.
//
// Return contract of SetEnabled: false means declined/unavailable and the
// caller must NOT persist the tier-4 mode (the previous mode stands).

void RankedAccount_Init();              // once, overlay init (dllmain); no-op today
void RankedAccount_Pump();              // every frame from HookProcessStuff
bool RankedAccount_SetEnabled(bool on); // false = declined/unavailable; do not persist
bool RankedAccount_IsLinked();          // ini [Settings]RankedLinked == 1
