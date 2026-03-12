/*
 * WASM API for east-c.
 *
 * Provides a minimal, generic API for compiling and executing East IR
 * from JavaScript via WebAssembly. Platform functions are implemented
 * as JS callbacks that the WASM module calls via imports.
 *
 * The API is deliberately UI-agnostic — it's just "execute East IR fast".
 */

#include <east/east.h>
#include <east/eval_result.h>
#include <east/type_of_type.h>
#include <east/gc.h>

#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Global state                                                       */
/* ------------------------------------------------------------------ */

static PlatformRegistry *g_platform = NULL;
static BuiltinRegistry *g_builtins = NULL;
static int g_initialized = 0;

/* ------------------------------------------------------------------ */
/*  JS-imported platform function bridge                               */
/* ------------------------------------------------------------------ */

/*
 * JS provides platform function implementations via these imports.
 *
 * Protocol for generic platform functions:
 *   1. JS calls east_wasm_compile() with Beast2-full IR bytes
 *   2. When execution hits an IR_PLATFORM node, C calls js_platform_call()
 *   3. JS receives: platform function name, type params as Beast2 type values,
 *      and args as Beast2-encoded values
 *   4. JS executes the platform function and writes the Beast2-encoded result
 *      back into WASM memory
 *   5. C decodes the result and continues execution
 *
 * This avoids needing to register each platform function individually in C.
 * All platform dispatch happens through a single bridge function.
 */

/*
 * JS platform call bridge.
 *
 * Implemented as EM_JS so Emscripten links it as a JS import.
 * The actual implementation is overridden at module instantiation time
 * via moduleOpts.js_platform_call from the TypeScript wrapper.
 *
 * This stub calls into Module.js_platform_call which the TS wrapper sets up.
 */
EM_JS(int, js_platform_call, (
    const char *name,
    const uint8_t *type_params_buf, size_t type_params_len,
    const uint8_t *args_buf, size_t args_len,
    uint8_t *out_buf, size_t *out_len
), {
    if (Module.js_platform_call) {
        return Module.js_platform_call(name, type_params_buf, type_params_len,
                                        args_buf, args_len, out_buf, out_len);
    }
    /* No handler registered — write error */
    return 1;
});

/* Max result buffer size for platform calls (1MB - should be plenty) */
#define PLATFORM_RESULT_BUF_SIZE (1024 * 1024)

/* Shared result buffer to avoid repeated malloc/free */
static uint8_t *g_platform_result_buf = NULL;

/* ------------------------------------------------------------------ */
/*  Generic platform function factory                                  */
/* ------------------------------------------------------------------ */

/*
 * We use a single GenericPlatformFactory that handles ALL platform functions.
 * The factory creates a closure that captures the function name and type params,
 * then delegates to JS via js_platform_call().
 *
 * Since C function pointers can't capture state, we use a small registry
 * of trampolines keyed by (name, type_params) hash.
 */

/* Trampoline entry: captures name + type params for a specific instantiation */
typedef struct PlatformTrampoline {
    char *name;
    EastType **type_params;
    size_t num_type_params;
    /* Beast2-encoded type params (cached for fast JS calls) */
    uint8_t *type_params_encoded;
    size_t type_params_encoded_len;
    struct PlatformTrampoline *next;
} PlatformTrampoline;

#define TRAMPOLINE_BUCKETS 256
static PlatformTrampoline *g_trampolines[TRAMPOLINE_BUCKETS];
/* Currently executing trampoline (set before call, used by the PlatformFn) */
static __thread PlatformTrampoline *g_current_trampoline = NULL;

static uint32_t trampoline_hash(const char *name, EastType **tp, size_t ntp) {
    uint32_t h = 5381;
    for (const char *c = name; *c; c++) h = h * 33 + (uint32_t)*c;
    h ^= (uint32_t)ntp;
    /* Mix in type pointers for uniqueness */
    for (size_t i = 0; i < ntp; i++) h = h * 33 + (uint32_t)(uintptr_t)tp[i];
    return h;
}

/* Encode type params as Beast2-full array of type values */
static void encode_type_params(PlatformTrampoline *t) {
    if (t->num_type_params == 0) {
        t->type_params_encoded = NULL;
        t->type_params_encoded_len = 0;
        return;
    }

    /* Build an array of type values */
    EastValue *arr = east_array_new(east_type_type);
    for (size_t i = 0; i < t->num_type_params; i++) {
        EastValue *tv = east_type_to_value(t->type_params[i]);
        east_array_push(arr, tv);
        east_value_release(tv);
    }

    EastType *arr_type = east_array_type(east_type_type);
    ByteBuffer *buf = east_beast2_encode_full(arr, arr_type);
    east_type_release(arr_type);
    east_value_release(arr);

    t->type_params_encoded = malloc(buf->len);
    memcpy(t->type_params_encoded, buf->data, buf->len);
    t->type_params_encoded_len = buf->len;
    byte_buffer_free(buf);
}

/* The actual PlatformFn that all trampolines share */
static EvalResult platform_bridge_fn(EastValue **args, size_t num_args) {
    PlatformTrampoline *t = g_current_trampoline;
    if (!t) return eval_error("platform bridge: no active trampoline");

    /* Encode args as Beast2-full array */
    /* We need the types from the IR node, but we don't have them here.
     * Instead, encode as a Beast2-full value which includes the type header.
     * We'll encode each arg individually and pack them into a simple format:
     * [count][len1][beast2_full_1][len2][beast2_full_2]...
     */
    ByteBuffer *args_buf = byte_buffer_new(1024);

    /* Write arg count as 4-byte LE */
    uint32_t count = (uint32_t)num_args;
    byte_buffer_write_bytes(args_buf, (uint8_t *)&count, 4);

    for (size_t i = 0; i < num_args; i++) {
        /* For each arg, we need its type. We can get it from the value kind,
         * but for full fidelity we need the IR type. Since platform functions
         * receive already-evaluated EastValues, we'll use beast2_encode_full
         * which embeds the type. But we need the type...
         *
         * The solution: use the trampoline's type_params to reconstruct
         * the argument types. For generic platform functions, type_params
         * describe the parameterized types. The actual arg types depend on
         * the platform function signature, which JS knows.
         *
         * Simpler approach: encode each value with its concrete type from
         * the EastValue itself. We'll add a helper for this.
         */
        EastType *arg_type = NULL;
        EastValue *v = args[i];

        /* Infer type from value */
        switch (v->kind) {
            case EAST_VAL_NULL:     arg_type = &east_null_type; break;
            case EAST_VAL_BOOLEAN:  arg_type = &east_boolean_type; break;
            case EAST_VAL_INTEGER:  arg_type = &east_integer_type; break;
            case EAST_VAL_FLOAT:    arg_type = &east_float_type; break;
            case EAST_VAL_STRING:   arg_type = &east_string_type; break;
            case EAST_VAL_DATETIME: arg_type = &east_datetime_type; break;
            case EAST_VAL_BLOB:     arg_type = &east_blob_type; break;
            case EAST_VAL_ARRAY:    { east_type_retain(v->data.array.elem_type);
                                      arg_type = east_array_type(v->data.array.elem_type); break; }
            case EAST_VAL_SET:      { east_type_retain(v->data.set.elem_type);
                                      arg_type = east_set_type(v->data.set.elem_type); break; }
            case EAST_VAL_DICT:     { east_type_retain(v->data.dict.key_type);
                                      east_type_retain(v->data.dict.val_type);
                                      arg_type = east_dict_type(v->data.dict.key_type, v->data.dict.val_type); break; }
            case EAST_VAL_STRUCT:   { east_type_retain(v->data.struct_.type);
                                      arg_type = v->data.struct_.type; break; }
            case EAST_VAL_VARIANT:  { east_type_retain(v->data.variant.type);
                                      arg_type = v->data.variant.type; break; }
            case EAST_VAL_REF:      arg_type = &east_blob_type; break; /* fallback */
            case EAST_VAL_VECTOR:   { east_type_retain(v->data.vector.elem_type);
                                      arg_type = east_vector_type(v->data.vector.elem_type); break; }
            case EAST_VAL_MATRIX:   { east_type_retain(v->data.matrix.elem_type);
                                      arg_type = east_matrix_type(v->data.matrix.elem_type); break; }
            case EAST_VAL_FUNCTION: arg_type = &east_blob_type; break; /* fallback */
        }

        ByteBuffer *vbuf = east_beast2_encode_full(v, arg_type);

        /* Write length + data */
        uint32_t vlen = (uint32_t)vbuf->len;
        byte_buffer_write_bytes(args_buf, (uint8_t *)&vlen, 4);
        byte_buffer_write_bytes(args_buf, vbuf->data, vbuf->len);

        byte_buffer_free(vbuf);

        /* Release constructed types (not primitives/singletons) */
        if (v->kind == EAST_VAL_ARRAY || v->kind == EAST_VAL_SET ||
            v->kind == EAST_VAL_DICT || v->kind == EAST_VAL_VECTOR ||
            v->kind == EAST_VAL_MATRIX) {
            east_type_release(arg_type);
        }
    }

    /* Call JS */
    size_t out_len = PLATFORM_RESULT_BUF_SIZE;
    int rc = js_platform_call(
        t->name,
        t->type_params_encoded, t->type_params_encoded_len,
        args_buf->data, args_buf->len,
        g_platform_result_buf, &out_len
    );

    byte_buffer_free(args_buf);

    if (rc != 0) {
        /* Error: out_buf contains error message as UTF-8 */
        char *msg = malloc(out_len + 1);
        memcpy(msg, g_platform_result_buf, out_len);
        msg[out_len] = '\0';
        EvalResult err = eval_error(msg);
        free(msg);
        return err;
    }

    if (out_len == 0) {
        /* No return value (null) */
        return eval_ok(east_null());
    }

    /* Decode result from Beast2-full */
    EastValue *result = east_beast2_decode_full(g_platform_result_buf, out_len, NULL);
    if (!result) {
        return eval_error("platform bridge: failed to decode result from JS");
    }

    return eval_ok(result);
}

/* Factory function: creates a PlatformFn for a specific (name, type_params) */
static PlatformFn platform_bridge_factory(EastType **type_params, size_t num_type_params) {
    /* This is called by the compiler when it encounters a generic platform node.
     * We need to return a PlatformFn, but we also need to stash the type_params.
     * We use the g_current_trampoline thread-local to pass context. */

    /* Note: The factory approach doesn't let us capture state in the returned
     * PlatformFn. Instead, we'll set up the trampoline at call time.
     * For now, just return the bridge function — the trampoline is set up
     * by our custom platform_registry_get wrapper. */
    (void)type_params;
    (void)num_type_params;
    return platform_bridge_fn;
}

/* register_js_platform is handled by east_wasm_register_platform export */

/* ------------------------------------------------------------------ */
/*  Handle table for compiled functions                                */
/* ------------------------------------------------------------------ */

#define MAX_HANDLES 4096
static EastCompiledFn *g_handles[MAX_HANDLES];
static uint32_t g_next_handle = 1;

static uint32_t alloc_handle(EastCompiledFn *fn) {
    for (uint32_t i = g_next_handle; i < MAX_HANDLES; i++) {
        if (g_handles[i] == NULL) {
            g_handles[i] = fn;
            g_next_handle = i + 1;
            return i;
        }
    }
    /* Wrap around */
    for (uint32_t i = 1; i < g_next_handle; i++) {
        if (g_handles[i] == NULL) {
            g_handles[i] = fn;
            g_next_handle = i + 1;
            return i;
        }
    }
    return 0; /* out of handles */
}

static EastCompiledFn *get_handle(uint32_t h) {
    if (h == 0 || h >= MAX_HANDLES) return NULL;
    return g_handles[h];
}

static void free_handle(uint32_t h) {
    if (h > 0 && h < MAX_HANDLES) {
        g_handles[h] = NULL;
    }
}

/* ------------------------------------------------------------------ */
/*  Trampoline management for platform calls                           */
/* ------------------------------------------------------------------ */

/* Find or create a trampoline for a (name, type_params) pair */
static PlatformTrampoline *get_or_create_trampoline(
    const char *name, EastType **type_params, size_t num_type_params
) {
    uint32_t h = trampoline_hash(name, type_params, num_type_params) % TRAMPOLINE_BUCKETS;

    /* Search existing */
    for (PlatformTrampoline *t = g_trampolines[h]; t; t = t->next) {
        if (strcmp(t->name, name) == 0 && t->num_type_params == num_type_params) {
            bool match = true;
            for (size_t i = 0; i < num_type_params && match; i++) {
                if (!east_type_equal(t->type_params[i], type_params[i])) match = false;
            }
            if (match) return t;
        }
    }

    /* Create new */
    PlatformTrampoline *t = calloc(1, sizeof(PlatformTrampoline));
    t->name = strdup(name);
    t->num_type_params = num_type_params;
    if (num_type_params > 0) {
        t->type_params = malloc(sizeof(EastType *) * num_type_params);
        for (size_t i = 0; i < num_type_params; i++) {
            t->type_params[i] = type_params[i];
            east_type_retain(type_params[i]);
        }
    }
    encode_type_params(t);
    t->next = g_trampolines[h];
    g_trampolines[h] = t;

    return t;
}

/* ------------------------------------------------------------------ */
/*  Custom eval wrapper to set up trampolines                          */
/* ------------------------------------------------------------------ */

/*
 * We hook into the platform call mechanism by wrapping the platform
 * registry's get function. When the compiler resolves a platform function,
 * we create a trampoline and store it. Then when the PlatformFn is called,
 * we set g_current_trampoline before calling platform_bridge_fn.
 *
 * Actually, since the PlatformFn returned by the factory is always
 * platform_bridge_fn, and we can't pass context through PlatformFn's
 * signature, we need a different approach.
 *
 * Solution: We'll override the compiler's IR_PLATFORM handling.
 * But that requires modifying east-c internals.
 *
 * Simpler solution: Use a pre-call hook. Since WASM is single-threaded,
 * we can safely use a global to set the current trampoline context
 * right before the PlatformFn is invoked.
 *
 * We'll register a non-generic platform function for each name that
 * sets up the trampoline. But we don't know the names in advance...
 *
 * Simplest solution: Register all platform functions as non-generic
 * with a naming convention. The JS side tells us which functions to
 * register during init.
 */

/* ------------------------------------------------------------------ */
/*  Last error tracking                                                */
/* ------------------------------------------------------------------ */

static char *g_last_error = NULL;

static void set_last_error(const char *msg) {
    free(g_last_error);
    g_last_error = msg ? strdup(msg) : NULL;
}

static void clear_last_error(void) {
    free(g_last_error);
    g_last_error = NULL;
}

/* ------------------------------------------------------------------ */
/*  Public WASM API                                                    */
/* ------------------------------------------------------------------ */

/*
 * Get the last error message. Returns pointer to null-terminated string
 * in WASM memory, or NULL if no error. Valid until next API call.
 */
EMSCRIPTEN_KEEPALIVE
const char *east_wasm_last_error(void) {
    return g_last_error;
}

EMSCRIPTEN_KEEPALIVE
void east_wasm_init(void) {
    if (g_initialized) return;

    east_type_of_type_init();

    g_platform = platform_registry_new();
    g_builtins = builtin_registry_new();
    east_register_all_builtins(g_builtins);

    g_platform_result_buf = malloc(PLATFORM_RESULT_BUF_SIZE);

    memset(g_handles, 0, sizeof(g_handles));
    memset(g_trampolines, 0, sizeof(g_trampolines));

    g_initialized = 1;
}

/*
 * Register a platform function name that will be delegated to JS.
 * Must be called after east_wasm_init() and before compiling IR that uses it.
 *
 * name: null-terminated function name (e.g. "state_read")
 * is_generic: true if the function takes type parameters
 * is_async: true if the function is async
 */
EMSCRIPTEN_KEEPALIVE
void east_wasm_register_platform(const char *name, int is_generic, int is_async) {
    if (is_generic) {
        platform_registry_add_generic(g_platform, name, platform_bridge_factory, is_async != 0);
    } else {
        platform_registry_add(g_platform, name, platform_bridge_fn, is_async != 0);
    }
}

/*
 * Compile East IR from Beast2-full encoded bytes.
 * Returns a handle (>0) on success, 0 on failure.
 *
 * The IR should be Beast2-full encoded (type header + value).
 * The type is expected to be east_ir_type.
 */
EMSCRIPTEN_KEEPALIVE
uint32_t east_wasm_compile(const uint8_t *ir_bytes, size_t ir_len) {
    clear_last_error();
    if (!g_initialized) {
        set_last_error("east_wasm_init() not called");
        return 0;
    }

    /* Decode Beast2-full as IR type */
    EastValue *ir_val = east_beast2_decode_full(ir_bytes, ir_len, east_ir_type);
    if (!ir_val) {
        set_last_error("failed to decode Beast2-full IR bytes");
        return 0;
    }

    /* Convert to IR node tree */
    IRNode *ir = east_ir_from_value(ir_val);
    east_value_release(ir_val);
    if (!ir) {
        set_last_error("failed to convert IR value to IR node tree");
        return 0;
    }

    /* Extract body if top-level is a function */
    IRNode *body = ir;
    if (ir->kind == IR_ASYNC_FUNCTION || ir->kind == IR_FUNCTION) {
        body = ir->data.function.body;
    }

    /* Compile */
    EastCompiledFn *fn = east_compile(body, g_platform, g_builtins);
    if (!fn) {
        set_last_error("failed to compile IR");
        ir_node_release(ir);
        return 0;
    }

    uint32_t handle = alloc_handle(fn);
    if (handle == 0) {
        set_last_error("out of compiled function handles");
        east_compiled_fn_free(fn);
    }

    ir_node_release(ir);
    return handle;
}

/*
 * Execute a compiled function with no arguments.
 * Returns Beast2-full encoded result.
 *
 * result_buf: caller-allocated buffer for the result
 * result_len: [in] capacity, [out] actual length
 * error_buf: caller-allocated buffer for error message (if any)
 * error_len: [in] capacity, [out] actual length
 *
 * Returns: 0 = success, 1 = error
 */
EMSCRIPTEN_KEEPALIVE
int east_wasm_call(uint32_t handle,
                    uint8_t *result_buf, size_t *result_len,
                    char *error_buf, size_t *error_len) {
    EastCompiledFn *fn = get_handle(handle);
    if (!fn) {
        const char *msg = "invalid handle";
        size_t mlen = strlen(msg);
        if (mlen > *error_len) mlen = *error_len;
        memcpy(error_buf, msg, mlen);
        *error_len = mlen;
        *result_len = 0;
        return 1;
    }

    EvalResult result = east_call(fn, NULL, 0);

    if (result.status == EVAL_ERROR) {
        const char *msg = result.error_message ? result.error_message : "unknown error";
        size_t mlen = strlen(msg);
        if (mlen > *error_len) mlen = *error_len;
        memcpy(error_buf, msg, mlen);
        *error_len = mlen;
        *result_len = 0;
        eval_result_free(&result);
        return 1;
    }

    if (!result.value || result.value->kind == EAST_VAL_NULL) {
        *result_len = 0;
        if (result.value) east_value_release(result.value);
        eval_result_free(&result);
        return 0;
    }

    /* We need a type to encode. Use the compiled function's IR type. */
    EastType *result_type = fn->ir->type;
    if (!result_type) {
        /* Fallback: can't encode without type */
        *result_len = 0;
        east_value_release(result.value);
        eval_result_free(&result);
        return 0;
    }

    ByteBuffer *buf = east_beast2_encode_full(result.value, result_type);
    if (buf->len > *result_len) {
        /* Buffer too small */
        const char *msg = "result buffer too small";
        size_t mlen = strlen(msg);
        if (mlen > *error_len) mlen = *error_len;
        memcpy(error_buf, msg, mlen);
        *error_len = mlen;
        *result_len = 0;
        byte_buffer_free(buf);
        east_value_release(result.value);
        eval_result_free(&result);
        return 1;
    }

    memcpy(result_buf, buf->data, buf->len);
    *result_len = buf->len;

    byte_buffer_free(buf);
    east_value_release(result.value);
    eval_result_free(&result);
    return 0;
}

/*
 * Execute a compiled function with Beast2-full encoded arguments.
 * Same format as platform bridge: [count:u32][len1:u32][beast2_full_1]...
 *
 * Returns: 0 = success, 1 = error
 */
EMSCRIPTEN_KEEPALIVE
int east_wasm_call_with_args(uint32_t handle,
                              const uint8_t *args_buf, size_t args_len,
                              uint8_t *result_buf, size_t *result_len,
                              char *error_buf, size_t *error_len) {
    EastCompiledFn *fn = get_handle(handle);
    if (!fn) {
        const char *msg = "invalid handle";
        size_t mlen = strlen(msg);
        if (mlen > *error_len) mlen = *error_len;
        memcpy(error_buf, msg, mlen);
        *error_len = mlen;
        *result_len = 0;
        return 1;
    }

    /* Decode args */
    size_t offset = 0;
    uint32_t num_args = 0;
    if (args_len >= 4) {
        memcpy(&num_args, args_buf, 4);
        offset = 4;
    }

    EastValue **args = NULL;
    if (num_args > 0) {
        args = calloc(num_args, sizeof(EastValue *));
        for (uint32_t i = 0; i < num_args; i++) {
            if (offset + 4 > args_len) break;
            uint32_t vlen;
            memcpy(&vlen, args_buf + offset, 4);
            offset += 4;
            if (offset + vlen > args_len) break;
            args[i] = east_beast2_decode_full(args_buf + offset, vlen, NULL);
            offset += vlen;
        }
    }

    EvalResult result = east_call(fn, args, num_args);

    /* Release args */
    for (uint32_t i = 0; i < num_args; i++) {
        if (args[i]) east_value_release(args[i]);
    }
    free(args);

    if (result.status == EVAL_ERROR) {
        const char *msg = result.error_message ? result.error_message : "unknown error";
        size_t mlen = strlen(msg);
        if (mlen > *error_len) mlen = *error_len;
        memcpy(error_buf, msg, mlen);
        *error_len = mlen;
        *result_len = 0;
        eval_result_free(&result);
        return 1;
    }

    if (!result.value || result.value->kind == EAST_VAL_NULL) {
        *result_len = 0;
        if (result.value) east_value_release(result.value);
        eval_result_free(&result);
        return 0;
    }

    EastType *result_type = fn->ir->type;
    if (!result_type) {
        *result_len = 0;
        east_value_release(result.value);
        eval_result_free(&result);
        return 0;
    }

    ByteBuffer *buf = east_beast2_encode_full(result.value, result_type);
    if (buf->len > *result_len) {
        const char *msg = "result buffer too small";
        size_t mlen = strlen(msg);
        if (mlen > *error_len) mlen = *error_len;
        memcpy(error_buf, msg, mlen);
        *error_len = mlen;
        *result_len = 0;
        byte_buffer_free(buf);
        east_value_release(result.value);
        eval_result_free(&result);
        return 1;
    }

    memcpy(result_buf, buf->data, buf->len);
    *result_len = buf->len;

    byte_buffer_free(buf);
    east_value_release(result.value);
    eval_result_free(&result);
    return 0;
}

/*
 * Free a compiled function handle.
 */
EMSCRIPTEN_KEEPALIVE
void east_wasm_free(uint32_t handle) {
    EastCompiledFn *fn = get_handle(handle);
    if (fn) {
        east_compiled_fn_free(fn);
        free_handle(handle);
    }
}

/*
 * Run garbage collection cycle.
 */
EMSCRIPTEN_KEEPALIVE
void east_wasm_gc(void) {
    east_gc_collect();
}

/*
 * Allocate memory in WASM heap (for JS to write data into).
 */
EMSCRIPTEN_KEEPALIVE
void *east_wasm_malloc(size_t size) {
    return malloc(size);
}

/*
 * Free memory in WASM heap.
 */
EMSCRIPTEN_KEEPALIVE
void east_wasm_free_buf(void *ptr) {
    free(ptr);
}

/*
 * Set the current trampoline for the next platform call.
 * Called by the custom eval loop before invoking a platform function.
 *
 * This is needed because PlatformFn doesn't carry context.
 * Since WASM is single-threaded, a global is safe.
 */
EMSCRIPTEN_KEEPALIVE
void east_wasm_set_platform_context(const char *name,
                                     const uint8_t *type_params_buf,
                                     size_t type_params_len) {
    /* Decode type params if provided */
    EastType **type_params = NULL;
    size_t num_type_params = 0;

    if (type_params_buf && type_params_len > 0) {
        EastType *arr_type = east_array_type(east_type_type);
        EastValue *arr = east_beast2_decode_full(type_params_buf, type_params_len, arr_type);
        east_type_release(arr_type);

        if (arr && arr->kind == EAST_VAL_ARRAY) {
            num_type_params = east_array_len(arr);
            type_params = malloc(sizeof(EastType *) * num_type_params);
            for (size_t i = 0; i < num_type_params; i++) {
                EastValue *tv = east_array_get(arr, i);
                type_params[i] = east_type_from_value(tv);
            }
        }
        if (arr) east_value_release(arr);
    }

    g_current_trampoline = get_or_create_trampoline(name, type_params, num_type_params);

    /* Clean up temporary type_params array (trampoline retains its own copies) */
    for (size_t i = 0; i < num_type_params; i++) {
        east_type_release(type_params[i]);
    }
    free(type_params);
}
