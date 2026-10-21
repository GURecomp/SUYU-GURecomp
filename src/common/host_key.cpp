#include "common/host_key.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <atomic>
#endif

namespace Common {

#ifdef _WIN32
void SetHostKeyPoller(HostKeyPoller) {}

bool HostKeyDown(int windows_vk) {
    return (GetAsyncKeyState(windows_vk) & 0x8000) != 0;
}
#else
namespace {
std::atomic<HostKeyPoller> g_poller{nullptr};
}

void SetHostKeyPoller(HostKeyPoller poller) {
    g_poller.store(poller);
}

bool HostKeyDown(int windows_vk) {
    const HostKeyPoller p = g_poller.load();
    return p != nullptr && p(windows_vk);
}
#endif

} // namespace Common
