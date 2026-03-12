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

// Type for the Emscripten-generated module
export interface EastWasmModule {
    ccall: (name: string, returnType: string | null, argTypes: string[], args: unknown[]) => unknown;
    cwrap: (name: string, returnType: string | null, argTypes: string[]) => (...args: unknown[]) => unknown;
    HEAPU8: Uint8Array;
    HEAP32: Int32Array;
    HEAPU32: Uint32Array;
    UTF8ToString: (ptr: number) => string;
    stringToUTF8: (str: string, ptr: number, maxBytes: number) => void;
    lengthBytesUTF8: (str: string) => number;
    _malloc: (size: number) => number;
    _free: (ptr: number) => void;
    _east_wasm_init: () => void;
    _east_wasm_register_platform: (namePtr: number, isGeneric: number, isAsync: number) => void;
    _east_wasm_compile: (irPtr: number, irLen: number) => number;
    _east_wasm_call: (handle: number, resultPtr: number, resultLenPtr: number, errorPtr: number, errorLenPtr: number) => number;
    _east_wasm_call_with_args: (handle: number, argsPtr: number, argsLen: number, resultPtr: number, resultLenPtr: number, errorPtr: number, errorLenPtr: number) => number;
    _east_wasm_free: (handle: number) => void;
    _east_wasm_gc: () => void;
    _east_wasm_malloc: (size: number) => number;
    _east_wasm_free_buf: (ptr: number) => void;
    _east_wasm_set_platform_context: (namePtr: number, tpPtr: number, tpLen: number) => void;
    _east_wasm_last_error: () => number; // returns pointer to null-terminated string, or 0
}

/** Result buffer size (1MB) — matches C side */
const RESULT_BUF_SIZE = 1024 * 1024;

/** Error buffer size (64KB) */
const ERROR_BUF_SIZE = 64 * 1024;

/**
 * Platform function implementation.
 * Receives Beast2-full encoded args, returns Beast2-full encoded result (or null).
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
}

/**
 * Compiled function handle. Opaque — use call() and free().
 */
export type CompiledHandle = number & { __brand: 'CompiledHandle' };

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
            mod._east_wasm_register_platform(namePtr, reg.isGeneric ? 1 : 0, reg.isAsync ? 1 : 0);
            mod._free(namePtr);
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

            const rc = mod._east_wasm_call(
                handle,
                resultBufPtr, resultLenPtr,
                errorBufPtr, errorLenPtr,
            );

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

            const rc = mod._east_wasm_call_with_args(
                handle,
                argsPtr, argsBytes.length,
                resultBufPtr, resultLenPtr,
                errorBufPtr, errorLenPtr,
            );

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
    };
}

/**
 * Build the platform call bridge for Emscripten module options.
 * Must be set as `moduleOpts.js_platform_call` before module initialization.
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
            const argsData = new Uint8Array(mod.HEAPU8.buffer, argsPtr, argsLen);
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

            // Call the implementation
            const result = impl(args);

            // Write result back to WASM memory
            if (result && result.length > 0) {
                const outLenView = new DataView(mod.HEAPU8.buffer, outLenPtr, 4);
                const capacity = outLenView.getUint32(0, true);
                if (result.length > capacity) {
                    writeErrorToWasmBridge(mod, 'platform result too large', outPtr, outLenPtr);
                    return 1;
                }
                new Uint8Array(mod.HEAPU8.buffer, outPtr, result.length).set(result);
                outLenView.setUint32(0, result.length, true);
            } else {
                new DataView(mod.HEAPU8.buffer, outLenPtr, 4).setUint32(0, 0, true);
            }

            return 0;
        } catch (e) {
            const msg = e instanceof Error ? e.message : String(e);
            writeErrorToWasmBridge(mod, msg, outPtr, outLenPtr);
            return 1;
        }
    };
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

/** Decode args list from the packed format: [count:u32le][len1:u32le][data1]... */
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
        if (offset + len > data.length) break;
        args.push(data.slice(offset, offset + len));
        offset += len;
    }
    return args;
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
