# Contributing to Sync

Thanks for your interest in contributing.

Contributions follow the Noise Factor
[contributing policy](https://github.com/noisefactorllc/.github/blob/main/CONTRIBUTING.md) and
[Code of Conduct](https://github.com/noisefactorllc/.github/blob/main/CODE_OF_CONDUCT.md). The policy covers which pull requests we
accept and what LLM-assisted pull requests need to include. This page adds
what's specific to Sync.

## Getting set up

Sync requires CMake 3.21 or newer, a C++20 compiler, OpenSSL 3, libuv, and
pkg-config. On macOS, the native publisher also uses the system Foundation and
Metal frameworks.

```bash
cmake -S . -B build
cmake --build build -j4
ctest --test-dir build --output-on-failure
npm run test:unit
SYNC_DAEMON_PATH=build/syncd npm run test:integration
```

Keep changes focused and include regression coverage for behavior changes.
Never commit pairing tokens, credential stores, private keys, or locally built
binaries.

## Reporting issues

Open a GitHub issue with the operating system, browser or native host, Sync
commit, and exact reproduction steps. Report suspected vulnerabilities
privately using [SECURITY.md](SECURITY.md).
