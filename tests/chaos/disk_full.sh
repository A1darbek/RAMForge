#!/usr/bin/env bash
set -euo pipefail
echo "▶️  disk-full test"

# ── locate compiled binary regardless of spaces in path ──
ROOT="$( cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." &>/dev/null && pwd )"
BIN="$ROOT/ramforge"; [[ -x "$BIN" ]] || { echo "no bin"; exit 1; }

# ── prepare 64-MB ext4 FS with *zero* reserved blocks ──
WORK=$(mktemp -d)
truncate -s 64M  "$WORK/vol.img"
mkfs.ext4 -F -m 0 "$WORK/vol.img" > /dev/null
mkdir "$WORK/mnt"
sudo mount -o loop "$WORK/vol.img" "$WORK/mnt"

pushd "$WORK/mnt" >/dev/null                  #  /…/mnt
sudo mkdir data
sudo chown "$(id -u):$(id -g)" data
cd data                                       #  /…/mnt/data

# ── start server (unprivileged) in its own process-group ──
setsid "$BIN" --aof always --workers 1 2>/dev/null &
PGID=$!
sleep 0.5

# ── exhaust almost all space ──
sudo dd if=/dev/zero of=filler.bin bs=1M count=55 || true   # expect ENOSPC

# ── attempt a write ──
STATUS=$(curl -s -o /dev/null -w '%{http_code}' \
         -XPOST -d '{"id":2,"name":"trinity"}' \
         -H "Content-Type: application/json" \
         http://localhost:1109/users || true)

# ── kill *all* ramforge processes, wait a beat ──
kill -9 -"$PGID" 2>/dev/null || true
pkill -9 -f '[r]amforge'    2>/dev/null || true
sleep 0.4

popd >/dev/null                               # leave mount
sudo umount "$WORK/mnt"
rm -rf "$WORK"

EXPECTED="503"
[[ "$STATUS" == "$EXPECTED" ]] && echo "✅ disk-full passed" \
                               || { echo "❌ disk-full failed (got $STATUS)"; exit 1; }
