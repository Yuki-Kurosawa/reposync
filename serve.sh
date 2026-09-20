#!/usr/bin/env bash
# Serve a local Debian-style repo root over HTTP (python3, PSF license).
# Usage: serve.sh <repo_root> [port]
# Both reposync (sources.list.d / target.sources) and apt can point at it,
# exactly like a real remote repository.
set -u
cd "$(dirname "$0")"
ROOT=${1:?usage: serve.sh <repo_root> [port]}
PORT=${2:-8931}
exec python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$ROOT"
