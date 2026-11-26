# BLKT

BLKT is a lightweight, headless, single-account Bilibili live server built with Qt 6 and C++17.

## Start

Place a pre-existing `cookie.txt` beside the executable before launch.

Windows:
```powershell
.\blkt.exe --room 123456
```

Linux:
```sh
./blkt --room 123456
```

Use Ctrl+C or SIGTERM for graceful shutdown. Logs go to stdout. BLKT does not create log, cache, database, config, or temporary files. Windows may independently create Prefetch records; applications cannot prevent OS-managed execution telemetry.

## Community JavaScript plugins

The BLKT repository and release packages contain **no plugins or example plugins**. Plugins are community-maintained and installed separately.

Put each community plugin directly in the runtime `plugins/` directory as exactly one JavaScript file:

```text
blkt.exe
cookie.txt
plugins/
  welcome.js
  gifts.js
```

There is no plugin subdirectory, manifest, JSON metadata, package manager, or bundled sample. The filename without `.js` is the plugin ID. Names must match `[A-Za-z0-9_-]{1,48}`.

A plugin is not loaded unless explicitly allow-listed:

```powershell
.\blkt.exe --room 123456 --allow-plugin welcome
```

The JavaScript API is intentionally small:

- `BLKT.on("message", callback)` — receive validated chat events.
- `BLKT.on("gift", callback)` — receive validated gift events.
- `BLKT.giftInfo(giftId)` — read the in-memory gift catalog.
- `BLKT.log(text)` — write a bounded message to the host log.
- `BLKT.sendMessage(text)` — send to the currently connected room, only when separately granted.

Sending messages is denied by default. Explicitly grant it to a plugin that is already allow-listed:

```powershell
.\blkt.exe --room 123456 --allow-plugin welcome --plugin-send-message welcome
```

Each event dispatch has a shared 250 ms execution budget and a 64-callback cap. A plugin can register at most 8 handlers for each event. A plugin that exhausts the event time budget is disabled.

Community plugin authors should distribute their own single-file `.js` packages separately; BLKT does not ship plugin source or examples.

## Credentials

Cookie is read-only and must be provisioned before startup. It is not included in source or release artifacts. For Netscape exports, BLKT imports only `SESSDATA`, `bili_jct`, and `buvid3` from the shared Bilibili domain at the root path when the entry is not expired. Other cookie names and host-only or path-scoped entries are ignored. Raw Cookie headers are filtered to the same three names. All authenticated endpoints use HTTPS/WSS.

## Windows portable runtime layout

The Windows package keeps the executable and ordinary Qt runtime DLLs directly in `bin/`. Unused Qt Quick/OpenGL runtime components, translations, and unrelated plugin families are excluded.

```text
bin/
  blkt.exe
  Qt6Core.dll
  Qt6Network.dll
  Qt6WebSockets.dll
  Qt6Qml.dll
  ...
  qt.conf
  tls/
    <Qt TLS backend plugin DLL>
  plugins/
    <community JavaScript files only>
```

Qt requires dynamically loaded plugins to remain in a plugin-type subdirectory. Therefore the required TLS backend stays in `bin/tls/`; the generated Qt runtime plugin tree is removed. The top-level `bin/plugins/` directory is reserved for community JavaScript files and contains no Qt runtime plugin DLLs.
