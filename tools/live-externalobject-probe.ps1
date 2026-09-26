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
    $DllPath = Join-Path $Root "build-final\Release\ESDB.dll"
}
if (-not $CoreDllPath) {
    $CoreDllPath = Join-Path (Split-Path -Parent $DllPath) "ESDBCore.dll"
}
if (-not $WrapperPath) {
    $WrapperPath = Join-Path $Root "extendscript\esdb.jsx"
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
$libraryName = "ESDBLive$stamp"
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
    requireValue(esdbVersion == "0.2.1", "unexpected ESDB version");
    requireValue(sqliteVersion == "3.53.4", "unexpected SQLite version");
    requireValue(ESDB.abiVersion() == 2, "unexpected ESDB ABI version");
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


    /*
     * Advanced typed SQL escape hatch. SQL text and values cross ExternalObject
     * in separate arguments; values are bound natively, never interpolated.
     */
    var rawCreate = db.run(
        "CREATE TABLE raw_live(" +
        "id INTEGER PRIMARY KEY, label TEXT NOT NULL, payload BLOB NOT NULL, " +
        "score REAL NOT NULL, optional TEXT, flag INTEGER NOT NULL, small INTEGER NOT NULL)"
    );
    requireValue(String(rawCreate.changes) == "0", "raw DDL returned unexpected change count");

    var injectionText = "x'); DROP TABLE raw_live; --";
    var rawInsert = db.run(
        "INSERT INTO raw_live(id,label,payload,score,optional,flag,small) VALUES(?,?,?,?,?,?,?)",
        [
            ESDB.int64("9007199254740993"),
            injectionText,
            ESDB.bytes("00FF1080"),
            3.25,
            null,
            true,
            -123
        ]
    );
    requireValue(String(rawInsert.changes) == "1", "raw INSERT did not report one change");

    var rawRead = db.query(
        "SELECT id,label,payload,score,optional,flag,small FROM raw_live WHERE id=?",
        [ESDB.int64("9007199254740993")],
        { maxRows: 10 }
    );
    requireValue(rawRead.columns.length == 7, "raw SELECT column metadata missing");
    requireValue(rawRead.rows.length == 1 && rawRead.returnedRowCount == 1, "raw SELECT row count mismatch");
    requireValue(String(rawRead.totalRowCount) == "1", "raw SELECT total row count mismatch");
    requireValue(rawRead.truncated === false, "raw SELECT unexpectedly truncated");
    requireValue(rawRead.rows[0][0] instanceof ESDB.Int64, "raw INTEGER did not decode as exact Int64");
    requireValue(String(rawRead.rows[0][0]) == "9007199254740993", "raw INTEGER rounded through ExternalObject");
    requireValue(rawRead.rows[0][1] === injectionText, "bound injection-shaped string changed");
    requireValue(rawRead.rows[0][2] instanceof ESDB.Bytes, "raw BLOB did not decode as Bytes");
    requireValue(rawRead.rows[0][2].toHex() == "00FF1080", "raw BLOB changed");
    requireValue(rawRead.rows[0][3] === 3.25, "raw REAL changed");
    requireValue(rawRead.rows[0][4] === null, "raw NULL changed");
    requireValue(
        rawRead.rows[0][5] instanceof ESDB.Int64 && String(rawRead.rows[0][5]) == "1",
        "raw BOOL parameter was not bound as SQLite INTEGER"
    );
    requireValue(
        rawRead.rows[0][6] instanceof ESDB.Int64 && String(rawRead.rows[0][6]) == "-123",
        "raw INT32 parameter was not bound as SQLite INTEGER"
    );

    var rawEmptyResult = db.query(
        "SELECT 1 AS empty_result WHERE 0",
        [],
        { maxRows: 10 }
    );
    requireValue(
        rawEmptyResult.columns.length == 1 &&
        rawEmptyResult.columns[0] == "empty_result",
        "zero-row raw SELECT lost its column metadata"
    );
    requireValue(
        rawEmptyResult.rows.length == 0 &&
        String(rawEmptyResult.totalRowCount) == "0",
        "zero-row raw SELECT reported a non-empty result"
    );

    /* If label had been interpolated instead of bound, this table would be gone. */
    var tableStillThere = db.query(
        "SELECT count(*) FROM raw_live",
        [],
        { maxRows: 1 }
    );
    requireValue(
        String(tableStillThere.rows[0][0]) == "1",
        "injection-shaped bound value altered SQL structure"
    );

    db.transaction(function (transactionDb) {
        var txInsert = transactionDb.run(
            "INSERT INTO raw_live(id,label,payload,score,optional,flag,small) VALUES(?,?,?,?,?,?,?)",
            [2, "transactional", ESDB.bytes(""), -0, "committed", false, 123]
        );
        requireValue(String(txInsert.changes) == "1", "raw SQL transaction insert failed");
    });
    requireValue(db.transactionActive() === false, "raw SQL left transaction active");

    var rawRollbackObserved = false;
    try {
        db.transaction(function (transactionDb) {
            transactionDb.run(
                "INSERT INTO raw_live(id,label,payload,score,optional,flag,small) VALUES(?,?,?,?,?,?,?)",
                [3, "must-rollback", ESDB.bytes("AA"), 1.5, null, true, -7]
            );
            throw new Error("intentional raw SQL rollback");
        });
    } catch (rawRollbackError) {
        rawRollbackObserved = true;
    }
    requireValue(rawRollbackObserved, "raw SQL transaction rollback exception did not propagate");
    requireValue(db.transactionActive() === false, "raw SQL rollback left transaction active");
    var rolledBackRaw = db.query(
        "SELECT count(*) FROM raw_live WHERE id=?",
        [3],
        { maxRows: 1 }
    );
    requireValue(String(rolledBackRaw.rows[0][0]) == "0", "raw SQL rollback leaked inserted row");

    var rawWindow = db.query(
        "SELECT id FROM raw_live ORDER BY id",
        [],
        { maxRows: 1 }
    );
    requireValue(rawWindow.rows.length == 1, "raw SQL maxRows did not cap returned rows");
    requireValue(String(rawWindow.totalRowCount) == "2", "raw SQL total row count lost after truncation");
    requireValue(rawWindow.truncated === true, "raw SQL truncation flag missing");

    var mismatchRejected = false;
    try {
        db.query("SELECT ?", [], { maxRows: 1 });
    } catch (rawMismatchError) {
        mismatchRejected = true;
    }
    requireValue(mismatchRejected, "raw SQL placeholder mismatch was not rejected");

    /*
     * Durable Runtime/ObjectStore plane. This is SQLite-backed state and is
     * transactionally persistent across close/reopen.
     */
    var durable = db.objectStore("settings🚀").ensure();
    var unicodeKey = "theme🚀";
    var unicodeValue = "A\u0000B🚀";
    var rev1 = durable.set(unicodeKey, unicodeValue);
    requireValue(typeof rev1 == "string" && rev1 != "0", "UTF-8 ObjectStore write returned invalid revision");
    requireValue(durable.get(unicodeKey) === unicodeValue, "embedded-NUL/astral UTF-8 value did not round-trip");

    var exact = ESDB.int64("9007199254740993");
    durable.set("exact-int64", exact);
    var exactRead = durable.get("exact-int64");
    requireValue(exactRead instanceof ESDB.Int64, "INT64 did not return Int64 marker");
    requireValue(String(exactRead) == "9007199254740993", "INT64 rounded across ExternalObject");

    durable.set("bytes", ESDB.bytes("00FF1080"));
    var bytesRead = durable.get("bytes");
    requireValue(bytesRead instanceof ESDB.Bytes, "BYTES did not return Bytes marker");
    requireValue(bytesRead.toHex() == "00FF1080", "BYTES payload changed");

    var rollbackObserved = false;
    try {
        db.transaction(function () {
            durable.set("rolled-back", "must-not-survive");
            throw new Error("intentional rollback");
        });
    } catch (rollbackError) {
        rollbackObserved = true;
    }
    requireValue(rollbackObserved, "transaction callback throw did not propagate");
    requireValue(!durable.has("rolled-back"), "SQLite rollback leaked ObjectStore value");
    requireValue(db.transactionActive() === false, "transaction remained active after rollback");

    db.transaction(function () {
        durable.set("committed-a", 1);
        durable.set("committed-b", 2);
    });
    requireValue(durable.get("committed-a") === 1 && durable.get("committed-b") === 2, "transaction commit lost ObjectStore values");

    var durableWindow = durable.changesSince("0", 100);
    requireValue(
        durableWindow.count == 5,
        "ObjectStore change window omitted mutations: count=" + durableWindow.count +
        ", revision=" + durable.revision() +
        ", lastRevision=" + durableWindow.lastRevision
    );
    var sawUnicode = false;
    var wi;
    for (wi = 0; wi < durableWindow.changes.length; wi += 1) {
        if (durableWindow.changes[wi].key === unicodeKey) {
            sawUnicode = true;
            break;
        }
    }
    requireValue(sawUnicode, "ObjectStore change window did not preserve Unicode key");

    var beforePruneRevision = durable.revision();
    var pruned = db.pruneObjectChanges(beforePruneRevision);
    requireValue(typeof pruned == "string", "ObjectStore prune did not return exact count");
    requireValue(durable.revision() == beforePruneRevision, "prune reset durable ObjectStore revision");
    requireValue(durable.count() >= 1, "prune modified ObjectStore contents");

    /*
     * Zustand-shaped Store plane. This is process memory only: no SQLite
     * transaction or disk flush occurs on set/patch.
     */
    var store = ESDB.store("ui🚀");
    store.set("negative-zero", -0);
    var negativeZero = store.get("negative-zero");
    requireValue(negativeZero === 0 && (1 / negativeZero) < 0, "memory Store lost negative zero");

    store.set("nan", Number("NaN"));
    requireValue(isNaN(store.get("nan")), "memory Store NaN did not round-trip");
    store.set("positive-infinity", Number("Infinity"));
    requireValue(store.get("positive-infinity") > 1e300, "memory Store infinity did not round-trip");

    var patchRevision = store.patch({
        zoom: 1.25,
        grid: true,
        label: "session🚀"
    });
    requireValue(typeof patchRevision == "string", "memory Store patch did not return exact revision");
    requireValue(
        store.get("zoom") === 1.25 &&
        store.get("grid") === true &&
        store.get("label") === "session🚀",
        "atomic memory Store patch values missing"
    );

    var entries = store.entries(100);
    requireValue(entries.length >= 6, "memory Store scan omitted values");

    var subscriptionCalls = 0;
    var subscriptionStart = store.revision();
    var subscription = store.subscribe("watched", function (change) {
        subscriptionCalls += 1;
        requireValue(change.key == "watched", "key-filtered memory subscription delivered wrong key");
    }, subscriptionStart);
    store.set("not-watched", 1);
    store.set("watched", 2);
    var polled = subscription.poll(100);
    requireValue(subscriptionCalls == 1 && polled.count == 1, "memory subscription did not deliver exactly one filtered change");
    requireValue(subscription.poll(100).count == 0, "memory subscription replayed acknowledged changes");
    subscription.close();

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

    /* Store lifetime is independent of the durable Runtime connection. */
    requireValue(store.get("label") === "session🚀", "memory Store disappeared when SQLite closed");

    /* Durable ObjectStore survives close/reopen of the SQLite Runtime. */
    var reopened = ESDB.open(dbFile);
    var durableReopened = reopened.objectStore("settings🚀");
    requireValue(durableReopened.get(unicodeKey) === unicodeValue, "ObjectStore value did not persist across reopen");
    requireValue(durableReopened.get("committed-b") === 2, "committed ObjectStore value did not persist");
    requireValue(reopened.close() === true, "reopened database close failed");
    requireValue(ESDB.handleCount() == 0, "reopened handle did not close");

    var raw = new ExternalObject("lib:$libraryName");
    var stale = raw.health(handle);
    requireValue(typeof stale == "undefined", "stale handle unexpectedly resolved");
    var staleError = String(raw.lastError(0));
    requireValue(staleError.indexOf('"adapterCode":5') >= 0, "stale-handle error code missing");
    raw = null;

    requireValue(store.destroy() === true, "memory Store destroy failed");
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
        "|memoryStore=1|objectStore=1|persistence=1|unicode=1|int64=1|bytes=1" +
        "|rawSql=1|rawBind=1|rawExactInt64=1|rawBool=1|rawInt32=1|rawEmptyMetadata=1|rawTruncate=1|rawInjectionSafe=1|rawRollback=1" +
        "|tx=1|subscription=1|prune=1|recovery=1|unload=1|stale=1";
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
    if ($script:LaunchedIllustrator -and $null -ne $app) {
        try { $app.Quit() } catch { }
        try { Wait-Process -Name Illustrator -Timeout 15 -ErrorAction SilentlyContinue } catch { }
    }
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
}
