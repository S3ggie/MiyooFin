#!/bin/sh
# MiyooFin ui-script launcher surgery (shared library, no side effects).
#
# Pure-local file operations on <appdir>/launch.sh used by BOTH:
#   - tools/ui-script/device-run.sh, which pipes this exact file through
#     SSH stdin (`sh -s -- <appdir> ...`) so it runs ON THE DEVICE, and
#   - tools/ui-script/test-launcher-roundtrip.sh, which sources this file
#     and runs the SAME functions against a local fixture directory.
# There is exactly one copy of the inject/restore logic; the offline test
# exercises the bytes the device runs.
#
# Filesystem constraints (why this file looks the way it does):
#   launch.sh on the device is root-owned with mode 0777 while the SSH
#   user is unprivileged (uid 1000). That combination means:
#   - `cp -p` FAILS (cannot preserve ownership/permissions across users),
#     so the backup is taken with a plain `cp` (no -p);
#   - `chmod` on launch.sh FAILS (Operation not permitted), so the file is
#     NEVER chmod'ed — it is already 0777 and an in-place redirect
#     (`cat ... > launch.sh`) preserves inode, owner, and mode;
#   - `mv` onto launch.sh would replace the inode (new owner/mode), so the
#     injected content is written strictly IN PLACE, never via rename.
# Consequences, enforced below: no `cp -p`, no `chmod`, no `mv` onto
# launch.sh anywhere in this file. Audit with:
#   grep -nE '^[[:space:]]*(cp +-p|chmod([[:space:]]|$)|mv )' tools/ui-script/launcher-surgery.sh
# (must print nothing).
#
# All functions operate on explicit directory arguments, print
# `uiscript-launcher:`-prefixed diagnostics, and return non-zero on any
# failure without touching anything they do not own: a failed inject
# never removes the backup, and a failed restore never removes anything.

# Print a checksum of a file with whatever tool is available. The caller
# uses it as a tripwire (same machine, same tool, before vs after), not
# as a security proof; byte-identity is proven with cmp.
miyoofin_launcher_sum() {
    if command -v sha256sum >/dev/null 2>&1; then
        sha256sum "${1:?}" | awk '{ print $1 }'
    elif command -v sha1sum >/dev/null 2>&1; then
        sha1sum "${1:?}" | awk '{ print $1 }'
    elif command -v md5sum >/dev/null 2>&1; then
        md5sum "${1:?}" | awk '{ print $1 }'
    else
        cksum "${1:?}" | awk '{ print $1"-"$2 }'
    fi
}

# Succeed iff the launcher at $1 looks pristine: present, executable,
# syntactically valid, and free of harness injection markers. The clean
# packaged launcher exports LD_LIBRARY_PATH (not LD_PRELOAD), so the
# marker patterns below cannot match it.
miyoofin_launcher_check_clean() {
    _mlc_file=${1:?}
    [ -f "$_mlc_file" ] || { echo "uiscript-launcher: missing: $_mlc_file" >&2; return 1; }
    [ -x "$_mlc_file" ] || { echo "uiscript-launcher: not executable: $_mlc_file" >&2; return 1; }
    sh -n "$_mlc_file" 2>/dev/null \
        || { echo "uiscript-launcher: syntax check failed: $_mlc_file" >&2; return 1; }
    if grep -q 'MIYOOFIN_UI_\|LD_PRELOAD=' "$_mlc_file" 2>/dev/null; then
        echo "uiscript-launcher: injection markers still present: $_mlc_file" >&2
        return 1
    fi
    return 0
}

# Back up <appdir>/launch.sh to <appdir>/<backup> and install the
# injected launcher IN PLACE. Verifies afterwards that the file is still
# executable, contains the shim exports, and passes `sh -n`.
#   $1 appdir  $2 backup name  $3 scratch dir  $4 devicekeys (1/0)
miyoofin_launcher_inject() {
    _mli_appdir=${1:?}; _mli_bak=${2:?}; _mli_scratch=${3:?}; _mli_devkeys=${4:?}
    _mli_src="$_mli_appdir/launch.sh"
    _mli_bakpath="$_mli_appdir/$_mli_bak"
    _mli_tmp="$_mli_appdir/launch.sh.uiscript-new"

    # Refuse to clobber: a backup already here means a previous run did
    # not restore, and only the operator knows whether the installed
    # launcher is safe to overwrite.
    if [ -e "$_mli_bakpath" ]; then
        echo "uiscript-launcher: refusing: backup already present: $_mli_bakpath" >&2
        return 1
    fi
    [ -x "$_mli_src" ] || { echo "uiscript-launcher: missing/not executable: $_mli_src" >&2; return 1; }
    if grep -q 'MIYOOFIN_UI_\|LD_PRELOAD=' "$_mli_src" 2>/dev/null; then
        echo "uiscript-launcher: refusing: $_mli_src already looks injected" >&2
        return 1
    fi
    grep -q '^\./miyoofin' "$_mli_src" \
        || { echo "uiscript-launcher: no ./miyoofin anchor line in $_mli_src" >&2; return 1; }

    # Plain cp (no -p): preserving ownership across users fails on this
    # filesystem, and the backup only needs the bytes.
    cp "$_mli_src" "$_mli_bakpath" \
        || { echo "uiscript-launcher: backup copy failed" >&2; return 1; }
    cmp -s "$_mli_src" "$_mli_bakpath" \
        || { echo "uiscript-launcher: backup differs from original" >&2; return 1; }

    # A stale temp file must never be installed; drop a regular file, but
    # leave anything else alone so the build step fails loudly below.
    if [ -e "$_mli_tmp" ] && [ ! -d "$_mli_tmp" ]; then rm -f "$_mli_tmp"; fi
    awk -v scratch="$_mli_scratch" -v devicekeys="$_mli_devkeys" '
        $0 == "./miyoofin" {
            print "export LD_PRELOAD=" scratch "/shim-arm.so"
            print "export MIYOOFIN_UI_SCRIPT=" scratch "/script.txt"
            print "export MIYOOFIN_UI_LOG=" scratch "/app.log"
            print "export MIYOOFIN_UI_SHOT_DIR=" scratch "/shots"
            print "export MIYOOFIN_UI_RESULT=" scratch "/result.txt"
            if (devicekeys == "1") print "export MIYOOFIN_UI_DEVICE_KEYS=1"
            else print "unset MIYOOFIN_UI_DEVICE_KEYS"
            print ": > \"$MIYOOFIN_UI_LOG\""
            print ": > \"$MIYOOFIN_UI_RESULT\""
            print "./miyoofin >>\"$MIYOOFIN_UI_LOG\" 2>&1"
            replaced=1; next
        }
        { print }
        END { if (!replaced) exit 1 }
    ' "$_mli_bakpath" > "$_mli_tmp" \
        || { echo "uiscript-launcher: injection build failed (anchor line not found?)" >&2; return 1; }

    # In-place install: redirecting into the existing file preserves its
    # inode, owner, and 0777 mode. No chmod, no mv, no rename.
    cat "$_mli_tmp" > "$_mli_src" \
        || { rm -f "$_mli_tmp"; echo "uiscript-launcher: in-place install failed" >&2; return 1; }
    rm -f "$_mli_tmp"

    [ -x "$_mli_src" ] \
        || { echo "uiscript-launcher: injected file lost its executable bit" >&2; return 1; }
    grep -q 'MIYOOFIN_UI_SCRIPT' "$_mli_src" \
        || { echo "uiscript-launcher: injected exports missing" >&2; return 1; }
    grep -q 'LD_PRELOAD=' "$_mli_src" \
        || { echo "uiscript-launcher: LD_PRELOAD missing after inject" >&2; return 1; }
    sh -n "$_mli_src" 2>/dev/null \
        || { echo "uiscript-launcher: injected file fails syntax check" >&2; return 1; }
    echo "uiscript-launcher: injected $_mli_src (backup: $_mli_bakpath)"
    return 0
}

# Write the backup at <appdir>/<backup> back over <appdir>/launch.sh IN
# PLACE, verify it (byte-compare, syntax, executable, markers absent,
# optional checksum match against $3), then remove the backup.
#   $1 appdir  $2 backup name  [$3 expected pre-injection checksum]
# A missing backup is OK only when the launcher is already clean (the
# caller failed before modifying anything); otherwise it is a hard error
# because an injected launcher would be left with nothing to restore it
# from.
miyoofin_launcher_restore() {
    _mlr_appdir=${1:?}; _mlr_bak=${2:?}; _mlr_expect=${3:-}
    _mlr_src="$_mlr_appdir/launch.sh"
    _mlr_bakpath="$_mlr_appdir/$_mlr_bak"

    if [ ! -f "$_mlr_bakpath" ]; then
        if miyoofin_launcher_check_clean "$_mlr_src" 2>/dev/null; then
            echo "uiscript-launcher: no backup and launcher untouched; nothing to restore"
            return 0
        fi
        echo "uiscript-launcher: backup missing AND launcher still injected: $_mlr_src" >&2
        return 1
    fi

    # In-place restore: same reason as the inject path — no ownership or
    # mode syscalls, so nothing can fail with Operation not permitted.
    cat "$_mlr_bakpath" > "$_mlr_src" \
        || { echo "uiscript-launcher: restore write failed" >&2; return 1; }
    cmp -s "$_mlr_bakpath" "$_mlr_src" \
        || { echo "uiscript-launcher: restored file differs from backup" >&2; return 1; }
    if [ -n "$_mlr_expect" ]; then
        _mlr_got=$(miyoofin_launcher_sum "$_mlr_src") || return 1
        [ "$_mlr_got" = "$_mlr_expect" ] \
            || { echo "uiscript-launcher: restored checksum $_mlr_got != pre-injection $_mlr_expect" >&2; return 1; }
    fi
    miyoofin_launcher_check_clean "$_mlr_src" || return 1
    rm -f "$_mlr_bakpath"
    [ ! -e "$_mlr_bakpath" ] \
        || { echo "uiscript-launcher: backup removal failed: $_mlr_bakpath" >&2; return 1; }
    echo "uiscript-launcher: launch.sh restored and verified"
    return 0
}
