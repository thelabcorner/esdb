/*
 * ESDB ExtendScript facade v0.1
 *
 * ES3-safe wrapper over the ESABI ExternalObject adapter.
 *
 * Channel rule: strings that must be byte-exact (paths, Store names/keys,
 * UTF-8 values, structured payloads) cross ExternalObject as ASCII hex. Adobe's
 * measured ExternalObject string lane cannot faithfully carry embedded NUL or
 * all surrogate pairs, so raw host strings are never the durable wire format.
 *
 * ARRAY/OBJECT ergonomics use an external strict structured codec. ESON is
 * auto-detected when present; ESDB does not bundle it so this MIT project does
 * not absorb ESON's GPL license. Call ESDB.useStructuredCodec({parse,stringify})
 * to provide another explicit codec.
 */
var ESDB = (function () {
    var bridge = null;
    var loadedSpec = null;
    var structuredCodec = null;
    var hasOwn = Object.prototype.hasOwnProperty;
    var objectToString = Object.prototype.toString;

    var TYPE_NULL = 0;
    var TYPE_BOOL = 1;
    var TYPE_INT32 = 2;
    var TYPE_INT64 = 3;
    var TYPE_DOUBLE = 4;
    var TYPE_UTF8 = 5;
    var TYPE_BYTES = 6;
    var TYPE_ARRAY = 7;
    var TYPE_OBJECT = 8;

    var TX_DEFERRED = 0;
    var TX_IMMEDIATE = 1;
    var TX_EXCLUSIVE = 2;

    var MAX_REVISION = "9223372036854775807";
    var MAX_SAFE_INTEGER = 9007199254740991;

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

    function lastErrorOK() {
        return lastErrorJSON().indexOf("\"ok\":true") >= 0;
    }

    function failNative(prefix) {
        fail(prefix + ": " + lastErrorJSON());
    }

    function hexByte(value) {
        var digits = "0123456789ABCDEF";
        return digits.charAt((value >>> 4) & 15) + digits.charAt(value & 15);
    }

    function utf8ToHex(value) {
        var text = String(value);
        var out = [];
        var i;
        var code;
        var low;
        var point;

        for (i = 0; i < text.length; i += 1) {
            code = text.charCodeAt(i);
            if (code <= 0x7f) {
                out[out.length] = hexByte(code);
            } else if (code <= 0x7ff) {
                out[out.length] = hexByte(0xc0 | (code >>> 6));
                out[out.length] = hexByte(0x80 | (code & 0x3f));
            } else if (code >= 0xd800 && code <= 0xdbff) {
                if (i + 1 >= text.length) {
                    fail("unpaired high surrogate in string");
                }
                low = text.charCodeAt(i + 1);
                if (low < 0xdc00 || low > 0xdfff) {
                    fail("unpaired high surrogate in string");
                }
                point = 0x10000 + ((code - 0xd800) << 10) + (low - 0xdc00);
                out[out.length] = hexByte(0xf0 | (point >>> 18));
                out[out.length] = hexByte(0x80 | ((point >>> 12) & 0x3f));
                out[out.length] = hexByte(0x80 | ((point >>> 6) & 0x3f));
                out[out.length] = hexByte(0x80 | (point & 0x3f));
                i += 1;
            } else if (code >= 0xdc00 && code <= 0xdfff) {
                fail("unpaired low surrogate in string");
            } else {
                out[out.length] = hexByte(0xe0 | (code >>> 12));
                out[out.length] = hexByte(0x80 | ((code >>> 6) & 0x3f));
                out[out.length] = hexByte(0x80 | (code & 0x3f));
            }
        }
        return out.join("");
    }

    function nibble(code) {
        if (code >= 48 && code <= 57) return code - 48;
        if (code >= 65 && code <= 70) return code - 55;
        if (code >= 97 && code <= 102) return code - 87;
        return -1;
    }

    function normalizeHex(value) {
        var text = String(value);
        var i;
        if ((text.length & 1) != 0) {
            fail("hex payload must contain an even number of digits");
        }
        for (i = 0; i < text.length; i += 1) {
            if (nibble(text.charCodeAt(i)) < 0) {
                fail("hex payload contains a non-hex character");
            }
        }
        return text.toUpperCase();
    }

    function hexBytes(hex) {
        var text = normalizeHex(hex);
        var bytes = [];
        var i;
        for (i = 0; i < text.length; i += 2) {
            bytes[bytes.length] =
                (nibble(text.charCodeAt(i)) << 4) |
                nibble(text.charCodeAt(i + 1));
        }
        return bytes;
    }

    function utf8FromHex(hex) {
        var bytes = hexBytes(hex);
        var out = [];
        var i = 0;
        var b0;
        var b1;
        var b2;
        var b3;
        var point;

        while (i < bytes.length) {
            b0 = bytes[i++];
            if (b0 <= 0x7f) {
                out[out.length] = String.fromCharCode(b0);
                continue;
            }

            if (b0 >= 0xc2 && b0 <= 0xdf) {
                if (i >= bytes.length) fail("truncated UTF-8 sequence");
                b1 = bytes[i++];
                if ((b1 & 0xc0) != 0x80) fail("invalid UTF-8 continuation byte");
                point = ((b0 & 0x1f) << 6) | (b1 & 0x3f);
                out[out.length] = String.fromCharCode(point);
                continue;
            }

            if (b0 >= 0xe0 && b0 <= 0xef) {
                if (i + 1 >= bytes.length) fail("truncated UTF-8 sequence");
                b1 = bytes[i++];
                b2 = bytes[i++];
                if ((b1 & 0xc0) != 0x80 || (b2 & 0xc0) != 0x80) {
                    fail("invalid UTF-8 continuation byte");
                }
                if (b0 == 0xe0 && b1 < 0xa0) fail("overlong UTF-8 sequence");
                if (b0 == 0xed && b1 >= 0xa0) fail("UTF-8 encodes a surrogate");
                point = ((b0 & 0x0f) << 12) |
                    ((b1 & 0x3f) << 6) |
                    (b2 & 0x3f);
                out[out.length] = String.fromCharCode(point);
                continue;
            }

            if (b0 >= 0xf0 && b0 <= 0xf4) {
                if (i + 2 >= bytes.length) fail("truncated UTF-8 sequence");
                b1 = bytes[i++];
                b2 = bytes[i++];
                b3 = bytes[i++];
                if ((b1 & 0xc0) != 0x80 ||
                    (b2 & 0xc0) != 0x80 ||
                    (b3 & 0xc0) != 0x80) {
                    fail("invalid UTF-8 continuation byte");
                }
                if (b0 == 0xf0 && b1 < 0x90) fail("overlong UTF-8 sequence");
                if (b0 == 0xf4 && b1 >= 0x90) fail("UTF-8 code point exceeds U+10FFFF");
                point = ((b0 & 0x07) << 18) |
                    ((b1 & 0x3f) << 12) |
                    ((b2 & 0x3f) << 6) |
                    (b3 & 0x3f);
                point -= 0x10000;
                out[out.length] = String.fromCharCode(
                    0xd800 | ((point >>> 10) & 0x3ff),
                    0xdc00 | (point & 0x3ff)
                );
                continue;
            }

            fail("invalid UTF-8 lead byte");
        }

        return out.join("");
    }

    function canonicalUnsigned(value, maxText, label) {
        var text;
        if (typeof value == "number") {
            if (!isFinite(value) || value < 0 || Math.floor(value) != value ||
                value > MAX_SAFE_INTEGER) {
                fail(label + " number must be a safe non-negative integer; pass a decimal string for larger values");
            }
            text = String(value);
        } else {
            text = String(value);
        }

        if (!/^(0|[1-9][0-9]*)$/.test(text)) {
            fail(label + " must be a canonical unsigned decimal integer");
        }
        if (text.length > maxText.length ||
            (text.length == maxText.length && text > maxText)) {
            fail(label + " exceeds ESDB's signed-64 revision domain");
        }
        return text;
    }

    function canonicalInt64(value) {
        var text = String(value);
        var negative = false;
        var magnitude;
        var max;

        if (!/^-?(0|[1-9][0-9]*)$/.test(text)) {
            fail("INT64 must be a canonical signed decimal integer");
        }
        if (text == "-0") return "0";
        negative = text.charAt(0) == "-";
        magnitude = negative ? text.substring(1) : text;
        max = negative ? "9223372036854775808" : "9223372036854775807";
        if (magnitude.length > max.length ||
            (magnitude.length == max.length && magnitude > max)) {
            fail("INT64 is outside the signed 64-bit range");
        }
        return negative ? "-" + magnitude : magnitude;
    }

    function Int64Value(value) {
        this.text = canonicalInt64(value);
    }

    Int64Value.prototype.toString = function () {
        return this.text;
    };

    function BytesValue(hex) {
        this.hex = normalizeHex(hex);
    }

    BytesValue.prototype.toHex = function () {
        return this.hex;
    };

    BytesValue.prototype.toArray = function () {
        return hexBytes(this.hex);
    };

    function bytesFromArray(values) {
        var out = [];
        var i;
        var value;
        if (!values || typeof values.length != "number") {
            fail("bytesFromArray expects an array-like value");
        }
        for (i = 0; i < values.length; i += 1) {
            value = Number(values[i]);
            if (!isFinite(value) || Math.floor(value) != value ||
                value < 0 || value > 255) {
                fail("byte at index " + i + " is outside 0..255");
            }
            out[out.length] = hexByte(value);
        }
        return new BytesValue(out.join(""));
    }

    function resolveStructuredCodec() {
        if (structuredCodec) return structuredCodec;
        if (typeof ESON == "object" && ESON &&
            typeof ESON.parse == "function" &&
            typeof ESON.stringify == "function") {
            return ESON;
        }
        fail("ARRAY/OBJECT values require ESON or an explicit ESDB.useStructuredCodec({parse,stringify}) codec");
    }

    function useStructuredCodec(codec) {
        if (codec === null) {
            structuredCodec = null;
            return api;
        }
        if (!codec || typeof codec.parse != "function" ||
            typeof codec.stringify != "function") {
            fail("structured codec must expose parse(text) and stringify(value)");
        }
        structuredCodec = codec;
        return api;
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

    function parseValueWire(text) {
        var wire = String(text);
        var second;
        var type;
        if (wire.substring(0, 3) != "V1:") fail("invalid Store value wire version");
        second = wire.indexOf(":", 3);
        if (second < 0) fail("malformed Store value wire");
        type = Number(wire.substring(3, second));
        if (Math.floor(type) != type || type < -1 || type > TYPE_OBJECT) {
            fail("invalid Store value type in wire payload");
        }
        return {
            found: type != -1,
            type: type,
            payload: wire.substring(second + 1)
        };
    }

    function decodeStoreValue(raw) {
        var payload;
        var numberValue;
        var codec;
        if (!raw.found) return void 0;
        payload = raw.payload;

        if (raw.type == TYPE_NULL) return null;
        if (raw.type == TYPE_BOOL) {
            if (payload == "0") return false;
            if (payload == "1") return true;
            fail("invalid BOOL payload from native adapter");
        }
        if (raw.type == TYPE_INT32) {
            if (!/^-?(0|[1-9][0-9]*)$/.test(payload)) {
                fail("invalid INT32 payload from native adapter");
            }
            numberValue = Number(payload);
            if (numberValue < -2147483648 || numberValue > 2147483647) {
                fail("INT32 payload is out of range");
            }
            return numberValue;
        }
        if (raw.type == TYPE_INT64) return new Int64Value(payload);
        if (raw.type == TYPE_DOUBLE) {
            if (payload == "NaN") return Number("NaN");
            if (payload == "Infinity") return Number("Infinity");
            if (payload == "-Infinity") return Number("-Infinity");
            numberValue = Number(payload);
            if (isNaN(numberValue)) fail("invalid DOUBLE payload from native adapter");
            return numberValue;
        }
        if (raw.type == TYPE_UTF8) return utf8FromHex(payload);
        if (raw.type == TYPE_BYTES) return new BytesValue(payload);
        if (raw.type == TYPE_ARRAY || raw.type == TYPE_OBJECT) {
            codec = resolveStructuredCodec();
            var parsed = codec.parse(utf8FromHex(payload));
            if (raw.type == TYPE_ARRAY) {
                if (objectToString.call(parsed) != "[object Array]") {
                    fail("ARRAY payload decoded to a non-array value");
                }
            } else if (!parsed || typeof parsed != "object" ||
                       objectToString.call(parsed) == "[object Array]") {
                fail("OBJECT payload decoded to a non-object value");
            }
            return parsed;
        }
        fail("unsupported Store value type " + raw.type);
    }

    function encodeStoreValue(value) {
        var codec;
        var text;
        var tag = objectToString.call(value);

        if (value === null) {
            return { lane: "text", type: TYPE_NULL, payload: "" };
        }
        if (value instanceof Int64Value) {
            return { lane: "text", type: TYPE_INT64, payload: value.text };
        }
        if (value instanceof BytesValue) {
            return { lane: "text", type: TYPE_BYTES, payload: value.hex };
        }

        if (typeof value == "boolean") {
            return { lane: "number", type: TYPE_BOOL, payload: value ? 1 : 0 };
        }
        if (typeof value == "number") {
            if (isFinite(value) &&
                Math.floor(value) == value &&
                value >= -2147483648 &&
                value <= 2147483647 &&
                !(value == 0 && 1 / value < 0)) {
                return { lane: "number", type: TYPE_INT32, payload: value };
            }
            return { lane: "number", type: TYPE_DOUBLE, payload: value };
        }
        if (typeof value == "string") {
            return { lane: "text", type: TYPE_UTF8, payload: utf8ToHex(value) };
        }
        if (typeof value == "undefined") {
            fail("undefined is not in the ESDB value domain; use null explicitly");
        }

        codec = resolveStructuredCodec();
        text = codec.stringify(value);
        if (typeof text != "string") {
            fail("structured codec stringify() did not return a string");
        }
        if (tag == "[object Array]") {
            return { lane: "text", type: TYPE_ARRAY, payload: utf8ToHex(text) };
        }
        if (typeof value == "object") {
            return { lane: "text", type: TYPE_OBJECT, payload: utf8ToHex(text) };
        }
        fail("unsupported Store value type: " + typeof value);
    }


    function encodeQueryParameter(value) {
        var text;
        if (value === null) return "V1:" + TYPE_NULL + ":";
        if (value instanceof Int64Value) {
            return "V1:" + TYPE_INT64 + ":" + value.text;
        }
        if (value instanceof BytesValue) {
            return "V1:" + TYPE_BYTES + ":" + value.hex;
        }
        if (typeof value == "boolean") {
            return "V1:" + TYPE_BOOL + ":" + (value ? "1" : "0");
        }
        if (typeof value == "number") {
            if (isFinite(value) && Math.floor(value) == value &&
                Math.abs(value) <= MAX_SAFE_INTEGER &&
                !(value == 0 && 1 / value < 0)) {
                if (value >= -2147483648 && value <= 2147483647) {
                    return "V1:" + TYPE_INT32 + ":" + String(value);
                }
                return "V1:" + TYPE_INT64 + ":" + canonicalInt64(value);
            }
            return "V1:" + TYPE_DOUBLE + ":" + patchNumberPayload(value);
        }
        if (typeof value == "string") {
            return "V1:" + TYPE_UTF8 + ":" + utf8ToHex(value);
        }
        if (typeof value == "undefined") {
            fail("SQL parameters may not be undefined; use null explicitly");
        }
        fail("SQL parameters accept only null, boolean, number, string, ESDB.Int64, or ESDB.Bytes");
    }

    function queryParameterPacket(values) {
        var params = values === void 0 || values === null ? [] : values;
        var lines = [];
        var i;
        if (objectToString.call(params) != "[object Array]") {
            fail("SQL parameters must be an Array");
        }
        if (params.length > 1024) {
            fail("SQL parameter count exceeds 1024");
        }
        lines[0] = "P1:" + params.length;
        for (i = 0; i < params.length; i += 1) {
            lines[lines.length] = encodeQueryParameter(params[i]);
        }
        return lines.join("\n");
    }

    function parseQueryValueField(field) {
        var split = field.indexOf(":");
        var type;
        var raw;
        if (split <= 0) fail("malformed SQL row value");
        type = Number(field.substring(0, split));
        if (!isFinite(type) || Math.floor(type) != type ||
            type < TYPE_NULL || type > TYPE_OBJECT) {
            fail("invalid SQL row value type");
        }
        raw = {
            found: true,
            type: type,
            payload: field.substring(split + 1)
        };
        return decodeStoreValue(raw);
    }

    function parseQueryResultWire(text) {
        var wire = String(text);
        var lines = wire.split("\n");
        var header;
        var columnCount;
        var returnedCount;
        var totalRows;
        var changes;
        var truncated;
        var namesLine;
        var nameFields;
        var columns = [];
        var rows = [];
        var i;
        var j;
        var fields;
        var row;

        if (!lines.length || lines[0].substring(0, 3) != "Q1:") {
            fail("invalid SQL query wire version");
        }
        header = lines[0].substring(3).split(":");
        if (header.length != 5) fail("malformed SQL query header");

        columnCount = Number(header[0]);
        returnedCount = Number(header[1]);
        if (!isFinite(columnCount) || Math.floor(columnCount) != columnCount ||
            columnCount < 0 || columnCount > 32767) {
            fail("invalid SQL query column count");
        }
        if (!isFinite(returnedCount) || Math.floor(returnedCount) != returnedCount ||
            returnedCount < 0 || returnedCount > 10000) {
            fail("invalid SQL query returned-row count");
        }
        totalRows = canonicalUnsigned(header[2], MAX_REVISION, "SQL total row count");
        changes = canonicalUnsigned(header[3], MAX_REVISION, "SQL change count");
        if (header[4] != "0" && header[4] != "1") {
            fail("invalid SQL query truncation flag");
        }
        truncated = header[4] == "1";

        if (lines.length < 2 || lines[1].substring(0, 2) != "N:") {
            fail("SQL query result is missing column metadata");
        }
        namesLine = lines[1].substring(2);
        if (columnCount > 0) {
            nameFields = namesLine.split("|");
            if (nameFields.length != columnCount) {
                fail("SQL query column count does not match metadata");
            }
            for (i = 0; i < nameFields.length; i += 1) {
                columns[columns.length] = utf8FromHex(nameFields[i]);
            }
        } else if (namesLine.length) {
            fail("zero-column SQL result contains column metadata");
        }

        for (i = 2; i < lines.length; i += 1) {
            if (!lines[i]) continue;
            if (lines[i].substring(0, 2) != "R:") {
                fail("malformed SQL query row");
            }
            fields = lines[i].substring(2).split("|");
            if (fields.length != columnCount) {
                fail("SQL row column count does not match metadata");
            }
            row = [];
            for (j = 0; j < fields.length; j += 1) {
                row[row.length] = parseQueryValueField(fields[j]);
            }
            rows[rows.length] = row;
        }
        if (rows.length != returnedCount) {
            fail("SQL returned-row count does not match payload");
        }

        return {
            columns: columns,
            rows: rows,
            returnedRowCount: returnedCount,
            totalRowCount: new Int64Value(totalRows),
            changes: new Int64Value(changes),
            truncated: truncated
        };
    }

    function patchNumberPayload(value) {
        if (isNaN(value)) return "NaN";
        if (value == Number("Infinity")) return "Infinity";
        if (value == Number("-Infinity")) return "-Infinity";
        if (value == 0 && 1 / value < 0) return "-0";
        return String(value);
    }

    function parseDeleteWire(text) {
        var wire = String(text);
        var parts;
        if (wire.substring(0, 3) != "V1:") fail("invalid Store delete wire version");
        parts = wire.substring(3).split(":");
        if (parts.length != 2 || (parts[0] != "0" && parts[0] != "1")) {
            fail("malformed Store delete result");
        }
        return {
            deleted: parts[0] == "1",
            revision: canonicalUnsigned(parts[1], MAX_REVISION, "revision")
        };
    }

    function parseChangeWire(text) {
        var wire = String(text);
        var newline = wire.indexOf("\n");
        var headerText = newline < 0 ? wire : wire.substring(0, newline);
        var body = newline < 0 ? "" : wire.substring(newline + 1);
        var header;
        var lines;
        var changes = [];
        var i;
        var fields;
        var count;

        if (headerText.substring(0, 3) != "V1:") {
            fail("invalid Store change wire version");
        }
        header = headerText.substring(3).split(":");
        if (header.length != 2) fail("malformed Store change header");
        header[0] = canonicalUnsigned(header[0], MAX_REVISION, "last revision");
        count = Number(header[1]);
        if (!isFinite(count) || Math.floor(count) != count || count < 0 || count > 10000) {
            fail("invalid Store change count");
        }

        if (body.length) {
            lines = body.split("\n");
            for (i = 0; i < lines.length; i += 1) {
                if (!lines[i]) continue;
                fields = lines[i].split(":");
                if (fields.length != 5) fail("malformed Store change record");
                changes[changes.length] = {
                    revision: canonicalUnsigned(fields[0], MAX_REVISION, "change revision"),
                    operation: Number(fields[1]),
                    valueType: Number(fields[2]),
                    store: utf8FromHex(fields[3]),
                    key: utf8FromHex(fields[4])
                };
            }
        }
        if (changes.length != count) {
            fail("Store change count does not match payload");
        }
        return {
            lastRevision: header[0],
            count: count,
            changes: changes
        };
    }

    function parseScanWire(text) {
        var wire = String(text);
        var newline = wire.indexOf("\n");
        var headerText = newline < 0 ? wire : wire.substring(0, newline);
        var body = newline < 0 ? "" : wire.substring(newline + 1);
        var count;
        var lines;
        var records = [];
        var i;
        var fields;
        var raw;

        if (headerText.substring(0, 3) != "V1:") {
            fail("invalid scan wire version");
        }
        count = Number(headerText.substring(3));
        if (!isFinite(count) || Math.floor(count) != count ||
            count < 0 || count > 10000) {
            fail("invalid scan record count");
        }

        if (body.length) {
            lines = body.split("\n");
            for (i = 0; i < lines.length; i += 1) {
                if (!lines[i]) continue;
                fields = lines[i].split(":");
                if (fields.length != 4) fail("malformed scan record");
                raw = {
                    found: true,
                    type: Number(fields[2]),
                    payload: fields[3]
                };
                records[records.length] = {
                    revision: canonicalUnsigned(
                        fields[0], MAX_REVISION, "record revision"),
                    key: utf8FromHex(fields[1]),
                    value: decodeStoreValue(raw)
                };
            }
        }
        if (records.length != count) {
            fail("scan record count does not match payload");
        }
        return {
            count: count,
            records: records,
            lastKey: count ? records[count - 1].key : null
        };
    }

    function parseMemoryChangeWire(text) {
        var wire = String(text);
        var newline = wire.indexOf("\n");
        var headerText = newline < 0 ? wire : wire.substring(0, newline);
        var body = newline < 0 ? "" : wire.substring(newline + 1);
        var header;
        var lines;
        var changes = [];
        var i;
        var fields;
        var count;
        var operation;

        if (headerText.substring(0, 3) != "V1:") {
            fail("invalid memory Store change wire version");
        }
        header = headerText.substring(3).split(":");
        if (header.length != 2) fail("malformed memory Store change header");
        header[0] = canonicalUnsigned(
            header[0], MAX_REVISION, "last revision");
        count = Number(header[1]);
        if (!isFinite(count) || Math.floor(count) != count ||
            count < 0 || count > 10000) {
            fail("invalid memory Store change count");
        }

        if (body.length) {
            lines = body.split("\n");
            for (i = 0; i < lines.length; i += 1) {
                if (!lines[i]) continue;
                fields = lines[i].split(":");
                if (fields.length != 4) {
                    fail("malformed memory Store change record");
                }
                operation = Number(fields[1]);
                changes[changes.length] = {
                    revision: canonicalUnsigned(
                        fields[0], MAX_REVISION, "change revision"),
                    operation: operation,
                    valueType: Number(fields[2]),
                    key: operation == 3 ? null : utf8FromHex(fields[3])
                };
            }
        }
        if (changes.length != count) {
            fail("memory Store change count does not match payload");
        }
        return {
            lastRevision: header[0],
            count: count,
            changes: changes
        };
    }

    function Database(handleValue) {
        this._handle = handleValue;
    }

    Database.prototype.isOpen = function () {
        return this._handle > 0;
    };

    Database.prototype._requireOpen = function () {
        if (!this.isOpen()) fail("database is closed");
        return requireBridge();
    };

    Database.prototype.close = function () {
        var xo;
        var ok;

        if (!this.isOpen()) return true;
        xo = requireBridge();
        ok = xo.close(this._handle);
        if (!ok) failNative("close failed");
        this._handle = 0;
        return true;
    };

    Database.prototype.healthJSON = function () {
        var result = this._requireOpen().health(this._handle);
        if (result === void 0) failNative("health failed");
        return String(result);
    };

    Database.prototype.dataVersion = function () {
        var value = this._requireOpen().dataVersion(this._handle);
        if (value < 0) failNative("dataVersion failed");
        return value;
    };

    /*
     * Advanced typed SQL escape hatch. SQL text and values cross the bridge in
     * separate arguments; values are bound natively and are never interpolated.
     * Production schema DDL remains migration-owned.
     */
    Database.prototype.query = function (sqlValue, parameters, options) {
        var sql = String(sqlValue);
        var opts = options || {};
        var maxRows = opts.maxRows === void 0 ? 1000 : Number(opts.maxRows);
        var packet;
        var result;

        if (!sql.length) fail("SQL query must not be empty");
        if (sql.indexOf("\u0000") >= 0) fail("SQL query may not contain NUL");
        if (!isFinite(maxRows) || Math.floor(maxRows) != maxRows ||
            maxRows < 0 || maxRows > 10000) {
            fail("SQL maxRows must be an integer in 0..10000");
        }

        packet = queryParameterPacket(parameters);
        result = this._requireOpen().querySql(
            this._handle,
            utf8ToHex(sql),
            packet,
            maxRows
        );
        if (result === void 0) failNative("typed SQL query failed");
        return parseQueryResultWire(result);
    };

    Database.prototype.run = function (sqlValue, parameters) {
        return this.query(sqlValue, parameters, { maxRows: 0 });
    };

    Database.prototype.transactionActive = function () {
        var xo = this._requireOpen();
        var active = xo.transactionActive(this._handle);
        if (!active && !lastErrorOK()) failNative("transactionActive failed");
        return !!active;
    };

    Database.prototype.transaction = function (callback, mode) {
        var xo = this._requireOpen();
        var txMode = mode === void 0 ? TX_IMMEDIATE : Number(mode);
        var began;
        var result;
        var committed = false;
        var commitError;

        if (typeof callback != "function") fail("transaction expects a callback");
        if (txMode != TX_DEFERRED && txMode != TX_IMMEDIATE && txMode != TX_EXCLUSIVE) {
            fail("invalid transaction mode");
        }

        began = xo.transactionBegin(this._handle, txMode);
        if (!began) failNative("transaction begin failed");

        try {
            result = callback(this);
            if (!xo.transactionCommit(this._handle)) {
                commitError = lastErrorJSON();
                try {
                    xo.transactionRollback(this._handle);
                } catch (ignoreCommitRollback) {}
                fail("transaction commit failed: " + commitError);
            }
            committed = true;
            return result;
        } catch (e) {
            if (!committed && this.isOpen()) {
                try {
                    if (xo.transactionActive(this._handle)) {
                        xo.transactionRollback(this._handle);
                    }
                } catch (ignoreRollback) {}
            }
            throw e;
        }
    };

    Database.prototype.objectStore = function (name) {
        return new ObjectStore(this, String(name));
    };

    Database.prototype.objectStoreRevision = function () {
        var result = this._requireOpen().objectStoreRevision(this._handle);
        if (result === void 0) failNative("Store revision failed");
        return canonicalUnsigned(String(result), MAX_REVISION, "revision");
    };

    Database.prototype.changesSince = function (afterRevision, limit) {
        return objectChangesSince(this, null, afterRevision, limit);
    };

    Database.prototype.pruneObjectChanges = function (throughRevision) {
        var exact = canonicalUnsigned(throughRevision, MAX_REVISION, "through revision");
        var result = this._requireOpen().objectStorePrune(this._handle, exact);
        if (result === void 0) failNative("ObjectStore prune failed");
        return canonicalUnsigned(String(result), MAX_REVISION, "deleted count");
    };

    function ObjectStore(database, name) {
        if (!database || !database.isOpen()) fail("Store requires an open database");
        if (!name.length) fail("Store name must not be empty");
        this.database = database;
        this.name = name;
        this._nameHex = utf8ToHex(name);
    }

    ObjectStore.prototype.ensure = function () {
        var xo = this.database._requireOpen();
        if (!xo.objectStoreEnsure(this.database._handle, this._nameHex)) {
            failNative("Store ensure failed");
        }
        return this;
    };

    ObjectStore.prototype.set = function (keyValue, value) {
        var key = String(keyValue);
        var keyHex;
        var encoded;
        var xo;
        var result;

        if (!key.length) fail("Store key must not be empty");
        keyHex = utf8ToHex(key);
        encoded = encodeStoreValue(value);
        xo = this.database._requireOpen();

        if (encoded.lane == "number") {
            result = xo.objectStorePutNumber(
                this.database._handle,
                this._nameHex,
                keyHex,
                encoded.type,
                encoded.payload
            );
        } else {
            result = xo.objectStorePutText(
                this.database._handle,
                this._nameHex,
                keyHex,
                encoded.type,
                encoded.payload
            );
        }
        if (result === void 0) failNative("Store set failed");
        return canonicalUnsigned(String(result), MAX_REVISION, "revision");
    };

    ObjectStore.prototype.put = ObjectStore.prototype.set;

    ObjectStore.prototype.getRaw = function (keyValue) {
        var key = String(keyValue);
        var result;
        if (!key.length) fail("Store key must not be empty");
        result = this.database._requireOpen().objectStoreGet(
            this.database._handle,
            this._nameHex,
            utf8ToHex(key)
        );
        if (result === void 0) failNative("Store get failed");
        return parseValueWire(result);
    };

    ObjectStore.prototype.get = function (keyValue, defaultValue) {
        var raw = this.getRaw(keyValue);
        if (!raw.found) {
            return arguments.length >= 2 ? defaultValue : void 0;
        }
        return decodeStoreValue(raw);
    };

    ObjectStore.prototype.has = function (keyValue) {
        var key = String(keyValue);
        var xo;
        var result;
        if (!key.length) fail("Store key must not be empty");
        xo = this.database._requireOpen();
        result = xo.objectStoreExists(
            this.database._handle,
            this._nameHex,
            utf8ToHex(key)
        );
        if (!result && !lastErrorOK()) failNative("Store exists failed");
        return !!result;
    };

    ObjectStore.prototype.remove = function (keyValue) {
        var key = String(keyValue);
        var result;
        if (!key.length) fail("Store key must not be empty");
        result = this.database._requireOpen().objectStoreDelete(
            this.database._handle,
            this._nameHex,
            utf8ToHex(key)
        );
        if (result === void 0) failNative("Store delete failed");
        return parseDeleteWire(result);
    };
    ObjectStore.prototype["delete"] = ObjectStore.prototype.remove;

    ObjectStore.prototype.count = function () {
        var result = this.database._requireOpen().objectStoreCount(
            this.database._handle,
            this._nameHex
        );
        if (result === void 0) failNative("ObjectStore count failed");
        return canonicalUnsigned(String(result), MAX_REVISION, "count");
    };

    ObjectStore.prototype.scan = function (afterKey, limit) {
        var afterHex = afterKey === null || afterKey === void 0
            ? ""
            : utf8ToHex(String(afterKey));
        var bounded = limit === void 0 ? 0 : Number(limit);
        var result;

        if (!isFinite(bounded) || Math.floor(bounded) != bounded ||
            bounded < 0 || bounded > 10000) {
            fail("scan limit must be an integer in 0..10000 (0 means 10000)");
        }

        result = this.database._requireOpen().objectStoreScan(
            this.database._handle,
            this._nameHex,
            afterHex,
            bounded
        );
        if (result === void 0) failNative("ObjectStore scan failed");
        return parseScanWire(result);
    };

    ObjectStore.prototype.getAll = function (limit) {
        var window = this.scan(null, limit);
        var values = [];
        var i;
        for (i = 0; i < window.records.length; i += 1) {
            values[values.length] = window.records[i].value;
        }
        return values;
    };

    ObjectStore.prototype.entries = function (limit) {
        return this.scan(null, limit).records;
    };

    ObjectStore.prototype.revision = function () {
        return this.database.objectStoreRevision();
    };

    ObjectStore.prototype.patch = function (values) {
        var self = this;
        if (!values || typeof values != "object" ||
            objectToString.call(values) == "[object Array]") {
            fail("patch expects an object of key/value pairs");
        }
        return this.database.transaction(function () {
            var key;
            for (key in values) {
                if (hasOwn.call(values, key)) {
                    self.set(key, values[key]);
                }
            }
            return self.revision();
        }, TX_IMMEDIATE);
    };

    function objectChangesSince(database, store, afterRevision, limit) {
        var exact = canonicalUnsigned(
            afterRevision === void 0 ? "0" : afterRevision,
            MAX_REVISION,
            "after revision"
        );
        var bounded = limit === void 0 ? 0 : Number(limit);
        var filterHex = store ? store._nameHex : "";
        var result;

        if (!isFinite(bounded) || Math.floor(bounded) != bounded ||
            bounded < 0 || bounded > 10000) {
            fail("change limit must be an integer in 0..10000 (0 means 10000)");
        }
        result = database._requireOpen().objectStoreChanges(
            database._handle,
            filterHex,
            exact,
            bounded
        );
        if (result === void 0) failNative("Store changes query failed");
        return parseChangeWire(result);
    }

    ObjectStore.prototype.changesSince = function (afterRevision, limit) {
        return objectChangesSince(this.database, this, afterRevision, limit);
    };

    function ObjectSubscription(store, keyFilter, handler, afterRevision) {
        this.store = store;
        this.key = keyFilter;
        this.handler = handler;
        this.closed = false;
        this._revision = afterRevision === void 0
            ? store.revision()
            : canonicalUnsigned(afterRevision, MAX_REVISION, "subscription revision");
    }

    ObjectSubscription.prototype.revision = function () {
        return this._revision;
    };

    ObjectSubscription.prototype.poll = function (limit) {
        var window;
        var i;
        var change;
        var delivered = [];

        if (this.closed) fail("subscription is closed");
        window = this.store.changesSince(this._revision, limit);

        for (i = 0; i < window.changes.length; i += 1) {
            change = window.changes[i];
            if (this.key === null || change.key == this.key) {
                if (this.handler) {
                    this.handler(change);
                }
                delivered[delivered.length] = change;
            }
            this._revision = change.revision;
        }
        if (!window.changes.length) {
            this._revision = window.lastRevision;
        }
        return {
            revision: this._revision,
            count: delivered.length,
            changes: delivered
        };
    };

    ObjectSubscription.prototype.close = function () {
        this.closed = true;
        this.handler = null;
        return true;
    };

    ObjectStore.prototype.subscribe = function (keyOrHandler, handlerOrRevision, maybeRevision) {
        var keyFilter = null;
        var handler = null;
        var afterRevision;

        if (typeof keyOrHandler == "function") {
            handler = keyOrHandler;
            afterRevision = handlerOrRevision;
        } else if (keyOrHandler === null || keyOrHandler === void 0) {
            if (typeof handlerOrRevision == "function") {
                handler = handlerOrRevision;
                afterRevision = maybeRevision;
            } else {
                afterRevision = handlerOrRevision;
            }
        } else {
            keyFilter = String(keyOrHandler);
            if (!keyFilter.length) fail("subscription key must not be empty");
            if (typeof handlerOrRevision == "function") {
                handler = handlerOrRevision;
                afterRevision = maybeRevision;
            } else {
                afterRevision = handlerOrRevision;
            }
        }
        return new ObjectSubscription(this, keyFilter, handler, afterRevision);
    };

    function Store(name) {
        this.name = String(name);
        if (!this.name.length) fail("Store name must not be empty");
        this._nameHex = utf8ToHex(this.name);
        this._destroyed = false;
    }

    Store.prototype._requireLive = function () {
        if (this._destroyed) fail("Store has been destroyed");
        return requireBridge();
    };

    Store.prototype.set = function (keyValue, value) {
        var key = String(keyValue);
        var keyHex;
        var encoded;
        var xo;
        var result;

        if (!key.length) fail("Store key must not be empty");
        keyHex = utf8ToHex(key);
        encoded = encodeStoreValue(value);
        xo = this._requireLive();

        if (encoded.lane == "number") {
            result = xo.storePutNumber(
                this._nameHex,
                keyHex,
                encoded.type,
                encoded.payload
            );
        } else {
            result = xo.storePutText(
                this._nameHex,
                keyHex,
                encoded.type,
                encoded.payload
            );
        }
        if (result === void 0) failNative("memory Store set failed");
        return canonicalUnsigned(String(result), MAX_REVISION, "revision");
    };

    Store.prototype.put = Store.prototype.set;

    Store.prototype.patch = function (values) {
        var key;
        var keys = [];
        var encodedValues = [];
        var encoded;
        var lane;
        var payload;
        var lines = [];
        var result;
        var i;

        if (!values || typeof values != "object" ||
            objectToString.call(values) == "[object Array]") {
            fail("patch expects an object of key/value pairs");
        }

        for (key in values) {
            if (hasOwn.call(values, key)) {
                if (!String(key).length) fail("Store key must not be empty");
                if (keys.length >= 1024) {
                    fail("patch exceeds the 1024-entry Store patch limit");
                }
                keys[keys.length] = String(key);
                encodedValues[encodedValues.length] =
                    encodeStoreValue(values[key]);
            }
        }

        for (i = 0; i < keys.length; i += 1) {
            encoded = encodedValues[i];
            lane = encoded.lane == "number" ? "N" : "T";
            payload = lane == "N"
                ? patchNumberPayload(encoded.payload)
                : String(encoded.payload);
            lines[lines.length] =
                utf8ToHex(keys[i]) + ":" +
                String(encoded.type) + ":" +
                lane + ":" +
                payload;
        }

        result = this._requireLive().storePatch(
            this._nameHex,
            "P1:" + String(lines.length) + "\n" + lines.join("\n")
        );
        if (result === void 0) failNative("memory Store patch failed");
        return canonicalUnsigned(String(result), MAX_REVISION, "revision");
    };

    Store.prototype.getRaw = function (keyValue) {
        var key = String(keyValue);
        var result;
        if (!key.length) fail("Store key must not be empty");
        result = this._requireLive().storeGet(
            this._nameHex,
            utf8ToHex(key)
        );
        if (result === void 0) failNative("memory Store get failed");
        return parseValueWire(result);
    };

    Store.prototype.get = function (keyValue, defaultValue) {
        var raw = this.getRaw(keyValue);
        if (!raw.found) {
            return arguments.length >= 2 ? defaultValue : void 0;
        }
        return decodeStoreValue(raw);
    };

    Store.prototype.has = function (keyValue) {
        var key = String(keyValue);
        var xo;
        var result;
        if (!key.length) fail("Store key must not be empty");
        xo = this._requireLive();
        result = xo.storeExists(this._nameHex, utf8ToHex(key));
        if (!result && !lastErrorOK()) failNative("memory Store exists failed");
        return !!result;
    };

    Store.prototype.remove = function (keyValue) {
        var key = String(keyValue);
        var result;
        if (!key.length) fail("Store key must not be empty");
        result = this._requireLive().storeDelete(
            this._nameHex,
            utf8ToHex(key)
        );
        if (result === void 0) failNative("memory Store delete failed");
        return parseDeleteWire(result);
    };
    Store.prototype["delete"] = Store.prototype.remove;

    Store.prototype.count = function () {
        var result = this._requireLive().storeCount(this._nameHex);
        if (result === void 0) failNative("memory Store count failed");
        return canonicalUnsigned(String(result), MAX_REVISION, "count");
    };

    Store.prototype.clear = function () {
        var result = this._requireLive().storeClear(this._nameHex);
        if (result === void 0) failNative("memory Store clear failed");
        return parseDeleteWire(result);
    };

    Store.prototype.revision = function () {
        var result = this._requireLive().storeRevision(this._nameHex);
        if (result === void 0) failNative("memory Store revision failed");
        return canonicalUnsigned(String(result), MAX_REVISION, "revision");
    };

    Store.prototype.retainedFloor = function () {
        var result = this._requireLive().storeRetainedFloor(this._nameHex);
        if (result === void 0) failNative("memory Store retained-floor query failed");
        return canonicalUnsigned(
            String(result), MAX_REVISION, "retained floor");
    };

    Store.prototype.scan = function (afterKey, limit) {
        var afterHex = afterKey === null || afterKey === void 0
            ? ""
            : utf8ToHex(String(afterKey));
        var bounded = limit === void 0 ? 0 : Number(limit);
        var result;

        if (!isFinite(bounded) || Math.floor(bounded) != bounded ||
            bounded < 0 || bounded > 10000) {
            fail("scan limit must be an integer in 0..10000 (0 means 10000)");
        }

        result = this._requireLive().storeScan(
            this._nameHex,
            afterHex,
            bounded
        );
        if (result === void 0) failNative("memory Store scan failed");
        return parseScanWire(result);
    };

    Store.prototype.entries = function (limit) {
        return this.scan(null, limit).records;
    };

    Store.prototype.getAll = function (limit) {
        var window = this.scan(null, limit);
        var values = [];
        var i;
        for (i = 0; i < window.records.length; i += 1) {
            values[values.length] = window.records[i].value;
        }
        return values;
    };

    Store.prototype.changesSince = function (afterRevision, limit) {
        var exact = canonicalUnsigned(
            afterRevision === void 0 ? "0" : afterRevision,
            MAX_REVISION,
            "after revision"
        );
        var bounded = limit === void 0 ? 0 : Number(limit);
        var result;

        if (!isFinite(bounded) || Math.floor(bounded) != bounded ||
            bounded < 0 || bounded > 10000) {
            fail("change limit must be an integer in 0..10000 (0 means 10000)");
        }

        result = this._requireLive().storeChanges(
            this._nameHex,
            exact,
            bounded
        );
        if (result === void 0) failNative("memory Store changes query failed");
        return parseMemoryChangeWire(result);
    };

    Store.prototype.destroy = function () {
        if (this._destroyed) return true;
        if (!this._requireLive().storeDestroy(this._nameHex)) {
            failNative("memory Store destroy failed");
        }
        this._destroyed = true;
        return true;
    };

    function StoreSubscription(store, keyFilter, handler, afterRevision) {
        this.store = store;
        this.key = keyFilter;
        this.handler = handler;
        this.closed = false;
        this._revision = afterRevision === void 0
            ? store.revision()
            : canonicalUnsigned(
                afterRevision, MAX_REVISION, "subscription revision");
    }

    StoreSubscription.prototype.revision = function () {
        return this._revision;
    };

    StoreSubscription.prototype.poll = function (limit) {
        var window;
        var i;
        var change;
        var delivered = [];

        if (this.closed) fail("subscription is closed");
        window = this.store.changesSince(this._revision, limit);

        for (i = 0; i < window.changes.length; i += 1) {
            change = window.changes[i];
            if (change.operation == 3 ||
                this.key === null ||
                change.key == this.key) {
                if (this.handler) this.handler(change);
                delivered[delivered.length] = change;
            }
            this._revision = change.revision;
        }
        if (!window.changes.length) {
            this._revision = window.lastRevision;
        }
        return {
            revision: this._revision,
            count: delivered.length,
            changes: delivered
        };
    };

    StoreSubscription.prototype.close = function () {
        this.closed = true;
        this.handler = null;
        return true;
    };

    Store.prototype.subscribe = function (
        keyOrHandler,
        handlerOrRevision,
        maybeRevision
    ) {
        var keyFilter = null;
        var handler = null;
        var afterRevision;

        if (typeof keyOrHandler == "function") {
            handler = keyOrHandler;
            afterRevision = handlerOrRevision;
        } else if (keyOrHandler === null || keyOrHandler === void 0) {
            if (typeof handlerOrRevision == "function") {
                handler = handlerOrRevision;
                afterRevision = maybeRevision;
            } else {
                afterRevision = handlerOrRevision;
            }
        } else {
            keyFilter = String(keyOrHandler);
            if (!keyFilter.length) fail("subscription key must not be empty");
            if (typeof handlerOrRevision == "function") {
                handler = handlerOrRevision;
                afterRevision = maybeRevision;
            } else {
                afterRevision = handlerOrRevision;
            }
        }

        return new StoreSubscription(
            this, keyFilter, handler, afterRevision);
    };

    function store(name) {
        ensureLoaded();
        return new Store(name);
    }

    function open(pathValue) {
        var xo = ensureLoaded();
        var staged;
        var handleValue;
        var path = pathText(pathValue);

        staged = xo.stageHex(utf8ToHex(path));
        if (!staged) failNative("path staging failed");

        handleValue = xo.openStaged(0);
        if (!(handleValue > 0)) failNative("open failed");
        return new Database(handleValue);
    }

    function version() {
        var result = ensureLoaded().version(0);
        if (result === void 0) failNative("version query failed");
        return String(result);
    }

    function sqliteVersion() {
        var result = ensureLoaded().sqliteVersion(0);
        if (result === void 0) failNative("SQLite version query failed");
        return String(result);
    }

    function abiVersion() {
        return ensureLoaded().abiVersion(0);
    }

    function handleCount() {
        return ensureLoaded().handleCount(0);
    }

    function toSafeNumber(exactValue) {
        var text = canonicalUnsigned(exactValue, "9007199254740991", "exact integer");
        return Number(text);
    }

    var api = {};
    api.load = load;
    api.unload = unload;
    api.open = open;
    api.store = store;
    api.version = version;
    api.sqliteVersion = sqliteVersion;
    api.abiVersion = abiVersion;
    api.lastErrorJSON = lastErrorJSON;
    api.handleCount = handleCount;
    api.useStructuredCodec = useStructuredCodec;
    api.int64 = function (value) { return new Int64Value(value); };
    api.bytes = function (hex) { return new BytesValue(hex); };
    api.bytesFromArray = bytesFromArray;
    api.toSafeNumber = toSafeNumber;
    api.Database = Database;
    api.Store = Store;
    api.StoreSubscription = StoreSubscription;
    api.ObjectStore = ObjectStore;
    api.ObjectSubscription = ObjectSubscription;
    api.Int64 = Int64Value;
    api.Bytes = BytesValue;
    api.ValueType = {
        NULL: TYPE_NULL,
        BOOL: TYPE_BOOL,
        INT32: TYPE_INT32,
        INT64: TYPE_INT64,
        DOUBLE: TYPE_DOUBLE,
        UTF8: TYPE_UTF8,
        BYTES: TYPE_BYTES,
        ARRAY: TYPE_ARRAY,
        OBJECT: TYPE_OBJECT
    };
    api.TransactionMode = {
        DEFERRED: TX_DEFERRED,
        IMMEDIATE: TX_IMMEDIATE,
        EXCLUSIVE: TX_EXCLUSIVE
    };
    api.loadedSpec = function () {
        return loadedSpec;
    };
    return api;
}());
