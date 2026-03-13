# east-c-wasm Bridge Design

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  TypeScript (Node.js or Browser)                        │
│                                                         │
│  1. JSON IR → Beast2-full encode → wasm.compile()       │
│  2. wasm.call(handle) → Beast2-full result → decode     │
│                                                         │
│  Platform Bridge (js_platform_call):                    │
│    ┌─────────────────────────────────────────┐          │
│    │ Receives packed args from C             │          │
│    │ For each arg:                           │          │
│    │   • Value arg → Beast2 decode → JS val  │          │
│    │   • Function arg → handle → JS wrapper  │          │
│    │ Calls JS platform implementation        │          │
│    │ Beast2-encodes result → writes to WASM  │          │
│    └─────────────────────────────────────────┘          │
└────────────────────┬────────────────────────────────────┘
                     │ js_platform_call (EM_JS + ASYNCIFY)
┌────────────────────▼────────────────────────────────────┐
│  C / WebAssembly (east-c interpreter)                   │
│                                                         │
│  - Beast2-full decode → IR value → IRNode tree          │
│  - Compile IRNode → EastCompiledFn                      │
│  - Tree-walking eval (same interpreter as native)       │
│  - IR_PLATFORM node → pack args → js_platform_call      │
│  - Function handle table for callback args              │
└─────────────────────────────────────────────────────────┘
```

## The Function Value Problem

### How native runtimes handle function args

In both the **TS runtime** and **native C runtime**, function values received as platform args are just pointers — no serialization:

**TS (east-node-std/src/test.ts):**
```typescript
test.implement(async (name: string, body: () => Promise<null>) => {
    await body();  // body is a plain JS callable
});
```

**C (east-c-std/tests/test_compliance.c):**
```c
EastCompiledFn *body = args[1]->data.function.compiled;
EvalResult r = east_call(body, NULL, 0);  // body is a C pointer
```

Neither runtime serializes the function. The function value stays in-process.

### What the WASM bridge currently does wrong

The WASM bridge tries to Beast2-encode the function value (full IR + captures), send the bytes to JS, then calls `decodeBeast2For(FunctionType(...))` which invokes `compile_internal` — the TS tree-walking compiler — to reconstruct a JS-callable function from the IR.

This fails because:
1. The IR was compiled by east-c, not the TS compiler
2. Variable IDs and capture contexts are C-specific
3. `compile_internal` expects TS-formatted IR

**The function is already compiled in WASM. There is no reason to recompile it in JS.**

### The TS Beast2 decoder's `compileFunctionOverride`

The TS `decodeBeast2For` already has an escape hatch:

```typescript
type Beast2DecodeOptions = {
    platform?: PlatformFunction[];
    compileFunctionOverride?: (
        ir: FunctionIR,
        captureContext: RuntimeContext,
        platform: PlatformFunction[]
    ) => ((...args: unknown[]) => unknown) | null;
};
```

This doesn't help us because:
- C would still Beast2-encode the full IR + captures (expensive, ~megabytes for complex functions)
- JS would decode the IR only to throw it away and create a handle wrapper
- The override API assumes decoded IR is meaningful — but C-compiled IR isn't valid for TS

---

## Solution: Function Handles

Keep function values in WASM. Pass opaque handle IDs to JS. JS wraps handles in callables that invoke back into WASM.

### Packed Args Protocol

Current format: `[count:u32le] [len1:u32le][data1] [len2:u32le][data2] ...`

Extended with a sentinel length to distinguish value args from function handles:

| Arg type | Encoding |
|----------|----------|
| Value | `[len:u32le][beast2_bytes...]` where `len < 0xFFFFFFFF` |
| Function handle | `[0xFFFFFFFF:u32le][handle_id:u32le][input_count:u32le][fn_type_beast2...]` |

The sentinel `0xFFFFFFFF` (4GB) can never occur as a valid Beast2 payload length.

After the handle ID, the function's type descriptor is Beast2-full encoded so JS knows the input/output types for the wrapper. This uses the same `east_beast2_encode_full` that already works for type values — encoding the FunctionType as a Beast2 type value.

### C Side Changes (wasm_api.c)

#### 1. Temporary handle table for borrowed function values

```c
#define MAX_TEMP_HANDLES 64
static struct {
    EastValue *fn_values[MAX_TEMP_HANDLES];
    size_t count;
} g_temp_handles;

static uint32_t alloc_temp_handle(EastValue *fn_val) {
    if (g_temp_handles.count >= MAX_TEMP_HANDLES) return 0;
    uint32_t id = 0x80000000 | (uint32_t)g_temp_handles.count;  // high bit = temp
    east_value_retain(fn_val);
    g_temp_handles.fn_values[g_temp_handles.count++] = fn_val;
    return id;
}

static void free_temp_handles(void) {
    for (size_t i = 0; i < g_temp_handles.count; i++) {
        east_value_release(g_temp_handles.fn_values[i]);
        g_temp_handles.fn_values[i] = NULL;
    }
    g_temp_handles.count = 0;
}
```

Temp handles use the high bit (`0x80000000`) to distinguish from compiled function handles. This avoids collision with the existing `g_handles` table used for top-level compiled functions.

#### 2. Modified `platform_bridge_fn` — function arg detection

In the arg encoding loop, when the declared input type is `EAST_TYPE_FUNCTION` or `EAST_TYPE_ASYNC_FUNCTION` and the value is `EAST_VAL_FUNCTION`:

```c
if (declared && i < declared->num_inputs &&
    (declared->input_types[i]->kind == EAST_TYPE_FUNCTION ||
     declared->input_types[i]->kind == EAST_TYPE_ASYNC_FUNCTION) &&
    v->kind == EAST_VAL_FUNCTION) {

    /* Write sentinel length */
    uint32_t sentinel = 0xFFFFFFFF;
    byte_buffer_write_bytes(args_buf, (uint8_t *)&sentinel, 4);

    /* Write handle ID */
    uint32_t handle_id = alloc_temp_handle(v);
    byte_buffer_write_bytes(args_buf, (uint8_t *)&handle_id, 4);

    /* Write input count */
    uint32_t input_count = (uint32_t)declared->input_types[i]->data.function.num_inputs;
    byte_buffer_write_bytes(args_buf, (uint8_t *)&input_count, 4);

    /* Write function type as Beast2-full encoded type value */
    EastValue *type_val = east_type_to_value(declared->input_types[i]);
    EastType *type_type = east_type_type;
    ByteBuffer *tbuf = east_beast2_encode_full(type_val, type_type);
    uint32_t tlen = (uint32_t)tbuf->len;
    byte_buffer_write_bytes(args_buf, (uint8_t *)&tlen, 4);
    byte_buffer_write_bytes(args_buf, tbuf->data, tbuf->len);
    byte_buffer_free(tbuf);
    east_value_release(type_val);

    continue;  /* skip normal Beast2 encoding */
}
```

After `js_platform_call` returns, call `free_temp_handles()`.

#### 3. New export: `east_wasm_invoke_fn`

For JS to call a function handle back into WASM:

```c
EMSCRIPTEN_KEEPALIVE
int east_wasm_invoke_fn(uint32_t handle_id,
                        const uint8_t *args_buf, size_t args_len,
                        uint8_t *result_buf, size_t *result_len,
                        char *error_buf, size_t *error_len)
```

This differs from `east_wasm_call_with_args` in that:
- It resolves temp handles (`0x80000000 | idx`) to the `EastValue*` in `g_temp_handles`
- It extracts `fn_val->data.function.compiled` to get the `EastCompiledFn*`
- It uses the function's IR return type (`fn->ir->type`) for result encoding
- For input decoding: uses `east_beast2_decode_auto` (Beast2-full args are self-describing)

```c
{
    EastValue *fn_val = NULL;
    if (handle_id & 0x80000000) {
        uint32_t idx = handle_id & 0x7FFFFFFF;
        if (idx < g_temp_handles.count)
            fn_val = g_temp_handles.fn_values[idx];
    }
    if (!fn_val || fn_val->kind != EAST_VAL_FUNCTION) {
        /* write error ... */
        return 1;
    }

    EastCompiledFn *fn = fn_val->data.function.compiled;
    /* decode args, east_call(fn, args, num_args), encode result */
    /* Same pattern as east_wasm_call_with_args */
}
```

### JS Side Changes (common.ts)

#### 1. EastWasmModule — add the new export

```typescript
export interface EastWasmModule extends EmscriptenModule {
    // ... existing exports ...
    _east_wasm_invoke_fn: (handle: number, argsPtr: number, argsLen: number,
                           resultPtr: number, resultLenPtr: number,
                           errorPtr: number, errorLenPtr: number) => number;
}
```

#### 2. Modified `callJsPlatformFn` — detect function handles

Replace the current arg decoding loop:

```typescript
function callJsPlatformFn(
    fn: (...args: unknown[]) => unknown,
    inputTypes: EastTypeValue[],
    outputType: EastTypeValue,
    args: Uint8Array[],               // ← currently Beast2 blobs per arg
    wasm: EastWasmModule,             // ← NEW: needed for function handle invocation
    isAsync?: boolean,
): Uint8Array | null | Promise<Uint8Array | null> {
    const decoded = args.map((argBytes, i) => {
        const inputType = inputTypes[i]!;

        // Check for function handle sentinel
        if (isFunctionHandle(argBytes)) {
            return createFnWrapper(argBytes, inputType, wasm);
        }

        // Normal Beast2 decode
        return decodeBeast2For(inputType)(argBytes);
    });

    // ... rest unchanged ...
}
```

#### 3. Function handle detection and wrapper creation

```typescript
/** Sentinel length value indicating a function handle */
const FN_HANDLE_SENTINEL = 0xFFFFFFFF;

function isFunctionHandle(argBytes: Uint8Array): boolean {
    // The sentinel is encoded as the length prefix in decodeArgsList.
    // But since decodeArgsList already strips the length prefix,
    // we need to detect handles at a different layer.
    // See "Modified decodeArgsList" below.
    return false; // placeholder — detection happens in decodeArgsList
}
```

#### 4. Modified `decodeArgsList` — handle-aware

The sentinel must be detected in `decodeArgsList` before the arg bytes are split:

```typescript
export type DecodedArg =
    | { kind: 'value'; data: Uint8Array }
    | { kind: 'function'; handleId: number; inputCount: number; fnTypeBytes: Uint8Array };

export function decodeArgsListV2(data: Uint8Array): DecodedArg[] {
    if (data.length < 4) return [];
    const view = new DataView(data.buffer, data.byteOffset, data.byteLength);
    const count = view.getUint32(0, true);
    const args: DecodedArg[] = [];
    let offset = 4;

    for (let i = 0; i < count; i++) {
        if (offset + 4 > data.length) break;
        const len = view.getUint32(offset, true);
        offset += 4;

        if (len === FN_HANDLE_SENTINEL) {
            // Function handle: [handle_id:u32][input_count:u32][type_len:u32][type_bytes...]
            const handleId = view.getUint32(offset, true); offset += 4;
            const inputCount = view.getUint32(offset, true); offset += 4;
            const typeLen = view.getUint32(offset, true); offset += 4;
            const fnTypeBytes = data.slice(offset, offset + typeLen); offset += typeLen;
            args.push({ kind: 'function', handleId, inputCount, fnTypeBytes });
        } else {
            // Normal value
            if (offset + len > data.length) break;
            args.push({ kind: 'value', data: data.slice(offset, offset + len) });
            offset += len;
        }
    }
    return args;
}
```

#### 5. JS wrapper factory

```typescript
function createFnHandleWrapper(
    handleId: number,
    fnType: EastTypeValue,       // decoded from fnTypeBytes
    mod: EastWasmModule,
): (...args: unknown[]) => unknown {
    // Extract input/output types from the FunctionType or AsyncFunctionType
    const { inputs, output, isAsync } = extractFnSignature(fnType);

    const invoke = (...jsArgs: unknown[]): unknown => {
        // Beast2-encode each JS arg using the declared input types
        const encodedArgs = jsArgs.map((arg, i) => encodeBeast2For(inputs[i]!)(arg));
        const packedArgs = encodeArgsList(encodedArgs);

        // Call into WASM
        const argsPtr = writeBytes(mod, packedArgs);
        // ... allocate result/error buffers, call mod._east_wasm_invoke_fn ...
        // ... decode result via decodeBeast2For(output) ...
        // ... cleanup ...
    };

    if (isAsync) {
        return async (...args: unknown[]) => invoke(...args);
    }
    return invoke;
}
```

#### 6. Updated `createPlatformBridge`

The bridge needs access to the module for function handle invocation:

```typescript
export function createPlatformBridge(
    platformFns: Map<string, PlatformRegistration>,
    genericCache: Map<string, PlatformFn>,
    getMod: () => EastWasmModule,
): BridgeFn {
    return (namePtr, tpPtr, tpLen, argsPtr, argsLen, outPtr, outLenPtr) => {
        const mod = getMod();
        const name = mod.UTF8ToString(namePtr);
        const reg = platformFns.get(name);
        // ...

        const argsData = new Uint8Array(mod.HEAPU8.buffer, argsPtr, argsLen).slice();
        const decodedArgs = decodeArgsListV2(argsData);

        // Resolve each arg
        const jsArgs = decodedArgs.map((arg, i) => {
            if (arg.kind === 'function') {
                const fnType = decodeBeast2For(EastTypeType)(arg.fnTypeBytes) as EastTypeValue;
                return createFnHandleWrapper(arg.handleId, fnType, mod);
            }
            const inputType = /* get from reg */ inputTypes[i]!;
            return decodeBeast2For(inputType)(arg.data);
        });

        // Call the JS implementation with resolved args
        const result = reg.fn!(...jsArgs);  // ← no longer goes through callJsPlatformFn
        // ...
    };
}
```

### Handle Lifecycle

```
C: platform_bridge_fn called
  │
  ├─ For each function arg: alloc_temp_handle(fn_val)  ← retains EastValue
  │     Writes sentinel + handle_id to args buffer
  │
  ├─ js_platform_call(args_buf)
  │     │
  │     ├─ JS: decodeArgsListV2 → detects function handles
  │     ├─ JS: createFnHandleWrapper(handleId, fnType, mod)
  │     ├─ JS: calls platform impl (e.g., test body())
  │     │     │
  │     │     └─ wrapper called → mod._east_wasm_invoke_fn(handleId, args, ...)
  │     │           │
  │     │           └─ C: resolve temp handle → east_call(fn, args, n)
  │     │                 (function executes in WASM, may trigger nested
  │     │                  platform calls → re-entrant js_platform_call)
  │     │
  │     └─ returns result to C
  │
  └─ free_temp_handles()  ← releases all retained EastValues
```

**Key properties:**
- Handles are **borrowed** — valid only during the platform call that created them
- `alloc_temp_handle` retains the EastValue; `free_temp_handles` releases them
- WASM is single-threaded, so the global `g_temp_handles` is safe
- Re-entrancy (nested platform calls) works because ASYNCIFY saves/restores the C stack, and each `platform_bridge_fn` invocation appends to `g_temp_handles` (handles are only freed at the outermost call's cleanup)

**Re-entrancy detail:** If a callback triggers a nested platform call that also has function args, those get appended to the same `g_temp_handles` array. This is fine — `free_temp_handles` must only be called at the outermost `platform_bridge_fn` return. Use a depth counter:

```c
static int g_bridge_depth = 0;

static EvalResult platform_bridge_fn(EastValue **args, size_t num_args) {
    g_bridge_depth++;
    // ... encode args, call JS, decode result ...
    g_bridge_depth--;
    if (g_bridge_depth == 0) free_temp_handles();
    return result;
}
```

### What Changes for `callJsPlatformFn`

The current `callJsPlatformFn` function has this signature:

```typescript
function callJsPlatformFn(
    fn: (...args: unknown[]) => unknown,
    inputTypes: EastTypeValue[],
    outputType: EastTypeValue,
    args: Uint8Array[],
    allPlatform?: PlatformFunction[],
    isAsync?: boolean,
): Uint8Array | null | Promise<Uint8Array | null>;
```

This function currently:
1. Beast2-decodes each arg using `decodeBeast2For(inputTypes[i], { platform: allPlatform })`
2. Calls `fn(...decoded)`
3. Beast2-encodes the result

The `{ platform: allPlatform }` decode option exists because `decodeBeast2For` for FunctionType needs the platform registry to call `compile_internal`. **This is exactly what breaks.**

With function handles, `callJsPlatformFn` no longer needs to decode function args from Beast2 at all — they come as pre-wrapped JS callables from `createFnHandleWrapper`. The `allPlatform` parameter and `Beast2DecodeOptions.platform` become unnecessary for the WASM bridge path.

**Recommendation:** Move arg decoding into the bridge itself (in `createPlatformBridge`), where handle detection naturally lives. `callJsPlatformFn` can be simplified or removed — the bridge directly decodes value args and wraps function args, then calls the JS implementation.

### Changes to TS `east` package

**Remove `compileFunctionOverride` from `Beast2DecodeOptions`** in `east/src/serialization/beast2.ts`. This hook was added specifically for the WASM bridge but is superseded by the sentinel/handle approach — which bypasses Beast2 function decoding entirely. No other consumer uses it. Remove the option type, the conditional in the decode function path, and any tests for it.

### What Does NOT Change

- **`Beast2DecodeOptions.platform`** — Stays. The TS runtime's own Beast2 function decoder still needs the platform registry to call `compile_internal` for TS-native function deserialization (e.g., `parallel.ts` worker threads).
- **`PlatformFunction` interface** — The `{ name, inputs, output, type, fn }` interface is correct and matches how east-node-std defines platform functions.
- **`registerPlatformFunctions`** — The `buildPlatformRegistrations` mapping from `PlatformFunction[]` to `PlatformRegistration[]` is correct.
- **`EastWasm` public API** — `compile()`, `call()`, `callWithArgs()`, `free()` remain unchanged.
- **Beast2 encoding for non-function values** — All value types continue to use Beast2-full encoding across the bridge.
- **ASYNCIFY** — Same flags, same mechanism. Function handle invocations from JS into WASM may trigger async platform calls (nested re-entry), which ASYNCIFY handles.
- **Input type registration** — `inputTypesBytes` at registration time still needed. The C side uses declared input types to detect function-typed args and switch to handle encoding.

---

## Implementation Checklist

### C side (wasm_api.c)

- [ ] Add temp handle table (`g_temp_handles`) with retain/release
- [ ] Add bridge depth counter for re-entrant cleanup
- [ ] Modify `platform_bridge_fn` arg encoding loop: detect function-typed args via declared input types, write sentinel + handle + type
- [ ] Add `free_temp_handles()` call at bridge depth 0
- [ ] Add `east_wasm_invoke_fn` export
- [ ] Add `_east_wasm_invoke_fn` to `EastWasmModule` interface in common.ts

### JS side (common.ts)

- [ ] Add `DecodedArg` type and `decodeArgsListV2` with sentinel detection
- [ ] Add `createFnHandleWrapper` factory
- [ ] Add `extractFnSignature` helper to pull inputs/output from FunctionType/AsyncFunctionType
- [ ] Modify `createPlatformBridge` to use `decodeArgsListV2` and handle function args inline
- [ ] Remove `allPlatform` / `Beast2DecodeOptions.platform` from the bridge path (keep in `callJsPlatformFn` for non-WASM use)
- [ ] Pre-allocate shared result/error buffers for `east_wasm_invoke_fn` calls (like `resultBufPtr`/`errorBufPtr` in `createEastWasmFromModule`)

### TS `east` package (../east)

- [ ] Remove `compileFunctionOverride` from `Beast2DecodeOptions` in `src/serialization/beast2.ts`
- [ ] Remove the override conditional in the Beast2 function decode path
- [ ] Remove any tests for `compileFunctionOverride`

### Tests

- [ ] `wasm.spec.ts`: Add test for function handle round-trip (register platform fn that receives and calls a function arg)
- [ ] `compliance.spec.ts`: Should now pass for all suites including function-arg ones (Function, Recursive, etc.)

---

## Appendix: Platform Functions That Receive Function Args

| Platform Function | Input Types | Source |
|-------------------|-------------|--------|
| `test` | `[String, AsyncFunction([], Null)]` | east-node-std/test.ts |
| `describe` | `[String, AsyncFunction([], Null)]` | east-node-std/test.ts |
| `parallel_map` | `[Array(T), Function([T], R)]` | east-node-std/parallel.ts (generic) |

The `test` and `describe` cases are simple — the body takes no args and returns null. `parallel_map` is more complex — the callback takes a typed arg and returns a typed result, requiring proper Beast2 encode/decode through the wrapper. All three cases are handled by the design above.
