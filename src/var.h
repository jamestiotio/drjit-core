/*
    src/var.h -- Variable/computation graph-related functions.

    Copyright (c) 2021 Wenzel Jakob <wenzel.jakob@epfl.ch>

    All rights reserved. Use of this source code is governed by a BSD-style
    license that can be found in the LICENSE file.
*/

#pragma once

#include <drjit-core/jit.h>
#include <utility>

enum VarKind : uint32_t;

struct Variable;

/// Look up a variable by its ID
extern Variable *jitc_var(uint32_t index);

/// Create a value constant variable of the given size
extern uint32_t jitc_var_literal(JitBackend backend, VarType type,
                                 const void *value, size_t size,
                                 int eval, int is_class = 0);

/// Create a variable counting from 0 ... size - 1
extern uint32_t jitc_var_counter(JitBackend backend, size_t size,
                                 bool simplify_scalar);

/// Create a variable representing the result of a custom IR statement
extern uint32_t jitc_var_stmt(JitBackend backend,
                              VarType type,
                              const char *stmt,
                              int stmt_static,
                              uint32_t n_dep,
                              const uint32_t *dep);

/// Create a new IR node. Just a wrapper around jitc_var_new without any error checking
extern uint32_t jitc_var_new_node_0(JitBackend backend, VarKind kind,
                                    VarType vt, uint32_t size, bool symbolic,
                                    uint64_t payload = 0);

extern uint32_t jitc_var_new_node_1(JitBackend backend, VarKind kind,
                                    VarType vt, uint32_t size, bool symbolic,
                                    uint32_t a0, Variable *v0,
                                    uint64_t payload = 0);

extern uint32_t jitc_var_new_node_2(JitBackend backend, VarKind kind,
                                    VarType vt, uint32_t size, bool symbolic,
                                    uint32_t a0, Variable *v0, uint32_t a1, Variable *v1,
                                    uint64_t payload = 0);

extern uint32_t jitc_var_new_node_3(JitBackend backend, VarKind kind,
                                    VarType vt, uint32_t size, bool symbolic,
                                    uint32_t a0, Variable *v0, uint32_t a1, Variable *v1,
                                    uint32_t a2, Variable *v2, uint64_t payload = 0);

extern uint32_t jitc_var_new_node_4(JitBackend backend, VarKind kind,
                                    VarType vt, uint32_t size, bool symbolic,
                                    uint32_t a0, Variable *v0, uint32_t a1, Variable *v1,
                                    uint32_t a2, Variable *v2, uint32_t a3, Variable *v4,
                                    uint64_t payload = 0);

/// Create a variable that refers to a memory region
extern uint32_t jitc_var_pointer(JitBackend backend, const void *value,
                                 uint32_t dep, int write);

/// Wrap an input variable of a virtual function call before recording computation
extern uint32_t jitc_var_wrap_vcall(uint32_t index);

/// Register an existing variable with the JIT compiler
extern uint32_t jitc_var_mem_map(JitBackend backend, VarType type, void *ptr,
                                 size_t size, int free);

/// Copy a memory region onto the device and return its variable index
extern uint32_t jitc_var_mem_copy(JitBackend backend, AllocType atype,
                                  VarType vtype, const void *ptr,
                                  size_t size);

/// Duplicate a variable
extern uint32_t jitc_var_copy(uint32_t index);

/// Create a resized copy of a variable
extern uint32_t jitc_var_resize(uint32_t index, size_t size);

/// Increase the external reference count of a given variable
extern void jitc_var_inc_ref(uint32_t index, Variable *v) noexcept;

/// Increase the external reference count of a given variable
extern void jitc_var_inc_ref(uint32_t index) noexcept;

/// Increase the external reference count of a given variable
inline uint32_t jitc_var_new_ref(uint32_t index) noexcept {
    jitc_var_inc_ref(index);
    return index;
}

/// Decrease the external reference count of a given variable
extern void jitc_var_dec_ref(uint32_t index, Variable *v) noexcept;

/// Decrease the external reference count of a given variable
extern void jitc_var_dec_ref(uint32_t index) noexcept;

/// Increase the side effect reference count of a given variable
extern void jitc_var_inc_ref_se(uint32_t index, Variable *v) noexcept;

/// Increase the side effect reference count of a given variable
extern void jitc_var_inc_ref_se(uint32_t index) noexcept;

/// Decrease the side effect reference count of a given variable
extern void jitc_var_dec_ref_se(uint32_t index, Variable *v) noexcept;

/// Decrease the side effect reference count of a given variable
extern void jitc_var_dec_ref_se(uint32_t index) noexcept;

// Query the type of a given variable
extern VarType jitc_var_type(uint32_t index);

/// Assign a descriptive label to a variable with only 1 reference
extern void jitc_var_set_label(uint32_t index, const char *label);

/// Query the descriptive label associated with a given variable
extern const char *jitc_var_label(uint32_t index);

/// Assign a callback function that is invoked when the given variable is freed
extern void jitc_var_set_callback(uint32_t index,
                                  void (*callback)(uint32_t, int, void *),
                                  void *payload);

/// Migrate a variable to a different flavor of memory
extern uint32_t jitc_var_migrate(uint32_t index, AllocType type);

/// Indicate to the JIT compiler that a variable has side effects
extern void jitc_var_mark_side_effect(uint32_t index);

/// Return a human-readable summary of the contents of a variable
const char *jitc_var_str(uint32_t index);

/// Read a single element of a variable and write it to 'dst'
extern void jitc_var_read(uint32_t index, size_t offset, void *dst);

/// Reverse of jitc_var_read(). Copy 'src' to a single element of a variable
extern uint32_t jitc_var_write(uint32_t index, size_t offset, const void *src);

/// Schedule a variable \c index for future evaluation via \ref jit_eval()
extern int jitc_var_schedule(uint32_t index);

/// Evaluate a value constant variable
extern void jitc_var_eval_literal(uint32_t index, Variable *v);

/// Evaluate the variable \c index right away, if it is unevaluated/dirty.
extern int jitc_var_eval(uint32_t index);

/// Return the pointer location of the variable, evaluate if needed
extern void *jitc_var_ptr(uint32_t index);

/// Return a human-readable summary of registered variables
extern const char *jitc_var_whos();

/// Return a GraphViz representation of registered variables
extern const char *jitc_var_graphviz();

/// Remove a variable from the cache used for common subexpression elimination
extern void jitc_lvn_drop(uint32_t index, const Variable *v);

/// Register a variable with cache used for common subexpression elimination
extern void jitc_lvn_put(uint32_t index, const Variable *v);

/// Append the given variable to the instruction trace and return its ID
extern uint32_t jitc_var_new(Variable &v, bool disable_lvn = false);

/// Query the current (or future, if not yet evaluated) allocation flavor of a variable
extern AllocType jitc_var_alloc_type(uint32_t index);

/// Query the device (or future, if not yet evaluated) associated with a variable
extern int jitc_var_device(uint32_t index);

/// Return a mask of currently active lanes
extern uint32_t jitc_var_mask_peek(JitBackend backend);

/// Push an active mask
extern void jitc_var_mask_push(JitBackend backend, uint32_t index);

/// Pop an active mask
extern void jitc_var_mask_pop(JitBackend backend);

/// Combine the given mask 'index' with the mask stack. 'size' indicates the wavefront size
extern uint32_t jitc_var_mask_apply(uint32_t index, uint32_t size);

/// Return the default mask
extern uint32_t jitc_var_mask_default(JitBackend backend, size_t size);

/// Start a new scope of the program being recorded
extern uint32_t jitc_new_scope(JitBackend backend);

/// Reduce (And) a boolean array to a single value, synchronizes.
extern bool jitc_var_all(uint32_t index);

/// Reduce (Or) a boolean array to a single value, synchronizes.
extern bool jitc_var_any(uint32_t index);

/// Reduce a variable to a single value
extern uint32_t jitc_var_reduce(JitBackend backend, VarType vt,
                                ReduceOp reduce_op, uint32_t index);

/// Compute an inclusive or exclusive prefix sum of a given variable
extern uint32_t jitc_var_prefix_sum(uint32_t index, bool exclusive);

/// Create a variable containing the buffer storing a specific attribute
extern uint32_t jitc_var_registry_attr(JitBackend backend, VarType type,
                                       const char *domain, const char *name);

/// Return an implicit mask for operations within a virtual function call
extern uint32_t jitc_var_vcall_mask(JitBackend);

/// Descriptive names and byte sizes for the various variable types
extern const char *type_name      [(int) VarType::Count];
extern const char *type_name_short[(int) VarType::Count];
extern const uint32_t type_size   [(int) VarType::Count];

/// Type names and register names for CUDA and LLVM
extern const char *type_name_llvm       [(int) VarType::Count];
extern const char *type_name_llvm_bin   [(int) VarType::Count];
extern const char *type_name_llvm_abbrev[(int) VarType::Count];
extern const char *type_name_llvm_big   [(int) VarType::Count];
extern const char *type_name_ptx        [(int) VarType::Count];
extern const char *type_name_ptx_bin    [(int) VarType::Count];
extern const char *type_prefix          [(int) VarType::Count];
extern const char *type_size_str        [(int) VarType::Count];
