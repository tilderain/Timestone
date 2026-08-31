#include "savedata.h"

#include "mk10utils.h"
#include "steam_api_compat.h"
#include <Windows.h>
#include <wincrypt.h>
#include <algorithm>
#include <cstring>
#include <vector>

extern void HookLog(const char* fmt, ...);

namespace {

constexpr size_t kGameConfigPayloadSize = 0x2550;
constexpr size_t kProfileOffset = 0x20;
constexpr size_t kCharsOffset = 0xb8;
constexpr size_t kAssistsOffset = 0xdc;
constexpr size_t kColorsOffset = 0x100;

struct BlowfishState {
    uint32_t p[18];
    uint32_t s[1024];
};

uint32_t BlowfishF(const BlowfishState& state, uint32_t value)
{
    return ((state.s[value >> 24] + state.s[0x100 + ((value >> 16) & 0xff)]) ^
            state.s[0x200 + ((value >> 8) & 0xff)]) +
           state.s[0x300 + (value & 0xff)];
}

void BlowfishEncrypt(const BlowfishState& state, uint32_t& left, uint32_t& right)
{
    uint32_t a = left ^ state.p[0];
    uint32_t b = right;
    for (int round = 1; round <= 16; ++round) {
        if (round & 1) b ^= BlowfishF(state, a) ^ state.p[round];
        else a ^= BlowfishF(state, b) ^ state.p[round];
    }
    left = b ^ state.p[17];
    right = a;
}

void BlowfishDecrypt(const BlowfishState& state, uint32_t& left, uint32_t& right)
{
    uint32_t a = left ^ state.p[17];
    uint32_t b = right;
    for (int round = 16; round >= 1; --round) {
        if ((round & 1) == 0) b ^= BlowfishF(state, a) ^ state.p[round];
        else a ^= BlowfishF(state, b) ^ state.p[round];
    }
    left = b ^ state.p[0];
    right = a;
}

void InitializeCipher(BlowfishState& state)
{
    memcpy(state.p, reinterpret_cast<const void*>(_addr(0x140d2c050)),
           sizeof(state.p));
    memcpy(state.s, reinterpret_cast<const void*>(_addr(0x140d2c0a0)),
           sizeof(state.s));

    constexpr uint8_t key[] = {'u', 'm', 'v', 'c', '3'};
    size_t keyIndex = 0;
    for (uint32_t& value : state.p) {
        uint32_t keyWord = 0;
        for (int byte = 0; byte < 4; ++byte) {
            keyWord = (keyWord << 8) | key[keyIndex];
            keyIndex = (keyIndex + 1) % sizeof(key);
        }
        value ^= keyWord;
    }

    uint32_t left = 0;
    uint32_t right = 0;
    for (size_t i = 0; i < 18; i += 2) {
        BlowfishEncrypt(state, left, right);
        state.p[i] = left;
        state.p[i + 1] = right;
    }
    for (size_t i = 0; i < 1024; i += 2) {
        BlowfishEncrypt(state, left, right);
        state.s[i] = left;
        state.s[i + 1] = right;
    }
}

uint32_t ReadU32(const uint8_t* data)
{
    uint32_t value = 0;
    memcpy(&value, data, sizeof(value));
    return value;
}

uint64_t ReadU64(const uint8_t* data)
{
    uint64_t value = 0;
    memcpy(&value, data, sizeof(value));
    return value;
}

bool VerifySha1(const uint8_t* data, size_t size)
{
    if (!data || size < 0x40) return false;
    const uint32_t hashedBytes = ReadU32(data + 0x20);
    if (hashedBytes > size - 0x40) return false;

    HCRYPTPROV provider = 0;
    HCRYPTHASH hash = 0;
    BYTE digest[20] = {};
    DWORD digestSize = sizeof(digest);
    const bool valid = CryptAcquireContextA(
            &provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT) &&
        CryptCreateHash(provider, CALG_SHA1, 0, 0, &hash) &&
        CryptHashData(hash, data + 0x40, hashedBytes, 0) &&
        CryptGetHashParam(hash, HP_HASHVAL, digest, &digestSize, 0) &&
        digestSize == sizeof(digest);
    if (valid) {
        for (size_t i = 0; i < sizeof(digest); i += 4) {
            std::swap(digest[i], digest[i + 3]);
            std::swap(digest[i + 1], digest[i + 2]);
        }
    }
    if (hash) CryptDestroyHash(hash);
    if (provider) CryptReleaseContext(provider, 0);
    return valid && memcmp(digest, data + 0x0c, sizeof(digest)) == 0;
}

const uint8_t* FindGameConfigPayload(const uint8_t* data, size_t size)
{
    if (size < 0x40 || ReadU32(data) != 1 || !VerifySha1(data, size))
        return nullptr;

    const uint32_t tableBytes = ReadU32(data + 8);
    const uint32_t hashedBytes = ReadU32(data + 0x20);
    const size_t contentEnd = 0x40 + static_cast<size_t>(hashedBytes);
    if ((tableBytes & 7) != 0 || tableBytes > hashedBytes || contentEnd > size)
        return nullptr;

    size_t cursor = 0x40 + tableBytes;
    const uint32_t recordCount = tableBytes / 8;
    for (uint32_t i = 0; i < recordCount; ++i) {
        if (cursor > contentEnd || contentEnd - cursor < 0x10) return nullptr;
        const uint32_t type = ReadU32(data + cursor + 4);
        const uint64_t value = ReadU64(data + cursor + 8);
        cursor += 0x10;
        if (type != 0x0c) continue;
        if (value > contentEnd - cursor) return nullptr;
        if (value == kGameConfigPayloadSize) return data + cursor;
        cursor += static_cast<size_t>(value);
    }
    return nullptr;
}

bool ReadSteamSave(std::vector<uint8_t>& data)
{
    HMODULE module = GetModuleHandleA("steam_api64.dll");
    if (!module) return false;

    using GetClientFn = void* (*)();
    using GetUserFn = HSteamUser (*)();
    using GetPipeFn = HSteamPipe (*)();
    using GetRemoteStorageFn = void* (*)(void*, HSteamUser, HSteamPipe, const char*);
    using GetFileSizeFn = int32_t (*)(void*, const char*);
    using FileReadFn = int32_t (*)(void*, const char*, void*, int32_t);

    auto getClient = reinterpret_cast<GetClientFn>(GetProcAddress(module, "SteamClient"));
    auto getUser = reinterpret_cast<GetUserFn>(GetProcAddress(module, "SteamAPI_GetHSteamUser"));
    auto getPipe = reinterpret_cast<GetPipeFn>(GetProcAddress(module, "SteamAPI_GetHSteamPipe"));
    auto getRemoteStorage = reinterpret_cast<GetRemoteStorageFn>(GetProcAddress(
        module, "SteamAPI_ISteamClient_GetISteamRemoteStorage"));
    auto getFileSize = reinterpret_cast<GetFileSizeFn>(GetProcAddress(
        module, "SteamAPI_ISteamRemoteStorage_GetFileSize"));
    auto fileRead = reinterpret_cast<FileReadFn>(GetProcAddress(
        module, "SteamAPI_ISteamRemoteStorage_FileRead"));
    if (!getClient || !getUser || !getPipe || !getRemoteStorage ||
        !getFileSize || !fileRead)
        return false;

    void* storage = getRemoteStorage(getClient(), getUser(), getPipe(),
        "STEAMREMOTESTORAGE_INTERFACE_VERSION013");
    if (!storage) return false;
    const int32_t size = getFileSize(storage, "savedata.bin");
    if (size <= 0 || size > 1024 * 1024 || (size & 7) != 0) return false;

    data.resize(static_cast<size_t>(size));
    return fileRead(storage, "savedata.bin", data.data(), size) == size;
}

} // namespace

size_t Savedata_LoadRosters(SavedRoster* rosters, size_t capacity)
{
    if (!rosters || capacity == 0) return 0;
    std::vector<uint8_t> data;
    if (!ReadSteamSave(data)) {
        HookLog("Savedata: retail savedata.bin is unavailable\n");
        return 0;
    }

    BlowfishState cipher;
    InitializeCipher(cipher);
    for (size_t offset = 0; offset < data.size(); offset += 8) {
        uint32_t left = ReadU32(data.data() + offset);
        uint32_t right = ReadU32(data.data() + offset + 4);
        BlowfishDecrypt(cipher, left, right);
        memcpy(data.data() + offset, &left, sizeof(left));
        memcpy(data.data() + offset + 4, &right, sizeof(right));
    }

    const uint8_t* payload = FindGameConfigPayload(data.data(), data.size());
    if (!payload) {
        HookLog("Savedata: savedata.bin failed validation\n");
        return 0;
    }

    const uint8_t* profile = payload + kProfileOffset;
    size_t count = 0;
    for (size_t preset = 0; preset < 3 && count < capacity; ++preset) {
        SavedRoster roster = {};
        bool valid = true;
        for (size_t slot = 0; slot < 3; ++slot) {
            const size_t index = preset * 3 + slot;
            roster.chars[slot] = static_cast<int32_t>(ReadU32(
                profile + kCharsOffset + index * 4));
            roster.assists[slot] = static_cast<int32_t>(ReadU32(
                profile + kAssistsOffset + index * 4));
            roster.colors[slot] = static_cast<int32_t>(ReadU32(
                profile + kColorsOffset + index * 4));
            valid &= roster.chars[slot] >= 1 && roster.chars[slot] <= 51;
            valid &= roster.assists[slot] >= 0 && roster.assists[slot] <= 2;
            valid &= roster.colors[slot] >= 0 && roster.colors[slot] <= 8;
        }
        if (valid) rosters[count++] = roster;
    }
    HookLog("Savedata: loaded %zu retail preset roster(s)\n", count);
    return count;
}
