# SPDX-FileCopyrightText: 2026 suyu Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# RyuLdn wire layer checks (round trips, recorded LdnServer session, malformed streams).
add_executable(ryuldn-test ryuldn_test.cpp)
target_link_libraries(ryuldn-test PRIVATE common network)
if (WIN32)
    target_link_libraries(ryuldn-test PRIVATE winmm ws2_32)
endif()
