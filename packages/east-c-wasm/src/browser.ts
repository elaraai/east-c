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

    // Load the Emscripten glue JS. It's typically CJS, so we fetch it as text
    // and evaluate via a blob URL to avoid issues with bundlers and CJS/ESM mismatch.
    const glueResponse = await fetch(glueUrl);
    const glueText = await glueResponse.text();
    const blobUrl = URL.createObjectURL(
        new Blob(
            [`${glueText}\nexport default Module;`],
            { type: 'application/javascript' }
        )
    );

    let createModule: (opts?: Record<string, unknown>) => Promise<EastWasmModule>;
    try {
        const glueModule = await import(/* @vite-ignore */ blobUrl);
        createModule = glueModule.default;
    } finally {
        URL.revokeObjectURL(blobUrl);
    }

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
