#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <upci.h>

#include <sys/ddi.h>
#include <sys/sunddi.h>


#include "device-assignment.h"
#include "xdma.h"

#define XDMA_COMM_UINT8(d, addr) (*((uint8_t *) &(d)->xdma_command[(addr)]))
#define XDMA_COMM_UINT16(d, addr) (*((uint16_t *) &(d)->xdma_command[(addr)]))
#define XDMA_COMM_UINT32(d, addr) (*((uint32_t *) &(d)->xdma_command[(addr)]))


#define	ROUND_UP(N, S)		((((N) + (S) - 1) / (S)) * (S))
#define	ROUND_DOWN(N,S)		(((N) / (S)) * (S))

static xdma_ent_t *
xdma_find_active_map(AssignedDevRegion *d, uint64_t gxoff, uint64_t hxphys)
{
	xdma_ent_t *xde;

	/*
	 * Start searching from the tail of the list.
	 * it is more likely we are looking for a recently
	 * allocated entry.
	 */
	for (xde = list_tail(&d->xdma_list); xde != NULL;
	    xde = list_prev(&d->xdma_list, xde)) {

		if (gxoff != 0 && gxoff >= xde->xd_gx_off &&
		    gxoff < xde->xd_gx_off + xde->xd_length) {
			return (xde);
		}

		if (hxphys != 0 && hxphys >= xde->xd_hx_phys &&
		    hxphys < xde->xd_hx_phys + xde->xd_length) {
			return (xde);
		}
	}

	return (NULL);
}

static xdma_ent_t *
xdma_find_shadow_map(AssignedDevRegion *d, uint_t type, size_t len)
{
	xdma_ent_t *xde;

	for (xde = list_tail(&d->xdma_free_list); xde != NULL;
	    xde = list_prev(&d->xdma_free_list, xde)) {
		if (xde->xd_type == type && len <= xde->xd_total_length) {
			list_remove(&d->xdma_free_list, xde);
			return (xde);
		}
	}

	return (NULL);
}


static uint32_t
xdma_exec_alloc_map(AssignedDevRegion *d, xdma_cmd_t *xc)
{
	upci_dma_t ud;
	xdma_ent_t *xde;

	pthread_rwlock_wrlock(&d->xdma_rwlock);

	/* make sure we have space for the new allocation */
	if (xc->xc_size  == 0 ||
	    d->xdma_cur_offset + xc->xc_size > XDMA_REGION_SIZE) {
		goto out;
	}


	ud.ud_type = (xc->xc_type == XDMA_CMD_MAP_TYPE_COH) ?
	    DDI_DMA_CONSISTENT : DDI_DMA_STREAMING;
	ud.ud_write = xc->xc_dir;	/* bzero */
	ud.ud_length = xc->xc_size;
	ud.ud_rwoff = 0;
	ud.ud_host_phys = 0;
	ud.ud_udata = 0;

	if ((xde = xdma_find_shadow_map(d, xc->xc_type, xc->xc_size)) != NULL) {
		xc->xc_status = XDMA_CMD_STATUS_OK;
		xc->xc_gx_off = xde->xd_gx_off;
		xc->xc_hx_phys = xde->xd_hx_phys;

		xde->xd_flags = XDMA_ENT_FLAGS_ACTIVE;
		xde->xd_length = xc->xc_size;
		xde->xd_gb_vir = xc->xc_gb_vir;
		xde->xd_gb_phys = xc->xc_gb_phys;
		xde->xd_gb_off = xc->xc_gb_off;

		list_insert_tail(&d->xdma_list, xde);
		pthread_rwlock_unlock(&d->xdma_rwlock);
		return (0);
	}

	if (ioctl(d->region->upci_fd, UPCI_IOCTL_XDMA_ALLOC, &ud) == 0) {

		xc->xc_status = XDMA_CMD_STATUS_OK;
		xc->xc_gx_off = d->xdma_cur_offset;
		xc->xc_hx_phys = ud.ud_host_phys;

		xde = qemu_malloc(sizeof (xdma_ent_t));

		xde->xd_flags = XDMA_ENT_FLAGS_ACTIVE;
		xde->xd_type = xc->xc_type;
		xde->xd_length = xc->xc_size;
		xde->xd_total_length = xc->xc_size;
		xde->xd_bbuff = 0;
		xde->xd_gx_off = d->xdma_cur_offset;
		xde->xd_hx_phys = ud.ud_host_phys;
		xde->xd_gb_vir = xc->xc_gb_vir;
		xde->xd_gb_phys = xc->xc_gb_phys;
		xde->xd_gb_off = xc->xc_gb_off;

		/* stream maps should have bounce buffers */
		if (xc->xc_type == XDMA_CMD_MAP_TYPE_STR) {
			xde->xd_bbuff = qemu_malloc(xc->xc_size);
		}

		list_insert_tail(&d->xdma_list, xde);

		d->xdma_cur_offset += xde->xd_length;
		d->xdma_cur_offset = ROUND_UP(d->xdma_cur_offset, 4096);
		pthread_rwlock_unlock(&d->xdma_rwlock);
		return (0);
	}
out:
	fprintf(stderr, "%s: failed\n", __func__);
	xc->xc_status = XDMA_CMD_STATUS_ER;

	pthread_rwlock_unlock(&d->xdma_rwlock);
	return (1);
}

static void
xdma_free_shadow_maps(AssignedDevRegion *d)
{
	upci_dma_t ud;
	xdma_ent_t *xde, *nxde;

	/*
	 * TODO: Find a good condition to run this code.
	 * For now it is disabled.
	 */

	/* just return */
	return;

	for (xde = list_head(&d->xdma_free_list); xde != NULL; xde = nxde) {

		ud.ud_type = (xde->xd_type == XDMA_CMD_MAP_TYPE_COH) ?
		    DDI_DMA_CONSISTENT : DDI_DMA_STREAMING;
		ud.ud_write = 0;
		ud.ud_length = 0;
		ud.ud_rwoff = 0;
		ud.ud_host_phys = xde->xd_hx_phys;
		ud.ud_udata = 0;

		if (ioctl(d->region->upci_fd,
		    UPCI_IOCTL_XDMA_REMOVE, &ud) !=0) {
			return;
		}

		nxde = list_next(&d->xdma_free_list, xde);
		list_remove(&d->xdma_free_list, xde);
		qemu_free(xde->xd_bbuff);
		qemu_free(xde);
	}
}

static uint32_t
xdma_exec_remove_map(AssignedDevRegion *d, xdma_cmd_t *xc)
{
	xdma_ent_t *xde;

	pthread_rwlock_wrlock(&d->xdma_rwlock);

	xde = xdma_find_active_map(d, xc->xc_gx_off, xc->xc_hx_phys);
	if (xde != NULL) {
		list_remove(&d->xdma_list, xde);

		xde->xd_flags = XDMA_ENT_FLAGS_SHADOW;
		xde->xd_length = 0;
		list_insert_tail(&d->xdma_free_list, xde);
		xdma_free_shadow_maps(d);
		xc->xc_status = XDMA_CMD_STATUS_OK;
		pthread_rwlock_unlock(&d->xdma_rwlock);
		return (0);
	}

	fprintf(stderr, "%s: failed\n", __func__);
	xc->xc_status = XDMA_CMD_STATUS_ER;
	pthread_rwlock_unlock(&d->xdma_rwlock);
	return (1);
}

static uint32_t
xdma_exec_inquiry_map(AssignedDevRegion *d, xdma_cmd_t *xc)
{
	xdma_ent_t *xde;

	pthread_rwlock_rdlock(&d->xdma_rwlock);

	xde = xdma_find_active_map(d, xc->xc_gx_off, xc->xc_hx_phys);

	if (xde != NULL) {
		xc->xc_type = xde->xd_type;
		xc->xc_dir = 0;
		xc->xc_size = xde->xd_length;
		xc->xc_gx_off = xde->xd_gx_off;
		xc->xc_hx_phys = xde->xd_hx_phys;
		xc->xc_gb_vir = xde->xd_gb_vir;
		xc->xc_gb_phys = xde->xd_gb_phys;
		xc->xc_gb_off = xde->xd_gb_off;

		xc->xc_status = XDMA_CMD_STATUS_OK;
		pthread_rwlock_unlock(&d->xdma_rwlock);
		return (0);
	}

	fprintf(stderr, "%s: failed\n", __func__);
	xc->xc_status = XDMA_CMD_STATUS_ER;
	pthread_rwlock_unlock(&d->xdma_rwlock);
	return (1);
}

static uint32_t
xdma_exec_sync_map(AssignedDevRegion *d, xdma_cmd_t *xc)
{
	upci_dma_t ud;
	xdma_ent_t *xde;

	pthread_rwlock_rdlock(&d->xdma_rwlock);
	xde = xdma_find_active_map(d, xc->xc_gx_off, xc->xc_hx_phys);
	if (xde == NULL) {
		goto error;
	}

	if (xde->xd_gb_phys != 0 && xc->xc_dir == XDMA_CMD_SYNC_FORDEV) {
		cpu_physical_memory_read(xde->xd_gb_phys + xde->xd_gb_off,
		    xde->xd_bbuff,
		    xde->xd_length);
	}

	ud.ud_type = (xc->xc_type == XDMA_CMD_MAP_TYPE_COH) ?
	    DDI_DMA_CONSISTENT : DDI_DMA_STREAMING;
	ud.ud_write = (xc->xc_dir == XDMA_CMD_SYNC_FORCPU) ?
	    DDI_DMA_SYNC_FORCPU : DDI_DMA_SYNC_FORDEV;
	ud.ud_length = 0;
	ud.ud_rwoff = 0;
	ud.ud_host_phys = xde->xd_hx_phys;
	ud.ud_udata = (uintptr_t) (xde->xd_gb_phys? xde->xd_bbuff : NULL);

	if (ioctl(d->region->upci_fd, UPCI_IOCTL_XDMA_SYNC, &ud) == 0) {

		if (xde->xd_gb_phys != 0 &&
		    xc->xc_dir == XDMA_CMD_SYNC_FORCPU) {
			cpu_physical_memory_write(
			    xde->xd_gb_phys + xde->xd_gb_off,
			    xde->xd_bbuff,
			    xde->xd_length);
		}

		xc->xc_status = XDMA_CMD_STATUS_OK;
		pthread_rwlock_unlock(&d->xdma_rwlock);
		return (0);
	}
error:
	fprintf(stderr, "%s: failed\n", __func__);
	xc->xc_status = XDMA_CMD_STATUS_ER;
	pthread_rwlock_unlock(&d->xdma_rwlock);
	return (1);
}

static uint32_t
xdma_execute_command(AssignedDevRegion *d)
{
	xdma_cmd_t *xc;

	xc =  (xdma_cmd_t *) d->xdma_command;

	switch (xc->xc_command) {
		case XDMA_CMD_COMMAND_ALLOC:
			xdma_exec_alloc_map(d, xc);
		break;
		case XDMA_CMD_COMMAND_REMOVE:
			xdma_exec_remove_map(d, xc);
		break;
		case XDMA_CMD_COMMAND_INQUIRY:
			xdma_exec_inquiry_map(d, xc);
		break;
		case XDMA_CMD_COMMAND_SYNC:
			xdma_exec_sync_map(d, xc);
		break;
	}
	return (0);
}

static uint32_t
xdma_slow_bar_rw_common(AssignedDevRegion *d,
    target_phys_addr_t addr, uint32_t val, int len, int write) {

	upci_dma_t ud;
	xdma_ent_t *xde;

	pthread_rwlock_rdlock(&d->xdma_rwlock);

	if ((xde = xdma_find_active_map(d, addr, 0)) == NULL) {
		fprintf(stderr, "%s: failed to find the map addr = %llx\n",
		    __func__, addr);
		goto error;
	}

	ud.ud_type = 0;
	ud.ud_write = write;
	ud.ud_length = len;
	ud.ud_rwoff = (uintptr_t) addr - xde->xd_gx_off;
	ud.ud_host_phys = xde->xd_hx_phys;
	ud.ud_udata = val;

	if (ioctl(d->region->upci_fd, UPCI_IOCTL_XDMA_RW, &ud) == 0) {
		pthread_rwlock_unlock(&d->xdma_rwlock);
		return (uint32_t) ud.ud_udata;
	}
error:
	fprintf(stderr, "%s: failed\n", __func__);
	pthread_rwlock_unlock(&d->xdma_rwlock);
	return (0);
}

static uint32_t
xdma_slow_bar_read_common(AssignedDevRegion *d,
    target_phys_addr_t addr, int len) {
	return xdma_slow_bar_rw_common (d, addr, 0, len, 0);
}

static void
xdma_slow_bar_write_common(AssignedDevRegion *d,
    target_phys_addr_t addr, uint32_t val, int len) {
	xdma_slow_bar_rw_common(d, addr, val, len, 1);
}

uint32_t xdma_slow_bar_readb(AssignedDevRegion *d, target_phys_addr_t addr)
{
	if (addr < sizeof(xdma_cmd_t)) {
		return XDMA_COMM_UINT8(d, addr);
	}
	return xdma_slow_bar_read_common(d, addr, 1);
}


uint32_t xdma_slow_bar_readw(AssignedDevRegion *d, target_phys_addr_t addr)
{
	if (addr < sizeof(xdma_cmd_t)) {
		return XDMA_COMM_UINT16(d, addr);
	}
	return xdma_slow_bar_read_common(d, addr, 2);
}

uint32_t xdma_slow_bar_readl(AssignedDevRegion *d, target_phys_addr_t addr)
{
	if (addr < sizeof(xdma_cmd_t)) {
		return XDMA_COMM_UINT32(d, addr);
	}
	return xdma_slow_bar_read_common(d, addr, 4);
}


void xdma_slow_bar_writeb(AssignedDevRegion *d, target_phys_addr_t addr,
    uint32_t val)
{
	if (addr < sizeof(xdma_cmd_t)) {
		XDMA_COMM_UINT8(d, addr) = val;
		if (addr == 0) {
			xdma_execute_command(d);
		}
		return;
	}

	xdma_slow_bar_write_common(d, addr, val, 1);
}

void xdma_slow_bar_writew(AssignedDevRegion *d, target_phys_addr_t addr,
    uint32_t val)
{
	if (addr < sizeof(xdma_cmd_t)) {
		XDMA_COMM_UINT16(d, addr) = val;
		return;
	}

	xdma_slow_bar_write_common(d, addr, val, 2);
}

void xdma_slow_bar_writel(AssignedDevRegion *d, target_phys_addr_t addr,
    uint32_t val)
{
	if (addr < sizeof(xdma_cmd_t)) {
		XDMA_COMM_UINT32(d, addr) = val;
		return;
	}

	xdma_slow_bar_write_common(d, addr, val, 4);
}
