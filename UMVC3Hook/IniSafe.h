#pragma once

#include <cstddef>
#include <string>

struct IniSafe_Entry {
    std::string section;
    std::string key;
    std::string value;
};

// One read, backup, and atomic replacement for the entire batch.
bool IniSafe_WriteBatch(const IniSafe_Entry* entries, size_t count, const char* filename);

// Set a key without truncating the live file. Reads remain tolerant and are
// retried normally; an actual I/O failure never becomes an empty input file.
bool IniSafe_WriteString(const char* section, const char* key,
                         const char* value, const char* filename);

// Add a user-facing `; <comment>` line directly above `[section] key`. If the
// key does not exist and defaultValue is non-null, the section and
// `key=defaultValue` are created beneath the comment. Idempotent: an identical
// marker line already present leaves the file untouched. Intended for
// one-time boot legends; IniSafe's writer preserves comment lines on update.
bool IniSafe_EnsureLineComment(const char* section, const char* key,
                               const char* comment, const char* defaultValue,
                               const char* filename);
