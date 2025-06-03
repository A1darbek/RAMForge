#!/usr/bin/env bash
set -euo pipefail
echo "▶️  power-loss / torn-write test"

# ─── skip if loop+dm not available ───
if ! command -v losetup >/dev/null || ! command -v dmsetup >/dev/null; then
    echo "⚠️  loop/dm unavailable — skipping test (not a failure)"
    exit 0
fi
if ! sudo losetup -f --show /dev/null &>/dev/null; then
    echo "⚠️  no permission for losetup — skipping test"
    exit 0
fi
sudo losetup -D

# create a tmpfs backed loop-device
TMP=$(mktemp -d)
truncate -s 128M "$TMP/aof.img"
LOOP=$(sudo losetup -f --show "$TMP/aof.img")
sudo dmsetup create flk --table "0 $(blockdev --getsz $LOOP) linear $LOOP 0"
DEVICE=/dev/mapper/flk
mkfs.ext4 -F "$DEVICE" > /dev/null
mkdir "$TMP/mnt"
sudo mount "$DEVICE" "$TMP/mnt"
pushd "$TMP/mnt" >/dev/null

sudo ../../ramforge --aof always --workers 1 &
PID=$!
sleep 0.5

curl -s -XPOST -d '{"id":7,"name":"smith"}' \
     -H "Content-Type: application/json" \
     http://localhost:1109/users > /dev/null

# enable flakey drop (simulate lost sector for next 2s)
sudo dmsetup message flk 0 "drop_writes 1"
sleep 2
sudo dmsetup message flk 0 "drop_writes 0"

sudo kill -9 $PID

set +e
sudo ../../ramforge --aof always --workers 1
RC=$?
set -e

sudo umount "$TMP/mnt"; sudo dmsetup remove flk; sudo losetup -d "$LOOP"; rm -rf "$TMP"

if [[ $RC -eq 2 ]]; then
    echo "✅ power-loss passed (refused to start on corrupt AOF)"
else
    echo "❌ power-loss failed (exit $RC)"; exit 1
fi
