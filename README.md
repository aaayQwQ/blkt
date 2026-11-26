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

A minimal community plugin:

```javascript
"use strict";

BLKT.on("message", function (event) {
    BLKT.log(event.user + ": " + event.text);
});

BLKT.on("gift", function (event) {
    BLKT.log(event.user + " sent " + event.giftName);
});
```

Scripts use the ECMAScript implementation provided by Qt 6 QJSEngine. Node.js APIs such as `require`, `process`, and filesystem access are not provided. The host exposes no arbitrary HTTP, filesystem, process, shell, or account-cookie API to scripts. Message sending is an explicit capability and is limited to the active room; the core also applies its own validation and rate limit.

Each file is limited to 512 KiB, at most 16 scripts are loaded, and each synchronous evaluation/callback has a 250 ms execution deadline. Repeated callback failures disable that plugin. QJSEngine runs scripts in the host process and is **not an OS security sandbox** or a hard memory quota. Only install code you trust.

## Credentials

Cookie is read-only and must be provisioned before startup. It is not included in source or release artifacts.
