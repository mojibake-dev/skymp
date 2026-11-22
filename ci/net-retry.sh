#!/bin/sh
# Runs one CI step and gives a failure that came from the network its own exit code, 75 (EX_TEMPFAIL in
# sysexits.h), which the jobs' `retry: exit_codes: [75]` retries; any other failure keeps its own code, so a
# build or test that fails on the code is never run again. A network failure is one of the fetches' own messages
# (ci/net-failures.txt: Docker Hub's, git's from GitHub, vcpkg's, cargo's) in the step's last 400 lines.
#
#   ci/net-retry.sh <command> [args...]
set -u
here=$(dirname "$0")
log=$(mktemp)
trap 'rm -f "$log" "$log.status"' EXIT
# the step's output streams as before; a copy is kept to read a failure by
{ "$@" 2>&1; echo $? > "$log.status"; } | tee "$log"
status=$(cat "$log.status")
[ "$status" -eq 0 ] && exit 0
seen=$(tail -n 400 "$log" | grep -E -f "$here/net-failures.txt" | tail -n 1)
if [ -n "$seen" ]; then
  echo "ci/net-retry: the step failed on the network (exit $status): $seen"
  echo "ci/net-retry: exit 75, so the job is retried"
  exit 75
fi
exit "$status"
