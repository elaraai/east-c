#ifndef EAST_COMPILER_H
#define EAST_COMPILER_H

#include "builtins.h"
#include "env.h"
#include "eval_result.h"
#include "ir.h"
#include "platform.h"
#include "types.h"
#include "values.h"

struct EastCompiledFn {
    IRNode *ir;
    Environment *captures;
    char **param_names;
    size_t num_params;
    PlatformRegistry *platform;
    BuiltinRegistry *builtins;
    EastValue *source_ir;  // original IR variant value for serialization
};

// Top-level API
EastCompiledFn *east_compile(IRNode *ir, PlatformRegistry *platform, BuiltinRegistry *builtins);
EvalResult east_call(EastCompiledFn *fn, EastValue **args, size_t num_args);
void east_compiled_fn_free(EastCompiledFn *fn);

// Internal evaluation
EvalResult eval_ir(IRNode *node, Environment *env, PlatformRegistry *platform, BuiltinRegistry *builtins);

// Access the current platform/builtins registries (valid during east_call)
PlatformRegistry *east_current_platform(void);
BuiltinRegistry *east_current_builtins(void);

// Set thread-local platform/builtins for worker threads (call before beast2 decode)
void east_set_thread_context(PlatformRegistry *p, BuiltinRegistry *b);

#endif
