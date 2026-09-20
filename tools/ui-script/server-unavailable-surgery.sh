#!/bin/sh
# MiyooFin ui-script --server-unavailable configuration surgery.
#
# This file is sourced by the offline test and piped verbatim to the device by
# device-run.sh.  It deliberately prints filenames/status only: session.txt
# contains the access token and must never be sent to the harness log.

# Refuse stale backups before touching either file.  Both files must already
# exist; failing closed avoids a partial route when a saved session is absent.
miyoofin_server_unavailable_check() {
    _msuc_appdir=${1:?}; _msuc_server_bak=${2:?}; _msuc_session_bak=${3:?}
    [ -f "$_msuc_appdir/server.txt" ] \
        || { echo "uiscript-server: missing: $_msuc_appdir/server.txt" >&2; return 1; }
    [ -f "$_msuc_appdir/session.txt" ] \
        || { echo "uiscript-server: missing: $_msuc_appdir/session.txt" >&2; return 1; }
    [ ! -e "$_msuc_appdir/$_msuc_server_bak" ] \
        || { echo "uiscript-server: refusing stale backup: $_msuc_appdir/$_msuc_server_bak" >&2; return 1; }
    [ ! -e "$_msuc_appdir/$_msuc_session_bak" ] \
        || { echo "uiscript-server: refusing stale backup: $_msuc_appdir/$_msuc_session_bak" >&2; return 1; }
}

# Back up both files, verify each backup byte-for-byte, then replace only the
# server route values.  server_url remains the warm server identity used by
# CatalogDb; RouteRequest uses the local/public endpoint overrides below for
# actual requests.  Older session files may not have those override keys, so
# add them when absent.  The access_token and every other session line comes
# from the backup unchanged.  Temporary files are kept beside their source so
# the final writes preserve the existing file inode/mode.
miyoofin_server_unavailable_apply() {
    _msua_appdir=${1:?}; _msua_server_bak=${2:?}; _msua_session_bak=${3:?}; _msua_endpoint=${4:?}
    _msua_server="$_msua_appdir/server.txt"
    _msua_session="$_msua_appdir/session.txt"
    _msua_server_backup="$_msua_appdir/$_msua_server_bak"
    _msua_session_backup="$_msua_appdir/$_msua_session_bak"
    _msua_server_tmp="$_msua_server.uiscript-new"
    _msua_session_tmp="$_msua_session.uiscript-new"

    miyoofin_server_unavailable_check "$_msua_appdir" "$_msua_server_bak" "$_msua_session_bak" || return 1
    [ ! -e "$_msua_server_tmp" ] && [ ! -e "$_msua_session_tmp" ] \
        || { echo 'uiscript-server: refusing stale temporary file' >&2; return 1; }

    cp "$_msua_server" "$_msua_server_backup" \
        || { echo 'uiscript-server: server.txt backup failed' >&2; return 1; }
    cmp -s "$_msua_server" "$_msua_server_backup" \
        || { echo 'uiscript-server: server.txt backup differs' >&2; return 1; }
    cp "$_msua_session" "$_msua_session_backup" \
        || { echo 'uiscript-server: session.txt backup failed' >&2; return 1; }
    cmp -s "$_msua_session" "$_msua_session_backup" \
        || { echo 'uiscript-server: session.txt backup differs' >&2; return 1; }

    printf '%s\n' "$_msua_endpoint" >"$_msua_server_tmp" \
        || { echo 'uiscript-server: server.txt route build failed' >&2; return 1; }
    awk -v endpoint="$_msua_endpoint" '
        BEGIN { local_routed = 0; public_routed = 0 }
        /^local_server_url=/ {
            sub(/=.*/, "=" endpoint)
            local_routed = 1
        }
        /^public_server_url=/ {
            sub(/=.*/, "=" endpoint)
            public_routed = 1
        }
        { print }
        END {
            if (!local_routed) print "local_server_url=" endpoint
            if (!public_routed) print "public_server_url=" endpoint
        }
    ' "$_msua_session_backup" >"$_msua_session_tmp" \
        || { rm -f "$_msua_server_tmp" "$_msua_session_tmp"; echo 'uiscript-server: session endpoint build failed' >&2; return 1; }

    cat "$_msua_server_tmp" >"$_msua_server" \
        || { rm -f "$_msua_server_tmp" "$_msua_session_tmp"; echo 'uiscript-server: server.txt route install failed' >&2; return 1; }
    cat "$_msua_session_tmp" >"$_msua_session" \
        || { rm -f "$_msua_server_tmp" "$_msua_session_tmp"; echo 'uiscript-server: session.txt route install failed' >&2; return 1; }
    rm -f "$_msua_server_tmp" "$_msua_session_tmp"
    echo 'uiscript-server: routed server.txt and session endpoint overrides'
    return 0
}

# Restore both files in place and prove exact byte identity before deleting
# either backup.  Keeping both backups until both comparisons pass makes a
# retry safe after a halfway restore failure.
miyoofin_server_unavailable_restore() {
    _msur_appdir=${1:?}; _msur_server_bak=${2:?}; _msur_session_bak=${3:?}
    _msur_server="$_msur_appdir/server.txt"
    _msur_session="$_msur_appdir/session.txt"
    _msur_server_backup="$_msur_appdir/$_msur_server_bak"
    _msur_session_backup="$_msur_appdir/$_msur_session_bak"

    if [ ! -e "$_msur_server_backup" ] && [ ! -e "$_msur_session_backup" ]; then
        echo 'uiscript-server: no configuration backup; configuration was untouched'
        return 0
    fi
    [ -f "$_msur_server_backup" ] && [ -f "$_msur_session_backup" ] \
        || { echo 'uiscript-server: one or more configuration backups are missing' >&2; return 1; }
    cat "$_msur_server_backup" >"$_msur_server" \
        || { echo 'uiscript-server: server.txt restore write failed' >&2; return 1; }
    cat "$_msur_session_backup" >"$_msur_session" \
        || { echo 'uiscript-server: session.txt restore write failed' >&2; return 1; }
    cmp -s "$_msur_server_backup" "$_msur_server" \
        || { echo 'uiscript-server: server.txt restore differs from backup' >&2; return 1; }
    cmp -s "$_msur_session_backup" "$_msur_session" \
        || { echo 'uiscript-server: session.txt restore differs from backup' >&2; return 1; }
    rm -f "$_msur_server_backup" "$_msur_session_backup"
    [ ! -e "$_msur_server_backup" ] && [ ! -e "$_msur_session_backup" ] \
        || { echo 'uiscript-server: configuration backup removal failed' >&2; return 1; }
    echo 'uiscript-server: server.txt and session.txt restored byte-identically'
    return 0
}
