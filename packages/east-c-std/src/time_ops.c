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
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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

static EvalResult time_get_timezone_offset(EastValue **args, size_t num_args) {
    (void)num_args;
    int64_t epoch_ms = args[0]->data.datetime;
    const char *zone_name = args[1]->data.string.data;

    /* Save and restore TZ to avoid side effects */
    char *old_tz = getenv("TZ");
    char *saved_tz = old_tz ? strdup(old_tz) : NULL;

    /* Set the target timezone */
    setenv("TZ", zone_name, 1);
    tzset();

    /* Convert epoch milliseconds to time_t */
    time_t epoch_sec = (time_t)(epoch_ms / 1000);

    /* Get local time in the target timezone */
    struct tm local_tm;
    struct tm utc_tm;
    localtime_r(&epoch_sec, &local_tm);
    gmtime_r(&epoch_sec, &utc_tm);

    /* Restore original TZ */
    if (saved_tz) {
        setenv("TZ", saved_tz, 1);
        free(saved_tz);
    } else {
        unsetenv("TZ");
    }
    tzset();

    /* Validate that the timezone was recognized by checking if localtime
     * produced a reasonable result. POSIX treats unrecognized TZ as UTC,
     * so if tm_gmtoff is 0 and the zone name is not UTC/GMT, it's likely invalid.
     * However, some valid zones ARE UTC+0, so we use tm_zone for validation. */
#ifdef __linux__
    /* On Linux, tm_zone is set to the abbreviation. If POSIX fell back to UTC
     * for an unknown zone, tm_gmtoff will be 0. We can't perfectly distinguish
     * "valid UTC+0 zone" from "invalid zone treated as UTC", but this covers
     * the common case. */
    (void)0; /* Accept the result as-is; POSIX semantics are best-effort */
#endif

    /* Compute offset in minutes using tm_gmtoff (seconds east of UTC) */
    int64_t offset_minutes = local_tm.tm_gmtoff / 60;

    return eval_ok(east_integer(offset_minutes));
}

void east_std_register_time(PlatformRegistry *reg) {
    platform_registry_add(reg, "time_now", time_now, false);
    platform_registry_add(reg, "time_sleep", time_sleep, false);
    platform_registry_add(reg, "time_get_timezone_offset", time_get_timezone_offset, false);
}
