#ifndef __XDMA_H__
#define __XDMA_H__

#include <sys/list.h>
#include "device-assignment.h"



#define XDMA_COMM_OFFSET	0x00

typedef struct {
	uint64_t command;
	uint64_t status;
	uint64_t out1;
	uint64_t out2;
	uint64_t out3;
	uint64_t in1;
	uint64_t in2;
	uint64_t in3;
	uint64_t in4;
	uint64_t in5;
	uint64_t in6;
	uint64_t in7;
} xdma_command_t;

#define XDMA_BEGIN_VIRTUAL	0x2000		/* 8 KB */
#define XDMA_REGION_SIZE	0x800000	/* 8 MB */

typedef struct xdma_ch_ent_s {
	uint64_t	xh_flags;
	uint64_t	xh_length;
	uint64_t	xh_cookie;
	uint64_t	xh_off;
	list_node_t	xh_next;
} xdma_ch_ent_t;

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
