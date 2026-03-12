/**
 * Copyright (c) 2025 Elara AI Pty Ltd
 * Dual-licensed under AGPL-3.0 and commercial license. See LICENSE for details.
 */

/**
 * @elaraai/east-c-wasm/browser
 *
 * Browser-compatible loader for east-c-wasm. Uses fetch + dynamic import
 * instead of Node.js createRequire/path APIs.
 */

import {
    type EastWasmModule,
    type EastWasm,
    type EastWasmOptions,
    type PlatformFn,
    type PlatformRegistration,
    createEastWasmFromModule,
    createPlatformBridge,
} from './common.js';

export type { EastWasm, EastWasmOptions, PlatformFn, PlatformRegistration };
export type { CompiledHandle, GenericPlatformFactory, EastWasmModule } from './common.js';
export { registerPlatformFunctions } from './common.js';

/**
 * Load and initialize the WASM module in a browser environment.
 *
 * @param options - Must provide `wasmUrl`. Optionally provide `glueUrl` for the
 *   Emscripten JS glue file. If `glueUrl` is not provided, it is derived by
 *   replacing `.wasm` with `.js` in `wasmUrl`.
 */
export async function createEastWasmBrowser(options: EastWasmOptions & { wasmUrl: string }): Promise<EastWasm> {
    const wasmUrl = options.wasmUrl;
    const glueUrl = options.glueUrl ?? wasmUrl.replace(/\.wasm$/, '.js');

    // Platform function registry (JS side) — shared with the bridge
    const platformFns = new Map<string, PlatformRegistration>();
    const genericCache = new Map<string, PlatformFn>();

    // Declare mod here so the bridge closure can reference it
    let mod: EastWasmModule;

    const bridge = createPlatformBridge(platformFns, genericCache, () => mod);

    // The Emscripten glue is built with EXPORT_ES6=1, so it's a native ESM
    // with `export default createEastWasmModule`. Dynamic import works directly.
    const glueModule = await import(/* @vite-ignore */ glueUrl);
    const createModule: (opts?: Record<string, unknown>) => Promise<EastWasmModule> = glueModule.default;

    const moduleOpts: Record<string, unknown> = {
        locateFile(path: string) {
            if (path.endsWith('.wasm')) return wasmUrl;
            return path;
        },
        js_platform_call: bridge,
    };

    mod = await createModule(moduleOpts);

    // Pass the shared platform maps so the bridge and EastWasm API use the same registry
    return createEastWasmFromModule(mod, { platformFns, genericCache });
}

export default createEastWasmBrowser;
