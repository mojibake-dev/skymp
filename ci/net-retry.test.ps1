# ci/net-retry.ps1's exits, each on a step whose output is a fetch's own failure or none: 0 for a passing step,
# the step's own code for a failure of the code, and 75 for a network failure in the step's last 400 lines.
# Each step is a child pwsh given its script by -EncodedCommand, so no quoting crosses the process boundary.
#
#   & ci/net-retry.test.ps1
$pwsh = (Get-Process -Id $PID).Path
$wrapper = Join-Path $PSScriptRoot 'net-retry.ps1'
$failed = 0
function Expect([int]$want, [string]$name, [string]$step) {
  $encoded = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($step))
  $out = & $wrapper $pwsh -NoProfile -NonInteractive -EncodedCommand $encoded 2>&1
  $got = $LASTEXITCODE
  if ($got -eq $want) {
    Write-Output "ok - $name"
  } else {
    Write-Output "not ok - ${name}: exit $got, $want wanted"
    $out | Select-Object -Last 5 | ForEach-Object { "    $_" }
    $script:failed = 1
  }
}

Expect 0 'a step that passes exits 0' "Write-Output 'built'"
Expect 3 'a step that fails on the code keeps its own code' "Write-Output 'error: expected one of'; exit 3"
Expect 75 "vcpkg's bootstrap download is a network failure" "Write-Output 'bootstrap.ps1 : Downloading vcpkg.exe failed. Please check your internet connection'; exit 1"
Expect 75 "BuildKit's Docker Hub token fetch is a network failure" "Write-Output 'ERROR: failed to solve: DeadlineExceeded: failed to fetch anonymous token'; exit 1"
Expect 1 'a network line more than 400 lines before the failure is not the failure' "Write-Output 'Downloading vcpkg.exe failed'; 1..401 | ForEach-Object { Write-Output ""line `$_"" }; Write-Output 'error: a test failed'; exit 1"
Expect 0 'a passing step that printed a network line still exits 0' "Write-Output 'TLS handshake timeout, retrying'; exit 0"

exit $failed
