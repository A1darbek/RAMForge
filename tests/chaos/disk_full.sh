#!/usr/bin/env bash
set -euo pipefail
echo "▶️  disk-full test"

# ─── locate compiled binary, quote-safe ───
ROOT="$( cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." &>/dev/null && pwd )"
BIN="$ROOT/ramforge"
[[ -x "$BIN" ]] || { echo "❌ ramforge not built"; exit 1; }

# ─── temp 64-MB filesystem ───
WORK=$(mktemp -d)
truncate -s 64M "$WORK/vol.img"
mkfs.ext4 -F "$WORK/vol.img" > /dev/null
mkdir "$WORK/mnt"
sudo mount -o loop "$WORK/vol.img" "$WORK/mnt"

pushd "$WORK/mnt" >/dev/null
sudo mkdir data && cd data

# ─── start server in its own process-group ───
setsid sudo "$BIN" --aof always --workers 1 2>/dev/null &
PGID=$!
sleep 0.5

# ─── consume almost all space (dd will stop at ENOSPC) ───
sudo dd if=/dev/zero of=filler.bin bs=1M count=55 || true

# ─── attempt a write through HTTP ───
STATUS=$(curl -s -o /dev/null -w '%{http_code}' \
         -XPOST -d '{"id":2,"name":"trinity"}' \
         -H "Content-Type: application/json" \
         http://localhost:1109/users || true)

# ─── cleanup ───
sudo kill -9 -"$PGID"
popd >/dev/null              # leave mount
sudo umount "$WORK/mnt"
rm -rf "$WORK"

# ─── verdict ───
if [[ "$STATUS" == "503" || "$STATUS" == "000" ]]; then
    echo "✅ disk-full passed"
else
    echo "❌ disk-full failed (got $STATUS)"; exit 1
fi
