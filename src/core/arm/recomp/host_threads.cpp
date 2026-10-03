// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <vector>

#include <fmt/format.h>

#include "core/arm/recomp/host_threads.h"

#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#endif

namespace Core::HostThreads {

#ifdef _WIN32
namespace {

std::string Narrow(const wchar_t* w) {
    std::string s;
    for (; w && *w; ++w) {
        s += *w < 0x80 ? static_cast<char>(*w) : '?';
    }
    return s;
}

double Seconds(const FILETIME& ft) {
    const ULONGLONG t = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return static_cast<double>(t) / 1e7; // 100 ns units
}

} // namespace

std::string Describe(double uptime_s, size_t max_threads, double* total_cpu_s) {
    using GetDesc = HRESULT(WINAPI*)(HANDLE, PWSTR*);
    static const auto get_desc = reinterpret_cast<GetDesc>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"KernelBase.dll"),
                                               "GetThreadDescription")));
    struct Row {
        double user, kernel;
        DWORD id;
        std::string name;
    };
    std::vector<Row> rows;
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        return {};
    }
    const DWORD pid = GetCurrentProcessId();
    THREADENTRY32 te{};
    te.dwSize = sizeof(te);
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid) {
            continue;
        }
        const HANDLE h = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE, te.th32ThreadID);
        if (!h) {
            continue;
        }
        FILETIME created, exited, kernel, user;
        if (GetThreadTimes(h, &created, &exited, &kernel, &user)) {
            Row r{Seconds(user), Seconds(kernel), te.th32ThreadID, {}};
            PWSTR desc = nullptr;
            if (get_desc && SUCCEEDED(get_desc(h, &desc)) && desc) {
                r.name = Narrow(desc);
                LocalFree(desc);
            }
            rows.push_back(std::move(r));
        }
        CloseHandle(h);
    }
    CloseHandle(snap);
    if (total_cpu_s) {
        *total_cpu_s = 0;
        for (const Row& r : rows) {
            *total_cpu_s += r.user + r.kernel;
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.user + a.kernel > b.user + b.kernel;
    });
    std::string out = fmt::format("# {} threads; cpu = user+kernel seconds, % of one core over "
                                  "{:.0f} s uptime\n",
                                  rows.size(), uptime_s);
    for (size_t i = 0; i < std::min(rows.size(), max_threads); ++i) {
        const Row& r = rows[i];
        out += fmt::format("{:7.1f}s {:5.1f}%  (kernel {:5.1f}s)  {}\n", r.user + r.kernel,
                           uptime_s > 0 ? 100.0 * (r.user + r.kernel) / uptime_s : 0.0, r.kernel,
                           r.name.empty() ? fmt::format("thread {}", r.id) : r.name);
    }
    return out;
}
#else
std::string Describe(double, size_t, double*) {
    return {};
}
#endif

} // namespace Core::HostThreads
