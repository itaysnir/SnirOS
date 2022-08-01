#include "ns.h"
#include <inc/lib.h>

#define ETH_MAX_PACKET_SIZE 1518
extern union Nsipc nsipcbuf;

void
output(envid_t ns_envid)
{
    // LAB 6: Your code here:
    // 	- read a packet from the network server
    //	- send the packet to the device driver
    binaryname = "ns_output";
    int ret_code;

    for (;;)
    {
        envid_t from_env;
        int perm;
        if ((ret_code = ipc_recv(&from_env, &nsipcbuf, &perm)) < 0)
        {
            // If ipc_recv fails, the environment remains running, and waiting for new requests
            cprintf("output: ipc_recv failed with: %e", ret_code);
            continue;
        }
        // Checking the request was initiated by the network server
        // The ONLY message the network server is allowed to send to the output environment is NSREQ_OUTPUT
        // Also, the desired request page (that contains Nsipc) is indeed present.
        if (from_env != ns_envid || ret_code != NSREQ_OUTPUT || !(perm & PTE_P))
        {
            cprintf("output: got an invalid recv from envid: %d ret_code: %e perm: %d", from_env, ret_code, perm);
            continue;
        }

        struct jif_pkt *pkt = &nsipcbuf.pkt;
        int pkt_len = pkt->jp_len;
        char *pkt_data = pkt->jp_data;
        // Send the packet, while handling fragmentation if necessary
        int packet_offset = 0;
        int send_ret_code = 0;
        for (; packet_offset < pkt_len; packet_offset += ETH_MAX_PACKET_SIZE)
        {
            int fragmented_pkt_size = MIN(pkt_len - packet_offset, ETH_MAX_PACKET_SIZE);
            for (;;)
            {
                send_ret_code = sys_send_packet(&pkt_data[packet_offset], fragmented_pkt_size);
                if (send_ret_code == -FULL_RING_BUF)
                {
                    sys_yield();
                }
                else
                {
                    break;
                }
            }
        }
    }
}
