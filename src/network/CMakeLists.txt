# SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

# SPDX-FileCopyrightText: 2022 yuzu Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later

add_library(network STATIC
    announce_multiplayer_session.cpp
    announce_multiplayer_session.h
    ldn_backend.cpp
    ldn_backend.h
    ryuldn/backend.cpp
    ryuldn/backend.h
    ryuldn/client.cpp
    ryuldn/client.h
    ryuldn/protocol.cpp
    ryuldn/protocol.h
    ryuldn/room_bridge.cpp
    ryuldn/room_bridge.h
    ryuldn/server.cpp
    ryuldn/server.h
    ryuldn/socket_util.cpp
    ryuldn/socket_util.h
    network.cpp
    network.h
    packet.cpp
    packet.h
    room.cpp
    room.h
    room_member.cpp
    room_member.h
    verify_user.cpp
    verify_user.h
)

create_target_directory_groups(network)

target_link_libraries(network PRIVATE common enet::enet Boost::headers)
if (WIN32)
    target_link_libraries(network PRIVATE ws2_32)
endif()
if (ENABLE_WEB_SERVICE)
    target_compile_definitions(network PRIVATE ENABLE_WEB_SERVICE)
    target_link_libraries(network PRIVATE web_service)
endif()

# Solaris uses /lib/amd64/libsocket.so and /lib/amd64/libnsl.so
if (PLATFORM_SUN)
    target_link_libraries(network PRIVATE socket nsl)
endif()
