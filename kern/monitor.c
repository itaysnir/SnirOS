// Simple command-line kernel monitor useful for
// controlling the kernel and exploring the system interactively.

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/memlayout.h>
#include <inc/assert.h>
#include <inc/x86.h>

#include <kern/console.h>
#include <kern/monitor.h>
#include <kern/kdebug.h>
#include <kern/pmap.h>      // Our addition
#include <kern/trap.h>
#include <kern/env.h>

#define CMDBUF_SIZE	80	// enough for one VGA text line

// Our addition
extern pde_t *kern_pgdir;

struct Command {
	const char *name;
	const char *desc;
	// return -1 to force monitor to exit
	int (*func)(int argc, char** argv, struct Trapframe* tf);
};

static struct Command commands[] = {
	{ "help", "Display this list of commands", mon_help },
	{ "kerninfo", "Display information about the kernel", mon_kerninfo },
	{ "backtrace", "Display information about kernel stack", mon_backtrace },
    { "showmappings", "Display information of all physical page mappings", mon_showmappings },
    { "continue", "Continues execution of the environment", mon_continue},
    { "c", "Continues execution of the environment", mon_continue},
    { "si", "Performs a single instruction of the current environment", mon_si},
    };
#define NCOMMANDS (sizeof(commands)/sizeof(commands[0]))

/***** Implementations of basic kernel monitor commands *****/

int
mon_help(int argc, char **argv, struct Trapframe *tf)
{
	int i;

	for (i = 0; i < NCOMMANDS; i++)
		cprintf("%s - %s\n", commands[i].name, commands[i].desc);
	return 0;
}

int
mon_kerninfo(int argc, char **argv, struct Trapframe *tf)
{
	extern char _start[], entry[], etext[], edata[], end[];

	cprintf("Special kernel symbols:\n");
	cprintf("  _start                  %08x (phys)\n", _start);
	cprintf("  entry  %08x (virt)  %08x (phys)\n", entry, entry - KERNBASE);
	cprintf("  etext  %08x (virt)  %08x (phys)\n", etext, etext - KERNBASE);
	cprintf("  edata  %08x (virt)  %08x (phys)\n", edata, edata - KERNBASE);
	cprintf("  end    %08x (virt)  %08x (phys)\n", end, end - KERNBASE);
	cprintf("Kernel executable memory footprint: %dKB\n",
		ROUNDUP(end - entry, 1024) / 1024);
	return 0;
}

int
mon_backtrace(int argc, char **argv, struct Trapframe *tf)
{
	cprintf("Stack backtrace:\n");

	uint32_t current_ebp = read_ebp();

	while (current_ebp != 0)
	{
		uint32_t current_ra = *(uint32_t*)(current_ebp + 4);
		uint32_t arg_5 = *(uint32_t*)(current_ebp + 8);
		uint32_t arg_4 = *(uint32_t*)(current_ebp + 12);
		uint32_t arg_3 = *(uint32_t*)(current_ebp + 16);
		uint32_t arg_2 = *(uint32_t*)(current_ebp + 20);
		uint32_t arg_1 = *(uint32_t*)(current_ebp + 24);

		struct Eipdebuginfo eip_debug;

		cprintf("  ebp %08x  eip %08x  args %08x %08x %08x %08x %08x\n", current_ebp, current_ra, arg_5, arg_4, arg_3, arg_2, arg_1);

		debuginfo_eip((uintptr_t)current_ra, (struct Eipdebuginfo*)(&eip_debug));

		cprintf("         %s:%d: %.*s+%d\n", eip_debug.eip_file, eip_debug.eip_line, eip_debug.eip_fn_namelen, eip_debug.eip_fn_name, current_ra - (uint32_t) eip_debug.eip_fn_addr);
		current_ebp = *(uint32_t*)(current_ebp);
	}
		return 0;
}

int
mon_showmappings(int argc, char **argv, struct Trapframe *tf)
{
    if (argc != 3)
    {
        cprintf("Usage: showmappings <start_addr> <end_addr>\n");
        return -1;
    }
    uint32_t start_addr = strtol(argv[1], 0, 16);
    uint32_t end_addr = strtol(argv[2], 0, 16);
    if (end_addr < start_addr)
    {
        cprintf("start_addr cannot exceed end_addr\n");
        return -1;
    }

    uint32_t start_page = ROUNDDOWN(start_addr, PGSIZE);
    uint32_t end_page = ROUNDDOWN(end_addr, PGSIZE);
    uint32_t page_addr = start_page;
    for ( ; page_addr < end_addr ; page_addr += PGSIZE )
    {
        pte_t* pte = pgdir_walk(kern_pgdir, (const void*) page_addr, 0);
        if (pte == NULL)
        {
            cprintf("VA %08x is not mapped to physical address\n", page_addr);
        }
        else
        {
            cprintf("VA %08x is mapped to PA %08x permissions: PTE_P %x PTE_W %x PTE_U %x\n",
                    page_addr,
                    PTE_ADDR(*pte),
                    *pte & PTE_P,
                    *pte & PTE_W,
                    *pte & PTE_U
                    );
        }
    }

    return 0;
}

int
mon_continue(int argc, char **argv, struct Trapframe *tf)
{
    if (argc != 1)
    {
        cprintf("Usage: continue\n");
        return -1;
    }

    if (tf->tf_trapno != T_BRKPT && tf->tf_trapno != T_DEBUG)
    {
        panic("mon_continue: unexpected trap!\n");
    }

    if (tf->tf_trapno == T_DEBUG)
    {
        tf->tf_eflags = (tf->tf_eflags & ~(1 << 8));
    }

    env_run(curenv);
    return 0;
}

int mon_si(int argc, char **argv, struct Trapframe *tf)
{
    if (argc != 1)
    {
        cprintf("Usage: si\n");
        return -1;
    }

    if (tf->tf_trapno != T_BRKPT && tf->tf_trapno != T_DEBUG)
    {
        panic("mon_si: unexpected trap!\n");
    }

    tf->tf_eflags |= (0x1 << 8);

    env_run(curenv);

    return 0;
}


/***** Kernel monitor command interpreter *****/

#define WHITESPACE "\t\r\n "
#define MAXARGS 16

static int
runcmd(char *buf, struct Trapframe *tf)
{
	int argc;
	char *argv[MAXARGS];
	int i;

	// Parse the command buffer into whitespace-separated arguments
	argc = 0;
	argv[argc] = 0;
	while (1) {
		// gobble whitespace
		while (*buf && strchr(WHITESPACE, *buf))
			*buf++ = 0;
		if (*buf == 0)
			break;

		// save and scan past next arg
		if (argc == MAXARGS-1) {
			cprintf("Too many arguments (max %d)\n", MAXARGS);
			return 0;
		}
		argv[argc++] = buf;
		while (*buf && !strchr(WHITESPACE, *buf))
			buf++;
	}
	argv[argc] = 0;

	// Lookup and invoke the command
	if (argc == 0)
		return 0;
	for (i = 0; i < NCOMMANDS; i++) {
		if (strcmp(argv[0], commands[i].name) == 0)
			return commands[i].func(argc, argv, tf);
	}
	cprintf("Unknown command '%s'\n", argv[0]);
	return 0;
}

void
monitor(struct Trapframe *tf)
{
	char *buf;

	cprintf("Welcome to the JOS kernel monitor!\n");
	cprintf("Type 'help' for a list of commands.\n");

	if (tf != NULL)
		print_trapframe(tf);


	while (1) {
		buf = readline("K> ");
		if (buf != NULL)
			if (runcmd(buf, tf) < 0)
				break;
	}
}
