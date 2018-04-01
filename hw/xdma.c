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


static uint32_t
xdma_alloc_coherent(AssignedDevRegion *d, xdma_command_t *cmd)
{
	upci_coherent_t ch;
	xdma_ch_ent_t *xhe;
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

		xhe = qemu_malloc(sizeof (xdma_ch_ent_t));
		xhe->xh_flags = flags;
		xhe->xh_length = len;
		xhe->xh_cookie = ch.ch_cookie;
		xhe->xh_virtual = d->xdma_virtual;
		list_insert_tail(&d->xdma_ch_list, xhe);

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

static uint32_t
xdma_execute_command(AssignedDevRegion *d)
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

static xdma_ch_ent_t
*xdma_find_coherent_map(AssignedDevRegion *d, uint64_t addr)
{
	xdma_ch_ent_t *xhe;
	for (xhe = list_head(&d->xdma_ch_list); xhe != NULL;
	    xhe = list_next(&d->xdma_ch_list, xhe)) {
		if (addr >= xhe->xh_virtual &&
		    addr < xhe->xh_virtual + xhe->xh_length)
			return (xhe);
	}

	return (NULL);
}

static uint32_t
xdma_slow_bar_rw_common(AssignedDevRegion *d,
    target_phys_addr_t addr, uint32_t val, int len, int write) {

	int cmd;
	upci_coherent_t ch;
	xdma_ch_ent_t *xhe;

	fprintf(stderr, "%s: addr = %llx len = %lx write = %d\n",
	    __func__, addr, len, write);

	if ((xhe = xdma_find_coherent_map(d, addr)) == NULL) {
		goto error;
	}

	fprintf(stderr, "%s: map found\n", __func__);

	ch.ch_flags = 0;
	ch.ch_length = len;
	ch.ch_cookie = xhe->xh_cookie;
	ch.ch_offset = addr - xhe->xh_virtual;
	ch.ch_udata = val;

	cmd = write ? UPCI_IOCTL_XDMA_WRITE_COHERENT :
	    UPCI_IOCTL_XDMA_READ_COHERENT;

	if (ioctl(d->region->upci_fd, cmd, &ch) == 0) {
		return (uint32_t) ch.ch_udata;
	}
error:
	fprintf(stderr, "%s: failed\n", __func__);
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
	if (addr < sizeof(xdma_command_t)) {
		return XDMA_COMM_UINT8(d, addr);
	}
	return xdma_slow_bar_read_common(d, addr, 1);
}


uint32_t xdma_slow_bar_readw(AssignedDevRegion *d, target_phys_addr_t addr)
{
	if (addr < sizeof(xdma_command_t)) {
		return XDMA_COMM_UINT16(d, addr);
	}
	return xdma_slow_bar_read_common(d, addr, 2);
}

uint32_t xdma_slow_bar_readl(AssignedDevRegion *d, target_phys_addr_t addr)
{
	if (addr < sizeof(xdma_command_t)) {
		return XDMA_COMM_UINT32(d, addr);
	}
	return xdma_slow_bar_read_common(d, addr, 4);
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

	xdma_slow_bar_write_common(d, addr, val, 1);
}

void xdma_slow_bar_writew(AssignedDevRegion *d, target_phys_addr_t addr,
    uint32_t val)
{
	if (addr < sizeof(xdma_command_t)) {
		XDMA_COMM_UINT16(d, addr) = val;
		return;
	}

	xdma_slow_bar_write_common(d, addr, val, 2);
}

void xdma_slow_bar_writel(AssignedDevRegion *d, target_phys_addr_t addr,
    uint32_t val)
{
	if (addr < sizeof(xdma_command_t)) {
		XDMA_COMM_UINT32(d, addr) = val;
		return;
	}

	xdma_slow_bar_write_common(d, addr, val, 4);
}
