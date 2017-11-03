/*
 * Copyright (C) 2015 Mihai Carabas <mihai.carabas@gmail.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL AUTHOR OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <sys/cdefs.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/kernel.h>
#include <sys/module.h>
#include <sys/sysctl.h>
#include <sys/malloc.h>
#include <sys/pcpu.h>
#include <sys/lock.h>
#include <sys/mutex.h>
#include <sys/proc.h>
#include <sys/rwlock.h>
#include <sys/sched.h>
#include <sys/smp.h>
#include <sys/cpuset.h>

#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>
#include <vm/pmap.h>
#include <vm/vm_map.h>
#include <vm/vm_extern.h>
#include <vm/vm_param.h>

#include <machine/cpu.h>
#include <machine/vm.h>
#include <machine/pcb.h>
#include <machine/param.h>
#include <machine/smp.h>
#include <machine/vmparam.h>
#include <machine/vmm.h>
#include <machine/vmm_dev.h>

#include "vmm_stat.h"
#include "vmm_mem.h"
#include "mmu.h"
#include "arm64.h"
#include "vgic.h"

#define	BSP	0			/* the boostrap processor */

struct vcpu {
	int		flags;
	enum vcpu_state	state;
	struct mtx	mtx;
	int		hostcpu;	/* host cpuid this vcpu last ran on */
	int		vcpuid;
	void		*stats;
	struct vm_exit	exitinfo;
	uint64_t	nextpc;		/* (x) next instruction to execute */
};

#define	vcpu_lock_initialized(v) mtx_initialized(&((v)->mtx))
#define	vcpu_lock_init(v)	mtx_init(&((v)->mtx), "vcpu lock", 0, MTX_SPIN)
#define	vcpu_lock(v)		mtx_lock_spin(&((v)->mtx))
#define	vcpu_unlock(v)		mtx_unlock_spin(&((v)->mtx))
#define	vcpu_assert_locked(v)	mtx_assert(&((v)->mtx), MA_OWNED)

struct mem_seg {
	uint64_t	gpa;
	size_t		len;
	bool		wired;
	vm_object_t	object;
};
#define	VM_MAX_MEMORY_SEGMENTS	2

struct vm {
	void		*cookie;
	struct vcpu	vcpu[VM_MAXCPU];
	int		num_mem_segs;
	struct vm_memory_segment mem_segs[VM_MAX_MEMORY_SEGMENTS];
	char		name[VM_MAX_NAMELEN];
	/*
	 * Set of active vcpus.
	 * An active vcpu is one that has been started implicitly (BSP) or
	 * explicitly (AP) by sending it a startup ipi.
	 */
	cpuset_t	active_cpus;
};

extern uint64_t hypmode_enabled;

static bool vmm_initialized = false;

static struct vmm_ops *ops = NULL;

#define	VMM_INIT(num)	(ops != NULL ? (*ops->init)(num) : 0)
#define	VMM_CLEANUP()	(ops != NULL ? (*ops->cleanup)() : 0)

#define	VMINIT(vm) (ops != NULL ? (*ops->vminit)(vm): NULL)
#define	VMRUN(vmi, vcpu, pc, pmap, rvc, sc) \
	(ops != NULL ? (*ops->vmrun)(vmi, vcpu, pc, pmap, rvc, sc) : ENXIO)
#define	VMCLEANUP(vmi)	(ops != NULL ? (*ops->vmcleanup)(vmi) : NULL)
#define	VMMMAP_SET(vmi, ipa, pa, len, prot)				\
    	(ops != NULL ? 							\
    	(*ops->vmmapset)(vmi, ipa, pa, len, prot) : ENXIO)
#define	VMMMAP_GET(vmi, gpa) \
	(ops != NULL ? (*ops->vmmapget)(vmi, gpa) : ENXIO)
#define	VMGETREG(vmi, vcpu, num, retval)		\
	(ops != NULL ? (*ops->vmgetreg)(vmi, vcpu, num, retval) : ENXIO)
#define	VMSETREG(vmi, vcpu, num, val)		\
	(ops != NULL ? (*ops->vmsetreg)(vmi, vcpu, num, val) : ENXIO)
#define	VMGETCAP(vmi, vcpu, num, retval)	\
	(ops != NULL ? (*ops->vmgetcap)(vmi, vcpu, num, retval) : ENXIO)
#define	VMSETCAP(vmi, vcpu, num, val)		\
	(ops != NULL ? (*ops->vmsetcap)(vmi, vcpu, num, val) : ENXIO)

#define	fpu_start_emulating()	load_cr0(rcr0() | CR0_TS)
#define	fpu_stop_emulating()	clts()

static int vm_handle_wfi(struct vm *vm, int vcpuid,
			 struct vm_exit *vme, bool *retu);

static MALLOC_DEFINE(M_VMM, "vmm", "vmm");

/* statistics */
static VMM_STAT(VCPU_TOTAL_RUNTIME, "vcpu total runtime");

SYSCTL_NODE(_hw, OID_AUTO, vmm, CTLFLAG_RW, NULL, NULL);

/*
 * Halt the guest if all vcpus are executing a HLT instruction with
 * interrupts disabled.
 */
static int halt_detection_enabled = 1;
SYSCTL_INT(_hw_vmm, OID_AUTO, halt_detection, CTLFLAG_RDTUN,
    &halt_detection_enabled, 0,
    "Halt VM if all vcpus execute HLT with interrupts disabled");

static int vmm_ipinum;
SYSCTL_INT(_hw_vmm, OID_AUTO, ipinum, CTLFLAG_RD, &vmm_ipinum, 0,
    "IPI vector used for vcpu notifications");

static int trace_guest_exceptions;
SYSCTL_INT(_hw_vmm, OID_AUTO, trace_guest_exceptions, CTLFLAG_RDTUN,
    &trace_guest_exceptions, 0,
    "Trap into hypervisor on all guest exceptions and reflect them back");

static void
vcpu_cleanup(struct vm *vm, int i, bool destroy)
{
//	struct vcpu *vcpu = &vm->vcpu[i];
}

static void
vcpu_init(struct vm *vm, uint32_t vcpu_id)
{
	struct vcpu *vcpu;

	vcpu = &vm->vcpu[vcpu_id];

	vcpu_lock_init(vcpu);
	vcpu->hostcpu = NOCPU;
	vcpu->vcpuid = vcpu_id;
}

struct vm_exit *
vm_exitinfo(struct vm *vm, int cpuid)
{
	struct vcpu *vcpu;

	if (cpuid < 0 || cpuid >= VM_MAXCPU)
		panic("vm_exitinfo: invalid cpuid %d", cpuid);

	vcpu = &vm->vcpu[cpuid];

	return (&vcpu->exitinfo);
}

static int
vmm_init(void)
{
	ops = &vmm_ops_arm;

	return (VMM_INIT(0));
}

static int
vmm_handler(module_t mod, int what, void *arg)
{
	int error;

	switch (what) {
	case MOD_LOAD:

		printf("VMM_HANDLER:\n");
		printf("\thypmode_enabled = %lu\n", hypmode_enabled);

		vmmdev_init();
		error = vmm_init();
		if (error == 0)
			vmm_initialized = true;
		break;
	case MOD_UNLOAD:
		error = vmmdev_cleanup();
		if (error == 0 && vmm_initialized) {
			error = VMM_CLEANUP();
			if (error)
				vmm_initialized = false;
		}
		break;
	default:
		error = 0;
		break;
	}
	return (error);
}

static moduledata_t vmm_kmod = {
	"vmm",
	vmm_handler,
	NULL
};

/*
 * vmm initialization has the following dependencies:
 *
 * - HYP initialization requires smp_rendezvous() and therefore must happen
 *   after SMP is fully functional (after SI_SUB_SMP).
 */
DECLARE_MODULE(vmm, vmm_kmod, SI_SUB_SMP + 1, SI_ORDER_ANY);
MODULE_VERSION(vmm, 1);

int
vm_create(const char *name, struct vm **retvm)
{
	struct vm *vm;
	int i;

	/*
	 * If vmm.ko could not be successfully initialized then don't attempt
	 * to create the virtual machine.
	 */
	if (!vmm_initialized)
		return (ENXIO);

	if (name == NULL || strlen(name) >= VM_MAX_NAMELEN)
		return (EINVAL);

	vm = malloc(sizeof(struct vm), M_VMM, M_WAITOK | M_ZERO);
	strcpy(vm->name, name);
	vm->cookie = VMINIT(vm);

#if 0
	/* TEMP - PL804 timer mapping */
	VMMMAP_SET(vm->cookie, 0x1c110000, 0x1c110000, PAGE_SIZE, VM_PROT_ALL);
#endif

	for (i = 0; i < VM_MAXCPU; i++)
		vcpu_init(vm, i);

	vm_activate_cpu(vm, BSP);

	*retvm = vm;
	return (0);
}

static void
vm_cleanup(struct vm *vm, bool destroy)
{
	VMCLEANUP(vm->cookie);
}

void
vm_destroy(struct vm *vm)
{
	vm_cleanup(vm, true);
	free(vm, M_VMM);
}

const char *
vm_name(struct vm *vm)
{
	return (vm->name);
}

#include <sys/queue.h>
#include <sys/linker.h>

static caddr_t
search_by_type(const char *type, caddr_t preload_metadata)
{
    caddr_t	curp, lname;
    uint32_t	*hdr;
    int		next;

    if (preload_metadata != NULL) {

	curp = preload_metadata;
	lname = NULL;
	for (;;) {
	    hdr = (uint32_t *)curp;
	    if (hdr[0] == 0 && hdr[1] == 0)
		break;

	    /* remember the start of each record */
	    if (hdr[0] == MODINFO_NAME)
		lname = curp;

	    /* Search for a MODINFO_TYPE field */
	    if ((hdr[0] == MODINFO_TYPE) &&
		!strcmp(type, curp + sizeof(uint32_t) * 2))
		return(lname);

	    /* skip to next field */
	    next = sizeof(uint32_t) * 2 + hdr[1];
	    next = roundup(next, sizeof(u_long));
	    curp += next;
	}
    }
    return(NULL);
}

int
vm_run(struct vm *vm, struct vm_run *vmrun)
{
	int error, vcpuid;
	register_t pc;
	struct vcpu *vcpu;
	struct vm_exit *vme;
	bool retu;
	void *rvc, *sc;

	vcpuid = vmrun->cpuid;
	pc = vmrun->pc;

	if (vcpuid < 0 || vcpuid >= VM_MAXCPU)
		return (EINVAL);

	if (!CPU_ISSET(vcpuid, &vm->active_cpus))
		return (EINVAL);

	vcpu = &vm->vcpu[vcpuid];
	vme = &vcpu->exitinfo;

	vm_paddr_t pa = hypmap_get(vm->cookie, VM_GUEST_BASE_IPA + 0x1000);
	uint32_t *instr = (uint32_t *)PHYS_TO_DMAP(pa);
	printf("first instruction:\n");
       	printf("0x%08x\n", *instr);
	printf("\n");

	pa = hypmap_get(vm->cookie, VM_GUEST_BASE_IPA + 0xe89000 + 0x3fd000);
	char  *envp = (char *)PHYS_TO_DMAP(pa);
	printf("env:\n");
	while (true) {
		printf("%d ", *envp);
		if (*envp == 0 && *(envp + 1) == 0) {
			printf("%d ", *envp);
			break;
		}
		envp++;
	}
	printf("\n\n");

	pa = hypmap_get(vm->cookie, VM_GUEST_BASE_IPA + 0xe8a000 + 0x3fd000);
	char  *preload_metadata = (char *)PHYS_TO_DMAP(pa);
	printf("preload_search_by_type(\"elf kernel\") = 0x%016lx\n",
			(uint64_t)search_by_type("elf kernel", preload_metadata));
	printf("\n");


	rvc = sc = NULL;
restart:
	critical_enter();
	error = VMRUN(vm->cookie, vcpuid, pc, NULL, rvc, sc);
	critical_exit();

	VMCLEANUP(vm->cookie);
	panic("\n\n\ncleanup successful!\n\n");

	if (error == 0) {
		switch (vme->exitcode) {
		case VM_EXITCODE_INST_EMUL:
			/* Check if we need to do in-kernel emulation */
			pc = vme->pc + vme->inst_length;
			retu = true;
			error = vgic_emulate_distributor(vm->cookie, vcpuid, vme, &retu);
			break;

		case VM_EXITCODE_WFI:
			pc = vme->pc + vme->inst_length;
			retu = true;
			/* vm_handle_wfi always sets retu to false... */
			error = vm_handle_wfi(vm, vcpuid, vme, &retu);
			break;

		default:
			retu = true;	/* handled in userland */
			break;
		}
	}

	if (error == 0 && retu == false)
		goto restart;

	/* copy the exit information */
	bcopy(vme, &vmrun->vm_exit, sizeof(struct vm_exit));

	return (error);
}

int
vm_activate_cpu(struct vm *vm, int vcpuid)
{

	if (vcpuid < 0 || vcpuid >= VM_MAXCPU)
		return (EINVAL);

	if (CPU_ISSET(vcpuid, &vm->active_cpus))
		return (EBUSY);

	CPU_SET_ATOMIC(vcpuid, &vm->active_cpus);
	return (0);

}

cpuset_t
vm_active_cpus(struct vm *vm)
{

	return (vm->active_cpus);
}

void *
vcpu_stats(struct vm *vm, int vcpuid)
{

	return (vm->vcpu[vcpuid].stats);
}

static int
vcpu_set_state_locked(struct vcpu *vcpu, enum vcpu_state newstate,
    bool from_idle)
{
	int error;

	vcpu_assert_locked(vcpu);

	/*
	 * State transitions from the vmmdev_ioctl() must always begin from
	 * the VCPU_IDLE state. This guarantees that there is only a single
	 * ioctl() operating on a vcpu at any point.
	 */
	if (from_idle) {
		while (vcpu->state != VCPU_IDLE)
			msleep_spin(&vcpu->state, &vcpu->mtx, "vmstat", hz);
	} else {
		KASSERT(vcpu->state != VCPU_IDLE, ("invalid transition from "
		    "vcpu idle state"));
	}

	if (vcpu->state == VCPU_RUNNING) {
		KASSERT(vcpu->hostcpu == curcpu, ("curcpu %d and hostcpu %d "
		    "mismatch for running vcpu", curcpu, vcpu->hostcpu));
	} else {
		KASSERT(vcpu->hostcpu == NOCPU, ("Invalid hostcpu %d for a "
		    "vcpu that is not running", vcpu->hostcpu));
	}

	/*
	 * The following state transitions are allowed:
	 * IDLE -> FROZEN -> IDLE
	 * FROZEN -> RUNNING -> FROZEN
	 * FROZEN -> SLEEPING -> FROZEN
	 */
	switch (vcpu->state) {
	case VCPU_IDLE:
	case VCPU_RUNNING:
	case VCPU_SLEEPING:
		error = (newstate != VCPU_FROZEN);
		break;
	case VCPU_FROZEN:
		error = (newstate == VCPU_FROZEN);
		break;
	default:
		error = 1;
		break;
	}

	if (error)
		return (EBUSY);

	vcpu->state = newstate;
	if (newstate == VCPU_RUNNING)
		vcpu->hostcpu = curcpu;
	else
		vcpu->hostcpu = NOCPU;

	if (newstate == VCPU_IDLE)
		wakeup(&vcpu->state);

	return (0);
}

int
vcpu_set_state(struct vm *vm, int vcpuid, enum vcpu_state newstate,
		bool from_idle)
{
	int error;
	struct vcpu *vcpu;

	if (vcpuid < 0 || vcpuid >= VM_MAXCPU)
		panic("vm_set_run_state: invalid vcpuid %d", vcpuid);

	vcpu = &vm->vcpu[vcpuid];

	vcpu_lock(vcpu);
	error = vcpu_set_state_locked(vcpu, newstate, from_idle);
	vcpu_unlock(vcpu);

	return (error);
}

enum vcpu_state
vcpu_get_state(struct vm *vm, int vcpuid, int *hostcpu)
{
	struct vcpu *vcpu;
	enum vcpu_state state;

	if (vcpuid < 0 || vcpuid >= VM_MAXCPU)
		panic("vm_get_run_state: invalid vcpuid %d", vcpuid);

	vcpu = &vm->vcpu[vcpuid];

	vcpu_lock(vcpu);
	state = vcpu->state;
	if (hostcpu != NULL)
		*hostcpu = vcpu->hostcpu;
	vcpu_unlock(vcpu);

	return (state);
}

uint64_t
vm_gpa2hpa(struct vm *vm, uint64_t gpa, size_t len)
{
	uint64_t nextpage;

	nextpage = trunc_page(gpa + PAGE_SIZE);
	if (len > nextpage - gpa)
		panic("vm_gpa2hpa: invalid gpa/len: 0x%016lx/%zu", gpa, len);

	return (VMMMAP_GET(vm->cookie, gpa));
}

int
vm_gpabase2memseg(struct vm *vm, uint64_t gpabase,
		  struct vm_memory_segment *seg)
{
	int i;

	for (i = 0; i < vm->num_mem_segs; i++) {
		if (gpabase == vm->mem_segs[i].gpa) {
			*seg = vm->mem_segs[i];
			return (0);
		}
	}
	return (-1);
}

int
vm_get_register(struct vm *vm, int vcpu, int reg, uint64_t *retval)
{

	if (vcpu < 0 || vcpu >= VM_MAXCPU)
		return (EINVAL);

	if (reg >= VM_REG_LAST)
		return (EINVAL);

	return (VMGETREG(vm->cookie, vcpu, reg, retval));
}

int
vm_set_register(struct vm *vm, int vcpuid, int reg, uint64_t val)
{
	struct vcpu *vcpu;
	int error;

	if (vcpuid < 0 || vcpuid >= VM_MAXCPU)
		return (EINVAL);

	if (reg >= VM_REG_LAST)
		return (EINVAL);
	error = VMSETREG(vm->cookie, vcpuid, reg, val);
	if (error)
		return (error);

	vcpu = &vm->vcpu[vcpuid];
	vcpu->nextpc = val;

	return(0);
}

void *
vm_get_cookie(struct vm *vm)
{
	return vm->cookie;
}

static void
vm_free_mem_seg(struct vm *vm, struct vm_memory_segment *seg)
{
	size_t len;
	uint64_t hpa;

	len = 0;
	while (len < seg->len) {
		hpa = vm_gpa2hpa(vm, seg->gpa + len, PAGE_SIZE);
		if (hpa == (uint64_t)-1) {
			panic("vm_free_mem_segs: cannot free hpa "
			      "associated with gpa 0x%016lx", seg->gpa + len);
		}

		vmm_mem_free(hpa, PAGE_SIZE);

		len += PAGE_SIZE;
	}

	bzero(seg, sizeof(struct vm_memory_segment));
}

/*
 * Return true if 'gpa' is available for allocation, false otherwise
 */
static bool
vm_ipa_available(struct vm *vm, uint64_t ipa)
{
	uint64_t ipabase, ipalimit;
	int i;

	if (!page_aligned(ipa))
		panic("vm_ipa_available: ipa (0x%016lx) not page aligned", ipa);

	for (i = 0; i < vm->num_mem_segs; i++) {
		ipabase = vm->mem_segs[i].gpa;
		ipalimit = ipabase + vm->mem_segs[i].len;
		if (ipa >= ipabase && ipa < ipalimit)
			return (false);
	}

	return (true);
}

/*
 * Allocate 'len' bytes for the virtual machine starting at address 'ipa'
 */
int
vm_malloc(struct vm *vm, uint64_t ipa, size_t len)
{
	struct vm_memory_segment *seg;
	int error, available, allocated;
	uint64_t ipa2;
	vm_paddr_t pa;

	if (!page_aligned(ipa) != 0 || !page_aligned(len) || len == 0)
		return (EINVAL);

	available = allocated = 0;
	ipa2 = ipa;
	while (ipa2 < ipa + len) {
		if (vm_ipa_available(vm, ipa2))
			available++;
		else
			allocated++;
		ipa2 += PAGE_SIZE;
	}

	/*
	 * If there are some allocated and some available pages in the address
	 * range then it is an error.
	 */
	if (allocated != 0  && available != 0)
		return (EINVAL);

	/*
	 * If the entire address range being requested has already been
	 * allocated then there isn't anything more to do.
	 */
	if (allocated != 0 && available == 0)
		return (0);

	if (vm->num_mem_segs == VM_MAX_MEMORY_SEGMENTS)
		return (E2BIG);

	seg = &vm->mem_segs[vm->num_mem_segs];
	error = 0;
	seg->gpa = ipa;
	seg->len = 0;
	while (seg->len < len) {
		pa = vmm_mem_alloc(PAGE_SIZE);
		if (pa == 0) {
			error = ENOMEM;
			break;
		}
		VMMMAP_SET(vm->cookie, ipa, pa, PAGE_SIZE, VM_PROT_ALL);

		seg->len += PAGE_SIZE;
		ipa += PAGE_SIZE;
	}
	vm->num_mem_segs++;

	return (0);
}

int
vm_attach_vgic(struct vm *vm, uint64_t distributor_paddr, uint64_t cpu_int_paddr)
{
	return vgic_attach_to_vm(vm->cookie, distributor_paddr, cpu_int_paddr);
}

static int
vm_handle_wfi(struct vm *vm, int vcpuid, struct vm_exit *vme, bool *retu)
{
	struct vcpu *vcpu;
	struct hypctx *hypctx;
	bool intr_disabled;

	vcpu = &vm->vcpu[vcpuid];
	hypctx = vme->u.wfi.hypctx;
	intr_disabled = !(hypctx->regs.spsr & PSR_I);

	vcpu_lock(vcpu);
	while (1) {
		if (!intr_disabled && vgic_vcpu_pending_irq(hypctx))
			break;

		if (vcpu_should_yield(vm, vcpuid))
			break;

		vcpu_set_state_locked(vcpu, VCPU_SLEEPING, false);
		msleep_spin(vcpu, &vcpu->mtx, "vmidle", hz);
		vcpu_set_state_locked(vcpu, VCPU_FROZEN, false);
	}
	vcpu_unlock(vcpu);

	*retu = false;
	return (0);
}
