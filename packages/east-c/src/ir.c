#include "east/ir.h"

#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                    */
/* ------------------------------------------------------------------ */

static IRNode *ir_alloc(IRNodeKind kind, EastType *type) {
    IRNode *node = calloc(1, sizeof(IRNode));
    if (!node) return NULL;
    node->kind = kind;
    node->ref_count = 1;
    node->type = type;
    if (type) east_type_retain(type);
    return node;
}

static IRVariable ir_variable_dup(const IRVariable *src) {
    IRVariable v;
    v.name = src->name ? strdup(src->name) : NULL;
    v.mutable = src->mutable;
    v.captured = src->captured;
    return v;
}

static void ir_variable_free(IRVariable *v) {
    free(v->name);
    v->name = NULL;
}

/* Duplicate an array of IRVariable structs (deep-copies names). */
static IRVariable *ir_variables_dup(const IRVariable *src, size_t count) {
    if (count == 0 || !src) return NULL;
    IRVariable *dst = malloc(count * sizeof(IRVariable));
    if (!dst) return NULL;
    for (size_t i = 0; i < count; i++) {
        dst[i] = ir_variable_dup(&src[i]);
    }
    return dst;
}

/* Duplicate an array of IRNode pointers, retaining each. */
static IRNode **ir_nodes_dup(IRNode **src, size_t count) {
    if (count == 0 || !src) return NULL;
    IRNode **dst = malloc(count * sizeof(IRNode *));
    if (!dst) return NULL;
    for (size_t i = 0; i < count; i++) {
        dst[i] = src[i];
        if (dst[i]) ir_node_retain(dst[i]);
    }
    return dst;
}

/* Duplicate an array of EastType pointers, retaining each. */
static EastType **ir_types_dup(EastType **src, size_t count) {
    if (count == 0 || !src) return NULL;
    EastType **dst = malloc(count * sizeof(EastType *));
    if (!dst) return NULL;
    for (size_t i = 0; i < count; i++) {
        dst[i] = src[i];
        if (dst[i]) east_type_retain(dst[i]);
    }
    return dst;
}

/* ------------------------------------------------------------------ */
/*  Builder functions                                                   */
/* ------------------------------------------------------------------ */

IRNode *ir_value(EastType *type, EastValue *value) {
    IRNode *n = ir_alloc(IR_VALUE, type);
    if (!n) return NULL;
    n->data.value.value = value;
    if (value) east_value_retain(value);
    return n;
}

IRNode *ir_variable(EastType *type, const char *name, bool mutable, bool captured) {
    IRNode *n = ir_alloc(IR_VARIABLE, type);
    if (!n) return NULL;
    n->data.variable.name = name ? strdup(name) : NULL;
    n->data.variable.mutable = mutable;
    n->data.variable.captured = captured;
    return n;
}

IRNode *ir_let(EastType *type, const char *var_name, bool mutable, bool captured, IRNode *value) {
    IRNode *n = ir_alloc(IR_LET, type);
    if (!n) return NULL;
    n->data.let.var.name = var_name ? strdup(var_name) : NULL;
    n->data.let.var.mutable = mutable;
    n->data.let.var.captured = captured;
    n->data.let.value = value;
    if (value) ir_node_retain(value);
    return n;
}

IRNode *ir_assign(EastType *type, const char *name, IRNode *value) {
    IRNode *n = ir_alloc(IR_ASSIGN, type);
    if (!n) return NULL;
    n->data.assign.name = name ? strdup(name) : NULL;
    n->data.assign.value = value;
    if (value) ir_node_retain(value);
    return n;
}

IRNode *ir_block(EastType *type, IRNode **stmts, size_t num_stmts) {
    IRNode *n = ir_alloc(IR_BLOCK, type);
    if (!n) return NULL;
    n->data.block.stmts = ir_nodes_dup(stmts, num_stmts);
    n->data.block.num_stmts = num_stmts;
    return n;
}

IRNode *ir_if_else(EastType *type, IRNode *cond, IRNode *then_b, IRNode *else_b) {
    IRNode *n = ir_alloc(IR_IF_ELSE, type);
    if (!n) return NULL;
    n->data.if_else.cond = cond;
    if (cond) ir_node_retain(cond);
    n->data.if_else.then_branch = then_b;
    if (then_b) ir_node_retain(then_b);
    n->data.if_else.else_branch = else_b;
    if (else_b) ir_node_retain(else_b);
    return n;
}

IRNode *ir_match(EastType *type, IRNode *expr, IRMatchCase *cases, size_t num_cases) {
    IRNode *n = ir_alloc(IR_MATCH, type);
    if (!n) return NULL;
    n->data.match.expr = expr;
    if (expr) ir_node_retain(expr);
    n->data.match.num_cases = num_cases;
    if (num_cases > 0 && cases) {
        n->data.match.cases = malloc(num_cases * sizeof(IRMatchCase));
        for (size_t i = 0; i < num_cases; i++) {
            n->data.match.cases[i].case_name =
                cases[i].case_name ? strdup(cases[i].case_name) : NULL;
            n->data.match.cases[i].bind_name =
                cases[i].bind_name ? strdup(cases[i].bind_name) : NULL;
            n->data.match.cases[i].body = cases[i].body;
            if (cases[i].body) ir_node_retain(cases[i].body);
        }
    } else {
        n->data.match.cases = NULL;
    }
    return n;
}

IRNode *ir_while(EastType *type, IRNode *cond, IRNode *body, const char *label) {
    IRNode *n = ir_alloc(IR_WHILE, type);
    if (!n) return NULL;
    n->data.while_.cond = cond;
    if (cond) ir_node_retain(cond);
    n->data.while_.body = body;
    if (body) ir_node_retain(body);
    n->data.while_.label = label ? strdup(label) : NULL;
    return n;
}

IRNode *ir_for_array(EastType *type, const char *var, const char *idx,
                     IRNode *array, IRNode *body, const char *label) {
    IRNode *n = ir_alloc(IR_FOR_ARRAY, type);
    if (!n) return NULL;
    n->data.for_array.var_name = var ? strdup(var) : NULL;
    n->data.for_array.index_name = idx ? strdup(idx) : NULL;
    n->data.for_array.array = array;
    if (array) ir_node_retain(array);
    n->data.for_array.body = body;
    if (body) ir_node_retain(body);
    n->data.for_array.label = label ? strdup(label) : NULL;
    return n;
}

IRNode *ir_for_set(EastType *type, const char *var, IRNode *set,
                   IRNode *body, const char *label) {
    IRNode *n = ir_alloc(IR_FOR_SET, type);
    if (!n) return NULL;
    n->data.for_set.var_name = var ? strdup(var) : NULL;
    n->data.for_set.set = set;
    if (set) ir_node_retain(set);
    n->data.for_set.body = body;
    if (body) ir_node_retain(body);
    n->data.for_set.label = label ? strdup(label) : NULL;
    return n;
}

IRNode *ir_for_dict(EastType *type, const char *key, const char *val,
                    IRNode *dict, IRNode *body, const char *label) {
    IRNode *n = ir_alloc(IR_FOR_DICT, type);
    if (!n) return NULL;
    n->data.for_dict.key_name = key ? strdup(key) : NULL;
    n->data.for_dict.val_name = val ? strdup(val) : NULL;
    n->data.for_dict.dict = dict;
    if (dict) ir_node_retain(dict);
    n->data.for_dict.body = body;
    if (body) ir_node_retain(body);
    n->data.for_dict.label = label ? strdup(label) : NULL;
    return n;
}

static IRNode *ir_function_impl(IRNodeKind kind, EastType *type,
                                IRVariable *captures, size_t num_captures,
                                IRVariable *params, size_t num_params,
                                IRNode *body) {
    IRNode *n = ir_alloc(kind, type);
    if (!n) return NULL;
    n->data.function.captures = ir_variables_dup(captures, num_captures);
    n->data.function.num_captures = num_captures;
    n->data.function.params = ir_variables_dup(params, num_params);
    n->data.function.num_params = num_params;
    n->data.function.body = body;
    if (body) ir_node_retain(body);
    return n;
}

IRNode *ir_function(EastType *type, IRVariable *captures, size_t num_captures,
                    IRVariable *params, size_t num_params, IRNode *body) {
    return ir_function_impl(IR_FUNCTION, type, captures, num_captures,
                            params, num_params, body);
}

IRNode *ir_async_function(EastType *type, IRVariable *captures, size_t num_captures,
                          IRVariable *params, size_t num_params, IRNode *body) {
    return ir_function_impl(IR_ASYNC_FUNCTION, type, captures, num_captures,
                            params, num_params, body);
}

static IRNode *ir_call_impl(IRNodeKind kind, EastType *type, IRNode *func,
                            IRNode **args, size_t num_args) {
    IRNode *n = ir_alloc(kind, type);
    if (!n) return NULL;
    n->data.call.func = func;
    if (func) ir_node_retain(func);
    n->data.call.args = ir_nodes_dup(args, num_args);
    n->data.call.num_args = num_args;
    return n;
}

IRNode *ir_call(EastType *type, IRNode *func, IRNode **args, size_t num_args) {
    return ir_call_impl(IR_CALL, type, func, args, num_args);
}

IRNode *ir_call_async(EastType *type, IRNode *func, IRNode **args, size_t num_args) {
    return ir_call_impl(IR_CALL_ASYNC, type, func, args, num_args);
}

IRNode *ir_platform(EastType *type, const char *name,
                    EastType **type_params, size_t num_tp,
                    IRNode **args, size_t num_args, bool is_async) {
    IRNode *n = ir_alloc(IR_PLATFORM, type);
    if (!n) return NULL;
    n->data.platform.name = name ? strdup(name) : NULL;
    n->data.platform.type_params = ir_types_dup(type_params, num_tp);
    n->data.platform.num_type_params = num_tp;
    n->data.platform.args = ir_nodes_dup(args, num_args);
    n->data.platform.num_args = num_args;
    n->data.platform.is_async = is_async;
    return n;
}

IRNode *ir_builtin(EastType *type, const char *name,
                   EastType **type_params, size_t num_tp,
                   IRNode **args, size_t num_args) {
    IRNode *n = ir_alloc(IR_BUILTIN, type);
    if (!n) return NULL;
    n->data.builtin.name = name ? strdup(name) : NULL;
    n->data.builtin.type_params = ir_types_dup(type_params, num_tp);
    n->data.builtin.num_type_params = num_tp;
    n->data.builtin.args = ir_nodes_dup(args, num_args);
    n->data.builtin.num_args = num_args;
    return n;
}

IRNode *ir_return(EastType *type, IRNode *value) {
    IRNode *n = ir_alloc(IR_RETURN, type);
    if (!n) return NULL;
    n->data.return_.value = value;
    if (value) ir_node_retain(value);
    return n;
}

IRNode *ir_break(const char *label) {
    IRNode *n = ir_alloc(IR_BREAK, NULL);
    if (!n) return NULL;
    n->data.loop_ctrl.label = label ? strdup(label) : NULL;
    return n;
}

IRNode *ir_continue(const char *label) {
    IRNode *n = ir_alloc(IR_CONTINUE, NULL);
    if (!n) return NULL;
    n->data.loop_ctrl.label = label ? strdup(label) : NULL;
    return n;
}

IRNode *ir_error(EastType *type, IRNode *message) {
    IRNode *n = ir_alloc(IR_ERROR, type);
    if (!n) return NULL;
    n->data.error.message = message;
    if (message) ir_node_retain(message);
    return n;
}

IRNode *ir_try_catch(EastType *type, IRNode *try_body,
                     const char *message_var, const char *stack_var,
                     IRNode *catch_body, IRNode *finally_body) {
    IRNode *n = ir_alloc(IR_TRY_CATCH, type);
    if (!n) return NULL;
    n->data.try_catch.try_body = try_body;
    if (try_body) ir_node_retain(try_body);
    n->data.try_catch.message_var = message_var ? strdup(message_var) : NULL;
    n->data.try_catch.stack_var = stack_var ? strdup(stack_var) : NULL;
    n->data.try_catch.catch_body = catch_body;
    if (catch_body) ir_node_retain(catch_body);
    n->data.try_catch.finally_body = finally_body;
    if (finally_body) ir_node_retain(finally_body);
    return n;
}

static IRNode *ir_new_collection_impl(IRNodeKind kind, EastType *type,
                                      IRNode **items, size_t num_items) {
    IRNode *n = ir_alloc(kind, type);
    if (!n) return NULL;
    n->data.new_collection.items = ir_nodes_dup(items, num_items);
    n->data.new_collection.num_items = num_items;
    return n;
}

IRNode *ir_new_array(EastType *type, IRNode **items, size_t num_items) {
    return ir_new_collection_impl(IR_NEW_ARRAY, type, items, num_items);
}

IRNode *ir_new_set(EastType *type, IRNode **items, size_t num_items) {
    return ir_new_collection_impl(IR_NEW_SET, type, items, num_items);
}

IRNode *ir_new_dict(EastType *type, IRNode **keys, IRNode **values,
                    size_t num_pairs) {
    IRNode *n = ir_alloc(IR_NEW_DICT, type);
    if (!n) return NULL;
    n->data.new_dict.keys = ir_nodes_dup(keys, num_pairs);
    n->data.new_dict.values = ir_nodes_dup(values, num_pairs);
    n->data.new_dict.num_pairs = num_pairs;
    return n;
}

IRNode *ir_new_ref(EastType *type, IRNode *value) {
    IRNode *n = ir_alloc(IR_NEW_REF, type);
    if (!n) return NULL;
    n->data.new_ref.value = value;
    if (value) ir_node_retain(value);
    return n;
}

IRNode *ir_new_vector(EastType *type, IRNode **items, size_t num_items) {
    IRNode *n = ir_alloc(IR_NEW_VECTOR, type);
    if (!n) return NULL;
    n->data.new_vector.items = ir_nodes_dup(items, num_items);
    n->data.new_vector.num_items = num_items;
    return n;
}

IRNode *ir_struct(EastType *type, char **field_names, IRNode **field_values,
                  size_t num_fields) {
    IRNode *n = ir_alloc(IR_STRUCT, type);
    if (!n) return NULL;
    n->data.struct_.num_fields = num_fields;
    if (num_fields > 0) {
        n->data.struct_.field_names = malloc(num_fields * sizeof(char *));
        for (size_t i = 0; i < num_fields; i++) {
            n->data.struct_.field_names[i] =
                field_names[i] ? strdup(field_names[i]) : NULL;
        }
        n->data.struct_.field_values = ir_nodes_dup(field_values, num_fields);
    } else {
        n->data.struct_.field_names = NULL;
        n->data.struct_.field_values = NULL;
    }
    return n;
}

IRNode *ir_get_field(EastType *type, IRNode *expr, const char *field_name) {
    IRNode *n = ir_alloc(IR_GET_FIELD, type);
    if (!n) return NULL;
    n->data.get_field.expr = expr;
    if (expr) ir_node_retain(expr);
    n->data.get_field.field_name = field_name ? strdup(field_name) : NULL;
    return n;
}

IRNode *ir_variant(EastType *type, const char *case_name, IRNode *value) {
    IRNode *n = ir_alloc(IR_VARIANT, type);
    if (!n) return NULL;
    n->data.variant.case_name = case_name ? strdup(case_name) : NULL;
    n->data.variant.value = value;
    if (value) ir_node_retain(value);
    return n;
}

IRNode *ir_wrap_recursive(EastType *type, IRNode *value) {
    IRNode *n = ir_alloc(IR_WRAP_RECURSIVE, type);
    if (!n) return NULL;
    n->data.recursive.value = value;
    if (value) ir_node_retain(value);
    return n;
}

IRNode *ir_unwrap_recursive(EastType *type, IRNode *value) {
    IRNode *n = ir_alloc(IR_UNWRAP_RECURSIVE, type);
    if (!n) return NULL;
    n->data.recursive.value = value;
    if (value) ir_node_retain(value);
    return n;
}

/* ------------------------------------------------------------------ */
/*  Location management                                                 */
/* ------------------------------------------------------------------ */

EastLocation *east_locations_dup(const EastLocation *src, size_t count) {
    if (!src || count == 0) return NULL;
    EastLocation *dst = malloc(count * sizeof(EastLocation));
    if (!dst) return NULL;
    for (size_t i = 0; i < count; i++) {
        dst[i].filename = src[i].filename ? strdup(src[i].filename) : NULL;
        dst[i].line = src[i].line;
        dst[i].column = src[i].column;
    }
    return dst;
}

void east_locations_free(EastLocation *locs, size_t count) {
    if (!locs) return;
    for (size_t i = 0; i < count; i++) {
        free(locs[i].filename);
    }
    free(locs);
}

void ir_node_set_location(IRNode *node, const EastLocation *locs, size_t num_locs) {
    if (!node) return;
    east_locations_free(node->locations, node->num_locations);
    node->locations = east_locations_dup(locs, num_locs);
    node->num_locations = num_locs;
}

/* ------------------------------------------------------------------ */
/*  Ref counting                                                        */
/* ------------------------------------------------------------------ */

void ir_node_retain(IRNode *node) {
    if (node) node->ref_count++;
}

void ir_node_release(IRNode *node) {
    if (!node) return;
    if (--node->ref_count > 0) return;

    /* Release the node type. */
    if (node->type) {
        east_type_release(node->type);
    }

    /* Release kind-specific data. */
    switch (node->kind) {
    case IR_VALUE:
        if (node->data.value.value)
            east_value_release(node->data.value.value);
        break;

    case IR_VARIABLE:
        free(node->data.variable.name);
        break;

    case IR_LET:
        ir_variable_free(&node->data.let.var);
        ir_node_release(node->data.let.value);
        break;

    case IR_ASSIGN:
        free(node->data.assign.name);
        ir_node_release(node->data.assign.value);
        break;

    case IR_BLOCK:
        for (size_t i = 0; i < node->data.block.num_stmts; i++) {
            ir_node_release(node->data.block.stmts[i]);
        }
        free(node->data.block.stmts);
        break;

    case IR_IF_ELSE:
        ir_node_release(node->data.if_else.cond);
        ir_node_release(node->data.if_else.then_branch);
        ir_node_release(node->data.if_else.else_branch);
        break;

    case IR_MATCH:
        ir_node_release(node->data.match.expr);
        for (size_t i = 0; i < node->data.match.num_cases; i++) {
            free(node->data.match.cases[i].case_name);
            free(node->data.match.cases[i].bind_name);
            ir_node_release(node->data.match.cases[i].body);
        }
        free(node->data.match.cases);
        break;

    case IR_WHILE:
        ir_node_release(node->data.while_.cond);
        ir_node_release(node->data.while_.body);
        free(node->data.while_.label);
        break;

    case IR_FOR_ARRAY:
        free(node->data.for_array.var_name);
        free(node->data.for_array.index_name);
        ir_node_release(node->data.for_array.array);
        ir_node_release(node->data.for_array.body);
        free(node->data.for_array.label);
        break;

    case IR_FOR_SET:
        free(node->data.for_set.var_name);
        ir_node_release(node->data.for_set.set);
        ir_node_release(node->data.for_set.body);
        free(node->data.for_set.label);
        break;

    case IR_FOR_DICT:
        free(node->data.for_dict.key_name);
        free(node->data.for_dict.val_name);
        ir_node_release(node->data.for_dict.dict);
        ir_node_release(node->data.for_dict.body);
        free(node->data.for_dict.label);
        break;

    case IR_FUNCTION:
    case IR_ASYNC_FUNCTION:
        for (size_t i = 0; i < node->data.function.num_captures; i++) {
            ir_variable_free(&node->data.function.captures[i]);
        }
        free(node->data.function.captures);
        for (size_t i = 0; i < node->data.function.num_params; i++) {
            ir_variable_free(&node->data.function.params[i]);
        }
        free(node->data.function.params);
        ir_node_release(node->data.function.body);
        if (node->data.function.source_ir) {
            east_value_release(node->data.function.source_ir);
        }
        break;

    case IR_CALL:
    case IR_CALL_ASYNC:
        ir_node_release(node->data.call.func);
        for (size_t i = 0; i < node->data.call.num_args; i++) {
            ir_node_release(node->data.call.args[i]);
        }
        free(node->data.call.args);
        break;

    case IR_PLATFORM:
        free(node->data.platform.name);
        for (size_t i = 0; i < node->data.platform.num_type_params; i++) {
            east_type_release(node->data.platform.type_params[i]);
        }
        free(node->data.platform.type_params);
        for (size_t i = 0; i < node->data.platform.num_args; i++) {
            ir_node_release(node->data.platform.args[i]);
        }
        free(node->data.platform.args);
        break;

    case IR_BUILTIN:
        free(node->data.builtin.name);
        for (size_t i = 0; i < node->data.builtin.num_type_params; i++) {
            east_type_release(node->data.builtin.type_params[i]);
        }
        free(node->data.builtin.type_params);
        for (size_t i = 0; i < node->data.builtin.num_args; i++) {
            ir_node_release(node->data.builtin.args[i]);
        }
        free(node->data.builtin.args);
        break;

    case IR_RETURN:
        ir_node_release(node->data.return_.value);
        break;

    case IR_BREAK:
    case IR_CONTINUE:
        free(node->data.loop_ctrl.label);
        break;

    case IR_ERROR:
        ir_node_release(node->data.error.message);
        break;

    case IR_TRY_CATCH:
        ir_node_release(node->data.try_catch.try_body);
        free(node->data.try_catch.message_var);
        free(node->data.try_catch.stack_var);
        ir_node_release(node->data.try_catch.catch_body);
        ir_node_release(node->data.try_catch.finally_body);
        break;

    case IR_NEW_ARRAY:
    case IR_NEW_SET:
        for (size_t i = 0; i < node->data.new_collection.num_items; i++) {
            ir_node_release(node->data.new_collection.items[i]);
        }
        free(node->data.new_collection.items);
        break;

    case IR_NEW_DICT:
        for (size_t i = 0; i < node->data.new_dict.num_pairs; i++) {
            ir_node_release(node->data.new_dict.keys[i]);
            ir_node_release(node->data.new_dict.values[i]);
        }
        free(node->data.new_dict.keys);
        free(node->data.new_dict.values);
        break;

    case IR_NEW_REF:
        ir_node_release(node->data.new_ref.value);
        break;

    case IR_NEW_VECTOR:
        for (size_t i = 0; i < node->data.new_vector.num_items; i++) {
            ir_node_release(node->data.new_vector.items[i]);
        }
        free(node->data.new_vector.items);
        break;

    case IR_STRUCT:
        for (size_t i = 0; i < node->data.struct_.num_fields; i++) {
            free(node->data.struct_.field_names[i]);
            ir_node_release(node->data.struct_.field_values[i]);
        }
        free(node->data.struct_.field_names);
        free(node->data.struct_.field_values);
        break;

    case IR_GET_FIELD:
        ir_node_release(node->data.get_field.expr);
        free(node->data.get_field.field_name);
        break;

    case IR_VARIANT:
        free(node->data.variant.case_name);
        ir_node_release(node->data.variant.value);
        break;

    case IR_WRAP_RECURSIVE:
    case IR_UNWRAP_RECURSIVE:
        ir_node_release(node->data.recursive.value);
        break;
    }

    east_locations_free(node->locations, node->num_locations);
    free(node);
}
