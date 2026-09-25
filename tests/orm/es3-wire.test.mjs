/*
 * Generated ES3 facade: no-SQL guarantee, named-operation surface, U1 wire
 * decoding, int64 lanes, nullable lanes, and failure handling.
 *
 * The facade is ES3 source, which is a strict subset of the Node vm's
 * language, so it runs directly in `node:vm` against a fake bridge. This is
 * NOT live Illustrator validation (no ExternalObject host is involved).
 */

import vm from "node:vm";
import { join } from "node:path";

import {
    EXAMPLE_GENERATED,
    assert,
    assertDeepEqual,
    assertEqual,
    assertThrows,
    freshWorkDir,
    loadFixture,
    readText,
    runCli,
    sealFixture
} from "./helpers.mjs";

const BRIDGE_CALL_PATTERN = /xo\.([A-Za-z_][A-Za-z0-9_]*)\(/g;

function loadFacade(code, bridge) {
    const sandbox = {
        ExternalObject: function () {
            return bridge;
        },
        Error,
        String,
        Number,
        Math,
        isFinite,
        parseInt
    };
    vm.runInNewContext(code, sandbox);
    const globalName = Object.keys(sandbox).find((key) => key.startsWith("ESDB_ORM_"));
    assert(globalName !== undefined, "facade global was created");
    return sandbox[globalName];
}

function createFakeBridge(hash, overrides = {}) {
    const calls = [];
    const bridge = {
        ormVersion: () => hash,
        ormPing: () => 42,
        ormOpen: (dummy, pathHex) => {
            calls.push(["ormOpen", pathHex]);
            return "U1:H:7";
        },
        ormClose: (handle) => {
            calls.push(["ormClose", handle]);
            return "U1:C:1";
        },
        ormLastError: () => "U1:O",
        userFindById: (handle, id) => {
            calls.push(["userFindById", handle, id]);
            return "U1:R:4|i|31|t|416461|t|61|i|3132";
        },
        userFindByEmail: (handle, email) => {
            calls.push(["userFindByEmail", handle, email]);
            return "U1:N";
        },
        userList: (handle, limit, offset) => {
            calls.push(["userList", handle, limit, offset]);
            return "U1:L:1|4|i|31|t|416461|t|61|i|3132";
        },
        userInsert: (...args) => {
            calls.push(["userInsert", ...args]);
            return "U1:C:1";
        },
        userUpdateName: (...args) => {
            calls.push(["userUpdateName", ...args]);
            return "U1:C:1";
        },
        userDeleteById: (...args) => {
            calls.push(["userDeleteById", ...args]);
            return "U1:C:0";
        },
        unload: () => {
            calls.push(["unload"]);
        }
    };
    Object.assign(bridge, overrides);
    return { bridge, calls };
}

function hexOf(text) {
    return Buffer.from(text, "utf8").toString("hex").toUpperCase();
}

export async function run() {
    const goldenIr = JSON.parse(readText(join(EXAMPLE_GENERATED, "user.ir.json")));
    const hash = goldenIr.integrity.hash;
    const code = readText(join(EXAMPLE_GENERATED, "es3", "user_repository.jsx"));

    /* Static guarantees: no SQL, ES3-only syntax, named operations only. */
    const sqlPatterns = [
        /\bSELECT\b/i,
        /\bINSERT\s+INTO\b/i,
        /\bDELETE\s+FROM\b/i,
        /\bUPDATE\s+\S+\s+SET\b/i,
        /\bCREATE\s+TABLE\b/i,
        /\bDROP\s+TABLE\b/i,
        /\bPRAGMA\b/i,
        /\bWHERE\b/i,
        /\bVALUES\s*\(/i
    ];
    for (const pattern of sqlPatterns) {
        assert(!pattern.test(code), `ES3 facade must not contain SQL (${pattern})`);
    }
    assert(!code.includes("=>"), "no arrow functions");
    assert(!code.includes("`"), "no template literals");
    assert(!/\bconst\b/.test(code), "no const");
    assert(!/\blet\b/.test(code), "no let");
    assert(!/\bclass\b/.test(code), "no class");
    assert(!/\bJSON\b/.test(code), "no JSON global (not available in ES3)");
    assert(!/Object\.keys|Array\.isArray|\.forEach\(|\.trim\(/.test(code), "no ES5 library calls");

    const bridgeCalls = [...code.matchAll(BRIDGE_CALL_PATTERN)].map((match) => match[1]);
    assertDeepEqual(
        [...new Set(bridgeCalls)].sort(),
        [
            "ormClose",
            "ormLastError",
            "ormOpen",
            "userDeleteById",
            "userFindByEmail",
            "userFindById",
            "userInsert",
            "userList",
            "userUpdateName"
        ].sort(),
        "only generated named bridge operations are invoked"
    );
    assert(code.includes("candidate.ormVersion(0)"), "load performs the hash handshake");
    assert(code.includes("candidate.ormPing(0)"), "load performs the ping handshake");

    /* Functional: row/rows/changes decode and lane encoding. */
    {
        const { bridge, calls } = createFakeBridge(hash);
        const api = loadFacade(code, bridge);
        assertEqual(api.irHash, hash, "facade exposes the IR hash");
        assertDeepEqual(api.columns, ["id", "name", "email", "createdAt"], "facade exposes logical columns");
        assertDeepEqual(
            api.operations,
            ["deleteById", "findByEmail", "findById", "insert", "list", "updateName"],
            "operation surface matches the IR query order"
        );
        api.load("lib:fake");
        const handle = api.open("C:/tmp/user.sqlite");
        assertEqual(handle, 7, "open returns the bridge handle");
        const repo = api.repository(handle);
        assertDeepEqual(repo.findById(1), { id: 1, name: "Ada", email: "a", createdAt: 12 }, "row decode");
        assertEqual(repo.findByEmail("nobody@example.com"), null, "one-cardinality miss returns null");
        assertEqual(repo.list(10, 0).length, 1, "list decode");
        assertEqual(repo.insert({ id: 2, name: "Grace", email: "g@example.com", createdAt: 99 }), 1, "insert changes");
        assertEqual(repo.updateName(1, "Ada King"), 1, "update changes");
        assertEqual(repo.deleteById(1), 0, "delete changes");
        api.close(handle);
        api.unload();
        assertDeepEqual(
            calls.map((call) => call[0]),
            [
                "ormOpen",
                "userFindById",
                "userFindByEmail",
                "userList",
                "userInsert",
                "userUpdateName",
                "userDeleteById",
                "ormClose",
                "unload"
            ],
            "bridge call order"
        );
        assertDeepEqual(calls[1], ["userFindById", 7, "i:1"], "int64 parameter lane");
        assertDeepEqual(calls[4].slice(0, 4), ["userInsert", 7, "i:2", "t:4772616365"], "insert parameter lanes");
    }

    /* int64 exact mode and safe-range enforcement. */
    {
        const big = "9007199254740993";
        const { bridge } = createFakeBridge(hash, {
            userFindById: () => `U1:R:4|e|${hexOf(big)}|t|416461|t|61|i|3132`
        });
        const api = loadFacade(code, bridge);
        api.load("lib:fake");
        const row = api.repository(7).findById(big, { exactIntegers: true });
        assertEqual(row.id, big, "exact mode returns decimal strings");
    }
    {
        const big = "9007199254740993";
        const { bridge } = createFakeBridge(hash, {
            userFindById: () => `U1:R:4|i|${hexOf(big)}|t|416461|t|61|i|3132`
        });
        const api = loadFacade(code, bridge);
        api.load("lib:fake");
        assertThrows(() => api.repository(7).findById(1), "safe integer range", "unsafe value without exact mode throws");
    }

    /* Error wire and lastError. */
    {
        const messageHex = hexOf("boom");
        const { bridge } = createFakeBridge(hash, {
            userFindById: () => `U1:E:-36:${messageHex}`
        });
        const api = loadFacade(code, bridge);
        api.load("lib:fake");
        const error = assertThrows(() => api.repository(7).findById(1), "boom", "error wire surfaces the message");
        assert(error.message.includes("-36"), "error wire surfaces the status");
        assertEqual(api.lastError().ok, true, "lastError ok");
    }
    {
        const messageHex = hexOf("boom");
        const { bridge } = createFakeBridge(hash, {
            ormLastError: () => `U1:E:-36:${messageHex}`
        });
        const api = loadFacade(code, bridge);
        api.load("lib:fake");
        const last = api.lastError();
        assertEqual(last.ok, false, "lastError reports failure");
        assertEqual(last.status, -36, "lastError status");
        assertEqual(last.message, "boom", "lastError message");
    }

    /* Load guards and handle lifecycle. */
    {
        const { bridge, calls } = createFakeBridge("0".repeat(64));
        const api = loadFacade(code, bridge);
        assertThrows(() => api.load("lib:fake"), "hash mismatch", "load rejects a bridge built from another IR");
        assert(calls.some((call) => call[0] === "unload"), "failed load unloads the candidate");
    }
    {
        const { bridge } = createFakeBridge(hash);
        const api = loadFacade(code, bridge);
        api.load("lib:fake");
        api.open("C:/tmp/x.sqlite");
        assertThrows(() => api.unload(), "database handle", "unload refuses while handles are open");
        api.close(7);
        assertEqual(api.unload(), true, "unload succeeds after close");
    }
    {
        const { bridge } = createFakeBridge(hash, { userFindById: () => "garbage" });
        const api = loadFacade(code, bridge);
        api.load("lib:fake");
        assertThrows(() => api.repository(7).findById(1), "invalid bridge wire version", "malformed wire rejected");
    }

    /* Byte-exact text transport: astral characters and embedded NUL. */
    {
        const { bridge, calls } = createFakeBridge(hash);
        const api = loadFacade(code, bridge);
        api.load("lib:fake");
        const text = "a\u0000b\u{1F600}";
        api.repository(7).insert({ id: 1, name: text, email: "e", createdAt: 0 });
        const lane = calls.find((call) => call[0] === "userInsert")[3];
        assert(lane.startsWith("t:"), "text lane");
        assertEqual(Buffer.from(lane.slice(2), "hex").toString("utf8"), text, "text round-trips byte-exactly");
    }
    {
        const text = "astral \u{1F600} text";
        const { bridge } = createFakeBridge(hash, {
            userFindById: () => `U1:R:4|i|31|t|${hexOf(text)}|t|61|i|3132`
        });
        const api = loadFacade(code, bridge);
        api.load("lib:fake");
        assertEqual(api.repository(7).findById(1).name, text, "astral text decodes");
    }

    /* Nullable lanes (types fixture). */
    {
        const work = freshWorkDir("es3-wire");
        const typesSealed = sealFixture(loadFixture("types.ir.json"), work, "types");
        const typesDir = join(work, "gen");
        assertEqual(runCli(["build", typesSealed, "--out", typesDir]).status, 0, "types build");
        const metricCode = readText(join(typesDir, "es3", "metric_repository.jsx"));
        const metricHash = JSON.parse(readText(join(typesDir, "bridge", "metric_orm_bridge.json"))).ir_hash;
        const { bridge, calls } = createFakeBridge(metricHash, {
            metricInsert: (...args) => {
                calls.push(["metricInsert", ...args]);
                return "U1:C:1";
            },
            metricUpdateNote: (...args) => {
                calls.push(["metricUpdateNote", ...args]);
                return "U1:C:1";
            }
        });
        const api = loadFacade(metricCode, bridge);
        api.load("lib:fake");
        const repo = api.repository(7);
        assertEqual(repo.insert({ id: 1, label: "x", payload: null, note: null }), 1, "nullable insert");
        assertDeepEqual(calls[0], ["metricInsert", 7, "i:1", "t:78", "n", "n"], "nullable lanes use n");
        assertThrows(
            () => repo.insert({ id: 1, label: null, payload: null, note: null }),
            "may not be null",
            "non-nullable null rejected"
        );
        assertEqual(repo.updateNote(1, null), 1, "nullable update set accepts null");
        assertDeepEqual(calls[calls.length - 1], ["metricUpdateNote", 7, "i:1", "n"], "nullable set lane");
    }
}
