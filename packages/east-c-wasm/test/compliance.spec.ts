/**
 * WASM compliance test runner.
 *
 * Loads the same IR JSON files as east-c's test_compliance.c and executes
 * them via east-c-wasm. Each IR file runs in its own WASM instance so
 * files can execute concurrently.
 *
 * Test platform functions (testPass, testFail, test, describe) call the body
 * inline rather than delegating to node:test.
 *
 * Usage:
 *   npm run test:compliance
 *   IR_DIR=/path/to/ir npm run test:compliance
 *   FILTER=Integer npm run test:compliance   # run a single suite
 */

import { describe, test } from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';

import { East, decodeJSONFor, NullType, StringType, AsyncFunctionType } from '@elaraai/east';
import { IRType, encodeBeast2For } from '@elaraai/east/internal';
import type { PlatformFunction } from '@elaraai/east/internal';

import { createEastWasm } from '../src/index.js';
import { registerPlatformFunctions } from '../src/common.js';

const IR_DIR = process.env['IR_DIR'] ?? '/tmp/east-test-ir';
const FILTER = process.env['FILTER'] ?? '';
const encodeIR = encodeBeast2For(IRType);
const decodeIRJSON = decodeJSONFor(IRType);

/**
 * Build inline test platform functions.
 * Each WASM instance gets its own counters and indent state.
 */
function buildTestPlatform() {
    const testPassDef = East.platform("testPass", [], NullType);
    const testFailDef = East.platform("testFail", [StringType], NullType);
    const wasmTestDef = East.asyncPlatform("test", [StringType, AsyncFunctionType([], NullType)], NullType);
    const wasmDescribeDef = East.asyncPlatform("describe", [StringType, AsyncFunctionType([], NullType)], NullType);

    let testCount = 0;
    let passCount = 0;
    let failCount = 0;
    let indent = '';

    const impl: PlatformFunction[] = [
        testPassDef.implement(() => { }),
        testFailDef.implement((msg: string) => { assert.fail(msg); }),
        wasmTestDef.implement(async (name: string, body: () => Promise<null>) => {
            testCount++;
            const t0 = performance.now();
            try {
                body();
                const elapsed = performance.now() - t0;
                passCount++;
                console.log(`${indent}  \u2714 ${name} (${elapsed.toFixed(6)}ms)`);
            } catch (e) {
                const elapsed = performance.now() - t0;
                failCount++;
                const msg = e instanceof Error ? e.message : String(e);
                console.log(`${indent}  \u2718 ${name} (${elapsed.toFixed(6)}ms) - ${msg}`);
                throw e;
            }
        }),
        wasmDescribeDef.implement(async (name: string, body: () => Promise<null>) => {
            const prevIndent = indent;
            console.log(`${indent}\u25b6 ${name}`);
            indent += '  ';
            const t0 = performance.now();
            try {
                body();
                const elapsed = performance.now() - t0;
                indent = prevIndent;
                console.log(`${indent}\u2714 ${name} (${elapsed.toFixed(6)}ms)`);
            } catch (e) {
                indent = prevIndent;
                throw e;
            }
        }),
    ];

    return { impl, stats: () => ({ testCount, passCount, failCount }) };
}

// Discover all IR JSON files
let files: string[];
try {
    files = readdirSync(IR_DIR)
        .filter(f => f.endsWith('.json'))
        .filter(f => !FILTER || f.replace(/\.json$/, '') === FILTER)
        .sort();
} catch {
    console.error(`No IR directory found at ${IR_DIR}. Run: cd ../east && EXPORT_TEST_IR=${IR_DIR} npm test`);
    files = [];
}

// Each file gets its own WASM instance and runs independently
for (const file of files) {
    const suiteName = file.replace(/\.json$/, '');

    test(suiteName, async () => {
        const { impl, stats } = buildTestPlatform();

        const wasm = await createEastWasm();
        registerPlatformFunctions(wasm, impl);

        const jsonBytes = readFileSync(join(IR_DIR, file));
        const ir = decodeIRJSON(jsonBytes);
        const irBytes = encodeIR(ir);

        const handle = wasm.compile(irBytes);
        const t0 = performance.now();
        try {
            wasm.call(handle);
        } finally {
            wasm.free(handle);
        }
        const elapsed = performance.now() - t0;

        const { testCount, passCount, failCount } = stats();
        console.log(`\u2139 tests ${testCount}`);
        console.log(`\u2139 pass ${passCount}`);
        if (failCount > 0) console.log(`\u2139 fail ${failCount}`);
        console.log(`\u2139 duration_ms ${elapsed.toFixed(6)}`);
    });
}
