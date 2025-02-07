//
// These are definitions YJIT uses to interface with the CRuby codebase,
// but which are only used internally by YJIT.
//

#ifndef YJIT_IFACE_H
#define YJIT_IFACE_H 1

#include "ruby/ruby.h"
#include "ruby/assert.h" // for RUBY_DEBUG
#include "vm_core.h"
#include "yjit_core.h"

#if RUBY_DEBUG

#define YJIT_DECLARE_COUNTERS(...) struct rb_yjit_runtime_counters { \
    int64_t __VA_ARGS__; \
}; \
static char yjit_counter_names[] = #__VA_ARGS__;

YJIT_DECLARE_COUNTERS(
    exec_instruction,

    oswb_callsite_not_simple,
    oswb_kw_splat,
    oswb_ic_empty,
    oswb_invalid_cme,
    oswb_ivar_set_method,
    oswb_zsuper_method,
    oswb_alias_method,
    oswb_undef_method,
    oswb_optimized_method,
    oswb_missing_method,
    oswb_bmethod,
    oswb_refined_method,
    oswb_unknown_method_type,
    oswb_cfunc_ruby_array_varg,
    oswb_cfunc_argc_mismatch,
    oswb_cfunc_toomany_args,
    oswb_iseq_tailcall,
    oswb_iseq_argc_mismatch,
    oswb_iseq_not_simple,
    oswb_not_implemented_method,
    oswb_getter_arity,
    oswb_se_receiver_not_heap,
    oswb_se_cf_overflow,
    oswb_se_cc_klass_differ,
    oswb_se_protected_check_failed,

    leave_se_finish_frame,
    leave_se_interrupt,
    leave_interp_return,

    getivar_se_self_not_heap,
    getivar_idx_out_of_range,
    getivar_undef,
    getivar_name_not_mapped,
    getivar_not_object,

    setivar_se_self_not_heap,
    setivar_idx_out_of_range,
    setivar_val_heapobject,
    setivar_name_not_mapped,
    setivar_not_object,
    setivar_frozen,

    oaref_argc_not_one,
    oaref_arg_not_fixnum,

    binding_allocations,
    binding_set,

    // Member with known name for iterating over counters
    last_member
)

#undef YJIT_DECLARE_COUNTERS

struct yjit_comment {
    uint32_t offset;
    const char *comment;
};

typedef rb_darray(struct yjit_comment) yjit_comment_array_t;

extern yjit_comment_array_t yjit_code_comments;

#endif // if RUBY_DEBUG

RUBY_EXTERN struct rb_yjit_options rb_yjit_opts;
RUBY_EXTERN int64_t rb_compiled_iseq_count;
RUBY_EXTERN struct rb_yjit_runtime_counters yjit_runtime_counters;

void cb_write_pre_call_bytes(codeblock_t* cb);
void cb_write_post_call_bytes(codeblock_t* cb);

void yjit_map_addr2insn(void *code_ptr, int insn);
VALUE *yjit_iseq_pc_at_idx(const rb_iseq_t *iseq, uint32_t insn_idx);
int yjit_opcode_at_pc(const rb_iseq_t *iseq, const VALUE *pc);

void check_cfunc_dispatch(VALUE receiver, struct rb_callinfo *ci, void *callee, rb_callable_method_entry_t *compile_time_cme);
bool cfunc_needs_frame(const rb_method_cfunc_t *cfunc);

RBIMPL_ATTR_NODISCARD() bool assume_bop_not_redefined(block_t *block, int redefined_flag, enum ruby_basic_operators bop);
void assume_method_lookup_stable(VALUE receiver_klass, const rb_callable_method_entry_t *cme, block_t *block);
RBIMPL_ATTR_NODISCARD() bool assume_single_ractor_mode(block_t *block);
void assume_stable_global_constant_state(block_t *block);

// this function *must* return passed exit_pc
const VALUE *rb_yjit_count_side_exit_op(const VALUE *exit_pc);

void yjit_unlink_method_lookup_dependency(block_t *block);
void yjit_block_assumptions_free(block_t *block);

#endif // #ifndef YJIT_IFACE_
