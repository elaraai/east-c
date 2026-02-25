/*
 * Compliance test runner for east-c.
 *
 * Loads TypeScript-exported IR JSON files and executes them to verify
 * cross-implementation compatibility.
 *
 * Usage: test_compliance <path-to-ir.json>
 *
 * To generate test IR files:
 *   cd ../east && npm run test:export
 */

#include <east/east.h>
#include <east/eval_result.h>
#include <east/type_of_type.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------ */
/*  Test counters                                                      */
/* ------------------------------------------------------------------ */

static int g_tests_run = 0;
static int g_tests_passed = 0;
static int g_tests_failed = 0;
static const char *g_current_describe = "";

/* ------------------------------------------------------------------ */
/*  Test platform functions                                            */
/* ------------------------------------------------------------------ */

static EvalResult plat_test_pass(EastValue **args, size_t num_args)
{
    (void)args; (void)num_args;
    return eval_ok(east_null());
}

static EvalResult plat_test_fail(EastValue **args, size_t num_args)
{
    (void)num_args;
    const char *message = "";
    if (num_args > 0 && args[0] && args[0]->kind == EAST_VAL_STRING) {
        message = args[0]->data.string.data;
    }
    return eval_error(message);
}

static EvalResult plat_describe(EastValue **args, size_t num_args)
{
    (void)num_args;

    const char *name = "";
    if (num_args > 0 && args[0] && args[0]->kind == EAST_VAL_STRING) {
        name = args[0]->data.string.data;
    }

    g_current_describe = name;
    printf("  %s\n", name);

    /* Call the body function (second argument) */
    if (num_args > 1 && args[1] && args[1]->kind == EAST_VAL_FUNCTION) {
        EastCompiledFn *body = args[1]->data.function.compiled;
        EvalResult r = east_call(body, NULL, 0);
        if (r.status == EVAL_ERROR) {
            fprintf(stderr, "    ERROR in describe \"%s\": %s\n",
                    name, r.error_message ? r.error_message : "?");
            eval_result_free(&r);
            /* Propagate the error */
            return eval_error("describe body failed");
        }
        if (r.value) east_value_release(r.value);
        eval_result_free(&r);
    }

    return eval_ok(east_null());
}

static EvalResult plat_test(EastValue **args, size_t num_args)
{
    (void)num_args;

    const char *name = "";
    if (num_args > 0 && args[0] && args[0]->kind == EAST_VAL_STRING) {
        name = args[0]->data.string.data;
    }

    g_tests_run++;

    /* Call the body function (second argument) */
    if (num_args > 1 && args[1] && args[1]->kind == EAST_VAL_FUNCTION) {
        EastCompiledFn *body = args[1]->data.function.compiled;
        EvalResult r = east_call(body, NULL, 0);
        if (r.status == EVAL_ERROR) {
            g_tests_failed++;
            printf("    FAIL %s: %s\n", name,
                   r.error_message ? r.error_message : "?");
            if (r.locations && r.num_locations > 0) {
                printf("      at %s:%ld:%ld\n",
                       r.locations[0].filename ? r.locations[0].filename : "?",
                       (long)r.locations[0].line,
                       (long)r.locations[0].column);
            }
            eval_result_free(&r);
            return eval_ok(east_null());
        }
        if (r.value) east_value_release(r.value);
        eval_result_free(&r);
    }

    g_tests_passed++;
    printf("    PASS %s\n", name);
    return eval_ok(east_null());
}

/* ------------------------------------------------------------------ */
/*  File loading                                                       */
/* ------------------------------------------------------------------ */

static char *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "Cannot open file: %s\n", path);
        return NULL;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t read = fread(buf, 1, (size_t)len, f);
    buf[read] = '\0';
    fclose(f);

    if (out_len) *out_len = read;
    return buf;
}

/* ------------------------------------------------------------------ */
/*  Main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <ir-json-file>\n", argv[0]);
        return 1;
    }

    const char *json_path = argv[1];

    /* Initialize type descriptors */
    east_type_of_type_init();

    /* Register builtins */
    BuiltinRegistry *builtins = builtin_registry_new();
    east_register_all_builtins(builtins);

    /* Register test platform functions */
    PlatformRegistry *platform = platform_registry_new();
    platform_registry_add(platform, "testPass", plat_test_pass, false);
    platform_registry_add(platform, "testFail", plat_test_fail, false);
    platform_registry_add(platform, "describe", plat_describe, true);
    platform_registry_add(platform, "test", plat_test, true);

    /* Stage 1: Read JSON file */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    size_t json_len = 0;
    char *json = read_file(json_path, &json_len);
    if (!json) return 1;

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double load_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                     (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("Load: %.1f ms (%.1f MB)\n", load_ms, json_len / (1024.0 * 1024.0));

    /* Stage 2: Decode JSON to EastValue using IRType */
    clock_gettime(CLOCK_MONOTONIC, &t0);

    EastValue *ir_val = east_json_decode(json, east_ir_type);
    free(json);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double decode_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                       (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("Decode: %.1f ms\n", decode_ms);

    if (!ir_val) {
        fprintf(stderr, "Failed to decode JSON as IR\n");
        return 1;
    }

    /* Stage 3: Convert EastValue variant tree to IRNode */
    clock_gettime(CLOCK_MONOTONIC, &t0);

    IRNode *ir = east_ir_from_value(ir_val);
    east_value_release(ir_val);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double convert_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                        (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("Convert: %.1f ms\n", convert_ms);

    if (!ir) {
        fprintf(stderr, "Failed to convert IR value to IR node\n");
        return 1;
    }

    /* Stage 4: Compile and execute */
    clock_gettime(CLOCK_MONOTONIC, &t0);

    /*
     * The top-level IR is an AsyncFunction with 0 params.
     * Extract the body and compile it directly.
     */
    IRNode *body = ir;
    if (ir->kind == IR_ASYNC_FUNCTION || ir->kind == IR_FUNCTION) {
        body = ir->data.function.body;
    }

    EastCompiledFn *fn = east_compile(body, platform, builtins);
    if (!fn) {
        fprintf(stderr, "Failed to compile IR\n");
        ir_node_release(ir);
        return 1;
    }

    /* Extract the filename from path for display */
    const char *fname = strrchr(json_path, '/');
    fname = fname ? fname + 1 : json_path;
    printf("\n%s:\n", fname);

    EvalResult result = east_call(fn, NULL, 0);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    double exec_ms = (t1.tv_sec - t0.tv_sec) * 1000.0 +
                     (t1.tv_nsec - t0.tv_nsec) / 1e6;

    if (result.status == EVAL_ERROR) {
        fprintf(stderr, "\nFATAL ERROR: %s\n",
                result.error_message ? result.error_message : "unknown");
        if (result.locations && result.num_locations > 0) {
            fprintf(stderr, "  at %s:%ld:%ld\n",
                    result.locations[0].filename
                        ? result.locations[0].filename : "?",
                    (long)result.locations[0].line,
                    (long)result.locations[0].column);
        }
    }

    printf("\nResults: %d/%d passed", g_tests_passed, g_tests_run);
    if (g_tests_failed > 0) {
        printf(" (%d failed)", g_tests_failed);
    }
    printf("\nExecute: %.1f ms\n", exec_ms);

    /* Cleanup */
    if (result.value) east_value_release(result.value);
    eval_result_free(&result);
    east_compiled_fn_free(fn);
    ir_node_release(ir);
    platform_registry_free(platform);
    builtin_registry_free(builtins);

    return g_tests_failed > 0 ? 1 : 0;
}
