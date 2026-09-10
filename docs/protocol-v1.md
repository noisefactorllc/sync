# Sync wire protocol v1

This document specifies protocol version 1 as implemented by the Sync native daemon.
Use the browser SDK unless your environment requires a direct protocol implementation.

## Transport and endpoints

The production daemon listens on IPv4 and IPv6 loopback only.
Its default port is 53979.
HTTP and WebSocket traffic use the same port.

| Endpoint | Transport | Purpose |
| --- | --- | --- |
| `/health` | HTTP GET or OPTIONS | Browser-visible health and capabilities |
| `/status` | HTTP GET without `Origin` | Local companion status and active sender count |
| `/pair` | WebSocket text | User-approved token creation |
| `/control` | WebSocket text | Authentication and sender control |
| `/senders/{id}` | WebSocket binary | Frame data for one sender |

The daemon accepts HTTP/1.1 headers up to 16 KiB.
It closes a connection if the complete header does not arrive in one second.
The daemon requires a valid loopback `Host` header.

## Origins

Each browser WebSocket upgrade requires an `Origin` header.
The daemon normalizes the origin before it uses an approved token or sender ticket.
An origin input can contain at most 1024 bytes.
The canonical result can contain at most 512 bytes.

The daemon accepts these origin forms:

- `https://` with a canonical DNS host, IPv4 literal, or IPv6 literal
- `http://` only with a loopback address or `localhost`
- `app://` with one lowercase canonical DNS host and no port

An origin cannot contain credentials, a path, a query, a fragment, a backslash, or a zone identifier.
Origin text must contain printable ASCII characters only.
The daemon rejects ambiguous numeric hosts and every `xn--` internationalized label.
It converts HTTP and HTTPS schemes and DNS names to lowercase.
It removes ports 80 and 443 from matching schemes.
An `app://` origin must already be lowercase and canonical.

The `/health` endpoint accepts a valid browser origin and returns that value in `Access-Control-Allow-Origin`.
Its OPTIONS response allows GET.
It also returns `Access-Control-Allow-Private-Network: true` when the preflight requests private network access.
The `/status` endpoint rejects requests that contain `Origin`.

## Health and capabilities

`GET /health` returns HTTP 200 with this JSON shape:

```json
{
  "product": "Sync",
  "status": "ok",
  "version": "0.2.0",
  "protocolVersions": [1],
  "instanceId": "example_instance",
  "capabilities": {
    "send": true,
    "receive": false,
    "providers": [
      {
        "id": "syphon",
        "direction": "send",
        "available": true,
        "selected": true
      }
    ]
  }
}
```

The product version is printable text with a maximum of 64 bytes.
The instance ID contains 1 through 128 ASCII letters, digits, underscores, or hyphens.
The protocol list contains 1 through 16 unique positive 16-bit versions.

The provider list contains at most four entries.
A provider ID contains 1 through 32 bytes.
`direction` is `send` or `receive`.
`available` states whether the runtime can use the provider.
`selected` states whether the daemon configuration selected the provider.

`capabilities.send` is true when a selected send provider is available.
`capabilities.receive` applies the same rule to receive providers.

`GET /status` returns the same fields and adds `activeSenders`.
The local companion uses this endpoint without browser CORS access.

## Pairing

Pairing uses a credential-free WebSocket at `/pair`.
The upgrade must not request a WebSocket subprotocol.
Production mode enables this endpoint only when a native pairing prompt is available.

The client must send one text message within one second:

```json
{
  "type": "pair",
  "protocolVersions": [1],
  "name": "My visual app"
}
```

The message can contain at most 1024 bytes.
It must contain exactly the three fields shown above.
The protocol list contains 1 through 8 unique unsigned 16-bit integers and must include version 1.

The application name contains 1 through 64 UTF-8 bytes.
It cannot contain control characters or Unicode formatting characters.
The native prompt displays this name and the normalized origin.

After approval, the daemon returns one text message:

```json
{
  "type": "paired",
  "protocolVersion": 1,
  "token": "64 lowercase hexadecimal characters"
}
```

The token is a bearer credential for the exact normalized origin.
The client must keep it out of URLs, logs, and WebSocket subprotocols.
The successful pairing socket then closes with code 1000.

Pairing failures use the common error shape and then close with code 1008.
The daemon can return these pairing error codes:

| Code | Meaning |
| --- | --- |
| `bad_request` | The request contains malformed data or an unsupported version. |
| `request_too_large` | The request exceeds 1024 bytes. |
| `duplicate_request` | The socket sent more than one request. |
| `pairing_denied` | The user denied the request. |
| `pairing_timeout` | The request, prompt, or token operation timed out. |
| `pairing_cooldown` | A recent prompt started the global 30-second cooldown. |
| `prompt_saturated` | Another prompt is active or the prompt cannot start. |
| `authority_saturated` | The bounded credential worker cannot accept the operation. |
| `prompt_failure` | The native prompt failed. |
| `store_failure` | The daemon could not commit the token. |
| `store_durability_uncertain` | The commit completed without a durability confirmation. |
| `origin_limit` | The pairing store reached its 64-origin limit. |
| `internal_error` | An internal operation failed. |

The production prompt deadline is 30 seconds.
Each completed or interrupted prompt starts a global 30-second cooldown.

## Control connection

Connect to `/control` without a WebSocket subprotocol.
Send `hello` as the first text message within one second:

```json
{
  "type": "hello",
  "token": "approved bearer token",
  "protocolVersions": [1]
}
```

The token contains 1 through 256 printable ASCII bytes.
The version list contains at most 16 unique unsigned 16-bit integers and must include version 1.
The complete control message can contain at most 16 KiB.

The daemon checks the token against the normalized WebSocket origin.
An invalid token or incompatible version returns `authentication_failed`.
The daemon then closes the socket with code 1008.

A successful hello returns:

```json
{
  "type": "welcome",
  "protocolVersion": 1,
  "version": "0.2.0",
  "instanceId": "example_instance",
  "capabilities": {
    "send": false,
    "receive": false,
    "providers": []
  }
}
```

The welcome capability object has the same rules as the health response.
Only authenticated control sockets can create, inspect, or close senders.

### Create a sender

Send:

```json
{"type":"createSender","name":"My output"}
```

The name contains 1 through 64 UTF-8 bytes.
It follows the same character rules as the pairing name.
A control connection cannot own two live senders with the same name.
The daemon supports at most 64 live senders.

The daemon returns:

```json
{
  "type": "senderCreated",
  "id": "32 lowercase hexadecimal characters",
  "name": "My output",
  "path": "/senders/32-lowercase-hexadecimal-characters",
  "ticket": "32 lowercase hexadecimal characters"
}
```

The current daemon creates a 32-character lowercase hexadecimal ID and ticket.
A compatible client accepts an ID up to 128 bytes and a ticket from 32 through 128 bytes.

### Read sender statistics

Send:

```json
{"type":"getStats","senderId":"sender-id"}
```

The daemon returns each unsigned 64-bit value as an unquoted JSON decimal integer:

```json
{
  "type": "stats",
  "id": "sender-id",
  "accepted": 120,
  "dropped": 3,
  "rejected": 0,
  "failed": 0,
  "lastSequence": 123,
  "lastPresentationTimeUs": 1780000123456,
  "checksum": 1469598103934665603
}
```

`accepted` counts frames that the native providers accepted.
`dropped` counts stale frames and provider backpressure.
`rejected` counts malformed frames.
`failed` counts provider failures.
The last sequence and presentation time describe the newest valid frame.

The checksum is an unsigned 64-bit diagnostic value.
The JavaScript SDK parses the decimal text before JSON can lose integer precision.
It returns the checksum as exactly 16 lowercase hexadecimal digits.
The SDK rejects counters that exceed the JavaScript safe-integer limit.

### Close a sender

Send:

```json
{"type":"closeSender","senderId":"sender-id"}
```

The daemon removes the sender and returns:

```json
{"type":"senderClosed","id":"sender-id"}
```

This operation also succeeds when the same owner closes a sender that its data socket already reaped.
Other missing senders return `sender_not_found`.

### Control errors

All control errors use this shape:

```json
{"type":"error","code":"bad_request","message":"Malformed control message"}
```

The daemon can return these control error codes:

- `authentication_failed`
- `bad_request`
- `out_of_order`
- `publisher_unavailable`
- `duplicate_sender`
- `sender_limit`
- `sender_not_found`
- `internal_error`

The daemon keeps an authenticated control connection open after an ordinary command error.
An authentication error closes the connection.

## Sender data connection

Open the `path` from `senderCreated` on the same daemon origin.
Supply exactly one WebSocket subprotocol:

```text
sync.sender.{ticket}
```

The ticket is a one-use bearer credential.
The data socket origin must equal the control socket's normalized origin.
The owner control socket must remain open.
Only one data socket can attach to a sender.
Do not put the ticket in the URL.

After the upgrade, send one complete binary WebSocket message for each frame.
The maximum message size is 64 MiB plus the 64-byte header.
The daemon can retain at most 128 MiB of inbound data payloads across connections.
An incomplete data message has a two-second deadline.

## Binary frame

All integer fields use little-endian byte order.
The payload starts immediately after the 64-byte header.

| Offset | Bytes | Type | Field | Required value |
| ---: | ---: | --- | --- | --- |
| 0 | 4 | `uint32` | Magic | `0x434e5953`, bytes `53 59 4e 43` (`SYNC`) |
| 4 | 2 | `uint16` | Version | `1` |
| 6 | 2 | `uint16` | Header size | `64` |
| 8 | 4 | `uint32` | Flags | `1`, top-down rows |
| 12 | 2 | `uint16` | Pixel format | `1`, RGBA8 unorm |
| 14 | 2 | `uint16` | Color space | `1` or `2` |
| 16 | 2 | `uint16` | Alpha mode | `1`, `2`, or `3` |
| 18 | 2 | `uint16` | Reserved | `0` |
| 20 | 4 | `uint32` | Width | 1 through 4096 |
| 24 | 4 | `uint32` | Height | 1 through 4096 |
| 28 | 4 | `uint32` | Row stride | At least `width * 4` |
| 32 | 4 | `uint32` | Payload bytes | Exactly `rowStride * height` |
| 36 | 8 | `uint64` | Sequence | Monotonic sender sequence |
| 44 | 8 | `uint64` | Presentation time | Microseconds on the sender clock |
| 52 | 12 | bytes | Reserved | All zero |

Pixel-format enum:

| Value | Meaning |
| ---: | --- |
| 1 | Four 8-bit unorm channels in RGBA order |

Color-space enum:

| Value | Meaning |
| ---: | --- |
| 1 | sRGB |
| 2 | Display P3 |

Alpha-mode enum:

| Value | Meaning |
| ---: | --- |
| 1 | Opaque |
| 2 | Straight alpha |
| 3 | Premultiplied alpha |

The first payload row is the top image row.
Each row starts with `width` RGBA pixels and can end with stride padding.
The payload cannot exceed 64 MiB.
The complete WebSocket message must equal `64 + payloadBytes` bytes.

The receiver drops a sequence value that is not greater than the previous valid sequence.
It records the latest sequence and presentation time before it calls the providers.

## Lifecycle

The authenticated control connection owns every sender that it creates.
Closing that connection removes all its senders and closes their data sockets with code 1000.
Closing a sender removes its native provider resources and closes its data socket with code 1000.
Closing a data socket also reaps its sender.

The browser SDK serializes control requests.
It closes a partially created sender when the data-socket upgrade fails.
It also closes the sender after an unexpected data-socket end.

The sender source remains owned by the caller.
The sender queue remains owned by the sender until `close()`.
The queue callback borrows the output bytes until the callback returns.

## WebSocket close codes

The daemon emits these WebSocket close codes:

| Code | Use |
| ---: | --- |
| 1000 | Normal pairing, sender, or control-owned sender cleanup |
| 1002 | WebSocket framing error or malformed binary frame |
| 1003 | Wrong text or binary opcode for the endpoint |
| 1007 | Invalid UTF-8 text payload |
| 1008 | Authentication, pairing policy, hello timeout, or incomplete-frame timeout |
| 1011 | A native provider failed while it published a frame |
| 1013 | The inbound payload has no remaining budget |

Code 1008 can use the reason `incomplete_frame_timeout`.
Code 1013 uses the reason `inbound_budget_exhausted`.
Other protocol close frames usually have an empty reason.

The daemon allows 750 milliseconds for a WebSocket close handshake or final write.
It can close the TCP connection without a close frame after that deadline.

## Version compatibility

Pairing and hello messages list the protocol versions that the client supports.
Protocol v1 requires both peers to select version 1.
The welcome and paired responses state the selected version.

Frame headers do not negotiate a version.
A v1 receiver rejects any frame whose header version or size differs from v1.
Future senders must use a negotiated future control version before they send a different frame format.

The SDK version and daemon product version are independent.
Use `SYNC_SDK_VERSION` for SDK diagnostics.
Use `welcome.version` or the health response for the daemon product version.
