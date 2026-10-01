#!/bin/sh
# MiyooFin ui-script --no-session configuration surgery.
#
# Sourced by the offline test and piped verbatim to the device by
# device-run.sh. It blanks session.txt so the app boots with its saved server
# URL but no session (ConnectScreen -> LoginScreen), then restores the file
# byte-for-byte. It prints filenames/status only: session.txt contains the
# access token and must never reach the harness log.
#
# Safety invariants:
#   - The backup is verified (cmp) BEFORE session.txt is touched. A backup
#     that fails verification is deleted, so a later restore finds "missing"
#     and refuses instead of writing a partial copy over the real session.
#   - session.txt is only ever rewritten IN PLACE (`:>` / `cat >`), never
#     renamed or chmod'd, so the inode and its mode survive.

miyoofin_no_session_apply() {
    _mns_appdir=${1:?}; _mns_bak=${2:?}
    _mns_session="$_mns_appdir/session.txt"
    _mns_backup="$_mns_appdir/$_mns_bak"

    [ -f "$_mns_session" ] \
        || { echo "uiscript-session: missing: $_mns_session" >&2; return 1; }
    [ ! -e "$_mns_backup" ] \
        || { echo "uiscript-session: refusing stale backup: $_mns_backup" >&2; return 1; }

    if ! cp "$_mns_session" "$_mns_backup"; then
        rm -f "$_mns_backup"
        echo 'uiscript-session: session.txt backup failed' >&2
        return 1
    fi
    if ! cmp -s "$_mns_session" "$_mns_backup"; then
        rm -f "$_mns_backup"
        echo 'uiscript-session: session.txt backup differs' >&2
        return 1
    fi
    : > "$_mns_session" \
        || { echo 'uiscript-session: blanking session.txt failed' >&2; return 1; }
    echo 'uiscript-session: session.txt backed up and blanked'
}

miyoofin_no_session_restore() {
    _mnr_appdir=${1:?}; _mnr_bak=${2:?}
    _mnr_session="$_mnr_appdir/session.txt"
    _mnr_backup="$_mnr_appdir/$_mnr_bak"

    [ -f "$_mnr_backup" ] \
        || { echo "uiscript-session: backup missing, nothing restored: $_mnr_backup" >&2; return 1; }
    cat "$_mnr_backup" > "$_mnr_session" \
        || { echo 'uiscript-session: writing session.txt back failed' >&2; return 1; }
    cmp -s "$_mnr_session" "$_mnr_backup" \
        || { echo 'uiscript-session: restored session.txt differs from backup' >&2; return 1; }
    rm -f "$_mnr_backup" \
        || { echo 'uiscript-session: restored but could not remove backup' >&2; return 1; }
    echo 'uiscript-session: session.txt restored and verified'
}
