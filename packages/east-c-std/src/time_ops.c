/*
 * Time platform functions for East.
 *
 * Provides time-related operations for East programs running in C.
 */

#include "east_std/east_std.h"
#include <east/values.h>
#include <east/eval_result.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>

static EvalResult time_now(EastValue **args, size_t num_args) {
    (void)args;
    (void)num_args;

    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    int64_t millis = (int64_t)ts.tv_sec * 1000 + (int64_t)ts.tv_nsec / 1000000;
    return eval_ok(east_integer(millis));
}

static EvalResult time_sleep(EastValue **args, size_t num_args) {
    (void)num_args;
    int64_t millis = args[0]->data.integer;

    if (millis > 0) {
        usleep((useconds_t)(millis * 1000));
    }
    return eval_ok(east_null());
}

void east_std_register_time(PlatformRegistry *reg) {
    platform_registry_add(reg, "time_now", time_now, false);
    platform_registry_add(reg, "time_sleep", time_sleep, false);
}
