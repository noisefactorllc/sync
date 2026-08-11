# Contributing to Sync

Thanks for your interest in contributing.

## Getting set up

Sync requires CMake 3.20 or newer, a C++20 compiler, OpenSSL 3, libuv, and
pkg-config. On macOS, the native publisher also uses the system Foundation and
Metal frameworks.

```bash
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
npm run test:browser
SYNC_DAEMON_PATH=build/syncd npm run test:integration
```

Keep changes focused and include regression coverage for behavior changes.
Never commit pairing tokens, credential stores, private keys, or locally built
binaries.

## Reporting issues

Open a GitHub issue with the operating system, browser or native host, Sync
commit, and exact reproduction steps. Report suspected vulnerabilities
privately using [SECURITY.md](SECURITY.md).
