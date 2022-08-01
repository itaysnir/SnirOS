#include <inc/lib.h>

void
umain(int argc, char **argv)
{
    int i;
    if (argc < 2)
    {
        cprintf("Usage: touch [file1] [file2] [...]\n");
        return;
    }

    for (i = 1; i < argc; i++)
    {
        if (open(argv[i], O_CREAT) < 0)
        {
            cprintf("Open %s failed\n", argv[i]);
            return;
        }
    }
}
