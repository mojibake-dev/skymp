#!/bin/sh
# ci/net-retry.sh's exits, each on a step whose output is a fetch's own failure or none: 0 for a passing step,
# the step's own code for a failure of the code, and 75 for a network failure in the step's last 400 lines.
# The network lines are the fetches' own, as CI's jobs printed them.
#
#   sh ci/net-retry.test.sh
set -u
here=$(dirname "$0")
failed=0
expect() {
  want=$1; name=$2; shift 2
  out=$("$here/net-retry.sh" "$@" 2>&1)
  got=$?
  if [ "$got" -eq "$want" ]; then
    echo "ok - $name"
  else
    echo "not ok - $name: exit $got, $want wanted"
    echo "$out" | tail -n 5 | sed 's/^/    /'
    failed=1
  fi
}

expect 0 "a step that passes exits 0" sh -c 'echo built'
expect 3 "a step that fails on the code keeps its own code" sh -c 'echo "error: expected one of"; exit 3'
expect 75 "BuildKit's Docker Hub token fetch is a network failure" sh -c \
  'echo "ERROR: failed to solve: DeadlineExceeded: failed to fetch anonymous token: Get \"https://auth.docker.io/token?scope=repository%3Askymp%2Fskymp-runtime-base%3Apull&service=registry.docker.io\": dial tcp: i/o timeout"; exit 1'
expect 75 "cargo-deny's advisory database fetch is a network failure" sh -c \
  "echo \"fatal: unable to access 'https://github.com/RustSec/advisory-db/': Failed to connect to github.com port 443 after 130836 ms: Couldn't connect to server\"; exit 1"
expect 75 "vcpkg's bootstrap download is a network failure" sh -c \
  'echo "bootstrap.ps1 : Downloading vcpkg.exe failed. Please check your internet connection"; exit 1'
expect 1 "a network line more than 400 lines before the failure is not the failure" sh -c \
  'echo "failed to fetch anonymous token"; i=0; while [ $i -lt 401 ]; do echo "line $i"; i=$((i + 1)); done; echo "error: a test failed"; exit 1'
expect 0 "a passing step that printed a network line still exits 0" sh -c 'echo "TLS handshake timeout, retrying"; exit 0'

exit $failed
