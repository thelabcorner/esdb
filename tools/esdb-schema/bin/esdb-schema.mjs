#!/usr/bin/env node
/*
 * esdb-schema CLI entry point. See src/cli.mjs for the command surface.
 */

import { CliError, run } from "../src/cli.mjs";

function report(error) {
    if (error instanceof CliError) {
        process.stderr.write(`${error.message}\n`);
        return error.exitCode;
    }
    if (error && error.name === "IrValidationError" && Array.isArray(error.diagnostics)) {
        for (const diagnostic of error.diagnostics) {
            process.stderr.write(`${diagnostic.code} ${diagnostic.path} ${diagnostic.message}\n`);
        }
        return 1;
    }
    process.stderr.write(`${error && error.message ? `${error.name}: ${error.message}` : String(error)}\n`);
    return 1;
}

try {
    process.exitCode = run(process.argv.slice(2));
} catch (error) {
    process.exitCode = report(error);
}
