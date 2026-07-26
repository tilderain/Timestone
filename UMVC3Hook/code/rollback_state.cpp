#include "rollback.h"

bool g_isResimulating = false;
int32_t g_resimulateCurrentFrame = 0;
int32_t g_postResimFrame = -1;
int32_t g_resimReentranceDepth = 0;

int32_t g_resimRequestedStart = -1;
int32_t g_resimRequestedEnd = -1;
int32_t g_restoreRequestedFrame = -1;

int g_f7State = 0;
int32_t g_f7StartFrame = 0;
int g_f7Remaining = 0;
int g_f7DiagSession = -1;

bool g_ndjsonLoggingEnabled = true;
bool g_ndjsonIncludeDirHistory = false;
