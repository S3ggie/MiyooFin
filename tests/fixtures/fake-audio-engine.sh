#!/bin/bash
# Stand-in for miyoofin-audio in host tests: speaks the same line protocol, "plays" every
# track for FAKE_LEN_MS (default 400), and can be told to fail or crash:
#   a path containing "bad"        -> error <path> instead of started
#   FAKE_CRASH_FILE=<f>            -> exit(1) on the first load, once (creates <f>)
len=${FAKE_LEN_MS:-400}
cur=""; next=""; elapsed=0; paused=0
echo ready
while true; do
    if read -r -t 0.1 cmd arg1 arg2; then
        case "$cmd" in
        load)
            if [ -n "$FAKE_CRASH_FILE" ] && [ ! -e "$FAKE_CRASH_FILE" ]; then
                : > "$FAKE_CRASH_FILE"
                exit 1
            fi
            next=""; paused=0
            case "$arg2" in
            *bad*) echo "error $arg2"; cur="" ;;
            *) cur="$arg2"; elapsed=${arg1%.*}000; echo "started $cur" ;;
            esac ;;
        next) next="$arg1" ;;
        nonext) next="" ;;
        pause) paused=1 ;;
        resume) paused=0 ;;
        seek) elapsed=${arg1%.*}000 ;;
        stop) cur=""; next="" ;;
        quit) exit 0 ;;
        esac
    fi
    if [ -n "$cur" ]; then
        [ "$paused" = 0 ] && elapsed=$((elapsed + 100))
        echo "pos $((elapsed / 1000)).$(((elapsed % 1000) / 100)) $((len / 1000)).$(((len % 1000) / 100)) $paused"
        if [ "$elapsed" -ge "$len" ]; then
            echo "ended $cur"
            if [ -n "$next" ]; then
                cur="$next"; next=""; elapsed=0
                case "$cur" in *bad*) echo "error $cur"; cur=""; echo idle ;; *) echo "started $cur" ;; esac
            else
                cur=""; echo idle
            fi
        fi
    fi
done
