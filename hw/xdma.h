#ifndef __XDMA_H__
#define __XDMA_H__

#include "device-assignment.h"








#define XDMA_REGION_SIZE	0x800000	/* 128 MB */

uint32_t xdma_slow_bar_readb(AssignedDevRegion *d, target_phys_addr_t addr);
uint32_t xdma_slow_bar_readw(AssignedDevRegion *d, target_phys_addr_t addr);
uint32_t xdma_slow_bar_readl(AssignedDevRegion *d, target_phys_addr_t addr);

void xdma_slow_bar_writeb(AssignedDevRegion *d, target_phys_addr_t addr,
    uint32_t val);
void xdma_slow_bar_writew(AssignedDevRegion *d, target_phys_addr_t addr,
    uint32_t val);
void xdma_slow_bar_writel(AssignedDevRegion *d, target_phys_addr_t addr,
    uint32_t val);



#endif /* __XDMA_H__ */
