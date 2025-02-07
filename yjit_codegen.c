#include "insns.inc"
#include "internal.h"
#include "vm_core.h"
#include "vm_sync.h"
#include "vm_callinfo.h"
#include "builtin.h"
#include "internal/compile.h"
#include "internal/class.h"
#include "internal/object.h"
#include "insns_info.inc"
#include "yjit.h"
#include "yjit_iface.h"
#include "yjit_core.h"
#include "yjit_codegen.h"
#include "yjit_asm.h"
#include "yjit_utils.h"

// Map from YARV opcodes to code generation functions
static codegen_fn gen_fns[VM_INSTRUCTION_SIZE] = { NULL };

// Code block into which we write machine code
static codeblock_t block;
codeblock_t* cb = NULL;

// Code block into which we write out-of-line machine code
static codeblock_t outline_block;
codeblock_t* ocb = NULL;

// Code for exiting back to the interpreter from the leave insn
static void *leave_exit_code;

// Print the current source location for debugging purposes
RBIMPL_ATTR_MAYBE_UNUSED()
static void
jit_print_loc(jitstate_t* jit, const char* msg)
{
    char *ptr;
    long len;
    VALUE path = rb_iseq_path(jit->iseq);
    RSTRING_GETMEM(path, ptr, len);
    fprintf(stderr, "%s %.*s:%u\n", msg, (int)len, ptr, rb_iseq_line_no(jit->iseq, jit->insn_idx));
}

// Get the current instruction's opcode
static int
jit_get_opcode(jitstate_t* jit)
{
    return yjit_opcode_at_pc(jit->iseq, jit->pc);
}

// Get the index of the next instruction
static uint32_t
jit_next_insn_idx(jitstate_t* jit)
{
    return jit->insn_idx + insn_len(jit_get_opcode(jit));
}

// Get an instruction argument by index
static VALUE
jit_get_arg(jitstate_t* jit, size_t arg_idx)
{
    RUBY_ASSERT(arg_idx + 1 < (size_t)insn_len(jit_get_opcode(jit)));
    return *(jit->pc + arg_idx + 1);
}

// Load a VALUE into a register and keep track of the reference if it is on the GC heap.
static void
jit_mov_gc_ptr(jitstate_t* jit, codeblock_t* cb, x86opnd_t reg, VALUE ptr)
{
    RUBY_ASSERT(reg.type == OPND_REG && reg.num_bits == 64);

    // Load the pointer constant into the specified register
    mov(cb, reg, const_ptr_opnd((void*)ptr));

    // The pointer immediate is encoded as the last part of the mov written out
    uint32_t ptr_offset = cb->write_pos - sizeof(VALUE);

    if (!SPECIAL_CONST_P(ptr)) {
        if (!rb_darray_append(&jit->block->gc_object_offsets, ptr_offset)) {
            rb_bug("allocation failed");
        }
    }
}

// Check if we are compiling the instruction at the stub PC
// Meaning we are compiling the instruction that is next to execute
static bool
jit_at_current_insn(jitstate_t* jit)
{
    const VALUE* ec_pc = jit->ec->cfp->pc;
    return (ec_pc == jit->pc);
}

// Peek at the nth topmost value on the Ruby stack.
// Returns the topmost value when n == 0.
static VALUE
jit_peek_at_stack(jitstate_t* jit, ctx_t* ctx, int n)
{
    RUBY_ASSERT(jit_at_current_insn(jit));

    // Note: this does not account for ctx->sp_offset because
    // this is only available when hitting a stub, and while
    // hitting a stub, cfp->sp needs to be up to date in case
    // codegen functions trigger GC. See :stub-sp-flush:.
    VALUE *sp = jit->ec->cfp->sp;

    return *(sp - 1 - n);
}

static VALUE
jit_peek_at_self(jitstate_t *jit, ctx_t *ctx)
{
    return jit->ec->cfp->self;
}

static bool jit_guard_known_klass(jitstate_t *jit, ctx_t* ctx, VALUE known_klass, insn_opnd_t insn_opnd, const int max_chain_depth, uint8_t *side_exit);

#if RUBY_DEBUG

// Increment a profiling counter with counter_name
#define GEN_COUNTER_INC(cb, counter_name) _gen_counter_inc(cb, &(yjit_runtime_counters . counter_name))
static void
_gen_counter_inc(codeblock_t *cb, int64_t *counter)
{
    if (!rb_yjit_opts.gen_stats) return;
     mov(cb, REG0, const_ptr_opnd(counter));
     cb_write_lock_prefix(cb); // for ractors.
     add(cb, mem_opnd(64, REG0, 0), imm_opnd(1));
}

// Increment a counter then take an existing side exit.
#define COUNTED_EXIT(side_exit, counter_name) _counted_side_exit(side_exit, &(yjit_runtime_counters . counter_name))
static uint8_t *
_counted_side_exit(uint8_t *existing_side_exit, int64_t *counter)
{
    if (!rb_yjit_opts.gen_stats) return existing_side_exit;

    uint8_t *start = cb_get_ptr(ocb, ocb->write_pos);
    _gen_counter_inc(ocb, counter);
    jmp_ptr(ocb, existing_side_exit);
    return start;
}

// Add a comment at the current position in the code block
static void
_add_comment(codeblock_t* cb, const char* comment_str)
{
    // Avoid adding duplicate comment strings (can happen due to deferred codegen)
    size_t num_comments = rb_darray_size(yjit_code_comments);
    if (num_comments > 0) {
        struct yjit_comment last_comment = rb_darray_get(yjit_code_comments, num_comments - 1);
        if (last_comment.offset == cb->write_pos && strcmp(last_comment.comment, comment_str) == 0) {
            return;
        }
    }

    struct yjit_comment new_comment = (struct yjit_comment){ cb->write_pos, comment_str };
    rb_darray_append(&yjit_code_comments, new_comment);
}

// Comments for generated machine code
#define ADD_COMMENT(cb, comment) _add_comment((cb), (comment))
yjit_comment_array_t yjit_code_comments;

#else

#define GEN_COUNTER_INC(cb, counter_name) ((void)0)
#define COUNTED_EXIT(side_exit, counter_name) side_exit
#define ADD_COMMENT(cb, comment) ((void)0)

#endif // if RUBY_DEBUG

// Save YJIT registers prior to a C call
static void
yjit_save_regs(codeblock_t* cb)
{
    push(cb, REG_CFP);
    push(cb, REG_EC);
    push(cb, REG_SP);
    push(cb, REG_SP); // Maintain 16-byte RSP alignment
}

// Restore YJIT registers after a C call
static void
yjit_load_regs(codeblock_t* cb)
{
    pop(cb, REG_SP); // Maintain 16-byte RSP alignment
    pop(cb, REG_SP);
    pop(cb, REG_EC);
    pop(cb, REG_CFP);
}

// Generate an inline exit to return to the interpreter
static uint8_t *
yjit_gen_exit(jitstate_t *jit, ctx_t *ctx, codeblock_t *cb)
{
    uint8_t *code_ptr = cb_get_ptr(cb, cb->write_pos);

    VALUE *exit_pc = jit->pc;

    // YJIT only ever patches the first instruction in an iseq
    if (jit->insn_idx == 0) {
        // Table mapping opcodes to interpreter handlers
        const void *const *handler_table = rb_vm_get_insns_address_table();

        // Write back the old instruction at the exit PC
        // Otherwise the interpreter may jump right back to the
        // JITted code we're trying to exit
        int exit_opcode = yjit_opcode_at_pc(jit->iseq, exit_pc);
        void* handler_addr = (void*)handler_table[exit_opcode];
        mov(cb, REG0, const_ptr_opnd(exit_pc));
        mov(cb, REG1, const_ptr_opnd(handler_addr));
        mov(cb, mem_opnd(64, REG0, 0), REG1);
    }

    // Generate the code to exit to the interpreters
    // Write the adjusted SP back into the CFP
    if (ctx->sp_offset != 0) {
        x86opnd_t stack_pointer = ctx_sp_opnd(ctx, 0);
        lea(cb, REG_SP, stack_pointer);
        mov(cb, member_opnd(REG_CFP, rb_control_frame_t, sp), REG_SP);
    }

    // Update the CFP on the EC
    mov(cb, member_opnd(REG_EC, rb_execution_context_t, cfp), REG_CFP);

    // Put PC into the return register, which the post call bytes dispatches to
    mov(cb, RAX, const_ptr_opnd(exit_pc));
    mov(cb, member_opnd(REG_CFP, rb_control_frame_t, pc), RAX);

    // Accumulate stats about interpreter exits
#if RUBY_DEBUG
    if (rb_yjit_opts.gen_stats) {
        mov(cb, RDI, const_ptr_opnd(exit_pc));
        call_ptr(cb, RSI, (void *)&rb_yjit_count_side_exit_op);
    }
#endif

    cb_write_post_call_bytes(cb);

    return code_ptr;
}

// Generate a continuation for gen_leave() that exits to the interpreter at REG_CFP->pc.
static uint8_t *
yjit_gen_leave_exit(codeblock_t *cb)
{
    uint8_t *code_ptr = cb_get_ptr(cb, cb->write_pos);

    // Note, gen_leave() fully reconstructs interpreter state before
    // coming here.

    // Every exit to the interpreter should be counted
    GEN_COUNTER_INC(cb, leave_interp_return);

    // Put PC into the return register, which the post call bytes dispatches to
    mov(cb, RAX, member_opnd(REG_CFP, rb_control_frame_t, pc));

    cb_write_post_call_bytes(cb);

    return code_ptr;
}

// A shorthand for generating an exit in the outline block
static uint8_t *
yjit_side_exit(jitstate_t *jit, ctx_t *ctx)
{
    return yjit_gen_exit(jit, ctx, ocb);
}

/*
Compile an interpreter entry block to be inserted into an iseq
Returns `NULL` if compilation fails.
*/
uint8_t *
yjit_entry_prologue(void)
{
    RUBY_ASSERT(cb != NULL);

    if (cb->write_pos + 1024 >= cb->mem_size) {
        rb_bug("out of executable memory");
    }

    // Align the current write positon to cache line boundaries
    cb_align_pos(cb, 64);

    uint8_t *code_ptr = cb_get_ptr(cb, cb->write_pos);

    // Write the interpreter entry prologue
    cb_write_pre_call_bytes(cb);

    // Load the current SP from the CFP into REG_SP
    mov(cb, REG_SP, member_opnd(REG_CFP, rb_control_frame_t, sp));

    // Setup cfp->jit_return
    // TODO: this could use an IP relative LEA instead of an 8 byte immediate
    mov(cb, REG0, const_ptr_opnd(leave_exit_code));
    mov(cb, member_opnd(REG_CFP, rb_control_frame_t, jit_return), REG0);

    return code_ptr;
}

// Generate code to check for interrupts and take a side-exit
static void
yjit_check_ints(codeblock_t* cb, uint8_t* side_exit)
{
    // Check for interrupts
    // see RUBY_VM_CHECK_INTS(ec) macro
    ADD_COMMENT(cb, "RUBY_VM_CHECK_INTS(ec)");
    mov(cb, REG0_32, member_opnd(REG_EC, rb_execution_context_t, interrupt_mask));
    not(cb, REG0_32);
    test(cb, member_opnd(REG_EC, rb_execution_context_t, interrupt_flag), REG0_32);
    jnz_ptr(cb, side_exit);
}

// Generate a stubbed unconditional jump to the next bytecode instruction.
// Blocks that are part of a guard chain can use this to share the same successor.
static void
jit_jump_to_next_insn(jitstate_t *jit, const ctx_t *current_context)
{
    // Reset the depth since in current usages we only ever jump to to
    // chain_depth > 0 from the same instruction.
    ctx_t reset_depth = *current_context;
    reset_depth.chain_depth = 0;

    blockid_t jump_block = { jit->iseq, jit_next_insn_idx(jit) };

    // Generate the jump instruction
    gen_direct_jump(
        jit->block,
        &reset_depth,
        jump_block
    );
}

// Compile a sequence of bytecode instructions for a given basic block version
void
yjit_gen_block(block_t *block, rb_execution_context_t *ec)
{
    RUBY_ASSERT(cb != NULL);
    RUBY_ASSERT(block != NULL);
    RUBY_ASSERT(!(block->blockid.idx == 0 && block->ctx.stack_size > 0));

    // Copy the block's context to avoid mutating it
    ctx_t ctx_copy = block->ctx;
    ctx_t* ctx = &ctx_copy;

    const rb_iseq_t *iseq = block->blockid.iseq;
    uint32_t insn_idx = block->blockid.idx;
    const uint32_t starting_insn_idx = insn_idx;

    // NOTE: if we are ever deployed in production, we
    // should probably just log an error and return NULL here,
    // so we can fail more gracefully
    if (cb->write_pos + 1024 >= cb->mem_size) {
        rb_bug("out of executable memory");
    }
    if (ocb->write_pos + 1024 >= ocb->mem_size) {
        rb_bug("out of executable memory (outlined block)");
    }

    // Initialize a JIT state object
    jitstate_t jit = {
        block,
        iseq,
        0,
        0,
        ec
    };

    // Mark the start position of the block
    block->start_pos = cb->write_pos;

    // For each instruction to compile
    for (;;) {
        // Get the current pc and opcode
        VALUE *pc = yjit_iseq_pc_at_idx(iseq, insn_idx);
        int opcode = yjit_opcode_at_pc(iseq, pc);
        RUBY_ASSERT(opcode >= 0 && opcode < VM_INSTRUCTION_SIZE);

        // opt_getinlinecache wants to be in a block all on its own. Cut the block short
        // if we run into it. See gen_opt_getinlinecache for details.
        if (opcode == BIN(opt_getinlinecache) && insn_idx > starting_insn_idx) {
            jit_jump_to_next_insn(&jit, ctx);
            break;
        }

        // Set the current instruction
        jit.insn_idx = insn_idx;
        jit.pc = pc;

        // Lookup the codegen function for this instruction
        codegen_fn gen_fn = gen_fns[opcode];
        if (!gen_fn) {
            // If we reach an unknown instruction,
            // exit to the interpreter and stop compiling
            yjit_gen_exit(&jit, ctx, cb);
            break;
        }

        if (0) {
            fprintf(stderr, "compiling %d: %s\n", insn_idx, insn_name(opcode));
            print_str(cb, insn_name(opcode));
        }

        // :count-placement:
        // Count bytecode instructions that execute in generated code.
        // Note that the increment happens even when the output takes side exit.
        GEN_COUNTER_INC(cb, exec_instruction);

        // Add a comment for the name of the YARV instruction
        ADD_COMMENT(cb, insn_name(opcode));

        // Call the code generation function
        codegen_status_t status = gen_fn(&jit, ctx);

        // For now, reset the chain depth after each instruction as only the
        // first instruction in the block can concern itself with the depth.
        ctx->chain_depth = 0;

        // If we can't compile this instruction
        // exit to the interpreter and stop compiling
        if (status == YJIT_CANT_COMPILE) {
            // TODO: if the codegen funcion makes changes to ctx and then return YJIT_CANT_COMPILE,
            // the exit this generates would be wrong. We could save a copy of the entry context
            // and assert that ctx is the same here.
            yjit_gen_exit(&jit, ctx, cb);
            break;
        }

        // Move to the next instruction to compile
        insn_idx += insn_len(opcode);

        // If the instruction terminates this block
        if (status == YJIT_END_BLOCK) {
            break;
        }
    }

    // Mark the end position of the block
    block->end_pos = cb->write_pos;

    // Store the index of the last instruction in the block
    block->end_idx = insn_idx;

    if (YJIT_DUMP_MODE >= 2) {
        // Dump list of compiled instrutions
        fprintf(stderr, "Compiled the following for iseq=%p:\n", (void *)iseq);
        for (uint32_t idx = block->blockid.idx; idx < insn_idx; ) {
            int opcode = yjit_opcode_at_pc(iseq, yjit_iseq_pc_at_idx(iseq, idx));
            fprintf(stderr, "  %04d %s\n", idx, insn_name(opcode));
            idx += insn_len(opcode);
        }
    }
}

static codegen_status_t
gen_dup(jitstate_t* jit, ctx_t* ctx)
{
    // Get the top value and its type
    val_type_t dup_type = ctx_get_opnd_type(ctx, OPND_STACK(0));
    x86opnd_t dup_val = ctx_stack_pop(ctx, 0);

    // Push the same value on top
    x86opnd_t loc0 = ctx_stack_push(ctx, dup_type);
    mov(cb, REG0, dup_val);
    mov(cb, loc0, REG0);

    return YJIT_KEEP_COMPILING;
}

static codegen_status_t
gen_nop(jitstate_t* jit, ctx_t* ctx)
{
    // Do nothing
    return YJIT_KEEP_COMPILING;
}

static codegen_status_t
gen_pop(jitstate_t* jit, ctx_t* ctx)
{
    // Decrement SP
    ctx_stack_pop(ctx, 1);
    return YJIT_KEEP_COMPILING;
}

static codegen_status_t
gen_putnil(jitstate_t* jit, ctx_t* ctx)
{
    // Write constant at SP
    x86opnd_t stack_top = ctx_stack_push(ctx, TYPE_NIL);
    mov(cb, stack_top, imm_opnd(Qnil));
    return YJIT_KEEP_COMPILING;
}

static codegen_status_t
gen_putobject(jitstate_t* jit, ctx_t* ctx)
{
    VALUE arg = jit_get_arg(jit, 0);

    if (FIXNUM_P(arg))
    {
        // Keep track of the fixnum type tag
        x86opnd_t stack_top = ctx_stack_push(ctx, TYPE_FIXNUM);
        x86opnd_t imm = imm_opnd((int64_t)arg);

        // 64-bit immediates can't be directly written to memory
        if (imm.num_bits <= 32)
        {
            mov(cb, stack_top, imm);
        }
        else
        {
            mov(cb, REG0, imm);
            mov(cb, stack_top, REG0);
        }
    }
    else if (arg == Qtrue || arg == Qfalse)
    {
        x86opnd_t stack_top = ctx_stack_push(ctx, TYPE_IMM);
        mov(cb, stack_top, imm_opnd((int64_t)arg));
    }
    else
    {
        // Load the value to push into REG0
        // Note that this value may get moved by the GC
        VALUE put_val = jit_get_arg(jit, 0);
        jit_mov_gc_ptr(jit, cb, REG0, put_val);

        // TODO: check for more specific types like array, string, symbol, etc.
        val_type_t val_type = SPECIAL_CONST_P(put_val)? TYPE_IMM:TYPE_HEAP;

        // Write argument at SP
        x86opnd_t stack_top = ctx_stack_push(ctx, val_type);
        mov(cb, stack_top, REG0);
    }

    return YJIT_KEEP_COMPILING;
}

static codegen_status_t
gen_putobject_int2fix(jitstate_t* jit, ctx_t* ctx)
{
    int opcode = jit_get_opcode(jit);
    int cst_val = (opcode == BIN(putobject_INT2FIX_0_))? 0:1;

    // Write constant at SP
    x86opnd_t stack_top = ctx_stack_push(ctx, TYPE_FIXNUM);
    mov(cb, stack_top, imm_opnd(INT2FIX(cst_val)));

    return YJIT_KEEP_COMPILING;
}

static codegen_status_t
gen_putself(jitstate_t* jit, ctx_t* ctx)
{
    // Load self from CFP
    mov(cb, REG0, member_opnd(REG_CFP, rb_control_frame_t, self));

    // Write it on the stack
    x86opnd_t stack_top = ctx_stack_push_self(ctx);
    mov(cb, stack_top, REG0);

    return YJIT_KEEP_COMPILING;
}

// Compute the index of a local variable from its slot index
uint32_t slot_to_local_idx(const rb_iseq_t *iseq, int32_t slot_idx)
{
    // Convoluted rules from local_var_name() in iseq.c
    int32_t local_table_size = iseq->body->local_table_size;
    int32_t op = slot_idx - VM_ENV_DATA_SIZE;
    int32_t local_idx = local_idx = local_table_size - op - 1;
    RUBY_ASSERT(local_idx >= 0 && local_idx < local_table_size);
    return (uint32_t)local_idx;
}

static codegen_status_t
gen_getlocal_wc0(jitstate_t* jit, ctx_t* ctx)
{
    // Compute the offset from BP to the local
    int32_t slot_idx = (int32_t)jit_get_arg(jit, 0);
    const int32_t offs = -(SIZEOF_VALUE * slot_idx);
    uint32_t local_idx = slot_to_local_idx(jit->iseq, slot_idx);

    // Load environment pointer EP from CFP
    mov(cb, REG0, member_opnd(REG_CFP, rb_control_frame_t, ep));

    // Load the local from the EP
    mov(cb, REG0, mem_opnd(64, REG0, offs));

    // Write the local at SP
    x86opnd_t stack_top = ctx_stack_push_local(ctx, local_idx);
    mov(cb, stack_top, REG0);

    return YJIT_KEEP_COMPILING;
}

static codegen_status_t
gen_getlocal_wc1(jitstate_t* jit, ctx_t* ctx)
{
    //fprintf(stderr, "gen_getlocal_wc1\n");

    // Load environment pointer EP from CFP
    mov(cb, REG0, member_opnd(REG_CFP, rb_control_frame_t, ep));

    // Get the previous EP from the current EP
    // See GET_PREV_EP(ep) macro
    // VALUE* prev_ep = ((VALUE *)((ep)[VM_ENV_DATA_INDEX_SPECVAL] & ~0x03))
    mov(cb, REG0, mem_opnd(64, REG0, SIZEOF_VALUE * VM_ENV_DATA_INDEX_SPECVAL));
    and(cb, REG0, imm_opnd(~0x03));

    // Load the local from the block
    // val = *(vm_get_ep(GET_EP(), level) - idx);
    int32_t local_idx = (int32_t)jit_get_arg(jit, 0);
    const int32_t offs = -(SIZEOF_VALUE * local_idx);
    mov(cb, REG0, mem_opnd(64, REG0, offs));

    // Write the local at SP
    x86opnd_t stack_top = ctx_stack_push(ctx, TYPE_UNKNOWN);
    mov(cb, stack_top, REG0);

    return YJIT_KEEP_COMPILING;
}

static codegen_status_t
gen_setlocal_wc0(jitstate_t* jit, ctx_t* ctx)
{
    /*
    vm_env_write(const VALUE *ep, int index, VALUE v)
    {
        VALUE flags = ep[VM_ENV_DATA_INDEX_FLAGS];
        if (LIKELY((flags & VM_ENV_FLAG_WB_REQUIRED) == 0)) {
    	VM_STACK_ENV_WRITE(ep, index, v);
        }
        else {
    	vm_env_write_slowpath(ep, index, v);
        }
    }
    */

    int32_t slot_idx = (int32_t)jit_get_arg(jit, 0);
    uint32_t local_idx = slot_to_local_idx(jit->iseq, slot_idx);

    // Load environment pointer EP from CFP
    mov(cb, REG0, member_opnd(REG_CFP, rb_control_frame_t, ep));

    // flags & VM_ENV_FLAG_WB_REQUIRED
    x86opnd_t flags_opnd = mem_opnd(64, REG0, sizeof(VALUE) * VM_ENV_DATA_INDEX_FLAGS);
    test(cb, flags_opnd, imm_opnd(VM_ENV_FLAG_WB_REQUIRED));

    // Create a size-exit to fall back to the interpreter
    uint8_t *side_exit = yjit_side_exit(jit, ctx);

    // if (flags & VM_ENV_FLAG_WB_REQUIRED) != 0
    jnz_ptr(cb, side_exit);

    // Set the type of the local variable in the context
    val_type_t temp_type = ctx_get_opnd_type(ctx, OPND_STACK(0));
    ctx_set_local_type(ctx, local_idx, temp_type);

    // Pop the value to write from the stack
    x86opnd_t stack_top = ctx_stack_pop(ctx, 1);
    mov(cb, REG1, stack_top);

    // Write the value at the environment pointer
    const int32_t offs = -8 * slot_idx;
    mov(cb, mem_opnd(64, REG0, offs), REG1);

    return YJIT_KEEP_COMPILING;
}

// Check that `self` is a pointer to an object on the GC heap
static void
guard_self_is_heap(codeblock_t *cb, x86opnd_t self_opnd, uint8_t *side_exit, ctx_t *ctx)
{

    // `self` is constant throughout the entire region, so we only need to do this check once.
    if (!ctx->self_type.is_heap) {
        ADD_COMMENT(cb, "guard self is heap");
        RUBY_ASSERT(Qfalse < Qnil);
        test(cb, self_opnd, imm_opnd(RUBY_IMMEDIATE_MASK));
        jnz_ptr(cb, side_exit);
        cmp(cb, self_opnd, imm_opnd(Qnil));
        jbe_ptr(cb, side_exit);

        ctx->self_type.is_heap = 1;
    }
}

static void
gen_jnz_to_target0(codeblock_t *cb, uint8_t *target0, uint8_t *target1, uint8_t shape)
{
    switch (shape)
    {
        case SHAPE_NEXT0:
        case SHAPE_NEXT1:
        RUBY_ASSERT(false);
        break;

        case SHAPE_DEFAULT:
        jnz_ptr(cb, target0);
        break;
    }
}

static void
gen_jz_to_target0(codeblock_t *cb, uint8_t *target0, uint8_t *target1, uint8_t shape)
{
    switch (shape)
    {
        case SHAPE_NEXT0:
        case SHAPE_NEXT1:
        RUBY_ASSERT(false);
        break;

        case SHAPE_DEFAULT:
        jz_ptr(cb, target0);
        break;
    }
}

enum jcc_kinds {
    JCC_JNE,
    JCC_JNZ,
    JCC_JZ,
    JCC_JE,
};

// Generate a jump to a stub that recompiles the current YARV instruction on failure.
// When depth_limitk is exceeded, generate a jump to a side exit.
static void
jit_chain_guard(enum jcc_kinds jcc, jitstate_t *jit, const ctx_t *ctx, uint8_t depth_limit, uint8_t *side_exit)
{
    branchgen_fn target0_gen_fn;

    switch (jcc) {
        case JCC_JNE:
        case JCC_JNZ:
            target0_gen_fn = gen_jnz_to_target0;
            break;
        case JCC_JZ:
        case JCC_JE:
            target0_gen_fn = gen_jz_to_target0;
            break;
        default:
            RUBY_ASSERT(false && "unimplemented jump kind");
            break;
    };

    if (ctx->chain_depth < depth_limit) {
        ctx_t deeper = *ctx;
        deeper.chain_depth++;

        gen_branch(
            jit->block,
            ctx,
            (blockid_t) { jit->iseq, jit->insn_idx },
            &deeper,
            BLOCKID_NULL,
            NULL,
            target0_gen_fn
        );
    }
    else {
        target0_gen_fn(cb, side_exit, NULL, SHAPE_DEFAULT);
    }
}

bool rb_iv_index_tbl_lookup(struct st_table *iv_index_tbl, ID id, struct rb_iv_index_tbl_entry **ent); // vm_insnhelper.c

enum {
    GETIVAR_MAX_DEPTH = 10,       // up to 5 different classes, and embedded or not for each
    OPT_AREF_MAX_CHAIN_DEPTH = 2, // hashes and arrays
    OSWB_MAX_DEPTH = 5,           // up to 5 different classes
};

// Codegen for setting an instance variable.
// Preconditions:
//   - receiver is in REG0
//   - receiver has the same class as CLASS_OF(comptime_receiver)
//   - no stack push or pops to ctx since the entry to the codegen of the instruction being compiled
static codegen_status_t
gen_set_ivar(jitstate_t *jit, ctx_t *ctx, const int max_chain_depth, VALUE comptime_receiver, ID ivar_name, insn_opnd_t reg0_opnd, uint8_t *side_exit)
{
    VALUE comptime_val_klass = CLASS_OF(comptime_receiver);
    const ctx_t starting_context = *ctx; // make a copy for use with jit_chain_guard

    // If the class uses the default allocator, instances should all be T_OBJECT
    // NOTE: This assumes nobody changes the allocator of the class after allocation.
    //       Eventually, we can encode whether an object is T_OBJECT or not
    //       inside object shapes.
    if (rb_get_alloc_func(comptime_val_klass) != rb_class_allocate_instance) {
        GEN_COUNTER_INC(cb, setivar_not_object);
        return YJIT_CANT_COMPILE;
    }
    RUBY_ASSERT(BUILTIN_TYPE(comptime_receiver) == T_OBJECT); // because we checked the allocator

    // ID for the name of the ivar
    ID id = ivar_name;
    struct rb_iv_index_tbl_entry *ent;
    struct st_table *iv_index_tbl = ROBJECT_IV_INDEX_TBL(comptime_receiver);

    // Lookup index for the ivar the instruction loads
    if (iv_index_tbl && rb_iv_index_tbl_lookup(iv_index_tbl, id, &ent)) {
        uint32_t ivar_index = ent->index;

        val_type_t val_type = ctx_get_opnd_type(ctx, OPND_STACK(0));
        x86opnd_t val_to_write = ctx_stack_opnd(ctx, 0);
        mov(cb, REG1, val_to_write);

        // Bail if the value to write is a heap object, because this needs a write barrier
        if (!val_type.is_imm) {
            ADD_COMMENT(cb, "guard value is immediate");
            test(cb, REG1, imm_opnd(RUBY_IMMEDIATE_MASK));
            jz_ptr(cb, COUNTED_EXIT(side_exit, setivar_val_heapobject));
            ctx_set_opnd_type(ctx, OPND_STACK(0), TYPE_IMM);
        }

        // Pop the value to write
        ctx_stack_pop(ctx, 1);

        // Bail if this object is frozen
        ADD_COMMENT(cb, "guard self is not frozen");
        x86opnd_t flags_opnd = member_opnd(REG0, struct RBasic, flags);
        test(cb, flags_opnd, imm_opnd(RUBY_FL_FREEZE));
        jnz_ptr(cb, COUNTED_EXIT(side_exit, setivar_frozen));

        // Pop receiver if it's on the temp stack
        if (!reg0_opnd.is_self) {
            (void)ctx_stack_pop(ctx, 1);
        }

        // Compile time self is embedded and the ivar index lands within the object
        if (RB_FL_TEST_RAW(comptime_receiver, ROBJECT_EMBED) && ivar_index < ROBJECT_EMBED_LEN_MAX) {
            // See ROBJECT_IVPTR() from include/ruby/internal/core/robject.h

            // Guard that self is embedded
            // TODO: BT and JC is shorter
            ADD_COMMENT(cb, "guard embedded setivar");
            test(cb, flags_opnd, imm_opnd(ROBJECT_EMBED));
            jit_chain_guard(JCC_JZ, jit, &starting_context, max_chain_depth, side_exit);

            // Store the ivar on the object
            x86opnd_t ivar_opnd = mem_opnd(64, REG0, offsetof(struct RObject, as.ary) + ivar_index * SIZEOF_VALUE);
            mov(cb, ivar_opnd, REG1);

            // Push the ivar on the stack
            // For attr_writer we'll need to push the value on the stack
            //x86opnd_t out_opnd = ctx_stack_push(ctx, TYPE_UNKNOWN);
        }
        else {
            // Compile time value is *not* embeded.

            // Guard that value is *not* embedded
            // See ROBJECT_IVPTR() from include/ruby/internal/core/robject.h
            ADD_COMMENT(cb, "guard extended setivar");
            x86opnd_t flags_opnd = member_opnd(REG0, struct RBasic, flags);
            test(cb, flags_opnd, imm_opnd(ROBJECT_EMBED));
            jit_chain_guard(JCC_JNZ, jit, &starting_context, max_chain_depth, side_exit);

            // check that the extended table is big enough
            if (ivar_index >= ROBJECT_EMBED_LEN_MAX + 1) {
                // Check that the slot is inside the extended table (num_slots > index)
                ADD_COMMENT(cb, "check index in extended table");
                x86opnd_t num_slots = mem_opnd(32, REG0, offsetof(struct RObject, as.heap.numiv));
                cmp(cb, num_slots, imm_opnd(ivar_index));
                jle_ptr(cb, COUNTED_EXIT(side_exit, setivar_idx_out_of_range));
            }

            // Get a pointer to the extended table
            x86opnd_t tbl_opnd = mem_opnd(64, REG0, offsetof(struct RObject, as.heap.ivptr));
            mov(cb, REG0, tbl_opnd);

            // Write the ivar to the extended table
            x86opnd_t ivar_opnd = mem_opnd(64, REG0, sizeof(VALUE) * ivar_index);
            mov(cb, ivar_opnd, REG1);
        }

        // Jump to next instruction. This allows guard chains to share the same successor.
        jit_jump_to_next_insn(jit, ctx);
        return YJIT_END_BLOCK;
    }

    GEN_COUNTER_INC(cb, setivar_name_not_mapped);
    return YJIT_CANT_COMPILE;
}

// Codegen for getting an instance variable.
// Preconditions:
//   - receiver is in REG0
//   - receiver has the same class as CLASS_OF(comptime_receiver)
//   - no stack push or pops to ctx since the entry to the codegen of the instruction being compiled
static codegen_status_t
gen_get_ivar(jitstate_t *jit, ctx_t *ctx, const int max_chain_depth, VALUE comptime_receiver, ID ivar_name, insn_opnd_t reg0_opnd, uint8_t *side_exit)
{
    VALUE comptime_val_klass = CLASS_OF(comptime_receiver);
    const ctx_t starting_context = *ctx; // make a copy for use with jit_chain_guard

    // If the class uses the default allocator, instances should all be T_OBJECT
    // NOTE: This assumes nobody changes the allocator of the class after allocation.
    //       Eventually, we can encode whether an object is T_OBJECT or not
    //       inside object shapes.
    if (rb_get_alloc_func(comptime_val_klass) != rb_class_allocate_instance) {
        GEN_COUNTER_INC(cb, getivar_not_object);
        return YJIT_CANT_COMPILE;
    }
    RUBY_ASSERT(BUILTIN_TYPE(comptime_receiver) == T_OBJECT); // because we checked the allocator

    // ID for the name of the ivar
    ID id = ivar_name;
    struct rb_iv_index_tbl_entry *ent;
    struct st_table *iv_index_tbl = ROBJECT_IV_INDEX_TBL(comptime_receiver);

    // Lookup index for the ivar the instruction loads
    if (iv_index_tbl && rb_iv_index_tbl_lookup(iv_index_tbl, id, &ent)) {
        uint32_t ivar_index = ent->index;

        // Pop receiver if it's on the temp stack
        if (!reg0_opnd.is_self) {
            (void)ctx_stack_pop(ctx, 1);
        }

        // Compile time self is embedded and the ivar index lands within the object
        if (RB_FL_TEST_RAW(comptime_receiver, ROBJECT_EMBED) && ivar_index < ROBJECT_EMBED_LEN_MAX) {
            // See ROBJECT_IVPTR() from include/ruby/internal/core/robject.h

            // Guard that self is embedded
            // TODO: BT and JC is shorter
            ADD_COMMENT(cb, "guard embedded getivar");
            x86opnd_t flags_opnd = member_opnd(REG0, struct RBasic, flags);
            test(cb, flags_opnd, imm_opnd(ROBJECT_EMBED));
            jit_chain_guard(JCC_JZ, jit, &starting_context, max_chain_depth, side_exit);

            // Load the variable
            x86opnd_t ivar_opnd = mem_opnd(64, REG0, offsetof(struct RObject, as.ary) + ivar_index * SIZEOF_VALUE);
            mov(cb, REG1, ivar_opnd);

            // Guard that the variable is not Qundef
            // TODO: use cmov to push Qnil in this case
            cmp(cb, REG1, imm_opnd(Qundef));
            je_ptr(cb, COUNTED_EXIT(side_exit, getivar_undef));

            // Push the ivar on the stack
            x86opnd_t out_opnd = ctx_stack_push(ctx, TYPE_UNKNOWN);
            mov(cb, out_opnd, REG1);
        }
        else {
            // Compile time value is *not* embeded.

            // Guard that value is *not* embedded
            // See ROBJECT_IVPTR() from include/ruby/internal/core/robject.h
            ADD_COMMENT(cb, "guard extended getivar");
            x86opnd_t flags_opnd = member_opnd(REG0, struct RBasic, flags);
            test(cb, flags_opnd, imm_opnd(ROBJECT_EMBED));
            jit_chain_guard(JCC_JNZ, jit, &starting_context, max_chain_depth, side_exit);

            // check that the extended table is big enough
            if (ivar_index >= ROBJECT_EMBED_LEN_MAX + 1) {
                // Check that the slot is inside the extended table (num_slots > index)
                x86opnd_t num_slots = mem_opnd(32, REG0, offsetof(struct RObject, as.heap.numiv));
                cmp(cb, num_slots, imm_opnd(ivar_index));
                jle_ptr(cb, COUNTED_EXIT(side_exit, getivar_idx_out_of_range));
            }

            // Get a pointer to the extended table
            x86opnd_t tbl_opnd = mem_opnd(64, REG0, offsetof(struct RObject, as.heap.ivptr));
            mov(cb, REG0, tbl_opnd);

            // Read the ivar from the extended table
            x86opnd_t ivar_opnd = mem_opnd(64, REG0, sizeof(VALUE) * ivar_index);
            mov(cb, REG0, ivar_opnd);

            // Check that the ivar is not Qundef
            cmp(cb, REG0, imm_opnd(Qundef));
            je_ptr(cb, COUNTED_EXIT(side_exit, getivar_undef));

            // Push the ivar on the stack
            x86opnd_t out_opnd = ctx_stack_push(ctx, TYPE_UNKNOWN);
            mov(cb, out_opnd, REG0);
        }

        // Jump to next instruction. This allows guard chains to share the same successor.
        jit_jump_to_next_insn(jit, ctx);
        return YJIT_END_BLOCK;
    }

    GEN_COUNTER_INC(cb, getivar_name_not_mapped);
    return YJIT_CANT_COMPILE;
}

static codegen_status_t
gen_getinstancevariable(jitstate_t *jit, ctx_t *ctx)
{
    // Defer compilation so we can specialize on a runtime `self`
    if (!jit_at_current_insn(jit)) {
        defer_compilation(jit->block, jit->insn_idx, ctx);
        return YJIT_END_BLOCK;
    }

    ID ivar_name = (ID)jit_get_arg(jit, 0);

    VALUE comptime_val = jit_peek_at_self(jit, ctx);
    VALUE comptime_val_klass = CLASS_OF(comptime_val);

    // Generate a side exit
    uint8_t *side_exit = yjit_side_exit(jit, ctx);

    // Guard that the receiver has the same class as the one from compile time.
    mov(cb, REG0, member_opnd(REG_CFP, rb_control_frame_t, self));
    guard_self_is_heap(cb, REG0, COUNTED_EXIT(side_exit, getivar_se_self_not_heap), ctx);

    jit_guard_known_klass(jit, ctx, comptime_val_klass, OPND_SELF, GETIVAR_MAX_DEPTH, side_exit);

    return gen_get_ivar(jit, ctx, GETIVAR_MAX_DEPTH, comptime_val, ivar_name, OPND_SELF, side_exit);
}

static codegen_status_t
gen_setinstancevariable(jitstate_t* jit, ctx_t* ctx)
{
    // Defer compilation so we can specialize on a runtime `self`
    if (!jit_at_current_insn(jit)) {
        defer_compilation(jit->block, jit->insn_idx, ctx);
        return YJIT_END_BLOCK;
    }

    ID ivar_name = (ID)jit_get_arg(jit, 0);

    VALUE comptime_val = jit_peek_at_self(jit, ctx);
    VALUE comptime_val_klass = CLASS_OF(comptime_val);

    // Generate a side exit
    uint8_t *side_exit = yjit_side_exit(jit, ctx);

    // Guard that the receiver has the same class as the one from compile time.
    mov(cb, REG0, member_opnd(REG_CFP, rb_control_frame_t, self));
    guard_self_is_heap(cb, REG0, COUNTED_EXIT(side_exit, setivar_se_self_not_heap), ctx);

    jit_guard_known_klass(jit, ctx, comptime_val_klass, OPND_SELF, GETIVAR_MAX_DEPTH, side_exit);

    return gen_set_ivar(jit, ctx, GETIVAR_MAX_DEPTH, comptime_val, ivar_name, OPND_SELF, side_exit);
}

static void
guard_two_fixnums(ctx_t* ctx, uint8_t* side_exit)
{
    // Get the stack operand types
    val_type_t arg1_type = ctx_get_opnd_type(ctx, OPND_STACK(0));
    val_type_t arg0_type = ctx_get_opnd_type(ctx, OPND_STACK(1));

    // Get stack operands without popping them
    x86opnd_t arg1 = ctx_stack_opnd(ctx, 0);
    x86opnd_t arg0 = ctx_stack_opnd(ctx, 1);

    // If not fixnums, fall back
    if (arg0_type.type != ETYPE_FIXNUM) {
        ADD_COMMENT(cb, "guard arg0 fixnum");
        test(cb, arg0, imm_opnd(RUBY_FIXNUM_FLAG));
        jz_ptr(cb, side_exit);
    }
    if (arg1_type.type != ETYPE_FIXNUM) {
        ADD_COMMENT(cb, "guard arg1 fixnum");
        test(cb, arg1, imm_opnd(RUBY_FIXNUM_FLAG));
        jz_ptr(cb, side_exit);
    }

    // Set stack types in context
    ctx_set_opnd_type(ctx, OPND_STACK(0), TYPE_FIXNUM);
    ctx_set_opnd_type(ctx, OPND_STACK(1), TYPE_FIXNUM);
}

// Conditional move operation used by comparison operators
typedef void (*cmov_fn)(codeblock_t* cb, x86opnd_t opnd0, x86opnd_t opnd1);

static codegen_status_t
gen_fixnum_cmp(jitstate_t* jit, ctx_t* ctx, cmov_fn cmov_op)
{
    // Create a size-exit to fall back to the interpreter
    // Note: we generate the side-exit before popping operands from the stack
    uint8_t* side_exit = yjit_side_exit(jit, ctx);

    if (!assume_bop_not_redefined(jit->block, INTEGER_REDEFINED_OP_FLAG, BOP_LT)) {
        return YJIT_CANT_COMPILE;
    }

    // Check that both operands are fixnums
    guard_two_fixnums(ctx, side_exit);

    // Get the operands and destination from the stack
    x86opnd_t arg1 = ctx_stack_pop(ctx, 1);
    x86opnd_t arg0 = ctx_stack_pop(ctx, 1);

    // Compare the arguments
    xor(cb, REG0_32, REG0_32); // REG0 = Qfalse
    mov(cb, REG1, arg0);
    cmp(cb, REG1, arg1);
    mov(cb, REG1, imm_opnd(Qtrue));
    cmov_op(cb, REG0, REG1);

    // Push the output on the stack
    x86opnd_t dst = ctx_stack_push(ctx, TYPE_UNKNOWN);
    mov(cb, dst, REG0);

    return YJIT_KEEP_COMPILING;
}

static codegen_status_t
gen_opt_lt(jitstate_t* jit, ctx_t* ctx)
{
    return gen_fixnum_cmp(jit, ctx, cmovl);
}

static codegen_status_t
gen_opt_le(jitstate_t* jit, ctx_t* ctx)
{
    return gen_fixnum_cmp(jit, ctx, cmovle);
}

static codegen_status_t
gen_opt_ge(jitstate_t* jit, ctx_t* ctx)
{
    return gen_fixnum_cmp(jit, ctx, cmovge);
}

static codegen_status_t
gen_opt_gt(jitstate_t* jit, ctx_t* ctx)
{
    return gen_fixnum_cmp(jit, ctx, cmovg);
}

static codegen_status_t gen_opt_send_without_block(jitstate_t *jit, ctx_t *ctx);

static codegen_status_t
gen_opt_aref(jitstate_t *jit, ctx_t *ctx)
{
    struct rb_call_data * cd = (struct rb_call_data *)jit_get_arg(jit, 0);
    int32_t argc = (int32_t)vm_ci_argc(cd->ci);

    // Only JIT one arg calls like `ary[6]`
    if (argc != 1) {
        GEN_COUNTER_INC(cb, oaref_argc_not_one);
        return YJIT_CANT_COMPILE;
    }

    // Defer compilation so we can specialize base on a runtime receiver
    if (!jit_at_current_insn(jit)) {
        defer_compilation(jit->block, jit->insn_idx, ctx);
        return YJIT_END_BLOCK;
    }

    // Remember the context on entry for adding guard chains
    const ctx_t starting_context = *ctx;

    // Specialize base on compile time values
    VALUE comptime_idx = jit_peek_at_stack(jit, ctx, 0);
    VALUE comptime_recv = jit_peek_at_stack(jit, ctx, 1);

    // Create a size-exit to fall back to the interpreter
    uint8_t *side_exit = yjit_side_exit(jit, ctx);

    if (CLASS_OF(comptime_recv) == rb_cArray && RB_FIXNUM_P(comptime_idx)) {
        if (!assume_bop_not_redefined(jit->block, ARRAY_REDEFINED_OP_FLAG, BOP_AREF)) {
            return YJIT_CANT_COMPILE;
        }

        // Pop the stack operands
        x86opnd_t idx_opnd = ctx_stack_pop(ctx, 1);
        x86opnd_t recv_opnd = ctx_stack_pop(ctx, 1);
        mov(cb, REG0, recv_opnd);

        // if (SPECIAL_CONST_P(recv)) {
        // Bail if receiver is not a heap object
        test(cb, REG0, imm_opnd(RUBY_IMMEDIATE_MASK));
        jnz_ptr(cb, side_exit);
        cmp(cb, REG0, imm_opnd(Qfalse));
        je_ptr(cb, side_exit);
        cmp(cb, REG0, imm_opnd(Qnil));
        je_ptr(cb, side_exit);

        // Bail if recv has a class other than ::Array.
        // BOP_AREF check above is only good for ::Array.
        mov(cb, REG1, mem_opnd(64, REG0, offsetof(struct RBasic, klass)));
        mov(cb, REG0, const_ptr_opnd((void *)rb_cArray));
        cmp(cb, REG0, REG1);
        jit_chain_guard(JCC_JNE, jit, &starting_context, OPT_AREF_MAX_CHAIN_DEPTH, side_exit);

        // Bail if idx is not a FIXNUM
        mov(cb, REG1, idx_opnd);
        test(cb, REG1, imm_opnd(RUBY_FIXNUM_FLAG));
        jz_ptr(cb, COUNTED_EXIT(side_exit, oaref_arg_not_fixnum));

        // Call VALUE rb_ary_entry_internal(VALUE ary, long offset).
        // It never raises or allocates, so we don't need to write to cfp->pc.
        {
            yjit_save_regs(cb);

            mov(cb, RDI, recv_opnd);
            sar(cb, REG1, imm_opnd(1)); // Convert fixnum to int
            mov(cb, RSI, REG1);
            call_ptr(cb, REG0, (void *)rb_ary_entry_internal);

            yjit_load_regs(cb);

            // Push the return value onto the stack
            x86opnd_t stack_ret = ctx_stack_push(ctx, TYPE_UNKNOWN);
            mov(cb, stack_ret, RAX);
        }

        // Jump to next instruction. This allows guard chains to share the same successor.
        jit_jump_to_next_insn(jit, ctx);
        return YJIT_END_BLOCK;
    }
    else if (CLASS_OF(comptime_recv) == rb_cHash) {
        if (!assume_bop_not_redefined(jit->block, HASH_REDEFINED_OP_FLAG, BOP_AREF)) {
            return YJIT_CANT_COMPILE;
        }

        // Pop the stack operands
        x86opnd_t idx_opnd = ctx_stack_pop(ctx, 1);
        x86opnd_t recv_opnd = ctx_stack_pop(ctx, 1);
        mov(cb, REG0, recv_opnd);

        // if (SPECIAL_CONST_P(recv)) {
        // Bail if receiver is not a heap object
        test(cb, REG0, imm_opnd(RUBY_IMMEDIATE_MASK));
        jnz_ptr(cb, side_exit);
        cmp(cb, REG0, imm_opnd(Qfalse));
        je_ptr(cb, side_exit);
        cmp(cb, REG0, imm_opnd(Qnil));
        je_ptr(cb, side_exit);

        // Bail if recv has a class other than ::Hash.
        // BOP_AREF check above is only good for ::Hash.
        mov(cb, REG1, mem_opnd(64, REG0, offsetof(struct RBasic, klass)));
        mov(cb, REG0, const_ptr_opnd((void *)rb_cHash));
        cmp(cb, REG0, REG1);
        jit_chain_guard(JCC_JNE, jit, &starting_context, OPT_AREF_MAX_CHAIN_DEPTH, side_exit);

        // Call VALUE rb_hash_aref(VALUE hash, VALUE key).
        {
            // Write incremented pc to cfp->pc as the routine can raise and allocate
            mov(cb, REG0, const_ptr_opnd(jit->pc + insn_len(BIN(opt_aref))));
            mov(cb, member_opnd(REG_CFP, rb_control_frame_t, pc), REG0);

            // About to change REG_SP which these operands depend on. Yikes.
            mov(cb, R8, recv_opnd);
            mov(cb, R9, idx_opnd);

            // Write sp to cfp->sp since rb_hash_aref might need to call #hash on the key
            if (ctx->sp_offset != 0) {
                x86opnd_t stack_pointer = ctx_sp_opnd(ctx, 0);
                lea(cb, REG_SP, stack_pointer);
                mov(cb, member_opnd(REG_CFP, rb_control_frame_t, sp), REG_SP);
                ctx->sp_offset = 0; // REG_SP now equals cfp->sp
            }

            yjit_save_regs(cb);

            mov(cb, C_ARG_REGS[0], R8);
            mov(cb, C_ARG_REGS[1], R9);
            call_ptr(cb, REG0, (void *)rb_hash_aref);

            yjit_load_regs(cb);

            // Push the return value onto the stack
            x86opnd_t stack_ret = ctx_stack_push(ctx, TYPE_UNKNOWN);
            mov(cb, stack_ret, RAX);
        }

        // Jump to next instruction. This allows guard chains to share the same successor.
        jit_jump_to_next_insn(jit, ctx);
        return YJIT_END_BLOCK;
    }
    else {
        // General case. Call the [] method.
        return gen_opt_send_without_block(jit, ctx);
    }
}

static codegen_status_t
gen_opt_and(jitstate_t* jit, ctx_t* ctx)
{
    // Create a size-exit to fall back to the interpreter
    // Note: we generate the side-exit before popping operands from the stack
    uint8_t* side_exit = yjit_side_exit(jit, ctx);

    if (!assume_bop_not_redefined(jit->block, INTEGER_REDEFINED_OP_FLAG, BOP_AND)) {
        return YJIT_CANT_COMPILE;
    }

    // Check that both operands are fixnums
    guard_two_fixnums(ctx, side_exit);

    // Get the operands and destination from the stack
    x86opnd_t arg1 = ctx_stack_pop(ctx, 1);
    x86opnd_t arg0 = ctx_stack_pop(ctx, 1);

    // Do the bitwise and arg0 & arg1
    mov(cb, REG0, arg0);
    and(cb, REG0, arg1);

    // Push the output on the stack
    x86opnd_t dst = ctx_stack_push(ctx, TYPE_FIXNUM);
    mov(cb, dst, REG0);

    return YJIT_KEEP_COMPILING;
}

static codegen_status_t
gen_opt_or(jitstate_t* jit, ctx_t* ctx)
{
    // Create a size-exit to fall back to the interpreter
    // Note: we generate the side-exit before popping operands from the stack
    uint8_t* side_exit = yjit_side_exit(jit, ctx);

    if (!assume_bop_not_redefined(jit->block, INTEGER_REDEFINED_OP_FLAG, BOP_OR)) {
        return YJIT_CANT_COMPILE;
    }

    // Check that both operands are fixnums
    guard_two_fixnums(ctx, side_exit);

    // Get the operands and destination from the stack
    x86opnd_t arg1 = ctx_stack_pop(ctx, 1);
    x86opnd_t arg0 = ctx_stack_pop(ctx, 1);

    // Do the bitwise or arg0 | arg1
    mov(cb, REG0, arg0);
    or(cb, REG0, arg1);

    // Push the output on the stack
    x86opnd_t dst = ctx_stack_push(ctx, TYPE_FIXNUM);
    mov(cb, dst, REG0);

    return YJIT_KEEP_COMPILING;
}

static codegen_status_t
gen_opt_minus(jitstate_t* jit, ctx_t* ctx)
{
    // Create a size-exit to fall back to the interpreter
    // Note: we generate the side-exit before popping operands from the stack
    uint8_t* side_exit = yjit_side_exit(jit, ctx);

    if (!assume_bop_not_redefined(jit->block, INTEGER_REDEFINED_OP_FLAG, BOP_MINUS)) {
        return YJIT_CANT_COMPILE;
    }

    // Check that both operands are fixnums
    guard_two_fixnums(ctx, side_exit);

    // Get the operands and destination from the stack
    x86opnd_t arg1 = ctx_stack_pop(ctx, 1);
    x86opnd_t arg0 = ctx_stack_pop(ctx, 1);

    // Subtract arg0 - arg1 and test for overflow
    mov(cb, REG0, arg0);
    sub(cb, REG0, arg1);
    jo_ptr(cb, side_exit);
    add(cb, REG0, imm_opnd(1));

    // Push the output on the stack
    x86opnd_t dst = ctx_stack_push(ctx, TYPE_FIXNUM);
    mov(cb, dst, REG0);

    return YJIT_KEEP_COMPILING;
}

static codegen_status_t
gen_opt_plus(jitstate_t* jit, ctx_t* ctx)
{
    // Create a size-exit to fall back to the interpreter
    // Note: we generate the side-exit before popping operands from the stack
    uint8_t* side_exit = yjit_side_exit(jit, ctx);

    if (!assume_bop_not_redefined(jit->block, INTEGER_REDEFINED_OP_FLAG, BOP_PLUS)) {
        return YJIT_CANT_COMPILE;
    }

    // Check that both operands are fixnums
    guard_two_fixnums(ctx, side_exit);

    // Get the operands and destination from the stack
    x86opnd_t arg1 = ctx_stack_pop(ctx, 1);
    x86opnd_t arg0 = ctx_stack_pop(ctx, 1);

    // Add arg0 + arg1 and test for overflow
    mov(cb, REG0, arg0);
    sub(cb, REG0, imm_opnd(1));
    add(cb, REG0, arg1);
    jo_ptr(cb, side_exit);

    // Push the output on the stack
    x86opnd_t dst = ctx_stack_push(ctx, TYPE_FIXNUM);
    mov(cb, dst, REG0);

    return YJIT_KEEP_COMPILING;
}

void
gen_branchif_branch(codeblock_t* cb, uint8_t* target0, uint8_t* target1, uint8_t shape)
{
    switch (shape)
    {
        case SHAPE_NEXT0:
        jz_ptr(cb, target1);
        break;

        case SHAPE_NEXT1:
        jnz_ptr(cb, target0);
        break;

        case SHAPE_DEFAULT:
        jnz_ptr(cb, target0);
        jmp_ptr(cb, target1);
        break;
    }
}

static codegen_status_t
gen_branchif(jitstate_t* jit, ctx_t* ctx)
{
    // FIXME: eventually, put VM_CHECK_INTS() only on backward branch targets
    // Check for interrupts
    uint8_t* side_exit = yjit_side_exit(jit, ctx);
    yjit_check_ints(cb, side_exit);

    // Test if any bit (outside of the Qnil bit) is on
    // RUBY_Qfalse  /* ...0000 0000 */
    // RUBY_Qnil    /* ...0000 1000 */
    x86opnd_t val_opnd = ctx_stack_pop(ctx, 1);
    test(cb, val_opnd, imm_opnd(~Qnil));

    // Get the branch target instruction offsets
    uint32_t next_idx = jit_next_insn_idx(jit);
    uint32_t jump_idx = next_idx + (uint32_t)jit_get_arg(jit, 0);
    blockid_t next_block = { jit->iseq, next_idx };
    blockid_t jump_block = { jit->iseq, jump_idx };

    // Generate the branch instructions
    gen_branch(
        jit->block,
        ctx,
        jump_block,
        ctx,
        next_block,
        ctx,
        gen_branchif_branch
    );

    return YJIT_END_BLOCK;
}

void
gen_branchunless_branch(codeblock_t* cb, uint8_t* target0, uint8_t* target1, uint8_t shape)
{
    switch (shape)
    {
        case SHAPE_NEXT0:
        jnz_ptr(cb, target1);
        break;

        case SHAPE_NEXT1:
        jz_ptr(cb, target0);
        break;

        case SHAPE_DEFAULT:
        jz_ptr(cb, target0);
        jmp_ptr(cb, target1);
        break;
    }
}

static codegen_status_t
gen_branchunless(jitstate_t* jit, ctx_t* ctx)
{
    // FIXME: eventually, put VM_CHECK_INTS() only on backward branch targets
    // Check for interrupts
    uint8_t* side_exit = yjit_side_exit(jit, ctx);
    yjit_check_ints(cb, side_exit);

    // Test if any bit (outside of the Qnil bit) is on
    // RUBY_Qfalse  /* ...0000 0000 */
    // RUBY_Qnil    /* ...0000 1000 */
    x86opnd_t val_opnd = ctx_stack_pop(ctx, 1);
    test(cb, val_opnd, imm_opnd(~Qnil));

    // Get the branch target instruction offsets
    uint32_t next_idx = jit_next_insn_idx(jit);
    uint32_t jump_idx = next_idx + (uint32_t)jit_get_arg(jit, 0);
    blockid_t next_block = { jit->iseq, next_idx };
    blockid_t jump_block = { jit->iseq, jump_idx };

    // Generate the branch instructions
    gen_branch(
        jit->block,
        ctx,
        jump_block,
        ctx,
        next_block,
        ctx,
        gen_branchunless_branch
    );

    return YJIT_END_BLOCK;
}

static codegen_status_t
gen_jump(jitstate_t* jit, ctx_t* ctx)
{
    // FIXME: eventually, put VM_CHECK_INTS() only on backward branch targets
    // Check for interrupts
    uint8_t* side_exit = yjit_side_exit(jit, ctx);
    yjit_check_ints(cb, side_exit);

    // Get the branch target instruction offsets
    uint32_t jump_idx = jit_next_insn_idx(jit) + (int32_t)jit_get_arg(jit, 0);
    blockid_t jump_block = { jit->iseq, jump_idx };

    // Generate the jump instruction
    gen_direct_jump(
        jit->block,
        ctx,
        jump_block
    );

    return YJIT_END_BLOCK;
}

/*
Guard that a stack operand has the same class as known_klass.
Recompile as contingency if possible, or take side exit a last resort.
*/
static bool
jit_guard_known_klass(jitstate_t *jit, ctx_t* ctx, VALUE known_klass, insn_opnd_t insn_opnd, const int max_chain_depth, uint8_t *side_exit)
{
    // Can't guard for for these classes because some of they are sometimes immediate (special const).
    // Can remove this by adding appropriate dynamic checks.
    if (known_klass == rb_cInteger ||
        known_klass == rb_cSymbol ||
        known_klass == rb_cFloat ||
        known_klass == rb_cNilClass ||
        known_klass == rb_cTrueClass ||
        known_klass == rb_cFalseClass) {
        return false;
    }

    val_type_t val_type = ctx_get_opnd_type(ctx, insn_opnd);

    // Check that the receiver is a heap object
    if (!val_type.is_heap)
    {
        // FIXME: use two comparisons instead of 3 here
        ADD_COMMENT(cb, "guard not immediate");
        RUBY_ASSERT(Qfalse < Qnil);
        test(cb, REG0, imm_opnd(RUBY_IMMEDIATE_MASK));
        jnz_ptr(cb, side_exit);
        cmp(cb, REG0, imm_opnd(Qnil));
        jbe_ptr(cb, side_exit);

        ctx_set_opnd_type(ctx, insn_opnd, TYPE_HEAP);
    }

    // Pointer to the klass field of the receiver &(recv->klass)
    x86opnd_t klass_opnd = mem_opnd(64, REG0, offsetof(struct RBasic, klass));

    // Bail if receiver class is different from known_klass
    // TODO: jit_mov_gc_ptr keeps a strong reference, which leaks the class.
    ADD_COMMENT(cb, "guard known class");
    jit_mov_gc_ptr(jit, cb, REG1, known_klass);
    cmp(cb, klass_opnd, REG1);
    jit_chain_guard(JCC_JNE, jit, ctx, max_chain_depth, side_exit);
    return true;
}

// Generate ancestry guard for protected callee.
// Calls to protected callees only go through when self.is_a?(klass_that_defines_the_callee).
static void
jit_protected_callee_ancestry_guard(jitstate_t *jit, codeblock_t *cb, const rb_callable_method_entry_t *cme, uint8_t *side_exit)
{
    // See vm_call_method().
    yjit_save_regs(cb);
    mov(cb, C_ARG_REGS[0], member_opnd(REG_CFP, rb_control_frame_t, self));
    jit_mov_gc_ptr(jit, cb, C_ARG_REGS[1], cme->defined_class);
    // Note: PC isn't written to current control frame as rb_is_kind_of() shouldn't raise.
    // VALUE rb_obj_is_kind_of(VALUE obj, VALUE klass);
    call_ptr(cb, REG0, (void *)&rb_obj_is_kind_of);
    yjit_load_regs(cb);
    test(cb, RAX, RAX);
    jz_ptr(cb, COUNTED_EXIT(side_exit, oswb_se_protected_check_failed));
}

static codegen_status_t
gen_oswb_cfunc(jitstate_t *jit, ctx_t *ctx, const struct rb_callinfo *ci, const rb_callable_method_entry_t *cme, int32_t argc)
{
    const rb_method_cfunc_t *cfunc = UNALIGNED_MEMBER_PTR(cme->def, body.cfunc);

    // If the function expects a Ruby array of arguments
    if (cfunc->argc < 0 && cfunc->argc != -1) {
        GEN_COUNTER_INC(cb, oswb_cfunc_ruby_array_varg);
        return YJIT_CANT_COMPILE;
    }

    // If the argument count doesn't match
    if (cfunc->argc >= 0 && cfunc->argc != argc) {
        GEN_COUNTER_INC(cb, oswb_cfunc_argc_mismatch);
        return YJIT_CANT_COMPILE;
    }

    // Don't JIT functions that need C stack arguments for now
    if (argc + 1 > NUM_C_ARG_REGS) {
        GEN_COUNTER_INC(cb, oswb_cfunc_toomany_args);
        return YJIT_CANT_COMPILE;
    }

    // Callee method ID
    //ID mid = vm_ci_mid(ci);
    //printf("JITting call to C function \"%s\", argc: %lu\n", rb_id2name(mid), argc);
    //print_str(cb, "");
    //print_str(cb, "calling CFUNC:");
    //print_str(cb, rb_id2name(mid));
    //print_str(cb, "recv");
    //print_ptr(cb, recv);

    // Create a size-exit to fall back to the interpreter
    uint8_t *side_exit = yjit_side_exit(jit, ctx);

    // Check for interrupts
    yjit_check_ints(cb, side_exit);

    // Points to the receiver operand on the stack
    x86opnd_t recv = ctx_stack_opnd(ctx, argc);

    // Store incremented PC into current control frame in case callee raises.
    mov(cb, REG0, const_ptr_opnd(jit->pc + insn_len(BIN(opt_send_without_block))));
    mov(cb, mem_opnd(64, REG_CFP, offsetof(rb_control_frame_t, pc)), REG0);

    // If this function needs a Ruby stack frame
    if (cfunc_needs_frame(cfunc)) {
        // Stack overflow check
        // #define CHECK_VM_STACK_OVERFLOW0(cfp, sp, margin)
        // REG_CFP <= REG_SP + 4 * sizeof(VALUE) + sizeof(rb_control_frame_t)
        lea(cb, REG0, ctx_sp_opnd(ctx, sizeof(VALUE) * 4 + sizeof(rb_control_frame_t)));
        cmp(cb, REG_CFP, REG0);
        jle_ptr(cb, COUNTED_EXIT(side_exit, oswb_se_cf_overflow));

        // Increment the stack pointer by 3 (in the callee)
        // sp += 3
        lea(cb, REG0, ctx_sp_opnd(ctx, sizeof(VALUE) * 3));

        // Put compile time cme into REG1. It's assumed to be valid because we are notified when
        // any cme we depend on become outdated. See rb_yjit_method_lookup_change().
        jit_mov_gc_ptr(jit, cb, REG1, (VALUE)cme);
        // Write method entry at sp[-3]
        // sp[-3] = me;
        mov(cb, mem_opnd(64, REG0, 8 * -3), REG1);

        // Write block handler at sp[-2]
        // sp[-2] = block_handler;
        mov(cb, mem_opnd(64, REG0, 8 * -2), imm_opnd(VM_BLOCK_HANDLER_NONE));

        // Write env flags at sp[-1]
        // sp[-1] = frame_type;
        uint64_t frame_type = VM_FRAME_MAGIC_CFUNC | VM_FRAME_FLAG_CFRAME | VM_ENV_FLAG_LOCAL;
        mov(cb, mem_opnd(64, REG0, 8 * -1), imm_opnd(frame_type));

        // Allocate a new CFP (ec->cfp--)
        sub(
            cb,
            member_opnd(REG_EC, rb_execution_context_t, cfp),
            imm_opnd(sizeof(rb_control_frame_t))
        );

        // Setup the new frame
        // *cfp = (const struct rb_control_frame_struct) {
        //    .pc         = 0,
        //    .sp         = sp,
        //    .iseq       = 0,
        //    .self       = recv,
        //    .ep         = sp - 1,
        //    .block_code = 0,
        //    .__bp__     = sp,
        // };
        mov(cb, REG1, member_opnd(REG_EC, rb_execution_context_t, cfp));
        mov(cb, member_opnd(REG1, rb_control_frame_t, pc), imm_opnd(0));
        mov(cb, member_opnd(REG1, rb_control_frame_t, sp), REG0);
        mov(cb, member_opnd(REG1, rb_control_frame_t, iseq), imm_opnd(0));
        mov(cb, member_opnd(REG1, rb_control_frame_t, block_code), imm_opnd(0));
        mov(cb, member_opnd(REG1, rb_control_frame_t, __bp__), REG0);
        sub(cb, REG0, imm_opnd(sizeof(VALUE)));
        mov(cb, member_opnd(REG1, rb_control_frame_t, ep), REG0);
        mov(cb, REG0, recv);
        mov(cb, member_opnd(REG1, rb_control_frame_t, self), REG0);
    }

    // Verify that we are calling the right function
    if (YJIT_CHECK_MODE > 0) {
        // Save YJIT registers
        yjit_save_regs(cb);

        // Call check_cfunc_dispatch
        mov(cb, RDI, recv);
        jit_mov_gc_ptr(jit, cb, RSI, (VALUE)ci);
        mov(cb, RDX, const_ptr_opnd((void *)cfunc->func));
        jit_mov_gc_ptr(jit, cb, RCX, (VALUE)cme);
        call_ptr(cb, REG0, (void *)&check_cfunc_dispatch);

        // Load YJIT registers
        yjit_load_regs(cb);
    }

    // Save YJIT registers
    yjit_save_regs(cb);

    // Copy SP into RAX because REG_SP will get overwritten
    lea(cb, RAX, ctx_sp_opnd(ctx, 0));

    // Non-variadic method
    if (cfunc->argc >= 0) {
        // Copy the arguments from the stack to the C argument registers
        // self is the 0th argument and is at index argc from the stack top
        for (int32_t i = 0; i < argc + 1; ++i)
        {
            x86opnd_t stack_opnd = mem_opnd(64, RAX, -(argc + 1 - i) * SIZEOF_VALUE);
            x86opnd_t c_arg_reg = C_ARG_REGS[i];
            mov(cb, c_arg_reg, stack_opnd);
        }
    }
    // Variadic method
    if (cfunc->argc == -1) {
        // The method gets a pointer to the first argument
        // rb_f_puts(int argc, VALUE *argv, VALUE recv)
        mov(cb, C_ARG_REGS[0], imm_opnd(argc));
        lea(cb, C_ARG_REGS[1], mem_opnd(64, RAX, -(argc) * SIZEOF_VALUE));
        mov(cb, C_ARG_REGS[2], mem_opnd(64, RAX, -(argc + 1) * SIZEOF_VALUE));
    }

    // Pop the C function arguments from the stack (in the caller)
    ctx_stack_pop(ctx, argc + 1);

    // Call the C function
    // VALUE ret = (cfunc->func)(recv, argv[0], argv[1]);
    // cfunc comes from compile-time cme->def, which we assume to be stable.
    // Invalidation logic is in rb_yjit_method_lookup_change()
    call_ptr(cb, REG0, (void*)cfunc->func);

    // Load YJIT registers
    yjit_load_regs(cb);

    // Push the return value on the Ruby stack
    x86opnd_t stack_ret = ctx_stack_push(ctx, TYPE_UNKNOWN);
    mov(cb, stack_ret, RAX);

    // If this function needs a Ruby stack frame
    if (cfunc_needs_frame(cfunc)) {
        // Pop the stack frame (ec->cfp++)
        add(
            cb,
            member_opnd(REG_EC, rb_execution_context_t, cfp),
            imm_opnd(sizeof(rb_control_frame_t))
        );
    }

    // TODO: gen_oswb_iseq() jumps to the next instruction with ctx->sp_offset == 0
    // after the call, while this does not. This difference prevents
    // the two call types from sharing the same successor.

    // Jump (fall through) to the call continuation block
    // We do this to end the current block after the call
    jit_jump_to_next_insn(jit, ctx);
    return YJIT_END_BLOCK;
}

bool rb_simple_iseq_p(const rb_iseq_t *iseq);

static void
gen_return_branch(codeblock_t* cb, uint8_t* target0, uint8_t* target1, uint8_t shape)
{
    switch (shape)
    {
        case SHAPE_NEXT0:
        case SHAPE_NEXT1:
        RUBY_ASSERT(false);
        break;

        case SHAPE_DEFAULT:
        mov(cb, REG0, const_ptr_opnd(target0));
        mov(cb, member_opnd(REG_CFP, rb_control_frame_t, jit_return), REG0);
        break;
    }
}

static codegen_status_t
gen_oswb_iseq(jitstate_t *jit, ctx_t *ctx, const struct rb_callinfo *ci, const rb_callable_method_entry_t *cme, int32_t argc)
{
    const rb_iseq_t *iseq = def_iseq_ptr(cme->def);
    const VALUE* start_pc = iseq->body->iseq_encoded;
    int num_params = iseq->body->param.size;
    int num_locals = iseq->body->local_table_size - num_params;

    if (num_params != argc) {
        GEN_COUNTER_INC(cb, oswb_iseq_argc_mismatch);
        return YJIT_CANT_COMPILE;
    }

    if (!rb_simple_iseq_p(iseq)) {
        // Only handle iseqs that have simple parameters.
        // See vm_callee_setup_arg().
        GEN_COUNTER_INC(cb, oswb_iseq_not_simple);
        return YJIT_CANT_COMPILE;
    }

    if (vm_ci_flag(ci) & VM_CALL_TAILCALL) {
        // We can't handle tailcalls
        GEN_COUNTER_INC(cb, oswb_iseq_tailcall);
        return YJIT_CANT_COMPILE;
    }

    // Create a size-exit to fall back to the interpreter
    uint8_t* side_exit = yjit_side_exit(jit, ctx);

    // Check for interrupts
    yjit_check_ints(cb, side_exit);

    // Points to the receiver operand on the stack
    x86opnd_t recv = ctx_stack_opnd(ctx, argc);

    // Store the updated SP on the current frame (pop arguments and receiver)
    lea(cb, REG0, ctx_sp_opnd(ctx, sizeof(VALUE) * -(argc + 1)));
    mov(cb, member_opnd(REG_CFP, rb_control_frame_t, sp), REG0);

    // Store the next PC in the current frame
    mov(cb, REG0, const_ptr_opnd(jit->pc + insn_len(BIN(opt_send_without_block))));
    mov(cb, mem_opnd(64, REG_CFP, offsetof(rb_control_frame_t, pc)), REG0);

    // Stack overflow check
    // #define CHECK_VM_STACK_OVERFLOW0(cfp, sp, margin)
    ADD_COMMENT(cb, "stack overflow check");
    lea(cb, REG0, ctx_sp_opnd(ctx, sizeof(VALUE) * (num_locals + iseq->body->stack_max) + sizeof(rb_control_frame_t)));
    cmp(cb, REG_CFP, REG0);
    jle_ptr(cb, COUNTED_EXIT(side_exit, oswb_se_cf_overflow));

    // Adjust the callee's stack pointer
    lea(cb, REG0, ctx_sp_opnd(ctx, sizeof(VALUE) * (3 + num_locals)));

    // Initialize local variables to Qnil
    for (int i = 0; i < num_locals; i++) {
        mov(cb, mem_opnd(64, REG0, sizeof(VALUE) * (i - num_locals - 3)), imm_opnd(Qnil));
    }

    // Put compile time cme into REG1. It's assumed to be valid because we are notified when
    // any cme we depend on become outdated. See rb_yjit_method_lookup_change().
    jit_mov_gc_ptr(jit, cb, REG1, (VALUE)cme);
    // Write method entry at sp[-3]
    // sp[-3] = me;
    mov(cb, mem_opnd(64, REG0, 8 * -3), REG1);

    // Write block handler at sp[-2]
    // sp[-2] = block_handler;
    mov(cb, mem_opnd(64, REG0, 8 * -2), imm_opnd(VM_BLOCK_HANDLER_NONE));

    // Write env flags at sp[-1]
    // sp[-1] = frame_type;
    uint64_t frame_type = VM_FRAME_MAGIC_METHOD | VM_ENV_FLAG_LOCAL;
    mov(cb, mem_opnd(64, REG0, 8 * -1), imm_opnd(frame_type));

    // Allocate a new CFP (ec->cfp--)
    sub(cb, REG_CFP, imm_opnd(sizeof(rb_control_frame_t)));
    mov(cb, member_opnd(REG_EC, rb_execution_context_t, cfp), REG_CFP);

    // Setup the new frame
    // *cfp = (const struct rb_control_frame_struct) {
    //    .pc         = pc,
    //    .sp         = sp,
    //    .iseq       = iseq,
    //    .self       = recv,
    //    .ep         = sp - 1,
    //    .block_code = 0,
    //    .__bp__     = sp,
    // };
    mov(cb, member_opnd(REG_CFP, rb_control_frame_t, block_code), imm_opnd(0));
    mov(cb, member_opnd(REG_CFP, rb_control_frame_t, sp), REG0);
    mov(cb, member_opnd(REG_CFP, rb_control_frame_t, __bp__), REG0);
    sub(cb, REG0, imm_opnd(sizeof(VALUE)));
    mov(cb, member_opnd(REG_CFP, rb_control_frame_t, ep), REG0);
    mov(cb, REG0, recv);
    mov(cb, member_opnd(REG_CFP, rb_control_frame_t, self), REG0);
    jit_mov_gc_ptr(jit, cb, REG0, (VALUE)iseq);
    mov(cb, member_opnd(REG_CFP, rb_control_frame_t, iseq), REG0);
    mov(cb, REG0, const_ptr_opnd(start_pc));
    mov(cb, member_opnd(REG_CFP, rb_control_frame_t, pc), REG0);

    // Stub so we can return to JITted code
    blockid_t return_block = { jit->iseq, jit_next_insn_idx(jit) };

    // Create a context for the callee
    ctx_t callee_ctx = DEFAULT_CTX;

    // Set the argument types in the callee's context
    for (int32_t arg_idx = 0; arg_idx < argc; ++arg_idx) {
        val_type_t arg_type = ctx_get_opnd_type(ctx, OPND_STACK(argc - arg_idx - 1));
        ctx_set_local_type(&callee_ctx, arg_idx, arg_type);
    }
    val_type_t recv_type = ctx_get_opnd_type(ctx, OPND_STACK(argc));
    ctx_set_opnd_type(&callee_ctx, OPND_SELF, recv_type);

    // Pop arguments and receiver in return context, push the return value
    // After the return, the JIT and interpreter SP will match up
    ctx_t return_ctx = *ctx;
    ctx_stack_pop(&return_ctx, argc + 1);
    ctx_stack_push(&return_ctx, TYPE_UNKNOWN);
    return_ctx.sp_offset = 0;
    return_ctx.chain_depth = 0;

    // Write the JIT return address on the callee frame
    gen_branch(
        jit->block,
        ctx,
        return_block,
        &return_ctx,
        return_block,
        &return_ctx,
        gen_return_branch
    );

    //print_str(cb, "calling Ruby func:");
    //print_str(cb, rb_id2name(vm_ci_mid(ci)));

    // Load the updated SP from the CFP
    mov(cb, REG_SP, member_opnd(REG_CFP, rb_control_frame_t, sp));

    // Directly jump to the entry point of the callee
    gen_direct_jump(
        jit->block,
        &callee_ctx,
        (blockid_t){ iseq, 0 }
    );

    return YJIT_END_BLOCK;
}

static codegen_status_t
gen_opt_send_without_block(jitstate_t* jit, ctx_t* ctx)
{
    // Relevant definitions:
    // rb_execution_context_t       : vm_core.h
    // invoker, cfunc logic         : method.h, vm_method.c
    // rb_callinfo                  : vm_callinfo.h
    // rb_callable_method_entry_t   : method.h
    // vm_call_cfunc_with_frame     : vm_insnhelper.c
    //
    // For a general overview for how the interpreter calls methods,
    // see vm_call_method().

    struct rb_call_data *cd = (struct rb_call_data *)jit_get_arg(jit, 0);
    const struct rb_callinfo *ci = cd->ci; // info about the call site

    int32_t argc = (int32_t)vm_ci_argc(ci);
    ID mid = vm_ci_mid(ci);

    // Don't JIT calls with keyword splat
    if (vm_ci_flag(ci) & VM_CALL_KW_SPLAT) {
        GEN_COUNTER_INC(cb, oswb_kw_splat);
        return YJIT_CANT_COMPILE;
    }

    // Don't JIT calls that aren't simple
    if (!(vm_ci_flag(ci) & VM_CALL_ARGS_SIMPLE)) {
        GEN_COUNTER_INC(cb, oswb_callsite_not_simple);
        return YJIT_CANT_COMPILE;
    }

    // Defer compilation so we can specialize on class of receiver
    if (!jit_at_current_insn(jit)) {
        defer_compilation(jit->block, jit->insn_idx, ctx);
        return YJIT_END_BLOCK;
    }

    VALUE comptime_recv = jit_peek_at_stack(jit, ctx, argc);
    VALUE comptime_recv_klass = CLASS_OF(comptime_recv);

    // Guard that the receiver has the same class as the one from compile time
    uint8_t *side_exit = yjit_side_exit(jit, ctx);

    // Points to the receiver operand on the stack
    x86opnd_t recv = ctx_stack_opnd(ctx, argc);
    insn_opnd_t recv_opnd = OPND_STACK(argc);
    mov(cb, REG0, recv);
    if (!jit_guard_known_klass(jit, ctx, comptime_recv_klass, recv_opnd, OSWB_MAX_DEPTH, side_exit)) {
        return YJIT_CANT_COMPILE;
    }

    // Do method lookup
    const rb_callable_method_entry_t *cme = rb_callable_method_entry(comptime_recv_klass, mid);
    if (!cme) {
        // TODO: counter
        return YJIT_CANT_COMPILE;
    }

    switch (METHOD_ENTRY_VISI(cme)) {
    case METHOD_VISI_PUBLIC:
        // Can always call public methods
        break;
    case METHOD_VISI_PRIVATE:
        if (!(vm_ci_flag(ci) & VM_CALL_FCALL)) {
            // Can only call private methods with FCALL callsites.
            // (at the moment they are callsites without a receiver or an explicit `self` receiver)
            return YJIT_CANT_COMPILE;
        }
        break;
    case METHOD_VISI_PROTECTED:
        jit_protected_callee_ancestry_guard(jit, cb, cme, side_exit);
        break;
    case METHOD_VISI_UNDEF:
        RUBY_ASSERT(false && "cmes should always have a visibility");
        break;
    }

    // Register block for invalidation
    RUBY_ASSERT(cme->called_id == mid);
    assume_method_lookup_stable(comptime_recv_klass, cme, jit->block);

    // Method calls may corrupt types
    ctx_clear_local_types(ctx);

    switch (cme->def->type) {
    case VM_METHOD_TYPE_ISEQ:
        return gen_oswb_iseq(jit, ctx, ci, cme, argc);
    case VM_METHOD_TYPE_CFUNC:
        return gen_oswb_cfunc(jit, ctx, ci, cme, argc);
    case VM_METHOD_TYPE_IVAR:
        if (argc != 0) {
            // Argument count mismatch. Getters take no arguments.
            GEN_COUNTER_INC(cb, oswb_getter_arity);
            return YJIT_CANT_COMPILE;
        }
        else {
            mov(cb, REG0, recv);

            ID ivar_name = cme->def->body.attr.id;
            return gen_get_ivar(jit, ctx, OSWB_MAX_DEPTH, comptime_recv, ivar_name, recv_opnd, side_exit);
        }
    case VM_METHOD_TYPE_ATTRSET:
        GEN_COUNTER_INC(cb, oswb_ivar_set_method);
        return YJIT_CANT_COMPILE;
    case VM_METHOD_TYPE_BMETHOD:
        GEN_COUNTER_INC(cb, oswb_bmethod);
        return YJIT_CANT_COMPILE;
    case VM_METHOD_TYPE_ZSUPER:
        GEN_COUNTER_INC(cb, oswb_zsuper_method);
        return YJIT_CANT_COMPILE;
    case VM_METHOD_TYPE_ALIAS:
        GEN_COUNTER_INC(cb, oswb_alias_method);
        return YJIT_CANT_COMPILE;
    case VM_METHOD_TYPE_UNDEF:
        GEN_COUNTER_INC(cb, oswb_undef_method);
        return YJIT_CANT_COMPILE;
    case VM_METHOD_TYPE_NOTIMPLEMENTED:
        GEN_COUNTER_INC(cb, oswb_not_implemented_method);
        return YJIT_CANT_COMPILE;
    case VM_METHOD_TYPE_OPTIMIZED:
        GEN_COUNTER_INC(cb, oswb_optimized_method);
        return YJIT_CANT_COMPILE;
    case VM_METHOD_TYPE_MISSING:
        GEN_COUNTER_INC(cb, oswb_missing_method);
        return YJIT_CANT_COMPILE;
    case VM_METHOD_TYPE_REFINED:
        GEN_COUNTER_INC(cb, oswb_refined_method);
        return YJIT_CANT_COMPILE;
    // no default case so compiler issues a warning if this is not exhaustive
    }

    return YJIT_CANT_COMPILE;
}

static codegen_status_t
gen_leave(jitstate_t* jit, ctx_t* ctx)
{
    // Only the return value should be on the stack
    RUBY_ASSERT(ctx->stack_size == 1);

    // Create a size-exit to fall back to the interpreter
    uint8_t* side_exit = yjit_side_exit(jit, ctx);

    // Load environment pointer EP from CFP
    mov(cb, REG0, member_opnd(REG_CFP, rb_control_frame_t, ep));

    // if (flags & VM_FRAME_FLAG_FINISH) != 0
    ADD_COMMENT(cb, "check for finish frame");
    x86opnd_t flags_opnd = mem_opnd(64, REG0, sizeof(VALUE) * VM_ENV_DATA_INDEX_FLAGS);
    test(cb, flags_opnd, imm_opnd(VM_FRAME_FLAG_FINISH));
    jnz_ptr(cb, COUNTED_EXIT(side_exit, leave_se_finish_frame));

    // Check for interrupts
    yjit_check_ints(cb, COUNTED_EXIT(side_exit, leave_se_interrupt));

    // Load the return value
    mov(cb, REG0, ctx_stack_pop(ctx, 1));

    // Pop the current frame (ec->cfp++)
    // Note: the return PC is already in the previous CFP
    add(cb, REG_CFP, imm_opnd(sizeof(rb_control_frame_t)));
    mov(cb, member_opnd(REG_EC, rb_execution_context_t, cfp), REG_CFP);

    // Push the return value on the caller frame
    // The SP points one above the topmost value
    add(cb, member_opnd(REG_CFP, rb_control_frame_t, sp), imm_opnd(SIZEOF_VALUE));
    mov(cb, REG_SP, member_opnd(REG_CFP, rb_control_frame_t, sp));
    mov(cb, mem_opnd(64, REG_SP, -SIZEOF_VALUE), REG0);

    // Jump to the JIT return address on the frame that was just popped
    const int32_t offset_to_jit_return = -((int32_t)sizeof(rb_control_frame_t)) + (int32_t)offsetof(rb_control_frame_t, jit_return);
    jmp_rm(cb, mem_opnd(64, REG_CFP, offset_to_jit_return));

    return YJIT_END_BLOCK;
}

RUBY_EXTERN rb_serial_t ruby_vm_global_constant_state;

static codegen_status_t
gen_opt_getinlinecache(jitstate_t *jit, ctx_t *ctx)
{
    VALUE jump_offset = jit_get_arg(jit, 0);
    VALUE const_cache_as_value = jit_get_arg(jit, 1);
    IC ic = (IC)const_cache_as_value;

    // See vm_ic_hit_p().
    struct iseq_inline_constant_cache_entry *ice = ic->entry;
    if (!ice || // cache not filled
        ice->ic_serial != ruby_vm_global_constant_state || // cache out of date
        ice->ic_cref /* cache only valid for certain lexical scopes */) {
        // In these cases, leave a block that unconditionally side exits
        // for the interpreter to invalidate.
        return YJIT_CANT_COMPILE;
    }

    // Optimize for single ractor mode.
    // FIXME: This leaks when st_insert raises NoMemoryError
    if (!assume_single_ractor_mode(jit->block)) return YJIT_CANT_COMPILE;

    // Invalidate output code on any and all constant writes
    // FIXME: This leaks when st_insert raises NoMemoryError
    assume_stable_global_constant_state(jit->block);

    x86opnd_t stack_top = ctx_stack_push(ctx, TYPE_UNKNOWN);
    jit_mov_gc_ptr(jit, cb, REG0, ice->value);
    mov(cb, stack_top, REG0);

    // Jump over the code for filling the cache
    uint32_t jump_idx = jit_next_insn_idx(jit) + (int32_t)jump_offset;
    gen_direct_jump(
        jit->block,
        ctx,
        (blockid_t){ .iseq = jit->iseq, .idx = jump_idx }
    );

    return YJIT_END_BLOCK;
}

static void
yjit_reg_op(int opcode, codegen_fn gen_fn)
{
    RUBY_ASSERT(opcode >= 0 && opcode < VM_INSTRUCTION_SIZE);
    // Check that the op wasn't previously registered
    RUBY_ASSERT(gen_fns[opcode] == NULL);

    gen_fns[opcode] = gen_fn;
}

void
yjit_init_codegen(void)
{
    // Initialize the code blocks
    uint32_t mem_size = 128 * 1024 * 1024;
    uint8_t *mem_block = alloc_exec_mem(mem_size);

    cb = &block;
    cb_init(cb, mem_block, mem_size/2);

    ocb = &outline_block;
    cb_init(ocb, mem_block + mem_size/2, mem_size/2);

    // Generate the interpreter exit code for leave
    leave_exit_code = yjit_gen_leave_exit(cb);

    // Map YARV opcodes to the corresponding codegen functions
    yjit_reg_op(BIN(dup), gen_dup);
    yjit_reg_op(BIN(nop), gen_nop);
    yjit_reg_op(BIN(pop), gen_pop);
    yjit_reg_op(BIN(putnil), gen_putnil);
    yjit_reg_op(BIN(putobject), gen_putobject);
    yjit_reg_op(BIN(putobject_INT2FIX_0_), gen_putobject_int2fix);
    yjit_reg_op(BIN(putobject_INT2FIX_1_), gen_putobject_int2fix);
    yjit_reg_op(BIN(putself), gen_putself);
    yjit_reg_op(BIN(getlocal_WC_0), gen_getlocal_wc0);
    yjit_reg_op(BIN(getlocal_WC_1), gen_getlocal_wc1);
    yjit_reg_op(BIN(setlocal_WC_0), gen_setlocal_wc0);
    yjit_reg_op(BIN(getinstancevariable), gen_getinstancevariable);
    yjit_reg_op(BIN(setinstancevariable), gen_setinstancevariable);
    yjit_reg_op(BIN(opt_lt), gen_opt_lt);
    yjit_reg_op(BIN(opt_le), gen_opt_le);
    yjit_reg_op(BIN(opt_ge), gen_opt_ge);
    yjit_reg_op(BIN(opt_gt), gen_opt_gt);
    yjit_reg_op(BIN(opt_aref), gen_opt_aref);
    yjit_reg_op(BIN(opt_and), gen_opt_and);
    yjit_reg_op(BIN(opt_or), gen_opt_or);
    yjit_reg_op(BIN(opt_minus), gen_opt_minus);
    yjit_reg_op(BIN(opt_plus), gen_opt_plus);
    yjit_reg_op(BIN(opt_getinlinecache), gen_opt_getinlinecache);
    yjit_reg_op(BIN(branchif), gen_branchif);
    yjit_reg_op(BIN(branchunless), gen_branchunless);
    yjit_reg_op(BIN(jump), gen_jump);
    yjit_reg_op(BIN(opt_send_without_block), gen_opt_send_without_block);
    yjit_reg_op(BIN(leave), gen_leave);
}
