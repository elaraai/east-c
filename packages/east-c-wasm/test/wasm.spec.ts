/**
 * Copyright (c) 2025 Elara AI Pty Ltd
 * Dual-licensed under AGPL-3.0 and commercial license. See LICENSE for details.
 */

import { describe, test } from 'node:test';
import assert from 'node:assert/strict';

import {
    East,
    IntegerType,
    FloatType,
    StringType,
    BooleanType,
    NullType,
    ArrayType,
    SetType,
    DictType,
    StructType,
    VariantType,
    RecursiveType,
    variant,
    decodeBeast2,
    decodeBeast2For,
} from '@elaraai/east';
import { IRType, encodeBeast2For } from '@elaraai/east/internal';

import { createEastWasm } from '../src/index.js';
import type { EastWasm } from '../src/index.js';
import { registerPlatformFunctions, encodeArgsList } from '../src/common.js';

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
        const result = await wasm.call(handle);
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
        const result = await wasm.call(handle);
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
        const result = await wasm.call(handle);
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
        const result = await wasm.call(handle);
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
        const result = await wasm.call(handle);
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
        const result = await wasm.call(handle);
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
        const result = await wasm.call(handle);
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

        await assert.rejects(
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
        const result = await wasm.call(handle);
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

    await test('platform function — non-generic', async () => {
        // Define a platform function that doubles an integer
        const double = East.platform("test_double", [IntegerType], IntegerType);

        // Register using registerPlatformFunctions bridge
        registerPlatformFunctions(wasm, [double.implement((x: bigint) => x * 2n)]);

        // Use a zero-arg function that calls the platform function internally
        const fn = East.function([], IntegerType, ($) => {
            const x = $.const(21n, IntegerType);
            return double(x);
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = await wasm.call(handle);
        assert.ok(result, 'should return a result');

        const decoded = decodeBeast2(result);
        assert.equal(decoded.value, 42n);
        wasm.free(handle);
    });

    await test('platform function — multiple calls', async () => {
        // test_double is already registered
        const double = East.platform("test_double", [IntegerType], IntegerType);

        // Call the platform function multiple times
        const fn = East.function([], IntegerType, ($) => {
            const a = $.let(double($.const(3n, IntegerType)), IntegerType);  // 6
            const b = $.let(double(a), IntegerType);                          // 12
            return double(b);                                                  // 24
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = await wasm.call(handle);
        assert.ok(result, 'should return a result');

        const decoded = decodeBeast2(result);
        assert.equal(decoded.value, 24n);
        wasm.free(handle);
    });

    await test('large IR — stress-test compilation', async () => {
        // Build a function with many operations to stress-test WASM memory
        const fn = East.function([], IntegerType, ($) => {
            const sum = $.let(0n, IntegerType);
            const i = $.let(0n, IntegerType);
            // Large while loop with struct creation and string ops
            $.while(East.less(i, 100n), ($) => {
                const s = $.let(East.str`item_${i}_value`, StringType);
                $.assign(sum, sum.add(s.length()));
                $.assign(i, i.add(1n));
            });
            return sum;
        });

        const irBytes = toIRBytes(fn);

        // Verify IR is non-trivially large
        assert.ok(irBytes.length > 500, `IR should be large, got ${irBytes.length} bytes`);

        const handle = wasm.compile(irBytes);
        const result = await wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        // "item_X_value" is 12-14 chars, sum of all string lengths
        assert.ok((decoded.value as bigint) > 1000n, `sum should be > 1000, got ${decoded.value}`);
        wasm.free(handle);
    });

    await test('function with closures and captures', async () => {
        // This simulates the pattern used in UI rendering:
        // a function that returns another function (closure with captures)
        const fn = East.function([], IntegerType, ($) => {
            const multiplier = $.const(10n, IntegerType);
            const transform = $.const(
                East.function([IntegerType], IntegerType, ($, x) => {
                    return x.multiply(multiplier);
                })
            );
            return transform(5n);
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = await wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        assert.equal(decoded.value, 50n);
        wasm.free(handle);
    });

    // ========================================================================
    // Stress tests (inspired by profile_generator.ts)
    // ========================================================================

    await test('stress — array generate, map, filter, reduce (2000)', async () => {
        const fn = East.function([], IntegerType, ($) => {
            const arr = $.let(East.Array.generate(2000n, IntegerType, ($, i) => i));
            const squared = $.let(arr.map(($, x) => x.multiply(x)));
            const evens = $.let(squared.filter(($, x) => x.remainder(2n).equals(0n)));
            const sum = $.let(evens.reduce(($, acc, x) => acc.add(x), 0n));
            return sum.add(evens.length());
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = await wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        assert.ok((decoded.value as bigint) > 0n, `expected positive result, got ${decoded.value}`);
        wasm.free(handle);
    });

    await test('stress — nested loops with conditionals (50x50)', async () => {
        const fn = East.function([], IntegerType, ($) => {
            const result = $.let(0n, IntegerType);
            const i = $.let(0n, IntegerType);
            $.while(East.less(i, 50n), ($) => {
                const j = $.let(0n, IntegerType);
                $.while(East.less(j, 50n), ($) => {
                    $.if(i.add(j).remainder(2n).equals(0n), ($) => {
                        $.assign(result, result.add(i.multiply(j)));
                    }).else(($) => {
                        $.assign(result, result.subtract(1n));
                    });
                    $.assign(j, j.add(1n));
                });
                $.assign(i, i.add(1n));
            });
            return result;
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = await wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        assert.ok(typeof decoded.value === 'bigint', `expected bigint, got ${typeof decoded.value}`);
        wasm.free(handle);
    });

    await test('stress — variant matching over array', async () => {
        const ResultType = VariantType({
            success: IntegerType,
            error: StringType,
            pending: NullType,
        });

        const items = Array.from({ length: 60 }, (_, i) =>
            i % 3 === 0 ? variant("success" as const, BigInt(i))
            : i % 3 === 1 ? variant("error" as const, "err")
            : variant("pending" as const, null)
        );

        const fn = East.function([], IntegerType, ($) => {
            const arr = $.let(items, ArrayType(ResultType));
            const total = $.let(0n, IntegerType);
            $.for(arr, ($, item) => {
                $.match(item, {
                    success: ($, val) => { $.assign(total, total.add(val)); },
                    error: ($, _msg) => { $.assign(total, total.subtract(1n)); },
                    pending: ($) => { /* noop */ },
                });
            });
            return total;
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = await wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        // success values: 0,3,6,...,57 → sum = 570, minus 20 errors = 550
        assert.equal(decoded.value, 550n);
        wasm.free(handle);
    });

    await test('stress — string generate, map, filter', async () => {
        const fn = East.function([], IntegerType, ($) => {
            const strings = $.let(East.Array.generate(50n, StringType, ($, i) =>
                East.str`string_number_${i}_with_padding`
            ));
            const upper = $.let(strings.map(($, s) => s.upperCase()));
            const filtered = $.let(upper.filter(($, s) => s.contains("5")));
            const lengths = $.let(filtered.map(($, s) => s.length()));
            return lengths.sum();
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = await wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        assert.ok((decoded.value as bigint) > 0n, `expected positive result, got ${decoded.value}`);
        wasm.free(handle);
    });

    await test('stress — deep struct nesting', async () => {
        // Build nested struct type: { value: Integer, nested: { value: Integer, nested: ... } }
        let currentType = StructType({ value: IntegerType });
        for (let i = 1; i < 5; i++) {
            currentType = StructType({
                value: IntegerType,
                nested: currentType,
                extra: StringType,
            });
        }
        const DeepType = currentType;

        // Build the value matching the nested struct type
        type DeepValue = { value: bigint } | { value: bigint; nested: DeepValue; extra: string };
        let currentValue: DeepValue = { value: 42n };
        for (let i = 1; i < 5; i++) {
            currentValue = {
                value: BigInt(i),
                nested: currentValue,
                extra: `level_${i}`,
            };
        }

        const fn = East.function([], IntegerType, ($) => {
            const deep = $.const(currentValue as { value: bigint; nested: { value: bigint }; extra: string }, DeepType);
            return deep.value;
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = await wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        assert.equal(decoded.value, 4n); // outermost level value
        wasm.free(handle);
    });

    await test('stress — dict with struct keys', async () => {
        const KeyType = StructType({ x: IntegerType, y: IntegerType });
        const ValueType = StructType({ name: StringType, score: FloatType });
        const DType = DictType(KeyType, ValueType);

        const fn = East.function([], IntegerType, ($) => {
            const dict = $.let(new Map(), DType);
            const i = $.let(0n, IntegerType);
            $.while(East.less(i, 20n), ($) => {
                const key = $.let({ x: i, y: i.multiply(2n) }, KeyType);
                const value = $.let({
                    name: East.str`item_${i}`,
                    score: i.toFloat().multiply(1.5),
                }, ValueType);
                $(dict.insert(key, value));
                $.assign(i, i.add(1n));
            });
            return dict.size();
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = await wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        assert.equal(decoded.value, 20n);
        wasm.free(handle);
    });

    await test('stress — set operations (insert, intersection)', async () => {
        const SType = SetType(IntegerType);

        const fn = East.function([], IntegerType, ($) => {
            const set1 = $.let(new Set<bigint>(), SType);
            const set2 = $.let(new Set<bigint>(), SType);
            const i = $.let(0n, IntegerType);
            $.while(East.less(i, 30n), ($) => {
                $(set1.insert(i));
                $.if(i.remainder(2n).equals(0n), ($) => {
                    $(set2.insert(i));
                });
                $.assign(i, i.add(1n));
            });
            const intersection = $.let(set1.intersection(set2));
            return set1.size().add(set2.size()).add(intersection.size());
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = await wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        // set1=30, set2=15, intersection=15 → 60
        assert.equal(decoded.value, 60n);
        wasm.free(handle);
    });

    await test('stress — mixed struct array with filter and reduce', async () => {
        const RecordType = StructType({
            id: IntegerType,
            name: StringType,
            scores: ArrayType(FloatType),
            active: BooleanType,
        });

        const recordsData = Array.from({ length: 30 }, (_, i) => ({
            id: BigInt(i),
            name: `record_${i}`,
            scores: [Number(i), Number(i) * 1.5, Number(i) * 2.0],
            active: i % 2 === 0,
        }));

        const fn = East.function([], FloatType, ($) => {
            const records = $.let(recordsData, ArrayType(RecordType));
            const activeRecords = $.let(records.filter(($, r) => r.active));
            const avgScores = $.let(activeRecords.map(($, r) => r.scores.mean()));
            const total = $.let(avgScores.reduce(($, acc, x) => acc.add(x), 0.0));
            return total;
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = await wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        assert.ok(typeof decoded.value === 'number', `expected number, got ${typeof decoded.value}`);
        assert.ok((decoded.value as number) > 0, `expected positive result, got ${decoded.value}`);
        wasm.free(handle);
    });

    await test('stress — float arithmetic (sqrt, sin, cos)', async () => {
        const fn = East.function([], FloatType, ($) => {
            const floats = $.let(East.Array.generate(30n, FloatType, ($, i) =>
                i.toFloat().add(0.5)
            ));
            const processed = $.let(floats.map(($, x) =>
                x.sqrt().add(x.sin()).multiply(x.cos().abs())
            ));
            return processed.sum();
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = await wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        assert.ok(typeof decoded.value === 'number', `expected number, got ${typeof decoded.value}`);
        wasm.free(handle);
    });

    await test('stress — large struct array input (5000 records)', async () => {
        // Pass 5000 struct records as an argument via callWithArgs
        const RecordType = StructType({
            id: IntegerType,
            name: StringType,
            priority: IntegerType,
            score: FloatType,
        });

        const records: { id: bigint; name: string; priority: bigint; score: number }[] = [];
        for (let i = 0; i < 5000; i++) {
            records.push({
                id: BigInt(5000 - i),
                name: `record_${String(i).padStart(5, '0')}`,
                priority: BigInt(i % 10),
                score: Math.sin(i) * 100,
            });
        }

        const fn = East.function([ArrayType(RecordType)], IntegerType, ($, arr) => {
            // Sort by priority descending (negate), then accumulate
            const sorted = $.let(arr.sort(($, r) => r.priority.negate()));
            const total = $.let(0n, IntegerType);
            $.for(sorted, ($, r) => {
                $.assign(total, total.add(r.id).add(r.priority));
            });
            return total;
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);

        const encodeRecords = encodeBeast2For(ArrayType(RecordType));
        const argsBytes = encodeArgsList([encodeRecords(records)]);
        const result = await wasm.callWithArgs(handle, argsBytes);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        assert.ok((decoded.value as bigint) > 0n, `expected positive result, got ${decoded.value}`);
        wasm.free(handle);
    });

    await test('stress — large struct array output (5000 generated records)', async () => {
        // Generate a large Array<Struct> at runtime in WASM and return it
        const RecordType = StructType({
            id: IntegerType,
            name: StringType,
            active: BooleanType,
        });
        const OutputType = ArrayType(RecordType);

        const fn = East.function([], OutputType, ($) => {
            return East.Array.generate(5000n, RecordType, ($, i) => {
                return East.value({
                    id: i,
                    name: East.str`record_${i}`,
                    active: i.remainder(2n).equals(0n),
                });
            });
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = await wasm.call(handle);
        assert.ok(result, 'should return result bytes');
        assert.ok(result.length > 1000, `result should be large, got ${result.length} bytes`);

        // Decode the full struct array using typed decoder
        const decode = decodeBeast2For(OutputType);
        const decoded = decode(result) as { id: bigint; name: string; active: boolean }[];
        assert.equal(decoded.length, 5000);
        assert.equal(decoded[0]!.id, 0n);
        assert.equal(decoded[0]!.name, 'record_0');
        assert.equal(decoded[0]!.active, true);
        assert.equal(decoded[4999]!.id, 4999n);
        assert.equal(decoded[4999]!.name, 'record_4999');
        assert.equal(decoded[4999]!.active, false);
        wasm.free(handle);
    });

    await test('stress — dict with struct keys and values (50 entries)', async () => {
        // Embed a large dict with struct keys in the IR
        const KeyType = StructType({ x: IntegerType, y: IntegerType, z: IntegerType });
        const ValueType = StructType({ data: StringType, count: IntegerType });

        const entries: [{ x: bigint; y: bigint; z: bigint }, { data: string; count: bigint }][] = [];
        for (let i = 0; i < 50; i++) {
            entries.push([
                { x: BigInt(i % 10), y: BigInt(Math.floor(i / 10)), z: BigInt(i) },
                { data: `value_${i}`, count: BigInt(i * 2) },
            ]);
        }

        const fn = East.function([], IntegerType, ($) => {
            const dict = $.let(new Map(entries), DictType(KeyType, ValueType));
            const result = $.let(0n, IntegerType);
            $.for(dict, ($, v, k) => {
                // Look up the same key — verifies struct key hashing works
                $.if(dict.has(k), ($) => {
                    const found = $.let(dict.get(k));
                    $.assign(result, result.add(found.count));
                });
            });
            return result;
        });

        const irBytes = toIRBytes(fn);
        assert.ok(irBytes.length > 2000, `IR should be large, got ${irBytes.length} bytes`);

        const handle = wasm.compile(irBytes);
        const result = await wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        // sum of count values: 0+2+4+...+98 = 2*sum(0..49) = 2450
        assert.equal(decoded.value, 2450n);
        wasm.free(handle);
    });

    await test('stress — complex sort 100 struct records', async () => {
        // Embed 100 records, sort by multiple fields, iterate result
        const RecordType = StructType({
            id: IntegerType,
            name: StringType,
            priority: IntegerType,
            score: FloatType,
        });

        const records: { id: bigint; name: string; priority: bigint; score: number }[] = [];
        for (let i = 0; i < 100; i++) {
            records.push({
                id: BigInt(100 - i),
                name: `record_${String(i).padStart(5, '0')}`,
                priority: BigInt(i % 10),
                score: Math.sin(i) * 100,
            });
        }

        const fn = East.function([], IntegerType, ($) => {
            const arr = $.let(records, ArrayType(RecordType));
            const sorted = $.let(arr.sort(($, r) =>
                r.priority.negate().multiply(1000000n).add(r.id)
            ));
            const total = $.let(0n, IntegerType);
            $.for(sorted, ($, r) => {
                $.assign(total, total.add(r.id).add(r.priority));
            });
            return total;
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = await wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        // sum of ids (1..100) = 5050, sum of priorities (0..9 * 10) = 450 → 5500
        assert.equal(decoded.value, 5500n);
        wasm.free(handle);
    });

    await test('stress — while loop 10k iterations', async () => {
        const fn = East.function([], IntegerType, ($) => {
            const counter = $.let(0n, IntegerType);
            const sum = $.let(0n, IntegerType);
            $.while(counter.lessThan(10000n), ($) => {
                $.assign(sum, sum.add(counter));
                $.assign(counter, counter.add(1n));
            });
            return sum;
        });

        const irBytes = toIRBytes(fn);
        const handle = wasm.compile(irBytes);
        const result = await wasm.call(handle);
        assert.ok(result);

        const decoded = decodeBeast2(result);
        // sum of 0..9999 = 49995000
        assert.equal(decoded.value, 49995000n);
        wasm.free(handle);
    });

    await test('gc does not crash', () => {
        wasm.gc();
    });
});
