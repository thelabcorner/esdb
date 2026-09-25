[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$Dll
)

$ErrorActionPreference = "Stop"

if (-not (Test-Path -LiteralPath $Dll)) {
    throw "ESDB ExternalObject DLL not found: $Dll"
}

$dumpbinCommand = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
$dumpbinPath = if ($dumpbinCommand) { $dumpbinCommand.Source } else { $null }
if (-not $dumpbinPath) {
    $vswhere = Join-Path ([Environment]::GetFolderPath("ProgramFilesX86")) `
        "Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $dumpbinCandidates = @(
            & $vswhere -latest -products '*' `
                -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
                -find 'VC\Tools\MSVC\**\bin\Hostx64\x64\dumpbin.exe' 2>$null
        )
        if ($LASTEXITCODE -eq 0 -and $dumpbinCandidates.Count -gt 0 -and
            (Test-Path -LiteralPath $dumpbinCandidates[0])) {
            $dumpbinPath = $dumpbinCandidates[0]
        }
    }
}
if (-not $dumpbinPath) {
    throw "dumpbin.exe was not found on PATH or in the latest Visual Studio x64 C++ tools installation."
}

$headers = & $dumpbinPath /headers $Dll 2>&1
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
    "querySql",
    "handleCount",
    "health",
    "lastError",
    "openStaged",
    "ping",
    "sqliteVersion",
    "stage",
    "stageHex",
    "objectStoreChanges",
    "objectStoreCount",
    "objectStoreDelete",
    "objectStoreEnsure",
    "objectStoreExists",
    "objectStoreGet",
    "objectStorePrune",
    "objectStorePutNumber",
    "objectStorePutText",
    "objectStoreScan",
    "objectStoreRevision",
    "storeChanges",
    "storeClear",
    "storeCount",
    "storeDelete",
    "storeDestroy",
    "storeExists",
    "storeGet",
    "storePutNumber",
    "storePutText",
    "storePatch",
    "storeRetainedFloor",
    "storeRevision",
    "storeScan",
    "transactionActive",
    "transactionBegin",
    "transactionCommit",
    "transactionRollback",
    "version"
) | Sort-Object

$exportsRaw = & $dumpbinPath /exports $Dll 2>&1
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
