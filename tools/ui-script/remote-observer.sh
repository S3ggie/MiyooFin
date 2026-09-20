#!/bin/sh
# Reusable, read-only observer for remote UI-script runs.
#
# The caller must source tools/miyoo/ssh-common.sh first.  No command in this
# file changes the device.  Snapshots deliberately redact URLs and token-like
# values before they are written to the host observation log.

remote_observer_snapshot() {
    _mro_target=${1:?remote_observer_snapshot needs target}
    _mro_output=${2:?remote_observer_snapshot needs output path}
    _mro_log=${3:-/tmp/miyoofin-ui-script/app.log}
    _mro_lines=${4:-40}
    _mro_download_root=${5:-}
    _mro_tmp="${_mro_output}.tmp.$$"

    if ! miyoo_ssh "$_mro_target" sh -s -- \
        "$_mro_log" "$_mro_lines" "$_mro_download_root" >"$_mro_tmp" <<'EOF'
set -u
log_path=$1
log_lines=$2
download_root=${3:-}

comm_pids() {
    wanted=$1
    result=
    for comm in /proc/[0-9]*/comm; do
        [ -r "$comm" ] || continue
        name=$(cat "$comm" 2>/dev/null || true)
        if [ "$name" = "$wanted" ]; then
            pid=${comm#/proc/}
            pid=${pid%/comm}
            result="$result $pid"
        fi
    done
    printf '%s\n' "${result# }"
}

describe_processes() {
    wanted=$1
    pids=$(comm_pids "$wanted")
    count=0
    rss=0
    threads=0
    for pid in $pids; do
        count=$((count + 1))
        status="/proc/$pid/status"
        one_rss=$(awk '$1 == "VmRSS:" { print $2; exit }' "$status" 2>/dev/null || true)
        one_threads=$(awk '$1 == "Threads:" { print $2; exit }' "$status" 2>/dev/null || true)
        case $one_rss in ''|*[!0-9]*) one_rss=0 ;; esac
        case $one_threads in ''|*[!0-9]*) one_threads=0 ;; esac
        rss=$((rss + one_rss))
        threads=$((threads + one_threads))
    done
    printf 'process name=%s count=%s pids=%s rss_kb=%s threads=%s\n' \
        "$wanted" "$count" "${pids:-none}" "$rss" "$threads"
}

printf 'sample_epoch=%s\n' "$(date +%s)"
describe_processes MainUI
describe_processes miyoofin
if [ -n "$download_root" ]; then
    download_files=0
    download_parts=0
    download_manifests=0
    download_complete=0
    download_active=0
    download_bytes=0
    download_expected=0
    if [ -d "$download_root" ]; then
        for manifest in "$download_root"/*/items/*/manifest.v2; do
            [ -f "$manifest" ] || continue
            download_manifests=$((download_manifests + 1))
            downloaded=$(awk -F= '$1 == "downloaded" { print $2; exit }' "$manifest" 2>/dev/null || true)
            expected=$(awk -F= '$1 == "size" { print $2; exit }' "$manifest" 2>/dev/null || true)
            state=$(awk -F= '$1 == "state" { print $2; exit }' "$manifest" 2>/dev/null || true)
            case $downloaded in ''|*[!0-9]*) downloaded=0 ;; esac
            case $expected in ''|*[!0-9]*) expected=0 ;; esac
            case $state in ''|*[!0-9]*) state=-1 ;; esac
            download_bytes=$((download_bytes + downloaded))
            download_expected=$((download_expected + expected))
            [ "$state" = 6 ] && download_complete=$((download_complete + 1))
            [ "$state" = 2 ] && download_active=$((download_active + 1))
        done
        for partial in "$download_root"/*/items/*/segments/*.part \
                       "$download_root"/*/items/*/chunks/*.part; do
            [ -f "$partial" ] && download_parts=$((download_parts + 1)) || true
        done
        for media in "$download_root"/*/items/*/segments/*.bin \
                     "$download_root"/*/items/*/chunks/*.bin; do
            [ -f "$media" ] && download_files=$((download_files + 1)) || true
        done
    fi
    printf 'download_state root=%s manifests=%s media_files=%s partial_files=%s complete_items=%s active_items=%s downloaded_bytes=%s expected_bytes=%s\n' \
        "$download_root" "$download_manifests" "$download_files" "$download_parts" \
        "$download_complete" "$download_active" "$download_bytes" "$download_expected"
fi
if [ -f "$log_path" ]; then
    printf 'log_tail_begin path=%s lines=%s\n' "$log_path" "$log_lines"
    # Redact all URLs, then redact common token-shaped values that may not be
    # embedded in a URL.  The observer must never become an access-token log.
    tail -n "$log_lines" "$log_path" 2>/dev/null \
        | sed -r \
            -e 's#https?://[^[:space:]]+#<url-redacted>#g' \
            -e 's#(X-Emby-Token[[:space:]]*:?[[:space:]]+)[^[:space:]]+#\1<redacted>#Ig' \
            -e 's#(X-Emby-Authorization[[:space:]]*:[[:space:]]*Token[[:space:]]+)[^[:space:]]+#\1<redacted>#Ig' \
            -e 's#(access_token|api_key|authorization|token)=([^[:space:]]+)#\1=<redacted>#Ig'
    printf 'log_tail_end\n'
else
    printf 'log_tail_missing path=%s\n' "$log_path"
fi
EOF
    then
        rm -f "$_mro_tmp"
        return 1
    fi
    mv "$_mro_tmp" "$_mro_output"
}

remote_observer_no_duplicate_processes() {
    _mro_snapshot=${1:?remote_observer_no_duplicate_processes needs snapshot}
    awk '
        $1 == "process" && ($2 == "name=MainUI" || $2 == "name=miyoofin") {
            split($3, count, "=")
            if (count[2] > 1) bad=1
        }
        END { exit bad ? 1 : 0 }
    ' "$_mro_snapshot"
}

remote_observer_postcondition() {
    _mro_target=${1:?remote_observer_postcondition needs target}
    _mro_condition=${2:?remote_observer_postcondition needs condition}
    _mro_app_dir=${3:?remote_observer_postcondition needs app dir}
    _mro_launcher_backup=${4:?remote_observer_postcondition needs launcher backup}
    _mro_server_backup=${5:-}
    _mro_session_backup=${6:-}

    miyoo_ssh "$_mro_target" sh -s -- \
        "$_mro_condition" "$_mro_app_dir" "$_mro_launcher_backup" \
        "$_mro_server_backup" "$_mro_session_backup" <<'EOF'
set -eu
condition=$1
app_dir=$2
launcher_backup=$3
server_backup=${4:-}
session_backup=${5:-}

count_comm() {
    wanted=$1
    count=0
    for comm in /proc/[0-9]*/comm; do
        [ -r "$comm" ] || continue
        [ "$(cat "$comm" 2>/dev/null || true)" = "$wanted" ] \
            && count=$((count + 1)) || true
    done
    printf '%s\n' "$count"
}

mainui=$(count_comm MainUI)
app=$(count_comm miyoofin)

case "$condition" in
    app-running)
        [ "$app" -gt 0 ] && [ "$mainui" -eq 0 ]
        ;;
    mainui-restored)
        [ "$app" -eq 0 ] && [ "$mainui" -eq 1 ]
        ;;
    restored|clean)
        [ "$app" -eq 0 ]
        [ "$mainui" -eq 1 ]
        [ -x "$app_dir/launch.sh" ]
        [ ! -e "$app_dir/$launcher_backup" ]
        ! grep -qE 'MIYOOFIN_UI_|LD_PRELOAD=' "$app_dir/launch.sh"
        [ -z "$server_backup" ] || [ ! -e "$app_dir/$server_backup" ]
        [ -z "$session_backup" ] || [ ! -e "$app_dir/$session_backup" ]
        ;;
    *)
        echo "unknown remote postcondition: $condition" >&2
        exit 2
        ;;
esac
printf 'POSTCONDITION OK %s\n' "$condition"
EOF
}

remote_observer_wait_postcondition() {
    _mro_target=${1:?remote_observer_wait_postcondition needs target}
    _mro_condition=${2:?remote_observer_wait_postcondition needs condition}
    _mro_timeout=${3:?remote_observer_wait_postcondition needs timeout}
    _mro_interval=${4:?remote_observer_wait_postcondition needs interval}
    shift 4
    _mro_elapsed=0
    while [ "$_mro_elapsed" -le "$_mro_timeout" ]; do
        if remote_observer_postcondition \
            "$_mro_target" "$_mro_condition" "$@"; then
            return 0
        fi
        sleep "$_mro_interval"
        _mro_elapsed=$((_mro_elapsed + _mro_interval))
    done
    return 1
}
