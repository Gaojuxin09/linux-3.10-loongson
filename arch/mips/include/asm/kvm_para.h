/*
 * Copyright (C) 2018 Loongson Inc.
 * Author:  Bibo Mao, maobibo@loongson.cn
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 */
#ifndef _ASM_MIPS_KVM_PARA_H
#define _ASM_MIPS_KVM_PARA_H

#include <uapi/asm/kvm_para.h>

#define KVM_HYPERCALL ".word 0x42000028"

#define tlbmiss_tlbwr_normal		0x0
#define tlbmiss_tlbwr_huge		0x1
#define tlbm_tlbp_and_tlbwi_normal	0x1000
#define tlbm_tlbp_and_tlbwi_huge	0x1001
#define tlbl_tlbp_and_tlbwi_normal	0x2000
#define tlbl_tlbp_and_tlbwi_huge	0x2001
#define tlbs_tlbp_and_tlbwi_normal	0x3000
#define tlbs_tlbp_and_tlbwi_huge	0x3001

#define KVM_MIPS_GET_RTAS_INFO		0x10


/*
 * Hypercalls for KVM.
 *
 * Hypercall number is passed in v0.
 * Return value will be placed in v0.
 * Up to 3 arguments are passed in a0, a1, and a2.
 */
static inline unsigned long kvm_hypercall0(unsigned long num)
{
	register unsigned long n asm("v0");
	register unsigned long r asm("v0");

	n = num;
	__asm__ __volatile__(
		KVM_HYPERCALL
		: "=r" (r) : "r" (n) : "memory"
		);

	return r;
}

static inline unsigned long kvm_hypercall1(unsigned long num,
					unsigned long arg0)
{
	register unsigned long n asm("v0");
	register unsigned long r asm("v0");
	register unsigned long a0 asm("a0");

	n = num;
	a0 = arg0;
	__asm__ __volatile__(
		KVM_HYPERCALL
		: "=r" (r) : "r" (n), "r" (a0) : "memory"
		);

	return r;
}

static inline unsigned long kvm_hypercall2(unsigned long num,
					unsigned long arg0, unsigned long arg1)
{
	register unsigned long n asm("v0");
	register unsigned long r asm("v0");
	register unsigned long a0 asm("a0");
	register unsigned long a1 asm("a1");

	n = num;
	a0 = arg0;
	a1 = arg1;
	__asm__ __volatile__(
		KVM_HYPERCALL
		: "=r" (r) : "r" (n), "r" (a0), "r" (a1) : "memory"
		);

	return r;
}

static inline unsigned long kvm_hypercall3(unsigned long num,
	unsigned long arg0, unsigned long arg1, unsigned long arg2)
{
	register unsigned long n asm("v0");
	register unsigned long r asm("v0");
	register unsigned long a0 asm("a0");
	register unsigned long a1 asm("a1");
	register unsigned long a2 asm("a2");

	n = num;
	a0 = arg0;
	a1 = arg1;
	a2 = arg2;
	__asm__ __volatile__(
		KVM_HYPERCALL
		: "=r" (r) : "r" (n), "r" (a0), "r" (a1), "r" (a2) : "memory"
		);

	return r;
}

static inline unsigned long kvm_hypercall4(unsigned long num,
	unsigned long arg0, unsigned long arg1, unsigned long arg2, unsigned long arg3)
{
	register unsigned long n asm("v0");
	register unsigned long r asm("v0");
	register unsigned long a0 asm("a0");
	register unsigned long a1 asm("a1");
	register unsigned long a2 asm("a2");
	register unsigned long a3 asm("a3");

	n = num;
	a0 = arg0;
	a1 = arg1;
	a2 = arg2;
	a3 = arg3;
	__asm__ __volatile__(
			KVM_HYPERCALL
			: "=r" (r) : "r" (n), "r" (a0), "r" (a1), "r" (a2), "r" (a3) : "memory"
			);

	return r;
}

#endif /* _ASM_MIPS_KVM_PARA_H */
