#pragma once

// Starts the launch-menu connection music if it is not already playing.
void LaunchMusic_Start();

// Requests a fade and returns immediately. The playback thread stops itself.
void LaunchMusic_BeginFadeOut();

// Stops playback and waits for the decoder thread to release WinMM resources.
void LaunchMusic_Stop();

bool LaunchMusic_IsPlaying();
bool LaunchMusic_IsMuted();
void LaunchMusic_SetMuted(bool muted);
