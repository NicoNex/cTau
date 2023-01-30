#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "vm.h"
#include "../obj/obj.h"
#include "../code/code.h"

#define vm_current_frame(vm) (&vm->frames[vm->frame_idx])
#define vm_push_frame(vm, frame) vm->frames[++vm->frame_idx] = frame
#define vm_pop_frame(vm) (&vm->frames[vm->frame_idx--])

#define vm_stack_push(vm, obj) vm->stack[vm->sp++] = obj
#define vm_stack_pop(vm) (vm->stack[--vm->sp])
#define vm_stack_pop_ignore(vm) vm->sp--
#define vm_stack_peek(vm) (vm->stack[vm->sp-1])

#define DISPATCH() goto *jump_table[*frame->ip++]
#define UNHANDLED() puts("unhandled opcode"); return -1

#define ASSERT(obj, t) (obj->type == t)
#define ASSERT2(obj, t1, t2) (ASSERT(obj, t1) || ASSERT(obj, t2))
#define M_ASSERT(o1, o2, t) (ASSERT(o1, t) && ASSERT(o2, t))
#define M_ASSERT2(o1, o2, t1, t2) (ASSERT2(o1, t1, t2) && ASSERT2(o2, t1, t2))

static inline struct frame new_frame(struct object *cl, uint32_t base_ptr) {
	return (struct frame) {
		.cl = cl,
		.base_ptr = base_ptr,
		.ip = cl->data.cl->fn->instructions,
		.start = cl->data.cl->fn->instructions
	};
}

struct state new_state() {
	return (struct state) {
		.st = new_symbol_table(),
		.consts = calloc(0, sizeof(struct object *)),
		.nconsts = 0,
		.globals = {0}
	};
}

struct vm *new_vm(struct bytecode bytecode) {
	struct vm *vm = calloc(1, sizeof(struct vm));
	vm->state.consts = bytecode.consts;

	struct object *fn = new_function_obj(bytecode.insts, bytecode.len, 0, 0);
	struct object *cl = new_closure_obj(fn->data.fn, NULL, 0);
	vm->frames[0] = new_frame(cl, 0);

	return vm;
}

struct vm *new_vm_with_state(struct bytecode bytecode, struct state state) {
	struct vm *vm = calloc(1, sizeof(struct vm));
	vm->state = state;

	struct object *fn = new_function_obj(bytecode.insts, bytecode.len, 0, 0);
	struct object *cl = new_closure_obj(fn->data.fn, NULL, 0);
	vm->frames[0] = new_frame(cl, 0);

	return vm;
}

void vm_dispose(struct vm *vm) {
	for (size_t i = 0; i < STACK_SIZE; i++) {
		if (vm->stack[i] != NULL) {
			free(vm->stack[i]);
		}
	}

	free(vm);
}

static inline void vm_push_closure(struct vm *restrict vm, uint32_t const_idx, uint32_t num_free) {
	struct object *cnst = vm->state.consts[const_idx];

	if (cnst->type != obj_function) {
		printf("vm_push_closure: expected closure, but got %d\n", cnst->type);
		exit(1);
	}
	
	struct object **free = malloc(sizeof(struct object *) * num_free);
	for (int i = 0; i < num_free; i++) {
		free[i] = vm->stack[vm->sp-num_free+i];
	}

	struct object *cl = new_closure_obj(cnst->data.fn, free, num_free);
	vm->sp -= num_free;
	vm_stack_push(vm, cl);
}

static inline struct object *unwrap(struct object *o) {
	if (o->type == obj_getsetter) {
		// TODO: fill this.
	}
	return o;
}

static inline double to_double(struct object * restrict o) {
	if (ASSERT(o, obj_integer)) {
		return o->data.i;
	}
	return o->data.f;
}

static inline uint32_t is_truthy(struct object * restrict o) {
	switch (o->type) {
	case obj_boolean:
		return o == true_obj;
	case obj_integer:
		return o->data.i != 0;
	case obj_float:
		return o->data.f != 0;
	case obj_null:
		return 0;
	default:
		return 1;
	}
}

static inline void unsupported_operator_error(char *op, struct object *l, struct object *r) {
	printf("unsupported operator '%s' for types %s and %s\n", op, otype_str(l->type), otype_str(r->type));
	exit(1);
}

static inline void unsupported_prefix_operator_error(char *op, struct object *o) {
	printf("unsupported operator '%s' for type %s\n", op, otype_str(o->type));
	exit(1);
}

static inline void vm_exec_add(struct vm * restrict vm) {
	struct object *right = unwrap(vm_stack_pop(vm));
	struct object *left = unwrap(vm_stack_pop(vm));

	if (M_ASSERT(left, right, obj_integer)) {
		vm_stack_push(vm, new_integer_obj(left->data.i + right->data.i));
	} else if (M_ASSERT2(left, right, obj_integer, obj_float)) {
		double l = to_double(left);
		double r = to_double(right);
		vm_stack_push(vm, new_float_obj(l + r));
	} else if (M_ASSERT(left, right, obj_string)) {
		puts("adding two strings is not yet supported!");
		exit(1);
	} else {
		puts("unsupported operator '+' for the two types");
		exit(1);
	}
}

static inline void vm_exec_sub(struct vm * restrict vm) {
	struct object *right = unwrap(vm_stack_pop(vm));
	struct object *left = unwrap(vm_stack_pop(vm));

	if (M_ASSERT(left, right, obj_integer)) {
		vm_stack_push(vm, new_integer_obj(left->data.i - right->data.i));
	} else if (M_ASSERT2(left, right, obj_integer, obj_float)) {
		double l = to_double(left);
		double r = to_double(right);
		vm_stack_push(vm, new_float_obj(l - r));
	} else {
		unsupported_operator_error("-", left, right);
	}
}

static inline void vm_exec_mul(struct vm * restrict vm) {
	struct object *right = unwrap(vm_stack_pop(vm));
	struct object *left = unwrap(vm_stack_pop(vm));

	if (M_ASSERT(left, right, obj_integer)) {
		vm_stack_push(vm, new_integer_obj(left->data.i * right->data.i));
	} else if (M_ASSERT2(left, right, obj_integer, obj_float)) {
		double l = to_double(left);
		double r = to_double(right);
		vm_stack_push(vm, new_float_obj(l * r));
	} else {
		unsupported_operator_error("*", left, right);
	}
}

static inline void vm_exec_div(struct vm * restrict vm) {
	struct object *right = unwrap(vm_stack_pop(vm));
	struct object *left = unwrap(vm_stack_pop(vm));

	if (M_ASSERT(left, right, obj_integer)) {
		vm_stack_push(vm, new_integer_obj(left->data.i / right->data.i));
	} else if (M_ASSERT2(left, right, obj_integer, obj_float)) {
		double l = to_double(left);
		double r = to_double(right);
		vm_stack_push(vm, new_float_obj(l / r));
	} else {
		unsupported_operator_error("/", left, right);
	}
}

static inline void vm_exec_mod(struct vm * restrict vm) {
	struct object *right = unwrap(vm_stack_pop(vm));
	struct object *left = unwrap(vm_stack_pop(vm));

	if (!M_ASSERT(left, right, obj_integer)) {
		unsupported_operator_error("%", left, right);
	}
	vm_stack_push(vm, new_integer_obj(left->data.i % right->data.i));
}

static inline void vm_exec_and(struct vm * restrict vm) {
	struct object *right = unwrap(vm_stack_pop(vm));
	struct object *left = unwrap(vm_stack_pop(vm));

	vm_stack_push(vm, parse_bool(is_truthy(left) && is_truthy(right)));
}

static inline void vm_exec_or(struct vm * restrict vm) {
	struct object *right = unwrap(vm_stack_pop(vm));
	struct object *left = unwrap(vm_stack_pop(vm));

	vm_stack_push(vm, parse_bool(is_truthy(left) || is_truthy(right)));
}

static inline void vm_exec_eq(struct vm * restrict vm) {
	struct object *right = unwrap(vm_stack_pop(vm));
	struct object *left = unwrap(vm_stack_pop(vm));

	if (M_ASSERT2(left, right, obj_boolean, obj_null)) {
		vm_stack_push(vm, parse_bool(left == right));
	} else if (M_ASSERT(left, right, obj_integer)) {
		vm_stack_push(vm, parse_bool(left->data.i == right->data.i));
	} else if (M_ASSERT2(left, right, obj_integer, obj_float)) {
		double l = to_double(left);
		double r = to_double(right);
		vm_stack_push(vm, parse_bool(l == r));
	} else if (M_ASSERT(left, right, obj_string)) {
		char *l = left->data.str;
		char *r = right->data.str;
		struct object *res = left->len == right->len ? parse_bool(strcmp(l, r) == 0) : false_obj;
		vm_stack_push(vm, res);
	} else {
		vm_stack_push(vm, false_obj);
	}
}

static inline void vm_exec_not_eq(struct vm * restrict vm) {
	struct object *right = unwrap(vm_stack_pop(vm));
	struct object *left = unwrap(vm_stack_pop(vm));

	if (M_ASSERT2(left, right, obj_boolean, obj_null)) {
		vm_stack_push(vm, parse_bool(left != right));
	} else if (M_ASSERT(left, right, obj_integer)) {
		vm_stack_push(vm, parse_bool(left->data.i != right->data.i));
	} else if (M_ASSERT2(left, right, obj_integer, obj_float)) {
		double l = to_double(left);
		double r = to_double(right);
		vm_stack_push(vm, parse_bool(l != r));
	} else if (M_ASSERT(left, right, obj_string)) {
		char *l = left->data.str;
		char *r = right->data.str;
		struct object *res = left->len == right->len ? parse_bool(strcmp(l, r) != 0) : false_obj;
		vm_stack_push(vm, res);
	} else {
		vm_stack_push(vm, false_obj);
	}
}

static inline void vm_exec_greater_than(struct vm * restrict vm) {
	struct object *right = unwrap(vm_stack_pop(vm));
	struct object *left = unwrap(vm_stack_pop(vm));

	if (M_ASSERT(left, right, obj_integer)) {
		vm_stack_push(vm, parse_bool(left->data.i > right->data.i));
	} else if (M_ASSERT2(left, right, obj_integer, obj_float)) {
		double l = to_double(left);
		double r = to_double(right);
		vm_stack_push(vm, parse_bool(l > r));
	} else if (M_ASSERT(left, right, obj_string)) {
		char *l = left->data.str;
		char *r = right->data.str;
		vm_stack_push(vm, parse_bool(strcmp(l, r) > 0));
	} else {
		unsupported_operator_error(">", left, right);
	}
}

static inline void vm_exec_greater_than_eq(struct vm * restrict vm) {
	struct object *right = unwrap(vm_stack_pop(vm));
	struct object *left = unwrap(vm_stack_pop(vm));

	if (M_ASSERT(left, right, obj_integer)) {
		vm_stack_push(vm, parse_bool(left->data.i >= right->data.i));
	} else if (M_ASSERT2(left, right, obj_integer, obj_float)) {
		double l = to_double(left);
		double r = to_double(right);
		vm_stack_push(vm, parse_bool(l >= r));
	} else if (M_ASSERT(left, right, obj_string)) {
		char *l = left->data.str;
		char *r = right->data.str;
		vm_stack_push(vm, parse_bool(strcmp(l, r) >= 0));
	} else {
		unsupported_operator_error(">", left, right);
	}
}

static inline void vm_exec_minus(struct vm * restrict vm) {
	struct object *right = unwrap(vm_stack_pop(vm));

	switch (right->type) {
	case obj_integer:
		vm_stack_push(vm, new_integer_obj(-right->data.i));
	case obj_float:
		vm_stack_push(vm, new_float_obj(-right->data.f));
	default:
		unsupported_prefix_operator_error("-", right);
	}
}

static inline void vm_exec_bang(struct vm * restrict vm) {
	struct object *right = unwrap(vm_stack_pop(vm));

	switch (right->type) {
	case obj_boolean:
		vm_stack_push(vm, parse_bool(!right->data.i));
	case obj_null:
		vm_stack_push(vm, true_obj);
	default:
		vm_stack_push(vm, false_obj);
	}
}

static inline void vm_call_closure(struct vm * restrict vm, struct object *cl, size_t numargs) {
	int num_params = cl->data.cl->fn->num_params;

	if (num_params != numargs) {
		printf("wrong number of arguments: expected %d, got %lu\n", num_params, numargs);
		exit(1);
	}

	struct frame frame = new_frame(cl, vm->sp-numargs);
	vm_push_frame(vm, frame);
	vm->sp = frame.base_ptr + cl->data.cl->fn->num_locals;
}

static inline void vm_exec_call(struct vm * restrict vm, size_t numargs) {
	struct object *o = unwrap(vm->stack[vm->sp-1-numargs]);

	switch (o->type) {
	case obj_closure:
		return vm_call_closure(vm, o, numargs);
	case obj_builtin:
		puts("calling builtins is not yet supported");
		exit(1);
	default:
		puts("calling non-function");
		exit(1);
	}
}

static inline void vm_exec_return(struct vm * restrict vm) {
	struct frame *frame = vm_pop_frame(vm);
	vm->sp = frame->base_ptr - 1;
	vm_stack_push(vm, null_obj);
}

static inline void vm_exec_return_value(struct vm * restrict vm) {
	struct object *o = unwrap(vm_stack_pop(vm));
	struct frame *frame = vm_pop_frame(vm);
	vm->sp = frame->base_ptr - 1;
	vm_stack_push(vm, o);
}

struct object *vm_last_popped_stack_elem(struct vm * restrict vm) {
	return vm->stack[vm->sp];
}

/*
 * The following comment is taken from CPython's source:
 * https://github.com/python/cpython/blob/3.11/Python/ceval.c#L1243

 * Computed GOTOs, or
       the-optimization-commonly-but-improperly-known-as-"threaded code"
 * using gcc's labels-as-values extension
 * (http://gcc.gnu.org/onlinedocs/gcc/Labels-as-Values.html).

 * The traditional bytecode evaluation loop uses a "switch" statement, which
 * decent compilers will optimize as a single indirect branch instruction
 * combined with a lookup table of jump addresses. However, since the
 * indirect jump instruction is shared by all opcodes, the CPU will have a
 * hard time making the right prediction for where to jump next (actually,
 * it will be always wrong except in the uncommon case of a sequence of
 * several identical opcodes).

 * "Threaded code" in contrast, uses an explicit jump table and an explicit
 * indirect jump instruction at the end of each opcode. Since the jump
 * instruction is at a different address for each opcode, the CPU will make a
 * separate prediction for each of these instructions, which is equivalent to
 * predicting the second opcode of each opcode pair. These predictions have
 * a much better chance to turn out valid, especially in small bytecode loops.

 * A mispredicted branch on a modern CPU flushes the whole pipeline and
 * can cost several CPU cycles (depending on the pipeline depth),
 * and potentially many more instructions (depending on the pipeline width).
 * A correctly predicted branch, however, is nearly free.

 * At the time of this writing, the "threaded code" version is up to 15-20%
 * faster than the normal "switch" version, depending on the compiler and the
 * CPU architecture.

 * NOTE: care must be taken that the compiler doesn't try to "optimize" the
 * indirect jumps by sharing them between all opcodes. Such optimizations
 * can be disabled on gcc by using the -fno-gcse flag (or possibly
 * -fno-crossjumping).
 */

int vm_run(struct vm * restrict vm) {
#include "jump_table.h"

	register struct frame *frame = vm_current_frame(vm);
	DISPATCH();

	TARGET_CONST: {
		uint16_t idx = read_uint16(frame->ip);
		frame->ip += 2;
		vm_stack_push(vm, vm->state.consts[idx]);
		DISPATCH();
	}

	TARGET_TRUE: {
		vm_stack_push(vm, true_obj);
		DISPATCH();
	}

	TARGET_FALSE: {
		vm_stack_push(vm, false_obj);
		DISPATCH();
	}

	TARGET_NULL: {
		vm_stack_push(vm, null_obj);
		DISPATCH();
	}

	TARGET_LIST: {
		UNHANDLED();
		DISPATCH();
	}

	TARGET_MAP: {
		UNHANDLED();
		DISPATCH();
	}

	TARGET_CLOSURE: {
		uint16_t const_idx = read_uint16(frame->ip);
		uint8_t num_free = read_uint8(frame->ip+2);
		frame->ip += 3;
		vm_push_closure(vm, const_idx, num_free);
		DISPATCH();
	}

	TARGET_CURRENT_CLOSURE: {
		vm_stack_push(vm, frame->cl);
		DISPATCH();
	}

	TARGET_ADD: {
		vm_exec_add(vm);
		DISPATCH();
	}

	TARGET_SUB: {
		vm_exec_sub(vm);
		DISPATCH();
	}

	TARGET_MUL: {
		vm_exec_mul(vm);
		DISPATCH();
	}

	TARGET_DIV: {
		vm_exec_div(vm);
		DISPATCH();
	}

	TARGET_MOD: {
		vm_exec_mod(vm);
		DISPATCH();
	}

	TARGET_BW_AND: {
		vm_exec_and(vm);
		DISPATCH();
	}

	TARGET_BW_OR: {
		vm_exec_or(vm);
		DISPATCH();
	}

	TARGET_BW_XOR: {
		UNHANDLED();
		DISPATCH();
	}

	TARGET_BW_NOT: {
		UNHANDLED();
		DISPATCH();
	}

	TARGET_BW_LSHIFT: {
		UNHANDLED();
		DISPATCH();
	}

	TARGET_BW_RSHIFT: {
		UNHANDLED();
		DISPATCH();
	}

	TARGET_AND: {
		vm_exec_and(vm);
		DISPATCH();
	}

	TARGET_OR: {
		vm_exec_or(vm);
		DISPATCH();
	}

	TARGET_EQUAL: {
		vm_exec_eq(vm);
		DISPATCH();
	}

	TARGET_NOT_EQUAL: {
		vm_exec_not_eq(vm);
		DISPATCH();
	}

	TARGET_GREATER_THAN: {
		vm_exec_greater_than(vm);
		DISPATCH();
	}

	TARGET_GREATER_THAN_EQUAL: {
		vm_exec_greater_than_eq(vm);
		DISPATCH();
	}

	TARGET_MINUS: {
		vm_exec_minus(vm);
		DISPATCH();
	}

	TARGET_BANG: {
		vm_exec_bang(vm);
		DISPATCH();
	}

	TARGET_INDEX: {
		UNHANDLED();
		DISPATCH();
	}

	TARGET_CALL: {
		uint8_t num_args = read_uint8(frame->ip++);
		vm_exec_call(vm, num_args);
		frame = vm_current_frame(vm);
		DISPATCH();
	}

	TARGET_CONCURRENT_CALL: {
		UNHANDLED();
		DISPATCH();
	}

	TARGET_RETURN: {
		vm_exec_return(vm);
		frame = vm_current_frame(vm);
		DISPATCH();
	}

	TARGET_RETURN_VALUE: {
		vm_exec_return_value(vm);
		frame = vm_current_frame(vm);
		DISPATCH();
	}

	TARGET_JUMP: {
		uint16_t pos = read_uint16(frame->ip);
		frame->ip = &frame->start[pos];
		DISPATCH();
	}

	TARGET_JUMP_NOT_TRUTHY: {
		uint16_t pos = read_uint16(frame->ip);
		frame->ip += 2;

		struct object *cond = unwrap(vm_stack_pop(vm));
		if (!is_truthy(cond)) {
			frame->ip = &frame->start[pos];
		}
		DISPATCH();
	}

	TARGET_DOT: {
		UNHANDLED();
		DISPATCH();
	}

	TARGET_DEFINE: {
		UNHANDLED();
		DISPATCH();
	}

	TARGET_GET_GLOBAL: {
		int global_idx = read_uint16(frame->ip);
		frame->ip += 2;
		vm_stack_push(vm, vm->state.globals[global_idx]);
		DISPATCH();
	}

	TARGET_SET_GLOBAL: {
		int global_idx = read_uint16(frame->ip);
		frame->ip += 2;
		vm->state.globals[global_idx] = vm_stack_peek(vm);
		DISPATCH();
	}

	TARGET_GET_LOCAL: {
		int local_idx = read_uint8(frame->ip++);
		vm_stack_push(vm, vm->stack[frame->base_ptr+local_idx]);
		DISPATCH();
	}

	TARGET_SET_LOCAL: {
		int local_idx = read_uint8(frame->ip++);
		vm->stack[frame->base_ptr+local_idx] = vm_stack_peek(vm);
		DISPATCH();
	}

	TARGET_GET_BUILTIN: {
		UNHANDLED();
		DISPATCH();
	}

	TARGET_GET_FREE: {
		int free_idx = read_uint8(frame->ip++);
		struct object *cl = frame->cl;
		vm_stack_push(vm, cl->data.cl->free[free_idx]);
		DISPATCH();
	}

	TARGET_LOAD_MODULE: {
		UNHANDLED();
		DISPATCH();
	}

	TARGET_INTERPOLATE: {
		UNHANDLED();
		DISPATCH();
	}

	TARGET_POP: {
		vm_stack_pop_ignore(vm);
		DISPATCH();
	}

	TARGET_HALT:
		return 0;
}
