# BLKT 1.0.0

BLKT is a lightweight, headless, single-account Bilibili live server written in **Qt 6 and C++17**. It provides authenticated live-room connectivity, danmaku and gift events, a permission-gated JavaScript plugin API, and a portable Windows runtime.

## Requirements

- Qt 6.4 or newer (Core, Network, WebSockets and Qml)
- CMake 3.21+
- C++17 compiler
- A valid Bilibili web session exported to `cookie.txt`

## Quick start

Place `cookie.txt` beside the executable, then run:

**Windows**
```powershell
.\blkt.exe --room 123456
```

**Linux**
```sh
./blkt --room 123456
```

Use Ctrl+C or SIGTERM for graceful shutdown. Logs are written to stdout. BLKT does not intentionally persist logs, cache, configuration, database, credentials, or temporary files. Windows may independently create Prefetch records; applications cannot prevent OS-managed execution telemetry.

## Credentials

Cookie is read-only and must be provisioned before startup. Netscape `cookies.txt` exports and raw Cookie headers are supported. BLKT imports only `SESSDATA`, `bili_jct`, and `buvid3`. Netscape entries must be shared Bilibili-domain cookies with root path and must not be expired. UTF-8 BOM exports are accepted. Unrelated cookies are discarded.

Keep `cookie.txt` private and never commit it to source control. Refresh the export when Bilibili invalidates the session.

## Community JavaScript plugins

The repository and release packages contain **no third-party plugins or sample plugins**. Install trusted community plugins separately. Each plugin is one JavaScript file directly inside the runtime `plugins/` directory:

```text
blkt.exe
cookie.txt
plugins/
  welcome.js
```

There is no plugin subdirectory, manifest, package manager, or bundled plugin. The filename without `.js` is the plugin ID; IDs must match `[A-Za-z0-9_-]{1,48}`.

A plugin is not loaded unless explicitly allow-listed:

```powershell
.\blkt.exe --room 123456 --allow-plugin welcome
```

The plugin API:

- `BLKT.on("message", callback)` — receive validated danmaku events.
- `BLKT.on("gift", callback)` — receive validated individual gift events (`SEND_GIFT`).
- `BLKT.on("interaction", callback)` — follows, shares and user interactions.
- `BLKT.on("entry", callback)` — entry effects and welcome events.
- `BLKT.on("like", callback)` — like counter updates.
- `BLKT.on("watched", callback)` — watched-count updates.
- `BLKT.on("notice", callback)` — room notices.
- `BLKT.on("superChat", callback)` — Super Chat messages.
- `BLKT.on("guard", callback)` — guard purchases.
- `BLKT.on("comboGift", callback)` — combo-gift summaries (`COMBO_SEND`), separate from individual gifts.
- `BLKT.on("online", callback)` — online-count updates.
- `BLKT.on("roomStats", callback)` — room online/watched/score statistics.
- `BLKT.giftInfo(giftId)` — query the in-memory gift catalog.
- `BLKT.log(text)` — write a bounded message to the host log.
- `BLKT.sendMessage(text)` — send to the active room, only when separately granted.

Platform events contain `roomId`, `type`, `timestamp`, and normalized scalar fields such as `user`, `uid`, `text`, `count`, `price`, `level`, and `giftName`. Numeric IDs and counters are strings to preserve 64-bit precision in JavaScript. Unknown commands and unrecognized payload fields are not forwarded.

Message sending is denied by default. Grant it explicitly to an already allow-listed plugin:

```powershell
.\blkt.exe --room 123456 --allow-plugin welcome --plugin-send-message welcome
```

Each event dispatch has a shared 250 ms execution budget and a 64-callback cap. Each plugin can register at most 8 handlers per event; at most 16 plugins are loaded. Repeated callback errors or exhausted execution budgets disable the affected plugin.

**Security boundary:** QJSEngine exposes only the BLKT API by default, but this is not an operating-system sandbox. Only install JavaScript from sources you trust. Do not run untrusted plugins with message-send permission.

## Windows portable runtime

The Windows package places the executable and ordinary Qt runtime DLLs directly in `bin/`. The required Qt TLS backend remains in `bin/tls/`; community JavaScript files belong in `bin/plugins/`.

```text
bin/
  blkt.exe
  Qt6Core.dll
  Qt6Network.dll
  Qt6WebSockets.dll
  Qt6Qml.dll
  qt.conf
  tls/
    <Qt TLS backend plugin DLL>
  plugins/
    <community JavaScript files only>
```

Unused Qt Quick/OpenGL runtime components, translations, and unrelated plugin families are excluded where the installed Qt deployment tool supports it.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

GitHub Actions builds and tests Windows x64, Linux x64, macOS Intel, and macOS Apple Silicon packages. CI artifacts are attached to each workflow run.

## Protocol behavior

The client resolves short room IDs to canonical room IDs, obtains WBI-signed danmaku connection information, authenticates the live WebSocket, decodes plain and zlib packets, and maintains live and viewer heartbeats. Protocol v2 is used to retain the zlib-only runtime and avoid adding a Brotli DLL dependency.

Danmaku, individual gifts, and supported room events are validated before entering the core and being dispatched to plugins. Combo-gift summaries use a separate `comboGift` event to avoid double counting. Both the protocol parser and the core apply command, room, type, and field allow-lists. A successful WebSocket authentication does not itself prove that a room has active chat traffic; end-to-end testing requires an actual danmaku message in the room.

## Scope and limitations

- One authenticated Bilibili account and one active room per process.
- No GUI, AI features, database, built-in plugin marketplace, or bundled community plugins.
- Linux packages require Qt 6 runtime libraries on the target system.
- Bilibili may change private/web API behavior without notice; live integration must be verified against an active room and a current account session.
