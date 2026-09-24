/*
 * ESDB ExtendScript facade v0.1
 *
 * Thin ES3-safe wrapper over the ESABI ExternalObject adapter. This is not the
 * future ESDB Store API: it intentionally exposes only the adapter operations
 * that exist today.
 */
var ESDB = (function () {
    var bridge = null;
    var loadedSpec = null;

    function fail(message) {
        throw new Error("ESDB: " + message);
    }

    function requireBridge() {
        if (!bridge) {
            fail("not loaded; call ESDB.load() first");
        }
        return bridge;
    }

    function pathText(pathValue) {
        if (pathValue && typeof pathValue.fsName == "string") {
            return pathValue.fsName;
        }
        return String(pathValue);
    }

    function lastErrorJSON() {
        var xo = requireBridge();
        var text = xo.lastError(0);
        return text === void 0 ? "" : String(text);
    }

    function load(spec) {
        var librarySpec;
        var candidate = null;
        var pingValue;
        var loadError;

        if (bridge) {
            return api;
        }
        if (typeof ExternalObject == "undefined") {
            fail("ExternalObject is unavailable in this host");
        }

        librarySpec = spec ? String(spec) : "lib:ESDB";
        try {
            candidate = new ExternalObject(librarySpec);
            pingValue = candidate ? candidate.ping(0) : void 0;
            if (!candidate || pingValue !== 42) {
                loadError = "native adapter failed its ping check";
            }
        } catch (e) {
            loadError = "native adapter load failed: " + String(e);
        }

        if (loadError) {
            if (candidate) {
                try {
                    candidate.unload();
                } catch (ignore) {}
            }
            bridge = null;
            loadedSpec = null;
            fail(loadError);
        }

        bridge = candidate;
        loadedSpec = librarySpec;
        return api;
    }

    function ensureLoaded() {
        if (!bridge) {
            load();
        }
        return bridge;
    }

    function unload() {
        var xo;
        var openHandles;

        if (!bridge) {
            return true;
        }
        xo = bridge;
        openHandles = xo.handleCount(0);
        if (openHandles !== 0) {
            fail("cannot unload while " + openHandles + " database handle(s) are open");
        }
        try {
            xo.unload();
        } catch (e) {
            fail("native adapter unload failed: " + String(e));
        }
        bridge = null;
        loadedSpec = null;
        return true;
    }

    function Database(handleValue) {
        this._handle = handleValue;
    }

    Database.prototype.isOpen = function () {
        return this._handle > 0;
    };

    Database.prototype.close = function () {
        var xo;
        var ok;

        if (!this.isOpen()) {
            return true;
        }
        xo = requireBridge();
        ok = xo.close(this._handle);
        if (!ok) {
            fail("close failed: " + lastErrorJSON());
        }
        this._handle = 0;
        return true;
    };

    Database.prototype.healthJSON = function () {
        var result;

        if (!this.isOpen()) {
            fail("database is closed");
        }
        result = requireBridge().health(this._handle);
        if (result === void 0) {
            fail("health failed: " + lastErrorJSON());
        }
        return String(result);
    };

    Database.prototype.dataVersion = function () {
        var value;

        if (!this.isOpen()) {
            fail("database is closed");
        }
        value = requireBridge().dataVersion(this._handle);
        if (value < 0) {
            fail("dataVersion failed: " + lastErrorJSON());
        }
        return value;
    };

    function open(pathValue) {
        var xo = ensureLoaded();
        var staged;
        var handleValue;

        staged = xo.stage(pathText(pathValue));
        if (!staged) {
            fail("path staging failed: " + lastErrorJSON());
        }

        handleValue = xo.openStaged(0);
        if (!(handleValue > 0)) {
            fail("open failed: " + lastErrorJSON());
        }
        return new Database(handleValue);
    }

    function version() {
        var result = ensureLoaded().version(0);
        if (result === void 0) {
            fail("version query failed: " + lastErrorJSON());
        }
        return String(result);
    }

    function sqliteVersion() {
        var result = ensureLoaded().sqliteVersion(0);
        if (result === void 0) {
            fail("SQLite version query failed: " + lastErrorJSON());
        }
        return String(result);
    }

    function abiVersion() {
        return ensureLoaded().abiVersion(0);
    }

    function handleCount() {
        return ensureLoaded().handleCount(0);
    }

    var api = {};
    api.load = load;
    api.unload = unload;
    api.open = open;
    api.version = version;
    api.sqliteVersion = sqliteVersion;
    api.abiVersion = abiVersion;
    api.lastErrorJSON = lastErrorJSON;
    api.handleCount = handleCount;
    api.Database = Database;
    api.loadedSpec = function () {
        return loadedSpec;
    };
    return api;
}());
