#!/bin/bash
# mfctl: one-line remote control of MiyooFin on the Miyoo over SSH, for fast manual and
# AI-driven testing. Server-safe by default: streamed playback is refused unless you set
# MF_ALLOW_STREAM=1 (each stream makes Jellyfin run an ffmpeg transcode).
#
#   mfctl.sh status              what is running on the device (app / player / menu)
#   mfctl.sh deploy [what]       cross-build in Docker and install (what: app player bridge reporter all)
#   mfctl.sh play local          open the first downloaded episode (no server load)
#   mfctl.sh play stream <name>  start a streamed scenario: remote | audio (needs MF_ALLOW_STREAM=1)
#   mfctl.sh key <name|code>...  press player keys: right left up down a b x y select start menu
#   mfctl.sh shot [name]         screenshot the player frame -> $MF_OUT/<name>.png (default shot)
#   mfctl.sh log [pattern]       recent player log lines (default MFSEEK/MFEXACT/MFRESTART/errors)
#   mfctl.sh subs                subtitle fetch state for the current playback
#   mfctl.sh jellyfin            newest Jellyfin ffmpeg command: does it use vulkan/libplacebo?
#   mfctl.sh quit                end the current playback cleanly (player quit key)
#
# Env: MIYOO_HOST (default 192.168.1.198) MIYOO_SSH_KEY (default ~/.ssh/miyoo_ed25519)
#      MF_OUT (screenshot dir, default /tmp/mfctl)
set -eu
HOST=${MIYOO_HOST:-192.168.1.198}
KEY=${MIYOO_SSH_KEY:-$HOME/.ssh/miyoo_ed25519}
OUT=${MF_OUT:-/tmp/mfctl}
APP=/mnt/SDCARD/App/MiyooFin
ROOT=$(CDPATH= cd -- "$(dirname "$0")/../.." && pwd)
SSHO="-p 2222 -i $KEY -o BatchMode=yes -o ConnectTimeout=8"
r() { ssh $SSHO "onion@$HOST" "$@"; }
mkdir -p "$OUT"

procs() { r 'for f in /proc/[0-9]*/comm; do cat $f 2>/dev/null; done | grep -i "^miyoofin\|^mainui" | sort | uniq -c'; }
keycode() {
    case "$1" in
        right) echo 275 ;; left) echo 276 ;; up) echo 273 ;; down) echo 274 ;;
        a) echo 32 ;; b) echo 306 ;; x) echo 304 ;; y) echo 308 ;;
        select) echo 305 ;; start) echo 13 ;; menu) echo 27 ;;
        [0-9]*) echo "$1" ;;
        *) echo "unknown key: $1" >&2; exit 2 ;;
    esac
}
wait_for() { # <process-name-prefix> <seconds>
    for _ in $(seq 1 "$2"); do
        if r "for f in /proc/[0-9]*/comm; do cat \$f 2>/dev/null; done | grep -q '^$1'"; then return 0; fi
        sleep 1
    done
    return 1
}
need_free_menu() {
    if ! r 'for f in /proc/[0-9]*/comm; do cat $f 2>/dev/null; done | grep -q "^MainUI"'; then
        echo "MiyooFin is open on the device (no MainUI): close it first." >&2
        exit 3
    fi
}

cmd=${1:-status}; shift || true
case "$cmd" in
status)
    procs
    r "cat $APP/player-prefs.txt 2>/dev/null; tail -2 $APP/playback-launch.log 2>/dev/null | cut -c1-110"
    ;;
deploy)
    what=${1:-all}
    cd "$ROOT"
    case "$what" in all) targets="all bridge reporter player" ;; app) targets="all" ;; *) targets="$what" ;; esac
    sg docker -c "docker run --rm --user $(id -u):$(id -g) -v $PWD:/build miyoofin-toolchain make -f Makefile.cross -j8 PERF_TELEMETRY=1 RELEASE=1 $targets" 2>&1 | grep -E " error|Error" || true
    for f in miyoofin miyoofin-https-bridge miyoofin-playback-reporter miyoofin-player; do
        case "$what:$f" in
            all:*|app:miyoofin|bridge:miyoofin-https-bridge|reporter:miyoofin-playback-reporter|player:miyoofin-player) ;;
            *) continue ;;
        esac
        scp -q -P 2222 -i "$KEY" -o BatchMode=yes "output/build-arm/$f" "onion@$HOST:$APP/$f.new"
        r "cd $APP && mv $f.new $f"
        echo "deployed $f"
    done
    scp -q -P 2222 -i "$KEY" -o BatchMode=yes distributions/onionos/playback_runner.sh "onion@$HOST:$APP/playback_runner.sh.new"
    r "cd $APP && mv playback_runner.sh.new playback_runner.sh && rm -f /tmp/miyoofin-player-cmd"
    ;;
play)
    kind=${1:-local}
    case "$kind" in
        local) scenario=play-local ;;
        stream)
            [ "${MF_ALLOW_STREAM:-0}" = 1 ] || { echo "streamed playback starts a server transcode: set MF_ALLOW_STREAM=1 to proceed" >&2; exit 4; }
            case "${2:-remote}" in remote) scenario=play-remote ;; audio) scenario=play-audio ;; *) echo "scenario: remote|audio" >&2; exit 2 ;; esac ;;
        *) echo "play local | play stream <remote|audio>" >&2; exit 2 ;;
    esac
    need_free_menu
    r 'rm -f /tmp/miyoofin-player-cmd'
    cd "$ROOT"
    (sg docker -c "env MIYOO_SSH_KEY=$KEY MIYOO_SSH_TARGET=onion@$HOST MIYOO_HOST=$HOST MIYOO_SSH_PORT=2222 MIYOOFIN_UI_TIMEOUT_S=900 sh tools/ui-script/device-run.sh $scenario" > "$OUT/scenario.log" 2>&1 &)
    if wait_for miyoofin-playe 120; then echo "player is up (give it ~20 s to buffer)"; else echo "player did not start; see $OUT/scenario.log" >&2; exit 5; fi
    ;;
key)
    [ $# -gt 0 ] || { echo "key <name|code>..." >&2; exit 2; }
    lines=""
    for k in "$@"; do lines="${lines}key $(keycode "$k")\n"; done
    r "printf '${lines}' > /tmp/miyoofin-player-cmd"
    ;;
shot)
    name=${1:-shot}
    r 'rm -f /tmp/miyoofin-player-shot.bmp 2>/dev/null; printf "shot\n" > /tmp/miyoofin-player-cmd'
    for _ in $(seq 1 10); do sleep 1; r 'test -s /tmp/miyoofin-player-shot.bmp' && break; done
    scp -q -P 2222 -i "$KEY" -o BatchMode=yes "onion@$HOST:/tmp/miyoofin-player-shot.bmp" "$OUT/$name.bmp"
    python3 -c "from PIL import Image;Image.open('$OUT/$name.bmp').save('$OUT/$name.png')"
    echo "$OUT/$name.png"
    ;;
log)
    pat=${1:-MFSEEK|MFEXACT|MFRESTART|MFBASE|error while|Could not|Failed}
    r "tr '\r' '\n' < $APP/playback-ffplay.log | grep -aE '$pat' | tail -30 | cut -c1-140"
    ;;
subs)
    r "cd $APP; echo tracks:; cat playback-tracks.txt 2>/dev/null | cut -c1-90; echo files:; ls -la subs 2>&1 | tail -n +2 | head; echo fetch:; grep -av 'no version' playback-subs.log 2>/dev/null | tail -5"
    ;;
jellyfin)
    ses=$(r "cat $APP/session.txt")
    SES="$ses" python3 - <<'EOF'
import json,os,re,urllib.parse,urllib.request
kv=dict(l.split('=',1) for l in os.environ['SES'].splitlines() if '=' in l)
base=kv.get('local_server_url') or kv['server_url']; H={'X-Emby-Token':kv['access_token']}
def get(u): return urllib.request.urlopen(urllib.request.Request(base+u,headers=H),timeout=30).read()
logs=sorted([l for l in json.loads(get('/System/Logs')) if 'ffmpeg.transcode' in l['Name'].lower()],key=lambda l:l['DateModified'])
for l in logs[-3:]:
    t=get('/System/Logs/Log?name='+urllib.parse.quote(l['Name'])).decode('utf8','replace')
    cmd=next((x for x in t.splitlines() if ' -i file:' in x),'')
    bad=any(k in cmd.lower() for k in ('vulkan','libplacebo'))
    print(l['DateModified'][11:19],'VULKAN/LIBPLACEBO IN COMMAND' if bad else 'ok: no vulkan/libplacebo')
EOF
    ;;
quit)
    r 'printf "key 27\n" > /tmp/miyoofin-player-cmd'
    for _ in $(seq 1 40); do r 'for f in /proc/[0-9]*/comm; do cat $f 2>/dev/null; done | grep -q "^miyoofin-playe"' || break; sleep 1; done
    r 'rm -f /tmp/miyoofin-player-cmd'
    procs
    ;;
*)
    sed -n '2,20p' "$0"
    exit 2
    ;;
esac
