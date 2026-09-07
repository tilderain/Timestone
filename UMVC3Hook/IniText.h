#pragma once

#include <string>
#include <string_view>

// Deliberately tolerant: only interpret section headers and the requested key.
// Everything else (including malformed lines) is retained byte-for-byte.
namespace IniText {
template <typename C>
std::basic_string_view<C> Trim(std::basic_string_view<C> text)
{
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r'))
        text.remove_suffix(1);
    return text;
}

template <typename C>
bool Equal(std::basic_string_view<C> a, std::basic_string_view<C> b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        C x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x += 'a' - 'A';
        if (y >= 'A' && y <= 'Z') y += 'a' - 'A';
        if (x != y) return false;
    }
    return true;
}

template <typename C>
std::basic_string<C> Update(std::basic_string<C> text,
    std::basic_string_view<C> section, std::basic_string_view<C> key,
    std::basic_string_view<C> value, size_t start = 0)
{
    using View = std::basic_string_view<C>;
    using String = std::basic_string<C>;
    bool active = false, foundSection = false;
    size_t insert = text.size();
    const String separators = {static_cast<C>('\r'), static_cast<C>('\n')};
    String newline(1, '\n');
    const size_t firstNewline = text.find_first_of(separators, start);
    if (firstNewline == String::npos) newline.insert(newline.begin(), '\r');
    else if (text[firstNewline] == '\r') {
        newline.assign(1, '\r');
        if (firstNewline + 1 < text.size() && text[firstNewline + 1] == '\n')
            newline += static_cast<C>('\n');
    }

    for (size_t pos = start; pos < text.size();) {
        size_t end = text.find_first_of(separators, pos);
        if (end == String::npos) end = text.size();
        const View raw(text.data() + pos, end - pos);
        const View line = Trim(raw);
        if (!line.empty() && line.front() == '[') {
            if (active) { insert = pos; active = false; }
            // A broken header ends the previous section but is never discarded.
            const size_t close = line.rfind(']');
            if (!foundSection && close != View::npos && close > 0 &&
                Equal(Trim(line.substr(1, close - 1)), section)) {
                active = foundSection = true;
                insert = text.size();
            }
        } else if (active && !line.empty() && line.front() != ';' && line.front() != '#') {
            const size_t equals = raw.find('=');
            if (equals != View::npos && Equal(Trim(raw.substr(0, equals)), key)) {
                size_t valueEnd = end;
                if (valueEnd > pos && text[valueEnd - 1] == '\r') --valueEnd;
                text.replace(pos + equals + 1, valueEnd - pos - equals - 1, value);
                return text;
            }
        }
        pos = end == text.size() ? end : end + 1;
        if (end < text.size() && text[end] == '\r' && pos < text.size() && text[pos] == '\n')
            ++pos;
    }

    String addition;
    if (insert > start && text[insert - 1] != '\n' && text[insert - 1] != '\r') addition += newline;
    if (!foundSection) {
        addition += static_cast<C>('[');
        addition += section;
        addition += static_cast<C>(']');
        addition += newline;
    }
    addition += key;
    addition += static_cast<C>('=');
    addition += value;
    addition += newline;
    text.insert(insert, addition);
    return text;
}
} // namespace IniText
