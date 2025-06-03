#!/usr/bin/env bash
set -euo pipefail
pkill -9 -f '[r]amforge' 2>/dev/null || true
sleep 0.3

echo "▶️  hard-kill test"

ROOT="$( cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." &>/dev/null && pwd )"
BIN="$ROOT/ramforge"; [[ -x "$BIN" ]] || { echo "no bin"; exit 1; }

rm -f append.aof dump.rdb

setsid "$BIN" --aof always --workers 1 2>/dev/null &
PGID=$!
sleep 0.5

curl -s -XPOST -d '{"id":1,"name":"neo"}' -H "Content-Type: application/json" \
     http://localhost:1109/users >/dev/null

kill -9 -"$PGID"                       # whole process-group

setsid "$BIN" --aof always --workers 1 2>/dev/null &
PGID2=$!
sleep 0.5

BODY=$(curl -s http://localhost:1109/users/1)
kill -9 -"$PGID2"

[[ "$BODY" == *'"neo"'* ]] && echo "✅ hard-kill passed" \
                           || { echo "❌ hard-kill failed"; exit 1; }
