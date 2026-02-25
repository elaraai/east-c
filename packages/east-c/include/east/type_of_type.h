#ifndef EAST_TYPE_OF_TYPE_H
#define EAST_TYPE_OF_TYPE_H

#include "types.h"
#include "values.h"
#include "ir.h"

/*
 * Type descriptors for East's homoiconic type system.
 *
 * East types and IR nodes are themselves East values (variants).
 * These type descriptors allow JSON (de)serialization of types and IR:
 *
 *   EastValue *ir_val = east_json_decode(json, east_ir_type);
 *   IRNode *ir = east_ir_from_value(ir_val);
 *
 * Call east_type_of_type_init() once before using these globals.
 */

// Type descriptors (initialized by east_type_of_type_init)
extern EastType *east_type_type;          // Recursive variant: 19 type cases
extern EastType *east_literal_value_type; // Variant: 7 literal value cases
extern EastType *east_ir_type;            // Recursive variant: 34 IR node cases

// Initialize the type descriptors. Call once at startup.
void east_type_of_type_init(void);

// Convert decoded EastTypeType variant value -> EastType*
EastType *east_type_from_value(EastValue *value);

// Convert EastType* -> EastTypeType variant value
EastValue *east_type_to_value(EastType *type);

// Convert decoded IRType variant value -> IRNode*
IRNode *east_ir_from_value(EastValue *value);

#endif
