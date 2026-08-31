#include "launch_music.h"

#include <Windows.h>
#include <mmsystem.h>
#include <mmreg.h>
#include <ksmedia.h>
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
constexpr int kMaxSourceChannels = 6;
constexpr size_t kBufferCount = 4;
constexpr size_t kBufferFrames = 4096;
constexpr uint32_t kFadeDurationSeconds = 2;
constexpr DWORD kQueuedAudioDrainMs = 400;
constexpr float kPlaybackGain = 0.5f;

HANDLE g_musicThread = nullptr;
HANDLE g_stopEvent = nullptr;
HANDLE g_fadeCompleteEvent = nullptr;
SRWLOCK g_waveOutLock = SRWLOCK_INIT;
HWAVEOUT g_waveOut = nullptr;
std::atomic<bool> g_playing{false};
std::atomic<bool> g_muted{false};
std::atomic<bool> g_fadeRequested{false};
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

void ConvertToNativePcm(const float* source, int sourceChannels, int frameCount,
                        short* output)
{
    // Vorbis 5.1 order differs from the Windows 5.1 channel-mask order.
    static constexpr int vorbisToWindows51[] = {0, 2, 1, 5, 3, 4};
    for (int frame = 0; frame < frameCount; ++frame) {
        const float* input = source + frame * sourceChannels;
        for (int channel = 0; channel < sourceChannels; ++channel) {
            const int sourceChannel = sourceChannels == 6
                ? vorbisToWindows51[channel] : channel;
            const float sample = std::clamp(
                input[sourceChannel] * kPlaybackGain, -1.0f, 1.0f);
            output[frame * sourceChannels + channel] =
                static_cast<short>(sample * 32767.0f);
        }
    }
}

bool FillBuffer(stb_vorbis* decoder, short* output, float* sourceBuffer,
                int sourceChannels, uint32_t& samplePosition,
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

        const int decoded = stb_vorbis_get_samples_float_interleaved(
            decoder, sourceChannels, sourceBuffer,
            static_cast<int>(requested * sourceChannels));
        if (decoded <= 0) {
            const uint32_t restart = loopEnd ? loopStart : 0;
            if (!stb_vorbis_seek(decoder, restart)) return false;
            samplePosition = restart;
            continue;
        }
        ConvertToNativePcm(sourceBuffer, sourceChannels, decoded,
                           output + written * sourceChannels);
        written += static_cast<size_t>(decoded);
        samplePosition += static_cast<uint32_t>(decoded);
    }
    return true;
}

void ApplyOutputGain(short* samples, size_t frameCount, int channelCount, bool& fading,
                     uint64_t& fadeFramesRemaining, uint64_t fadeFrameCount,
                     bool& fadeCompleteSignaled)
{
    if (!fading && g_fadeRequested.load(std::memory_order_acquire)) {
        fading = true;
        fadeFramesRemaining = fadeFrameCount;
    }

    for (size_t frame = 0; frame < frameCount; ++frame) {
        const int64_t fadeGain = static_cast<int64_t>(
            fading ? fadeFramesRemaining : fadeFrameCount);
        const int64_t fadeDivisor = static_cast<int64_t>(fadeFrameCount);
        for (int channel = 0; channel < channelCount; ++channel) {
            const size_t index = frame * channelCount + channel;
            samples[index] = static_cast<short>(
                static_cast<int64_t>(samples[index]) * fadeGain /
                fadeDivisor);
        }
        if (fading && fadeFramesRemaining > 0)
            --fadeFramesRemaining;
    }

    if (fading && fadeFramesRemaining == 0 && !fadeCompleteSignaled) {
        fadeCompleteSignaled = true;
        SetEvent(g_fadeCompleteEvent);
    }
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
    if (info.sample_rate == 0 || info.channels <= 0 ||
        info.channels > kMaxSourceChannels) {
        HookLog("LaunchMusic: invalid stream format\n");
        stb_vorbis_close(decoder);
        return 0;
    }

    uint32_t loopStart = 0;
    uint32_t loopEnd = 0;
    ReadLoopPoints(decoder, loopStart, loopEnd);

    WAVEFORMATEXTENSIBLE format = {};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = info.channels;
    format.Format.nSamplesPerSec = info.sample_rate;
    format.Format.wBitsPerSample = 16;
    format.Format.nBlockAlign = format.Format.nChannels * format.Format.wBitsPerSample / 8;
    format.Format.nAvgBytesPerSec = format.Format.nSamplesPerSec * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 16;
    format.dwChannelMask = info.channels == 6 ? KSAUDIO_SPEAKER_5POINT1 : 0;
    format.SubFormat = {WAVE_FORMAT_PCM, 0x0000, 0x0010,
                        {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

    HANDLE bufferEvent = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HWAVEOUT waveOut = nullptr;
    MMRESULT result = bufferEvent
        ? waveOutOpen(&waveOut, WAVE_MAPPER, &format.Format,
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
    if (g_muted.load(std::memory_order_relaxed))
        waveOutPause(waveOut);
    ReleaseSRWLockExclusive(&g_waveOutLock);

    short samples[kBufferCount][kBufferFrames * kMaxSourceChannels] = {};
    float sourceSamples[kBufferFrames * kMaxSourceChannels] = {};
    WAVEHDR headers[kBufferCount] = {};
    uint32_t samplePosition = 0;
    size_t preparedCount = 0;
    bool playing = true;
    bool fading = false;
    bool fadeCompleteSignaled = false;
    uint64_t fadeFramesRemaining = 0;
    const uint64_t fadeFrameCount =
        static_cast<uint64_t>(info.sample_rate) * kFadeDurationSeconds;

    for (size_t i = 0; i < kBufferCount && playing; ++i) {
        playing = FillBuffer(decoder, samples[i], sourceSamples, info.channels,
                             samplePosition, loopStart, loopEnd);
        if (!playing) break;
        ApplyOutputGain(samples[i], kBufferFrames, info.channels, fading,
                        fadeFramesRemaining,
                        fadeFrameCount, fadeCompleteSignaled);
        headers[i].lpData = reinterpret_cast<LPSTR>(samples[i]);
        headers[i].dwBufferLength = static_cast<DWORD>(
            kBufferFrames * info.channels * sizeof(short));
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
            if (!FillBuffer(decoder, samples[i], sourceSamples, info.channels,
                            samplePosition, loopStart, loopEnd)) {
                playing = false;
                break;
            }
            ApplyOutputGain(samples[i], kBufferFrames, info.channels, fading,
                            fadeFramesRemaining,
                            fadeFrameCount, fadeCompleteSignaled);
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
    g_fadeRequested.store(false, std::memory_order_relaxed);

    g_stopEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    g_fadeCompleteEvent = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!g_stopEvent || !g_fadeCompleteEvent) {
        HookLog("LaunchMusic: could not create stop event (%lu)\n", GetLastError());
        if (g_stopEvent) CloseHandle(g_stopEvent);
        if (g_fadeCompleteEvent) CloseHandle(g_fadeCompleteEvent);
        g_stopEvent = nullptr;
        g_fadeCompleteEvent = nullptr;
        return;
    }
    g_musicThread = CreateThread(nullptr, 0, MusicThreadProc, nullptr, 0, nullptr);
    if (!g_musicThread) {
        HookLog("LaunchMusic: could not create playback thread (%lu)\n", GetLastError());
        CloseHandle(g_stopEvent);
        CloseHandle(g_fadeCompleteEvent);
        g_stopEvent = nullptr;
        g_fadeCompleteEvent = nullptr;
    }
}

void LaunchMusic_Stop()
{
    if (!g_musicThread) return;

    if (g_playing.load(std::memory_order_acquire) &&
        !g_muted.load(std::memory_order_relaxed)) {
        ResetEvent(g_fadeCompleteEvent);
        g_fadeRequested.store(true, std::memory_order_release);
        HANDLE fadeWaitHandles[] = {g_fadeCompleteEvent, g_musicThread};
        if (WaitForMultipleObjects(2, fadeWaitHandles, FALSE, 4000) == WAIT_OBJECT_0)
            Sleep(kQueuedAudioDrainMs);
    }

    SetEvent(g_stopEvent);
    WaitForSingleObject(g_musicThread, INFINITE);
    CloseHandle(g_musicThread);
    CloseHandle(g_stopEvent);
    CloseHandle(g_fadeCompleteEvent);
    g_musicThread = nullptr;
    g_stopEvent = nullptr;
    g_fadeCompleteEvent = nullptr;
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
    if (g_waveOut) {
        if (muted)
            waveOutPause(g_waveOut);
        else
            waveOutRestart(g_waveOut);
    }
    ReleaseSRWLockShared(&g_waveOutLock);

    char path[MAX_PATH] = {};
    if (GetIniPath(path, sizeof(path)) &&
        !WritePrivateProfileStringA("Audio", "LobbyMusicMuted", muted ? "1" : "0", path)) {
        HookLog("LaunchMusic: could not save mute setting to %s\n", path);
    }
}
