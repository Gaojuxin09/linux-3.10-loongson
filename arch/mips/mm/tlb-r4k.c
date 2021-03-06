/*
 * This file is subject to the terms and conditions of the GNU General Public
 * License.  See the file "COPYING" in the main directory of this archive
 * for more details.
 *
 * Copyright (C) 1996 David S. Miller (davem@davemloft.net)
 * Copyright (C) 1997, 1998, 1999, 2000 Ralf Baechle ralf@gnu.org
 * Carsten Langgaard, carstenl@mips.com
 * Copyright (C) 2002 MIPS Technologies, Inc.  All rights reserved.
 */
#include <linux/init.h>
#include <linux/sched.h>
#include <linux/smp.h>
#include <linux/mm.h>
#include <linux/hugetlb.h>
#include <linux/module.h>

#include <asm/cpu.h>
#include <asm/cpu-type.h>
#include <asm/bootinfo.h>
#include <asm/mmu_context.h>
#include <asm/pgtable.h>
#include <asm/tlb.h>
#include <asm/tlbmisc.h>

extern void build_tlb_refill_handler(void);

/* Atomicity and interruptability */
#ifdef CONFIG_MIPS_MT_SMTC

#include <asm/smtc.h>
#include <asm/mipsmtregs.h>

#define ENTER_CRITICAL(flags) \
	{ \
	unsigned int mvpflags; \
	local_irq_save(flags);\
	mvpflags = dvpe()
#define EXIT_CRITICAL(flags) \
	evpe(mvpflags); \
	local_irq_restore(flags); \
	}
#else

#define ENTER_CRITICAL(flags) local_irq_save(flags)
#define EXIT_CRITICAL(flags) local_irq_restore(flags)

#endif /* CONFIG_MIPS_MT_SMTC */

/*
 * LOONGSON-2 has a 4 entry itlb which is a subset of jtlb, LOONGSON-3 has
 * a 4 entry itlb and a 4 entry dtlb which are subsets of jtlb. Unfortunately,
 * itlb/dtlb are not totally transparent to software.
 */
static inline void flush_micro_tlb(void)
{
	change_c0_diag(0x4, 0x4);
}

static inline void flush_micro_tlb_vm(struct vm_area_struct *vma)
{
	if (vma->vm_flags & VM_EXEC)
		flush_micro_tlb();
}

static inline void local_flush_tlb_all_fast(void)
{
	change_c0_diag(0x300c, 0x300c);
}

#ifdef CONFIG_KVM_GUEST_LS3A3000
static noinline void emulate_tlb_ops(unsigned long address,
			    unsigned long pageshift, unsigned long even_pte,
			    unsigned long odd_pte, int op_type,
			    unsigned long flags)
{
	__asm__ __volatile__(
	"	.set	push			\n"
	"	.set	noreorder		\n"
	"	move	$2, %[A4]		\n"
	"	move	$3, %[A5]		\n"
	"	move	$4, %[A0]		\n"
	"	move	$5, %[A1]		\n"
	"	move	$6, %[A2]		\n"
	"	move	$7, %[A3]		\n"
	"	.word	0x42000028		\n" //hypcall
	"	nop				\n"
	"	.set	reorder			\n"
	"	.set	pop			\n"
	:
	: [A0] "r" (address), [A1] "r" (pageshift), [A2] "r" (even_pte),
	  [A3] "r" (odd_pte), [A4] "r" (op_type), [A5] "r" (flags)
	: "$2", "$3", "$4", "$5", "$6", "$7", "$8", "$9");
}

void local_flush_tlb_all(void)
{
	unsigned long flags;

	ENTER_CRITICAL(flags);

	emulate_tlb_ops(0, 1088, 0, 0, 0x5002, 2);
	EXIT_CRITICAL(flags);
}
EXPORT_SYMBOL(local_flush_tlb_all);

void local_flush_tlb_range(struct vm_area_struct *vma, unsigned long start,
	unsigned long end)
{
	struct mm_struct *mm = vma->vm_mm;
	int cpu = smp_processor_id();

	if (cpu_context(cpu, mm) != 0) {
		unsigned long size, flags;

		ENTER_CRITICAL(flags);
		start = round_down(start, PAGE_SIZE << 1);
		end = round_up(end, PAGE_SIZE << 1);
		size = (end - start) >> (PAGE_SHIFT + 1);
		if (size <= (current_cpu_data.tlbsizeftlbsets ?
			     current_cpu_data.tlbsize / 8 :
			     current_cpu_data.tlbsize / 2)) {
			int oldpid = read_c0_entryhi();
			int newpid = cpu_asid(cpu, mm);

			htw_stop();
			write_c0_entryhi(newpid);
			emulate_tlb_ops(start, end, size, 0, 0x5003, 2);
			tlbw_use_hazard();
			write_c0_entryhi(oldpid);
			htw_start();
		} else {
			drop_mmu_context(mm, cpu);
		}
		EXIT_CRITICAL(flags);
	}
}

void local_flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	unsigned long size, flags;

	ENTER_CRITICAL(flags);
	size = (end - start + (PAGE_SIZE - 1)) >> PAGE_SHIFT;
	size = (size + 1) >> 1;
	if (size <= (current_cpu_data.tlbsizeftlbsets ?
		     current_cpu_data.tlbsize / 8 :
		     current_cpu_data.tlbsize / 2)) {

		start &= (PAGE_MASK << 1);
		end += ((PAGE_SIZE << 1) - 1);
		end &= (PAGE_MASK << 1);
		htw_stop();

		emulate_tlb_ops(start, end, size, 0, 0x5004, 2);
		htw_start();
	} else {
		local_flush_tlb_all();
	}
	EXIT_CRITICAL(flags);
}

void local_flush_tlb_page(struct vm_area_struct *vma, unsigned long page)
{
	int cpu = smp_processor_id();

	if (cpu_context(cpu, vma->vm_mm) != 0) {
		unsigned long flags;
		int oldpid, newpid;

		newpid = cpu_asid(cpu, vma->vm_mm);
		ENTER_CRITICAL(flags);
		oldpid = read_c0_entryhi();
		htw_stop();
		write_c0_entryhi(newpid);

		emulate_tlb_ops(page, page, 0, 0, 0x5001, 2);
		write_c0_entryhi(oldpid);
		htw_start();
		flush_micro_tlb_vm(vma);
		EXIT_CRITICAL(flags);
	}
}

/*
 * This one is only used for pages with the global bit set so we don't care
 * much about the ASID.
 */
void local_flush_tlb_one(unsigned long page)
{
	unsigned long flags;
	int oldpid;

	ENTER_CRITICAL(flags);
	oldpid = read_c0_entryhi();
	htw_stop();
	emulate_tlb_ops(page, page, 0, 0, 0x5005, 2);
	write_c0_entryhi(oldpid);
	htw_start();
	EXIT_CRITICAL(flags);
}

/*
 * We will need multiple versions of update_mmu_cache(), one that just
 * updates the TLB with the new pte(s), and another which also checks
 * for the R4k "end of page" hardware bug and does the needy.
 */
void __update_tlb(struct vm_area_struct * vma, unsigned long address, pte_t pte)
{
	unsigned long flags;
	pgd_t *pgdp;
	pud_t *pudp;
	pmd_t *pmdp;
	pte_t *ptep;
	unsigned long pageshift,even_pte,odd_pte;
	unsigned long tmp_address;
	unsigned long vm_flags;

	/*
	 * Handle debugger faulting in for debugee.
	 */
	if (current->active_mm != vma->vm_mm)
		return;

	ENTER_CRITICAL(flags);

	htw_stop();

	tmp_address = address;
	address &= (PAGE_MASK << 1);
	pgdp = pgd_offset(vma->vm_mm, address);
	pudp = pud_offset(pgdp, address);
	pmdp = pmd_offset(pudp, address);

	ptep = pte_offset_map(pmdp, address);
	pageshift= 14;
	even_pte = pte_val(*ptep++);
	odd_pte = pte_val(*ptep);

	if((tmp_address >> PAGE_SHIFT) & 0x1)
		if(odd_pte & _PAGE_DIRTY)
			vm_flags = 3;
		else
			vm_flags = 2;
	else
		if(even_pte & _PAGE_DIRTY)
			vm_flags = 3;
		else
			vm_flags = 2;

	emulate_tlb_ops(tmp_address, pageshift, even_pte, odd_pte, 0x4000, vm_flags);
	htw_start();
	flush_micro_tlb_vm(vma);
	EXIT_CRITICAL(flags);
}
#else
void local_flush_tlb_all(void)
{
	unsigned long flags;
	int entry;
	unsigned long old_ctx;
	int ftlbhighset;


	ENTER_CRITICAL(flags);

	/* Blast 'em all away. */
	/* Save old context and create impossible VPN2 value */
	htw_stop();
	entry = read_c0_wired();
	if (!entry) {
		if (cpu_has_tlbinv)  {
#ifndef CONFIG_CPU_LOONGSON3
			if (current_cpu_data.tlbsizevtlb) {
				write_c0_index(0);
				mtc0_tlbw_hazard();
				tlbinvf();  /* invalidate VTLB */
			}
			ftlbhighset = current_cpu_data.tlbsizevtlb +
				current_cpu_data.tlbsizeftlbsets;
			for (entry = current_cpu_data.tlbsizevtlb;
				entry < ftlbhighset; entry++) {
				write_c0_index(entry);
				mtc0_tlbw_hazard();
				tlbinvf();  /* invalidate one FTLB set */
			}

#else			 /* here is optimization for 3A2000+ */

			local_flush_tlb_all_fast();
			EXIT_CRITICAL(flags);
			return;
#endif
		} else {
			old_ctx = read_c0_entryhi();
			write_c0_entrylo0(0);
			write_c0_entrylo1(0);
			while (entry < current_cpu_data.tlbsize) {
				/* Make sure all entries differ. */
				write_c0_entryhi(UNIQUE_ENTRYHI(entry));
				write_c0_index(entry);
				mtc0_tlbw_hazard();
				tlb_write_indexed();
				entry++;
			}
			tlbw_use_hazard();
			write_c0_entryhi(old_ctx);
		}
	} else {
		old_ctx = read_c0_entryhi();
		write_c0_entrylo0(0);
		write_c0_entrylo1(0);
		while (entry < current_cpu_data.tlbsizevtlb) {
			/* Make sure all entries differ. */
			write_c0_entryhi(UNIQUE_ENTRYHI(entry));
			write_c0_index(entry);
			mtc0_tlbw_hazard();
			tlb_write_indexed();
			entry++;
		}

		if (cpu_has_tlbinv) {
			ftlbhighset = current_cpu_data.tlbsizevtlb +
				current_cpu_data.tlbsizeftlbsets;
			for (entry = current_cpu_data.tlbsizevtlb;
				entry < ftlbhighset; entry++) {
				write_c0_index(entry);
				mtc0_tlbw_hazard();
				tlbinvf();  /* invalidate one FTLB set */
			}

		} else {

			while (entry < current_cpu_data.tlbsize) {
				/* Make sure all entries differ. */
				write_c0_entryhi(UNIQUE_ENTRYHI(entry));
				write_c0_index(entry);
				mtc0_tlbw_hazard();
				tlb_write_indexed();
				entry++;
			}

		}
		tlbw_use_hazard();
		write_c0_entryhi(old_ctx);

	}
	flush_micro_tlb();
	htw_start();
	EXIT_CRITICAL(flags);
}
EXPORT_SYMBOL(local_flush_tlb_all);

void local_flush_tlb_range(struct vm_area_struct *vma, unsigned long start,
	unsigned long end)
{
	struct mm_struct *mm = vma->vm_mm;
	int cpu = smp_processor_id();

	if (cpu_context(cpu, mm) != 0) {
		unsigned long size, flags;

		ENTER_CRITICAL(flags);
		start = round_down(start, PAGE_SIZE << 1);
		end = round_up(end, PAGE_SIZE << 1);
		size = (end - start) >> (PAGE_SHIFT + 1);
		if (size <= (current_cpu_data.tlbsizeftlbsets ?
			     current_cpu_data.tlbsize / 8 :
			     current_cpu_data.tlbsize / 2)) {
			int oldpid = read_c0_entryhi();
			int newpid = cpu_asid(cpu, mm);

			htw_stop();

			while (start < end) {
				int idx;

				write_c0_entryhi(start | newpid);
				start += (PAGE_SIZE << 1);
				mtc0_tlbw_hazard();
				tlb_probe();
				tlb_probe_hazard();
				idx = read_c0_index();
				write_c0_entrylo0(0);
				write_c0_entrylo1(0);
				if (idx < 0)
					continue;
				/* Make sure all entries differ. */
				write_c0_entryhi(UNIQUE_ENTRYHI(idx));
				mtc0_tlbw_hazard();
				tlb_write_indexed();
			}
			tlbw_use_hazard();
			write_c0_entryhi(oldpid);
			htw_start();
		} else {
			drop_mmu_context(mm, cpu);
		}
		flush_micro_tlb();
		EXIT_CRITICAL(flags);
	}
}

void local_flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	unsigned long size, flags;

	ENTER_CRITICAL(flags);
	size = (end - start + (PAGE_SIZE - 1)) >> PAGE_SHIFT;
	size = (size + 1) >> 1;
	if (size <= (current_cpu_data.tlbsizeftlbsets ?
		     current_cpu_data.tlbsize / 8 :
		     current_cpu_data.tlbsize / 2)) {
		int pid = read_c0_entryhi();

		start &= (PAGE_MASK << 1);
		end += ((PAGE_SIZE << 1) - 1);
		end &= (PAGE_MASK << 1);
		htw_stop();

		while (start < end) {
			int idx;

			write_c0_entryhi(start);
			start += (PAGE_SIZE << 1);
			mtc0_tlbw_hazard();
			tlb_probe();
			tlb_probe_hazard();
			idx = read_c0_index();
			write_c0_entrylo0(0);
			write_c0_entrylo1(0);
			if (idx < 0)
				continue;
			/* Make sure all entries differ. */
			write_c0_entryhi(UNIQUE_ENTRYHI(idx));
			mtc0_tlbw_hazard();
			tlb_write_indexed();
		}
		tlbw_use_hazard();
		write_c0_entryhi(pid);
		htw_start();
	} else {
		local_flush_tlb_all();
	}
	flush_micro_tlb();
	EXIT_CRITICAL(flags);
}

void local_flush_tlb_page(struct vm_area_struct *vma, unsigned long page)
{
	int cpu = smp_processor_id();

	if (cpu_context(cpu, vma->vm_mm) != 0) {
		unsigned long flags;
		int oldpid, newpid, idx;

		newpid = cpu_asid(cpu, vma->vm_mm);
		page &= (PAGE_MASK << 1);
		ENTER_CRITICAL(flags);
		oldpid = read_c0_entryhi();
		htw_stop();
		write_c0_entryhi(page | newpid);
		mtc0_tlbw_hazard();
		tlb_probe();
		tlb_probe_hazard();
		idx = read_c0_index();
		write_c0_entrylo0(0);
		write_c0_entrylo1(0);
		if (idx < 0)
			goto finish;
		/* Make sure all entries differ. */
		write_c0_entryhi(UNIQUE_ENTRYHI(idx));
		mtc0_tlbw_hazard();
		tlb_write_indexed();
		tlbw_use_hazard();

	finish:
		write_c0_entryhi(oldpid);
		htw_start();
		flush_micro_tlb_vm(vma);
		EXIT_CRITICAL(flags);
	}
}

/*
 * This one is only used for pages with the global bit set so we don't care
 * much about the ASID.
 */
void local_flush_tlb_one(unsigned long page)
{
	unsigned long flags;
	int oldpid, idx;

	ENTER_CRITICAL(flags);
	oldpid = read_c0_entryhi();
	htw_stop();
	page &= (PAGE_MASK << 1);
	write_c0_entryhi(page);
	mtc0_tlbw_hazard();
	tlb_probe();
	tlb_probe_hazard();
	idx = read_c0_index();
	write_c0_entrylo0(0);
	write_c0_entrylo1(0);
	if (idx >= 0) {
		/* Make sure all entries differ. */
		write_c0_entryhi(UNIQUE_ENTRYHI(idx));
		mtc0_tlbw_hazard();
		tlb_write_indexed();
		tlbw_use_hazard();
	}
	write_c0_entryhi(oldpid);
	htw_start();
	flush_micro_tlb();
	EXIT_CRITICAL(flags);
}

/*
 * We will need multiple versions of update_mmu_cache(), one that just
 * updates the TLB with the new pte(s), and another which also checks
 * for the R4k "end of page" hardware bug and does the needy.
 */
void __update_tlb(struct vm_area_struct * vma, unsigned long address, pte_t pte)
{
	unsigned long flags;
	pgd_t *pgdp;
	pud_t *pudp;
	pmd_t *pmdp;
	pte_t *ptep;
	int idx, pid;

	/*
	 * Handle debugger faulting in for debugee.
	 */
	if (current->active_mm != vma->vm_mm)
		return;

	ENTER_CRITICAL(flags);

	htw_stop();

	pid = read_c0_entryhi() & cpu_asid_mask(&current_cpu_data);
	address &= (PAGE_MASK << 1);
	write_c0_entryhi(address | pid);
	pgdp = pgd_offset(vma->vm_mm, address);
	mtc0_tlbw_hazard();
	tlb_probe();
	tlb_probe_hazard();
	pudp = pud_offset(pgdp, address);
	pmdp = pmd_offset(pudp, address);
	idx = read_c0_index();
#ifdef CONFIG_MIPS_HUGE_TLB_SUPPORT
	/* this could be a huge page  */
	if (pmd_huge(*pmdp)) {
		unsigned long lo;
		write_c0_pagemask(PM_HUGE_MASK);
		ptep = (pte_t *)pmdp;
		lo = pte_to_entrylo(pte_val(*ptep));
		write_c0_entrylo0(lo);
		write_c0_entrylo1(lo + (HPAGE_SIZE >> 7));

		mtc0_tlbw_hazard();
		if (idx < 0)
			tlb_write_random();
		else
			tlb_write_indexed();
		tlbw_use_hazard();
		write_c0_pagemask(PM_DEFAULT_MASK);
	} else
#endif
	{
		ptep = pte_offset_map(pmdp, address);

#if defined(CONFIG_64BIT_PHYS_ADDR) && defined(CONFIG_CPU_MIPS32)
		write_c0_entrylo0(ptep->pte_high);
		ptep++;
		write_c0_entrylo1(ptep->pte_high);
#else
		write_c0_entrylo0(pte_to_entrylo(pte_val(*ptep++)));
		write_c0_entrylo1(pte_to_entrylo(pte_val(*ptep)));
#endif
		mtc0_tlbw_hazard();
		if (idx < 0)
			tlb_write_random();
		else
			tlb_write_indexed();
	}
	tlbw_use_hazard();
	htw_start();
	flush_micro_tlb_vm(vma);
	EXIT_CRITICAL(flags);
}
#endif

static void local_flush_tlb_mm_slow(unsigned long asid)
{
	unsigned long flags;
	int ftlbhighset;
	int entry;
	int old_ehi;

	ENTER_CRITICAL(flags);

	entry = read_c0_wired();
	old_ehi = read_c0_entryhi();


	if (!entry) {
		write_c0_entryhi(asid);
		write_c0_index(0);
		tlbinv();
	} else {
		write_c0_entrylo0(0);
		write_c0_entrylo1(0);
		while (entry < boot_cpu_data.tlbsizevtlb) {
			/* invalidate unwired VTLB */
			write_c0_index(entry);
			write_c0_entryhi(UNIQUE_ENTRYHI_ASID(entry, asid));
			tlb_write_indexed();
			entry++;
		}

	}

	ftlbhighset = boot_cpu_data.tlbsizevtlb +
		boot_cpu_data.tlbsizeftlbsets;
	entry = boot_cpu_data.tlbsizevtlb;

	while (entry < ftlbhighset) {
		write_c0_index(entry);
		tlbinv();  /* invalidate one FTLB set */
		entry++;
	}

	write_c0_entryhi(old_ehi);
	EXIT_CRITICAL(flags);
}
/* All entries common to a mm share an asid.  To effectively flush
   these entries, we just bump the asid. */
void local_flush_tlb_mm(struct mm_struct *mm)
{
	int cpu;
	unsigned long asid;

	preempt_disable();

	cpu = smp_processor_id();

	if ((asid = cpu_context(cpu, mm)) != 0) {
		if (cpu_data[cpu].cputype == CPU_LOONGSON3_COMP)
			local_flush_tlb_mm_slow(asid & cpu_asid_mask(&cpu_data[cpu]));
		else
			drop_mmu_context(mm, cpu);

	}

	preempt_enable();
}

void add_wired_entry(unsigned long entrylo0, unsigned long entrylo1,
		     unsigned long entryhi, unsigned long pagemask)
{
	unsigned long flags;
	unsigned long wired;
	unsigned long old_pagemask;
	unsigned long old_ctx;

	ENTER_CRITICAL(flags);
	/* Save old context and create impossible VPN2 value */
	old_ctx = read_c0_entryhi();
	htw_stop();
	old_pagemask = read_c0_pagemask();
	wired = read_c0_wired();
	write_c0_wired(wired + 1);
	write_c0_index(wired);
	tlbw_use_hazard();	/* What is the hazard here? */
	write_c0_pagemask(pagemask);
	write_c0_entryhi(entryhi);
	write_c0_entrylo0(entrylo0);
	write_c0_entrylo1(entrylo1);
	mtc0_tlbw_hazard();
	tlb_write_indexed();
	tlbw_use_hazard();

	write_c0_entryhi(old_ctx);
	tlbw_use_hazard();	/* What is the hazard here? */
	htw_start();
	write_c0_pagemask(old_pagemask);
	local_flush_tlb_all();
	EXIT_CRITICAL(flags);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE

int __init has_transparent_hugepage(void)
{
	unsigned int mask;
	unsigned long flags;

	ENTER_CRITICAL(flags);
	write_c0_pagemask(PM_HUGE_MASK);
	back_to_back_c0_hazard();
	mask = read_c0_pagemask();
	write_c0_pagemask(PM_DEFAULT_MASK);

	EXIT_CRITICAL(flags);

	return mask == PM_HUGE_MASK;
}

#endif /* CONFIG_TRANSPARENT_HUGEPAGE  */

static int  ntlb;
static int __init set_ntlb(char *str)
{
	get_option(&str, &ntlb);
	return 1;
}

__setup("ntlb=", set_ntlb);

void  tlb_init(void)
{
	/*
	 * You should never change this register:
	 *   - On R4600 1.7 the tlbp never hits for pages smaller than
	 *     the value in the c0_pagemask register.
	 *   - The entire mm handling assumes the c0_pagemask register to
	 *     be set to fixed-size pages.
	 */
	write_c0_pagemask(PM_DEFAULT_MASK);
	write_c0_wired(0);
	if (current_cpu_type() == CPU_R10000 ||
	    current_cpu_type() == CPU_R12000 ||
	    current_cpu_type() == CPU_R14000)
		write_c0_framemask(0);

	if (cpu_has_rixi) {
		/*
		 * Enable the no read, no exec bits, and enable large virtual
		 * address.
		 */
#ifdef CONFIG_64BIT
		set_c0_pagegrain(PG_RIE | PG_XIE | PG_ELPA);
#else
		set_c0_pagegrain(PG_RIE | PG_XIE);
#endif
	}

	/* From this point on the ARC firmware is dead.	 */
	local_flush_tlb_all();

	/* Did I tell you that ARC SUCKS?  */

	if (ntlb) {
		if (ntlb > 1 && ntlb <= current_cpu_data.tlbsize) {
			int wired = current_cpu_data.tlbsize - ntlb;
			write_c0_wired(wired);
			write_c0_index(wired-1);
			printk("Restricting TLB to %d entries\n", ntlb);
		} else
			printk("Ignoring invalid argument ntlb=%d\n", ntlb);
	}

	build_tlb_refill_handler();
}
