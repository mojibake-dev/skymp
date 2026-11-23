# Runs one CI step (a native command) and gives a failure that came from the network its own exit code, 75
# (EX_TEMPFAIL in sysexits.h), which the jobs' `retry: exit_codes: [75]` retries; any other failure keeps its
# own code, so a build or test that fails on the code is never run again. A network failure is one of the
# fetches' own messages (ci/net-failures.txt, shared with ci/net-retry.sh) in the step's last 400 lines. On
# Windows it wraps vcpkg's bootstrap, whose download has no retry of its own.
#
#   & ci/net-retry.ps1 <command> [args...]
$command, $rest = $args
$log = New-TemporaryFile
try {
  # the step's output streams as before; a copy is kept to read a failure by. Native stderr comes through as
  # text, never as a terminating error.
  $ErrorActionPreference = 'Continue'
  & $command @rest 2>&1 | ForEach-Object { "$_" } | Tee-Object -FilePath $log
  $status = $LASTEXITCODE
  if ($status -eq 0) { exit 0 }
  $patterns = Get-Content (Join-Path $PSScriptRoot 'net-failures.txt') | Where-Object { $_ }
  $seen = Get-Content $log -Tail 400 | Where-Object { $line = $_; $patterns | Where-Object { $line -match $_ } } | Select-Object -Last 1
  if ($seen) {
    Write-Output "ci/net-retry: the step failed on the network (exit $status): $seen"
    Write-Output "ci/net-retry: exit 75, so the job is retried"
    exit 75
  }
  exit $status
} finally {
  Remove-Item -Force $log -ErrorAction SilentlyContinue
}
