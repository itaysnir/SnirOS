#include <inc/lib.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>

#define PORT 7

#define BUFFSIZE 32
#define MAXPENDING 5    // Max connection requests

#define MAX_CLIENTS 10
#define SHARED_MEM 0x60000000


int get_fd_num(int sock_id, struct Fd** fds)
{
    int i = 0;
    for (; i < MAX_CLIENTS ; i++)
    {
        if (fds[i] != NULL && fds[i]->fd_sock.sockid == sock_id)
        {
            return fd2num(fds[i]);
        }
    }
    return -1;
}


bool socket_exists(int sock_id, struct Fd** fds)
{
    int i = 0;
    for (; i < MAX_CLIENTS ; i++)
    {
        if (fds[i] != NULL && fds[i]->fd_sock.sockid == sock_id)
        {
            return true;
        }
    }
    return false;
}

int get_lowest_fd_index(struct Fd** fds)
{
    int i = 0;
    for (; i < MAX_CLIENTS ; i++)
    {
        if (fds[i] == NULL)
        {
            return i;
        }
    }
    return -1;
}


static void
die(char *m)
{
    cprintf("%s\n", m);
    exit();
}

struct Fd* alloc_sock_fd(int sock_id)
{
    int m;
    struct Fd *fd = NULL;
    if ((m = fd_alloc(&fd)) < 0)
    {
        die("fd_alloc failed at child");
    }
    if ((m = sys_page_alloc(0, fd, PTE_P | PTE_U | PTE_W | PTE_SHARE)) < 0)
    {
        die("sys_page_alloc failed at child");
    }
    fd->fd_dev_id = 115;
    fd->fd_offset = 0;
    fd->fd_omode = 2;
    fd->fd_sock.sockid = sock_id;

    return fd;
}


void
umain(int argc, char **argv)
{
    int serversock, clientsock;
    struct sockaddr_in echoserver, echoclient;
    char buffer[BUFFSIZE];
    unsigned int echolen;
    int received = 0;
    int r;

    // Create the TCP socket
    if ((serversock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP)) < 0)
        die("Failed to create socket");

    cprintf("opened socket\n");

    // Construct the server sockaddr_in structure
    memset(&echoserver, 0, sizeof(echoserver));       // Clear struct
    echoserver.sin_family = AF_INET;                  // Internet/IP
    echoserver.sin_addr.s_addr = htonl(INADDR_ANY);   // IP address
    echoserver.sin_port = htons(PORT);		  // server port

    cprintf("trying to bind\n");

    // Bind the server socket
    if (bind(serversock, (struct sockaddr *) &echoserver,
             sizeof(echoserver)) < 0) {
        die("Failed to bind the server socket");
    }

    // Listen on the server socket
    if (listen(serversock, MAXPENDING) < 0)
        die("Failed to listen on server socket");

    cprintf("bound\n");


    envid_t parent_envid = sys_getenvid();
    // if child - serve all
    envid_t child_envid = fork();

    if (child_envid < 0)
    {
        die("error forking");
    }
    else if (child_envid == 0)
    {
        char buffer[BUFFSIZE];
        int *client_sockets = (int *)SHARED_MEM;

        struct Fd* client_fds[MAX_CLIENTS];
        struct Fd* fd = NULL;
        int i = 0, j = 0, bytes_read = 0;
        while (1)
        {
            for (i = 0 ; i < MAX_CLIENTS ; i++)
            {
                if (client_sockets[i] <= 0)
                {
                    continue;
                }
                if (!socket_exists(client_sockets[i], client_fds))
                {
                    fd = alloc_sock_fd(client_sockets[i]);
                    client_fds[get_lowest_fd_index(client_fds)] = fd;
                }

                sys_enable_non_blocking_socket();
                bytes_read = read(client_sockets[i], buffer, BUFFSIZE);
                if (bytes_read == 0)
                {
                    // Client exits via CTRL+C
                    continue;
                }

                if (bytes_read < 0)
                {
                    // No buffer to read, continue
                    continue;
                }

                for (j = 0 ; j < MAX_CLIENTS ; j++)
                {
                    if (client_sockets[j] <= 0)
                    {
                        continue;
                    }
                    write(client_sockets[j], buffer, bytes_read);
                }
            }
        }

        return;
    }
    else
    {   // This is parent
        int *parent_client_sockets = (int *)SHARED_MEM;
        if ((r = sys_page_alloc(parent_envid, parent_client_sockets, PTE_P | PTE_W | PTE_U)) < 0)
        {
            die("parent: sys_page_alloc failed\n");
        }

        if ((r = sys_page_map(parent_envid, (void *)SHARED_MEM, child_envid, (void *)SHARED_MEM, PTE_P | PTE_W | PTE_U)) < 0)
        {
            die("parent: sys_page_map failed\n");
        }


        while (1)
        {
            unsigned int clientlen = sizeof(echoclient);
            // Wait for client connection
            if ((clientsock = accept(serversock, (struct sockaddr *) &echoclient, &clientlen)) < 0)
            {
                die("Failed to accept client connection");
            }

            int i;
            for (i = 0 ; i < MAX_CLIENTS ; i++)
            {
                if (parent_client_sockets[i] <= 0)
                {
                    parent_client_sockets[i] = clientsock;
                    break;
                }
            }
            if (i == MAX_CLIENTS)
            {
                close(clientsock);
            }
        }
    }

    close(serversock);
}
