#include "device-assignment.h"
#include "xdma.h"


uint32_t xdma_slow_bar_readb(AssignedDevRegion *d, target_phys_addr_t addr)
{
	return (0);
}


uint32_t xdma_slow_bar_readw(AssignedDevRegion *d, target_phys_addr_t addr)
{
	return (0);
}

uint32_t xdma_slow_bar_readl(AssignedDevRegion *d, target_phys_addr_t addr)
{
	return (0);
}


void xdma_slow_bar_writeb(AssignedDevRegion *d, target_phys_addr_t addr,
    uint32_t val)
{
}

void xdma_slow_bar_writew(AssignedDevRegion *d, target_phys_addr_t addr,
    uint32_t val)
{
}

void xdma_slow_bar_writel(AssignedDevRegion *d, target_phys_addr_t addr,
    uint32_t val)
{
}
