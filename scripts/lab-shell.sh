#!/usr/bin/env bash
# Open a shell in the lab container (macOS / any Docker host).
set -euo pipefail
cd "$(dirname "$0")/.."
docker compose run --rm lab "$@"
