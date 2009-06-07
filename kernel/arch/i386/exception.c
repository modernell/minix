/* This file contains a simple exception handler.  Exceptions in user
 * processes are converted to signals. Exceptions in a kernel task cause
 * a panic.
 */

#include "../../kernel.h"
#include "proto.h"
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <minix/sysutil.h>
#include "../../proc.h"
#include "../../proto.h"
#include "../../vm.h"

extern int vm_copy_in_progress, catch_pagefaults;
extern struct proc *vm_copy_from, *vm_copy_to;
extern u32_t npagefaults;

PUBLIC u32_t pagefault_count = 0;

void pagefault(vir_bytes old_eip, struct proc *pr, int trap_errno,
	u32_t *old_eipptr, u32_t *old_eaxptr, u32_t pagefaultcr2)
{
	int s;
	vir_bytes ph;
	u32_t pte;
	int procok = 0, pcok = 0, rangeok = 0;
	int in_physcopy = 0;
	vir_bytes test_eip;

	vmassert(old_eipptr);
	vmassert(old_eaxptr);

	vmassert(*old_eipptr == old_eip);
	vmassert(old_eipptr != &old_eip);

	vmassert(pagefault_count == 1);

#if 0
	printf("kernel: pagefault in pr %d, addr 0x%lx, his cr3 0x%lx, actual cr3 0x%lx\n",
		pr->p_endpoint, pagefaultcr2, pr->p_seg.p_cr3, read_cr3());
#endif

	if(pr->p_seg.p_cr3) {
#if 0
		vm_print(pr->p_seg.p_cr3);
#endif
		vmassert(pr->p_seg.p_cr3 == read_cr3());
	} else {
		vmassert(ptproc);
		vmassert(ptproc->p_seg.p_cr3 == read_cr3());
	}

	test_eip = k_reenter ? old_eip : pr->p_reg.pc;

	in_physcopy = (test_eip > (vir_bytes) phys_copy) &&
	   (test_eip < (vir_bytes) phys_copy_fault);

	if((k_reenter || iskernelp(pr)) &&
		catch_pagefaults && in_physcopy) {
#if 0
		printf("pf caught! addr 0x%lx\n", pagefaultcr2);
#endif
		*old_eipptr = phys_copy_fault;
		*old_eaxptr = pagefaultcr2;

		pagefault_count = 0;

		return;
	}

	npagefaults++;

	/* System processes that don't have their own page table can't
	 * have page faults. VM does have its own page table but also
	 * can't have page faults (because VM has to handle them).
	 */
	if(k_reenter || (pr->p_endpoint <= INIT_PROC_NR &&
	 !(pr->p_misc_flags & MF_FULLVM)) || pr->p_endpoint == VM_PROC_NR) {
		/* Page fault we can't / don't want to
		 * handle.
		 */
		kprintf("pagefault for process %d ('%s'), pc = 0x%x, addr = 0x%x, flags = 0x%x, k_reenter %d\n",
			pr->p_endpoint, pr->p_name, pr->p_reg.pc,
			pagefaultcr2, trap_errno, k_reenter);
		proc_stacktrace(pr);
  		minix_panic("page fault in system process", pr->p_endpoint);

		return;
	}

	/* Don't schedule this process until pagefault is handled. */
	vmassert(pr->p_seg.p_cr3 == read_cr3());
	vmassert(!RTS_ISSET(pr, PAGEFAULT));
	RTS_LOCK_SET(pr, PAGEFAULT);

	/* Save pagefault details, suspend process,
	 * add process to pagefault chain,
	 * and tell VM there is a pagefault to be
	 * handled.
	 */
	pr->p_pagefault.pf_virtual = pagefaultcr2;
	pr->p_pagefault.pf_flags = trap_errno;
	pr->p_nextpagefault = pagefaults;
	pagefaults = pr;
		
	lock_notify(HARDWARE, VM_PROC_NR);

	pagefault_count = 0;

	return;
}

/*===========================================================================*
 *				exception				     *
 *===========================================================================*/
PUBLIC void exception(vec_nr, trap_errno, old_eip, old_cs, old_eflags,
	old_eipptr, old_eaxptr, pagefaultcr2)
unsigned vec_nr;
u32_t trap_errno;
u32_t old_eip;
U16_t old_cs;
u32_t old_eflags;
u32_t *old_eipptr;
u32_t *old_eaxptr;
u32_t pagefaultcr2;
{
/* An exception or unexpected interrupt has occurred. */

struct proc *t;

  struct ex_s {
	char *msg;
	int signum;
	int minprocessor;
  };
  static struct ex_s ex_data[] = {
	{ "Divide error", SIGFPE, 86 },
	{ "Debug exception", SIGTRAP, 86 },
	{ "Nonmaskable interrupt", SIGBUS, 86 },
	{ "Breakpoint", SIGEMT, 86 },
	{ "Overflow", SIGFPE, 86 },
	{ "Bounds check", SIGFPE, 186 },
	{ "Invalid opcode", SIGILL, 186 },
	{ "Coprocessor not available", SIGFPE, 186 },
	{ "Double fault", SIGBUS, 286 },
	{ "Copressor segment overrun", SIGSEGV, 286 },
	{ "Invalid TSS", SIGSEGV, 286 },
	{ "Segment not present", SIGSEGV, 286 },
	{ "Stack exception", SIGSEGV, 286 },	/* STACK_FAULT already used */
	{ "General protection", SIGSEGV, 286 },
	{ "Page fault", SIGSEGV, 386 },		/* not close */
	{ NIL_PTR, SIGILL, 0 },			/* probably software trap */
	{ "Coprocessor error", SIGFPE, 386 },
  };
  register struct ex_s *ep;
  struct proc *saved_proc;

  /* Save proc_ptr, because it may be changed by debug statements. */
  saved_proc = proc_ptr;	
  
  ep = &ex_data[vec_nr];

  if (vec_nr == 2) {		/* spurious NMI on some machines */
	kprintf("got spurious NMI\n");
	return;
  }

  if(vec_nr == PAGE_FAULT_VECTOR) {
	pagefault(old_eip, saved_proc, trap_errno,
		old_eipptr, old_eaxptr, pagefaultcr2);
	return;
  }

  /* If an exception occurs while running a process, the k_reenter variable 
   * will be zero. Exceptions in interrupt handlers or system traps will make 
   * k_reenter larger than zero.
   */
  if (k_reenter == 0 && ! iskernelp(saved_proc)) {
	{

		kprintf(
"exception for process %d, endpoint %d ('%s'), pc = 0x%x:0x%x, sp = 0x%x:0x%x\n",
			proc_nr(saved_proc), saved_proc->p_endpoint,
			saved_proc->p_name,
			saved_proc->p_reg.cs, saved_proc->p_reg.pc,
			saved_proc->p_reg.ss, saved_proc->p_reg.sp);
  		kprintf(
  "vec_nr= %d, trap_errno= 0x%lx, eip= 0x%lx, cs= 0x%x, eflags= 0x%lx\n",
			vec_nr, (unsigned long)trap_errno,
			(unsigned long)old_eip, old_cs,
			(unsigned long)old_eflags);
		proc_stacktrace(saved_proc);
	}

	kprintf("kernel: cause_sig %d for %d\n",
		ep->signum, saved_proc->p_endpoint);
	cause_sig(proc_nr(saved_proc), ep->signum);
	return;
  }

  /* Exception in system code. This is not supposed to happen. */
  if (ep->msg == NIL_PTR || machine.processor < ep->minprocessor)
	kprintf("\nIntel-reserved exception %d\n", vec_nr);
  else
	kprintf("\n%s\n", ep->msg);
  kprintf("k_reenter = %d ", k_reenter);
  kprintf("process %d (%s), ", proc_nr(saved_proc), saved_proc->p_name);
  kprintf("pc = %u:0x%x\n", (unsigned) saved_proc->p_reg.cs,
	  (unsigned) saved_proc->p_reg.pc);
  kprintf(
  "vec_nr= %d, trap_errno= 0x%lx, eip= 0x%lx, cs= 0x%x, eflags= 0x%lx\n",
	vec_nr, (unsigned long)trap_errno,
	(unsigned long)old_eip, old_cs, (unsigned long)old_eflags);
  proc_stacktrace(saved_proc);

  minix_panic("exception in a kernel task", saved_proc->p_endpoint);
}

/*===========================================================================*
 *				stacktrace				     *
 *===========================================================================*/
PUBLIC void proc_stacktrace(struct proc *proc)
{
	reg_t bp, v_bp, v_pc, v_hbp;

	v_bp = proc->p_reg.fp;

	kprintf("%-8.8s %6d 0x%lx ",
		proc->p_name, proc->p_endpoint, proc->p_reg.pc);

	while(v_bp) {

#define PRCOPY(pr, pv, v, n) \
  (iskernelp(pr) ? (memcpy((char *) v, (char *) pv, n), OK) : \
     data_copy(pr->p_endpoint, pv, SYSTEM, (vir_bytes) (v), n))

	        if(PRCOPY(proc, v_bp, &v_hbp, sizeof(v_hbp)) != OK) {
			kprintf("(v_bp 0x%lx ?)", v_bp);
			break;
		}
		if(PRCOPY(proc, v_bp + sizeof(v_pc), &v_pc, sizeof(v_pc)) != OK) {
			kprintf("(v_pc 0x%lx ?)", v_bp + sizeof(v_pc));
			break;
		}
		kprintf("0x%lx ", (unsigned long) v_pc);
		if(v_hbp != 0 && v_hbp <= v_bp) {
			kprintf("(hbp %lx ?)", v_hbp);
			break;
		}
		v_bp = v_hbp;
	}
	kprintf("\n");
}
