/**
 * Copyright (c) 2025 Elara AI Pty Ltd
 * Dual-licensed under AGPL-3.0 and commercial license. See LICENSE for details.
 */

/**
 * @elaraai/east-c-wasm/common
 *
 * Platform-agnostic core for east-c-wasm. Contains types, the EastWasm interface,
 * and `createEastWasmFromModule` which takes an initialized Emscripten module
 * and returns an EastWasm instance.
 */

import {
    type EastTypeValue,
    ArrayType,
    encodeBeast2For,
    decodeBeast2For,
    decodeBeast2WithHandles,
    variant,
    EastTypeType,
} from "@elaraai/east";
import type { PlatformFunction } from "@elaraai/east/internal";

// Extend EmscriptenModule with east-c-wasm specific C exports
// and Emscripten runtime helpers that are on the module object but not in the base type.
export interface EastWasmModule extends EmscriptenModule {
    UTF8ToString: (ptr: number, maxBytesToRead?: number) => string;
    stringToUTF8: (str: string, outPtr: number, maxBytesToRead?: number) => void;
    lengthBytesUTF8: (str: string) => number;
    _east_wasm_init: () => void;
    _east_wasm_register_platform: (namePtr: number, isGeneric: number, isAsync: number, inputTypesPtr: number, inputTypesLen: number) => void;
    _east_wasm_compile: (irPtr: number, irLen: number) => number;
    _east_wasm_call: (handle: number, resultPtr: number, resultLenPtr: number, errorPtr: number, errorLenPtr: number) => number;
    _east_wasm_call_with_args: (handle: number, argsPtr: number, argsLen: number, resultPtr: number, resultLenPtr: number, errorPtr: number, errorLenPtr: number) => number;
    _east_wasm_free: (handle: number) => void;
    _east_wasm_gc: () => void;
    _east_wasm_malloc: (size: number) => number;
    _east_wasm_free_buf: (ptr: number) => void;
    _east_wasm_set_platform_context: (namePtr: number, tpPtr: number, tpLen: number) => void;
    _east_wasm_last_error: () => number; // returns pointer to null-terminated string, or 0
    _east_wasm_invoke_fn: (handleId: number, argsPtr: number, argsLen: number, resultPtr: number, resultLenPtr: number, errorPtr: number, errorLenPtr: number) => number;
    _east_wasm_compile_value: (bytesPtr: number, bytesLen: number, resultPtr: number, resultLenPtr: number, errorPtr: number, errorLenPtr: number) => number;
}

/** Result buffer size (1MB) — matches C side */
const RESULT_BUF_SIZE = 1024 * 1024;

/** Error buffer size (64KB) */
const ERROR_BUF_SIZE = 64 * 1024;

/**
 * Platform function implementation.
 * Receives Beast2-full encoded args, returns Beast2-full encoded result (or null).
 * All platform functions execute synchronously — async is not supported in WASM.
 */
export type PlatformFn = (args: Uint8Array[]) => Uint8Array | null;

/**
 * Generic platform function factory.
 * Called with Beast2-full encoded type params, returns a PlatformFn.
 */
export type GenericPlatformFactory = (typeParamsBytes: Uint8Array) => PlatformFn;

export interface PlatformRegistration {
    name: string;
    isGeneric: boolean;
    isAsync: boolean;
    fn?: PlatformFn | undefined;
    factory?: GenericPlatformFactory | undefined;
    /** Beast2-full encoded input types array — enables proper encoding of function args in the bridge */
    inputTypesBytes?: Uint8Array | undefined;
}

/**
 * Compiled function handle. Opaque — use call() and free().
 */
export type CompiledHandle = number & { __brand: 'CompiledHandle' };

/**
 * Result of compileValue(). Contains a JS value tree where functions
 * are callable WASM-backed handles.
 */
export interface CompiledValue {
    /** The decoded type from the beast2 header. */
    type: EastTypeValue;
    /** JS value tree — functions are callable wrappers backed by WASM handles. */
    value: any;
    /** All function handles allocated (for cleanup). */
    handles: CompiledHandle[];
    /** Release all WASM handles. Must be called when done. */
    free(): void;
}

/**
 * EastWasm instance. Created by calling `createEastWasm()` or `createEastWasmFromModule()`.
 */
export interface EastWasm {
    /** Register a platform function that will be called from WASM. */
    registerPlatform(reg: PlatformRegistration): void;

    /** Compile East IR from Beast2-full encoded bytes (type header + IR value). */
    compile(irBytes: Uint8Array): CompiledHandle;

    /** Execute a compiled function with no arguments. */
    call(handle: CompiledHandle): Uint8Array | null;

    /**
     * Execute a compiled function with Beast2-full encoded arguments.
     * Args format: [count:u32le][len1:u32le][beast2_full_1]...
     */
    callWithArgs(handle: CompiledHandle, argsBytes: Uint8Array): Uint8Array | null;

    /** Free a compiled function. Must be called when done. */
    free(handle: CompiledHandle): void;

    /** Run garbage collection on the WASM heap. */
    gc(): void;

    /**
     * Decode a beast2-full value, compiling embedded functions to WASM.
     * Returns a JS value tree where functions are callable WASM handles.
     * Platform must be registered before calling this.
     */
    compileValue(bytes: Uint8Array): CompiledValue;
}

/**
 * Options for creating an EastWasm instance.
 */
export interface EastWasmOptions {
    /** URL or path to east-c.wasm file. Auto-detected if not provided. */
    wasmUrl?: string | undefined;
    /** URL or path to east-c.js glue file. Auto-detected if not provided. */
    glueUrl?: string | undefined;
}

/**
 * Create an EastWasm instance from an already-initialized Emscripten module.
 * This is the platform-agnostic core — both Node.js and browser loaders use this.
 *
 * @param mod - Initialized Emscripten module
 * @param externalState - Optional external platform registry maps. When provided
 *   (e.g., from a browser loader that shares them with createPlatformBridge),
 *   the returned EastWasm instance uses these maps instead of creating new ones.
 */
export function createEastWasmFromModule(
    mod: EastWasmModule,
    externalState?: { platformFns: Map<string, PlatformRegistration>; genericCache: Map<string, PlatformFn> },
): EastWasm {
    // Platform function registry (JS side)
    const platformFns = externalState?.platformFns ?? new Map<string, PlatformRegistration>();
    const genericCache = externalState?.genericCache ?? new Map<string, PlatformFn>();

    // Pre-allocate result and error buffers in WASM memory
    const resultBufPtr = mod._malloc(RESULT_BUF_SIZE);
    const errorBufPtr = mod._malloc(ERROR_BUF_SIZE);
    const resultLenPtr = mod._malloc(4);
    const errorLenPtr = mod._malloc(4);

    // Initialize the C runtime
    mod._east_wasm_init();

    function writeErrorToWasm(msg: string, outPtr: number, outLenPtr: number): void {
        const encoded = new TextEncoder().encode(msg);
        const outLenView = new DataView(mod.HEAPU8.buffer, outLenPtr, 4);
        const capacity = outLenView.getUint32(0, true);
        const len = Math.min(encoded.length, capacity);
        new Uint8Array(mod.HEAPU8.buffer, outPtr, len).set(encoded.subarray(0, len));
        outLenView.setUint32(0, len, true);
    }

    function allocString(s: string): number {
        const len = mod.lengthBytesUTF8(s) + 1;
        const ptr = mod._malloc(len);
        mod.stringToUTF8(s, ptr, len);
        return ptr;
    }

    function writeBytes(data: Uint8Array): number {
        const ptr = mod._malloc(data.length);
        mod.HEAPU8.set(data, ptr);
        return ptr;
    }

    return {
        registerPlatform(reg: PlatformRegistration): void {
            platformFns.set(reg.name, reg);
            const namePtr = allocString(reg.name);
            let typesPtr = 0;
            let typesLen = 0;
            if (reg.inputTypesBytes && reg.inputTypesBytes.length > 0) {
                typesPtr = writeBytes(reg.inputTypesBytes);
                typesLen = reg.inputTypesBytes.length;
            }
            mod._east_wasm_register_platform(namePtr, reg.isGeneric ? 1 : 0, reg.isAsync ? 1 : 0, typesPtr, typesLen);
            mod._free(namePtr);
            if (typesPtr) mod._free(typesPtr);
        },

        compile(irBytes: Uint8Array): CompiledHandle {
            const irPtr = writeBytes(irBytes);
            const handle = mod._east_wasm_compile(irPtr, irBytes.length);
            mod._free(irPtr);
            if (handle === 0) {
                const errPtr = mod._east_wasm_last_error();
                const detail = errPtr ? mod.UTF8ToString(errPtr) : 'unknown error';
                throw new Error(`east-c-wasm compile: ${detail}`);
            }
            return handle as CompiledHandle;
        },

        call(handle: CompiledHandle): Uint8Array | null {
            new DataView(mod.HEAPU8.buffer, resultLenPtr, 4).setUint32(0, RESULT_BUF_SIZE, true);
            new DataView(mod.HEAPU8.buffer, errorLenPtr, 4).setUint32(0, ERROR_BUF_SIZE, true);

            const rc = mod._east_wasm_call(handle, resultBufPtr, resultLenPtr, errorBufPtr, errorLenPtr);

            if (rc !== 0) {
                const errLen = new DataView(mod.HEAPU8.buffer, errorLenPtr, 4).getUint32(0, true);
                const errBytes = new Uint8Array(mod.HEAPU8.buffer, errorBufPtr, errLen);
                throw new Error(`east-c-wasm: ${new TextDecoder().decode(errBytes)}`);
            }

            const resLen = new DataView(mod.HEAPU8.buffer, resultLenPtr, 4).getUint32(0, true);
            if (resLen === 0) return null;
            return new Uint8Array(mod.HEAPU8.buffer, resultBufPtr, resLen).slice();
        },

        callWithArgs(handle: CompiledHandle, argsBytes: Uint8Array): Uint8Array | null {
            const argsPtr = writeBytes(argsBytes);
            new DataView(mod.HEAPU8.buffer, resultLenPtr, 4).setUint32(0, RESULT_BUF_SIZE, true);
            new DataView(mod.HEAPU8.buffer, errorLenPtr, 4).setUint32(0, ERROR_BUF_SIZE, true);

            const rc = mod._east_wasm_call_with_args(handle, argsPtr, argsBytes.length, resultBufPtr, resultLenPtr, errorBufPtr, errorLenPtr);

            mod._free(argsPtr);

            if (rc !== 0) {
                const errLen = new DataView(mod.HEAPU8.buffer, errorLenPtr, 4).getUint32(0, true);
                const errBytes = new Uint8Array(mod.HEAPU8.buffer, errorBufPtr, errLen);
                throw new Error(`east-c-wasm: ${new TextDecoder().decode(errBytes)}`);
            }

            const resLen = new DataView(mod.HEAPU8.buffer, resultLenPtr, 4).getUint32(0, true);
            if (resLen === 0) return null;
            return new Uint8Array(mod.HEAPU8.buffer, resultBufPtr, resLen).slice();
        },

        free(handle: CompiledHandle): void {
            mod._east_wasm_free(handle);
        },

        gc(): void {
            mod._east_wasm_gc();
        },

        compileValue(bytes: Uint8Array): CompiledValue {
            const bytesPtr = writeBytes(bytes);

            // Use a larger result buffer for compile_value since output can be large
            const cvResultBufSize = Math.max(RESULT_BUF_SIZE, bytes.length);
            const cvResultBufPtr = mod._malloc(cvResultBufSize);
            const cvResultLenPtr = mod._malloc(4);
            const cvErrorBufPtr = mod._malloc(ERROR_BUF_SIZE);
            const cvErrorLenPtr = mod._malloc(4);

            new DataView(mod.HEAPU8.buffer, cvResultLenPtr, 4).setUint32(0, cvResultBufSize, true);
            new DataView(mod.HEAPU8.buffer, cvErrorLenPtr, 4).setUint32(0, ERROR_BUF_SIZE, true);

            const rc = mod._east_wasm_compile_value(
                bytesPtr, bytes.length,
                cvResultBufPtr, cvResultLenPtr, cvErrorBufPtr, cvErrorLenPtr,
            );
            mod._free(bytesPtr);

            if (rc !== 0) {
                const errLen = new DataView(mod.HEAPU8.buffer, cvErrorLenPtr, 4).getUint32(0, true);
                const errBytes = new Uint8Array(mod.HEAPU8.buffer, cvErrorBufPtr, errLen);
                const msg = new TextDecoder().decode(errBytes);
                mod._free(cvResultBufPtr);
                mod._free(cvResultLenPtr);
                mod._free(cvErrorBufPtr);
                mod._free(cvErrorLenPtr);
                throw new Error(`east-c-wasm compile_value: ${msg}`);
            }

            const resLen = new DataView(mod.HEAPU8.buffer, cvResultLenPtr, 4).getUint32(0, true);
            const resultBytes = new Uint8Array(mod.HEAPU8.buffer, cvResultBufPtr, resLen).slice();
            mod._free(cvResultBufPtr);
            mod._free(cvResultLenPtr);
            mod._free(cvErrorBufPtr);
            mod._free(cvErrorLenPtr);

            // Decode beast2-full result with handle-aware decoder
            const { type, value, handles: handleIds } = decodeBeast2WithHandles(
                resultBytes,
                (handleId: number, fnType: EastTypeValue) => {
                    return createCompiledHandleWrapper(
                        handleId as CompiledHandle, fnType, mod,
                        { resultBufPtr, errorBufPtr, resultLenPtr, errorLenPtr },
                    );
                },
            );

            const handles = handleIds.map(id => id as CompiledHandle);
            return {
                type,
                value,
                handles,
                free: () => handles.forEach(h => mod._east_wasm_free(h)),
            };
        },
    };
}

/**
 * Build the platform call bridge for Emscripten module options.
 * Must be set as `moduleOpts.js_platform_call` before module initialization.
 *
 * All platform functions execute synchronously. Async platform functions
 * are not supported in WASM — use the TypeScript runtime for async operations.
 */
export function createPlatformBridge(
    platformFns: Map<string, PlatformRegistration>,
    genericCache: Map<string, PlatformFn>,
    getMod: () => EastWasmModule,
): (namePtr: number, tpPtr: number, tpLen: number, argsPtr: number, argsLen: number, outPtr: number, outLenPtr: number) => number {
    return (
        namePtr: number,
        tpPtr: number, tpLen: number,
        argsPtr: number, argsLen: number,
        outPtr: number, outLenPtr: number,
    ): number => {
        const mod = getMod();

        try {
            const name = mod.UTF8ToString(namePtr);
            const reg = platformFns.get(name);
            if (!reg) {
                writeErrorToWasmBridge(mod, `platform function not registered: ${name}`, outPtr, outLenPtr);
                return 1;
            }

            // Decode args from WASM memory
            const argsData = new Uint8Array(mod.HEAPU8.buffer, argsPtr, argsLen).slice();
            const args = decodeArgsList(argsData);

            // Get the implementation
            let impl: PlatformFn;
            if (reg.isGeneric && reg.factory) {
                const tpBytes = tpLen > 0
                    ? new Uint8Array(mod.HEAPU8.buffer, tpPtr, tpLen).slice()
                    : new Uint8Array(0);
                const cacheKey = `${name}|${bufToHex(tpBytes)}`;
                let cached = genericCache.get(cacheKey);
                if (!cached) {
                    cached = reg.factory(tpBytes);
                    genericCache.set(cacheKey, cached);
                }
                impl = cached;
            } else if (reg.fn) {
                impl = reg.fn;
            } else {
                writeErrorToWasmBridge(mod, `platform function ${name} has no implementation`, outPtr, outLenPtr);
                return 1;
            }

            // Call the implementation (always synchronous)
            const result = impl(args);

            writeResultToWasm(mod, result, outPtr, outLenPtr);
            return 0;
        } catch (e) {
            const msg = e instanceof Error ? e.message : String(e);
            writeErrorToWasmBridge(mod, msg, outPtr, outLenPtr);
            return 1;
        }
    };
}

function writeResultToWasm(mod: EastWasmModule, result: Uint8Array | null, outPtr: number, outLenPtr: number): void {
    if (result && result.length > 0) {
        const outLenView = new DataView(mod.HEAPU8.buffer, outLenPtr, 4);
        const capacity = outLenView.getUint32(0, true);
        if (result.length > capacity) {
            writeErrorToWasmBridge(mod, 'platform result too large', outPtr, outLenPtr);
            return;
        }
        new Uint8Array(mod.HEAPU8.buffer, outPtr, result.length).set(result);
        outLenView.setUint32(0, result.length, true);
    } else {
        new DataView(mod.HEAPU8.buffer, outLenPtr, 4).setUint32(0, 0, true);
    }
}

function writeErrorToWasmBridge(mod: EastWasmModule, msg: string, outPtr: number, outLenPtr: number): void {
    const encoded = new TextEncoder().encode(msg);
    const outLenView = new DataView(mod.HEAPU8.buffer, outLenPtr, 4);
    const capacity = outLenView.getUint32(0, true);
    const len = Math.min(encoded.length, capacity);
    new Uint8Array(mod.HEAPU8.buffer, outPtr, len).set(encoded.subarray(0, len));
    outLenView.setUint32(0, len, true);
}

// ============================================================================
// Helpers
// ============================================================================

/** Sentinel value indicating a function handle in packed args */
const FN_HANDLE_SENTINEL = 0xFFFFFFFF;

/**
 * Decode args list from the packed format: [count:u32le][len1:u32le][data1]...
 *
 * When a sentinel length (0xFFFFFFFF) is encountered, the function handle header
 * [handle_id:u32][input_count:u32][type_len:u32][type_bytes...] is packed into
 * the Uint8Array entry so that callJsPlatformFn can detect and resolve it.
 */
export function decodeArgsList(data: Uint8Array): Uint8Array[] {
    if (data.length < 4) return [];
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const count = view.getUint32(0, true);
    const args: Uint8Array[] = [];
    let offset = 4;
    for (let i = 0; i < count; i++) {
        if (offset + 4 > data.length) break;
        const len = view.getUint32(offset, true);
        offset += 4;

        if (len === FN_HANDLE_SENTINEL) {
            // Function handle: pack sentinel + [handle_id][input_count][type_len][type_bytes]
            // into a single Uint8Array so callJsPlatformFn can detect it
            const headerStart = offset;
            if (offset + 12 > data.length) break;
            offset += 4; // handle_id
            offset += 4; // input_count
            const typeLen = view.getUint32(offset, true);
            offset += 4; // type_len
            if (offset + typeLen > data.length) break;
            offset += typeLen;
            // Pack: [sentinel:u32le][handle_id:u32le][input_count:u32le][type_len:u32le][type_bytes]
            const packed = new Uint8Array(4 + (offset - headerStart));
            const packedView = new DataView(packed.buffer);
            packedView.setUint32(0, FN_HANDLE_SENTINEL, true);
            packed.set(data.slice(headerStart, offset), 4);
            args.push(packed);
        } else {
            if (offset + len > data.length) break;
            args.push(data.slice(offset, offset + len));
            offset += len;
        }
    }
    return args;
}

/**
 * Check if an arg byte array is a packed function handle (starts with sentinel).
 */
function isFnHandleArg(argBytes: Uint8Array): boolean {
    if (argBytes.length < 16) return false; // sentinel(4) + handle(4) + count(4) + typeLen(4) minimum
    const view = new DataView(argBytes.buffer, argBytes.byteOffset, argBytes.byteLength);
    return view.getUint32(0, true) === FN_HANDLE_SENTINEL;
}

/**
 * Parse a packed function handle arg into its components.
 */
function parseFnHandleArg(argBytes: Uint8Array): { handleId: number; inputCount: number; fnTypeBytes: Uint8Array } {
    const view = new DataView(argBytes.buffer, argBytes.byteOffset, argBytes.byteLength);
    // Skip sentinel (4 bytes)
    const handleId = view.getUint32(4, true);
    const inputCount = view.getUint32(8, true);
    const typeLen = view.getUint32(12, true);
    const fnTypeBytes = argBytes.slice(16, 16 + typeLen);
    return { handleId, inputCount, fnTypeBytes };
}

/** Extract input/output types from a FunctionType or AsyncFunctionType value */
function extractFnSignature(fnType: EastTypeValue): { inputs: EastTypeValue[]; output: EastTypeValue } {
    if (fnType.type === 'Function' || fnType.type === 'AsyncFunction') {
        return { inputs: fnType.value.inputs, output: fnType.value.output };
    }
    throw new Error(`extractFnSignature: unexpected type ${(fnType as { type: string }).type}`);
}

/**
 * Create a JS wrapper around a WASM function handle.
 * When called, Beast2-encodes args, calls _east_wasm_invoke_fn, Beast2-decodes result.
 * Executes synchronously — no ASYNCIFY needed.
 */
function createFnHandleWrapper(
    handleId: number,
    fnType: EastTypeValue,
    mod: EastWasmModule,
    invokeBufs: { resultBufPtr: number; errorBufPtr: number; resultLenPtr: number; errorLenPtr: number },
): (...args: unknown[]) => unknown {
    const { inputs, output } = extractFnSignature(fnType);

    return (...jsArgs: unknown[]): unknown => {
        // Beast2-encode each arg
        const encodedArgs: Uint8Array[] = jsArgs.map((arg, i) => {
            const inputType = inputs[i];
            if (!inputType) throw new Error(`invoke_fn: no input type for arg ${i}`);
            return encodeBeast2For(inputType)(arg);
        });
        const packedArgs = encodeArgsList(encodedArgs);

        // Write args to WASM memory
        const argsPtr = mod._malloc(packedArgs.length);
        mod.HEAPU8.set(packedArgs, argsPtr);

        // Set up result/error buffers
        new DataView(mod.HEAPU8.buffer, invokeBufs.resultLenPtr, 4).setUint32(0, RESULT_BUF_SIZE, true);
        new DataView(mod.HEAPU8.buffer, invokeBufs.errorLenPtr, 4).setUint32(0, ERROR_BUF_SIZE, true);

        const rc = mod._east_wasm_invoke_fn(
            handleId, argsPtr, packedArgs.length,
            invokeBufs.resultBufPtr, invokeBufs.resultLenPtr,
            invokeBufs.errorBufPtr, invokeBufs.errorLenPtr,
        );

        mod._free(argsPtr);

        if (rc !== 0) {
            const errLen = new DataView(mod.HEAPU8.buffer, invokeBufs.errorLenPtr, 4).getUint32(0, true);
            const errBytes = new Uint8Array(mod.HEAPU8.buffer, invokeBufs.errorBufPtr, errLen);
            throw new Error(new TextDecoder().decode(errBytes));
        }

        const resLen = new DataView(mod.HEAPU8.buffer, invokeBufs.resultLenPtr, 4).getUint32(0, true);
        if (resLen === 0) return null;

        const resultBytes = new Uint8Array(mod.HEAPU8.buffer, invokeBufs.resultBufPtr, resLen).slice();
        return decodeBeast2For(output)(resultBytes);
    };
}

/**
 * Create a JS wrapper around a persistent WASM handle from compileValue().
 * Uses _east_wasm_call_with_args (persistent handles) instead of _east_wasm_invoke_fn (temp handles).
 */
function createCompiledHandleWrapper(
    handle: CompiledHandle,
    fnType: EastTypeValue,
    mod: EastWasmModule,
    bufs: { resultBufPtr: number; errorBufPtr: number; resultLenPtr: number; errorLenPtr: number },
): (...args: unknown[]) => unknown {
    const { inputs, output } = extractFnSignature(fnType);

    return (...jsArgs: unknown[]): unknown => {
        const encodedArgs: Uint8Array[] = jsArgs.map((arg, i) => {
            const inputType = inputs[i];
            if (!inputType) throw new Error(`compiled handle call: no input type for arg ${i}`);
            return encodeBeast2For(inputType)(arg);
        });
        const packedArgs = encodeArgsList(encodedArgs);

        const argsPtr = mod._malloc(packedArgs.length);
        mod.HEAPU8.set(packedArgs, argsPtr);

        new DataView(mod.HEAPU8.buffer, bufs.resultLenPtr, 4).setUint32(0, RESULT_BUF_SIZE, true);
        new DataView(mod.HEAPU8.buffer, bufs.errorLenPtr, 4).setUint32(0, ERROR_BUF_SIZE, true);

        const rc = mod._east_wasm_call_with_args(
            handle, argsPtr, packedArgs.length,
            bufs.resultBufPtr, bufs.resultLenPtr, bufs.errorBufPtr, bufs.errorLenPtr,
        );

        mod._free(argsPtr);

        if (rc !== 0) {
            const errLen = new DataView(mod.HEAPU8.buffer, bufs.errorLenPtr, 4).getUint32(0, true);
            const errBytes = new Uint8Array(mod.HEAPU8.buffer, bufs.errorBufPtr, errLen);
            throw new Error(new TextDecoder().decode(errBytes));
        }

        const resLen = new DataView(mod.HEAPU8.buffer, bufs.resultLenPtr, 4).getUint32(0, true);
        if (resLen === 0) return null;

        const resultBytes = new Uint8Array(mod.HEAPU8.buffer, bufs.resultBufPtr, resLen).slice();
        return decodeBeast2For(output)(resultBytes);
    };
}

/** Encode args list into the packed format: [count:u32le][len1:u32le][data1]... */
export function encodeArgsList(args: Uint8Array[]): Uint8Array {
    let totalLen = 4; // count header
    for (const arg of args) totalLen += 4 + arg.length;
    const buf = new Uint8Array(totalLen);
    const view = new DataView(buf.buffer);
    view.setUint32(0, args.length, true);
    let offset = 4;
    for (const arg of args) {
        view.setUint32(offset, arg.length, true);
        offset += 4;
        buf.set(arg, offset);
        offset += arg.length;
    }
    return buf;
}

/** Convert Uint8Array to hex string (for cache keys) */
export function bufToHex(buf: Uint8Array): string {
    if (buf.length === 0) return '';
    const hex: string[] = [];
    for (let i = 0; i < buf.length; i++) {
        hex.push(buf[i]!.toString(16).padStart(2, '0'));
    }
    return hex.join('');
}

// ============================================================================
// High-level platform function registration
// ============================================================================

/**
 * Register an array of East PlatformFunction[] with a WASM instance.
 *
 * Bridges between the JS PlatformFunction format (decoded values) and
 * the WASM PlatformRegistration format (Beast2-full encoded bytes).
 */
export function registerPlatformFunctions(wasm: EastWasm, platform: PlatformFunction[]): void {
    for (const reg of buildPlatformRegistrations(platform)) {
        wasm.registerPlatform(reg);
    }
}

/**
 * Handle resolver: provides module + invoke buffers for creating function handle wrappers.
 * Set by the loader (index.ts / browser.ts) after module initialization.
 */
let _handleResolver: { mod: EastWasmModule; invokeBufs: { resultBufPtr: number; errorBufPtr: number; resultLenPtr: number; errorLenPtr: number } } | null = null;

/** Set the handle resolver. Called once after module init. */
export function setHandleResolver(mod: EastWasmModule): void {
    _handleResolver = {
        mod,
        invokeBufs: {
            resultBufPtr: mod._malloc(RESULT_BUF_SIZE),
            errorBufPtr: mod._malloc(ERROR_BUF_SIZE),
            resultLenPtr: mod._malloc(4),
            errorLenPtr: mod._malloc(4),
        },
    };
}

function buildPlatformRegistrations(allPlatform: PlatformFunction[]): PlatformRegistration[] {
    return allPlatform.map(pf => {
        const isGeneric = (pf.type_parameters?.length ?? 0) > 0;
        const isAsync = pf.type === 'async';

        if (isGeneric) {
            return {
                name: pf.name,
                isGeneric: true,
                isAsync,
                factory: (typeParamsBytes: Uint8Array) => {
                    const typeParams = decodeTypeParams(typeParamsBytes);
                    const inputTypes = pf.inputsFn ? pf.inputsFn(...typeParams) : pf.inputs;
                    const outputType = pf.outputsFn ? pf.outputsFn(...typeParams) : pf.output;
                    const jsFn = pf.fn(...typeParams);
                    return (args: Uint8Array[]) => callJsPlatformFn(jsFn, inputTypes, outputType, args, allPlatform);
                },
            };
        }

        // Encode input types as Beast2-full Array(EastTypeType) for C-side bridge
        const inputTypesBytes = encodeBeast2For(ArrayType(EastTypeType))(pf.inputs);

        return {
            name: pf.name,
            isGeneric: false,
            isAsync,
            fn: (args: Uint8Array[]) => callJsPlatformFn(pf.fn, pf.inputs, pf.output, args, allPlatform),
            inputTypesBytes,
        };
    });
}

/**
 * Bridge between WASM Beast2-encoded bytes and JS platform function implementations.
 * All platform functions execute synchronously.
 *
 * When an arg's bytes start with the function handle sentinel (0xFFFFFFFF),
 * it is resolved to a JS callable wrapper instead of Beast2-decoded.
 *
 * For async platform functions: East's asyncPlatform.implement() wraps NullType-
 * returning functions in an async wrapper that returns a Promise. Since WASM
 * executes synchronously, side effects (including function handle callbacks)
 * already ran. We discard the return value for async platforms.
 */
function callJsPlatformFn(
    fn: (...args: unknown[]) => unknown,
    inputTypes: EastTypeValue[],
    outputType: EastTypeValue,
    args: Uint8Array[],
    allPlatform?: PlatformFunction[],
): Uint8Array | null {
    const decodeOptions = allPlatform ? { platform: allPlatform } : undefined;
    const decoded = args.map((argBytes, i) => {
        // Check for function handle sentinel
        if (isFnHandleArg(argBytes) && _handleResolver) {
            const { handleId, fnTypeBytes } = parseFnHandleArg(argBytes);
            const fnType = decodeBeast2For(EastTypeType)(fnTypeBytes) as EastTypeValue;
            return createFnHandleWrapper(handleId, fnType, _handleResolver.mod, _handleResolver.invokeBufs);
        }
        const decoder = decodeBeast2For(inputTypes[i]!, decodeOptions);
        return decoder(argBytes);
    });
    const result = fn(...decoded);

    // For NullType output, return null immediately. This handles async platform
    // functions whose implement() wraps in async and returns a Promise — the side
    // effects already ran synchronously, so we discard the return value.
    // Silence any rejection on the dangling Promise to prevent unhandled rejection errors.
    if (outputType.type === 'Null') {
        if (result != null && typeof (result as any).catch === 'function') {
            (result as Promise<unknown>).catch(() => {});
        }
        return null;
    }
    if (result === null || result === undefined) return null;
    const encoder = encodeBeast2For(outputType);
    return encoder(result);
}

/**
 * Decode Beast2-full encoded type params array into EastTypeValue[].
 */
function decodeTypeParams(typeParamsBytes: Uint8Array): EastTypeValue[] {
    if (typeParamsBytes.length === 0) return [];
    const decoded = decodeBeast2For(
        ArrayType(EastTypeType)
    )(typeParamsBytes) as EastTypeValue[];
    return decoded;
}
