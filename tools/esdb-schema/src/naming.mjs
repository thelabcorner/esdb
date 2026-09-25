/*
 * esdb-schema: deterministic symbol naming shared by all code generators.
 */

export function splitWords(name) {
    return String(name)
        .replace(/([a-z0-9])([A-Z])/g, "$1_$2")
        .replace(/[^A-Za-z0-9]+/g, "_")
        .split("_")
        .filter((part) => part.length > 0);
}

export function toPascal(name) {
    return splitWords(name)
        .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
        .join("");
}

export function toCamel(name) {
    const pascal = toPascal(name);
    return pascal.length === 0 ? pascal : pascal.charAt(0).toLowerCase() + pascal.slice(1);
}

export function toSnake(name) {
    return splitWords(name)
        .map((part) => part.toLowerCase())
        .join("_");
}

export function toUpperSnake(name) {
    return toSnake(name).toUpperCase();
}

export function es3GlobalName(schemaName) {
    return `ESDB_ORM_${toUpperSnake(schemaName)}`;
}

export function cppNamespace(schemaName) {
    return `esdb_generated_${toSnake(schemaName)}`;
}

export function queryMethodName(queryName) {
    const index = queryName.indexOf(".");
    const suffix = index >= 0 ? queryName.slice(index + 1) : queryName;
    return toCamel(suffix);
}

export function querySurfaceName(schemaName, queryName) {
    const index = queryName.indexOf(".");
    if (index < 0 || toCamel(queryName.slice(0, index)) === toCamel(schemaName)) {
        return queryMethodName(queryName);
    }
    return `${toCamel(queryName.slice(0, index))}${toPascal(queryMethodName(queryName))}`;
}

export function bridgeFunctionName(schemaName, queryName) {
    return `${toCamel(schemaName)}${toPascal(querySurfaceName(schemaName, queryName))}`;
}

export function bridgeDefaultLibrary(schemaName) {
    return `lib:${toPascal(schemaName)}OrmBridge`;
}
