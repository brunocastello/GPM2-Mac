# Network (Link Play): feasibility study

Status: study only (2026-11-02). Not implemented: it falls outside the "modest" scope (see estimate).

## What the game uses (Gpm2.exe 1.02B, decompilation)
- `DPLAYX.dll` imports 2 ordinals: **#1 = `DirectPlayCreate(lpGUID, &lpDP, NULL)`** (`FUN_00537f50`) and **#2 = `DirectPlayEnumerate(cb, ctx)`**
  (called at `0x0044b3b0`; callback `FUN_0044b3c0` stores up to 10 providers: 16-byte GUID + name, shown in a menu 0x20 px/line).
  Today both are stubs that return 0 (`runtime/win32/misc.c`).
- After Create the game does `QueryInterface(IID {9d460580-a822-11cf-960c-0080c7534e82})` = **IDirectPlay2A** (DirectX 5/6), stores it in `DAT_0071eaa4`
  and releases the original interface. It does not use DirectPlay 3/4, lobby nor IDirectPlayAddress.
- Vtable methods actually called (offset -> method): 0x10 `Close`, 0x18 `CreatePlayer`, 0x24 `DestroyPlayer`, 0x30 `EnumPlayers`,
  0x34 `EnumSessions`, 0x38 `GetCaps` (flag 1), 0x50 `GetPlayerData`, 0x54 `GetPlayerName` (size: 1st call with NULL), 0x58 `GetSessionDesc`,
  0x60 `Open` (flags 1 = CREATE, 2 = JOIN; `DPSESSIONDESC2`, ~0x58 bytes on the stack), 0x64 `Receive`, 0x68 `Send` (up to 10 attempts, `Sleep(200)`;
  `0x80004005` if not initialized), 0x74 `SetPlayerData`, 8 `Release`. Thin wrappers at `0x00537f10..0x005386a0`. It never calls `Initialize`
  (`DirectPlayCreate` already initializes the chosen provider).
- Strings: "DP_MSG_DPLAY_DATA sent", "dplay data num %d", "Failed to DPlaySend", "Send failed (dplay 365)", session "GPM_JOINER", "Sending 3", modes
  "Modem"/"Serial"/"Network" (there is also a modem/serial path with ZMODEM, `Link Not Responding`, 19200 bps, outside this study).

## Message flow (high level)
1. `DirectPlayEnumerate` -> the user chooses the provider (Network/TCP-IP) -> `DirectPlayCreate(guid)` -> QI IDirectPlay2A.
2. Master: `Open(CREATE)` with `DPSESSIONDESC2` ("GPM_JOINER", 2 players) and `CreatePlayer`. Client: `EnumSessions` (callback with the list), `Open(JOIN)`,
   `CreatePlayer`, `EnumPlayers`/`GetPlayerName` for the other side's name.
3. Game data exchange: opaque packets (type in the 1st byte; type 3 produces the log "Sending 3") through `Send(from, to, flags, buf, len)` and `Receive` by polling
   (no special `DPRECEIVE_*`, DPSYS_* system messages arrive as messages with `from == DPID_SYSMSG`).

## What a TCP/IP-only implementation between two Macs requires
- COM in the core: an IDirectPlay2A object with a 31-entry vtable in guest memory (`win32_register` thunks per method; unused ones return
  `DPERR_UNSUPPORTED`), `QueryInterface/AddRef/Release`, and **reentrant calls into the guest** (callbacks of `DirectPlayEnumerate`, `EnumSessions`, `EnumPlayers`;
  the core does not yet have a helper to call a guest callback from an import — to be created).
- Synthetic "TCP/IP Connection For DirectPlay" provider: GUID {36E95EE0-8577-11cf-960C-0080C7534E82} announced in `DirectPlayEnumerate`; the name is unique.
- Socket layer on the host (`host_net_*`: listen/accept/connect/send/recv non-blocking, UDP broadcast or Bonjour/NSNetService for discovery,
  fixed port) keeping the core C99 without Apple headers (BSD sockets will do; Bonjour stays in the `.m`).
- Own protocol: framing (len + type), session handshake (desc, players, DPIDs), DPSYS messages (CREATEPLAYERORGROUP, DESTROYPLAYERORGROUP,
  SESSIONLOST), receive queue per player, player data (`SetPlayerData`/`GetPlayerData`), timeout and connection drop.
- The game's network dialogs (provider menu, session list, name) are already windows of the game itself; they only need the right answers.
- Tests: two headless processes exchanging packets + a script with the game (Link Play) in two instances; today the network match does not even start by script.

## Estimate
DirectPlay core (COM + guest callbacks + queues + DPSYS) ~900-1300 lines of C; socket host ~300-500 (C/BSD) + ~150 (.m, optional Bonjour);
protocol/handshake ~400; tests ~300. Total ~2000-2700 lines and several debugging sessions, because the behavior on the game's side (Link Play
states, ZMODEM on the serial path, synchronization of the two sides) can only be validated with two real instances. **It is not modest**: it stays as a phase of its own.
Main risk: the exact format of the game's packets is opaque (it does not need to be decoded if the transport is faithful), but the order of the DPSYS messages
and the semantics of `Receive` (`from/to`, `DPID_SYSMSG`, `DPRECEIVE_ALL`) must be exact.

## Log — implementation (2026-11-02)
Status: **implemented and tested only in the engine (loopback); it has never run with the game** (the game's Link Play is not reachable by script).
- `runtime/host/net.[ch]`: thin BSD sockets (TCP listen/accept/connect, UDP with broadcast). `win32/dplay_eng.[ch]`: engine (session, players,
  queues, DPSYS) without guest memory, with one I/O thread per open session (the host answers discovery/join even with the guest busy).
  `win32/dplay.c`: COM `IDirectPlay2A` (32-entry vtable in guest memory, 17 real methods, the rest `E_NOTIMPL`) + `DirectPlayCreate` (#1) and
  `DirectPlayEnumerate` (#2). Indirect guest calls to host functions: new `win32_thunk()` + `THUNK_MAGIC_BASE` range (0xFFE00000) in `dispatch()`.
- Single provider "TCP/IP (Internet)" with the GUID {36E95EE0-8577-11cf-960C-0080C7534E82}; another GUID -> `DPERR_UNAVAILABLE`. `QueryInterface` accepts the
  IDirectPlay* family (same GUID suffix) and IUnknown, returning the same object.
- Protocol: port 47624 (TCP and UDP; `GPM2_NET_PORT` overrides). Discovery: UDP "GPMQ" (broadcast 255.255.255.255 + unicast to the typed IP) -> reply "GPMR" +
  descriptor (guids, flags, max/current, user1-4, name) + TCP port. Session: TCP, frames `[u32 len][u8 type][data]` little-endian: HELLO(instance guid),
  WELCOME(descriptor + players), CREATEP/PNEW (the host assigns DPIDs), DELP/PDEL, DATA(from,to,flags,size), BYE. Star: the creator routes
  (`to == 0` = DPID_ALLPLAYERS). System messages (`from == 0`): CREATEPLAYERORGROUP (0x3), DESTROYPLAYERORGROUP (0x5), SESSIONLOST (0x31); the drop of
  a client generates DESTROY on the host, the host's drop generates SESSIONLOST on the client (and `Send` starts failing until `Close`).
- Address of the other Mac: `host_net_ask_address` (app: native alert with a text field, "ip" or "ip:port", empty = LAN only; headless: `GPM2_NET_HOST`),
  asked on the 1st `EnumSessions` of each object.
- Test: `make dplay-test` (`tests/dplay_test.c`): host and client in the same process on loopback (port 47999): enumeration by IP, join, players,
  limit of 2, data in both directions, broadcast, 50 packets in order, DESTROY and SESSIONLOST. Passes.
- Limitations: the COM layer was not exercised (no test guest); `EnumSessions` is always synchronous (waits <= 2 s, ignores ASYNC); no session password, groups,
  host migration nor `GetPlayerCaps/SetPlayerName/SetSessionDesc`; player data only local; a player created later does not receive CREATE for those that already existed (only
  `EnumPlayers`); broadcast discovery depends on the LAN allowing it (macOS's local network may ask for permission); maximum message 16 KB. The game's Modem/Serial paths
  (ZMODEM) remain out. The exact order of DPSYS that the game expects is a hypothesis until there is a test with two real instances.

### Bug "Unable to complete connection." in Link Play (2026-11-02)
- Cause: `IDirectPlay2::Open` with `DPOPEN_CREATE = 2` (JOIN = 1); `dplay.c` tested bit 1 as CREATE, so the Master fell into the JOIN path, returned
  `DPERR_NOSESSIONS` (0x887700DC) and the game (FUN_005387b0 -> FUN_00538030) showed string 16025. Found with the new `GPM2_DPLAY_TRACE=1` (each method, 6 args, HRESULT).
- Fix: `fl & 2u` = CREATE. Headless loopback test (`GPM2_NET_HOST=127.0.0.1`): Master `Open(2)`+`CreatePlayer` ok; Slave opens dialog #140
  "Select Service Provider" (session list via EnumSessions, `dlg-select 1006 0` + `dlg-click 1`), `Open(1)`+`CreatePlayer` ok, and the two exchange `Send`/`Receive`
  (HELLO/DPSYS); no alert. Not yet verified: later game screens on two instances (the capture does not show them in the headless).
- Game flow: Master button = DAT_00715090==2 (Open CREATE, name "GPM2 - <computer>", max 4); Slave = 1 (EnumSessions -> dialog -> Open JOIN).
