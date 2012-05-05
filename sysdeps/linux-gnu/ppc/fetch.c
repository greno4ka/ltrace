/*
 * This file is part of ltrace.
 * Copyright (C) 2012 Petr Machata, Red Hat Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA
 * 02110-1301 USA
 */

#include <assert.h>
#include <ptrace.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h>

#include "backend.h"
#include "fetch.h"
#include "type.h"
#include "proc.h"
#include "value.h"

static int allocate_gpr(struct fetch_context *ctx, struct Process *proc,
			struct arg_type_info *info, struct value *valuep);

/* Floating point registers have the same width on 32-bit as well as
 * 64-bit PPC, but <ucontext.h> presents a different API depending on
 * whether ltrace is PPC32 or PPC64.
 *
 * This is PPC64 definition.  The PPC32 is simply an array of 33
 * doubles, and doesn't contain the terminating pad.  Both seem
 * compatible enough.  */
struct fpregs_t
{
	double fpregs[32];
	double fpscr;
	unsigned int _pad[2];
};

typedef uint32_t gregs32_t[48];
typedef uint64_t gregs64_t[48];

struct fetch_context {
	target_address_t stack_pointer;
	int greg;
	int freg;
	int ret_struct;

	union {
		gregs32_t r32;
		gregs64_t r64;
	} regs;
	struct fpregs_t fpregs;

};

static int
fetch_context_init(struct Process *proc, struct fetch_context *context)
{
	context->greg = 3;
	context->freg = 1;

	if (proc->e_machine == EM_PPC64)
		context->stack_pointer = proc->stack_pointer + 8;
	else
		context->stack_pointer = proc->stack_pointer + 112;

	/* When ltrace is 64-bit, we might use PTRACE_GETREGS to
	 * obtain 64-bit as well as 32-bit registers.  But if we do it
	 * this way, 32-bit ltrace can obtain 64-bit registers.
	 *
	 * XXX this direction is not supported as of this writing, but
	 * should be eventually.  */
	if (proc->e_machine == EM_PPC64) {
		if (ptrace(PTRACE_GETREGS64, proc->pid, 0,
			   &context->regs.r64) < 0)
			return -1;
	} else if (ptrace(PTRACE_GETREGS, proc->pid, 0,
			  &context->regs.r32) < 0) {
		return -1;
	}

	if (ptrace(PTRACE_GETFPREGS, proc->pid, 0, &context->fpregs) < 0)
		return -1;

	return 0;
}

struct fetch_context *
arch_fetch_arg_init(enum tof type, struct Process *proc,
		    struct arg_type_info *ret_info)
{
	struct fetch_context *context = malloc(sizeof(*context));
	if (context == NULL
	    || fetch_context_init(proc, context) < 0) {
		free(context);
		return NULL;
	}

	/* Aggregates or unions of any length, and character strings
	 * of length longer than 8 bytes, will be returned in a
	 * storage buffer allocated by the caller. The caller will
	 * pass the address of this buffer as a hidden first argument
	 * in r3, causing the first explicit argument to be passed in
	 * r4.  */
	context->ret_struct = ret_info->type == ARGTYPE_STRUCT;
	if (context->ret_struct)
		context->greg++;

	return context;
}

struct fetch_context *
arch_fetch_arg_clone(struct Process *proc,
		     struct fetch_context *context)
{
	struct fetch_context *clone = malloc(sizeof(*context));
	if (clone == NULL)
		return NULL;
	*clone = *context;
	return clone;
}

static int
allocate_stack_slot(struct fetch_context *ctx, struct Process *proc,
		    struct arg_type_info *info, struct value *valuep)
{
	assert(!"allocate_stack_slot not implemented");
	abort();
}

static int
allocate_float(struct fetch_context *ctx, struct Process *proc,
	       struct arg_type_info *info, struct value *valuep)
{
	int pool = proc->e_machine == EM_PPC ? 8 : 13;
	if (ctx->freg <= pool) {
		union {
			double d;
			float f;
			char buf[0];
		} u = { .d = ctx->fpregs.fpregs[ctx->freg] };

		ctx->freg++;
		ctx->greg++;

		size_t sz = sizeof(double);
		if (info->type == ARGTYPE_FLOAT) {
			sz = sizeof(float);
			u.f = (float)u.d;
		}

		if (value_reserve(valuep, sz) == NULL)
			return -1;

		memcpy(value_get_raw_data(valuep), u.buf, sz);
		return 0;
	}
	return allocate_stack_slot(ctx, proc, info, valuep);
}

static uint64_t
read_gpr(struct fetch_context *ctx, struct Process *proc, int reg_num)
{
	if (proc->e_machine == EM_PPC)
		return ctx->regs.r32[reg_num];
	else
		return ctx->regs.r64[reg_num];
}

static int
allocate_gpr(struct fetch_context *ctx, struct Process *proc,
	     struct arg_type_info *info, struct value *valuep)
{
	if (ctx->greg > 10)
		return allocate_stack_slot(ctx, proc, info, valuep);

	int reg_num = ctx->greg++;
	if (valuep == NULL)
		return 0;

	size_t sz = type_sizeof(proc, info);
	if (sz == (size_t)-1)
		return -1;
	assert(sz == 1 || sz == 2 || sz == 4 || sz == 8);
	if (value_reserve(valuep, sz) == NULL)
		return -1;

	union {
		uint64_t i64;
		uint32_t i32;
		uint16_t i16;
		uint8_t i8;
		char buf[0];
	} u;

	u.i64 = read_gpr(ctx, proc, reg_num);

	/* The support for little endian PowerPC is in upstream Linux
	 * and BFD, and Unix-like Solaris, which we might well support
	 * at some point, runs PowerPC in little endian as well.  So
	 * let's do it the hard way.  */
	switch (sz) {
	case 1:
		u.i8 = u.i64;
		break;
	case 2:
		u.i16 = u.i64;
		break;
	case 4:
		u.i32 = u.i64;
	case 8:
		break;
	}

	memcpy(value_get_raw_data(valuep), u.buf, sz);
	return 0;
}

static int
allocate_argument(struct fetch_context *ctx, struct Process *proc,
		  struct arg_type_info *info, struct value *valuep)
{
	switch (info->type) {
	case ARGTYPE_VOID:
		value_set_word(valuep, 0);
		return 0;

	case ARGTYPE_INT:
	case ARGTYPE_UINT:
	case ARGTYPE_LONG:
	case ARGTYPE_ULONG:
	case ARGTYPE_CHAR:
	case ARGTYPE_SHORT:
	case ARGTYPE_USHORT:
	case ARGTYPE_POINTER:
		return allocate_gpr(ctx, proc, info, valuep);

	case ARGTYPE_FLOAT:
	case ARGTYPE_DOUBLE:
		return allocate_float(ctx, proc, info, valuep);

	case ARGTYPE_STRUCT:
		/* Fixed size aggregates and unions passed by value
		 * are mapped to as many doublewords of the parameter
		 * save area as the value uses in memory.  [...] The
		 * first eight doublewords mapped to the parameter
		 * save area correspond to the registers r3 through
		 * r10.  */
		assert(!"arch_fetch_arg_next structures not implemented");
		abort();

	case ARGTYPE_ARRAY:
		/* Arrays decay into pointers.  */
		assert(info->type != ARGTYPE_ARRAY);
		abort();
	}

	assert(info->type != info->type);
	abort();
}

int
arch_fetch_arg_next(struct fetch_context *ctx, enum tof type,
		    struct Process *proc,
		    struct arg_type_info *info, struct value *valuep)
{
	return allocate_argument(ctx, proc, info, valuep);
}

int
arch_fetch_retval(struct fetch_context *ctx, enum tof type,
		  struct Process *proc, struct arg_type_info *info,
		  struct value *valuep)
{
	if (ctx->ret_struct) {
		assert(info->type == ARGTYPE_STRUCT);

		uint64_t addr = read_gpr(ctx, proc, 3);
		value_init(valuep, proc, NULL, info, 0);

		if (value_pass_by_reference(valuep) < 0) {
			value_destroy(valuep);
			return -1;
		}

		valuep->where = VAL_LOC_INFERIOR;
		valuep->u.address = (target_address_t)addr;
		return 0;
	}

	if (fetch_context_init(proc, ctx) < 0)
		return -1;
	return allocate_argument(ctx, proc, info, valuep);
}

void
arch_fetch_arg_done(struct fetch_context *context)
{
	free(context);
}