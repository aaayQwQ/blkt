# BLKT

BLKT is a headless, single-account Bilibili live server using Qt 6 and C++17.

## Start

Put a pre-existing `cookie.txt` beside the executable before launch.

Windows:
```powershell
.\blkt.exe --room 123456
.\blkt.exe --room 123456 --allow-plugin example
```

Linux:
```sh
./blkt --room 123456 --allow-plugin example
```

Use Ctrl+C or SIGTERM for graceful shutdown. Logs go to stdout. BLKT does not create log, cache, database, config, or temporary files. Windows may independently create Prefetch records; applications cannot prevent OS-managed execution telemetry.

## Script plugins

A plugin is a directory under `plugins/` with `manifest.json` and `main.js`. It is disabled unless explicitly allowed with repeatable `--allow-plugin <id>`. Manifest permissions are host-enforced capabilities.

```json
{"id":"example","name":"Example","version":"1.0.0","api_version":1,"permissions":["receive_message","receive_gift"],"entry":"main.js"}
```

API: `BLKT.on("message", callback)`, `BLKT.on("gift", callback)`, `BLKT.sendMessage(roomId,text)`, `BLKT.giftInfo(giftId)`, `BLKT.log(text)`.

The embedded QJSEngine is not an OS sandbox. Each synchronous script evaluation/callback has a 250 ms execution deadline; repeated callback failures disable that plugin, and at most 16 plugins are loaded. This prevents simple infinite-loop scripts from permanently blocking the host, but it is not a hard CPU or memory quota. Scripts still execute in the host process. Only run code you trust. No Node.js `require`, process or filesystem API is exposed by default. Do not treat this as a hard security boundary.

## Credentials

Cookie is read-only and must be provisioned before startup. It is not included in release artifacts.
