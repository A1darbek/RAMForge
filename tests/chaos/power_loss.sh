#!/usr/bin/env bash
set -euo pipefail
echo "▶️  power-loss / torn-write test"

# ─── skip when loop / dm not available or not permitted ───
if ! command -v losetup >/dev/null || ! command -v dmsetup >/dev/null; then
    echo "⚠️  loop/dm unavailable — skipping test"
    exit 0
fi

PROBE=$(mktemp)
truncate -s 1M "$PROBE"
if ! sudo losetup -f --show "$PROBE" &>/dev/null; then
    echo "⚠️  no permission for losetup — skipping test"
    rm -f "$PROBE"
    exit 0
fi
sudo losetup -D               # detach probe loop dev
rm -f "$PROBE"

# ─── locate compiled binary (quote safe) ───
ROOT="$( cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." &>/dev/null && pwd )"
BIN="$ROOT/ramforge"; [[ -x "$BIN" ]] || { echo "no ramforge binary"; exit 1; }

# ─── tmp resources ───
TMP=$(mktemp -d)
truncate -s 128M "$TMP/aof.img"

# create loop device backed by the file
LOOP=$(sudo losetup -f --show "$TMP/aof.img")

# linear dm-mapper target over the loop dev
sudo dmsetup create flk --table "0 $(blockdev --getsz "$LOOP") linear $LOOP 0"
DEVICE=/dev/mapper/flk

mkfs.ext4 -F "$DEVICE" > /dev/null
mkdir "$TMP/mnt"
sudo mount "$DEVICE" "$TMP/mnt"
pushd "$TMP/mnt" >/dev/null

# ─── start server (unprivileged) in its own PG ───
setsid "$BIN" --aof always --workers 1 2>/dev/null &
PGID=$!
sleep 0.5

# write one record
curl -s -XPOST -d '{"id":7,"name":"smith"}' \
     -H "Content-Type: application/json" \
     http://localhost:1109/users >/dev/null

# simulate torn write: drop writes for 2 s
sudo dmsetup message flk 0 "drop_writes 1"
sleep 2
sudo dmsetup message flk 0 "drop_writes 0"

kill -9 -"$PGID"          # kill parent + worker

# ─── reboot attempt ───
set +e
"$BIN" --aof always --workers 1
RC=$?
set -e

# ─── cleanup ───
popd >/dev/null
sudo umount "$TMP/mnt"
sudo dmsetup remove flk
sudo losetup -d "$LOOP"
rm -rf "$TMP"

if [[ $RC -eq 2 ]]; then
    echo "✅ power-loss passed (refused to start on corrupt AOF)"
    exit 0
else
    echo "❌ power-loss failed (exit $RC)"
    exit 1
fi
