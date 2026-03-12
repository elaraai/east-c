/**
 * Copyright (c) 2025 Elara AI Pty Ltd
 * Dual-licensed under AGPL-3.0 and commercial license. See LICENSE for details.
 */

import { describe, test } from 'node:test';
import assert from 'node:assert/strict';

import {
    East,
    IntegerType,
    StringType,
    BooleanType,
    ArrayType,
    StructType,
    VariantType,
    RecursiveType,
    variant,
    decodeBeast2,
} from '@elaraai/east';
import { IRType, encodeBeast2For } from '@elaraai/east/internal';

import { createEastWasm } from '../src/index.js';
import type { EastWasm, CompiledHandle } from '../src/index.js';

// Beast2-full encoder for IR
const encodeIR = encodeBeast2For(IRType);

/** Helper: compile an East function to Beast2-full IR bytes */
function toIRBytes(fn: ReturnType<typeof East.function>): Uint8Array {
    const ir = fn.toIR().ir;
    return encodeIR(ir);
}

describe('east-c-wasm', async () => {
    let wasm: EastWasm;

    // Initialize WASM module once for all tests
    test('init', async () => {
        wasm = await createEastWasm();
        assert.ok(wasm, 'should create wasm instance');
    });

    await test('simple integer arithmetic', async () => {
        const fn = East.function([], IntegerType, ($) => {
            const a = $.let(2n, IntegerType);
            const b = $.let(3n, IntegerType);
            return a.add(b);
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = wasm.call(handle);
        assert.ok(result, 'should return a result');

        const decoded = decodeBeast2(result);
        assert.equal(decoded.value, 5n);
        wasm.free(handle);
    });

    await test('string operations', async () => {
        const fn = East.function([], StringType, ($) => {
            const a = $.const("hello ", StringType);
            const b = $.const("world", StringType);
            return a.concat(b);
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        assert.equal(decoded.value, "hello world");
        wasm.free(handle);
    });

    await test('array operations', async () => {
        const fn = East.function([], IntegerType, ($) => {
            const arr = $.let([1n, 2n, 3n, 4n, 5n], ArrayType(IntegerType));
            return arr.sum();
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        assert.equal(decoded.value, 15n);
        wasm.free(handle);
    });

    await test('variant matching', async () => {
        const ResultType = VariantType({
            ok: IntegerType,
            err: StringType,
        });

        const fn = East.function([], IntegerType, ($) => {
            const v = $.const(variant("ok", 42n), ResultType);
            const result = $.let(0n, IntegerType);
            $.match(v, {
                ok: ($, val) => { $.assign(result, val); },
                err: ($, _msg) => { $.assign(result, -1n); },
            });
            return result;
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        assert.equal(decoded.value, 42n);
        wasm.free(handle);
    });

    await test('recursive type — linked list equality', async () => {
        const LinkedListType = RecursiveType(self => VariantType({
            nil: BooleanType,
            cons: StructType({ head: BooleanType, tail: self }),
        }));

        const fn = East.function([], BooleanType, ($) => {
            const list1 = $.const(
                variant("cons", { head: true, tail: variant("nil", false) }),
                LinkedListType,
            );
            const list2 = $.const(
                variant("cons", { head: true, tail: variant("nil", false) }),
                LinkedListType,
            );
            return East.equal(list1, list2);
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        assert.equal(decoded.value, true);
        wasm.free(handle);
    });

    await test('recursive type — struct-based XML node', async () => {
        const fn = East.function([], StringType, ($) => {
            const XmlNodeType = RecursiveType(self => StructType({
                tag: StringType,
                children: ArrayType(self),
            }));
            const node = $.const({
                tag: "div",
                children: [],
            }, XmlNodeType);
            return node.unwrap().tag;
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        assert.equal(decoded.value, "div");
        wasm.free(handle);
    });

    await test('while loop', async () => {
        const fn = East.function([], IntegerType, ($) => {
            const sum = $.let(0n, IntegerType);
            const i = $.let(0n, IntegerType);
            $.while(East.less(i, 100n), ($) => {
                $.assign(sum, sum.add(i));
                $.assign(i, i.add(1n));
            });
            return sum;
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        assert.equal(decoded.value, 4950n); // sum of 0..99
        wasm.free(handle);
    });

    await test('runtime error — uncaught out of bounds', async () => {
        const fn = East.function([], IntegerType, ($) => {
            const arr = $.const([1n, 2n, 3n], ArrayType(IntegerType));
            return arr.get(10n); // out of bounds
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);

        assert.throws(
            () => wasm.call(handle),
            (err: Error) => {
                assert.ok(err.message.includes('east-c-wasm'), `error should be from east-c-wasm: ${err.message}`);
                return true;
            },
        );
        wasm.free(handle);
    });

    await test('runtime error — caught by try/catch in IR', async () => {
        const fn = East.function([], IntegerType, ($) => {
            const result = $.let(0n, IntegerType);
            const arr = $.const([1n, 2n, 3n], ArrayType(IntegerType));
            $.try($ => {
                $(arr.get(10n)); // out of bounds
                $.assign(result, 42n);
            }).catch(($, _message, _stack) => {
                $.assign(result, -1n);
            });
            return result;
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        assert.equal(decoded.value, -1n);
        wasm.free(handle);
    });

    await test('compile error — invalid bytes', async () => {
        assert.throws(
            () => wasm.compile(new Uint8Array([0, 1, 2, 3])),
            (err: Error) => {
                assert.ok(err.message.includes('compile'), `should mention compile: ${err.message}`);
                return true;
            },
        );
    });

    await test('gc does not crash', () => {
        wasm.gc();
    });
});
