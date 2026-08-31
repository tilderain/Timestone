#pragma once

// Starts the launch-menu connection music if it is not already playing.
void LaunchMusic_Start();

// Stops playback and waits for the decoder thread to release WinMM resources.
void LaunchMusic_Stop();

bool LaunchMusic_IsPlaying();
bool LaunchMusic_IsMuted();
void LaunchMusic_SetMuted(bool muted);
