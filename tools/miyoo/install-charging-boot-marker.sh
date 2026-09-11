#!/bin/sh
# Install the narrow one-shot developer full-boot guard in Onion runtime.sh.
set -eu

RUNTIME=/mnt/SDCARD/.tmp_update/runtime.sh
MARKER=/mnt/SDCARD/.tmp_update/config/.miyoofin_force_full_boot_once
LOG=/mnt/SDCARD/App/MiyooFin/telemetry-logs/charging-boot-marker.log
TMP=$RUNTIME.miyoofin.$$
NULL_SINK=/tmp/.miyoofin-charging-marker-null.$$
cleanup() { rm -f "$TMP" "$NULL_SINK"; }
trap cleanup EXIT HUP INT TERM
: > "$NULL_SINK"
log() { printf '%s %s\n' "$(date '+%Y-%m-%dT%H:%M:%S')" "$*" >> "$LOG" 2>"$NULL_SINK" || true; }

[ -f "$RUNTIME" ] || { log 'runtime_missing'; exit 1; }

# Already-installed form: require the complete fixed guard, not a partial match.
if grep -q 'miyoofin_force_full_boot_once' "$RUNTIME" \
    && grep -q 'chargingState' "$RUNTIME" \
    && grep -q 'start_networking' "$RUNTIME"; then
    log 'already_installed'
    exit 0
fi

# Refuse to patch an Onion update or unexpected local modification.  The
# original block is deliberately matched exactly before replacement.
awk '
BEGIN { replaced=0 }
{
    if ($0 == "    # Show charging animation") {
        getline a; getline b; getline c; getline d; getline e
        if (a == "    if [ $is_charging -eq 1 ]; then" &&
            b == "        cd $sysdir" &&
            c == "        chargingState" &&
            d == "    fi" && e == "") {
            print "    # Show charging animation"
            print "    if [ $is_charging -eq 1 ]; then"
            print "        if [ -f /mnt/SDCARD/.tmp_update/config/.miyoofin_force_full_boot_once ] && rm -f /mnt/SDCARD/.tmp_update/config/.miyoofin_force_full_boot_once; then"
            print "            sync"
            print "        else"
            print "            cd $sysdir"
            print "            chargingState"
            print "        fi"
            print "    fi"
            print e
            replaced=1
            next
        }
        print "    # Show charging animation"; print a; print b; print c; print d; print e
        next
    }
    print
}
END { if (!replaced) exit 2 }
' "$RUNTIME" > "$TMP" || { log 'unexpected_runtime_hook'; exit 1; }

mv -f "$TMP" "$RUNTIME"
log 'installed'
