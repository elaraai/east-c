/**
 * Copyright (c) 2025 Elara AI Pty Ltd
 * Dual-licensed under AGPL-3.0 and commercial license. See LICENSE for details.
 */

/**
 * @elaraai/east-c-wasm
 *
 * Generic WASM backend for executing East IR. No UI concepts —
 * just compile(irBytes) → handle, call(handle) → resultBytes, free(handle).
 *
 * Platform functions are registered from JS and called back via imports.
 *
 * This is the Node.js entry point. For browser usage, import from
 * `@elaraai/east-c-wasm/browser`.
 */

import { fileURLToPath } from 'node:url';
import { dirname, join } from 'node:path';

import {
    type EastWasmModule,
    type EastWasm,
    type EastWasmOptions,
    type PlatformFn,
    type PlatformRegistration,
    createEastWasmFromModule,
    createPlatformBridge,
} from './common.js';

// Re-export all types from common
export type { PlatformFn, GenericPlatformFactory, PlatformRegistration, CompiledHandle, EastWasm, EastWasmOptions, EastWasmModule } from './common.js';
export { createEastWasmFromModule, registerPlatformFunctions } from './common.js';

/**
 * Load and initialize the WASM module (Node.js).
 */
export async function createEastWasm(options?: EastWasmOptions): Promise<EastWasm> {
    // Resolve paths to WASM artifacts
    const __filename = fileURLToPath(import.meta.url);
    const __dirname = dirname(__filename);
    const wasmDir = join(__dirname, '..', 'wasm');

    // Load the Emscripten glue file (ESM with EXPORT_ES6=1)
    const gluePath = options?.glueUrl ?? join(wasmDir, 'east-c.js');
    const glueModule = await import(gluePath);
    const createModule = glueModule.default as (opts?: Record<string, unknown>) => Promise<EastWasmModule>;

    const wasmPath = options?.wasmUrl ?? join(wasmDir, 'east-c.wasm');

    // Platform function registry (JS side)
    const platformFns = new Map<string, PlatformRegistration>();
    const genericCache = new Map<string, PlatformFn>();

    // Declare mod here so the bridge closure can reference it
    let mod: EastWasmModule;

    const bridge = createPlatformBridge(platformFns, genericCache, () => mod);

    const moduleOpts: Record<string, unknown> = {
        locateFile(path: string) {
            if (path.endsWith('.wasm')) return wasmPath;
            return path;
        },
        js_platform_call: bridge,
    };

    mod = await createModule(moduleOpts);

    // Pass shared platform maps so bridge and API use the same registry
    return createEastWasmFromModule(mod, { platformFns, genericCache });
}

export default createEastWasm;
