// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include <fmt/format.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <cerrno>
#include <climits>
#include <csignal>
#include <cstring>
#include <fstream>
#include <spawn.h>
#include <unistd.h>
extern char** environ;
#endif

#include "suyu_cmd/relaunch.h"

namespace Relaunch {

namespace {
constexpr const char* kWaitVar = "SUYU_RESTART_WAIT_PID"; // set only for the child of a restart
constexpr auto kMaxWait = std::chrono::seconds(60);
#ifndef _WIN32
std::vector<std::string> g_args;
#endif
} // namespace

void OnStart(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    wchar_t buf[32]{};
    const DWORD n = GetEnvironmentVariableW(L"SUYU_RESTART_WAIT_PID", buf, 32);
    if (n == 0 || n >= 32) {
        return;
    }
    SetEnvironmentVariableW(L"SUYU_RESTART_WAIT_PID", nullptr); // not passed on to anything this process starts
    const DWORD pid = static_cast<DWORD>(std::wcstoul(buf, nullptr, 10));
    if (pid == 0 || pid == GetCurrentProcessId()) {
        return;
    }
    if (HANDLE h = OpenProcess(SYNCHRONIZE, FALSE, pid)) {
        WaitForSingleObject(h, static_cast<DWORD>(std::chrono::milliseconds(kMaxWait).count()));
        CloseHandle(h);
    } // no handle: it is already gone
#else
    for (int i = 0; i < argc; ++i) {
        g_args.emplace_back(argv[i]);
    }
    const char* v = std::getenv(kWaitVar);
    if (v == nullptr) {
        return;
    }
    const long pid = std::strtol(v, nullptr, 10);
    unsetenv(kWaitVar);
    if (pid <= 0 || pid == static_cast<long>(getpid())) {
        return;
    }
    const auto gone = [pid] {
        if (kill(static_cast<pid_t>(pid), 0) != 0 && errno == ESRCH) {
            return true;
        }
        // A zombie (exited, not reaped yet) still answers kill(): read its state.
        std::ifstream st(fmt::format("/proc/{}/stat", pid));
        std::string all;
        std::getline(st, all);
        const auto close = all.rfind(')');
        return close != std::string::npos && close + 2 < all.size() && all[close + 2] == 'Z';
    };
    const auto until = std::chrono::steady_clock::now() + kMaxWait;
    while (!gone() && std::chrono::steady_clock::now() < until) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
#endif
}

bool StartNewInstance(std::string* error, unsigned long* pid) {
#ifdef _WIN32
    std::wstring exe(32768, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, exe.data(), static_cast<DWORD>(exe.size()));
    if (len == 0 || len >= exe.size()) {
        *error = fmt::format("could not find the game's exe (error {})", GetLastError());
        return false;
    }
    exe.resize(len);
    std::wstring cmd = GetCommandLineW(); // the same arguments as this start
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    SetEnvironmentVariableW(L"SUYU_RESTART_WAIT_PID", std::to_wstring(GetCurrentProcessId()).c_str());
    const BOOL ok = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi);
    const DWORD err = GetLastError();
    SetEnvironmentVariableW(L"SUYU_RESTART_WAIT_PID", nullptr);
    if (!ok) {
        char msg[512]{};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, err, 0, msg, sizeof(msg),
                       nullptr);
        std::string text = msg;
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) {
            text.pop_back();
        }
        *error = fmt::format("could not start the game again (error {}: {})", err, text);
        return false;
    }
    *pid = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
#else
    char exe[PATH_MAX]{};
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) {
        *error = fmt::format("could not find the game's binary ({})", std::strerror(errno));
        return false;
    }
    if (g_args.empty()) {
        g_args.emplace_back(exe);
    }
    std::vector<char*> argv;
    for (auto& a : g_args) {
        argv.push_back(a.data());
    }
    argv.push_back(nullptr);
    setenv(kWaitVar, std::to_string(getpid()).c_str(), 1);
    pid_t child = 0;
    const int rc = posix_spawn(&child, exe, nullptr, nullptr, argv.data(), environ);
    unsetenv(kWaitVar);
    if (rc != 0) {
        *error = fmt::format("could not start the game again ({})", std::strerror(rc));
        return false;
    }
    *pid = static_cast<unsigned long>(child);
    return true;
#endif
}

} // namespace Relaunch
