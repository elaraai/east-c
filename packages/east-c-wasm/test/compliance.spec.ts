/**
 * WASM compliance test runner.
 *
 * Loads the same IR JSON files as east-c's test_compliance.c and executes
 * them via east-c-wasm. Test platform functions (testPass, testFail, test,
 * describe) execute the body inline — matching C's synchronous model — rather
 * than delegating to node:test's async scheduler.
 *
 * Usage:
 *   npm run test:compliance
 *   IR_DIR=/path/to/ir npm run test:compliance
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
import type { EastWasm } from '../src/index.js';

/**
 * Inline test platform that executes bodies synchronously (like C does).
 * test/describe just call the body directly — no node:test delegation.
 */
const testPass = East.platform("testPass", [], NullType);
const testFail = East.platform("testFail", [StringType], NullType);
const testPlatform = East.asyncPlatform("test", [StringType, AsyncFunctionType([], NullType)], NullType);
const describePlatform = East.asyncPlatform("describe", [StringType, AsyncFunctionType([], NullType)], NullType);

const WasmTestImpl: PlatformFunction[] = [
    testPass.implement(() => { }),
    testFail.implement((message: string) => { assert.fail(message); }),
    testPlatform.implement(async (name: string, body: () => Promise<null>) => { await body(); }),
    describePlatform.implement(async (name: string, body: () => Promise<null>) => { await body(); }),
];

const IR_DIR = process.env['IR_DIR'] ?? '/tmp/east-test-ir';
const encodeIR = encodeBeast2For(IRType);
const decodeIRJSON = decodeJSONFor(IRType);

describe('east-c-wasm compliance', async () => {
    let wasm: EastWasm;

    test('init', async () => {
        wasm = await createEastWasm();
        registerPlatformFunctions(wasm, WasmTestImpl);
    });

    // Discover all IR JSON files
    let files: string[];
    try {
        files = readdirSync(IR_DIR)
            .filter(f => f.endsWith('.json'))
            .sort();
    } catch {
        console.error(`No IR directory found at ${IR_DIR}. Run: cd ../east && EXPORT_TEST_IR=${IR_DIR} npm test`);
        files = [];
    }

    for (const file of files) {
        const suiteName = file.replace(/\.json$/, '');

        test(suiteName, async () => {
            const jsonBytes = readFileSync(join(IR_DIR, file));
            const ir = decodeIRJSON(jsonBytes);
            const irBytes = encodeIR(ir);

            const handle = wasm.compile(irBytes);
            try {
                await wasm.call(handle);
            } finally {
                wasm.free(handle);
            }
        });
    }
});
