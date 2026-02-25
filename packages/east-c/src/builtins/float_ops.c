/*
 * Float builtin functions.
 */
#include "east/builtins.h"
#include "east/values.h"
#include <math.h>
#include <stdlib.h>

/* --- static implementations --- */

static EastValue *float_add(EastValue **args, size_t n) {
    (void)n;
    return east_float(args[0]->data.float64 + args[1]->data.float64);
}

static EastValue *float_subtract(EastValue **args, size_t n) {
    (void)n;
    return east_float(args[0]->data.float64 - args[1]->data.float64);
}

static EastValue *float_multiply(EastValue **args, size_t n) {
    (void)n;
    return east_float(args[0]->data.float64 * args[1]->data.float64);
}

static EastValue *float_divide(EastValue **args, size_t n) {
    (void)n;
    return east_float(args[0]->data.float64 / args[1]->data.float64);
}

static EastValue *float_remainder(EastValue **args, size_t n) {
    (void)n;
    double a = args[0]->data.float64;
    double b = args[1]->data.float64;
    if (b == 0.0) return east_float(NAN);
    /* JavaScript-style remainder: result = a - trunc(a/b) * b */
    double result = a - trunc(a / b) * b;
    /* Preserve signed zero */
    if (result == 0.0) result = copysign(0.0, a);
    return east_float(result);
}

static EastValue *float_power(EastValue **args, size_t n) {
    (void)n;
    return east_float(pow(args[0]->data.float64, args[1]->data.float64));
}

static EastValue *float_negate(EastValue **args, size_t n) {
    (void)n;
    return east_float(-args[0]->data.float64);
}

static EastValue *float_abs_impl(EastValue **args, size_t n) {
    (void)n;
    return east_float(fabs(args[0]->data.float64));
}

static EastValue *float_sign(EastValue **args, size_t n) {
    (void)n;
    double a = args[0]->data.float64;
    if (isnan(a)) return east_float(0.0);
    if (a < 0.0) return east_float(-1.0);
    if (a > 0.0) return east_float(1.0);
    return east_float(0.0);
}

static EastValue *float_sqrt_impl(EastValue **args, size_t n) {
    (void)n;
    return east_float(sqrt(args[0]->data.float64));
}

static EastValue *float_log_impl(EastValue **args, size_t n) {
    (void)n;
    return east_float(log(args[0]->data.float64));
}

static EastValue *float_exp_impl(EastValue **args, size_t n) {
    (void)n;
    return east_float(exp(args[0]->data.float64));
}

static EastValue *float_sin_impl(EastValue **args, size_t n) {
    (void)n;
    return east_float(sin(args[0]->data.float64));
}

static EastValue *float_cos_impl(EastValue **args, size_t n) {
    (void)n;
    return east_float(cos(args[0]->data.float64));
}

static EastValue *float_tan_impl(EastValue **args, size_t n) {
    (void)n;
    return east_float(tan(args[0]->data.float64));
}

static EastValue *float_to_integer(EastValue **args, size_t n) {
    (void)n;
    double val = args[0]->data.float64;
    if (isnan(val)) {
        east_builtin_error("Cannot convert NaN to integer");
        return NULL;
    }
    /* 2^63 = 9223372036854775808.0 */
    if (val >= 9223372036854775808.0) {
        east_builtin_error("Float too high to convert to integer");
        return NULL;
    }
    /* -2^63 = -9223372036854775808.0 */
    if (val < -9223372036854775808.0) {
        east_builtin_error("Float too low to convert to integer");
        return NULL;
    }
    if (val != trunc(val)) {
        east_builtin_error("Cannot convert non-integer float to integer");
        return NULL;
    }
    return east_integer((int64_t)val);
}

/* --- factory functions --- */

static BuiltinImpl float_add_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return float_add; }
static BuiltinImpl float_subtract_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return float_subtract; }
static BuiltinImpl float_multiply_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return float_multiply; }
static BuiltinImpl float_divide_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return float_divide; }
static BuiltinImpl float_remainder_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return float_remainder; }
static BuiltinImpl float_power_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return float_power; }
static BuiltinImpl float_negate_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return float_negate; }
static BuiltinImpl float_abs_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return float_abs_impl; }
static BuiltinImpl float_sign_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return float_sign; }
static BuiltinImpl float_sqrt_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return float_sqrt_impl; }
static BuiltinImpl float_log_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return float_log_impl; }
static BuiltinImpl float_exp_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return float_exp_impl; }
static BuiltinImpl float_sin_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return float_sin_impl; }
static BuiltinImpl float_cos_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return float_cos_impl; }
static BuiltinImpl float_tan_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return float_tan_impl; }
static BuiltinImpl float_to_integer_factory(EastType **tp, size_t ntp) { (void)tp; (void)ntp; return float_to_integer; }

/* --- registration --- */

void east_register_float_builtins(BuiltinRegistry *reg) {
    builtin_registry_register(reg, "FloatAdd", float_add_factory);
    builtin_registry_register(reg, "FloatSubtract", float_subtract_factory);
    builtin_registry_register(reg, "FloatMultiply", float_multiply_factory);
    builtin_registry_register(reg, "FloatDivide", float_divide_factory);
    builtin_registry_register(reg, "FloatRemainder", float_remainder_factory);
    builtin_registry_register(reg, "FloatPow", float_power_factory);
    builtin_registry_register(reg, "FloatNegate", float_negate_factory);
    builtin_registry_register(reg, "FloatAbs", float_abs_factory);
    builtin_registry_register(reg, "FloatSign", float_sign_factory);
    builtin_registry_register(reg, "FloatSqrt", float_sqrt_factory);
    builtin_registry_register(reg, "FloatLog", float_log_factory);
    builtin_registry_register(reg, "FloatExp", float_exp_factory);
    builtin_registry_register(reg, "FloatSin", float_sin_factory);
    builtin_registry_register(reg, "FloatCos", float_cos_factory);
    builtin_registry_register(reg, "FloatTan", float_tan_factory);
    builtin_registry_register(reg, "FloatToInteger", float_to_integer_factory);
}
