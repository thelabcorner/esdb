[CmdletBinding()]
param(
    [string]$DllPath = "",
    [string]$CoreDllPath = "",
    [string]$WrapperPath = "",
    [switch]$Launch
)

$ErrorActionPreference = "Stop"
$script:LaunchedIllustrator = $false
$app = $null

$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
if (-not $DllPath) {
    $DllPath = Join-Path $Root "tests\orm\.work\user-native\Release\UserOrmBridge.dll"
}
if (-not $CoreDllPath) {
    $CoreDllPath = Join-Path (Split-Path -Parent $DllPath) "ESDBCore.dll"
}
if (-not $WrapperPath) {
    $WrapperPath = Join-Path $Root "examples\orm\user\generated\es3\user_repository.jsx"
}

$DllPath = (Resolve-Path -LiteralPath $DllPath).Path
$CoreDllPath = (Resolve-Path -LiteralPath $CoreDllPath).Path
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
$libraryName = "UserOrmBridgeLive$stamp"
$tempRoot = Join-Path ([IO.Path]::GetTempPath()) $libraryName
New-Item -ItemType Directory -Force -Path $tempRoot | Out-Null
$tempDll = Join-Path $tempRoot ($libraryName + ".dll")
$tempCoreDll = Join-Path $tempRoot "ESDBCore.dll"
Copy-Item -LiteralPath $DllPath -Destination $tempDll -Force
Copy-Item -LiteralPath $CoreDllPath -Destination $tempCoreDll -Force

$tempRootJs = Js-String $tempRoot
$wrapperJs = Js-String $WrapperPath
$rocket = [char]::ConvertFromUtf32(0x1F680)
$dbPath = Join-Path ([IO.Path]::GetTempPath()) ($libraryName + "-" + $rocket + ".sqlite")
$dbPathJs = Js-String $dbPath

$probe = @"
(function () {
    function fail(message) {
        throw new Error("ESDB ORM live probe: " + message);
    }
    function requireValue(condition, message) {
        if (!condition) fail(message);
    }

    ExternalObject.searchFolders = '$tempRootJs;' + ExternalObject.searchFolders;
    $.evalFile(new File('$wrapperJs'));

    requireValue(typeof ESDB_ORM_USER == "object", "generated facade did not load");
    requireValue(ESDB_ORM_USER.irHash == "f619d9734d3872bd3b788d50799a02a82ac83d026cb824ae408ba3588c86c5ee",
        "unexpected generated IR hash");

    ESDB_ORM_USER.load("lib:$libraryName");

    var dbFile = new File('$dbPathJs');
    if (dbFile.exists) dbFile.remove();

    var handle = ESDB_ORM_USER.open(dbFile);
    requireValue(typeof handle == "number" && handle > 0, "database handle did not open");

    var repo = ESDB_ORM_USER.repository(handle);

    requireValue(repo.insert({
        id: 1,
        name: "Ada",
        email: "ada@example.com",
        createdAt: 1735689600
    }) == 1, "safe-row insert failed");

    var ada = repo.findById(1);
    requireValue(ada && ada.id === 1 && ada.name == "Ada" &&
        ada.email == "ada@example.com" && ada.createdAt === 1735689600,
        "safe-row round-trip failed");

    requireValue(repo.updateName(1, "Ada King") == 1, "updateName failed");
    ada = repo.findByEmail("ada@example.com");
    requireValue(ada && ada.name == "Ada King", "findByEmail/update round-trip failed");

    var exactId = "9007199254740993";
    var exactCreated = "9223372036854775807";
    requireValue(repo.insert({
        id: exactId,
        name: "Exact",
        email: "exact@example.com",
        createdAt: exactCreated
    }) == 1, "exact-int64 insert failed");

    var exact = repo.findById(exactId, { exactIntegers: true });
    requireValue(exact && exact.id === exactId && exact.createdAt === exactCreated,
        "exact-int64 row rounded across Illustrator/ESABI");

    var list = repo.list(10, 0, { exactIntegers: true });
    requireValue(list.length == 2, "list did not return both rows");
    requireValue(typeof list[0].id == "string" && typeof list[1].id == "string",
        "exact list did not preserve integer strings");

    var unloadBlocked = false;
    try {
        ESDB_ORM_USER.unload();
    } catch (unloadError) {
        unloadBlocked = true;
    }
    requireValue(unloadBlocked, "facade unloaded with an open ORM handle");

    requireValue(repo.deleteById(1) == 1, "delete safe row failed");
    requireValue(repo.deleteById(exactId) == 1, "delete exact row failed");
    requireValue(ESDB_ORM_USER.close(handle) === true, "close failed");

    var staleRejected = false;
    try {
        repo.findById(1);
    } catch (staleError) {
        staleRejected = true;
    }
    requireValue(staleRejected, "closed generation-tagged handle was accepted");
    var last = ESDB_ORM_USER.lastError();
    requireValue(last && last.ok === false && last.status == 45,
        "stale-handle error was not preserved by ormLastError");

    requireValue(ESDB_ORM_USER.unload() === true, "unload failed");

    var wal = new File('$dbPathJs-wal');
    var shm = new File('$dbPathJs-shm');
    if (dbFile.exists) dbFile.remove();
    if (wal.exists) wal.remove();
    if (shm.exists) shm.remove();

    return "OK|" + app.version + "|" + $.version +
        "|orm=1|crud=1|update=1|int64=1|stale=1|unload=1";
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
