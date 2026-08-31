#include "launch_music.h"

#include <Windows.h>
#include <mmsystem.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define STB_VORBIS_NO_PUSHDATA_API
#define STB_VORBIS_MAX_CHANNELS 6
#define STB_VORBIS_HEADER_ONLY
#include "../third_party/stb_vorbis.c"

extern void HookLog(const char* fmt, ...);

namespace {

constexpr char kMusicRelativePath[] =
    "nativePCx64\\sound\\bgm\\source\\bgm_gm_015.sngw";
constexpr int kOutputChannels = 2;
constexpr size_t kBufferCount = 4;
constexpr size_t kBufferFrames = 4096;
constexpr uint32_t kPlaybackChannelVolume = 0x8000; // 50%.
constexpr DWORD kPlaybackVolume =
    kPlaybackChannelVolume | (kPlaybackChannelVolume << 16);
constexpr unsigned kFadeSteps = 20;
constexpr DWORD kFadeStepMs = 100; // Two-second fade.

HANDLE g_musicThread = nullptr;
HANDLE g_stopEvent = nullptr;
SRWLOCK g_waveOutLock = SRWLOCK_INIT;
HWAVEOUT g_waveOut = nullptr;
std::atomic<bool> g_playing{false};
std::atomic<bool> g_muted{false};
bool g_settingsLoaded = false;

bool GetIniPath(char* path, size_t pathSize)
{
    const DWORD length = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(pathSize));
    if (length == 0 || length >= pathSize) return false;

    char* slash = strrchr(path, '\\');
    char* forwardSlash = strrchr(path, '/');
    if (!slash || (forwardSlash && forwardSlash > slash)) slash = forwardSlash;
    char* filename = slash ? slash + 1 : path;
    const size_t remaining = pathSize - static_cast<size_t>(filename - path);
    const int written = snprintf(filename, remaining, "Timestone.ini");
    return written > 0 && static_cast<size_t>(written) < remaining;
}

void LoadMuteSetting()
{
    if (g_settingsLoaded) return;
    g_settingsLoaded = true;

    char path[MAX_PATH] = {};
    if (GetIniPath(path, sizeof(path)))
        g_muted.store(GetPrivateProfileIntA("Audio", "LobbyMusicMuted", 0, path) != 0,
                      std::memory_order_relaxed);
}

bool GetMusicPath(char* path, size_t pathSize)
{
    const DWORD length = GetModuleFileNameA(nullptr, path, static_cast<DWORD>(pathSize));
    if (length == 0 || length >= pathSize) return false;

    char* slash = strrchr(path, '\\');
    char* forwardSlash = strrchr(path, '/');
    if (!slash || (forwardSlash && forwardSlash > slash)) slash = forwardSlash;
    char* relative = slash ? slash + 1 : path;
    const size_t remaining = pathSize - static_cast<size_t>(relative - path);
    const int written = snprintf(relative, remaining, "%s", kMusicRelativePath);
    return written > 0 && static_cast<size_t>(written) < remaining;
}

void ReadLoopPoints(stb_vorbis* decoder, uint32_t& loopStart, uint32_t& loopEnd)
{
    const stb_vorbis_comment comments = stb_vorbis_get_comment(decoder);
    for (int i = 0; i < comments.comment_list_length; ++i) {
        const char* comment = comments.comment_list[i];
        if (strncmp(comment, "LoopStart=", 10) == 0)
            loopStart = static_cast<uint32_t>(strtoul(comment + 10, nullptr, 10));
        else if (strncmp(comment, "LoopEnd=", 8) == 0)
            loopEnd = static_cast<uint32_t>(strtoul(comment + 8, nullptr, 10));
    }
    if (loopEnd <= loopStart) {
        loopStart = 0;
        loopEnd = 0;
    }
}

bool FillBuffer(stb_vorbis* decoder, short* output, uint32_t& samplePosition,
                uint32_t loopStart, uint32_t loopEnd)
{
    size_t written = 0;
    while (written < kBufferFrames) {
        if (WaitForSingleObject(g_stopEvent, 0) == WAIT_OBJECT_0) return false;

        if (loopEnd && samplePosition >= loopEnd) {
            if (!stb_vorbis_seek(decoder, loopStart)) return false;
            samplePosition = loopStart;
        }

        size_t requested = kBufferFrames - written;
        if (loopEnd)
            requested = std::min(requested, static_cast<size_t>(loopEnd - samplePosition));

        const int decoded = stb_vorbis_get_samples_short_interleaved(
            decoder, kOutputChannels, output + written * kOutputChannels,
            static_cast<int>(requested * kOutputChannels));
        if (decoded <= 0) {
            const uint32_t restart = loopEnd ? loopStart : 0;
            if (!stb_vorbis_seek(decoder, restart)) return false;
            samplePosition = restart;
            continue;
        }
        written += static_cast<size_t>(decoded);
        samplePosition += static_cast<uint32_t>(decoded);
    }
    return true;
}

DWORD WINAPI MusicThreadProc(LPVOID)
{
    char path[MAX_PATH] = {};
    if (!GetMusicPath(path, sizeof(path))) {
        HookLog("LaunchMusic: could not construct music path\n");
        return 0;
    }

    int decoderError = 0;
    stb_vorbis* decoder = stb_vorbis_open_filename(path, &decoderError, nullptr);
    if (!decoder) {
        HookLog("LaunchMusic: could not open %s (stb error %d)\n", path, decoderError);
        return 0;
    }

    const stb_vorbis_info info = stb_vorbis_get_info(decoder);
    if (info.sample_rate == 0 || info.channels <= 0) {
        HookLog("LaunchMusic: invalid stream format\n");
        stb_vorbis_close(decoder);
        return 0;
    }

    uint32_t loopStart = 0;
    uint32_t loopEnd = 0;
    ReadLoopPoints(decoder, loopStart, loopEnd);

    WAVEFORMATEX format = {};
    format.wFormatTag = WAVE_FORMAT_PCM;
    format.nChannels = kOutputChannels;
    format.nSamplesPerSec = info.sample_rate;
    format.wBitsPerSample = 16;
    format.nBlockAlign = format.nChannels * format.wBitsPerSample / 8;
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;

    HANDLE bufferEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HWAVEOUT waveOut = nullptr;
    MMRESULT result = bufferEvent
        ? waveOutOpen(&waveOut, WAVE_MAPPER, &format,
                      reinterpret_cast<DWORD_PTR>(bufferEvent), 0, CALLBACK_EVENT)
        : MMSYSERR_NOMEM;
    if (result != MMSYSERR_NOERROR) {
        HookLog("LaunchMusic: waveOutOpen failed (%u)\n", static_cast<unsigned>(result));
        if (bufferEvent) CloseHandle(bufferEvent);
        stb_vorbis_close(decoder);
        return 0;
    }

    AcquireSRWLockExclusive(&g_waveOutLock);
    g_waveOut = waveOut;
    waveOutSetVolume(waveOut,
                     g_muted.load(std::memory_order_relaxed) ? 0 : kPlaybackVolume);
    ReleaseSRWLockExclusive(&g_waveOutLock);

    short samples[kBufferCount][kBufferFrames * kOutputChannels] = {};
    WAVEHDR headers[kBufferCount] = {};
    uint32_t samplePosition = 0;
    size_t preparedCount = 0;
    bool playing = true;

    for (size_t i = 0; i < kBufferCount && playing; ++i) {
        playing = FillBuffer(decoder, samples[i], samplePosition, loopStart, loopEnd);
        if (!playing) break;
        headers[i].lpData = reinterpret_cast<LPSTR>(samples[i]);
        headers[i].dwBufferLength = sizeof(samples[i]);
        result = waveOutPrepareHeader(waveOut, &headers[i], sizeof(headers[i]));
        if (result != MMSYSERR_NOERROR) {
            playing = false;
            break;
        }
        ++preparedCount;
        result = waveOutWrite(waveOut, &headers[i], sizeof(headers[i]));
        if (result != MMSYSERR_NOERROR) {
            playing = false;
            break;
        }
    }

    if (playing) {
        g_playing.store(true, std::memory_order_release);
        HookLog("LaunchMusic: playing %u Hz/%d channel source loop=%u-%u\n",
                info.sample_rate, info.channels, loopStart, loopEnd);
    }

    HANDLE waitHandles[] = {g_stopEvent, bufferEvent};
    while (playing) {
        const DWORD wait = WaitForMultipleObjects(2, waitHandles, FALSE, INFINITE);
        if (wait != WAIT_OBJECT_0 + 1) break;

        for (size_t i = 0; i < preparedCount; ++i) {
            if ((headers[i].dwFlags & WHDR_DONE) == 0) continue;
            if (!FillBuffer(decoder, samples[i], samplePosition, loopStart, loopEnd)) {
                playing = false;
                break;
            }
            result = waveOutWrite(waveOut, &headers[i], sizeof(headers[i]));
            if (result != MMSYSERR_NOERROR) {
                playing = false;
                break;
            }
        }
    }

    g_playing.store(false, std::memory_order_release);
    AcquireSRWLockExclusive(&g_waveOutLock);
    g_waveOut = nullptr;
    ReleaseSRWLockExclusive(&g_waveOutLock);
    waveOutReset(waveOut);
    for (size_t i = 0; i < preparedCount; ++i)
        waveOutUnprepareHeader(waveOut, &headers[i], sizeof(headers[i]));
    waveOutClose(waveOut);
    CloseHandle(bufferEvent);
    stb_vorbis_close(decoder);
    HookLog("LaunchMusic: stopped\n");
    return 0;
}

} // namespace

void LaunchMusic_Start()
{
    if (g_musicThread) return;
    LoadMuteSetting();

    g_stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent) {
        HookLog("LaunchMusic: could not create stop event (%lu)\n", GetLastError());
        return;
    }
    g_musicThread = CreateThread(nullptr, 0, MusicThreadProc, nullptr, 0, nullptr);
    if (!g_musicThread) {
        HookLog("LaunchMusic: could not create playback thread (%lu)\n", GetLastError());
        CloseHandle(g_stopEvent);
        g_stopEvent = nullptr;
    }
}

void LaunchMusic_Stop()
{
    if (!g_musicThread) return;

    if (g_playing.load(std::memory_order_acquire) &&
        !g_muted.load(std::memory_order_relaxed)) {
        for (unsigned step = kFadeSteps; step > 0; --step) {
            if (!g_playing.load(std::memory_order_acquire)) break;
            const uint32_t channelVolume =
                kPlaybackChannelVolume * (step - 1) / kFadeSteps;
            const DWORD volume = channelVolume | (channelVolume << 16);
            AcquireSRWLockShared(&g_waveOutLock);
            if (g_waveOut)
                waveOutSetVolume(g_waveOut, volume);
            ReleaseSRWLockShared(&g_waveOutLock);
            Sleep(kFadeStepMs);
        }
    }

    SetEvent(g_stopEvent);
    WaitForSingleObject(g_musicThread, INFINITE);
    CloseHandle(g_musicThread);
    CloseHandle(g_stopEvent);
    g_musicThread = nullptr;
    g_stopEvent = nullptr;
}

bool LaunchMusic_IsPlaying()
{
    return g_playing.load(std::memory_order_acquire);
}

bool LaunchMusic_IsMuted()
{
    LoadMuteSetting();
    return g_muted.load(std::memory_order_relaxed);
}

void LaunchMusic_SetMuted(bool muted)
{
    LoadMuteSetting();
    g_muted.store(muted, std::memory_order_relaxed);

    AcquireSRWLockShared(&g_waveOutLock);
    if (g_waveOut)
        waveOutSetVolume(g_waveOut, muted ? 0 : kPlaybackVolume);
    ReleaseSRWLockShared(&g_waveOutLock);

    char path[MAX_PATH] = {};
    if (GetIniPath(path, sizeof(path)) &&
        !WritePrivateProfileStringA("Audio", "LobbyMusicMuted", muted ? "1" : "0", path)) {
        HookLog("LaunchMusic: could not save mute setting to %s\n", path);
    }
}
