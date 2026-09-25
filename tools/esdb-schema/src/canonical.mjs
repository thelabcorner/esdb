/*
 * esdb-schema: esdb-canonical-json-v1.
 *
 * Rules (normative text: orm/ir/esdb-ir-v1.md §6):
 *   - UTF-8, no BOM, no insignificant whitespace;
 *   - object keys sorted ascending by Unicode code point;
 *   - minimal string escaping; non-ASCII emitted raw; lone surrogates rejected;
 *   - JSON numbers are forbidden (IR v1 carries numbers as decimal strings);
 *   - the hash projection drops the top-level integrity/generator/annotations
 *     keys. integrity.hash is the SHA-256 of the canonical *projection* bytes,
 *     not of the sealed file: the file necessarily contains integrity itself,
 *     so sha256(sealed file) != integrity.hash by construction.
 *   - the sealed file is canonicalize(seal(doc)) with no trailing newline.
 */

import { createHash } from "node:crypto";

import { CANONICAL_ID } from "./ir.mjs";

const MAX_DEPTH = 128;

export class CanonicalError extends Error {
    constructor(code, path, message) {
        super(`${code} ${path} ${message}`);
        this.name = "CanonicalError";
        this.code = code;
        this.path = path;
    }
}

function encodeString(value, path) {
    let out = '"';
    for (const ch of value) {
        const code = ch.codePointAt(0);
        if (code >= 0xd800 && code <= 0xdfff) {
            throw new CanonicalError("IR024", path, "string contains an unpaired surrogate");
        }
        switch (ch) {
            case '"':
                out += '\\"';
                break;
            case "\\":
                out += "\\\\";
                break;
            case "\b":
                out += "\\b";
                break;
            case "\t":
                out += "\\t";
                break;
            case "\n":
                out += "\\n";
                break;
            case "\f":
                out += "\\f";
                break;
            case "\r":
                out += "\\r";
                break;
            default:
                if (code < 0x20) {
                    out += `\\u${code.toString(16).padStart(4, "0")}`;
                } else {
                    out += ch;
                }
        }
    }
    return `${out}"`;
}

function compareCodePoints(a, b) {
    let i = 0;
    let j = 0;
    while (i < a.length && j < b.length) {
        const ca = a.codePointAt(i);
        const cb = b.codePointAt(j);
        if (ca !== cb) {
            return ca < cb ? -1 : 1;
        }
        i += ca > 0xffff ? 2 : 1;
        j += cb > 0xffff ? 2 : 1;
    }
    return a.length - i - (b.length - j);
}

function encodeValue(value, path, depth) {
    if (depth > MAX_DEPTH) {
        throw new CanonicalError("IR024", path, "document nesting exceeds the canonicalization depth limit");
    }
    if (value === null) {
        return "null";
    }
    if (typeof value === "boolean") {
        return value ? "true" : "false";
    }
    if (typeof value === "number") {
        throw new CanonicalError(
            "IR022",
            path,
            "IR v1 forbids JSON numbers; carry numeric data as canonical decimal strings"
        );
    }
    if (typeof value === "string") {
        return encodeString(value, path);
    }
    if (typeof value === "undefined" || typeof value === "function" || typeof value === "symbol") {
        throw new CanonicalError("IR024", path, `unsupported value of type ${typeof value}`);
    }
    if (Array.isArray(value)) {
        const parts = [];
        for (let i = 0; i < value.length; i += 1) {
            if (!(i in value)) {
                throw new CanonicalError("IR024", `${path}[${i}]`, "array holes are not representable in JSON");
            }
            parts.push(encodeValue(value[i], `${path}[${i}]`, depth + 1));
        }
        return `[${parts.join(",")}]`;
    }
    const keys = Object.keys(value).sort(compareCodePoints);
    const parts = [];
    for (const key of keys) {
        if (!Object.prototype.hasOwnProperty.call(value, key)) {
            continue;
        }
        parts.push(`${encodeString(key, path)}:${encodeValue(value[key], `${path}.${key}`, depth + 1)}`);
    }
    return `{${parts.join(",")}}`;
}

export function canonicalize(value) {
    return encodeValue(value, "$", 0);
}

export function sha256Hex(input) {
    return createHash("sha256").update(input, typeof input === "string" ? "utf8" : undefined).digest("hex");
}

export function hashProjection(doc) {
    const projection = {};
    for (const key of Object.keys(doc)) {
        if (key === "integrity" || key === "generator" || key === "annotations") {
            continue;
        }
        projection[key] = doc[key];
    }
    return sha256Hex(canonicalize(projection));
}

export function seal(doc) {
    const sealed = { ...doc };
    sealed.integrity = {
        algorithm: "sha256",
        canonical: CANONICAL_ID,
        hash: hashProjection(doc)
    };
    return sealed;
}

export function verifySeal(doc) {
    const expected = hashProjection(doc);
    const actual = doc && doc.integrity && typeof doc.integrity.hash === "string" ? doc.integrity.hash : null;
    return {
        ok: actual !== null && actual === expected,
        expected,
        actual
    };
}

/* Canonical bytes of the sealed document as written to disk. The embedded
 * integrity.hash hashes the semantic projection, not these bytes. */
export function sealedBytes(doc) {
    return Buffer.from(canonicalize(seal(doc)), "utf8");
}

export function canonicalBytes(value) {
    return Buffer.from(canonicalize(value), "utf8");
}
