/*
 * Integer builtin functions.
 */
#include "east/builtins.h"
#include "east/values.h"
#include <stdlib.h>
#include <stdint.h>

/* --- static implementations --- */

static EastValue *integer_add(EastValue **args, size_t n) {
    (void)n;
    return east_integer(args[0]->data.integer + args[1]->data.integer);
}

static EastValue *integer_subtract(EastValue **args, size_t n) {
    (void)n;
    return east_integer(args[0]->data.integer - args[1]->data.integer);
}

static EastValue *integer_multiply(EastValue **args, size_t n) {
    (void)n;
    return east_integer(args[0]->data.integer * args[1]->data.integer);
}

static EastValue *integer_divide(EastValue **args, size_t n) {
    (void)n;
    int64_t a = args[0]->data.integer;
    int64_t b = args[1]->data.integer;
    if (b == 0) return east_integer(0); /* guard against div-by-zero */
    /* Floor division matching C truncation toward zero for positive,
       but we want floored division like Python // */
    int64_t q = a / b;
    int64_t r = a % b;
    /* Adjust if remainder and divisor have different signs */
    if (r != 0 && ((r ^ b) < 0)) q -= 1;
    return east_integer(q);
}

static EastValue *integer_remainder(EastValue **args, size_t n) {
    (void)n;
    int64_t a = args[0]->data.integer;
    int64_t b = args[1]->data.integer;
    if (b == 0) return east_integer(0);
    /* JavaScript-style remainder: result has sign of dividend (truncated division) */
    int64_t r = a % b; /* C99 truncated semantics -- already matches JS */
    return east_integer(r);
}

static EastValue *integer_power(EastValue **args, size_t n) {
    (void)n;
    int64_t base = args[0]->data.integer;
    int64_t exp = args[1]->data.integer;
    if (exp < 0) return east_integer(0);
    int64_t result = 1;
    while (exp > 0) {
        if (exp & 1) result *= base;
        base *= base;
        exp >>= 1;
    }
    return east_integer(result);
}

static EastValue *integer_negate(EastValue **args, size_t n) {
    (void)n;
    return east_integer(-args[0]->data.integer);
}

static EastValue *integer_abs(EastValue **args, size_t n) {
    (void)n;
    int64_t a = args[0]->data.integer;
    return east_integer(a < 0 ? -a : a);
}

static EastValue *integer_sign(EastValue **args, size_t n) {
    (void)n;
    int64_t a = args[0]->data.integer;
    if (a < 0) return east_integer(-1);
    if (a > 0) return east_integer(1);
    return east_integer(0);
}

static EastValue *integer_log(EastValue **args, size_t n) {
    (void)n;
    int64_t a = args[0]->data.integer;
    int64_t base = args[1]->data.integer;
    if (a == 0 || base <= 1) return east_integer(0);
    int64_t abs_val = a < 0 ? -a : a;
    int64_t result = 0;
    while (abs_val >= base) {
        abs_val /= base;
        result++;
    }
    return east_integer(result);
}

static EastValue *integer_to_float(EastValue **args, size_t n) {
    (void)n;
    return east_float((double)args[0]->data.integer);
}

/* --- factory functions (no type params needed) --- */

static BuiltinImpl integer_add_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return integer_add; }
static BuiltinImpl integer_subtract_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return integer_subtract; }
static BuiltinImpl integer_multiply_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return integer_multiply; }
static BuiltinImpl integer_divide_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return integer_divide; }
static BuiltinImpl integer_remainder_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return integer_remainder; }
static BuiltinImpl integer_power_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return integer_power; }
static BuiltinImpl integer_negate_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return integer_negate; }
static BuiltinImpl integer_abs_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return integer_abs; }
static BuiltinImpl integer_sign_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return integer_sign; }
static BuiltinImpl integer_log_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return integer_log; }
static BuiltinImpl integer_to_float_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return integer_to_float; }

/* --- registration --- */

void east_register_integer_builtins(BuiltinRegistry *reg) {
    builtin_registry_register(reg, "IntegerAdd", integer_add_factory);
    builtin_registry_register(reg, "IntegerSubtract", integer_subtract_factory);
    builtin_registry_register(reg, "IntegerMultiply", integer_multiply_factory);
    builtin_registry_register(reg, "IntegerDivide", integer_divide_factory);
    builtin_registry_register(reg, "IntegerRemainder", integer_remainder_factory);
    builtin_registry_register(reg, "IntegerPow", integer_power_factory);
    builtin_registry_register(reg, "IntegerNegate", integer_negate_factory);
    builtin_registry_register(reg, "IntegerAbs", integer_abs_factory);
    builtin_registry_register(reg, "IntegerSign", integer_sign_factory);
    builtin_registry_register(reg, "IntegerLog", integer_log_factory);
    builtin_registry_register(reg, "IntegerToFloat", integer_to_float_factory);
}
