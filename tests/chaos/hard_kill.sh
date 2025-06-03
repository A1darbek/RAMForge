#!/usr/bin/env bash
set -euo pipefail
echo "▶️  hard-kill test"
rm -f append.aof dump.rdb

./ramforge --aof always --workers 1 &
PID=$!
sleep 0.5

curl -s -XPOST -d '{"id":1,"name":"neo"}' \
     -H "Content-Type: application/json" \
     http://localhost:1109/users > /dev/null

kill -9 $PID            # simulate power-loss
./ramforge --aof always --workers 1 &
PID2=$!
sleep 0.5

BODY=$(curl -s http://localhost:1109/users/1)
kill $PID2
[[ "$BODY" == *'"neo"'* ]] && echo "✅ hard-kill passed" || { echo "❌ hard-kill failed"; exit 1; }
