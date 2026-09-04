FROM ubuntu:24.04

RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config libssl-dev libuv1-dev \
    nodejs npm dpkg-dev shellcheck ca-certificates file procps \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /src
