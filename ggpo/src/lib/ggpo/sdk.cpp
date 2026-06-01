/* -----------------------------------------------------------------------
 * GGPO.net (http://ggpo.net)  -  Copyright 2009 GroundStorm Studios, LLC.
 *
 * Use of this software is governed by the MIT license that can be found
 * in the LICENSE file.
 *
 * This file replaces main.cpp's DllMain with clean API entry points
 * for static linking into UMVC3Hook (which has its own DllMain).
 * ----------------------------------------------------------------------- */

#include "types.h"
#include "backends/p2p.h"
#include "backends/synctest.h"
#include "backends/spectator.h"
#include "ggponet.h"

/*
 * ggpo_log / ggpo_logv
 */
void
ggpo_log(GGPOSession *ggpo, const char *fmt, ...)
{
   va_list args;
   va_start(args, fmt);
   ggpo_logv(ggpo, fmt, args);
   va_end(args);
}

void
ggpo_logv(GGPOSession *ggpo, const char *fmt, va_list args)
{
   if (ggpo) {
      ggpo->Logv(fmt, args);
   }
}

/*
 * ggpo_start_session
 */
GGPOErrorCode
ggpo_start_session(GGPOSession **session,
                   GGPOSessionCallbacks *cb,
                   const char *game,
                   int num_players,
                   int input_size,
                   unsigned short localport)
{
   *session = (GGPOSession *)new Peer2PeerBackend(cb,
                                                  game,
                                                  localport,
                                                  num_players,
                                                  input_size);
   return GGPO_OK;
}

/*
 * ggpo_add_player
 */
GGPOErrorCode
ggpo_add_player(GGPOSession *ggpo,
                GGPOPlayer *player,
                GGPOPlayerHandle *handle)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   return ggpo->AddPlayer(player, handle);
}

/*
 * ggpo_start_synctest
 */
GGPOErrorCode
ggpo_start_synctest(GGPOSession **ggpo,
                    GGPOSessionCallbacks *cb,
                    char *game,
                    int num_players,
                    int input_size,
                    int frames)
{
   *ggpo = (GGPOSession *)new SyncTestBackend(cb, game, frames, num_players);
   return GGPO_OK;
}

/*
 * ggpo_set_frame_delay
 */
GGPOErrorCode
ggpo_set_frame_delay(GGPOSession *ggpo,
                     GGPOPlayerHandle player,
                     int frame_delay)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   return ggpo->SetFrameDelay(player, frame_delay);
}

/*
 * ggpo_idle
 */
GGPOErrorCode
ggpo_idle(GGPOSession *ggpo, int timeout)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   return ggpo->DoPoll(timeout);
}

/*
 * ggpo_add_local_input
 */
GGPOErrorCode
ggpo_add_local_input(GGPOSession *ggpo,
                     GGPOPlayerHandle player,
                     void *values,
                     int size)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   return ggpo->AddLocalInput(player, values, size);
}

/*
 * ggpo_synchronize_input
 */
GGPOErrorCode
ggpo_synchronize_input(GGPOSession *ggpo,
                       void *values,
                       int size,
                       int *disconnect_flags)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   return ggpo->SyncInput(values, size, disconnect_flags);
}

/*
 * ggpo_disconnect_player
 */
GGPOErrorCode
ggpo_disconnect_player(GGPOSession *ggpo,
                       GGPOPlayerHandle player)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   return ggpo->DisconnectPlayer(player);
}

/*
 * ggpo_advance_frame
 */
GGPOErrorCode
ggpo_advance_frame(GGPOSession *ggpo)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   return ggpo->IncrementFrame();
}

/*
 * ggpo_client_chat
 */
GGPOErrorCode
ggpo_client_chat(GGPOSession *ggpo, char *text)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   return ggpo->Chat(text);
}

/*
 * ggpo_get_network_stats
 */
GGPOErrorCode
ggpo_get_network_stats(GGPOSession *ggpo,
                       GGPOPlayerHandle player,
                       GGPONetworkStats *stats)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   return ggpo->GetNetworkStats(stats, player);
}

/*
 * ggpo_close_session
 */
GGPOErrorCode
ggpo_close_session(GGPOSession *ggpo)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   delete ggpo;
   return GGPO_OK;
}

/*
 * ggpo_set_disconnect_timeout
 */
GGPOErrorCode
ggpo_set_disconnect_timeout(GGPOSession *ggpo, int timeout)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   return ggpo->SetDisconnectTimeout(timeout);
}

/*
 * ggpo_set_disconnect_notify_start
 */
GGPOErrorCode
ggpo_set_disconnect_notify_start(GGPOSession *ggpo, int timeout)
{
   if (!ggpo) {
      return GGPO_ERRORCODE_INVALID_SESSION;
   }
   return ggpo->SetDisconnectNotifyStart(timeout);
}

/*
 * ggpo_start_spectating
 */
GGPOErrorCode
ggpo_start_spectating(GGPOSession **session,
                      GGPOSessionCallbacks *cb,
                      const char *game,
                      int num_players,
                      int input_size,
                      unsigned short local_port,
                      char *host_ip,
                      unsigned short host_port)
{
   *session = (GGPOSession *)new SpectatorBackend(cb,
                                                  game,
                                                  local_port,
                                                  num_players,
                                                  input_size,
                                                  host_ip,
                                                  host_port);
   return GGPO_OK;
}
