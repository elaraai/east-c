/*
 * Test platform functions for East.
 *
 * Provides test assertion and organization operations for East programs
 * running in C. These functions mirror the test utilities in the Python
 * implementation for running East tests.
 */

#include "east_std/east_std.h"
#include <east/eval_result.h>
#include <east/values.h>
#include <stdio.h>
#include <stdlib.h>

static EvalResult test_pass(EastValue **args, size_t num_args) {
    (void)args;
    (void)num_args;
    /* No-op: test assertion passed, execution continues normally */
    return eval_ok(east_null());
}

static EvalResult test_fail(EastValue **args, size_t num_args) {
    (void)num_args;
    const char *message = args[0]->data.string.data;
    return eval_error(message);
}

static EvalResult test_impl(EastValue **args, size_t num_args) {
    (void)num_args;
    EastValue *name = args[0];
    EastValue *body = args[1];

    (void)name;

    /* Call the test function body if it is a compiled function */
    if (body->kind == EAST_VAL_FUNCTION && body->data.function.compiled) {
        /*
         * The test body is an EastCompiledFn. The runtime caller is
         * responsible for invoking the function -- we simply return
         * null here and let the VM/interpreter dispatch the call.
         * In a direct C harness, the caller would invoke the compiled
         * function pointer.
         */
    }

    return eval_ok(east_null());
}

static EvalResult describe_impl(EastValue **args, size_t num_args) {
    (void)num_args;
    EastValue *name = args[0];
    EastValue *body = args[1];

    (void)name;

    /* Same semantics as test_impl -- the runtime dispatches the call */
    if (body->kind == EAST_VAL_FUNCTION && body->data.function.compiled) {
        /* Let the VM/interpreter handle the actual invocation */
    }

    return eval_ok(east_null());
}

void east_std_register_test(PlatformRegistry *reg) {
    platform_registry_add(reg, "testPass", test_pass, false);
    platform_registry_add(reg, "testFail", test_fail, false);
    platform_registry_add(reg, "test", test_impl, false);
    platform_registry_add(reg, "describe", describe_impl, false);
}
