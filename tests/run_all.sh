#!/usr/bin/env bash
# Full host-verifiable acceptance check (no display / SDL required).
set -euo pipefail
cd "$(dirname "$0")/.."

echo "== C control core (layout / versioned commands / button FSM / policy / exit / journal) =="
make check

echo
echo "== Python backend (usable-area validation / draft conflict / versions / idempotency / recovery) =="
python3 server/server.py --self-test

echo
echo "All host acceptance checks passed."
