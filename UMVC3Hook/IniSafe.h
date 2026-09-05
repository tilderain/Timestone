#pragma once

// Set a key without truncating the live file. Reads remain tolerant and are
// retried normally; an actual I/O failure never becomes an empty input file.
bool IniSafe_WriteString(const char* section, const char* key,
                         const char* value, const char* filename);
