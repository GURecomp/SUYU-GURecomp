# Playing with Ryujinx players (RyuLdn)

**Experimental.** The exported game can carry its local play (Gathering Hall local mode)
through a **RyuLdn server**, the lobby server Ryujinx uses for its "LDN" multiplayer, instead
of a suyu room. Everyone on the same server sees the same lobbies, so exported-game players and
Ryujinx players can play together.

Room play (`backend = room`, INSTALL.md section 8) is unchanged and stays the default. One run
uses one of the two; switching needs a restart. Players in a suyu room and players on a RyuLdn
server can't be in the same lobby (a host can't serve both at once).

## The game menu

**Multiplayer** tab (F10):

- **Play over: Suyu room / Ryujinx LDN** at the top switches the backend (saved to
  `game_settings.ini`, applies at the next start).
- **Ryujinx LDN** tab, **Server** (switches at once, outside a lobby):
  - **Ryujinx public server** (default): `ldn.ryujinx.app`.
  - **Host for friends (Radmin VPN)**: this game runs the lobby server itself. Give friends
    one of the listed addresses (Radmin VPN or home network; hidden until clicked). Windows
    may ask whether the game may use the network: allow it. No public server needed.
  - **Join a friend's server**: paste the address of a friend who hosts.
  Exported-game friends pick **Join a friend's server**; Ryujinx friends set Multiplayer mode
  RyuLDN with the host's address as their LDN server. Then everyone hosts or searches in the
  game's local play as usual.
- **Public rooms** lists this game's open lobbies on the current server and any extra
  servers, with host name (the **Name** at the top of the Multiplayer tab), players and server; it refreshes every 15 s while open. Join them
  from the game's own local play search. Below the list you can add or remove extra servers
  (their addresses stay hidden until **Show**).

## Settings

In `game_settings.ini` next to the exe, section `[Multiplayer]`:

| Key | Value |
|---|---|
| `backend` | `room` (default) or `ryuldn` |
| `ryuldn_host` | The main server: lobbies you host live there. Default `ldn.ryujinx.app` (Ryujinx's public server); empty means the same. `self` = this game hosts the server for friends. Otherwise a friend's address (`host` or `host:port`). |
| `ryuldn_port` | `30456` (Ryujinx always uses this port); also the port your own server listens on. |
| `ryuldn_servers` | Extra servers, comma separated, `host` or `host:port`. Their lobbies show up in the game's search and in Public rooms, and joining one goes through its server. |
| `ryuldn_passphrase` | Empty = public lobbies. `Ryujinx-` followed by 8 hex digits (e.g. `Ryujinx-1a2b3c4d`) = only players with the same passphrase see each other; Ryujinx players set the same value as their LDN passphrase. Anything else counts as empty. |

Ryujinx players use their default multiplayer server (or the same address), mode "RyuLdn",
and the same game version (MHGU 1.4.0).

Ryujinx's public server is run for Ryujinx users; its operators state that other clients may
be banned. If that happens, use **Host for friends** (above), or run a standalone server.

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
- Host for friends: the in-game server does what a lobby needs (virtual network, search
  filters, version checks, relaying, idle pings). A Ryujinx host that offers its own P2P relay
  is told it isn't reachable and relayed by the server instead, as LdnServer does.
- Joining a Ryujinx host: works whether the host relays through the server or runs its own
  P2P proxy (Ryujinx does when UPnP works on its router); the host's proxy port must be
  reachable, as for Ryujinx joiners.
- Not done: hosting through our own P2P proxy (UPnP), IPv6 servers, lobbies mixing suyu-room
  and RyuLdn players.

## Credits

The RyuLdn protocol and behaviour come from the Ryujinx project: LdnServer and Ryujinx's LDN
client (both MIT licensed). This implementation was written from the protocol (message layouts
measured from the server's types, behaviour read from both), not copied. MIT notice of those
projects: Copyright (c) Ryujinx Team and Contributors; see LICENSES/MIT.txt.
