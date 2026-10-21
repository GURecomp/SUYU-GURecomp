// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstdint>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "common/log_redact.h"
#include "common/string_util.h"

namespace Common::Log {
namespace {

struct Root {
    std::string path; // no trailing separator
    std::string placeholder;
};

struct State {
    std::shared_mutex mutex;
    std::vector<Root> roots; // longest first
    std::vector<std::string> names; // bare words to hide (user name, computer name)
    std::vector<std::string> name_placeholders;
    std::atomic_bool initialised{false};
};

State& GetState() {
    static State state;
    return state;
}

std::atomic_bool g_enabled{true};

bool IsSep(char c) {
    return c == '\\' || c == '/';
}

bool IsAlnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool IsAlpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

char Fold(char c) {
#ifdef _WIN32
    if (c == '/') {
        return '\\';
    }
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
#else
    return c;
#endif
}

char FoldName(char c) {
    return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
}

std::string TrimSeps(std::string p) {
    while (p.size() > 1 && IsSep(p.back())) {
        p.pop_back();
    }
    return p;
}

void SortRoots(std::vector<Root>& roots) {
    std::stable_sort(roots.begin(), roots.end(),
                     [](const Root& a, const Root& b) { return a.path.size() > b.path.size(); });
}

#ifdef _WIN32
std::string EnvUtf8(const wchar_t* name) {
    wchar_t buf[2048];
    const DWORD n = GetEnvironmentVariableW(name, buf, 2048);
    if (n == 0 || n >= 2048) {
        return {};
    }
    return Common::UTF16ToUTF8(std::wstring_view{buf, n});
}
#endif

bool NameIsSafeToHide(const std::string& n) {
    if (n.size() < 4) {
        return false;
    }
    static constexpr const char* kCommon[] = {"user",   "admin",    "administrator", "default",
                                              "public", "guest",    "test",          "game",
                                              "owner",  "desktop",  "home",          "root",
                                              "path",   "username", "computername",  "userprofile",
                                              "suyu",   "yuzu",     "player"};
    std::string low;
    for (char c : n) {
        low += FoldName(c);
    }
    for (const char* c : kCommon) {
        if (low == c) {
            return false;
        }
    }
    return true;
}

void EnsureInitialised(State& st) {
    // Called with the unique lock held.
    if (st.initialised) {
        return;
    }
#ifdef _WIN32
    const std::string profile = EnvUtf8(L"USERPROFILE");
    if (!profile.empty()) {
        st.roots.push_back({TrimSeps(profile), "%USERPROFILE%"});
    }
    std::string user = EnvUtf8(L"USERNAME");
    std::string computer;
    wchar_t cn[MAX_COMPUTERNAME_LENGTH + 1]{};
    DWORD cn_len = MAX_COMPUTERNAME_LENGTH + 1;
    if (GetComputerNameW(cn, &cn_len)) {
        computer = Common::UTF16ToUTF8(std::wstring_view{cn, cn_len});
    }
    if (computer.empty()) {
        computer = EnvUtf8(L"COMPUTERNAME");
    }
#else
    std::string user;
    std::string computer;
    if (const char* home = std::getenv("HOME"); home && *home) {
        st.roots.push_back({TrimSeps(home), "~"});
    }
    for (const char* v : {"LOGNAME", "USER"}) {
        if (const char* s = std::getenv(v); s && *s) {
            user = s;
            break;
        }
    }
    if (const char* h = std::getenv("HOSTNAME"); h && *h) {
        computer = h;
    }
#endif
    // Longer name first, so "MY-PC" is not left as "<username>-PC".
    const bool hide_user = NameIsSafeToHide(user);
    const bool hide_computer = NameIsSafeToHide(computer) && computer != user;
    const auto add_user = [&] {
        if (hide_user) {
            st.names.push_back(user);
            st.name_placeholders.push_back("<username>");
        }
    };
    const auto add_computer = [&] {
        if (hide_computer) {
            st.names.push_back(computer);
            st.name_placeholders.push_back("<computername>");
        }
    };
    if (computer.size() > user.size()) {
        add_computer();
        add_user();
    } else {
        add_user();
        add_computer();
    }
    SortRoots(st.roots);
    st.initialised = true;
}

/// Length of `root` if it matches `s` at `i` (folding case/slashes on Windows) and ends at a
/// boundary, else 0.
size_t MatchRoot(std::string_view s, size_t i, const std::string& root) {
    if (i + root.size() > s.size()) {
        return 0;
    }
    for (size_t k = 0; k < root.size(); ++k) {
        if (Fold(s[i + k]) != Fold(root[k])) {
            return 0;
        }
    }
    const size_t end = i + root.size();
    if (end < s.size() && IsAlnum(s[end]) && !IsSep(root.back())) {
        return 0; // "C:\foo" must not match "C:\foobar"
    }
    return root.size();
}

bool IsSegmentChar(char c) {
    return !(c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' ||
             c == '>' || c == '|' || static_cast<unsigned char>(c) < 0x20);
}

/// Parses an absolute path whose first segment starts at `j`. Directory segments may contain
/// spaces; the last segment ends at the first whitespace. Returns the end offset and the file name.
size_t ParsePath(std::string_view s, size_t j, std::string_view& file) {
    for (;;) {
        size_t k = j;
        while (k < s.size() && IsSegmentChar(s[k])) {
            ++k;
        }
        if (k < s.size() && IsSep(s[k]) && k > j) {
            j = k + 1; // directory segment
            continue;
        }
        // Last segment: stop at whitespace, drop trailing punctuation.
        size_t e = j;
        while (e < k && s[e] != ' ' && s[e] != '\t') {
            ++e;
        }
        while (e > j && (s[e - 1] == '.' || s[e - 1] == ',' || s[e - 1] == ';' || s[e - 1] == ')' ||
                         s[e - 1] == ']' || s[e - 1] == '\'' || s[e - 1] == '`')) {
            --e;
        }
        file = s.substr(j, e - j);
        return e;
    }
}

std::string RedactPaths(std::string_view s, const std::vector<Root>& roots) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const char c = s[i];
        const bool prev_ok = i == 0 || !(IsAlnum(s[i - 1]) || s[i - 1] == '_' || s[i - 1] == '.');
        const bool drive = IsAlpha(c) && i + 2 < s.size() && s[i + 1] == ':' && IsSep(s[i + 2]);
        const bool unc = c == '\\' && i + 2 < s.size() && s[i + 1] == '\\' && IsAlnum(s[i + 2]);
        const bool unix_abs = c == '/' && i + 1 < s.size() && IsAlpha(s[i + 1]);
        if (!prev_ok || !(drive || unc || unix_abs)) {
            out += c;
            ++i;
            continue;
        }
        bool done = false;
        for (const Root& r : roots) {
            const size_t n = MatchRoot(s, i, r.path);
            if (n) {
                out += r.placeholder;
                i += n; // what follows (relative part) is copied by the normal loop
                done = true;
                break;
            }
        }
        if (done) {
            // Skip the generic handling for the remainder: it is relative to a placeholder and
            // is plain text now (it never starts with a drive letter).
            continue;
        }
        if (unix_abs) {
            // Only /home/<name> and /Users/<name> are recognisable in free text.
            for (std::string_view p : {std::string_view{"/home/"}, std::string_view{"/Users/"}}) {
                if (s.substr(i, p.size()) == p) {
                    size_t e = i + p.size();
                    while (e < s.size() && IsSegmentChar(s[e]) && s[e] != ' ') {
                        ++e;
                    }
                    out += '~';
                    i = e;
                    done = true;
                    break;
                }
            }
            if (!done) {
                out += c;
                ++i;
            }
            continue;
        }
        std::string_view file;
        const size_t start = drive ? i + 3 : i + 2;
        const size_t e = ParsePath(s, start, file);
        out += "<path>/";
        out.append(file);
        i = e;
    }
    return out;
}

std::string RedactNames(std::string text, const std::vector<std::string>& names,
                        const std::vector<std::string>& placeholders) {
    for (size_t n = 0; n < names.size(); ++n) {
        const std::string& name = names[n];
        std::string out;
        out.reserve(text.size());
        size_t i = 0;
        while (i < text.size()) {
            bool hit = false;
            if (i + name.size() <= text.size() && FoldName(text[i]) == FoldName(name[0]) &&
                (i == 0 || !IsAlnum(text[i - 1])) &&
                (i + name.size() == text.size() || !IsAlnum(text[i + name.size()]))) {
                hit = true;
                for (size_t k = 1; k < name.size(); ++k) {
                    if (FoldName(text[i + k]) != FoldName(name[k])) {
                        hit = false;
                        break;
                    }
                }
            }
            if (hit) {
                out += placeholders[n];
                i += name.size();
            } else {
                out += text[i++];
            }
        }
        text = std::move(out);
    }
    return text;
}

/// IPv4 pseudonyms: <ip-N class>, N = order of first appearance in this process. Port stays visible.
std::string IpTag(uint32_t ip) {
    static std::mutex mtx;
    static std::map<uint32_t, int> ids;
    int id;
    {
        std::lock_guard lock{mtx};
        id = ids.try_emplace(ip, static_cast<int>(ids.size()) + 1).first->second;
    }
    const unsigned a = ip >> 24, b = (ip >> 16) & 255;
    const char* cls = "public";
    if (a == 10 || (a == 172 && b >= 16 && b <= 31) || (a == 192 && b == 168) ||
        (a == 169 && b == 254)) {
        cls = "lan";
    } else if (a == 26) {
        cls = "vpn";
    }
    return "<ip-" + std::to_string(id) + " " + cls + ">";
}

bool IsDigit(char c) {
    return c >= '0' && c <= '9';
}

/// Four octets 0-255 (1-3 digits each), not touching other digits/dots/letters/underscore.
std::string RedactIps(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    size_t i = 0;
    while (i < s.size()) {
        const bool start = IsDigit(s[i]) && (i == 0 || !(IsAlnum(s[i - 1]) || s[i - 1] == '.' ||
                                                           s[i - 1] == '_'));
        if (start) {
            size_t j = i;
            uint32_t ip = 0;
            int octets = 0;
            while (octets < 4) {
                size_t k = j;
                unsigned v = 0;
                while (k < s.size() && IsDigit(s[k]) && k - j < 3) {
                    v = v * 10 + static_cast<unsigned>(s[k] - '0');
                    ++k;
                }
                if (k == j || v > 255) {
                    break;
                }
                ip = (ip << 8) | v;
                ++octets;
                j = k;
                if (octets < 4) {
                    if (j + 1 < s.size() && s[j] == '.' && IsDigit(s[j + 1])) {
                        ++j;
                    } else {
                        break;
                    }
                }
            }
            const bool tail_ok = j >= s.size() ||
                                 !(IsAlnum(s[j]) || s[j] == '_' ||
                                   (s[j] == '.' && j + 1 < s.size() && IsDigit(s[j + 1])));
            if (octets == 4 && tail_ok) {
                if ((ip >> 24) == 127 || ip == 0) {
                    out.append(s.substr(i, j - i));
                } else {
                    out += IpTag(ip);
                }
                i = j;
                continue;
            }
            // Not an address: copy the whole digit/dot run so a later start cannot match inside it.
            size_t e = i;
            while (e < s.size() && (IsDigit(s[e]) || s[e] == '.')) {
                ++e;
            }
            out.append(s.substr(i, e - i));
            i = e;
            continue;
        }
        out += s[i++];
    }
    return out;
}

} // namespace

void SetRedactEnabled(bool enabled) {
    g_enabled = enabled;
}

bool RedactEnabled() {
    return g_enabled;
}

void AddRedactRoot(std::string_view path, std::string_view placeholder) {
    if (path.empty()) {
        return;
    }
    State& st = GetState();
    std::unique_lock lock{st.mutex};
    EnsureInitialised(st);
    st.roots.push_back({TrimSeps(std::string{path}), std::string{placeholder}});
    SortRoots(st.roots);
}

std::string RedactText(std::string_view text) {
    if (!g_enabled || text.empty()) {
        return std::string{text};
    }
    State& st = GetState();
    if (!st.initialised) {
        std::unique_lock lock{st.mutex};
        EnsureInitialised(st);
    }
    std::shared_lock lock{st.mutex};
    return RedactIps(RedactNames(RedactPaths(text, st.roots), st.names, st.name_placeholders));
}

} // namespace Common::Log
