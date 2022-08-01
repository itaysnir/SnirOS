#include <kern/e1000.h>
#include <kern/pmap.h>
#include <kern/pci.h>

#include <inc/stdio.h>
#include <inc/string.h>
#include <inc/error.h>


// Global pointers
volatile uint32_t *nic_e1000;
bool blocking_socket_flag = false;

// Macros
#define E1000_ACCESS(offset) (nic_e1000[offset >> 2])
// TX pointers
uint8_t tx_data_buffer[TX_RING_BUFFER_SIZE][MAX_DATA_SIZE];
struct e1000_tx_desc *tx_ring_vaddr;
// RX pointers
uint8_t rx_data_buffer[RX_RING_BUFFER_SIZE][MAX_DATA_SIZE];
struct e1000_rx_desc *rx_ring_vaddr;

packet_filter_t blacklist[MAX_FILTER_COUNT];


uint16_t read_eeprom_memory(uint32_t words_offset)
{
    volatile uint32_t* eerd_reg = &E1000_ACCESS(E1000_EERD);
    // Requested address written within offset 8 bits, along with the start bit
    *eerd_reg = (words_offset << 8) | E1000_EERD_START;
    // Wait for "eerd done" flag
    while((*eerd_reg & E1000_EERD_DONE) == false)
    {
        ;
    }
    // Read the requested data
    uint16_t data = *eerd_reg >> 16;

    return data;
}

int flush_interrupt()
{
    uint32_t content = E1000_ACCESS(E1000_ICR);
    return content;
}

// LAB 6: Your driver code here
//int nic_e1000_attach(struct pci_func* pci_func)
int nic_e1000_attach(struct pci_func* pci_func)
{
    pci_func_enable(pci_func);
    nic_e1000 = mmio_map_region(pci_func->reg_base[0],
                                pci_func->reg_size[0]);
    // According to docs, need to write to the IMS_RXT0 register!
    // Moreover - dummy reads are required for flushing
    uint32_t dummy_read_1 = E1000_ACCESS(E1000_ICR);

    E1000_ACCESS(E1000_IMS) |= E1000_IMS_RXT0 | E1000_IMS_SRPD;
    uint32_t dummy_read_2 = E1000_ACCESS(E1000_IMS);

    E1000_ACCESS(E1000_RSRPD) = 1518;
    uint32_t dummy_read_3 = E1000_ACCESS(E1000_RSRPD);

    // Part A - send ring buffer
    struct PageInfo *tx_page = page_alloc(ALLOC_ZERO);
    if (tx_page == NULL)
    {
        panic("nic_e1000_attach: couldn't allocate tx page");
    }
    physaddr_t tx_ring = page2pa(tx_page);
    tx_ring_vaddr = page2kva(tx_page);
    E1000_ACCESS(E1000_TDBAL) = tx_ring;
    E1000_ACCESS(E1000_TDBAH) = 0; // 32 bit addressing
    E1000_ACCESS(E1000_TDLEN) = TX_RING_BUFFER_SIZE * sizeof(struct e1000_tx_desc);
    // Initializing TX Ring buffer descriptors
    int i = 0;
    for (; i < TX_RING_BUFFER_SIZE; i++)
    {
        tx_ring_vaddr[i].addr = (uint64_t) PADDR(&tx_data_buffer[i]);
        tx_ring_vaddr[i].cmd = 0; // DEXT = 0 for legacy mode, all others are disabled too
        tx_ring_vaddr[i].cmd |= (1 << 0); // set RS - indicate desc status
        tx_ring_vaddr[i].cmd |= (1 << 3); // set EOF - support fragmentation handling
        tx_ring_vaddr[i].status |= E1000_TXD_STAT_DD;
    }
    E1000_ACCESS(E1000_TDH) = 0;
    E1000_ACCESS(E1000_TDT) = 0;
    E1000_ACCESS(E1000_TCTL) = 0;
    E1000_ACCESS(E1000_TCTL) |= E1000_TCTL_PSP;
    E1000_ACCESS(E1000_TCTL) |= E1000_TCTL_CT & (0x10 << 4);
    E1000_ACCESS(E1000_TCTL) |= E1000_TCTL_COLD & (0x40 << 12);
    E1000_ACCESS(E1000_TIPG) = 10;
    E1000_ACCESS(E1000_TCTL) |= E1000_TCTL_EN;

    for (i = 0 ; i < 3 ; i++)
    {
        uint16_t mac_bytes = read_eeprom_memory(i);
        e1000_mac_addr[2 * i] = mac_bytes & 0xff;
        e1000_mac_addr[(2 * i) + 1] = (mac_bytes >> 8) & 0xff;
    }


    // Part B - receive ring buffer
    struct PageInfo *rx_page = page_alloc(ALLOC_ZERO);
    if (rx_page == NULL)
    {
        panic("nic_e1000_attach: couldn't allocate rx page");
    }
    physaddr_t rx_ring = page2pa(rx_page);
    rx_ring_vaddr = page2kva(rx_page);
    // Set mac address
    E1000_ACCESS(E1000_RAL) = *(uint32_t *)e1000_mac_addr;
    E1000_ACCESS(E1000_RAH) = *(uint16_t *)(e1000_mac_addr + 4); // 32 bit addressing
    E1000_ACCESS(E1000_RAH) |= E1000_RAH_AV;
    // Disable multicast
    E1000_ACCESS(E1000_MTA) = 0;
    E1000_ACCESS(E1000_RDBAL) = rx_ring;
    E1000_ACCESS(E1000_RDBAH) = 0;
    E1000_ACCESS(E1000_RDLEN) = RX_RING_BUFFER_SIZE * sizeof(struct e1000_rx_desc);
    // Initialize full ring buffer
    E1000_ACCESS(E1000_RDH) = 0;
    E1000_ACCESS(E1000_RDT) = RX_RING_BUFFER_SIZE - 1;
    // Set the RCTL register
    E1000_ACCESS(E1000_RCTL) = 0;
    E1000_ACCESS(E1000_RCTL) |= E1000_RCTL_SECRC;
    E1000_ACCESS(E1000_RCTL) |= E1000_RCTL_SZ_2048;

    for (i = 0; i < RX_RING_BUFFER_SIZE; i++) {
        rx_ring_vaddr[i].addr = (uint64_t) PADDR(&rx_data_buffer[i]);
        // Initially, descriptor done is cleared. Only when the NIC finishes filling the entry's buffer, this bit is set by the HW.
        rx_ring_vaddr[i].status &= ~E1000_RXD_STAT_DD;
    }
    E1000_ACCESS(E1000_RCTL) |= E1000_RCTL_EN;

    return 0;
}


int send_packet(char *buf, int size)
{
    // Invalid buf pointer
    if (buf == NULL)
    {
        return -INVALID_BUF_PTR;
    }
    // Packet size upper bounded by ethernet frame size
    if (size > ETH_PACKET_MAX_SIZE)
    {
        return -PKT_TOO_LONG;
    }
    uint32_t tail_index = E1000_ACCESS(E1000_TDT);
    // Checking if next descriptor TX is available
    if ((tx_ring_vaddr[tail_index].status & E1000_TXD_STAT_DD) == false)
    {
        return -FULL_RING_BUF;
    }
    // Turning off the TX descriptor done flag
    tx_ring_vaddr[tail_index].status &= ~E1000_TXD_STAT_DD;
    // Copy buf into the TX ring buffer
    if (memcpy(&tx_data_buffer[tail_index], buf, size) == NULL)
    {
        return -MEMCPY_FAILED;
    }
    // Update the TX descriptor length
    tx_ring_vaddr[tail_index].length = size;
    // Increment the TDT by 1, accounting the circular ring buffer
    E1000_ACCESS(E1000_TDT) = (tail_index + 1) % TX_RING_BUFFER_SIZE;

    return 0;
}

int recv_packet(char *buf, int size)
{
    if (buf == NULL)
    {
        return -INVALID_BUF_PTR;
    }
    int next_packet_index = (E1000_ACCESS(E1000_RDT) + 1) % RX_RING_BUFFER_SIZE;
    if (!(rx_ring_vaddr[next_packet_index].status & E1000_RXD_STAT_DD))
    {
        return -EMPTY_RING_BUF; // queue is empty
    }
    if (!(rx_ring_vaddr[next_packet_index].status & E1000_RXD_STAT_EOP))
    {
        return -JUMBO_PACKET; // queue is empty
    }

    if (!passes_filters(rx_data_buffer[next_packet_index]))
    {
        // Discard the packet, without copying it back to userspace
        E1000_ACCESS(E1000_RDT) = next_packet_index;
        rx_ring_vaddr[next_packet_index].status &= ~E1000_TXD_STAT_DD & ~E1000_RXD_STAT_EOP;
        return -BLACKLISTED;
    }

    int packet_size = MIN(rx_ring_vaddr[next_packet_index].length, size);
    memmove(buf, rx_data_buffer[next_packet_index], packet_size);
    E1000_ACCESS(E1000_RDT) = next_packet_index;
    rx_ring_vaddr[next_packet_index].status &= ~E1000_TXD_STAT_DD & ~E1000_RXD_STAT_EOP;

    return packet_size;
}

int enable_non_blocking_socket()
{
    blocking_socket_flag = true;
    return 0;
}

int disable_non_blocking_socket()
{
    int value = blocking_socket_flag;
    blocking_socket_flag = false;

    return value;
}

int set_input_envid(int32_t envid)
{
    input_envid = envid;
    return 0;
}

int add_filter(uint32_t ip, uint16_t port)
{
    packet_filter_t filter = {ip, port};
    int i;
    for (i = 0 ; i < MAX_FILTER_COUNT ; i++)
    {
        if (blacklist[i].ip == 0 && blacklist[i].port == 0)
        {
            blacklist[i] = filter;
            return 0;
        }
    }
    return -1;
}

bool passes_filters(void *pkt)
{
    uint32_t src_ip = *(uint32_t*)((uint32_t)(pkt) + 30);
    src_ip = ((src_ip>>24)&0xff) | // move byte 3 to byte 0
              ((src_ip<<8)&0xff0000) | // move byte 1 to byte 2
              ((src_ip>>8)&0xff00) | // move byte 2 to byte 1
              ((src_ip<<24)&0xff000000); // byte 0 to byte 3

    uint16_t src_port = *(uint16_t*)((uint32_t)(pkt) + 36);
    src_port = (src_port>>8) | (src_port<<8);


    int i;
    for (i = 0 ; i < MAX_FILTER_COUNT ; i++)
    {
        if ((blacklist[i].ip != 0 && blacklist[i].ip == src_ip) || (blacklist[i].port != 0 && blacklist[i].port == src_port))
        {
            return false;
        }
    }

    return true;
}
