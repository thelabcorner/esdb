/*
 * Deterministic symbol naming for derived query names.
 * Kept local to the frontend; the compiler owns binding-side naming.
 */

export function splitWords(name: string): string[] {
    return String(name)
        .replace(/([a-z0-9])([A-Z])/g, "$1_$2")
        .replace(/[^A-Za-z0-9]+/g, "_")
        .split("_")
        .filter((part) => part.length > 0);
}

export function toPascal(name: string): string {
    return splitWords(name)
        .map((part) => part.charAt(0).toUpperCase() + part.slice(1))
        .join("");
}
