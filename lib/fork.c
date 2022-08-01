// implement fork from user space

#include <inc/string.h>
#include <inc/lib.h>

// PTE_COW marks copy-on-write page table entries.
// It is one of the bits explicitly allocated to user processes (PTE_AVAIL).
#define PTE_COW		0x800

extern void _pgfault_upcall(void);
//
// Custom page fault handler - if faulting page is copy-on-write,
// map in our own private writable copy.
//
static void
pgfault(struct UTrapframe *utf)
{
	void *addr = (void *) utf->utf_fault_va;
	uint32_t err = utf->utf_err;
	int r;

	// Check that the faulting access was (1) a write, and (2) to a
	// copy-on-write page.  If not, panic.
	// Hint:
	//   Use the read-only page table mappings at uvpt
	//   (see <inc/memlayout.h>).

	// LAB 4: Your code here.

    if (!(err & FEC_WR))
    {
        panic("pgfault: bad error code: %d\n", err);
    }

    if (!(uvpd[PDX(addr)] & PTE_P))
    {
        panic("pgfault: bad PDE permissions\n");
    }

    if (!(uvpt[PGNUM(addr)] & (PTE_COW | PTE_P)))
    {
        panic("pgfault: bad PTE permissions\n");
    }

	// Allocate a new page, map it at a temporary location (PFTEMP),
	// copy the data from the old page to the new page, then move the new
	// page to the old page's address.
	// Hint:
	//   You should make three system calls.

	// LAB 4: Your code here.
    //if (sys_page_alloc(thisenv->env_id, PFTEMP, PTE_U | PTE_W | PTE_P) < 0)
    if (sys_page_alloc(0, PFTEMP, PTE_U | PTE_W | PTE_P) < 0)
    {
        panic("pgfault: PFTEMP page allocation failed\n");
    }

    memmove(PFTEMP, ROUNDDOWN(addr, PGSIZE), PGSIZE);

    //if (sys_page_map(thisenv->env_id, PFTEMP, thisenv->env_id, ROUNDDOWN(addr, PGSIZE), PTE_U | PTE_W | PTE_P) < 0)
    if (sys_page_map(0, PFTEMP, 0, ROUNDDOWN(addr, PGSIZE), PTE_U | PTE_W | PTE_P) < 0)
    {
        panic("pgfault: sys_page_map failed\n");
    }

    //if (sys_page_unmap(thisenv->env_id, PFTEMP) < 0)
    if (sys_page_unmap(0, PFTEMP) < 0)
    {
        panic("pgfault: sys_page_unmap failed\n");
    }
}

//
// Map our virtual page pn (address pn*PGSIZE) into the target envid
// at the same virtual address.  If the page is writable or copy-on-write,
// the new mapping must be created copy-on-write, and then our mapping must be
// marked copy-on-write as well.  (Exercise: Why do we need to mark ours
// copy-on-write again if it was already copy-on-write at the beginning of
// this function?)
//
// Returns: 0 on success, < 0 on error.
// It is also OK to panic on error.
//
static int
duppage(envid_t envid, unsigned pn)
{
	int r;

	// LAB 4: Your code here.
    void * addr = (void *)(pn * PGSIZE);
    pte_t addr_pte = uvpt[pn];

    // LAB 5: our addition here to PTE_SHARE
    if (!(addr_pte & (PTE_W | PTE_COW)) || (addr_pte & PTE_SHARE))
    {
        if ((r = sys_page_map(thisenv->env_id, addr, envid, addr, addr_pte & (PTE_SYSCALL))) < 0)
        {
            panic("duppage: sys_page_map failed with code: %e\n", r);
        }
        return 0;
    }

    addr_pte &= ~PTE_W;
    addr_pte |= PTE_COW;

    if ((r = sys_page_map(thisenv->env_id, addr, envid, addr, addr_pte & (PTE_SYSCALL))) < 0)
    {
        panic("duppage: sys_page_map failed with code: %e\n", r);
    }

    if ((r = sys_page_map(thisenv->env_id, addr, thisenv->env_id, addr, addr_pte & (PTE_SYSCALL))) < 0)
    {
        panic("duppage: sys_page_map failed with code: %e\n", r);
    }

    return 0;
}

//
// User-level fork with copy-on-write.
// Set up our page fault handler appropriately.
// Create a child.
// Copy our address space and page fault handler setup to the child.
// Then mark the child as runnable and return.
//
// Returns: child's envid to the parent, 0 to the child, < 0 on error.
// It is also OK to panic on error.
//
// Hint:
//   Use uvpd, uvpt, and duppage.
//   Remember to fix "thisenv" in the child process.
//   Neither user exception stack should ever be marked copy-on-write,
//   so you must allocate a new page for the child's user exception stack.
//
envid_t
fork(void)
{
	// LAB 4: Your code here.
    int r;
    envid_t envid;
    uintptr_t addr;

    set_pgfault_handler(pgfault);

    envid = sys_exofork();
    if (envid < 0)
    {
        panic("fork: sys_exofork failed with %e\n", envid);
    }

    if (envid == 0)
    {
        thisenv = &envs[ENVX(sys_getenvid())];
        return 0;
    }

    for (addr = 0 ; addr < UTOP ; addr += PGSIZE)
    {
        if (!(uvpd[PDX(addr)] & PTE_P) || !(uvpt[PGNUM(addr)] & PTE_P) || addr == (uintptr_t)(UXSTACKTOP - PGSIZE))
        {
            continue;
        }
        duppage(envid, PGNUM(addr));
    }

    if (sys_page_alloc(envid, (void *) (UXSTACKTOP - PGSIZE), PTE_W | PTE_U | PTE_P) < 0)
    {
        panic("fork: user exception stack allocation failed\n");
    }

    if (sys_env_set_pgfault_upcall(envid, _pgfault_upcall) < 0)
    {
        panic("fork: setting pgfault upcall failed\n");
    }

    if ((r = sys_env_set_status(envid, ENV_RUNNABLE)) < 0)
    {
        panic("fork: sys_env_set_status failed with %e", r);
    }

    return envid;
}

// Challenge!
int
sfork(void)
{
	panic("sfork not implemented");
	return -E_INVAL;
}
