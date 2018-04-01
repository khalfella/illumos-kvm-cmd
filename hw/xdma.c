#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <upci.h>

#include <upci.h>

#include "device-assignment.h"
#include "xdma.h"

#define XDMA_COMM_UINT8(d, addr) (*((uint8_t *) &(d)->xdma_command[(addr)]))
#define XDMA_COMM_UINT16(d, addr) (*((uint16_t *) &(d)->xdma_command[(addr)]))
#define XDMA_COMM_UINT32(d, addr) (*((uint32_t *) &(d)->xdma_command[(addr)]))


#define ROUND_UP(N, S) ((((N) + (S) - 1) / (S)) * (S))

uint32_t xdma_alloc_coherent(AssignedDevRegion *d, xdma_command_t *cmd)
{
	upci_coherent_t ch;
	uint64_t len, flags;

	flags = cmd->in1;
	len = cmd->in2;
	fprintf(stderr, "%s: offset = %llx length = %llx flags = %llx\n",
	    __func__, d->xdma_virtual, len, flags);
	if (len  == 0 || d->xdma_virtual + len > XDMA_REGION_SIZE) {
		goto out;
	}

	ch.ch_flags = flags;
	ch.ch_length = len;
	if (ioctl(d->region->upci_fd, UPCI_IOCTL_XDMA_ALLOC_COHERENT, &ch) == 0) {
		cmd->status = 0;
		cmd->out1 = ch.ch_cookie;
		cmd->out2 = d->xdma_virtual;
		d->xdma_virtual += ch.ch_length;
		d->xdma_virtual = ROUND_UP(d->xdma_virtual, 4096);
		fprintf(stderr, "%s: phy = %llx vir = %llx\n",
		    __func__, cmd->out1, cmd->out2);
		return (0);
	}
out:
	fprintf(stderr, "%s: failed\n", __func__);
	cmd->status = 1;
	return (1);
}

uint32_t xdma_execute_command(AssignedDevRegion *d)
{
	xdma_command_t *cmd;

	cmd =  (xdma_command_t *) d->xdma_command;

	fprintf(stderr, "%s: command = %d\n", __func__, cmd->command);

	switch (cmd->command) {
		case 1:
			xdma_alloc_coherent(d, cmd);
		break;
	}
	return (0);
}

uint32_t xdma_slow_bar_readb(AssignedDevRegion *d, target_phys_addr_t addr)
{
	if (addr < sizeof(xdma_command_t)) {
		return XDMA_COMM_UINT8(d, addr);
	}

	return (0);
}


uint32_t xdma_slow_bar_readw(AssignedDevRegion *d, target_phys_addr_t addr)
{
	if (addr < sizeof(xdma_command_t)) {
		return XDMA_COMM_UINT16(d, addr);
	}

	return (0);
}

uint32_t xdma_slow_bar_readl(AssignedDevRegion *d, target_phys_addr_t addr)
{
	if (addr < sizeof(xdma_command_t)) {
		return XDMA_COMM_UINT32(d, addr);
	}

	return (0);
}


void xdma_slow_bar_writeb(AssignedDevRegion *d, target_phys_addr_t addr,
    uint32_t val)
{
	if (addr < sizeof(xdma_command_t)) {
		XDMA_COMM_UINT8(d, addr) = val;
		if (addr == 0) {
			xdma_execute_command(d);
		}
		return;
	}
}

void xdma_slow_bar_writew(AssignedDevRegion *d, target_phys_addr_t addr,
    uint32_t val)
{
	if (addr < sizeof(xdma_command_t)) {
		XDMA_COMM_UINT16(d, addr) = val;
		return;
	}
}

void xdma_slow_bar_writel(AssignedDevRegion *d, target_phys_addr_t addr,
    uint32_t val)
{
	if (addr < sizeof(xdma_command_t)) {
		XDMA_COMM_UINT32(d, addr) = val;
		return;
	}
}
