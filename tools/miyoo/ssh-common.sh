#!/bin/sh
# Shared developer SSH defaults for the persistent Miyoo key daemon.

MIYOO_SSH_TARGET=${MIYOO_SSH_TARGET:-${MIYOO_HOST:-onion@192.168.1.197}}
MIYOO_SSH_PORT=${MIYOO_SSH_PORT:-2222}
MIYOO_SSH_KEY=${MIYOO_SSH_KEY:-$HOME/.ssh/id_ed25519}

miyoo_ssh() {
    if [ -n "${MIYOO_SSH_CONTROL_PATH:-}" ]; then
        ssh -T -p "$MIYOO_SSH_PORT" -i "$MIYOO_SSH_KEY" \
            -o BatchMode=yes -o PasswordAuthentication=no \
            -o StrictHostKeyChecking=accept-new \
            -o ConnectTimeout=10 -o ControlPath="$MIYOO_SSH_CONTROL_PATH" "$@"
    else
        ssh -T -p "$MIYOO_SSH_PORT" -i "$MIYOO_SSH_KEY" \
            -o BatchMode=yes -o PasswordAuthentication=no \
            -o StrictHostKeyChecking=accept-new \
            -o ConnectTimeout=10 "$@"
    fi
}

miyoo_scp() {
    scp -P "$MIYOO_SSH_PORT" -i "$MIYOO_SSH_KEY" \
        -o BatchMode=yes -o PasswordAuthentication=no \
        -o ConnectTimeout=10 "$@"
}
