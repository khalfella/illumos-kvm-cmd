#ifndef __XDMA_H__
#define __XDMA_H__

#include <sys/list.h>
#include "device-assignment.h"



#define XDMA_COMM_OFFSET	0x00
#define XDMA_BEGIN_VIRTUAL	0x2000		/* 8 KB */
#define XDMA_REGION_SIZE	0x2000000	/* 32 MB */

/* xdma commands */
#define XDMA_CMD_COMMAND_ALLOC		0x01	/* allocate new map */
#define XDMA_CMD_COMMAND_REMOVE		0x02	/* remove an existing map */
#define XDMA_CMD_COMMAND_INQUIRY	0x03	/* get map info */
#define XDMA_CMD_COMMAND_SYNC		0x04	/* sync cpu/device map view */

/* xdma command return status */
#define	XDMA_CMD_STATUS_OK		0x00	/* We are good */
#define	XDMA_CMD_STATUS_ER		0x01	/* Something wrong */

/* xdma command map type */
#define XDMA_CMD_MAP_TYPE_COH		0x01	/* coherent map */
#define XDMA_CMD_MAP_TYPE_STR		0x02	/* streaming map */

/* sync options */
#define XDMA_CMD_SYNC_FORCPU            0x01    /* sync for cpu */
#define XDMA_CMD_SYNC_FORDEV            0x02    /* sync for dev */

typedef struct xdma_cmd_s {
	uint64_t	xc_command;		/* alloc, rem, inq */
	uint64_t	xc_status;		/* ok, error */
	uint64_t	xc_type;		/* coherent, streaming */
	uint64_t	xc_dir;			/* map direction */
	uint64_t	xc_size;		/* map size */
	uint64_t	xc_gx_off;		/* offset in xdma map */
	uint64_t	xc_hx_phys;		/* cookie or dma_addr_t */

	/* guest buffer for streaming dma */
	uint64_t	xc_gb_vir;		/* guest buffer vir addr */
	uint64_t	xc_gb_phys;		/* guest buffer phys addr */
	uint64_t	xc_gb_off;		/* guest buffer map off */
} xdma_cmd_t;

#define XDMA_ENT_FLAGS_ACTIVE	0x00		/* Active map */
#define XDMA_ENT_FLAGS_SHADOW	0x01		/* Shadow map */

typedef struct xdma_ent_s {
	uint64_t	xd_flags;		/* active/shadow map */
	uint64_t	xd_type;		/* coherent or streaming */
	uint64_t	xd_length;		/* map used length/size */
	uint64_t	xd_total_length;	/* map total length/size */
	void		*xd_bbuff;		/* bounce buffer */
	uint64_t	xd_gx_off;		/* offset in xdma map */
	uint64_t	xd_hx_phys;		/* cookie or dma_addr_t */

	/* guest buffer */
	uint64_t	xd_gb_vir;		/* guest buffer vir addr */
	uint64_t	xd_gb_phys;		/* guest buffer phys addr */
	uint64_t	xd_gb_off;		/* offset in guest buffer */

	list_node_t	xd_next;
} xdma_ent_t;


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
