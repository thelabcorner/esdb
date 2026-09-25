[CmdletBinding()]
param(
    [string]$DllPath = "",
    [string]$CoreDllPath = "",
    [string]$WrapperPath = "",
    [string]$EstimerPath = "",
    [int]$Operations = 1000,
    [switch]$Launch
)

$ErrorActionPreference = "Stop"
$script:LaunchedIllustrator = $false
$app = $null

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $DllPath) {
    $DllPath = Join-Path $Root "orm-native\Release\UserOrmBridge.dll"
}
if (-not $CoreDllPath) {
    $CoreDllPath = Join-Path (Split-Path -Parent $DllPath) "ESDBCore.dll"
}
if (-not $WrapperPath) {
    $WrapperPath = Join-Path $Root "examples\orm\user\generated\es3\user_repository.jsx"
}
if (-not $EstimerPath) {
    $EstimerPath = Join-Path (Split-Path -Parent $Root) "estimer\dist\ESTIMER.jsx"
}
if ($Operations -lt 1) {
    throw "Operations must be >= 1."
}

$DllPath = (Resolve-Path -LiteralPath $DllPath).Path
$CoreDllPath = (Resolve-Path -LiteralPath $CoreDllPath).Path
$WrapperPath = (Resolve-Path -LiteralPath $WrapperPath).Path
$EstimerPath = (Resolve-Path -LiteralPath $EstimerPath).Path

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
$libraryName = "UserOrmBridgeBench$stamp"
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) $libraryName
New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
$tempDll = Join-Path $tempRoot ($libraryName + ".dll")
$tempCoreDll = Join-Path $tempRoot "ESDBCore.dll"
Copy-Item -LiteralPath $DllPath -Destination $tempDll -Force
Copy-Item -LiteralPath $CoreDllPath -Destination $tempCoreDll -Force

$tempRootJs = Js-String $tempRoot
$wrapperJs = Js-String $WrapperPath
$estimerJs = Js-String $EstimerPath
$dbPath = Join-Path ([IO.Path]::GetTempPath()) ($libraryName + ".sqlite")
$dbPathJs = Js-String $dbPath
$ops = [int]$Operations

$probe = @"
(function () {
    function fail(message) {
        throw new Error("ESDB ORM live benchmark: " + message);
    }
    function requireValue(condition, message) {
        if (!condition) fail(message);
    }

    ExternalObject.searchFolders = '$tempRootJs;' + ExternalObject.searchFolders;
    var estimerLoadError = "";
    try {
        $.evalFile(new File('$estimerJs'));
    } catch (estimerError) {
        estimerLoadError = String(estimerError);
    }
    $.evalFile(new File('$wrapperJs'));

    var EST = null;
    try {
        if (typeof ESTIMER != "undefined") EST = ESTIMER;
    } catch (directEstimerError) {}
    if (!EST) {
        try { EST = $.global.ESTIMER; } catch (globalEstimerError) {}
    }
    requireValue(EST && typeof EST.samples == "function",
        "ESTIMER did not load" + (estimerLoadError ? ": " + estimerLoadError : ""));
    requireValue(typeof ESDB_ORM_USER == "object", "generated facade did not load");

    ESDB_ORM_USER.load("lib:$libraryName");

    var dbFile = new File('$dbPathJs');
    if (dbFile.exists) dbFile.remove();

    var handle = ESDB_ORM_USER.open(dbFile);
    var repo = ESDB_ORM_USER.repository(handle);
    requireValue(repo.insert({
        id: 1,
        name: "Ada",
        email: "ada@example.com",
        createdAt: 1735689600
    }) == 1, "seed insert failed");
    requireValue(repo.findById(1).name == "Ada", "seed read failed");

    var OPS = $ops;
    var sink = 0;
    var emptyFn = function () {
        var i = 0;
        var x = sink;
        for (i = 0; i < OPS; i++) {
            x += (i & 1);
        }
        sink = x;
    };
    var findFn = function () {
        var i = 0;
        var row = null;
        for (i = 0; i < OPS; i++) {
            row = repo.findById(1);
        }
        if (!row || row.id !== 1 || row.name != "Ada") {
            fail("timed find returned wrong row");
        }
        sink += row.id;
    };

    EST.prime();
    var emptySamples = EST.samples(9, emptyFn, { warmup: 5, collectRejected: true });
    var findSamples = EST.samples(9, findFn, { warmup: 5, collectRejected: true });
    var emptyStats = EST.stats(emptySamples);
    var findStats = EST.stats(findSamples);

    requireValue(emptyStats.count > 0 && findStats.count > 0, "no valid ESTIMER samples");

    var rawPerOpUs = findStats.median / OPS;
    var correctedPerOpUs = (findStats.median - emptyStats.median) / OPS;
    if (correctedPerOpUs < 0) correctedPerOpUs = 0;

    ESDB_ORM_USER.close(handle);
    ESDB_ORM_USER.unload();

    var wal = new File('$dbPathJs-wal');
    var shm = new File('$dbPathJs-shm');
    if (dbFile.exists) dbFile.remove();
    if (wal.exists) wal.remove();
    if (shm.exists) shm.remove();

    var emptyRejected = emptySamples.rejected ? emptySamples.rejected.length : 0;
    var findRejected = findSamples.rejected ? findSamples.rejected.length : 0;

    return "OK|" + app.version + "|" + $.version + "|" + EST.lane() +
        "|ops=" + OPS +
        "|findMedianUs=" + findStats.median +
        "|emptyMedianUs=" + emptyStats.median +
        "|rawPerOpUs=" + rawPerOpUs +
        "|correctedPerOpUs=" + correctedPerOpUs +
        "|findSpreadUs=" + (findStats.max - findStats.min) +
        "|findRejected=" + findRejected +
        "|emptyRejected=" + emptyRejected;
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
