[CmdletBinding()]
param(
    [string]$DllPath = "",
    [string]$WrapperPath = "",
    [switch]$Launch
)

$ErrorActionPreference = "Stop"
$script:LaunchedIllustrator = $false
$app = $null

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $DllPath) {
    $DllPath = Join-Path $Root "build-final\Release\ESDB.dll"
}
if (-not $WrapperPath) {
    $WrapperPath = Join-Path $Root "extendscript\esdb.jsx"
}

$DllPath = (Resolve-Path -LiteralPath $DllPath).Path
$WrapperPath = (Resolve-Path -LiteralPath $WrapperPath).Path

function Get-Illustrator {
    try {
        return [Runtime.InteropServices.Marshal]::GetActiveObject("Illustrator.Application")
    } catch {
        if (-not $Launch) {
            throw "No running Illustrator.Application COM instance. Re-run with -Launch only when launching Illustrator is intentional."
        }
        $script:LaunchedIllustrator = $true
        return New-Object -ComObject Illustrator.Application
    }
}

function Js-String([string]$Value) {
    return ($Value.Replace("\", "/").Replace("'", "\'"))
}

$stamp = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
$libraryName = "ESDBLive$stamp"
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) $libraryName
New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
$tempDll = Join-Path $tempRoot ($libraryName + ".dll")
Copy-Item -LiteralPath $DllPath -Destination $tempDll -Force

$tempRootJs = Js-String $tempRoot
$wrapperJs = Js-String $WrapperPath
$dbPath = Join-Path ([IO.Path]::GetTempPath()) ($libraryName + ".sqlite")
$dbPathJs = Js-String $dbPath

$probe = @"
(function () {
    function fail(message) {
        throw new Error("ESDB live probe: " + message);
    }
    function requireValue(condition, message) {
        if (!condition) fail(message);
    }

    ExternalObject.searchFolders = '$tempRootJs;' + ExternalObject.searchFolders;

    $.evalFile(new File('$wrapperJs'));
    requireValue(typeof ESDB == "object", "JSX facade did not load");

    var missingFailed = false;
    try {
        ESDB.load("lib:$libraryName-Missing");
    } catch (missingError) {
        missingFailed = true;
    }
    requireValue(missingFailed, "missing-library load did not fail");
    requireValue(ESDB.loadedSpec() === null, "failed load published facade state");

    ESDB.load("lib:$libraryName");
    var esdbVersion = ESDB.version();
    var sqliteVersion = ESDB.sqliteVersion();
    requireValue(esdbVersion == "0.1.0", "unexpected ESDB version");
    requireValue(sqliteVersion == "3.53.4", "unexpected SQLite version");
    requireValue(ESDB.abiVersion() == 1, "unexpected ESDB ABI version");
    requireValue(ESDB.handleCount() == 0, "initial handle count is not zero");

    var dbFile = new File('$dbPathJs');
    if (dbFile.exists) dbFile.remove();

    var db = ESDB.open(dbFile);
    requireValue(db.isOpen(), "database did not open");
    requireValue(ESDB.handleCount() == 1, "handle count did not reach one");
    var handle = db._handle;

    var health = db.healthJSON();
    requireValue(typeof health == "string", "health did not return a string");
    requireValue(health.indexOf('"ok":true') >= 0, "health payload was not successful");
    requireValue(health.indexOf('"journalMode":') >= 0, "health payload omitted journal mode");

    var dataVersion = db.dataVersion();
    requireValue(typeof dataVersion == "number" && dataVersion >= 0, "invalid data_version");

    var unloadBlocked = false;
    try {
        ESDB.unload();
    } catch (unloadWhileOpenError) {
        unloadBlocked = true;
    }
    requireValue(unloadBlocked, "facade allowed unload with an open database");
    requireValue(ESDB.handleCount() == 1, "blocked unload disturbed the open handle");

    requireValue(db.close() === true, "database close failed");
    requireValue(ESDB.handleCount() == 0, "handle count did not return to zero");

    var raw = new ExternalObject("lib:$libraryName");
    var stale = raw.health(handle);
    requireValue(typeof stale == "undefined", "stale handle unexpectedly resolved");
    var staleError = String(raw.lastError(0));
    requireValue(staleError.indexOf('"adapterCode":5') >= 0, "stale-handle error code missing");
    raw = null;

    requireValue(ESDB.unload() === true, "facade unload failed");
    requireValue(ESDB.loadedSpec() === null, "facade unload retained loadedSpec");

    var wal = new File('$dbPathJs-wal');
    var shm = new File('$dbPathJs-shm');
    if (dbFile.exists) dbFile.remove();
    if (wal.exists) wal.remove();
    if (shm.exists) shm.remove();

    return "OK|" + app.version + "|" + $.version +
        "|esdb=" + esdbVersion +
        "|sqlite=" + sqliteVersion +
        "|dataVersion=" + dataVersion +
        "|recovery=1|unload=1|stale=1";
}());
"@

try {
    $app = Get-Illustrator
    $result = [string]$app.DoJavaScript($probe)
    Write-Output $result
    if (-not $result.StartsWith("OK|")) {
        exit 1
    }
} finally {
    foreach ($suffix in @("", "-wal", "-shm")) {
        $target = $dbPath + $suffix
        if (Test-Path -LiteralPath $target) {
            Remove-Item -LiteralPath $target -Force -ErrorAction SilentlyContinue
        }
    }
    for ($i = 0; $i -lt 20; $i++) {
        try {
            if (Test-Path -LiteralPath $tempRoot) {
                Remove-Item -LiteralPath $tempRoot -Recurse -Force -ErrorAction Stop
            }
            break
        } catch {
            Start-Sleep -Milliseconds 100
        }
    }
    if ($script:LaunchedIllustrator -and $null -ne $app) {
        try { $app.Quit() } catch { }
    }
}
