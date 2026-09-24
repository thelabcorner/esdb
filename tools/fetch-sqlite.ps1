# Fetches the pinned SQLite amalgamation into third_party/sqlite.
#
# The version and SHA3-256 hash below are the release facts published on
# https://sqlite.org/download.html. The build never uses a system SQLite and
# never falls back to "whatever is installed": a hash mismatch is a hard error.
#
# Usage:
#   pwsh -File tools/fetch-sqlite.ps1            # fetch if missing
#   pwsh -File tools/fetch-sqlite.ps1 -Force     # re-download and re-verify

[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

$Version = "3.53.4"
$Product = "3530400"
$Url = "https://www.sqlite.org/2026/sqlite-amalgamation-$Product.zip"
$Sha3 = "628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e"

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Dest = Join-Path $Root "third_party\sqlite"
$Marker = Join-Path $Dest ".fetched"
$Required = @("sqlite3.c", "sqlite3.h", "sqlite3ext.h")

function Get-Sha3_256 {
    param([Parameter(Mandatory = $true)][string]$Path)

    # Prefer the platform's own SHA3 support when present.
    if (Get-Command Get-FileHash -ErrorAction SilentlyContinue) {
        try {
            return (Get-FileHash -LiteralPath $Path -Algorithm SHA3-256).Hash.ToLowerInvariant()
        } catch {
            # Older PowerShell/.NET without SHA3 support falls through.
        }
    }
    try {
        [System.Security.Cryptography.SHA3_256]::Create().Dispose()
        $stream = [System.IO.File]::OpenRead($Path)
        try {
            $sha = [System.Security.Cryptography.SHA3_256]::Create()
            try {
                $bytes = $sha.ComputeHash($stream)
                return ([System.BitConverter]::ToString($bytes) -replace "-", "").ToLowerInvariant()
            } finally {
                $sha.Dispose()
            }
        } finally {
            $stream.Dispose()
        }
    } catch {
        # Fall back to Node's OpenSSL digest (Node is already required to build
        # the repository's tooling).
    }

    $node = Get-Command node -ErrorAction SilentlyContinue
    if (-not $node) {
        throw "No SHA3-256 implementation available. Install Node.js or a PowerShell/.NET runtime with SHA3 support."
    }
    $digest = & node -e "const c=require('crypto'),f=require('fs');process.stdout.write(c.createHash('sha3-256').update(f.readFileSync(process.argv[1])).digest('hex'))" $Path
    if ($LASTEXITCODE -ne 0 -or -not $digest) {
        throw "Node SHA3-256 computation failed for $Path"
    }
    return ([string]$digest).Trim().ToLowerInvariant()
}

New-Item -ItemType Directory -Force -Path $Dest | Out-Null

$present = $true
foreach ($name in $Required) {
    if (-not (Test-Path -LiteralPath (Join-Path $Dest $name))) { $present = $false }
}

if ($present -and -not $Force) {
    if (Test-Path -LiteralPath $Marker) {
        $record = Get-Content -LiteralPath $Marker -Raw
        if ($record -match [regex]::Escape($Version) -and $record -match $Sha3) {
            Write-Output "SQLite $Version amalgamation already present and pinned ($Dest)"
            exit 0
        }
    }
    Write-Output "Existing SQLite files have no matching pin record; refetching."
}

$tmp = Join-Path ([System.IO.Path]::GetTempPath()) "sqlite-amalgamation-$Product.zip"
$extract = Join-Path ([System.IO.Path]::GetTempPath()) "sqlite-amalgamation-$Product"

Write-Output "Downloading $Url"
Invoke-WebRequest -Uri $Url -OutFile $tmp

$actual = Get-Sha3_256 -Path $tmp
if ($actual -ne $Sha3) {
    Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
    throw "SQLite amalgamation hash mismatch. Expected SHA3-256 $Sha3, got $actual. Refusing to use an unverified download."
}

if (Test-Path -LiteralPath $extract) {
    Remove-Item -LiteralPath $extract -Recurse -Force
}
Expand-Archive -LiteralPath $tmp -DestinationPath $extract
$inner = Get-ChildItem -LiteralPath $extract -Directory | Select-Object -First 1
if (-not $inner) {
    throw "Unexpected amalgamation archive layout: no inner directory in $extract"
}

foreach ($name in $Required) {
    Copy-Item -LiteralPath (Join-Path $inner.FullName $name) -Destination (Join-Path $Dest $name) -Force
}

@(
    "sqlite_version=$Version",
    "sqlite_product=$Product",
    "sha3_256=$Sha3",
    "source=$Url",
    "fetched_utc=$([DateTime]::UtcNow.ToString('yyyy-MM-ddTHH:mm:ssZ'))"
) | Set-Content -LiteralPath $Marker -Encoding utf8

Remove-Item -LiteralPath $tmp -Force -ErrorAction SilentlyContinue
Remove-Item -LiteralPath $extract -Recurse -Force -ErrorAction SilentlyContinue

Write-Output "Verified and installed SQLite $Version into $Dest"
