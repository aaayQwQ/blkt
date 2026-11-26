# BLKT Plugin Worker Protocol v1

This document describes the process boundary used by BLKT V1.

## Process layout

- Windows worker executable suffix: `.exe`
- Linux/macOS worker executable suffix: `.blkt-plugin`
- Workers are discovered only in the application's `plugins/` directory.
- The host starts the executable directly with `--blkt-plugin-worker --blkt-protocol=1`; no shell is used.
- The host and worker communicate through standard input and standard output.
- Standard output is reserved for protocol packets. Diagnostic output must go to standard error.
- The host does not pass Cookie, CSRF, or the host's complete environment to a worker.

## Framing

Each packet is one UTF-8 JSON object followed by one LF byte.

Limits enforced by the host:

- Maximum packet size: 64 KiB.
- Maximum request rate: 30 requests per worker per second.
- Request ID length: 1–128 characters.
- Worker executable size: at most 256 MiB.

A malformed packet, invalid startup handshake, or excessive output causes the worker to be stopped.

## Startup

The host sends:

```json
{
  "type": "initialize",
  "protocol": 1,
  "plugin_id": "example",
  "permissions": ["receive_message", "receive_gift"]
}
```

The worker must reply:

```json
{
  "type": "ready",
  "protocol": 1,
  "plugin_id": "example"
}
```

The plugin ID must exactly match the executable filename without its platform suffix. The worker has five seconds to complete this handshake.

## Events

Message event:

```json
{
  "type": "event",
  "event": "message",
  "room_id": "123456",
  "user": "viewer",
  "text": "hello",
  "timestamp": 1730000000
}
```

Gift event:

```json
{
  "type": "event",
  "event": "gift",
  "room_id": "123456",
  "user": "viewer",
  "gift_name": "Rose",
  "gift_id": "1",
  "count": 1,
  "total_coin": "100",
  "coin_type": "gold",
  "timestamp": 1730000000
}
```

The host sends only events covered by the worker's granted permissions.

## Capability requests

Send a danmaku:

```json
{
  "type": "request",
  "id": "1",
  "operation": "send_message",
  "payload": {
    "room_id": "123456",
    "text": "hello"
  }
}
```

Read gift metadata:

```json
{
  "type": "request",
  "id": "2",
  "operation": "gift_info",
  "payload": {
    "gift_id": "1"
  }
}
```

The `room_id` must match the currently connected room. The host rejects attempts to send to another room.

The host replies with:

```json
{
  "type": "response",
  "id": "1",
  "ok": true
}
```

A successful gift lookup also contains a `result` object with `id`, `name`, `price`, and `coin_type`. Unauthorized or invalid operations return `ok: false`.

Permissions:

| Permission | Capability |
|---|---|
| `receive_message` | Receive danmaku events |
| `send_message` | Request danmaku sending |
| `receive_gift` | Receive gift events |
| `gift_info` | Query the in-memory gift catalog |

Permissions default to none and are not persisted between application runs.

## Shutdown

The host sends:

```json
{ "type": "shutdown" }
```

The worker should exit promptly. The host waits for a bounded grace period, then terminates or kills the process.

## Security boundary

A worker never receives a host object pointer, Cookie, CSRF token, or database handle. Capability requests are validated by the host.

**Important:** process separation is not an operating-system sandbox. In this initial implementation, a worker runs under the same OS user and may still access files and network resources that user can access. SHA-256 trust records identify the approved executable bytes; they do not prove that the program is safe. Only run plugins from sources you trust. OS-level sandbox enforcement remains a required security milestone.

The host itself does not write runtime caches, configuration, database, or log files. Plugin authors are expected to keep plugin state in memory and not write caches to disk; this rule is not yet technically enforceable against a malicious worker.
