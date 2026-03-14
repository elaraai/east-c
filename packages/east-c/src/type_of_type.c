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

/* ----- Field index constants -----
 *
 * Beast2 encodes struct fields positionally in declaration order.
 * Using index-based access avoids strcmp overhead in hot decode paths.
 */

/* Common IR struct fields */
#define IR_TYPE      0   /* EastTypeType */
#define IR_LOCATION  1   /* [Location] */

/* Location struct: { filename, line, column } */
#define LOC_FILENAME 0
#define LOC_LINE     1
#define LOC_COLUMN   2

/* IRLabel struct: { name, location } */
#define LABEL_NAME   0

/* Struct/Variant field entry in EastTypeType: { name, type } */
#define FE_NAME  0
#define FE_TYPE  1

/* Dict payload in EastTypeType / key-value entries: { key, value } */
#define KV_KEY   0
#define KV_VALUE 1

/* Struct field entry in IR: { name, value } */
#define SF_NAME  0
#define SF_VALUE 1

/* IfElse branch: { predicate, body } */
#define IF_PRED  0
#define IF_BODY  1

/* Match case: { case, variable, body } */
#define MC_CASE  0
#define MC_VAR   1
#define MC_BODY  2

/* Function payload in EastTypeType: { inputs, output } */
#define FN_INPUTS  0
#define FN_OUTPUT  1

/* Variable struct: { type, location, name, mutable, captured } */
#define VAR_NAME      2
#define VAR_MUTABLE   3
#define VAR_CAPTURED  4

/* ================================================================== */
/*  Global type descriptors                                            */
/* ================================================================== */

EastType *east_type_type = NULL;
EastType *east_literal_value_type = NULL;
EastType *east_ir_type = NULL;
EastType *east_ir_type_with_refs = NULL;

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
/*  transform_type_tree: clone a type tree with substitutions          */
/* ================================================================== */

static EastType *transform_type_tree(EastType *type,
                                      EastType *from, EastType *to,
                                      EastType *rec_from, EastType *rec_to)
{
    if (type == from) { east_type_retain(to); return to; }
    if (rec_from && type == rec_from) { east_type_retain(rec_to); return rec_to; }

    switch (type->kind) {
    case EAST_TYPE_STRUCT: {
        size_t nf = type->data.struct_.num_fields;
        const char **names = malloc(nf * sizeof(char*));
        EastType **types = malloc(nf * sizeof(EastType*));
        for (size_t i = 0; i < nf; i++) {
            names[i] = type->data.struct_.fields[i].name;
            types[i] = transform_type_tree(type->data.struct_.fields[i].type,
                                            from, to, rec_from, rec_to);
        }
        EastType *result = east_struct_type(names, types, nf);
        for (size_t i = 0; i < nf; i++) east_type_release(types[i]);
        free(names);
        free(types);
        return result;
    }
    case EAST_TYPE_VARIANT: {
        size_t nc = type->data.variant.num_cases;
        const char **names = malloc(nc * sizeof(char*));
        EastType **types = malloc(nc * sizeof(EastType*));
        for (size_t i = 0; i < nc; i++) {
            names[i] = type->data.variant.cases[i].name;
            types[i] = transform_type_tree(type->data.variant.cases[i].type,
                                            from, to, rec_from, rec_to);
        }
        EastType *result = east_variant_type(names, types, nc);
        for (size_t i = 0; i < nc; i++) east_type_release(types[i]);
        free(names);
        free(types);
        return result;
    }
    case EAST_TYPE_ARRAY: {
        EastType *elem = transform_type_tree(type->data.element,
                                              from, to, rec_from, rec_to);
        EastType *result = east_array_type(elem);
        east_type_release(elem);
        return result;
    }
    case EAST_TYPE_RECURSIVE: {
        EastType *new_rec = east_recursive_type_new();
        EastType *new_node = transform_type_tree(type->data.recursive.node,
                                                  from, to, type, new_rec);
        east_recursive_type_set(new_rec, new_node);
        east_recursive_type_finalize(new_rec);
        /* NOTE: east_recursive_type_set does NOT retain new_node (to avoid
         * cycles), so we must NOT release it here — it's owned by new_rec. */
        return new_rec;
    }
    default:
        east_type_retain(type);
        return type;
    }
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
        /* Build IRTypeWithTableRefs BEFORE releasing intermediates,
         * since transform_type_tree walks the inner type tree. */
        east_ir_type_with_refs = transform_type_tree(
            east_ir_type, east_type_type, &east_integer_type, NULL, NULL);

        /* (no debug) */

        east_type_release(c_while);
        east_type_release(c_wrap);
    }
}

/* ================================================================== */
/*  east_type_from_value_ctx                                           */
/*                                                                     */
/*  Internal helper: converts a decoded EastTypeType variant value to  */
/*  EastType*, tracking compound type depth and speculative recursive  */
/*  wrappers so that Recursive(N) self-references resolve correctly.   */
/*                                                                     */
/*  Mirrors the TypeScript toEastTypeValue encoder:                    */
/*  - Each compound type (Array, Set, Dict, Struct, Variant, Function, */
/*    etc.) pushes onto a depth stack before recursing into children.   */
/*  - Recursive(N) means "self-reference to the compound type at       */
/*    position depth - N in the stack".                                 */
/*  - A speculative recursive wrapper is created at each depth.  If    */
/*    any Recursive(N) targets it, the wrapper is wired up; otherwise  */
/*    it is discarded.                                                  */
/* ================================================================== */

typedef struct {
    EastType **wrappers;  /* Speculative recursive wrappers indexed by depth */
    int depth;            /* Current compound type nesting depth */
    int cap;              /* Capacity of wrappers array */
} RecCtx;

static void rec_ctx_push(RecCtx *ctx) {
    if (ctx->depth >= ctx->cap) {
        int new_cap = ctx->cap ? ctx->cap * 2 : 16;
        EastType **nw = realloc(ctx->wrappers, (size_t)new_cap * sizeof(EastType *));
        if (!nw) return;
        for (int i = ctx->cap; i < new_cap; i++) nw[i] = NULL;
        ctx->wrappers = nw;
        ctx->cap = new_cap;
    }
    ctx->wrappers[ctx->depth] = east_recursive_type_new();
    ctx->depth++;
}

/* Pop depth and check if the wrapper at this position was referenced.
 * If yes, wire it up as a recursive type wrapping `inner`.
 * If no, discard the unused wrapper and return `inner` directly. */
static EastType *rec_ctx_pop(RecCtx *ctx, EastType *inner) {
    ctx->depth--;
    EastType *wrapper = ctx->wrappers[ctx->depth];
    ctx->wrappers[ctx->depth] = NULL;

    if (!inner) {
        if (wrapper) east_type_release(wrapper);
        return NULL;
    }

    if (wrapper && wrapper->ref_count > 1) {
        /* Self-references were found — this IS a recursive type. */
        east_recursive_type_set(wrapper, inner);
        east_recursive_type_finalize(wrapper);
        return wrapper;
    } else {
        /* No self-references — discard the unused wrapper. */
        if (wrapper) east_type_release(wrapper);
        return inner;
    }
}

static EastType *east_type_from_value_ctx(EastValue *v, RecCtx *ctx)
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
        rec_ctx_push(ctx);
        EastType *elem = east_type_from_value_ctx(payload, ctx);
        if (!elem) return rec_ctx_pop(ctx, NULL);
        EastType *t = east_array_type(elem);
        if (elem->ref_count > 0) east_type_release(elem);
        return rec_ctx_pop(ctx, t);
    }
    if (strcmp(tag, "Set") == 0) {
        rec_ctx_push(ctx);
        EastType *elem = east_type_from_value_ctx(payload, ctx);
        if (!elem) return rec_ctx_pop(ctx, NULL);
        EastType *t = east_set_type(elem);
        if (elem->ref_count > 0) east_type_release(elem);
        return rec_ctx_pop(ctx, t);
    }
    if (strcmp(tag, "Ref") == 0) {
        rec_ctx_push(ctx);
        EastType *elem = east_type_from_value_ctx(payload, ctx);
        if (!elem) return rec_ctx_pop(ctx, NULL);
        EastType *t = east_ref_type(elem);
        if (elem->ref_count > 0) east_type_release(elem);
        return rec_ctx_pop(ctx, t);
    }
    if (strcmp(tag, "Vector") == 0) {
        rec_ctx_push(ctx);
        EastType *elem = east_type_from_value_ctx(payload, ctx);
        if (!elem) return rec_ctx_pop(ctx, NULL);
        EastType *t = east_vector_type(elem);
        if (elem->ref_count > 0) east_type_release(elem);
        return rec_ctx_pop(ctx, t);
    }
    if (strcmp(tag, "Matrix") == 0) {
        rec_ctx_push(ctx);
        EastType *elem = east_type_from_value_ctx(payload, ctx);
        if (!elem) return rec_ctx_pop(ctx, NULL);
        EastType *t = east_matrix_type(elem);
        if (elem->ref_count > 0) east_type_release(elem);
        return rec_ctx_pop(ctx, t);
    }

    /* Dict: payload is struct {key: type, value: type} */
    if (strcmp(tag, "Dict") == 0) {
        rec_ctx_push(ctx);
        EastValue *key_v = east_struct_get_field_idx(payload, KV_KEY);
        EastValue *val_v = east_struct_get_field_idx(payload, KV_VALUE);
        EastType *key = east_type_from_value_ctx(key_v, ctx);
        EastType *val = east_type_from_value_ctx(val_v, ctx);
        if (!key || !val) {
            if (key) east_type_release(key);
            if (val) east_type_release(val);
            return rec_ctx_pop(ctx, NULL);
        }
        EastType *t = east_dict_type(key, val);
        if (key->ref_count > 0) east_type_release(key);
        if (val->ref_count > 0) east_type_release(val);
        return rec_ctx_pop(ctx, t);
    }

    /* Struct: payload is array of {name: String, type: type} */
    if (strcmp(tag, "Struct") == 0) {
        if (!payload || payload->kind != EAST_VAL_ARRAY) return NULL;
        rec_ctx_push(ctx);
        size_t n = payload->data.array.len;
        const char **names = malloc(n * sizeof(char *));
        EastType **types = malloc(n * sizeof(EastType *));
        for (size_t i = 0; i < n; i++) {
            EastValue *field = payload->data.array.items[i];
            EastValue *name_v = east_struct_get_field_idx(field, FE_NAME);
            EastValue *type_v = east_struct_get_field_idx(field, FE_TYPE);
            names[i] = name_v->data.string.data;
            types[i] = east_type_from_value_ctx(type_v, ctx);
        }
        EastType *t = east_struct_type(names, types, n);
        for (size_t i = 0; i < n; i++) {
            if (types[i] && types[i]->ref_count > 0) east_type_release(types[i]);
        }
        free(names);
        free(types);
        return rec_ctx_pop(ctx, t);
    }

    /* Variant: payload is array of {name: String, type: type} */
    if (strcmp(tag, "Variant") == 0) {
        if (!payload || payload->kind != EAST_VAL_ARRAY) return NULL;
        rec_ctx_push(ctx);
        size_t n = payload->data.array.len;
        const char **names = malloc(n * sizeof(char *));
        EastType **types = malloc(n * sizeof(EastType *));
        for (size_t i = 0; i < n; i++) {
            EastValue *cas = payload->data.array.items[i];
            EastValue *name_v = east_struct_get_field_idx(cas, FE_NAME);
            EastValue *type_v = east_struct_get_field_idx(cas, FE_TYPE);
            names[i] = name_v->data.string.data;
            types[i] = east_type_from_value_ctx(type_v, ctx);
        }
        EastType *t = east_variant_type(names, types, n);
        for (size_t i = 0; i < n; i++) {
            if (types[i] && types[i]->ref_count > 0) east_type_release(types[i]);
        }
        free(names);
        free(types);
        return rec_ctx_pop(ctx, t);
    }

    /* Function / AsyncFunction: payload is struct {inputs: [type], output: type} */
    if (strcmp(tag, "Function") == 0 || strcmp(tag, "AsyncFunction") == 0) {
        rec_ctx_push(ctx);
        EastValue *inputs_v = east_struct_get_field_idx(payload, FN_INPUTS);
        EastValue *output_v = east_struct_get_field_idx(payload, FN_OUTPUT);
        size_t ni = inputs_v->data.array.len;
        EastType **inputs = malloc(ni * sizeof(EastType *));
        for (size_t i = 0; i < ni; i++) {
            inputs[i] = east_type_from_value_ctx(inputs_v->data.array.items[i], ctx);
        }
        EastType *output = east_type_from_value_ctx(output_v, ctx);
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
        return rec_ctx_pop(ctx, t);
    }

    /* Recursive(N): self-reference to the compound type at depth - N.
     * Resolve to the speculative wrapper at that position. */
    if (strcmp(tag, "Recursive") == 0) {
        if (payload && payload->kind == EAST_VAL_INTEGER) {
            int target = ctx->depth - (int)payload->data.integer;
            if (target >= 0 && target < ctx->depth && ctx->wrappers[target]) {
                east_type_retain(ctx->wrappers[target]);
                return ctx->wrappers[target];
            }
        }
        /* Fallback: disconnected wrapper */
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
    RecCtx ctx = { .wrappers = NULL, .depth = 0, .cap = 0 };
    EastType *result = east_type_from_value_ctx(v, &ctx);
    free(ctx.wrappers);
    return result;
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

/* Get a string field from a struct value by index. */
static inline const char *get_str_idx(EastValue *s, size_t idx)
{
    EastValue *v = east_struct_get_field_idx(s, idx);
    if (!v || v->kind != EAST_VAL_STRING) return "";
    return v->data.string.data;
}

static inline bool get_bool_idx(EastValue *s, size_t idx)
{
    EastValue *v = east_struct_get_field_idx(s, idx);
    if (!v || v->kind != EAST_VAL_BOOLEAN) return false;
    return v->data.boolean;
}

static inline EastValue *get_field_idx(EastValue *s, size_t idx)
{
    return east_struct_get_field_idx(s, idx);
}

/* Legacy name-based accessors — still used in non-hot paths */
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
    return get_str_idx(label_v, LABEL_NAME);
}

/* ================================================================== */
/*  Type interning for IR conversion                                   */
/*                                                                     */
/*  Large recursive types (e.g. UIComponentType) appear on hundreds    */
/*  of IR nodes.  Without interning, each node gets its own EastType*  */
/*  copy, causing O(N * type_size) allocation and massive GC overhead  */
/*  (GC must traverse every tracked object per collection cycle).      */
/*                                                                     */
/*  Strategy: build the type, then check if an identical type already  */
/*  exists via east_type_equal (which now handles Recursive types via  */
/*  co-inductive cycle detection).  Typically <20 unique types per IR  */
/*  so the linear scan is fast with very high hit rate.                */
/* ================================================================== */

/*
 * Type value cache: maps EastValue* type descriptors → EastType*.
 *
 * The EastValue variant tree describing a type is FINITE (Recursive
 * self-references are integer depth markers, not pointer cycles).
 * We hash the tree to a 64-bit fingerprint for O(1) lookup, falling
 * back to east_value_equal only on the rare hash collision.
 *
 * The hash is computed once per type_cache_get call.  With ~101
 * unique types and 64-bit hashes, collisions are negligible.
 */

/*
 * Type cache: maps EastValue* pointer → EastType*.
 *
 * Works because the beast2 decoder deduplicates Struct/Variant values
 * by byte range — identical bytes produce the same EastValue pointer.
 * So pointer equality is sufficient for cache lookup: O(1).
 */
typedef struct {
    EastValue **values;  /* type descriptor values (NOT retained — just pointers for comparison) */
    EastType  **types;   /* corresponding EastType* (retained) */
    size_t len;
    size_t cap;
} TypeCache;

static TypeCache ir_type_cache = { NULL, NULL, 0, 0 };

static void type_cache_init(void)
{
    ir_type_cache.len = 0;
    ir_type_cache.cap = 32;
    ir_type_cache.values = calloc(ir_type_cache.cap, sizeof(EastValue *));
    ir_type_cache.types = calloc(ir_type_cache.cap, sizeof(EastType *));
}

static void type_cache_free(void)
{
    for (size_t i = 0; i < ir_type_cache.len; i++) {
        east_type_release(ir_type_cache.types[i]);
    }
    free(ir_type_cache.values);
    free(ir_type_cache.types);
    ir_type_cache.values = NULL;
    ir_type_cache.types = NULL;
    ir_type_cache.len = 0;
    ir_type_cache.cap = 0;
}

static EastType *type_cache_get(EastValue *tv)
{
    if (!tv) return NULL;

    /* Pointer equality first (O(1) when beast2 dedup works) */
    for (size_t i = 0; i < ir_type_cache.len; i++) {
        if (ir_type_cache.values[i] == tv) {
            east_type_retain(ir_type_cache.types[i]);
            return ir_type_cache.types[i];
        }
    }

    /* Fall back to value equality for non-deduped values */
    for (size_t i = 0; i < ir_type_cache.len; i++) {
        if (east_value_equal(ir_type_cache.values[i], tv)) {
            /* Update pointer for future ptr hits */
            ir_type_cache.values[i] = tv;
            east_type_retain(ir_type_cache.types[i]);
            return ir_type_cache.types[i];
        }
    }

    /* Cache miss — build the type */
    EastType *type = east_type_from_value(tv);
    if (!type) return NULL;

    if (ir_type_cache.len >= ir_type_cache.cap) {
        ir_type_cache.cap *= 2;
        ir_type_cache.values = realloc(ir_type_cache.values,
                                       ir_type_cache.cap * sizeof(EastValue *));
        ir_type_cache.types = realloc(ir_type_cache.types,
                                      ir_type_cache.cap * sizeof(EastType *));
    }
    ir_type_cache.values[ir_type_cache.len] = tv;
    east_type_retain(type);
    ir_type_cache.types[ir_type_cache.len] = type;
    ir_type_cache.len++;

    return type;
}

/* Convert type field (EastTypeType variant) to EastType*, with caching */
static EastType *type_field(EastValue *s)
{
    EastValue *tv = east_struct_get_field_idx(s, IR_TYPE);
    return type_cache_get(tv);
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
    if (strcmp(tag, "String") == 0)
        return east_string_len(payload->data.string.data, payload->data.string.len);
    if (strcmp(tag, "DateTime") == 0) return east_datetime(payload->data.datetime);
    if (strcmp(tag, "Blob") == 0)
        return east_blob(payload->data.blob.data, payload->data.blob.len);

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

/* Convert an array of type values to EastType**, with interning */
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
        types[i] = type_cache_get(arr->data.array.items[i]);
    }
    return types;
}

/* Extract IRVariable info from a Variable IR node value */
static IRVariable var_from_ir_value(EastValue *v)
{
    IRVariable var = {0};
    if (!v || v->kind != EAST_VAL_VARIANT) return var;
    EastValue *s = v->data.variant.value;
    var.name = strdup(get_str_idx(s, VAR_NAME));
    var.mutable = get_bool_idx(s, VAR_MUTABLE);
    var.captured = get_bool_idx(s, VAR_CAPTURED);
    return var;
}

/* ================================================================== */
/*  east_ir_from_value                                                 */
/* ================================================================== */

/* Extract location array from a deserialized IR struct and set it on the node */
static void apply_location(IRNode *node, EastValue *s)
{
    if (!node || !s) return;
    EastValue *loc_arr = get_field_idx(s, IR_LOCATION);
    if (!loc_arr || loc_arr->kind != EAST_VAL_ARRAY) return;
    size_t n = loc_arr->data.array.len;
    if (n == 0) return;

    EastLocation *locs = calloc(n, sizeof(EastLocation));
    if (!locs) return;

    for (size_t i = 0; i < n; i++) {
        EastValue *loc = loc_arr->data.array.items[i];
        if (loc && loc->kind == EAST_VAL_STRUCT) {
            EastValue *fn = east_struct_get_field_idx(loc, LOC_FILENAME);
            EastValue *ln = east_struct_get_field_idx(loc, LOC_LINE);
            EastValue *col = east_struct_get_field_idx(loc, LOC_COLUMN);
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
    /* Value: { type, location, value } — field 2 = value */
    if (strcmp(tag, "Value") == 0) {
        EastValue *lit = literal_from_value(get_field_idx(s, 2));
        result = with_loc(ir_value(type, lit), s);
        east_value_release(lit); /* ir_value retained it */
        goto cleanup;
    }

    /* ----- Variable ----- */
    /* Variable: { type, location, name, mutable, captured } */
    if (strcmp(tag, "Variable") == 0) {
        result = with_loc(ir_variable(type, get_str_idx(s, VAR_NAME),
                           get_bool_idx(s, VAR_MUTABLE), get_bool_idx(s, VAR_CAPTURED)), s);
        goto cleanup;
    }

    /* ----- Let ----- */
    /* Let: { type, location, variable, value } — 2=variable, 3=value */
    if (strcmp(tag, "Let") == 0) {
        EastValue *var_v = get_field_idx(s, 2);
        IRVariable var = var_from_ir_value(var_v);
        IRNode *val = convert_ir(get_field_idx(s, 3));
        result = with_loc(ir_let(type, var.name, var.mutable, var.captured, val), s);
        ir_node_release(val);
        free(var.name);
        goto cleanup;
    }

    /* ----- Assign ----- */
    /* Assign: { type, location, variable, value } — 2=variable, 3=value */
    if (strcmp(tag, "Assign") == 0) {
        EastValue *var_v = get_field_idx(s, 2);
        const char *name = "";
        if (var_v && var_v->kind == EAST_VAL_VARIANT) {
            EastValue *vs = var_v->data.variant.value;
            name = get_str_idx(vs, VAR_NAME);
        }
        IRNode *val = convert_ir(get_field_idx(s, 3));
        result = with_loc(ir_assign(type, name, val), s);
        ir_node_release(val);
        goto cleanup;
    }

    /* ----- As (type cast - pass through) ----- */
    /* As: { type, location, value } — 2=value */
    if (strcmp(tag, "As") == 0) {
        result = convert_ir(get_field_idx(s, 2));
        goto cleanup;
    }

    /* ----- Block ----- */
    /* Block: { type, location, statements } — 2=statements */
    if (strcmp(tag, "Block") == 0) {
        size_t n;
        IRNode **stmts = convert_ir_array(get_field_idx(s, 2), &n);
        result = with_loc(ir_block(type, stmts, n), s);
        free_temp_nodes(stmts, n);
        goto cleanup;
    }

    /* ----- IfElse ----- */
    /* IfElse: { type, location, ifs, else_body } — 2=ifs, 3=else_body */
    if (strcmp(tag, "IfElse") == 0) {
        EastValue *ifs = get_field_idx(s, 2);
        IRNode *else_body = convert_ir(get_field_idx(s, 3));

        /* Chain if/elif branches from right to left */
        if (!ifs || ifs->kind != EAST_VAL_ARRAY || ifs->data.array.len == 0) {
            result = else_body;
            goto cleanup;
        }

        result = else_body;
        for (size_t i = ifs->data.array.len; i > 0; i--) {
            EastValue *branch = ifs->data.array.items[i - 1];
            IRNode *pred = convert_ir(east_struct_get_field_idx(branch, IF_PRED));
            IRNode *body = convert_ir(east_struct_get_field_idx(branch, IF_BODY));
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
    /* Match: { type, location, variant, cases } — 2=variant, 3=cases */
    if (strcmp(tag, "Match") == 0) {
        IRNode *expr = convert_ir(get_field_idx(s, 2));
        EastValue *cases_v = get_field_idx(s, 3);
        size_t nc = cases_v ? cases_v->data.array.len : 0;
        IRMatchCase *cases = calloc(nc > 0 ? nc : 1, sizeof(IRMatchCase));
        for (size_t i = 0; i < nc; i++) {
            EastValue *c = cases_v->data.array.items[i];
            cases[i].case_name = strdup(get_str_idx(c, MC_CASE));
            EastValue *var_v = east_struct_get_field_idx(c, MC_VAR);
            if (var_v && var_v->kind == EAST_VAL_VARIANT) {
                EastValue *vs = var_v->data.variant.value;
                cases[i].bind_name = strdup(get_str_idx(vs, VAR_NAME));
            }
            cases[i].body = convert_ir(east_struct_get_field_idx(c, MC_BODY));
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
    /* While: { type, location, predicate, label, body } — 2=predicate, 3=label, 4=body */
    if (strcmp(tag, "While") == 0) {
        IRNode *cond = convert_ir(get_field_idx(s, 2));
        IRNode *body = convert_ir(get_field_idx(s, 4));
        const char *label = label_from_value(get_field_idx(s, 3));
        result = with_loc(ir_while(type, cond, body, label), s);
        ir_node_release(cond);
        ir_node_release(body);
        goto cleanup;
    }

    /* ----- ForArray ----- */
    /* ForArray: { type, location, array, label, key, value, body } — 2=array, 3=label, 4=key, 5=value, 6=body */
    if (strcmp(tag, "ForArray") == 0) {
        IRNode *arr = convert_ir(get_field_idx(s, 2));
        IRNode *body = convert_ir(get_field_idx(s, 6));
        const char *label = label_from_value(get_field_idx(s, 3));
        EastValue *val_v = get_field_idx(s, 5);
        EastValue *key_v = get_field_idx(s, 4);
        const char *val_name = "";
        const char *idx_name = NULL;
        if (val_v && val_v->kind == EAST_VAL_VARIANT) {
            val_name = get_str_idx(val_v->data.variant.value, VAR_NAME);
        }
        if (key_v && key_v->kind == EAST_VAL_VARIANT) {
            idx_name = get_str_idx(key_v->data.variant.value, VAR_NAME);
        }
        result = with_loc(ir_for_array(type, val_name, idx_name, arr, body, label), s);
        ir_node_release(arr);
        ir_node_release(body);
        goto cleanup;
    }

    /* ----- ForSet ----- */
    /* ForSet: { type, location, set, label, key, body } — 2=set, 3=label, 4=key, 5=body */
    if (strcmp(tag, "ForSet") == 0) {
        IRNode *set = convert_ir(get_field_idx(s, 2));
        IRNode *body = convert_ir(get_field_idx(s, 5));
        const char *label = label_from_value(get_field_idx(s, 3));
        EastValue *key_v = get_field_idx(s, 4);
        const char *var_name = "";
        if (key_v && key_v->kind == EAST_VAL_VARIANT) {
            var_name = get_str_idx(key_v->data.variant.value, VAR_NAME);
        }
        result = with_loc(ir_for_set(type, var_name, set, body, label), s);
        ir_node_release(set);
        ir_node_release(body);
        goto cleanup;
    }

    /* ----- ForDict ----- */
    /* ForDict: { type, location, dict, label, key, value, body } — 2=dict, 3=label, 4=key, 5=value, 6=body */
    if (strcmp(tag, "ForDict") == 0) {
        IRNode *dict = convert_ir(get_field_idx(s, 2));
        IRNode *body = convert_ir(get_field_idx(s, 6));
        const char *label = label_from_value(get_field_idx(s, 3));
        EastValue *key_v = get_field_idx(s, 4);
        EastValue *val_v = get_field_idx(s, 5);
        const char *key_name = "";
        const char *val_name = "";
        if (key_v && key_v->kind == EAST_VAL_VARIANT) {
            key_name = get_str_idx(key_v->data.variant.value, VAR_NAME);
        }
        if (val_v && val_v->kind == EAST_VAL_VARIANT) {
            val_name = get_str_idx(val_v->data.variant.value, VAR_NAME);
        }
        result = with_loc(ir_for_dict(type, key_name, val_name, dict, body, label), s);
        ir_node_release(dict);
        ir_node_release(body);
        goto cleanup;
    }

    /* ----- Function / AsyncFunction ----- */
    /* Function: { type, location, captures, parameters, body } — 2=captures, 3=parameters, 4=body */
    if (strcmp(tag, "Function") == 0 || strcmp(tag, "AsyncFunction") == 0) {
        EastValue *caps_v = get_field_idx(s, 2);
        EastValue *params_v = get_field_idx(s, 3);
        IRNode *body = convert_ir(get_field_idx(s, 4));

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
    /* Call: { type, location, function, arguments } — 2=function, 3=arguments */
    if (strcmp(tag, "Call") == 0 || strcmp(tag, "CallAsync") == 0) {
        IRNode *func = convert_ir(get_field_idx(s, 2));
        size_t n;
        IRNode **args = convert_ir_array(get_field_idx(s, 3), &n);
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
    /* Platform: { type, location, name, type_parameters, arguments, async, optional } — 2=name, 3=tp, 4=args, 5=async, 6=optional */
    if (strcmp(tag, "Platform") == 0) {
        const char *name = get_str_idx(s, 2);
        bool is_async = get_bool_idx(s, 5);
        bool is_optional = get_bool_idx(s, 6);
        size_t ntp, nargs;
        EastType **tp = convert_type_array(get_field_idx(s, 3), &ntp);
        IRNode **args = convert_ir_array(get_field_idx(s, 4), &nargs);
        result = with_loc(ir_platform(type, name, tp, ntp, args, nargs, is_async, is_optional), s);
        free_temp_types(tp, ntp);
        free_temp_nodes(args, nargs);
        goto cleanup;
    }

    /* ----- Builtin ----- */
    /* Builtin: { type, location, builtin, type_parameters, arguments } — 2=builtin, 3=tp, 4=args */
    if (strcmp(tag, "Builtin") == 0) {
        const char *name = get_str_idx(s, 2);
        size_t ntp, nargs;
        EastType **tp = convert_type_array(get_field_idx(s, 3), &ntp);
        IRNode **args = convert_ir_array(get_field_idx(s, 4), &nargs);
        result = with_loc(ir_builtin(type, name, tp, ntp, args, nargs), s);
        free_temp_types(tp, ntp);
        free_temp_nodes(args, nargs);
        goto cleanup;
    }

    /* ----- Return ----- */
    /* Return: { type, location, value } — 2=value */
    if (strcmp(tag, "Return") == 0) {
        IRNode *val = convert_ir(get_field_idx(s, 2));
        result = with_loc(ir_return(type, val), s);
        ir_node_release(val);
        goto cleanup;
    }

    /* ----- Break ----- */
    /* Break: { type, location, label } — 2=label */
    if (strcmp(tag, "Break") == 0) {
        const char *label = label_from_value(get_field_idx(s, 2));
        result = with_loc(ir_break(label), s);
        goto cleanup;
    }

    /* ----- Continue ----- */
    /* Continue: { type, location, label } — 2=label */
    if (strcmp(tag, "Continue") == 0) {
        const char *label = label_from_value(get_field_idx(s, 2));
        result = with_loc(ir_continue(label), s);
        goto cleanup;
    }

    /* ----- Error ----- */
    /* Error: { type, location, message } — 2=message */
    if (strcmp(tag, "Error") == 0) {
        IRNode *msg = convert_ir(get_field_idx(s, 2));
        result = with_loc(ir_error(type, msg), s);
        ir_node_release(msg);
        goto cleanup;
    }

    /* ----- TryCatch ----- */
    /* TryCatch: { type, location, try_body, catch_body, message, stack, finally_body } — 2=try, 3=catch, 4=msg, 5=stack, 6=finally */
    if (strcmp(tag, "TryCatch") == 0) {
        IRNode *try_body = convert_ir(get_field_idx(s, 2));
        IRNode *catch_body = convert_ir(get_field_idx(s, 3));
        EastValue *msg_v = get_field_idx(s, 4);
        const char *message_var = "";
        if (msg_v && msg_v->kind == EAST_VAL_VARIANT) {
            message_var = get_str_idx(msg_v->data.variant.value, VAR_NAME);
        }
        EastValue *stack_v = get_field_idx(s, 5);
        const char *stack_var = "";
        if (stack_v && stack_v->kind == EAST_VAL_VARIANT) {
            stack_var = get_str_idx(stack_v->data.variant.value, VAR_NAME);
        }
        IRNode *finally_body = convert_ir(get_field_idx(s, 6));
        result = with_loc(ir_try_catch(type, try_body, message_var, stack_var,
                            catch_body, finally_body), s);
        ir_node_release(try_body);
        ir_node_release(catch_body);
        ir_node_release(finally_body);
        goto cleanup;
    }

    /* ----- NewArray ----- */
    /* NewArray: { type, location, values } — 2=values */
    if (strcmp(tag, "NewArray") == 0) {
        size_t n;
        IRNode **items = convert_ir_array(get_field_idx(s, 2), &n);
        result = with_loc(ir_new_array(type, items, n), s);
        free_temp_nodes(items, n);
        goto cleanup;
    }

    /* ----- NewSet ----- */
    /* NewSet: { type, location, values } — 2=values */
    if (strcmp(tag, "NewSet") == 0) {
        size_t n;
        IRNode **items = convert_ir_array(get_field_idx(s, 2), &n);
        result = with_loc(ir_new_set(type, items, n), s);
        free_temp_nodes(items, n);
        goto cleanup;
    }

    /* ----- NewDict ----- */
    /* NewDict: { type, location, values } — 2=values; each entry: { key, value } */
    if (strcmp(tag, "NewDict") == 0) {
        EastValue *vals = get_field_idx(s, 2);
        size_t n = (vals && vals->kind == EAST_VAL_ARRAY) ? vals->data.array.len : 0;
        IRNode **keys = calloc(n > 0 ? n : 1, sizeof(IRNode *));
        IRNode **values = calloc(n > 0 ? n : 1, sizeof(IRNode *));
        for (size_t i = 0; i < n; i++) {
            EastValue *entry = vals->data.array.items[i];
            keys[i] = convert_ir(east_struct_get_field_idx(entry, KV_KEY));
            values[i] = convert_ir(east_struct_get_field_idx(entry, KV_VALUE));
        }
        result = with_loc(ir_new_dict(type, keys, values, n), s);
        free_temp_nodes(keys, n);
        free_temp_nodes(values, n);
        goto cleanup;
    }

    /* ----- NewRef ----- */
    /* NewRef: { type, location, value } — 2=value */
    if (strcmp(tag, "NewRef") == 0) {
        IRNode *val = convert_ir(get_field_idx(s, 2));
        result = with_loc(ir_new_ref(type, val), s);
        ir_node_release(val);
        goto cleanup;
    }

    /* ----- NewVector ----- */
    /* NewVector: { type, location, values } — 2=values */
    if (strcmp(tag, "NewVector") == 0) {
        size_t n;
        IRNode **items = convert_ir_array(get_field_idx(s, 2), &n);
        result = with_loc(ir_new_vector(type, items, n), s);
        free_temp_nodes(items, n);
        goto cleanup;
    }

    /* ----- Struct ----- */
    /* Struct: { type, location, fields } — 2=fields; each field: { name, value } */
    if (strcmp(tag, "Struct") == 0) {
        EastValue *fields = get_field_idx(s, 2);
        size_t n = (fields && fields->kind == EAST_VAL_ARRAY) ? fields->data.array.len : 0;
        char **names = calloc(n > 0 ? n : 1, sizeof(char *));
        IRNode **values = calloc(n > 0 ? n : 1, sizeof(IRNode *));
        for (size_t i = 0; i < n; i++) {
            EastValue *f = fields->data.array.items[i];
            names[i] = strdup(get_str_idx(f, SF_NAME));
            values[i] = convert_ir(east_struct_get_field_idx(f, SF_VALUE));
        }
        result = with_loc(ir_struct(type, names, values, n), s);
        free_temp_nodes(values, n);
        for (size_t i = 0; i < n; i++) free(names[i]);
        free(names);
        goto cleanup;
    }

    /* ----- GetField ----- */
    /* GetField: { type, location, field, struct } — 2=field, 3=struct */
    if (strcmp(tag, "GetField") == 0) {
        IRNode *expr = convert_ir(get_field_idx(s, 3));
        const char *field_name = get_str_idx(s, 2);
        result = with_loc(ir_get_field(type, expr, field_name), s);
        ir_node_release(expr);
        goto cleanup;
    }

    /* ----- Variant ----- */
    /* Variant: { type, location, case, value } — 2=case, 3=value */
    if (strcmp(tag, "Variant") == 0) {
        const char *case_name = get_str_idx(s, 2);
        IRNode *val = convert_ir(get_field_idx(s, 3));
        result = with_loc(ir_variant(type, case_name, val), s);
        ir_node_release(val);
        goto cleanup;
    }

    /* ----- WrapRecursive ----- */
    /* WrapRecursive: { type, location, value } — 2=value */
    if (strcmp(tag, "WrapRecursive") == 0) {
        IRNode *val = convert_ir(get_field_idx(s, 2));
        result = with_loc(ir_wrap_recursive(type, val), s);
        ir_node_release(val);
        goto cleanup;
    }

    /* ----- UnwrapRecursive ----- */
    /* UnwrapRecursive: { type, location, value } — 2=value */
    if (strcmp(tag, "UnwrapRecursive") == 0) {
        IRNode *val = convert_ir(get_field_idx(s, 2));
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
    type_cache_init();
    IRNode *result = convert_ir(value);
    type_cache_free();
    return result;
}
