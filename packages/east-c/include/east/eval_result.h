#ifndef EAST_EVAL_RESULT_H
#define EAST_EVAL_RESULT_H

#include "ir.h"
#include "values.h"

typedef enum {
    EVAL_OK,
    EVAL_RETURN,
    EVAL_BREAK,
    EVAL_CONTINUE,
    EVAL_ERROR,
} EvalStatus;

typedef struct {
    EvalStatus status;
    EastValue *value;
    char *label;               // for break/continue
    char *error_message;       // for errors
    EastLocation *locations;   // error location stack trace (owned)
    size_t num_locations;
} EvalResult;

// Convenience constructors
EvalResult eval_ok(EastValue *value);
EvalResult eval_error(const char *msg);
void eval_result_free(EvalResult *result);

#endif
