/*
 * Type descriptors for East's homoiconic type system.
 *
 * Defines EastTypeType, LiteralValueType, and IRType as East types,
 * plus conversion functions from decoded variant values back to
 * native C EastType* and IRNode*.
 *
 * Mirrors type_of_type.ts / type_of_type.py in the reference implementations.
 */

#include "east/type_of_type.h"
#include "east/types.h"
#include "east/values.h"
#include "east/ir.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/*  Global type descriptors                                            */
/* ================================================================== */

EastType *east_type_type = NULL;
EastType *east_literal_value_type = NULL;
EastType *east_ir_type = NULL;

/* ================================================================== */
/*  Helper: build struct type with sorted fields                       */
/* ================================================================== */

static EastType *make_struct2(const char *n1, EastType *t1,
                              const char *n2, EastType *t2)
{
    const char *names[] = {n1, n2};
    EastType *types[] = {t1, t2};
    return east_struct_type(names, types, 2);
}

static EastType *make_struct3(const char *n1, EastType *t1,
                              const char *n2, EastType *t2,
                              const char *n3, EastType *t3)
{
    const char *names[] = {n1, n2, n3};
    EastType *types[] = {t1, t2, t3};
    return east_struct_type(names, types, 3);
}

/* ================================================================== */
/*  east_type_of_type_init                                             */
/* ================================================================== */

void east_type_of_type_init(void)
{
    if (east_type_type != NULL) return; /* already initialized */

    /* ============================================================== */
    /*  LiteralValueType                                               */
    /*  VariantType({ Null, Boolean, Integer, Float, String,           */
    /*                DateTime, Blob })                                 */
    /* ============================================================== */
    {
        /* Case order must match TypeScript declaration order exactly,
         * because beast2 encodes variant case indices numerically. */
        const char *names[] = {
            "Null", "Boolean", "Integer", "Float",
            "String", "DateTime", "Blob"
        };
        EastType *types[] = {
            &east_null_type, &east_boolean_type, &east_integer_type,
            &east_float_type, &east_string_type, &east_datetime_type,
            &east_blob_type
        };
        east_literal_value_type = east_variant_type(names, types, 7);
    }

    /* ============================================================== */
    /*  EastTypeType                                                    */
    /*  RecursiveType(self => VariantType({ ... 19 cases ... }))       */
    /* ============================================================== */
    {
        EastType *rec = east_recursive_type_new();

        /* Build helper types using self-reference */
        EastType *dict_payload = make_struct2("key", rec, "value", rec);
        EastType *field_struct = make_struct2("name", &east_string_type, "type", rec);
        EastType *field_array = east_array_type(field_struct);
        EastType *rec_arr = east_array_type(rec);
        EastType *func_struct = make_struct2("inputs", rec_arr, "output", rec);

        /* Case order must match TypeScript declaration order exactly,
         * because beast2 encodes variant case indices numerically. */
        const char *names[] = {
            "Never", "Null", "Boolean", "Integer", "Float",
            "String", "DateTime", "Blob", "Ref", "Array",
            "Set", "Dict", "Struct", "Variant", "Recursive",
            "Function", "AsyncFunction", "Vector", "Matrix"
        };
        EastType *types[] = {
            &east_null_type,/* Never -> Null */
            &east_null_type,/* Null -> Null */
            &east_null_type,/* Boolean -> Null */
            &east_null_type,/* Integer -> Null */
            &east_null_type,/* Float -> Null */
            &east_null_type,/* String -> Null */
            &east_null_type,/* DateTime -> Null */
            &east_null_type,/* Blob -> Null */
            rec,            /* Ref -> self (inner type) */
            rec,            /* Array -> self (element type) */
            rec,            /* Set -> self (element type) */
            dict_payload,   /* Dict -> {key: self, value: self} */
            field_array,    /* Struct -> [{name: String, type: self}] */
            field_array,    /* Variant -> [{name: String, type: self}] */
            &east_integer_type, /* Recursive -> Integer (depth) */
            func_struct,    /* Function -> {inputs: [self], output: self} */
            func_struct,    /* AsyncFunction -> {inputs: [self], output: self} */
            rec,            /* Vector -> self (element type) */
            rec,            /* Matrix -> self (element type) */
        };

        EastType *inner = east_variant_type(names, types, 19);
        east_recursive_type_set(rec, inner);
        east_type_type = rec;

        /* Release temporaries (rec holds refs via inner) */
        east_type_release(dict_payload);
        east_type_release(field_struct);
        east_type_release(field_array);
        east_type_release(func_struct);
        east_type_release(rec_arr);
        /* NOTE: inner is owned by recursive wrapper rec (not retained
         * to avoid cycles), so we must NOT release it here. */
    }

    /* ============================================================== */
    /*  IRType                                                          */
    /*  RecursiveType(self => VariantType({ ... 34 cases ... }))       */
    /* ============================================================== */
    {
        EastType *ir = east_recursive_type_new();
        EastType *tt = east_type_type;
        EastType *lv = east_literal_value_type;

        /* Shared sub-types.
         * Field order must match TypeScript declaration order exactly,
         * because beast2 encodes struct fields positionally. */

        /* LocationType: { filename, line, column } */
        EastType *loc_struct = make_struct3(
            "filename", &east_string_type,
            "line", &east_integer_type,
            "column", &east_integer_type
        );
        EastType *loc_arr = east_array_type(loc_struct);
        EastType *ir_arr = east_array_type(ir);
        EastType *tt_arr = east_array_type(tt);

        /* IRLabelType: { name, location } */
        EastType *ir_label = make_struct2("name", &east_string_type, "location", loc_arr);

        /* Dict entry: { key, value } */
        EastType *kv_struct = make_struct2("key", ir, "value", ir);
        EastType *kv_arr = east_array_type(kv_struct);

        /* Struct field: { name, value } */
        EastType *sf_struct = make_struct2("name", &east_string_type, "value", ir);
        EastType *sf_arr = east_array_type(sf_struct);

        /* IfElse branch: { predicate, body } */
        EastType *if_branch = make_struct2("predicate", ir, "body", ir);
        EastType *if_arr = east_array_type(if_branch);

        /* Match case: { case, variable, body } */
        EastType *match_case = make_struct3("case", &east_string_type, "variable", ir, "body", ir);
        EastType *match_arr = east_array_type(match_case);

        /* Now build each IR case struct.
         * All cases have: type (EastTypeType), location ([Location])
         * plus case-specific fields.
         *
         * Helper for building struct types with 4+ fields. */
        #define S4(n1,t1,n2,t2,n3,t3,n4,t4) \
            east_struct_type((const char*[]){n1,n2,n3,n4}, (EastType*[]){t1,t2,t3,t4}, 4)
        #define S5(n1,t1,n2,t2,n3,t3,n4,t4,n5,t5) \
            east_struct_type((const char*[]){n1,n2,n3,n4,n5}, (EastType*[]){t1,t2,t3,t4,t5}, 5)
        #define S6(n1,t1,n2,t2,n3,t3,n4,t4,n5,t5,n6,t6) \
            east_struct_type((const char*[]){n1,n2,n3,n4,n5,n6}, (EastType*[]){t1,t2,t3,t4,t5,t6}, 6)
        #define S7(n1,t1,n2,t2,n3,t3,n4,t4,n5,t5,n6,t6,n7,t7) \
            east_struct_type((const char*[]){n1,n2,n3,n4,n5,n6,n7}, (EastType*[]){t1,t2,t3,t4,t5,t6,t7}, 7)

        /* All field orderings must match TypeScript declaration order exactly. */
        EastType *c_error         = make_struct3("type",tt, "location",loc_arr, "message",ir);
        EastType *c_try_catch     = S7("type",tt, "location",loc_arr, "try_body",ir, "catch_body",ir, "message",ir, "stack",ir, "finally_body",ir);
        EastType *c_value         = make_struct3("type",tt, "location",loc_arr, "value",lv);
        EastType *c_variable      = S5("type",tt, "location",loc_arr, "name",&east_string_type, "mutable",&east_boolean_type, "captured",&east_boolean_type);
        EastType *c_let           = S4("type",tt, "location",loc_arr, "variable",ir, "value",ir);
        EastType *c_assign        = S4("type",tt, "location",loc_arr, "variable",ir, "value",ir);
        EastType *c_as            = make_struct3("type",tt, "location",loc_arr, "value",ir);
        EastType *c_function      = S5("type",tt, "location",loc_arr, "captures",ir_arr, "parameters",ir_arr, "body",ir);
        EastType *c_async_fn      = c_function; east_type_retain(c_async_fn);
        EastType *c_call          = S4("type",tt, "location",loc_arr, "function",ir, "arguments",ir_arr);
        EastType *c_call_async    = c_call; east_type_retain(c_call_async);
        EastType *c_new_ref       = make_struct3("type",tt, "location",loc_arr, "value",ir);
        EastType *c_new_array     = make_struct3("type",tt, "location",loc_arr, "values",ir_arr);
        EastType *c_new_set       = make_struct3("type",tt, "location",loc_arr, "values",ir_arr);
        EastType *c_new_dict      = make_struct3("type",tt, "location",loc_arr, "values",kv_arr);
        EastType *c_new_vector    = make_struct3("type",tt, "location",loc_arr, "values",ir_arr);
        EastType *c_new_matrix    = S5("type",tt, "location",loc_arr, "values",ir_arr, "rows",&east_integer_type, "cols",&east_integer_type);
        EastType *c_struct        = make_struct3("type",tt, "location",loc_arr, "fields",sf_arr);
        EastType *c_get_field     = S4("type",tt, "location",loc_arr, "field",&east_string_type, "struct",ir);
        EastType *c_variant       = S4("type",tt, "location",loc_arr, "case",&east_string_type, "value",ir);
        EastType *c_block         = make_struct3("type",tt, "location",loc_arr, "statements",ir_arr);
        EastType *c_if_else       = S4("type",tt, "location",loc_arr, "ifs",if_arr, "else_body",ir);
        EastType *c_match         = S4("type",tt, "location",loc_arr, "variant",ir, "cases",match_arr);
        EastType *c_unwrap        = make_struct3("type",tt, "location",loc_arr, "value",ir);
        EastType *c_wrap          = make_struct3("type",tt, "location",loc_arr, "value",ir);
        EastType *c_while         = S5("type",tt, "location",loc_arr, "predicate",ir, "label",ir_label, "body",ir);
        EastType *c_for_array     = S7("type",tt, "location",loc_arr, "array",ir, "label",ir_label, "key",ir, "value",ir, "body",ir);
        EastType *c_for_set       = S6("type",tt, "location",loc_arr, "set",ir, "label",ir_label, "key",ir, "body",ir);
        EastType *c_for_dict      = S7("type",tt, "location",loc_arr, "dict",ir, "label",ir_label, "key",ir, "value",ir, "body",ir);
        EastType *c_return        = make_struct3("type",tt, "location",loc_arr, "value",ir);
        EastType *c_continue      = make_struct3("type",tt, "location",loc_arr, "label",ir_label);
        EastType *c_break         = make_struct3("type",tt, "location",loc_arr, "label",ir_label);
        EastType *c_builtin       = S5("type",tt, "location",loc_arr, "builtin",&east_string_type, "type_parameters",tt_arr, "arguments",ir_arr);
        EastType *c_platform      = S7("type",tt, "location",loc_arr, "name",&east_string_type, "type_parameters",tt_arr, "arguments",ir_arr, "async",&east_boolean_type, "optional",&east_boolean_type);

        /* Case order must match TypeScript declaration order exactly,
         * because beast2 encodes variant case indices numerically. */
        const char *ir_names[] = {
            "Error", "TryCatch", "Value", "Variable", "Let",
            "Assign", "As", "Function", "AsyncFunction", "Call",
            "CallAsync", "NewRef", "NewArray", "NewSet", "NewDict",
            "NewVector", "NewMatrix", "Struct", "GetField", "Variant",
            "Block", "IfElse", "Match", "UnwrapRecursive", "WrapRecursive",
            "While", "ForArray", "ForSet", "ForDict", "Return",
            "Continue", "Break", "Builtin", "Platform"
        };
        EastType *ir_types[] = {
            c_error, c_try_catch, c_value, c_variable, c_let,
            c_assign, c_as, c_function, c_async_fn, c_call,
            c_call_async, c_new_ref, c_new_array, c_new_set, c_new_dict,
            c_new_vector, c_new_matrix, c_struct, c_get_field, c_variant,
            c_block, c_if_else, c_match, c_unwrap, c_wrap,
            c_while, c_for_array, c_for_set, c_for_dict, c_return,
            c_continue, c_break, c_builtin, c_platform
        };

        EastType *ir_inner = east_variant_type(ir_names, ir_types, 34);
        east_recursive_type_set(ir, ir_inner);
        east_ir_type = ir;

        #undef S4
        #undef S5
        #undef S6
        #undef S7

        /* Release shared sub-types */
        east_type_release(loc_struct);
        east_type_release(loc_arr);
        east_type_release(ir_arr);
        east_type_release(tt_arr);
        east_type_release(ir_label);
        east_type_release(kv_struct);
        east_type_release(kv_arr);
        east_type_release(sf_struct);
        east_type_release(sf_arr);
        east_type_release(if_branch);
        east_type_release(if_arr);
        east_type_release(match_case);
        east_type_release(match_arr);
        /* NOTE: ir_inner is owned by the recursive wrapper ir.
         * east_recursive_type_set does NOT retain to avoid cycles,
         * so we must NOT release it here either. */

        /* Release case structs */
        east_type_release(c_as);
        east_type_release(c_assign);
        east_type_release(c_async_fn);
        east_type_release(c_block);
        east_type_release(c_break);
        east_type_release(c_builtin);
        east_type_release(c_call);
        east_type_release(c_call_async);
        east_type_release(c_continue);
        east_type_release(c_error);
        east_type_release(c_for_array);
        east_type_release(c_for_dict);
        east_type_release(c_for_set);
        east_type_release(c_function);
        east_type_release(c_get_field);
        east_type_release(c_if_else);
        east_type_release(c_let);
        east_type_release(c_match);
        east_type_release(c_new_array);
        east_type_release(c_new_dict);
        east_type_release(c_new_matrix);
        east_type_release(c_new_ref);
        east_type_release(c_new_set);
        east_type_release(c_new_vector);
        east_type_release(c_platform);
        east_type_release(c_return);
        east_type_release(c_struct);
        east_type_release(c_try_catch);
        east_type_release(c_unwrap);
        east_type_release(c_value);
        east_type_release(c_variable);
        east_type_release(c_variant);
        east_type_release(c_while);
        east_type_release(c_wrap);
    }
}

/* ================================================================== */
/*  east_type_from_value_ctx                                           */
/*                                                                     */
/*  Internal helper: converts a decoded EastTypeType variant value to  */
/*  EastType*, tracking recursive wrappers in a stack so that          */
/*  Recursive(depth) self-references resolve correctly.                */
/* ================================================================== */

typedef struct {
    EastType **items;
    int len;
    int cap;
} RecStack;

static void rec_stack_push(RecStack *s, EastType *t) {
    if (s->len >= s->cap) {
        int new_cap = s->cap ? s->cap * 2 : 4;
        EastType **new_items = realloc(s->items, new_cap * sizeof(EastType *));
        if (!new_items) return;
        s->items = new_items;
        s->cap = new_cap;
    }
    s->items[s->len++] = t;
}

static EastType *east_type_from_value_ctx(EastValue *v, RecStack *rec_stack)
{
    if (!v || v->kind != EAST_VAL_VARIANT) return NULL;

    const char *tag = v->data.variant.case_name;
    EastValue *payload = v->data.variant.value;

    /* Primitive types (payload is null) */
    if (strcmp(tag, "Never") == 0)    return &east_never_type;
    if (strcmp(tag, "Null") == 0)     return &east_null_type;
    if (strcmp(tag, "Boolean") == 0)  return &east_boolean_type;
    if (strcmp(tag, "Integer") == 0)  return &east_integer_type;
    if (strcmp(tag, "Float") == 0)    return &east_float_type;
    if (strcmp(tag, "String") == 0)   return &east_string_type;
    if (strcmp(tag, "DateTime") == 0) return &east_datetime_type;
    if (strcmp(tag, "Blob") == 0)     return &east_blob_type;

    /* Container types with element: payload is the element type (variant) */
    if (strcmp(tag, "Array") == 0) {
        EastType *elem = east_type_from_value_ctx(payload, rec_stack);
        if (!elem) return NULL;
        EastType *t = east_array_type(elem);
        if (elem->ref_count > 0) east_type_release(elem);
        return t;
    }
    if (strcmp(tag, "Set") == 0) {
        EastType *elem = east_type_from_value_ctx(payload, rec_stack);
        if (!elem) return NULL;
        EastType *t = east_set_type(elem);
        if (elem->ref_count > 0) east_type_release(elem);
        return t;
    }
    if (strcmp(tag, "Ref") == 0) {
        EastType *elem = east_type_from_value_ctx(payload, rec_stack);
        if (!elem) return NULL;
        EastType *t = east_ref_type(elem);
        if (elem->ref_count > 0) east_type_release(elem);
        return t;
    }
    if (strcmp(tag, "Vector") == 0) {
        EastType *elem = east_type_from_value_ctx(payload, rec_stack);
        if (!elem) return NULL;
        EastType *t = east_vector_type(elem);
        if (elem->ref_count > 0) east_type_release(elem);
        return t;
    }
    if (strcmp(tag, "Matrix") == 0) {
        EastType *elem = east_type_from_value_ctx(payload, rec_stack);
        if (!elem) return NULL;
        EastType *t = east_matrix_type(elem);
        if (elem->ref_count > 0) east_type_release(elem);
        return t;
    }

    /* Dict: payload is struct {key: type, value: type} */
    if (strcmp(tag, "Dict") == 0) {
        EastValue *key_v = east_struct_get_field(payload, "key");
        EastValue *val_v = east_struct_get_field(payload, "value");
        EastType *key = east_type_from_value_ctx(key_v, rec_stack);
        EastType *val = east_type_from_value_ctx(val_v, rec_stack);
        if (!key || !val) return NULL;
        EastType *t = east_dict_type(key, val);
        if (key->ref_count > 0) east_type_release(key);
        if (val->ref_count > 0) east_type_release(val);
        return t;
    }

    /* Struct: payload is array of {name: String, type: type} */
    if (strcmp(tag, "Struct") == 0) {
        if (!payload || payload->kind != EAST_VAL_ARRAY) return NULL;
        size_t n = payload->data.array.len;
        const char **names = malloc(n * sizeof(char *));
        EastType **types = malloc(n * sizeof(EastType *));
        for (size_t i = 0; i < n; i++) {
            EastValue *field = payload->data.array.items[i];
            EastValue *name_v = east_struct_get_field(field, "name");
            EastValue *type_v = east_struct_get_field(field, "type");
            names[i] = name_v->data.string.data;
            types[i] = east_type_from_value_ctx(type_v, rec_stack);
        }
        EastType *t = east_struct_type(names, types, n);
        for (size_t i = 0; i < n; i++) {
            if (types[i] && types[i]->ref_count > 0) east_type_release(types[i]);
        }
        free(names);
        free(types);
        return t;
    }

    /* Variant: payload is array of {name: String, type: type} */
    if (strcmp(tag, "Variant") == 0) {
        if (!payload || payload->kind != EAST_VAL_ARRAY) return NULL;
        size_t n = payload->data.array.len;
        const char **names = malloc(n * sizeof(char *));
        EastType **types = malloc(n * sizeof(EastType *));
        for (size_t i = 0; i < n; i++) {
            EastValue *cas = payload->data.array.items[i];
            EastValue *name_v = east_struct_get_field(cas, "name");
            EastValue *type_v = east_struct_get_field(cas, "type");
            names[i] = name_v->data.string.data;
            types[i] = east_type_from_value_ctx(type_v, rec_stack);
        }
        EastType *t = east_variant_type(names, types, n);
        for (size_t i = 0; i < n; i++) {
            if (types[i] && types[i]->ref_count > 0) east_type_release(types[i]);
        }
        free(names);
        free(types);
        return t;
    }

    /* Function / AsyncFunction: payload is struct {inputs: [type], output: type} */
    if (strcmp(tag, "Function") == 0 || strcmp(tag, "AsyncFunction") == 0) {
        EastValue *inputs_v = east_struct_get_field(payload, "inputs");
        EastValue *output_v = east_struct_get_field(payload, "output");
        size_t ni = inputs_v->data.array.len;
        EastType **inputs = malloc(ni * sizeof(EastType *));
        for (size_t i = 0; i < ni; i++) {
            inputs[i] = east_type_from_value_ctx(inputs_v->data.array.items[i],
                                                  rec_stack);
        }
        EastType *output = east_type_from_value_ctx(output_v, rec_stack);
        EastType *t;
        if (strcmp(tag, "AsyncFunction") == 0) {
            t = east_async_function_type(inputs, ni, output);
        } else {
            t = east_function_type(inputs, ni, output);
        }
        for (size_t i = 0; i < ni; i++) {
            if (inputs[i] && inputs[i]->ref_count > 0) east_type_release(inputs[i]);
        }
        free(inputs);
        if (output && output->ref_count > 0) east_type_release(output);
        return t;
    }

    /* Recursive: payload is Integer (depth marker for self-reference).
     * east_type_from_value only supports one level of recursion, so
     * ALL self-references resolve to the single wrapper at rec_stack[0].
     * The depth value varies with nesting but always targets the same
     * outermost recursive wrapper. */
    if (strcmp(tag, "Recursive") == 0) {
        if (payload && payload->kind == EAST_VAL_INTEGER && rec_stack->len > 0) {
            east_type_retain(rec_stack->items[0]);
            return rec_stack->items[0];
        }
        /* If no valid depth or no stack, create a disconnected wrapper */
        return east_recursive_type_new();
    }

    return NULL;
}

/* ================================================================== */
/*  east_type_from_value (public API)                                  */
/*                                                                     */
/*  Converts a decoded EastTypeType variant value -> EastType*.        */
/*  The input is an EastValue* of kind EAST_VAL_VARIANT.               */
/* ================================================================== */

EastType *east_type_from_value(EastValue *v)
{
    RecStack rec_stack = { .items = NULL, .len = 0, .cap = 0 };

    /*
     * Pre-create a Recursive wrapper and push it on the stack at depth 0.
     * If the decoded type contains Recursive(N) self-references, they
     * will resolve to this wrapper via the stack.  After decoding, if
     * the wrapper was actually referenced (ref_count > 1), we know the
     * type IS recursive and we wire it up.  Otherwise we discard the
     * wrapper and return the decoded type directly.
     */
    EastType *wrapper = east_recursive_type_new();
    rec_stack_push(&rec_stack, wrapper);

    EastType *inner = east_type_from_value_ctx(v, &rec_stack);

    free(rec_stack.items);

    if (!inner) {
        east_type_release(wrapper);
        return NULL;
    }

    if (wrapper->ref_count > 1) {
        /* Self-references were found — this IS a recursive type. */
        east_recursive_type_set(wrapper, inner);
        east_recursive_type_finalize(wrapper);
        /* inner is owned by wrapper (not retained, to avoid cycle).
         * finalize adjusts ref_count so only external refs are tracked. */
        return wrapper;
    } else {
        /* No self-references — discard the unused wrapper. */
        east_type_release(wrapper);
        return inner;
    }
}

/* ================================================================== */
/*  east_type_to_value                                                 */
/*                                                                     */
/*  Converts EastType* -> EastTypeType variant value (EastValue*).     */
/*  Inverse of east_type_from_value.                                   */
/*                                                                     */
/*  The Recursive(N) depth value counts compound types in the nesting  */
/*  hierarchy, matching how beast2's typeCtx stack works in TypeScript. */
/*  Each compound type (Array, Dict, Struct, Variant, Function, etc.)  */
/*  pushes onto the context stack; N counts back from the top.         */
/* ================================================================== */

typedef struct {
    /* Compound type context stack (mirrors beast2 typeCtx) */
    int len;
    int cap;
    /* Recursive wrapper tracking: wrapper pointer + its stack index */
    struct { EastType *wrapper; int stack_index; } *recs;
    int num_recs;
    int recs_cap;
} TVCtx;

static void tv_ctx_push(TVCtx *ctx) {
    ctx->len++;
}

static void tv_ctx_pop(TVCtx *ctx) {
    ctx->len--;
}

static void tv_ctx_add_rec(TVCtx *ctx, EastType *wrapper) {
    if (ctx->num_recs >= ctx->recs_cap) {
        int new_cap = ctx->recs_cap ? ctx->recs_cap * 2 : 4;
        void *tmp = realloc(ctx->recs, (size_t)new_cap * sizeof(ctx->recs[0]));
        if (!tmp) return;
        ctx->recs = tmp;
        ctx->recs_cap = new_cap;
    }
    ctx->recs[ctx->num_recs].wrapper = wrapper;
    ctx->recs[ctx->num_recs].stack_index = ctx->len;  /* where inner will be pushed */
    ctx->num_recs++;
}

static EastValue *make_field_value(const char *name, EastValue *type_val)
{
    EastType *field_type = east_type_type->data.recursive.node;  /* inner variant */
    EastType *field_struct_type = NULL;

    /* Find the Struct case to get the field struct type: {name: String, type: self} */
    for (size_t i = 0; i < field_type->data.variant.num_cases; i++) {
        if (strcmp(field_type->data.variant.cases[i].name, "Struct") == 0) {
            /* Struct case value is Array({name: String, type: self}) */
            field_struct_type = field_type->data.variant.cases[i].type->data.element;
            break;
        }
    }

    const char *names[] = {"name", "type"};
    EastValue *vals[] = {east_string(name), type_val};
    EastValue *s = east_struct_new(names, vals, 2, field_struct_type);
    east_value_release(vals[0]);
    east_value_release(type_val);
    return s;
}

static EastValue *type_to_value_ctx(EastType *type, TVCtx *ctx)
{
    if (!type) return NULL;

    /* Check for self-reference: pointer matches a recursive wrapper */
    for (int i = ctx->num_recs - 1; i >= 0; i--) {
        if (type == ctx->recs[i].wrapper) {
            int64_t depth = ctx->len - ctx->recs[i].stack_index;
            return east_variant_new("Recursive", east_integer(depth),
                                    east_type_type->data.recursive.node);
        }
    }

    if (type->kind == EAST_TYPE_RECURSIVE) {
        /* Record wrapper → next stack index, recurse into inner type */
        tv_ctx_add_rec(ctx, type);
        EastValue *result = type_to_value_ctx(type->data.recursive.node, ctx);
        ctx->num_recs--;
        return result;
    }

    EastType *vtype = east_type_type->data.recursive.node;  /* inner variant */

    switch (type->kind) {
    case EAST_TYPE_NEVER:
        return east_variant_new("Never", east_null(), vtype);
    case EAST_TYPE_NULL:
        return east_variant_new("Null", east_null(), vtype);
    case EAST_TYPE_BOOLEAN:
        return east_variant_new("Boolean", east_null(), vtype);
    case EAST_TYPE_INTEGER:
        return east_variant_new("Integer", east_null(), vtype);
    case EAST_TYPE_FLOAT:
        return east_variant_new("Float", east_null(), vtype);
    case EAST_TYPE_STRING:
        return east_variant_new("String", east_null(), vtype);
    case EAST_TYPE_DATETIME:
        return east_variant_new("DateTime", east_null(), vtype);
    case EAST_TYPE_BLOB:
        return east_variant_new("Blob", east_null(), vtype);

    case EAST_TYPE_ARRAY: {
        tv_ctx_push(ctx);
        EastValue *elem = type_to_value_ctx(type->data.element, ctx);
        tv_ctx_pop(ctx);
        return east_variant_new("Array", elem, vtype);
    }
    case EAST_TYPE_SET: {
        tv_ctx_push(ctx);
        EastValue *elem = type_to_value_ctx(type->data.element, ctx);
        tv_ctx_pop(ctx);
        return east_variant_new("Set", elem, vtype);
    }
    case EAST_TYPE_REF: {
        tv_ctx_push(ctx);
        EastValue *elem = type_to_value_ctx(type->data.element, ctx);
        tv_ctx_pop(ctx);
        return east_variant_new("Ref", elem, vtype);
    }
    case EAST_TYPE_VECTOR: {
        tv_ctx_push(ctx);
        EastValue *elem = type_to_value_ctx(type->data.element, ctx);
        tv_ctx_pop(ctx);
        return east_variant_new("Vector", elem, vtype);
    }
    case EAST_TYPE_MATRIX: {
        tv_ctx_push(ctx);
        EastValue *elem = type_to_value_ctx(type->data.element, ctx);
        tv_ctx_pop(ctx);
        return east_variant_new("Matrix", elem, vtype);
    }

    case EAST_TYPE_DICT: {
        tv_ctx_push(ctx);
        EastValue *key = type_to_value_ctx(type->data.dict.key, ctx);
        EastValue *val = type_to_value_ctx(type->data.dict.value, ctx);
        tv_ctx_pop(ctx);
        /* Find Dict case type: {key: self, value: self} */
        EastType *dict_struct = NULL;
        for (size_t i = 0; i < vtype->data.variant.num_cases; i++) {
            if (strcmp(vtype->data.variant.cases[i].name, "Dict") == 0) {
                dict_struct = vtype->data.variant.cases[i].type;
                break;
            }
        }
        const char *names[] = {"key", "value"};
        EastValue *vals[] = {key, val};
        EastValue *payload = east_struct_new(names, vals, 2, dict_struct);
        east_value_release(key);
        east_value_release(val);
        return east_variant_new("Dict", payload, vtype);
    }

    case EAST_TYPE_STRUCT:
    case EAST_TYPE_VARIANT: {
        bool is_struct = (type->kind == EAST_TYPE_STRUCT);
        size_t n = is_struct ? type->data.struct_.num_fields
                             : type->data.variant.num_cases;
        EastTypeField *fields = is_struct ? type->data.struct_.fields
                                          : type->data.variant.cases;

        /* Build array of {name: String, type: EastTypeType} */
        const char *case_name = is_struct ? "Struct" : "Variant";
        EastType *arr_type = NULL;
        for (size_t i = 0; i < vtype->data.variant.num_cases; i++) {
            if (strcmp(vtype->data.variant.cases[i].name, case_name) == 0) {
                arr_type = vtype->data.variant.cases[i].type;
                break;
            }
        }
        tv_ctx_push(ctx);
        EastValue *arr = east_array_new(arr_type ? arr_type->data.element : NULL);
        for (size_t i = 0; i < n; i++) {
            EastValue *field = make_field_value(fields[i].name,
                type_to_value_ctx(fields[i].type, ctx));
            east_array_push(arr, field);
            east_value_release(field);
        }
        tv_ctx_pop(ctx);
        return east_variant_new(case_name, arr, vtype);
    }

    case EAST_TYPE_FUNCTION:
    case EAST_TYPE_ASYNC_FUNCTION: {
        bool is_async = (type->kind == EAST_TYPE_ASYNC_FUNCTION);
        const char *case_name = is_async ? "AsyncFunction" : "Function";
        size_t ni = type->data.function.num_inputs;

        tv_ctx_push(ctx);
        /* Build inputs array */
        EastValue *inputs = east_array_new(east_type_type);
        for (size_t i = 0; i < ni; i++) {
            EastValue *inp = type_to_value_ctx(type->data.function.inputs[i], ctx);
            east_array_push(inputs, inp);
            east_value_release(inp);
        }
        EastValue *output = type_to_value_ctx(type->data.function.output, ctx);
        tv_ctx_pop(ctx);

        /* Find function case type: {inputs: Array(self), output: self} */
        EastType *fn_struct = NULL;
        for (size_t i = 0; i < vtype->data.variant.num_cases; i++) {
            if (strcmp(vtype->data.variant.cases[i].name, case_name) == 0) {
                fn_struct = vtype->data.variant.cases[i].type;
                break;
            }
        }
        const char *names[] = {"inputs", "output"};
        EastValue *vals[] = {inputs, output};
        EastValue *payload = east_struct_new(names, vals, 2, fn_struct);
        east_value_release(inputs);
        east_value_release(output);
        return east_variant_new(case_name, payload, vtype);
    }

    case EAST_TYPE_RECURSIVE:
        /* Already handled above; unreachable */
        return NULL;
    }

    return NULL;
}

EastValue *east_type_to_value(EastType *type)
{
    if (!type) return NULL;
    if (!east_type_type) east_type_of_type_init();
    TVCtx ctx = { .len = 0, .cap = 0, .recs = NULL, .num_recs = 0, .recs_cap = 0 };
    EastValue *result = type_to_value_ctx(type, &ctx);
    free(ctx.recs);
    return result;
}

/* ================================================================== */
/*  Helpers for ir_from_value                                          */
/* ================================================================== */

/* Get a string field from a struct value, returning the C string pointer.
 * The returned pointer is valid as long as the struct value is alive. */
static const char *get_str(EastValue *s, const char *field)
{
    EastValue *v = east_struct_get_field(s, field);
    if (!v || v->kind != EAST_VAL_STRING) return "";
    return v->data.string.data;
}

static bool get_bool(EastValue *s, const char *field)
{
    EastValue *v = east_struct_get_field(s, field);
    if (!v || v->kind != EAST_VAL_BOOLEAN) return false;
    return v->data.boolean;
}


static EastValue *get_field(EastValue *s, const char *field)
{
    return east_struct_get_field(s, field);
}

/* Convert a label struct to a string (just the name field) */
static const char *label_from_value(EastValue *label_v)
{
    if (!label_v || label_v->kind != EAST_VAL_STRUCT) return NULL;
    return get_str(label_v, "name");
}

/* Convert type field (EastTypeType variant) to EastType* */
static EastType *type_field(EastValue *s)
{
    EastValue *tv = east_struct_get_field(s, "type");
    return east_type_from_value(tv);
}

/* Convert a literal value (LiteralValueType variant) to EastValue* */
static EastValue *literal_from_value(EastValue *v)
{
    if (!v || v->kind != EAST_VAL_VARIANT) return east_null();
    const char *tag = v->data.variant.case_name;
    EastValue *payload = v->data.variant.value;

    if (strcmp(tag, "Null") == 0)     return east_null();
    if (strcmp(tag, "Boolean") == 0)  return east_boolean(payload->data.boolean);
    if (strcmp(tag, "Integer") == 0)  return east_integer(payload->data.integer);
    if (strcmp(tag, "Float") == 0)    return east_float(payload->data.float64);
    if (strcmp(tag, "String") == 0)   return east_string_len(payload->data.string.data, payload->data.string.len);
    if (strcmp(tag, "DateTime") == 0) return east_datetime(payload->data.datetime);
    if (strcmp(tag, "Blob") == 0)     return east_blob(payload->data.blob.data, payload->data.blob.len);

    return east_null();
}

/* Forward declaration */
static IRNode *convert_ir(EastValue *v);

/* Release a temporary array of IRNode* after passing to an ir_* constructor
 * (which retains via ir_nodes_dup).  Releases each element and frees array. */
static void free_temp_nodes(IRNode **nodes, size_t n) {
    if (!nodes) return;
    for (size_t i = 0; i < n; i++) {
        if (nodes[i]) ir_node_release(nodes[i]);
    }
    free(nodes);
}

/* Release a temporary array of EastType* after passing to an ir_* constructor
 * (which retains via ir_types_dup).  Releases each element and frees array. */
static void free_temp_types(EastType **types, size_t n) {
    if (!types) return;
    for (size_t i = 0; i < n; i++) {
        if (types[i]) east_type_release(types[i]);
    }
    free(types);
}

/* Convert an array of IR values to an array of IRNode* */
static IRNode **convert_ir_array(EastValue *arr, size_t *out_count)
{
    if (!arr || arr->kind != EAST_VAL_ARRAY) {
        *out_count = 0;
        return NULL;
    }
    size_t n = arr->data.array.len;
    *out_count = n;
    if (n == 0) return NULL;
    IRNode **nodes = calloc(n, sizeof(IRNode *));
    for (size_t i = 0; i < n; i++) {
        nodes[i] = convert_ir(arr->data.array.items[i]);
    }
    return nodes;
}

/* Convert an array of type values to EastType** */
static EastType **convert_type_array(EastValue *arr, size_t *out_count)
{
    if (!arr || arr->kind != EAST_VAL_ARRAY) {
        *out_count = 0;
        return NULL;
    }
    size_t n = arr->data.array.len;
    *out_count = n;
    if (n == 0) return NULL;
    EastType **types = calloc(n, sizeof(EastType *));
    for (size_t i = 0; i < n; i++) {
        types[i] = east_type_from_value(arr->data.array.items[i]);
    }
    return types;
}

/* Extract IRVariable info from a Variable IR node value */
static IRVariable var_from_ir_value(EastValue *v)
{
    IRVariable var = {0};
    if (!v || v->kind != EAST_VAL_VARIANT) return var;
    EastValue *s = v->data.variant.value;
    var.name = strdup(get_str(s, "name"));
    var.mutable = get_bool(s, "mutable");
    var.captured = get_bool(s, "captured");
    return var;
}

/* ================================================================== */
/*  east_ir_from_value                                                 */
/* ================================================================== */

/* Extract location array from a deserialized IR struct and set it on the node */
static void apply_location(IRNode *node, EastValue *s)
{
    if (!node || !s) return;
    EastValue *loc_arr = get_field(s, "location");
    if (!loc_arr || loc_arr->kind != EAST_VAL_ARRAY) return;
    size_t n = loc_arr->data.array.len;
    if (n == 0) return;

    EastLocation *locs = calloc(n, sizeof(EastLocation));
    if (!locs) return;

    for (size_t i = 0; i < n; i++) {
        EastValue *loc = loc_arr->data.array.items[i];
        if (loc && loc->kind == EAST_VAL_STRUCT) {
            EastValue *fn = east_struct_get_field(loc, "filename");
            EastValue *ln = east_struct_get_field(loc, "line");
            EastValue *col = east_struct_get_field(loc, "column");
            if (fn && fn->kind == EAST_VAL_STRING) {
                locs[i].filename = strdup(fn->data.string.data);
            }
            if (ln && ln->kind == EAST_VAL_INTEGER) {
                locs[i].line = ln->data.integer;
            }
            if (col && col->kind == EAST_VAL_INTEGER) {
                locs[i].column = col->data.integer;
            }
        }
    }

    node->locations = locs;
    node->num_locations = n;
}

/* Helper: convert IR and apply location from struct s */
static IRNode *with_loc(IRNode *node, EastValue *s)
{
    apply_location(node, s);
    return node;
}

static IRNode *convert_ir(EastValue *v)
{
    if (!v || v->kind != EAST_VAL_VARIANT) return NULL;

    const char *tag = v->data.variant.case_name;
    EastValue *s = v->data.variant.value; /* struct payload */
    EastType *type = type_field(s);
    IRNode *result = NULL;

    /* ----- Value ----- */
    if (strcmp(tag, "Value") == 0) {
        EastValue *lit = literal_from_value(get_field(s, "value"));
        result = with_loc(ir_value(type, lit), s);
        east_value_release(lit); /* ir_value retained it */
        goto cleanup;
    }

    /* ----- Variable ----- */
    if (strcmp(tag, "Variable") == 0) {
        result = with_loc(ir_variable(type, get_str(s, "name"),
                           get_bool(s, "mutable"), get_bool(s, "captured")), s);
        goto cleanup;
    }

    /* ----- Let ----- */
    if (strcmp(tag, "Let") == 0) {
        EastValue *var_v = get_field(s, "variable");
        IRVariable var = var_from_ir_value(var_v);
        IRNode *val = convert_ir(get_field(s, "value"));
        result = with_loc(ir_let(type, var.name, var.mutable, var.captured, val), s);
        ir_node_release(val);
        free(var.name);
        goto cleanup;
    }

    /* ----- Assign ----- */
    if (strcmp(tag, "Assign") == 0) {
        EastValue *var_v = get_field(s, "variable");
        const char *name = "";
        if (var_v && var_v->kind == EAST_VAL_VARIANT) {
            EastValue *vs = var_v->data.variant.value;
            name = get_str(vs, "name");
        }
        IRNode *val = convert_ir(get_field(s, "value"));
        result = with_loc(ir_assign(type, name, val), s);
        ir_node_release(val);
        goto cleanup;
    }

    /* ----- As (type cast - pass through) ----- */
    if (strcmp(tag, "As") == 0) {
        result = convert_ir(get_field(s, "value"));
        goto cleanup;
    }

    /* ----- Block ----- */
    if (strcmp(tag, "Block") == 0) {
        size_t n;
        IRNode **stmts = convert_ir_array(get_field(s, "statements"), &n);
        result = with_loc(ir_block(type, stmts, n), s);
        free_temp_nodes(stmts, n);
        goto cleanup;
    }

    /* ----- IfElse ----- */
    if (strcmp(tag, "IfElse") == 0) {
        EastValue *ifs = get_field(s, "ifs");
        IRNode *else_body = convert_ir(get_field(s, "else_body"));

        /* Chain if/elif branches from right to left */
        if (!ifs || ifs->kind != EAST_VAL_ARRAY || ifs->data.array.len == 0) {
            result = else_body;
            goto cleanup;
        }

        result = else_body;
        for (size_t i = ifs->data.array.len; i > 0; i--) {
            EastValue *branch = ifs->data.array.items[i - 1];
            IRNode *pred = convert_ir(get_field(branch, "predicate"));
            IRNode *body = convert_ir(get_field(branch, "body"));
            IRNode *next = ir_if_else(type, pred, body, result);
            ir_node_release(pred);
            ir_node_release(body);
            ir_node_release(result);
            result = next;
        }
        with_loc(result, s);
        goto cleanup;
    }

    /* ----- Match ----- */
    if (strcmp(tag, "Match") == 0) {
        IRNode *expr = convert_ir(get_field(s, "variant"));
        EastValue *cases_v = get_field(s, "cases");
        size_t nc = cases_v ? cases_v->data.array.len : 0;
        IRMatchCase *cases = calloc(nc > 0 ? nc : 1, sizeof(IRMatchCase));
        for (size_t i = 0; i < nc; i++) {
            EastValue *c = cases_v->data.array.items[i];
            cases[i].case_name = strdup(get_str(c, "case"));
            EastValue *var_v = get_field(c, "variable");
            if (var_v && var_v->kind == EAST_VAL_VARIANT) {
                EastValue *vs = var_v->data.variant.value;
                cases[i].bind_name = strdup(get_str(vs, "name"));
            }
            cases[i].body = convert_ir(get_field(c, "body"));
        }
        result = with_loc(ir_match(type, expr, cases, nc), s);
        ir_node_release(expr);
        for (size_t i = 0; i < nc; i++) {
            ir_node_release(cases[i].body);
            free(cases[i].case_name);
            free(cases[i].bind_name);
        }
        free(cases);
        goto cleanup;
    }

    /* ----- While ----- */
    if (strcmp(tag, "While") == 0) {
        IRNode *cond = convert_ir(get_field(s, "predicate"));
        IRNode *body = convert_ir(get_field(s, "body"));
        const char *label = label_from_value(get_field(s, "label"));
        result = with_loc(ir_while(type, cond, body, label), s);
        ir_node_release(cond);
        ir_node_release(body);
        goto cleanup;
    }

    /* ----- ForArray ----- */
    if (strcmp(tag, "ForArray") == 0) {
        IRNode *arr = convert_ir(get_field(s, "array"));
        IRNode *body = convert_ir(get_field(s, "body"));
        const char *label = label_from_value(get_field(s, "label"));
        EastValue *val_v = get_field(s, "value");
        EastValue *key_v = get_field(s, "key");
        const char *val_name = "";
        const char *idx_name = NULL;
        if (val_v && val_v->kind == EAST_VAL_VARIANT) {
            val_name = get_str(val_v->data.variant.value, "name");
        }
        if (key_v && key_v->kind == EAST_VAL_VARIANT) {
            idx_name = get_str(key_v->data.variant.value, "name");
        }
        result = with_loc(ir_for_array(type, val_name, idx_name, arr, body, label), s);
        ir_node_release(arr);
        ir_node_release(body);
        goto cleanup;
    }

    /* ----- ForSet ----- */
    if (strcmp(tag, "ForSet") == 0) {
        IRNode *set = convert_ir(get_field(s, "set"));
        IRNode *body = convert_ir(get_field(s, "body"));
        const char *label = label_from_value(get_field(s, "label"));
        EastValue *key_v = get_field(s, "key");
        const char *var_name = "";
        if (key_v && key_v->kind == EAST_VAL_VARIANT) {
            var_name = get_str(key_v->data.variant.value, "name");
        }
        result = with_loc(ir_for_set(type, var_name, set, body, label), s);
        ir_node_release(set);
        ir_node_release(body);
        goto cleanup;
    }

    /* ----- ForDict ----- */
    if (strcmp(tag, "ForDict") == 0) {
        IRNode *dict = convert_ir(get_field(s, "dict"));
        IRNode *body = convert_ir(get_field(s, "body"));
        const char *label = label_from_value(get_field(s, "label"));
        EastValue *key_v = get_field(s, "key");
        EastValue *val_v = get_field(s, "value");
        const char *key_name = "";
        const char *val_name = "";
        if (key_v && key_v->kind == EAST_VAL_VARIANT) {
            key_name = get_str(key_v->data.variant.value, "name");
        }
        if (val_v && val_v->kind == EAST_VAL_VARIANT) {
            val_name = get_str(val_v->data.variant.value, "name");
        }
        result = with_loc(ir_for_dict(type, key_name, val_name, dict, body, label), s);
        ir_node_release(dict);
        ir_node_release(body);
        goto cleanup;
    }

    /* ----- Function / AsyncFunction ----- */
    if (strcmp(tag, "Function") == 0 || strcmp(tag, "AsyncFunction") == 0) {
        EastValue *caps_v = get_field(s, "captures");
        EastValue *params_v = get_field(s, "parameters");
        IRNode *body = convert_ir(get_field(s, "body"));

        size_t nc = caps_v ? caps_v->data.array.len : 0;
        size_t np = params_v ? params_v->data.array.len : 0;

        IRVariable *captures = nc > 0 ? calloc(nc, sizeof(IRVariable)) : NULL;
        IRVariable *params = np > 0 ? calloc(np, sizeof(IRVariable)) : NULL;

        for (size_t i = 0; i < nc; i++) {
            captures[i] = var_from_ir_value(caps_v->data.array.items[i]);
        }
        for (size_t i = 0; i < np; i++) {
            params[i] = var_from_ir_value(params_v->data.array.items[i]);
        }

        if (strcmp(tag, "AsyncFunction") == 0) {
            result = with_loc(ir_async_function(type, captures, nc, params, np, body), s);
        } else {
            result = with_loc(ir_function(type, captures, nc, params, np, body), s);
        }
        /* Store original IR variant value for serialization */
        result->data.function.source_ir = v;
        east_value_retain(v);
        ir_node_release(body);
        for (size_t i = 0; i < nc; i++) free(captures[i].name);
        free(captures);
        for (size_t i = 0; i < np; i++) free(params[i].name);
        free(params);
        goto cleanup;
    }

    /* ----- Call / CallAsync ----- */
    if (strcmp(tag, "Call") == 0 || strcmp(tag, "CallAsync") == 0) {
        IRNode *func = convert_ir(get_field(s, "function"));
        size_t n;
        IRNode **args = convert_ir_array(get_field(s, "arguments"), &n);
        if (strcmp(tag, "CallAsync") == 0) {
            result = with_loc(ir_call_async(type, func, args, n), s);
        } else {
            result = with_loc(ir_call(type, func, args, n), s);
        }
        ir_node_release(func);
        free_temp_nodes(args, n);
        goto cleanup;
    }

    /* ----- Platform ----- */
    if (strcmp(tag, "Platform") == 0) {
        const char *name = get_str(s, "name");
        bool is_async = get_bool(s, "async");
        size_t ntp, nargs;
        EastType **tp = convert_type_array(get_field(s, "type_parameters"), &ntp);
        IRNode **args = convert_ir_array(get_field(s, "arguments"), &nargs);
        result = with_loc(ir_platform(type, name, tp, ntp, args, nargs, is_async), s);
        free_temp_types(tp, ntp);
        free_temp_nodes(args, nargs);
        goto cleanup;
    }

    /* ----- Builtin ----- */
    if (strcmp(tag, "Builtin") == 0) {
        const char *name = get_str(s, "builtin");
        size_t ntp, nargs;
        EastType **tp = convert_type_array(get_field(s, "type_parameters"), &ntp);
        IRNode **args = convert_ir_array(get_field(s, "arguments"), &nargs);
        result = with_loc(ir_builtin(type, name, tp, ntp, args, nargs), s);
        free_temp_types(tp, ntp);
        free_temp_nodes(args, nargs);
        goto cleanup;
    }

    /* ----- Return ----- */
    if (strcmp(tag, "Return") == 0) {
        IRNode *val = convert_ir(get_field(s, "value"));
        result = with_loc(ir_return(type, val), s);
        ir_node_release(val);
        goto cleanup;
    }

    /* ----- Break ----- */
    if (strcmp(tag, "Break") == 0) {
        const char *label = label_from_value(get_field(s, "label"));
        result = with_loc(ir_break(label), s);
        goto cleanup;
    }

    /* ----- Continue ----- */
    if (strcmp(tag, "Continue") == 0) {
        const char *label = label_from_value(get_field(s, "label"));
        result = with_loc(ir_continue(label), s);
        goto cleanup;
    }

    /* ----- Error ----- */
    if (strcmp(tag, "Error") == 0) {
        IRNode *msg = convert_ir(get_field(s, "message"));
        result = with_loc(ir_error(type, msg), s);
        ir_node_release(msg);
        goto cleanup;
    }

    /* ----- TryCatch ----- */
    if (strcmp(tag, "TryCatch") == 0) {
        IRNode *try_body = convert_ir(get_field(s, "try_body"));
        IRNode *catch_body = convert_ir(get_field(s, "catch_body"));
        EastValue *msg_v = get_field(s, "message");
        const char *message_var = "";
        if (msg_v && msg_v->kind == EAST_VAL_VARIANT) {
            message_var = get_str(msg_v->data.variant.value, "name");
        }
        EastValue *stack_v = get_field(s, "stack");
        const char *stack_var = "";
        if (stack_v && stack_v->kind == EAST_VAL_VARIANT) {
            stack_var = get_str(stack_v->data.variant.value, "name");
        }
        IRNode *finally_body = convert_ir(get_field(s, "finally_body"));
        result = with_loc(ir_try_catch(type, try_body, message_var, stack_var,
                            catch_body, finally_body), s);
        ir_node_release(try_body);
        ir_node_release(catch_body);
        ir_node_release(finally_body);
        goto cleanup;
    }

    /* ----- NewArray ----- */
    if (strcmp(tag, "NewArray") == 0) {
        size_t n;
        IRNode **items = convert_ir_array(get_field(s, "values"), &n);
        result = with_loc(ir_new_array(type, items, n), s);
        free_temp_nodes(items, n);
        goto cleanup;
    }

    /* ----- NewSet ----- */
    if (strcmp(tag, "NewSet") == 0) {
        size_t n;
        IRNode **items = convert_ir_array(get_field(s, "values"), &n);
        result = with_loc(ir_new_set(type, items, n), s);
        free_temp_nodes(items, n);
        goto cleanup;
    }

    /* ----- NewDict ----- */
    if (strcmp(tag, "NewDict") == 0) {
        EastValue *vals = get_field(s, "values");
        size_t n = (vals && vals->kind == EAST_VAL_ARRAY) ? vals->data.array.len : 0;
        IRNode **keys = calloc(n > 0 ? n : 1, sizeof(IRNode *));
        IRNode **values = calloc(n > 0 ? n : 1, sizeof(IRNode *));
        for (size_t i = 0; i < n; i++) {
            EastValue *entry = vals->data.array.items[i];
            keys[i] = convert_ir(get_field(entry, "key"));
            values[i] = convert_ir(get_field(entry, "value"));
        }
        result = with_loc(ir_new_dict(type, keys, values, n), s);
        free_temp_nodes(keys, n);
        free_temp_nodes(values, n);
        goto cleanup;
    }

    /* ----- NewRef ----- */
    if (strcmp(tag, "NewRef") == 0) {
        IRNode *val = convert_ir(get_field(s, "value"));
        result = with_loc(ir_new_ref(type, val), s);
        ir_node_release(val);
        goto cleanup;
    }

    /* ----- NewVector ----- */
    if (strcmp(tag, "NewVector") == 0) {
        size_t n;
        IRNode **items = convert_ir_array(get_field(s, "values"), &n);
        result = with_loc(ir_new_vector(type, items, n), s);
        free_temp_nodes(items, n);
        goto cleanup;
    }

    /* ----- Struct ----- */
    if (strcmp(tag, "Struct") == 0) {
        EastValue *fields = get_field(s, "fields");
        size_t n = (fields && fields->kind == EAST_VAL_ARRAY) ? fields->data.array.len : 0;
        char **names = calloc(n > 0 ? n : 1, sizeof(char *));
        IRNode **values = calloc(n > 0 ? n : 1, sizeof(IRNode *));
        for (size_t i = 0; i < n; i++) {
            EastValue *f = fields->data.array.items[i];
            names[i] = strdup(get_str(f, "name"));
            values[i] = convert_ir(get_field(f, "value"));
        }
        result = with_loc(ir_struct(type, names, values, n), s);
        free_temp_nodes(values, n);
        for (size_t i = 0; i < n; i++) free(names[i]);
        free(names);
        goto cleanup;
    }

    /* ----- GetField ----- */
    if (strcmp(tag, "GetField") == 0) {
        IRNode *expr = convert_ir(get_field(s, "struct"));
        const char *field_name = get_str(s, "field");
        result = with_loc(ir_get_field(type, expr, field_name), s);
        ir_node_release(expr);
        goto cleanup;
    }

    /* ----- Variant ----- */
    if (strcmp(tag, "Variant") == 0) {
        const char *case_name = get_str(s, "case");
        IRNode *val = convert_ir(get_field(s, "value"));
        result = with_loc(ir_variant(type, case_name, val), s);
        ir_node_release(val);
        goto cleanup;
    }

    /* ----- WrapRecursive ----- */
    if (strcmp(tag, "WrapRecursive") == 0) {
        IRNode *val = convert_ir(get_field(s, "value"));
        result = with_loc(ir_wrap_recursive(type, val), s);
        ir_node_release(val);
        goto cleanup;
    }

    /* ----- UnwrapRecursive ----- */
    if (strcmp(tag, "UnwrapRecursive") == 0) {
        IRNode *val = convert_ir(get_field(s, "value"));
        result = with_loc(ir_unwrap_recursive(type, val), s);
        ir_node_release(val);
        goto cleanup;
    }

    /* ----- NewMatrix (not in C IR yet, treat as error) ----- */
    if (strcmp(tag, "NewMatrix") == 0) {
        fprintf(stderr, "WARNING: NewMatrix IR node not yet supported in C\n");
        result = ir_value(type, east_null());
        goto cleanup;
    }

    fprintf(stderr, "WARNING: Unknown IR node type: %s\n", tag);
    result = ir_value(type, east_null());

cleanup:
    east_type_release(type);
    return result;
}

IRNode *east_ir_from_value(EastValue *value)
{
    return convert_ir(value);
}
