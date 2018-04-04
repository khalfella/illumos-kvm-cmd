#include "device-assignment.h"
#include "xdma.h"

#define XDMA_COMM_UINT8(d, addr) (*((uint8_t *) &(d)->xdma_command[(addr)]))
#define XDMA_COMM_UINT16(d, addr) (*((uint16_t *) &(d)->xdma_command[(addr)]))
#define XDMA_COMM_UINT32(d, addr) (*((uint32_t *) &(d)->xdma_command[(addr)]))


uint32_t xdma_execute_command(AssignedDevRegion *d)
{
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
