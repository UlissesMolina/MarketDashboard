#!/usr/bin/env bash
# Runs feed_consumer and feed_replayer together on this host and prints both reports.
#
#   tools/feed_bench.sh FILE [replayer args...]
#
# Environment: CONSUMER_ARGS (extra consumer flags), CONSUMER_CPU (default 2),
# REPLAYER_CPU (default 4). Exits with the consumer's status.
set -uo pipefail
cd "$(dirname "$0")/.."

file=$1
shift
out=$(mktemp)
# shellcheck disable=SC2086  # CONSUMER_ARGS is meant to split into flags
./build/feed_consumer --cpu "${CONSUMER_CPU:-2}" ${CONSUMER_ARGS:-} >"$out" 2>&1 &
consumer=$!
trap 'kill "$consumer" 2>/dev/null; rm -f "$out"' EXIT
sleep 1  # let the consumer join the group before the first packet

echo "--- replayer"
./build/feed_replayer "$file" --cpu "${REPLAYER_CPU:-4}" "$@"
replayer=$?

wait "$consumer"
status=$?
echo "--- consumer (exit $status)"
cat "$out"
[[ $replayer -ne 0 ]] && echo "replayer exited $replayer"
exit "$status"
