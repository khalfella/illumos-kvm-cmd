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
xdma_find_map(AssignedDevRegion *d, uint64_t gxoff, uint64_t hxphys)
{
	xdma_ent_t *xde;
	for (xde = list_head(&d->xdma_list); xde != NULL;
	    xde = list_next(&d->xdma_list, xde)) {

		if (xde->xd_flags == XDMA_ENT_FLAGS_SHADOW)
			continue;

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

	if (ioctl(d->region->upci_fd, UPCI_IOCTL_XDMA_ALLOC, &ud) == 0) {

		xc->xc_status = XDMA_CMD_STATUS_OK;
		xc->xc_gx_off = d->xdma_cur_offset;
		xc->xc_hx_phys = ud.ud_host_phys;

		xde = qemu_malloc(sizeof (xdma_ent_t));

		xde->xd_flags = XDMA_ENT_FLAGS_ACTIVE;
		xde->xd_type = xc->xc_type;
		xde->xd_length = xc->xc_size;
		xde->xd_gx_off = d->xdma_cur_offset;
		xde->xd_hx_phys = ud.ud_host_phys;
		xde->xd_gb_vir = xc->xc_gb_vir;
		xde->xd_gb_phys = xc->xc_gb_phys;
		xde->xd_gb_off = xc->xc_gb_off;

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
xdma_try_removal(AssignedDevRegion *d)
{
	xdma_ent_t *xde, *pxde;

	for (xde = list_tail(&d->xdma_list); xde != NULL; xde = pxde) {

		if (xde->xd_flags != XDMA_ENT_FLAGS_SHADOW)
			break;

		d->xdma_cur_offset -= xde->xd_length;
		d->xdma_cur_offset = ROUND_DOWN(d->xdma_cur_offset, 4096);
		pxde = list_prev(&d->xdma_list, xde);
		list_remove(&d->xdma_list, xde);
		qemu_free(xde);
	}
}

static uint32_t
xdma_exec_remove_map(AssignedDevRegion *d, xdma_cmd_t *xc)
{
	upci_dma_t ud;
	xdma_ent_t *xde;

	pthread_rwlock_wrlock(&d->xdma_rwlock);

	if ((xde = xdma_find_map(d, xc->xc_gx_off, xc->xc_hx_phys)) == NULL) {
		goto error;
	}

	ud.ud_type = (xc->xc_type == XDMA_CMD_MAP_TYPE_COH) ?
	    DDI_DMA_CONSISTENT : DDI_DMA_STREAMING;
	ud.ud_write = 0;
	ud.ud_length = 0;
	ud.ud_rwoff = 0;
	ud.ud_host_phys = xde->xd_hx_phys;
	ud.ud_udata = 0;

	if (ioctl(d->region->upci_fd, UPCI_IOCTL_XDMA_REMOVE, &ud) == 0) {
		xde->xd_flags = XDMA_ENT_FLAGS_SHADOW;
		xdma_try_removal(d);
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
xdma_exec_inquiry_map(AssignedDevRegion *d, xdma_cmd_t *xc)
{
	xdma_ent_t *xde;

	pthread_rwlock_rdlock(&d->xdma_rwlock);

	if ((xde = xdma_find_map(d, xc->xc_gx_off, xc->xc_hx_phys)) == NULL) {
		goto error;
	}

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

error:
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
	if ((xde = xdma_find_map(d, xc->xc_gx_off, xc->xc_hx_phys)) == NULL) {
		goto error;
	}

	ud.ud_type = (xc->xc_type == XDMA_CMD_MAP_TYPE_COH) ?
	    DDI_DMA_CONSISTENT : DDI_DMA_STREAMING;
	ud.ud_write = (xc->xc_dir == XDMA_CMD_SYNC_FORCPU) ?
	    DDI_DMA_SYNC_FORCPU : DDI_DMA_SYNC_FORDEV;
	ud.ud_length = 0;
	ud.ud_rwoff = 0;
	ud.ud_host_phys = xde->xd_hx_phys;
	ud.ud_udata = 0;

	if (ioctl(d->region->upci_fd, UPCI_IOCTL_XDMA_SYNC, &ud) == 0) {
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

	if ((xde = xdma_find_map(d, addr, 0)) == NULL) {
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
