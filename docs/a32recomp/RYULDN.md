# Playing with Ryujinx players (RyuLdn)

**Experimental.** The exported game can carry its local play (Gathering Hall local mode)
through a **RyuLdn server**, the lobby server Ryujinx uses for its "LDN" multiplayer, instead
of a suyu room. Everyone on the same server sees the same lobbies, so exported-game players and
Ryujinx players can play together.

There is nothing to switch. Local play uses a suyu room and RyuLdn servers at the same time:

- **Hosting** (you host in the game's local play): this PC opens a suyu room and its own RyuLdn
  server by itself, creates the lobby, and opens it to both. Suyu friends join the room, Ryujinx
  friends use your address as their LDN server, and everyone is in the same lobby. They close
  again when the lobby does. See "Playing with both at once".
- **Joining**: the game's local-play search lists the lobbies of the room you joined, of its
  friend's server and of Ryujinx's public server together, and joining one goes through whichever
  it came from.

`game_settings.ini` `[Multiplayer] backend = room` or `ryuldn` forces one side only (advanced).

## The game menu

**Multiplayer** tab (F10): your **Name** at the top, then two tabs.

- **Join Room**: paste a friend's invite or address and press Join. One address covers both
  worlds: you join their room, and the lobbies on their server show up in the game's search.
  The same tab has the hosting options (room name, password, players, port) and your addresses
  to give friends (Copy invite; Ryujinx friends use the plain address as their LDN server).
  Windows may ask whether the game may use the network when you host: allow it.
- **Public rooms**: this game's open lobbies on Ryujinx's public server, your server and any
  extra servers, with host name, players and server; it refreshes every 15 s while open. Join
  them from the game's own local-play search. Extra servers can be added and removed below the list.

## Playing with both at once

Mode **Both**. The game that hosts is the translation layer between the two worlds:

1. Host local play in the game as usual. This PC opens the room (default port 24872) and its own
   server (port 30456) and creates the lobby on the server.
2. Suyu friends join your room (Join Room tab: invite or address) and search in local play.
   Ryujinx friends set Multiplayer mode RyuLDN and your address as their LDN server.

What the host does for you (`src/network/ryuldn/room_bridge.*`):

- answers the room's lobby searches with the server lobby, in the wire format suyu players
  already speak;
- joins each suyu player to the server lobby as an ordinary player of its own (so Ryujinx
  players, the server and the host game see a normal player with a 10.114.x.x address);
- relays the game traffic between the room and the server and rewrites the addresses on the
  way. Suyu players keep seeing room addresses (192.168.x.y): the host is the host's room
  address, other suyu players are themselves, and every Ryujinx player gets a stand-in
  192.168.250.n (n = lobby slot), which the room hands to the host (`Room::SetHostGateway`).
  Traffic between two suyu players stays inside the room.

Without a reachable server the host's lobby is a plain room lobby, and searching asks the room
and the server side by side, so one search shows both (a server with no answer is skipped for 10 s).
`ryuldn-test --bridge` runs this with a suyu host, two suyu players and a RyuLdn player in one
process.

## Settings

In `game_settings.ini` next to the exe, section `[Multiplayer]`:

| Key | Value |
|---|---|
| `backend` | empty/`auto` (default: room and servers together), `room` or `ryuldn` (one side only) |
| `ryuldn_public` | `off` leaves Ryujinx's public server out of the lobby search. Default on. |
| `ryuldn_host` | Where lobbies you host live. Empty/`self` (default) = this PC's own server, which opens while you host. Otherwise a server's address (`host` or `host:port`), e.g. your group's always-on server. |
| `ryuldn_port` | `30456` (Ryujinx always uses this port); also the port your own server listens on. |
| `ryuldn_servers` | Extra servers, comma separated, `host` or `host:port`. Their lobbies show up in the game's search and in Public rooms, and joining one goes through its server. |
| `ryuldn_passphrase` | Empty = public lobbies. `Ryujinx-` followed by 8 hex digits (e.g. `Ryujinx-1a2b3c4d`) = only players with the same passphrase see each other; Ryujinx players set the same value as their LDN passphrase. Anything else counts as empty. |

Ryujinx players use their default multiplayer server (or the same address), mode "RyuLdn",
and the same game version (MHGU 1.4.0).

Ryujinx's public server is run for Ryujinx users; its operators state that other clients may
be banned. If that happens, set `ryuldn_public = off` (hosting already uses this PC's own server), or run a standalone server.

## Running a standalone server

For a server that runs without anyone playing (e.g. on a group's always-on PC), use
[LdnServer](https://github.com/Ryubing/LdnServer) (C#, MIT):

1. Install the .NET 8 SDK or newer (a newer SDK builds it; the .NET 8 runtime runs it).
2. Get the source and build it: `dotnet build -c Release`.
3. The release build expects a Redis server for statistics and exits without one. Either run
   Redis (`LDN_REDIS_HOST`, `LDN_REDIS_PORT`), or make the `StatsDumper.Start(...)` and
   `StatsDumper.Stop()` calls in `Program.cs` optional.
4. Start `LanPlayServer` (`LDN_HOST` = address to listen on, default all; `LDN_PORT`, default
   30456) and open TCP port 30456 to the players (port forwarding or a VPN such as Radmin VPN).

## What works and what doesn't

- Hosting: the server relays the game traffic (Ryujinx joiners handle this).
- The in-game server (opens while you host) does what a lobby needs (virtual network, search
  filters, version checks, relaying, idle pings). A Ryujinx host that offers its own P2P relay
  is told it isn't reachable and relayed by the server instead, as LdnServer does.
- Joining a Ryujinx host: works whether the host relays through the server or runs its own
  P2P proxy (Ryujinx does when UPnP works on its router); the host's proxy port must be
  reachable, as for Ryujinx joiners.
- Both mode: a suyu player and a Ryujinx player each talk to the host (and to each other through
  it); suyu players only see Ryujinx players as stand-in addresses (192.168.250.n), which is
  what the game uses. TCP traffic isn't relayed (the game's local play is UDP).
- Not done: hosting through our own P2P proxy (UPnP), IPv6 servers, a lobby hosted by a suyu
  room player that Ryujinx players can join (the host has to be the exported game).

## Credits

The RyuLdn protocol and behaviour come from the Ryujinx project: LdnServer and Ryujinx's LDN
client (both MIT licensed). This implementation was written from the protocol (message layouts
measured from the server's types, behaviour read from both), not copied. MIT notice of those
projects: Copyright (c) Ryujinx Team and Contributors; see LICENSES/MIT.txt.
