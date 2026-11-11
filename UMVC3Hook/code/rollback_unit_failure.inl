// Independent fatal output: no recorder lock, object probing or game callbacks.
struct RollbackUnitFailureContext {
    const char* phase = "none";
    const void* object = nullptr;
    uint64_t id = 0;
    int32_t targetFrame = -1;
};
thread_local RollbackUnitFailureContext g_failureContext;
struct RollbackUnitFailureScope {
    RollbackUnitFailureContext previous = g_failureContext;
    RollbackUnitFailureScope(const char* phase, const void* object = nullptr, uint64_t id = 0,
                         int32_t frame = INT32_MIN) {
        g_failureContext = {phase, object, id, frame == INT32_MIN ? previous.targetFrame : frame};
    }
    ~RollbackUnitFailureScope() { g_failureContext = previous; }
};

void WriteRollbackUnitFailureRecord(const char* text)
{
#if defined(_WIN32)
    char path[MAX_PATH]{};
    const int length = Rollback_ResolveDllPath(path, sizeof(path), "lifetime_failure.log");
    if (length < 0 || size_t(length) >= sizeof(path)) std::strcpy(path, "lifetime_failure.log");
    HANDLE file = CreateFileA(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        const size_t length = std::strlen(text);
        size_t offset = 0;
        while (offset < length) {
            DWORD written = 0;
            if (!WriteFile(file, text + offset, DWORD(length - offset), &written, nullptr) || !written) break;
            offset += written;
        }
        FlushFileBuffers(file);
        CloseHandle(file);
    }
    OutputDebugStringA(text);
#else
    std::fputs(text, stderr);
    std::fflush(stderr);
#endif
    // Persist the independent artifact first. HookLog can already own its lock.
    HookLogTryWriteFatal(text);
}

template<class T> uint64_t RollbackUnitFailureValue(T value)
{
    if constexpr (std::is_pointer_v<T>) return uint64_t(reinterpret_cast<uintptr_t>(value));
    else return uint64_t(value);
}

[[noreturn]] void RollbackUnitFailure(const char* file, int line, const char* function, const char* reason,
    std::initializer_list<std::pair<const char*, uint64_t>> details)
{
    char text[8192]{};
    const uint64_t context = g_context.load(std::memory_order_relaxed);
    const int size = std::snprintf(text, sizeof(text),
        "\n=== LIFETIME INVARIANT FAILURE ===\nsource=%s:%d\nfunction=%s\nreason=%s\n"
        "pid=%lu thread=%lu time_ms=%llu frame=%d resim=%u target_frame=%d phase=%s\n"
        "object=%p identity=%llu native_caller=0x%llX lifetime_enabled=%u recording=%u\n",
        file, line, function, reason, static_cast<unsigned long>(GetCurrentProcessId()),
        static_cast<unsigned long>(GetCurrentThreadId()), static_cast<unsigned long long>(GetTickCount64()),
        int32_t(context >> 1), unsigned(context & 1), g_failureContext.targetFrame, g_failureContext.phase,
        g_failureContext.object, static_cast<unsigned long long>(g_failureContext.id),
        static_cast<unsigned long long>(g_caller), unsigned(g_rollbackUnitEnabled.load(std::memory_order_relaxed)),
        unsigned(g_recording.load(std::memory_order_relaxed)));
    size_t used = size > 0 ? std::min(size_t(size), sizeof(text) - 1) : 0;
    for (const auto& [name, value] : details) {
        const int size = std::snprintf(text + used, sizeof(text) - used, "%s=0x%llX\n", name,
            static_cast<unsigned long long>(value));
        if (size > 0) used += std::min(size_t(size), sizeof(text) - used - 1);
    }
    WriteRollbackUnitFailureRecord(text);
    std::abort(); // SIGABRT also produces the full crash report, after this record is flushed.
}

#if defined(_MSC_VER)
#define LIFE_FUNCTION __FUNCSIG__
#else
#define LIFE_FUNCTION __PRETTY_FUNCTION__
#endif
#define LIFE_VALUE(value) std::pair<const char*, uint64_t>{#value, RollbackUnitFailureValue(value)}
#define LIFE_FAIL(reason, ...) RollbackUnitFailure(__FILE__, __LINE__, LIFE_FUNCTION, reason, {__VA_ARGS__})
// Invariant guard. The condition is evaluated once and stringified once as the
// failure reason; use LIFE_FAIL when a specific message or value dump adds detail.
// Variadic so that template-argument and lambda commas need no extra parentheses.
#define LIFE_REQUIRE(...) do { \
        if (!(__VA_ARGS__)) RollbackUnitFailure(__FILE__, __LINE__, LIFE_FUNCTION, \
            "requirement failed: " #__VA_ARGS__, {}); \
    } while (false)
