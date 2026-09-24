[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Script = Join-Path $Root "tools\fetch-sqlite.cmake"

$cmake = Get-Command cmake -ErrorAction SilentlyContinue
if (-not $cmake) {
    throw "CMake 3.20+ is required to verify/repair the pinned SQLite dependency."
}

$args = @()
if ($Force) {
    $args += "-DESDB_SQLITE_FORCE=ON"
}
$args += @("-P", $Script)

& $cmake.Source @args
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}
