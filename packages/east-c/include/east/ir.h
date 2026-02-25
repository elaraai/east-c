#ifndef EAST_IR_H
#define EAST_IR_H

#include "types.h"
#include "values.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Source location for error reporting / stack traces
typedef struct {
    char *filename;
    int64_t line;
    int64_t column;
} EastLocation;

typedef enum {
    IR_VALUE,
    IR_VARIABLE,
    IR_LET,
    IR_ASSIGN,
    IR_BLOCK,
    IR_IF_ELSE,
    IR_MATCH,
    IR_WHILE,
    IR_FOR_ARRAY,
    IR_FOR_SET,
    IR_FOR_DICT,
    IR_FUNCTION,
    IR_ASYNC_FUNCTION,
    IR_CALL,
    IR_CALL_ASYNC,
    IR_PLATFORM,
    IR_BUILTIN,
    IR_RETURN,
    IR_BREAK,
    IR_CONTINUE,
    IR_ERROR,
    IR_TRY_CATCH,
    IR_NEW_ARRAY,
    IR_NEW_SET,
    IR_NEW_DICT,
    IR_NEW_REF,
    IR_NEW_VECTOR,
    IR_STRUCT,
    IR_GET_FIELD,
    IR_VARIANT,
    IR_WRAP_RECURSIVE,
    IR_UNWRAP_RECURSIVE,
} IRNodeKind;

typedef struct IRNode IRNode;

typedef struct {
    char *name;
    bool mutable;
    bool captured;
} IRVariable;

typedef struct {
    char *case_name;
    char *bind_name;
    IRNode *body;
} IRMatchCase;

struct IRNode {
    IRNodeKind kind;
    int ref_count;
    EastType *type;
    EastLocation *locations;     // Source location stack (array, owned)
    size_t num_locations;
    union {
        // IR_VALUE
        struct { EastValue *value; } value;

        // IR_VARIABLE
        struct { char *name; bool mutable; bool captured; } variable;

        // IR_LET
        struct { IRVariable var; IRNode *value; } let;

        // IR_ASSIGN
        struct { char *name; IRNode *value; } assign;

        // IR_BLOCK
        struct { IRNode **stmts; size_t num_stmts; } block;

        // IR_IF_ELSE
        struct { IRNode *cond; IRNode *then_branch; IRNode *else_branch; } if_else;

        // IR_MATCH
        struct {
            IRNode *expr;
            IRMatchCase *cases;
            size_t num_cases;
        } match;

        // IR_WHILE
        struct { IRNode *cond; IRNode *body; char *label; } while_;

        // IR_FOR_ARRAY
        struct {
            char *var_name;
            char *index_name;  // may be NULL
            IRNode *array;
            IRNode *body;
            char *label;
        } for_array;

        // IR_FOR_SET
        struct {
            char *var_name;
            IRNode *set;
            IRNode *body;
            char *label;
        } for_set;

        // IR_FOR_DICT
        struct {
            char *key_name;
            char *val_name;
            IRNode *dict;
            IRNode *body;
            char *label;
        } for_dict;

        // IR_FUNCTION, IR_ASYNC_FUNCTION
        struct {
            IRVariable *captures;
            size_t num_captures;
            IRVariable *params;
            size_t num_params;
            IRNode *body;
            EastValue *source_ir;  // original IR variant value for serialization
        } function;

        // IR_CALL, IR_CALL_ASYNC
        struct {
            IRNode *func;
            IRNode **args;
            size_t num_args;
        } call;

        // IR_PLATFORM
        struct {
            char *name;
            EastType **type_params;
            size_t num_type_params;
            IRNode **args;
            size_t num_args;
            bool is_async;
            bool optional;
        } platform;

        // IR_BUILTIN
        struct {
            char *name;
            EastType **type_params;
            size_t num_type_params;
            IRNode **args;
            size_t num_args;
        } builtin;

        // IR_RETURN
        struct { IRNode *value; } return_;

        // IR_BREAK, IR_CONTINUE
        struct { char *label; } loop_ctrl;

        // IR_ERROR
        struct { IRNode *message; } error;

        // IR_TRY_CATCH
        struct {
            IRNode *try_body;
            char *message_var;    // variable name for error message (string)
            char *stack_var;      // variable name for location stack (array of structs)
            IRNode *catch_body;
            IRNode *finally_body; // may be NULL if no finally block
        } try_catch;

        // IR_NEW_ARRAY, IR_NEW_SET
        struct {
            IRNode **items;
            size_t num_items;
        } new_collection;

        // IR_NEW_DICT
        struct {
            IRNode **keys;
            IRNode **values;
            size_t num_pairs;
        } new_dict;

        // IR_NEW_REF
        struct { IRNode *value; } new_ref;

        // IR_NEW_VECTOR
        struct {
            IRNode **items;
            size_t num_items;
        } new_vector;

        // IR_STRUCT
        struct {
            char **field_names;
            IRNode **field_values;
            size_t num_fields;
        } struct_;

        // IR_GET_FIELD
        struct {
            IRNode *expr;
            char *field_name;
        } get_field;

        // IR_VARIANT
        struct {
            char *case_name;
            IRNode *value;
        } variant;

        // IR_WRAP_RECURSIVE, IR_UNWRAP_RECURSIVE
        struct { IRNode *value; } recursive;
    } data;
};

// Builder functions
IRNode *ir_value(EastType *type, EastValue *value);
IRNode *ir_variable(EastType *type, const char *name, bool mutable, bool captured);
IRNode *ir_let(EastType *type, const char *var_name, bool mutable, bool captured, IRNode *value);
IRNode *ir_assign(EastType *type, const char *name, IRNode *value);
IRNode *ir_block(EastType *type, IRNode **stmts, size_t num_stmts);
IRNode *ir_if_else(EastType *type, IRNode *cond, IRNode *then_b, IRNode *else_b);
IRNode *ir_match(EastType *type, IRNode *expr, IRMatchCase *cases, size_t num_cases);
IRNode *ir_while(EastType *type, IRNode *cond, IRNode *body, const char *label);
IRNode *ir_for_array(EastType *type, const char *var, const char *idx, IRNode *array, IRNode *body, const char *label);
IRNode *ir_for_set(EastType *type, const char *var, IRNode *set, IRNode *body, const char *label);
IRNode *ir_for_dict(EastType *type, const char *key, const char *val, IRNode *dict, IRNode *body, const char *label);
IRNode *ir_function(EastType *type, IRVariable *captures, size_t num_captures, IRVariable *params, size_t num_params, IRNode *body);
IRNode *ir_async_function(EastType *type, IRVariable *captures, size_t num_captures, IRVariable *params, size_t num_params, IRNode *body);
IRNode *ir_call(EastType *type, IRNode *func, IRNode **args, size_t num_args);
IRNode *ir_call_async(EastType *type, IRNode *func, IRNode **args, size_t num_args);
IRNode *ir_platform(EastType *type, const char *name, EastType **type_params, size_t num_tp, IRNode **args, size_t num_args, bool is_async, bool optional);
IRNode *ir_builtin(EastType *type, const char *name, EastType **type_params, size_t num_tp, IRNode **args, size_t num_args);
IRNode *ir_return(EastType *type, IRNode *value);
IRNode *ir_break(const char *label);
IRNode *ir_continue(const char *label);
IRNode *ir_error(EastType *type, IRNode *message);
IRNode *ir_try_catch(EastType *type, IRNode *try_body, const char *message_var, const char *stack_var, IRNode *catch_body, IRNode *finally_body);
IRNode *ir_new_array(EastType *type, IRNode **items, size_t num_items);
IRNode *ir_new_set(EastType *type, IRNode **items, size_t num_items);
IRNode *ir_new_dict(EastType *type, IRNode **keys, IRNode **values, size_t num_pairs);
IRNode *ir_new_ref(EastType *type, IRNode *value);
IRNode *ir_new_vector(EastType *type, IRNode **items, size_t num_items);
IRNode *ir_struct(EastType *type, char **field_names, IRNode **field_values, size_t num_fields);
IRNode *ir_get_field(EastType *type, IRNode *expr, const char *field_name);
IRNode *ir_variant(EastType *type, const char *case_name, IRNode *value);
IRNode *ir_wrap_recursive(EastType *type, IRNode *value);
IRNode *ir_unwrap_recursive(EastType *type, IRNode *value);

// Ref counting
void ir_node_retain(IRNode *node);
void ir_node_release(IRNode *node);

// Location management
void ir_node_set_location(IRNode *node, const EastLocation *locs, size_t num_locs);
EastLocation *east_locations_dup(const EastLocation *src, size_t count);
void east_locations_free(EastLocation *locs, size_t count);

#endif
