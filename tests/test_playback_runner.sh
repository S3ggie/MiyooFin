#!/bin/sh
set -eu

ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
RUNNER="$ROOT/distributions/onionos/playback_runner.sh"
TMP_ROOT=$(mktemp -d /tmp/miyoofin-runner-test.XXXXXX)
trap 'rm -rf "$TMP_ROOT"' EXIT

fail() { echo "runner test failed: $*" >&2; exit 1; }

run_case() {
    case_dir=$1
    session=$2
    with_ca=$3
    app_dir="$TMP_ROOT/$case_dir"
    mkdir -p "$app_dir"
    cp "$RUNNER" "$app_dir/playback_runner.sh"
    printf '%s\n' 'item_id=item' 'item_type=movie' 'resume_ticks=0' 'source_mode=jellyfin' > "$app_dir/playback-request.txt"
    printf '%s\n' "$session" 'access_token=token' > "$app_dir/session.txt"
    if [ "$with_ca" = yes ]; then printf '%s\n' '-----BEGIN CERTIFICATE-----' 'test' '-----END CERTIFICATE-----' > "$app_dir/cacert.pem"; fi
    cat > "$app_dir/miyoofin-https-bridge" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$(dirname "$0")/bridge-args.txt"
exit 0
EOF
    chmod +x "$app_dir/miyoofin-https-bridge"
    (cd "$app_dir" && sh ./playback_runner.sh) >/dev/null 2>&1 || true
}

# HTTP-only routes pass an empty CA argument and launch normally.
run_case http_only 'server_url=http://192.168.1.212:8096' no
[ -f "$TMP_ROOT/http_only/bridge-args.txt" ] || fail 'HTTP-only route did not launch bridge'
grep -q '^http://192.168.1.212:8096/' "$TMP_ROOT/http_only/bridge-args.txt" || fail 'HTTP-only route did not use HTTP upstream'
[ "$(sed -n '2p' "$TMP_ROOT/http_only/bridge-args.txt")" = "" ] || fail 'HTTP-only route passed a CA path'
[ "$(sed -n '3p' "$TMP_ROOT/http_only/bridge-args.txt")" = 18080 ] || fail 'HTTP-only route passed unexpected port'

# LAN HTTP with an HTTPS fallback launches LAN only when CA data is absent.
run_case lan_fallback 'server_url=http://192.168.1.212:8096
public_server_url=https://jellyfin.example.com' no
[ -f "$TMP_ROOT/lan_fallback/bridge-args.txt" ] || fail 'LAN route did not launch bridge'
! grep -q -- '--fallback-url\|https://jellyfin.example.com' "$TMP_ROOT/lan_fallback/bridge-args.txt" || fail 'HTTPS fallback was not disabled'
grep -q 'Secure HTTPS fallback unavailable: cacert.pem not found' "$TMP_ROOT/lan_fallback/playback-launch.log" || fail 'missing fallback warning'
[ "$(sed -n '2p' "$TMP_ROOT/lan_fallback/bridge-args.txt")" = "" ] || fail 'LAN-only launch passed a CA path'

# HTTPS-only playback is rejected before the bridge starts without CA data.
run_case https_only 'server_url=https://jellyfin.example.com' no
! [ -f "$TMP_ROOT/https_only/bridge-args.txt" ] || fail 'HTTPS-only route launched without CA'
grep -q 'ERROR: cacert.pem not found for HTTPS playback' "$TMP_ROOT/https_only/playback-launch.log" || fail 'missing HTTPS-only rejection'

# An HTTPS route retains the CA argument when it is available.
run_case https_with_ca 'server_url=https://jellyfin.example.com' yes
[ "$(sed -n '2p' "$TMP_ROOT/https_with_ca/bridge-args.txt")" = "$TMP_ROOT/https_with_ca/cacert.pem" ] || fail 'HTTPS route did not pass CA path'

echo '[test] playback runner route-aware CA handling OK'
