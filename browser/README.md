# Sync browser client

`SyncBridgeClient` discovers and connects to the local Sync companion at
`http://127.0.0.1:53979` by default. An explicit IPv4 or IPv6 loopback endpoint
may be supplied for development and tests.

Passive `probe()` and `connect()` calls never initiate pairing. On browsers
with loopback permission discovery, they query `loopback-network` and report a
typed permission-required or permission-denied result before making a network
request. The client queries the legacy `local-network-access` descriptor only
when `loopback-network` is genuinely unsupported. If neither descriptor exists,
the bounded request remains the graceful compatibility path; an absent daemon
is still reported as unavailable rather than treated as an application error.

Call `pair(name)` only from a deliberate user action such as a click. The
explicit health request may produce the browser's loopback-network prompt, and
the native companion then shows a separate per-origin pairing prompt. The SDK
returns the resulting token but does not store it, log it, add it to a URL or
subprotocol, update the client's configured token, retry, or connect
automatically. The host application owns secure token persistence and creates
a new client with that token:

```js
import { SyncBridgeClient } from './browser/index.js';

pairButton.addEventListener('click', async () => {
  const pairingClient = new SyncBridgeClient();
  const { token } = await pairingClient.pair('Noisedeck');
  pairingClient.close();

  // Persist `token` using the host application's secure credential policy.
  const sync = new SyncBridgeClient({ token });
  await sync.connect();
});
```

## Permissions Policy

Top-level applications should send this HTTP response header:

```http
Permissions-Policy: loopback-network=(self)
```

An embedding page must also delegate the feature to the intended frame:

```html
<iframe src="https://visuals.example/app" allow="loopback-network"></iframe>
```

Without top-level policy and iframe delegation, an embedded client may be
unable to reach the companion even after user interaction. The legacy
permission name is compatibility fallback behavior in the SDK, not the primary
policy contract.
