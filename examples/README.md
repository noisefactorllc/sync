# Sync interoperability examples

The browser example sends one animated source through Canvas 2D, WebGL2, or WebGPU.
It uses the modules in `browser/` directly.

First, build and run the Sync daemon from the same source checkout.
The [developer guide](../docs/developers.md) also links published companion downloads.
A published build can differ from this local SDK candidate until a compatible release ships.

Start the loopback server from the repository root:

```bash
node examples/server.mjs
```

The command prints the bound URL. Open that URL in a supported browser.
Set `PORT=0` to request an available port.

Click **Connect** to start pairing. Sync shows the app name and the page origin.
The example keeps the returned token in memory only. It starts the output after approval.

You can select a different daemon endpoint for local development:

```text
http://127.0.0.1:4179/?endpoint=http://127.0.0.1:54000
```

The endpoint must be an explicit IPv4 or IPv6 loopback URL. Do not put a token in the URL.

The renderer owns its animation loop and source resources. The sender owns its export queue.
Stop releases the sender, queue, socket, and renderer resources. Restart also performs this cleanup.

The Electron wrapper uses the same files from the `app://com.example.visualizer` origin.
Install its local development dependency, and then start it:

```bash
cd examples/electron
npm install
npm start
```

The wrapper grants local network access and sanitized clipboard writes only to its exact app origin.
It does not grant clipboard reads or other permissions.
