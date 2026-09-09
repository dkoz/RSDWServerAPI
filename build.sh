#!/usr/bin/env bash
# Builds dist/librsdwapi.so in a Debian container, so the result matches the
# dedicated server's own runtime rather than whatever the host happens to be.
set -euo pipefail

cd "$(dirname "$0")"

IMAGE=rsdwapi-builder:debian

docker build -q -f Dockerfile.build -t "$IMAGE" . >/dev/null
docker run --rm -v "$PWD:/src" -w /src "$IMAGE" make "$@"

echo
echo "Built dist/librsdwapi.so"
docker run --rm -v "$PWD:/src" -w /src "$IMAGE" bash -c \
  "objdump -T dist/librsdwapi.so | grep -oE 'GLIBC(XX)?_[0-9.]+' | sort -Vu | tail -5"
