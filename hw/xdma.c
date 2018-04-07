#include "device-assignment.h"
#include "xdma.h"

#define XDMA_COMM_UINT8(d, addr) (*((uint8_t *) &(d)->xdma_command[(addr)]))
#define XDMA_COMM_UINT16(d, addr) (*((uint16_t *) &(d)->xdma_command[(addr)]))
#define XDMA_COMM_UINT32(d, addr) (*((uint32_t *) &(d)->xdma_command[(addr)]))


uint32_t xdma_alloc_coherent(AssignedDevRegion *d, xdma_command_t *cmd)
{
	uint64_t in_size, out_status, out_phys, out_virt;


	in_size = cmd->in1;
	fprintf(stderr, "%s: size = %llx\n", __func__, in_size);
	if (in_size == 0 || d->xdma_offset + in_size > XDMA_REGION_SIZE) {
		out_status = 1;
		goto out;
	}


	/*
	 * Here call upci to allocate
	 * the coherent mapping. For
	 * now just return an error.
	 */
	out_status = 1;
	out_phys = 0;
	out_virt = 0;
out:
	cmd->status = out_status;
	cmd->out1 = out_phys;
	cmd->out2 = out_virt;
	return (0);
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
