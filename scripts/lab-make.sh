#!/usr/bin/env bash
# Run make inside the container, e.g.  ./scripts/lab-make.sh -C lab2-cache check
set -euo pipefail
cd "$(dirname "$0")/.."
docker compose run --rm lab make "$@"
