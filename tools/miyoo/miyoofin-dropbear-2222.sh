#!/bin/sh
# MiyooFin developer-only Dropbear on port 2222.
# Onion's built-in port-22 Dropbear is intentionally untouched.
set -eu

BASE=/mnt/SDCARD/App/MiyooFin/tools/dropbear
KEYSTORE=/mnt/SDCARD/.ssh-persist/authorized_keys
RUNTIME_HOME=/tmp/onion-home
RUNTIME_SSH=$RUNTIME_HOME/.ssh
RUNTIME_KEYS=/tmp/miyoofin-dropbear-keys
HOST_KEY=$RUNTIME_KEYS/dropbear_ed25519_host_key
PID_FILE=$BASE/run/dropbear-2222.pid
LOG_FILE=$BASE/log/dropbear-2222.log
DAEMON=$BASE/bin/dropbear
PERSIST_HOST_KEY=$BASE/keys/dropbear_ed25519_host_key

mkdir -p "$RUNTIME_SSH" "$RUNTIME_KEYS" "$BASE/run" "$BASE/log"
chown 1000:1000 "$RUNTIME_HOME" "$RUNTIME_SSH"
chmod 700 "$RUNTIME_HOME" "$RUNTIME_SSH"
if [ ! -f "$KEYSTORE" ]; then
    echo "miyoofin-dropbear: missing persistent authorized_keys" >>"$LOG_FILE"
    exit 1
fi
cp "$KEYSTORE" "$RUNTIME_SSH/authorized_keys"
chown 1000:1000 "$RUNTIME_SSH/authorized_keys"
chmod 600 "$RUNTIME_SSH/authorized_keys"

if [ ! -f "$PERSIST_HOST_KEY" ]; then
    echo "miyoofin-dropbear: missing persistent host key" >>"$LOG_FILE"
    exit 1
fi
cp "$PERSIST_HOST_KEY" "$HOST_KEY"
chmod 600 "$HOST_KEY"

if [ -f "$PID_FILE" ]; then
    pid=$(sed -n '1p' "$PID_FILE" 2>/dev/null || true)
    case "$pid" in
        ''|*[!0-9]*) rm -f "$PID_FILE" ;;
        *)
            if [ -r "/proc/$pid/comm" ] && [ "$(cat "/proc/$pid/comm" 2>/dev/null || true)" = dropbear ]; then
                exit 0
            fi
            rm -f "$PID_FILE"
            ;;
    esac
fi

HOME="$RUNTIME_HOME" "$DAEMON" -s -p 2222 -r "$HOST_KEY" -P "$PID_FILE" -E >>"$LOG_FILE" 2>&1
