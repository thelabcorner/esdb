[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Dll
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Dll)) {
    throw "ESDB ExternalObject DLL not found: $Dll"
}

$dumpbin = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
if (-not $dumpbin) {
    throw "dumpbin.exe is required. Run this script from a Visual Studio developer environment."
}

$headers = & $dumpbin.Source /headers $Dll 2>&1
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin /headers failed for $Dll"
}
if (($headers -join "`n") -notmatch '(?im)8664 machine \(x64\)') {
    throw "ESDB ExternalObject is not a PE x64 image."
}

$expected = @(
    "ESFreeMem",
    "ESGetVersion",
    "ESInitialize",
    "ESTerminate",
    "abiVersion",
    "close",
    "dataVersion",
    "handleCount",
    "health",
    "lastError",
    "openStaged",
    "ping",
    "sqliteVersion",
    "stage",
    "version"
) | Sort-Object

$exportsRaw = & $dumpbin.Source /exports $Dll 2>&1
if ($LASTEXITCODE -ne 0) {
    throw "dumpbin /exports failed for $Dll"
}

$actual = @(
    foreach ($line in $exportsRaw) {
        if ($line -match '^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]+\s+(\S+)\s*$' ) {
            $Matches[1]
        }
    }
) | Sort-Object -Unique

$missing = @($expected | Where-Object { $_ -notin $actual })
$unexpected = @($actual | Where-Object { $_ -notin $expected })

if ($missing.Count -ne 0 -or $unexpected.Count -ne 0) {
    $message = "ExternalObject export mismatch." + [Environment]::NewLine +
        "Missing: " + ($missing -join ", ") + [Environment]::NewLine +
        "Unexpected: " + ($unexpected -join ", ")
    throw $message
}

Write-Output ("ESDB ExternalObject exports: PASS ({0} exact named exports, PE x64)" -f $actual.Count)
