#!/usr/bin/env bash
# Runs feed_consumer and feed_replayer together on this host and prints both reports.
#
#   tools/feed_bench.sh FILE [replayer args...]
#
# Environment: CONSUMER_ARGS (extra consumer flags), CONSUMER_CPU (default 2),
# REPLAYER_CPU (default 4), IFACE (local interface address passed to both tools
# as --iface; default 127.0.0.1, which keeps the feed on the host. 0.0.0.0 lets
# the routing table pick, which on WSL also puts it onto the Hyper-V vSwitch via
# eth0). An --iface in CONSUMER_ARGS or the replayer args overrides IFACE.
# If the replayer args contain --from X and CONSUMER_ARGS has no --from, the
# consumer gets --from X too, so it measures only the paced part of the replay.
# Exits with the consumer's status, or the replayer's if the consumer succeeded.
set -uo pipefail
cd "$(dirname "$0")/.."

if [[ $# -lt 1 ]]; then
    echo "usage: $0 FILE [replayer args...]" >&2
    exit 2
fi
file=$1
shift
consumer_args=${CONSUMER_ARGS:-}
if [[ " $consumer_args " != *" --from "* ]]; then
    prev=
    for a in "$@"; do
        if [[ $prev == --from ]]; then
            consumer_args+=" --from $a"
            break
        fi
        prev=$a
    done
fi
iface=${IFACE:-127.0.0.1}
out=$(mktemp)
# shellcheck disable=SC2086  # CONSUMER_ARGS is meant to split into flags
./build/feed_consumer --cpu "${CONSUMER_CPU:-2}" --iface "$iface" $consumer_args >"$out" 2>&1 &
consumer=$!
trap 'kill "$consumer" 2>/dev/null; rm -f "$out"' EXIT

# Let the consumer join the group before the first packet (up to 5 s).
joined=0
for ((i = 0; i < 50; ++i)); do
    if grep -q '^joined' "$out"; then
        joined=1
        break
    fi
    if ! kill -0 "$consumer" 2>/dev/null; then
        wait "$consumer"
        echo "consumer exited ($?) before joining:"
        cat "$out"
        exit 1
    fi
    sleep 0.1
done
if [[ $joined -eq 0 ]]; then
    echo "consumer did not join within 5 s:"
    cat "$out"
    exit 1
fi

echo "--- replayer"
./build/feed_replayer "$file" --cpu "${REPLAYER_CPU:-4}" --iface "$iface" "$@"
replayer=$?

if [[ $replayer -ne 0 ]]; then
    # No end-of-session is coming. Give the consumer 3 s to finish, then stop it.
    for ((i = 0; i < 30; ++i)); do
        kill -0 "$consumer" 2>/dev/null || break
        sleep 0.1
    done
    kill "$consumer" 2>/dev/null
fi

wait "$consumer"
status=$?
echo "--- consumer (exit $status)"
cat "$out"
[[ $replayer -ne 0 ]] && echo "replayer exited $replayer"
exit $((status != 0 ? status : replayer))
