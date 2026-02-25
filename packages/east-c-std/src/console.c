/*
 * Console I/O platform functions for East.
 *
 * Provides console output operations for East programs running in C.
 */

#include "east_std/east_std.h"
#include <east/values.h>
#include <east/eval_result.h>
#include <stdio.h>

static EvalResult console_log(EastValue **args, size_t num_args) {
    (void)num_args;
    printf("%s\n", args[0]->data.string.data);
    fflush(stdout);
    return eval_ok(east_null());
}

static EvalResult console_error(EastValue **args, size_t num_args) {
    (void)num_args;
    fprintf(stderr, "%s\n", args[0]->data.string.data);
    fflush(stderr);
    return eval_ok(east_null());
}

static EvalResult console_write(EastValue **args, size_t num_args) {
    (void)num_args;
    printf("%s", args[0]->data.string.data);
    fflush(stdout);
    return eval_ok(east_null());
}

void east_std_register_console(PlatformRegistry *reg) {
    platform_registry_add(reg, "console_log", console_log, false);
    platform_registry_add(reg, "console_error", console_error, false);
    platform_registry_add(reg, "console_write", console_write, false);
}
