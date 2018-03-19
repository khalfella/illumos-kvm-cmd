/*
 * Copyright (c) 2007, Neocleus Corporation.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms and conditions of the GNU General Public License,
 * version 2, as published by the Free Software Foundation.
 *
 * This program is distributed in the hope it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 59 Temple
 * Place - Suite 330, Boston, MA 02111-1307 USA.
 *
 *
 *  Assign a PCI device from the host to a guest VM.
 *
 *  Adapted for KVM by Qumranet.
 *
 *  Copyright (c) 2007, Neocleus, Alex Novik (alex@neocleus.com)
 *  Copyright (c) 2007, Neocleus, Guy Zana (guy@neocleus.com)
 *  Copyright (C) 2008, Qumranet, Amit Shah (amit.shah@qumranet.com)
 *  Copyright (C) 2008, Red Hat, Amit Shah (amit.shah@redhat.com)
 *  Copyright (C) 2008, IBM, Muli Ben-Yehuda (muli@il.ibm.com)
 */
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>

#include <upci.h>


#include "qemu-kvm.h"
#include "hw.h"
#include "pc.h"
#include "qemu-error.h"
#include "console.h"
#include "device-assignment.h"
#include "loader.h"
#include "monitor.h"
#include "range.h"
#include "sysemu.h"

/* From linux/ioport.h */
#define IORESOURCE_IO       0x00000100  /* Resource type */
#define IORESOURCE_MEM      0x00000200
#define IORESOURCE_IRQ      0x00000400
#define IORESOURCE_DMA      0x00000800
#define IORESOURCE_PREFETCH 0x00001000  /* No side effects */

/* #define DEVICE_ASSIGNMENT_DEBUG 1 */

#ifdef DEVICE_ASSIGNMENT_DEBUG
#define DEBUG(fmt, ...)                                       \
    do {                                                      \
      fprintf(stderr, "%s: " fmt, __func__ , __VA_ARGS__);    \
    } while (0)
#else
#define DEBUG(fmt, ...) do { } while(0)
#endif

/* upci functions */

static int
upci_open(char *upci_path)
{
	int fd;
	if (( fd = open(upci_path, O_RDWR)) == -1 ||
	    ioctl(fd, UPCI_IOCTL_OPEN, NULL) != 0 ||
	    ioctl(fd, UPCI_IOCTL_OPEN_REGS, NULL) != 0) {
		fprintf(stderr, "%s: failed to open\"%s\"\n", __func__,
		     upci_path);
		exit(1);
	}
	return (fd);
}

typedef struct puller_arg_s {
	int	pa_upci_fd;
	void	(*pa_intr_cb) (void *, int);
	void	*pa_cb_arg;
} puller_arg_t;

static void *
upci_msi_puller(void *arg)
{
	puller_arg_t *pa = (puller_arg_t *) arg;
	upci_int_get_t ig;
	while (1) {
		ig.ig_type = UPCI_INTR_TYPE_MSI;
		if (ioctl(pa->pa_upci_fd, UPCI_IOCTL_INT_GET, &ig) == 0) {
			pa->pa_intr_cb(pa->pa_cb_arg, ig.ig_number);
			fprintf(stderr, "%s: delivering msi interrupt\n",
			    __func__);
		} else {
			fprintf(stderr, "%s: ioctl != 0 msi interrupt\n",
			    __func__);
		}
	}

	free(arg);
	return (NULL);
}

static void *
upci_intx_puller(void *arg)
{
	puller_arg_t *pa = (puller_arg_t *) arg;
	upci_int_get_t ig;
	sigset_t ss;

	sigfillset(&ss);
	sigaddset(&ss, SIGINT);
	sigaddset(&ss, SIGQUIT);
	pthread_sigmask(SIG_SETMASK, &ss, NULL);

	while (1) {
		ig.ig_type = UPCI_INTR_TYPE_FIXED;
		if (ioctl(pa->pa_upci_fd, UPCI_IOCTL_INT_GET, &ig) == 0) {
			pa->pa_intr_cb(pa->pa_cb_arg, 0);
			fprintf(stderr, "%s: delivering intx interrupt\n",
			    __func__);
		} else {
			fprintf(stderr, "%s: ioctl != 0 intx interrupt\n",
			    __func__);
		}
	}

	free(arg);
	return (NULL);
}

static int
upci_start_intx_puller(int fd, void (*intx_intr_cb) (void *, int),
    void *intx_cb_arg)
{
	pthread_t intx_tid;
	puller_arg_t *intx_pa;

	if ((intx_pa = malloc(sizeof(*intx_pa))) == NULL) {
		fprintf(stderr, "%s: Failed to allocate memory\n");
		exit(1);
	}

	intx_pa->pa_upci_fd = fd;
	intx_pa->pa_intr_cb =  intx_intr_cb;
	intx_pa->pa_cb_arg = intx_cb_arg;

	pthread_create(&intx_tid, NULL, upci_intx_puller, intx_pa);

	return (0);
}

static int
upci_start_msi_puller(int fd, void (*msi_intr_cb) (void *, int),
    void *msi_cb_arg ) {

	pthread_t msi_tid;
	puller_arg_t *msi_pa;
	sigset_t ss;

	sigfillset(&ss);
	sigaddset(&ss, SIGINT);
	sigaddset(&ss, SIGQUIT);
	pthread_sigmask(SIG_SETMASK, &ss, NULL);

	if((msi_pa = malloc(sizeof(*msi_pa))) == NULL) {
		fprintf(stderr, "%s: Failed to allocate memory\n");
		exit(1);
	}

	msi_pa->pa_upci_fd = fd;
	msi_pa->pa_intr_cb = msi_intr_cb;
	msi_pa->pa_cb_arg = msi_cb_arg;

	pthread_create(&msi_tid, NULL, upci_msi_puller, msi_pa);
	return (0);
}


static int
upci_msi_update(int fd, int enable, int vcount)
{
	upci_int_update_t iu;

	iu.iu_type = UPCI_INTR_TYPE_MSI;
	iu.iu_enable = enable;
	iu.iu_vcount = vcount;

	fprintf(stderr, "%s: enable = %d, count = %d\n",
	    __func__, enable, vcount);

	if (ioctl(fd, UPCI_IOCTL_INT_UPDATE, &iu) != 0) {
		fprintf(stderr, "%s: failed\n", __func__);
		return (-1);
	}

	return (0);
}

static int
upci_intx_update(int fd, int enable)
{
	upci_int_update_t iu;

	iu.iu_type = UPCI_INTR_TYPE_FIXED;
	iu.iu_enable = enable;
	iu.iu_vcount = 0;

	fprintf(stderr, "%s: enable = %d\n", __func__, enable);

	if (ioctl(fd, UPCI_IOCTL_INT_UPDATE, &iu) != 0) {
		fprintf(stderr, "%s: failed\n", __func__);
		return (-1);
	}

	return (0);
}


static int
upci_get_dev_prop(int fd, uint32_t *nr, uint64_t *flags)
{
	upci_dev_info_t di;
	if (ioctl(fd, UPCI_IOCTL_DEV_INFO, &di) != 0) {
		fprintf(stderr, "%s: failed\n", __func__);
		return (-1);
	}

	*nr = di.di_nregs;
	*flags = di.di_flags;
	return (0);
}

static int
upci_get_reg_prop(int fd, int reg, uint64_t *base, uint64_t *size,
    uint64_t *flags)
{
	upci_reg_info_t ri;

	ri.ri_region = reg;
	if (ioctl(fd, UPCI_IOCTL_REG_INFO, &ri) != 0) {
		fprintf(stderr, "%s: failed\n", __func__);
		return (-1);
	}
	*base = ri.ri_base;
	*size = ri.ri_size;
	*flags = ri.ri_flags;
	return (0);
}

static size_t
upci_reg_rw(int fd, int reg, uint64_t off, char *buf, size_t sz, int write)
{
	upci_rw_cmd_t rw;

	if (sz != 1 && sz != 2 && sz != 4 && sz != 8) {
		fprintf(stderr, "%s: size not in (1, 2, 4, 8)\n", __func__);
		return (0);
	}

	rw.rw_region = reg;
	rw.rw_offset = off;
	rw.rw_count = sz;
	rw.rw_pdatain = (uintptr_t) buf;
	rw.rw_pdataout = (uintptr_t) buf;

	if (ioctl(fd, write ? UPCI_IOCTL_WRITE: UPCI_IOCTL_READ, &rw) != 0) {
		fprintf(stderr, "%s: io error\n", __func__);
		return (0);
	}

	unsigned char *ubuf = (unsigned char *) buf;
	switch (sz) {
		case 1:
		printf("   [%s] reg = %d off = %X data = %x\n",
		    write ? "W" : "R", reg, off, ubuf[0]);
		break;
		case 2:
		printf("   [%s] reg = %d off = %X data = %x %x\n",
		    write ? "W" : "R", reg, off, ubuf[0], ubuf[1]);
		break;
		case 4:
		printf("   [%s] reg = %d off = %X data = %x %x %x %x\n",
		    write ? "W" : "R", reg, off,
		    ubuf[0], ubuf[1], ubuf[2], ubuf[3]);
		break;
	}

	return (sz);
}

static size_t
upci_cfg_rw(int fd, uint64_t off, char *buff, size_t sz, int write)
{
	return upci_reg_rw(fd, -1, off, buff, sz, write);
}

static size_t
upci_cfg_prw(int fd, uint64_t off, char *buff, size_t sz, int write)
{
	off_t cur = 0;
	size_t bsz, osz = sz;
	while (sz > 0) {
		bsz  = (sz >= 4) ? 4 : ((sz >= 2) ? 2 : 1);
		if (upci_cfg_rw(fd, off + cur, buff + cur, bsz, write) != bsz) {
			fprintf(stderr, "%s: io error\n", __func__);
			return (0);
		}
		cur += bsz;
		sz -= bsz;
	}
	return (osz);
}


/* assigned_device_functions */



static void assigned_dev_load_option_rom(AssignedDevice *dev);

/*
static void assigned_dev_unregister_msix_mmio(AssignedDevice *dev);
*/

static void assigned_device_pci_cap_write_config(PCIDevice *pci_dev,
                                                 uint32_t address,
                                                 uint32_t val, int len);

static uint32_t assigned_device_pci_cap_read_config(PCIDevice *pci_dev,
                                                    uint32_t address, int len);

static uint32_t assigned_dev_ioport_rw(AssignedDevRegion *dev_region,
                                       uint32_t addr, int len, uint32_t *val)
{
	uint32_t ret = 0;
	uint32_t offset = addr - dev_region->e_physbase;
	int fd = dev_region->region->upci_fd;
	int r = dev_region->num;
	int write = (val != NULL);

	if (upci_reg_rw(fd, r, offset, (char *) (write ? val : &ret),
	    len, write) != len) {
		fprintf(stderr, "%s: failed in upci_reg_rw %s reg[%d]:%ld\n",
		    __func__, write ? "write" : "read", r, offset);
		exit(1);
	}
	DEBUG("in %s: op = %s reg=%d, len=%d\n",
	    __func__, write ? "write" : "read", r, len);
	return (ret);

}

static void assigned_dev_ioport_writeb(void *opaque, uint32_t addr,
                                       uint32_t value)
{
    assigned_dev_ioport_rw(opaque, addr, 1, &value);
    return;
}

static void assigned_dev_ioport_writew(void *opaque, uint32_t addr,
                                       uint32_t value)
{
    assigned_dev_ioport_rw(opaque, addr, 2, &value);
    return;
}

static void assigned_dev_ioport_writel(void *opaque, uint32_t addr,
                       uint32_t value)
{
    assigned_dev_ioport_rw(opaque, addr, 4, &value);
    return;
}

static uint32_t assigned_dev_ioport_readb(void *opaque, uint32_t addr)
{
    return assigned_dev_ioport_rw(opaque, addr, 1, NULL);
}

static uint32_t assigned_dev_ioport_readw(void *opaque, uint32_t addr)
{
    return assigned_dev_ioport_rw(opaque, addr, 2, NULL);
}

static uint32_t assigned_dev_ioport_readl(void *opaque, uint32_t addr)
{
    return assigned_dev_ioport_rw(opaque, addr, 4, NULL);
}

static uint32_t slow_bar_readb(void *opaque, target_phys_addr_t addr)
{
	AssignedDevRegion *d = opaque;
	uint32_t r = 0;
	if (upci_reg_rw(d->region->upci_fd, d->num, addr,
	    (char *) &r, 1, 0) != 1) {
		fprintf(stderr, "%s: error reading reg[%d]:%d\n",
		    __func__, d->num, addr);
		exit(1);
	}
	DEBUG("%s: addr=0x" TARGET_FMT_plx " val=0x%08x\n", __func__, addr, r);
	return (r);
}

static uint32_t slow_bar_readw(void *opaque, target_phys_addr_t addr)
{
	AssignedDevRegion *d = opaque;
	uint32_t r = 0;
	if (upci_reg_rw(d->region->upci_fd, d->num, addr,
	    (char *) &r, 2, 0) != 2) {
		fprintf(stderr, "%s: error reading reg[%d]:%d\n",
		    __func__, d->num, addr);
		exit(1);
	}
	DEBUG("%s: addr=0x" TARGET_FMT_plx " val=0x%08x\n", __func__, addr, r);
	return (r);
}

static uint32_t slow_bar_readl(void *opaque, target_phys_addr_t addr)
{
	AssignedDevRegion *d = opaque;
	uint32_t r = 0;
	if (upci_reg_rw(d->region->upci_fd, d->num, addr,
	    (char *) &r, 4, 0) != 4) {
		fprintf(stderr, "%s: error reading reg[%d]:%d\n",
		    __func__, d->num, addr);
		exit(1);
	}
	DEBUG("%s: addr=0x" TARGET_FMT_plx " val=0x%08x\n", __func__, addr, r);
	return (r);
}

static void slow_bar_writeb(void *opaque, target_phys_addr_t addr, uint32_t val)
{
	AssignedDevRegion *d = opaque;
	if (upci_reg_rw(d->region->upci_fd, d->num, addr,
	    (char *) &val, 1, 1) != 1) {
		fprintf(stderr, "%s: error reading reg[%d]:%d\n",
		    __func__, d->num, addr);
		exit(1);
	}
	DEBUG("%s: addr=0x" TARGET_FMT_plx " val=0x%08x\n", __func__, addr, r);
}

static void slow_bar_writew(void *opaque, target_phys_addr_t addr, uint32_t val)
{
	AssignedDevRegion *d = opaque;
	if (upci_reg_rw(d->region->upci_fd, d->num, addr,
	     (char *) &val, 2, 1) != 2) {
		fprintf(stderr, "%s: error reading reg[%d]:%d\n",
		    __func__, d->num, addr);
		exit(1);
	}
	DEBUG("%s: addr=0x" TARGET_FMT_plx " val=0x%08x\n", __func__, addr, r);
}

static void slow_bar_writel(void *opaque, target_phys_addr_t addr, uint32_t val)
{
	AssignedDevRegion *d = opaque;
	if (upci_reg_rw(d->region->upci_fd, d->num, addr,
	    (char *) &val, 4, 1) != 4) {
		fprintf(stderr, "%s: error reading reg[%d]:%d\n",
		    __func__, d->num, addr);
		exit(1);
	}
	DEBUG("%s: addr=0x" TARGET_FMT_plx " val=0x%08x\n", __func__, addr, r);
}

static CPUWriteMemoryFunc * const slow_bar_write[] = {
    &slow_bar_writeb,
    &slow_bar_writew,
    &slow_bar_writel
};

static CPUReadMemoryFunc * const slow_bar_read[] = {
    &slow_bar_readb,
    &slow_bar_readw,
    &slow_bar_readl
};

static void assigned_dev_iomem_map_slow(PCIDevice *pci_dev, int region_num,
                                        pcibus_t e_phys, pcibus_t e_size,
                                        int type)
{
	AssignedDevice *r_dev = container_of(pci_dev, AssignedDevice, dev);
	AssignedDevRegion *region = &r_dev->v_addrs[region_num];
	int m;

	DEBUG("%s", "slow map\n");
	m = cpu_register_io_memory(slow_bar_read, slow_bar_write, region,
	    DEVICE_NATIVE_ENDIAN);
	cpu_register_physical_memory(e_phys, e_size, m);

	/* MSI-X MMIO page exception code goes here*/
	/* We don't support MSI-X yet */
}

static void assigned_dev_ioport_map(PCIDevice *pci_dev, int region_num,
                                    pcibus_t addr, pcibus_t size, int type)
{
    AssignedDevice *r_dev = container_of(pci_dev, AssignedDevice, dev);
    AssignedDevRegion *region = &r_dev->v_addrs[region_num];

    region->e_physbase = addr;
    region->e_size = size;

    DEBUG("e_phys=0x%" FMT_PCIBUS " r_baseport=%x type=0x%x len=%" FMT_PCIBUS " region_num=%d \n",
          addr, region->u.r_baseport, type, size, region_num);

    register_ioport_read(addr, size, 1, assigned_dev_ioport_readb,
                         (r_dev->v_addrs + region_num));
    register_ioport_read(addr, size, 2, assigned_dev_ioport_readw,
                         (r_dev->v_addrs + region_num));
    register_ioport_read(addr, size, 4, assigned_dev_ioport_readl,
                         (r_dev->v_addrs + region_num));
    register_ioport_write(addr, size, 1, assigned_dev_ioport_writeb,
                          (r_dev->v_addrs + region_num));
    register_ioport_write(addr, size, 2, assigned_dev_ioport_writew,
                          (r_dev->v_addrs + region_num));
    register_ioport_write(addr, size, 4, assigned_dev_ioport_writel,
                          (r_dev->v_addrs + region_num));
}

static uint32_t assigned_dev_pci_read(PCIDevice *d, int pos, int len)
{
	AssignedDevice *pci_dev = container_of(d, AssignedDevice, dev);
	uint32_t val = 0;
	int fd = pci_dev->real_device.upci_fd;

	if (upci_cfg_rw(fd, pos,  (char *) &val, len, 0) != len) {
		fprintf(stderr, "%s: pci_cfg_rw failed\n", __func__);
		exit(1);
	}
	return (val);
}

static uint8_t assigned_dev_pci_read_byte(PCIDevice *d, int pos)
{
    return (uint8_t)assigned_dev_pci_read(d, pos, 1);
}

static void assigned_dev_pci_write(PCIDevice *d, int pos, uint32_t val, int len)
{
	AssignedDevice *pci_dev = container_of(d, AssignedDevice, dev);
	int fd = pci_dev->real_device.upci_fd;

	if (upci_cfg_rw(fd, pos,  (char *) &val, len, 1) != len) {
		fprintf(stderr, "%s: pci_cfg_rw failed\n", __func__);
		exit(1);
	}
}

static uint8_t pci_find_cap_offset(PCIDevice *d, uint8_t cap, uint8_t start)
{
    int id;
    int max_cap = 48;
    int pos = start ? start : PCI_CAPABILITY_LIST;
    int status;

    status = assigned_dev_pci_read_byte(d, PCI_STATUS);
    if ((status & PCI_STATUS_CAP_LIST) == 0)
        return 0;

    while (max_cap--) {
        pos = assigned_dev_pci_read_byte(d, pos);
        if (pos < 0x40)
            break;

        pos &= ~3;
        id = assigned_dev_pci_read_byte(d, pos + PCI_CAP_LIST_ID);

        if (id == 0xff)
            break;
        if (id == cap)
            return pos;

        pos += PCI_CAP_LIST_NEXT;
    }
    return 0;
}

static int assign_irq(AssignedDevice *dev);

static void assigned_dev_pci_write_config(PCIDevice *d, uint32_t address,
                                          uint32_t val, int len)
{
	int fd;
	AssignedDevice *pci_dev = container_of(d, AssignedDevice, dev);

	printf("%s: (%x.%x): address=%04x val=0x%08x len=%d\n",
	    __func__, ((d->devfn >> 3) & 0x1F), (d->devfn & 0x7),
	    (uint16_t) address, val, len);

	if (address >= PCI_CONFIG_HEADER_SIZE && d->config_map[address]) {
		printf("write goes to capability write\n");
		return assigned_device_pci_cap_write_config(d, address,
		    val, len);
	}

	if (address == 0x4) {
		printf("setting the command register\n");
		pci_default_write_config(d, address, val, len);
		/* Continue to program the card */
	}

	if ((address >= 0x10 && address <= 0x24) || address == 0x30 ||
	    address == 0x34 || address == 0x3c || address == 0x3d) {
		/* used for update-mappings (BAR emulation) */
		printf("updating bar\n");
		pci_default_write_config(d, address, val, len);

		/* update gues irq if necessary */
		if (address == 0x3c || address == 0x3d)
			assign_irq(pci_dev);
		return;
	}

	/* Write is going straight to the device */
	printf("write is going straight to the device\n");
	DEBUG("NON BAR (%x.%x): address=%04x val=0x%08x len=%d\n",
	    ((d->devfn >> 3) & 0x1F), (d->devfn & 0x7),
	    (uint16_t) address, val, len);

	fd = pci_dev->real_device.upci_fd;

	if (upci_cfg_rw(fd, address, (char *) &val, len, 1) != len) {
		fprintf(stderr, "%s: upci_cfg_rw failed\n",
		    __func__);
		exit(1);
	}
}

static uint32_t assigned_dev_pci_read_config(PCIDevice *d, uint32_t address,
                                             int len)
{
    uint32_t val = 0;
    int fd;
    AssignedDevice *pci_dev = container_of(d, AssignedDevice, dev);

        printf("%s:(%x.%x): address=%04x val=0x%08x len=%d\n",
              __func__, (d->devfn >> 3) & 0x1F, (d->devfn & 0x7), address, val, len);

    if (address >= PCI_CONFIG_HEADER_SIZE && d->config_map[address]) {
        val = assigned_device_pci_cap_read_config(d, address, len);
        DEBUG("(%x.%x): address=%04x val=0x%08x len=%d\n",
              (d->devfn >> 3) & 0x1F, (d->devfn & 0x7), address, val, len);
        return val;
    }

    if (address < 0x4 || (pci_dev->need_emulate_cmd && address == 0x4) ||
	(address >= 0x10 && address <= 0x24) || address == 0x30 ||
        address == 0x34 || address == 0x3c || address == 0x3d) {
        val = pci_default_read_config(d, address, len);
        DEBUG("(%x.%x): address=%04x val=0x%08x len=%d\n",
              (d->devfn >> 3) & 0x1F, (d->devfn & 0x7), address, val, len);
        return val;
    }

    /* vga specific, remove later */
    if (address == 0xFC)
        goto do_log;

    fd = pci_dev->real_device.upci_fd;

	if (upci_cfg_rw(fd, address, (char *) &val, len, 0) != len) {
		fprintf(stderr, "%s: upci_cfg_rw failed\n", __func__);
		exit(1);
	}

do_log:
    DEBUG("(%x.%x): address=%04x val=0x%08x len=%d\n",
          (d->devfn >> 3) & 0x1F, (d->devfn & 0x7), address, val, len);

    if (!pci_dev->cap.available) {
        /* kill the special capabilities */
        if (address == 4 && len == 4)
            val &= ~0x100000;
        else if (address == 6)
            val &= ~0x10;
    }

    return val;
}

static int
assigned_dev_register_regions(PCIRegion *io_regions,
    unsigned long regions_num, AssignedDevice *pci_dev)
{

	uint32_t i;
	PCIRegion *cur_region = io_regions;

	for (i = 0; i < regions_num; i++, cur_region++) {

		if (!(cur_region->flags & UPCI_IO_REG_VALID))
			continue;

		pci_dev->v_addrs[i].num = i;

		/* handle memory io regions */
		if (!(cur_region->flags & UPCI_IO_REG_IO)) {

			int slow_map = 1;
			int t = cur_region->flags & UPCI_IO_REG_PREFETCH
			    ? PCI_BASE_ADDRESS_MEM_PREFETCH
			    : PCI_BASE_ADDRESS_SPACE_MEMORY;

			if (cur_region->size & 0xFFF) {
				fprintf(stderr, "PCI region %d at address 0x%llx "
				    "has size 0x%x, which is not a multiple of 4K. "
				    "You might experience some performance hit "
				    "due to that.\n",
				    i, (unsigned long long)cur_region->base_addr,
				    cur_region->size);
				slow_map = 1;
			}

			/* map physical memory */
			pci_dev->v_addrs[i].e_physbase = cur_region->base_addr;


			/* Don't map the memory. We don't suppor that yet */
			pci_dev->v_addrs[i].u.r_virtbase = NULL;
			pci_dev->v_addrs[i].r_size = cur_region->size;
			pci_dev->v_addrs[i].e_size = 0;

			/* So far, only slow_map is working */
			if (slow_map) {
				pci_dev->v_addrs[i].memory_index = 0;
				pci_register_bar((PCIDevice *) pci_dev, i,
				    cur_region->size, t,
				    assigned_dev_iomem_map_slow);
			}

			/* May be we don't need this statement */
			continue;

		} else {

			/* handle port io regions */

			pci_dev->v_addrs[i].e_physbase = cur_region->base_addr;
			pci_dev->v_addrs[i].u.r_baseport = cur_region->base_addr;
			pci_dev->v_addrs[i].r_size = cur_region->size;
			pci_dev->v_addrs[i].e_size = 0;

			pci_register_bar((PCIDevice *) pci_dev, i,
			    cur_region->size, PCI_BASE_ADDRESS_SPACE_IO,
			    assigned_dev_ioport_map);

			 /* not relevant for port io */
			pci_dev->v_addrs[i].memory_index = 0;
		}
	}

	/* success */
	return (0);
}

static int get_real_device(AssignedDevice *pci_dev, char *upci_path)
{

	int r;
	uint64_t flags;
    	PCIRegion *rp;
    	PCIDevRegions *dev = &pci_dev->real_device;


	size_t sz;
	uint32_t nr;

    	dev->region_number = 0;

    	if (pci_dev->configfd_name && *pci_dev->configfd_name) {
		fprintf(stderr, "%s: configfd_name is"
		    " not supported: %m\n", __func__);
		return (1);
	}

	if ((dev->upci_fd = upci_open(pci_dev->host.upci_path)) == -1) {
		fprintf(stderr, "%s: failed to open upci_path: %m\n", __func__);
		return (1);
	} 


	sz = pci_config_size(&pci_dev->dev);
	if(upci_cfg_prw(dev->upci_fd, 0, (char *) pci_dev->dev.config, sz, 0) != sz) {
		fprintf(stderr, "%s: failed to read pci cfg\n", __func__);
		return (1);
	}

	/*
	 * Clear host resource mapping info.  If we choose not to register a
	 * BAR, such as might be the case with the option ROM, we can get
	 * confusing, unwritable, residual addresses from the host here.
	 */


	memset(&pci_dev->dev.config[PCI_BASE_ADDRESS_0], 0, 24);
	memset(&pci_dev->dev.config[PCI_ROM_ADDRESS], 0, 4);

	if (upci_get_dev_prop(dev->upci_fd, &nr, &flags) != 0) {
		fprintf(stderr, "%s: failed to read pci prop\n", __func__);
		return (1);
	}

	if (nr > PCI_ROM_SLOT) {
		fprintf(stderr, "%s: upci dev has too many regions\n", __func__);
		return (1);
	}

    	for (r = 0; r < nr; r++) {
		rp = dev->regions + r;


		if (upci_get_reg_prop(dev->upci_fd, r,
		    &rp->base_addr, &rp->size, &rp->flags) != 0) {
			fprintf(stderr, "%s: failed to get reg[%d] prop\n",
			    __func__, r);
			return (1);
		}

		if (rp->flags & UPCI_IO_REG_VALID) {
			rp->rn = r;
			rp->upci_fd = dev->upci_fd;
			pci_dev->v_addrs[r].region = rp;
			DEBUG("region[%d] size x%"PRIx64
			    " base_addr 0x%"PRIx64" flags x%"PRIx64" \n",
			    r, rp->size, rp->base_addr, rp->flags);
		} else {
			fprintf(stderr, "%s: Invalid region %d\n",
			    __func__, r);
		}
	}


	/* read and fill vendor ID */
	if(upci_cfg_rw(dev->upci_fd, 0, (char *) &pci_dev->dev.config[0],
	    2, 0) != 2) {
		fprintf(stderr, "%s: failed to read vendor id\n", __func__);
		return (1);
	}

	/* read and fill device ID */
	if(upci_cfg_rw(dev->upci_fd, 2, (char *) &pci_dev->dev.config[2],
	    2, 0) != 2) {
		fprintf(stderr, "%s: failed to read device id\n", __func__);
		return (1);
	}

	dev->region_number = r;

	/*
	 * TODO: Check for virtual functions and report error
	 */
	return (0);
}

static QLIST_HEAD(, AssignedDevice) devs = QLIST_HEAD_INITIALIZER(devs);

/*
static void free_dev_irq_entries(AssignedDevice *dev)
{
}
*/

static void free_assigned_device(AssignedDevice *dev)
{
}


static int assign_device(AssignedDevice *dev)
{
	return (0);
}

static int assign_irq(AssignedDevice *dev)
{
	int irq;
	/*
	 * short circuit this for now
	 */
	return (0);


	/* Interrupt PIN 0 means don't use INTx */
	if (assigned_dev_pci_read_byte(&dev->dev, PCI_INTERRUPT_PIN) == 0) {
		upci_intx_update(dev->real_device.upci_fd, 0);
		return 0;
	}

	irq = pci_map_irq(&dev->dev, dev->intpin);
	dev->girq = piix_get_irq(irq);
	upci_intx_update(dev->real_device.upci_fd, 1);

    return (0);
}

static void deassign_device(AssignedDevice *dev)
{
}

#if 0
AssignedDevInfo *get_assigned_device(int pcibus, int slot)
{
    AssignedDevice *assigned_dev = NULL;
    AssignedDevInfo *adev = NULL;

    QLIST_FOREACH(adev, &adev_head, next) {
        assigned_dev = adev->assigned_dev;
        if (pci_bus_num(assigned_dev->dev.bus) == pcibus &&
            PCI_SLOT(assigned_dev->dev.devfn) == slot)
            return adev;
    }

    return NULL;
}
#endif

/* The pci config space got updated. Check if irq numbers have changed
 * for our devices
 */
void assigned_dev_update_irqs(void)
{
    AssignedDevice *dev, *next;
    int r;

    dev = QLIST_FIRST(&devs);
    while (dev) {
        next = QLIST_NEXT(dev, next);
        r = assign_irq(dev);
        if (r < 0)
            qdev_unplug(&dev->dev.qdev);
        dev = next;
    }
}

static void
assigned_dev_intx_intr_cb(void *arg, int dummy)
{
	AssignedDevice *dev = (AssignedDevice *) arg;
	qemu_set_irq(dev->dev.irq[0], 1);
	qemu_set_irq(dev->dev.irq[0], 0);
	fprintf(stderr, "%s, intx fired qirq = %d\n", dev->girq);
}

static void
assigned_dev_msi_intr_cb(void *arg, int vector)
{
	AssignedDevice *dev = (AssignedDevice *) arg;
	fprintf(stderr, "%s: address = %X data = %x\n",
	    __func__, dev->msi_address_lo, dev->msi_data);

	kvm_set_irq(dev->entry->gsi, 1, NULL);
}

static void assigned_dev_update_msi(PCIDevice *pci_dev, unsigned int ctrl_pos)
{
	int vcount, gsi;
	uint8_t ctrl_byte;
    	AssignedDevice *assigned_dev;

	ctrl_byte = pci_dev->config[ctrl_pos];
	assigned_dev = container_of(pci_dev, AssignedDevice, dev);
	vcount = 0;

	if (ctrl_byte & PCI_MSI_FLAGS_ENABLE) {
		int pos = ctrl_pos - PCI_MSI_FLAGS;

		if (kvm_del_routing_entry(assigned_dev->entry) < 0) {
			fprintf(stderr, "%s: failed to del irq entry\n");
			exit(1);
		}

		assigned_dev->entry->u.msi.address_lo = 
		    pci_get_long(pci_dev->config + pos + PCI_MSI_ADDRESS_LO);
		assigned_dev->entry->u.msi.address_hi = 0;
		assigned_dev->entry->u.msi.data = 
		    pci_get_word(pci_dev->config + pos + PCI_MSI_DATA_32);
		assigned_dev->entry->type = KVM_IRQ_ROUTING_MSI;

		if ((gsi = kvm_get_irq_route_gsi()) < 0) {
			fprintf(stderr, "%s: failed to get irq gsi\n");
			exit(1);
		}

		assigned_dev->entry->gsi = gsi;

		if (kvm_add_routing_entry(assigned_dev->entry) < 0) {
			fprintf(stderr, "%s: failed to add irq entry\n");
			exit(1);
		}

		if (kvm_commit_irq_routes() < 0) {
			fprintf(stderr, "%s: failed to commit routes\n");
			exit(1);
		}

		vcount = pci_get_word(pci_dev->config + pos + PCI_MSI_FLAGS);
		vcount &= PCI_MSI_FLAGS_QSIZE;
		vcount >>= 4;
		vcount = 1 << vcount;
	}

	upci_msi_update(assigned_dev->real_device.upci_fd,
	    ctrl_byte & PCI_MSI_FLAGS_ENABLE ? 1 : 0,
	    vcount);
}

/* There can be multiple VNDR capabilities per device, we need to find the
 * one that starts closet to the given address without going over. */
static uint8_t find_vndr_start(PCIDevice *pci_dev, uint32_t address)
{
    uint8_t cap, pos;

    for (cap = pos = 0;
         (pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_VNDR, pos));
         pos += PCI_CAP_LIST_NEXT) {
        if (pos <= address) {
            cap = MAX(pos, cap);
        }
    }
    return cap;
}

/* Merge the bits set in mask from mval into val.  Both val and mval are
 * at the same addr offset, pos is the starting offset of the mask. */
static uint32_t merge_bits(uint32_t val, uint32_t mval, uint8_t addr,
                           int len, uint8_t pos, uint32_t mask)
{
    if (!ranges_overlap(addr, len, pos, 4)) {
        return val;
    }

    if (addr >= pos) {
        mask >>= (addr - pos) * 8;
    } else {
        mask <<= (pos - addr) * 8;
    }
    mask &= 0xffffffffU >> (4 - len) * 8;

    val &= ~mask;
    val |= (mval & mask);

    return val;
}

static uint32_t assigned_device_pci_cap_read_config(PCIDevice *pci_dev,
                                                    uint32_t address, int len)
{
    uint8_t cap, cap_id = pci_dev->config_map[address];
    uint32_t val;

    switch (cap_id) {

    case PCI_CAP_ID_VPD:
        cap = pci_find_capability(pci_dev, cap_id);
        val = assigned_dev_pci_read(pci_dev, address, len);
        return merge_bits(val, pci_get_long(pci_dev->config + address),
                          address, len, cap + PCI_CAP_LIST_NEXT, 0xff);

    case PCI_CAP_ID_VNDR:
        cap = find_vndr_start(pci_dev, address);
        val = assigned_dev_pci_read(pci_dev, address, len);
        return merge_bits(val, pci_get_long(pci_dev->config + address),
                          address, len, cap + PCI_CAP_LIST_NEXT, 0xff);
    }

    return pci_default_read_config(pci_dev, address, len);
}

static void assigned_device_pci_cap_write_config(PCIDevice *pci_dev,
                                                 uint32_t address,
                                                 uint32_t val, int len)
{
	uint8_t cap_id = pci_dev->config_map[address];
	pci_default_write_config(pci_dev, address, val, len);

	switch (cap_id) {
	case PCI_CAP_ID_MSI:
		printf("This is MSI write capid = %x\n", cap_id);
		uint8_t cap = pci_find_capability(pci_dev, cap_id);
		if (ranges_overlap(address - cap, len, PCI_MSI_FLAGS, 1)) {
			assigned_dev_update_msi(pci_dev, cap + PCI_MSI_FLAGS);
		}
	break;
	case PCI_CAP_ID_MSIX:
		fprintf(stderr, "MSIX is not supported yet!\n");
		exit(1);
	break;

	case PCI_CAP_ID_VPD:
	case PCI_CAP_ID_VNDR:
		printf("    [Hitting] This is VPD/VNDR write capid = %x\n", cap_id);
		assigned_dev_pci_write(pci_dev, address, val, len);
	break;
    }
}

static int assigned_device_pci_cap_init(PCIDevice *pci_dev)
{
	AssignedDevice *dev = container_of(pci_dev, AssignedDevice, dev);
	int ret, pos, gsi;

	/* Clear initial capabilities pointer and status copied from hw */
	pci_set_byte(pci_dev->config + PCI_CAPABILITY_LIST, 0);
	pci_set_word(pci_dev->config + PCI_STATUS,
	    pci_get_word(pci_dev->config + PCI_STATUS) &
	    ~PCI_STATUS_CAP_LIST);

	/*
	 * Expose MSI capability
	 * MSI capability is the 1st capability in capability config
	 */

	if ((pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_MSI, 0))) {
		dev->cap.available |= ASSIGNED_DEVICE_CAP_MSI;
		/* Only 32-bit/no-mask currently supported */
		if ((ret = pci_add_capability(pci_dev,
		    PCI_CAP_ID_MSI, pos, 10)) < 0) {
			return ret;
		}

		upci_start_msi_puller(dev->real_device.upci_fd,
		    assigned_dev_msi_intr_cb,
		    dev);

		pci_set_word(pci_dev->config + pos + PCI_MSI_FLAGS,
		    pci_get_word(pci_dev->config + pos + PCI_MSI_FLAGS) &
		    PCI_MSI_FLAGS_QMASK);

		pci_set_long(pci_dev->config + pos + PCI_MSI_ADDRESS_LO, 0);
		pci_set_word(pci_dev->config + pos + PCI_MSI_DATA_32, 0);

		/* Set writable fields */
		pci_set_word(pci_dev->wmask + pos + PCI_MSI_FLAGS,
		    PCI_MSI_FLAGS_QSIZE | PCI_MSI_FLAGS_ENABLE);
		pci_set_long(pci_dev->wmask + pos + PCI_MSI_ADDRESS_LO,
		    0xfffffffc);
		pci_set_word(pci_dev->wmask + pos + PCI_MSI_DATA_32, 0xffff);

		/* allocate one irq entry for now */
		dev->entry = calloc(1, sizeof(struct kvm_irq_routing_entry));
		dev->irq_entries_nr = 1;

		/* Initialize the entry */
		dev->entry->u.msi.address_lo = 0;
		dev->entry->u.msi.address_hi = 0;
		dev->entry->u.msi.data = 0;
		dev->entry->type = KVM_IRQ_ROUTING_MSI;
		if ((gsi = kvm_get_irq_route_gsi()) < 0) {
			fprintf(stderr, "%s: failed to get irq gsi\n");
			exit(1);
		}

		dev->entry->gsi = gsi;
		kvm_add_routing_entry(dev->entry);
	}

	/*
	 * Minimal PM support, nothing writable, device appears to NAK changes
	 */

	if ((pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_PM, 0))) {
		uint16_t pmc;
		if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_PM, pos,
		    PCI_PM_SIZEOF)) < 0) {
			return ret;
		}

		pmc = pci_get_word(pci_dev->config + pos + PCI_CAP_FLAGS);
		pmc &= (PCI_PM_CAP_VER_MASK | PCI_PM_CAP_DSI);
		pci_set_word(pci_dev->config + pos + PCI_CAP_FLAGS, pmc);

		/*
		 * assign_device will bring the device up to D0, so
		 * we don't need to worry about doing that ourselves here.
		 */
		pci_set_word(pci_dev->config + pos + PCI_PM_CTRL,
		    PCI_PM_CTRL_NO_SOFT_RESET);

		pci_set_byte(pci_dev->config + pos + PCI_PM_PPB_EXTENSIONS, 0);
		pci_set_byte(pci_dev->config + pos + PCI_PM_DATA_REGISTER, 0);
	}

    if ((pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_EXP, 0))) {
        uint8_t version;
        uint16_t type, devctl, lnkcap, lnksta;
        uint32_t devcap;
        int size = 0x3c; /* version 2 size */

        version = pci_get_byte(pci_dev->config + pos + PCI_EXP_FLAGS);
        version &= PCI_EXP_FLAGS_VERS;
        if (version == 1) {
            size = 0x14;
        } else if (version > 2) {
            fprintf(stderr, "Unsupported PCI express capability version %d\n",
                    version);
            return -EINVAL;
        }

        if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_EXP,
                                      pos, size)) < 0) {
            return ret;
        }

        type = pci_get_word(pci_dev->config + pos + PCI_EXP_FLAGS);
        type = (type & PCI_EXP_FLAGS_TYPE) >> 8;
        if (type != PCI_EXP_TYPE_ENDPOINT &&
            type != PCI_EXP_TYPE_LEG_END && type != PCI_EXP_TYPE_RC_END) {
            fprintf(stderr,
                    "Device assignment only supports endpoint assignment, "
                    "device type %d\n", type);
            return -EINVAL;
        }

        /* capabilities, pass existing read-only copy
         * PCI_EXP_FLAGS_IRQ: updated by hardware, should be direct read */

        /* device capabilities: hide FLR */
        devcap = pci_get_long(pci_dev->config + pos + PCI_EXP_DEVCAP);
        devcap &= ~PCI_EXP_DEVCAP_FLR;
        pci_set_long(pci_dev->config + pos + PCI_EXP_DEVCAP, devcap);

        /* device control: clear all error reporting enable bits, leaving
         *                 leaving only a few host values.  Note, these are
         *                 all writable, but not passed to hw.
         */
        devctl = pci_get_word(pci_dev->config + pos + PCI_EXP_DEVCTL);
        devctl = (devctl & (PCI_EXP_DEVCTL_READRQ | PCI_EXP_DEVCTL_PAYLOAD)) |
                  PCI_EXP_DEVCTL_RELAX_EN | PCI_EXP_DEVCTL_NOSNOOP_EN;
        pci_set_word(pci_dev->config + pos + PCI_EXP_DEVCTL, devctl);
        devctl = PCI_EXP_DEVCTL_BCR_FLR | PCI_EXP_DEVCTL_AUX_PME;
        pci_set_word(pci_dev->wmask + pos + PCI_EXP_DEVCTL, ~devctl);

        /* Clear device status */
        pci_set_word(pci_dev->config + pos + PCI_EXP_DEVSTA, 0);

        /* Link capabilities, expose links and latencues, clear reporting */
        lnkcap = pci_get_word(pci_dev->config + pos + PCI_EXP_LNKCAP);
        lnkcap &= (PCI_EXP_LNKCAP_SLS | PCI_EXP_LNKCAP_MLW |
                   PCI_EXP_LNKCAP_ASPMS | PCI_EXP_LNKCAP_L0SEL |
                   PCI_EXP_LNKCAP_L1EL);
        pci_set_word(pci_dev->config + pos + PCI_EXP_LNKCAP, lnkcap);
        pci_set_word(pci_dev->wmask + pos + PCI_EXP_LNKCAP,
                     PCI_EXP_LNKCTL_ASPMC | PCI_EXP_LNKCTL_RCB |
                     PCI_EXP_LNKCTL_CCC | PCI_EXP_LNKCTL_ES |
                     PCI_EXP_LNKCTL_CLKREQ_EN | PCI_EXP_LNKCTL_HAWD);

        /* Link control, pass existing read-only copy.  Should be writable? */

        /* Link status, only expose current speed and width */
        lnksta = pci_get_word(pci_dev->config + pos + PCI_EXP_LNKSTA);
        lnksta &= (PCI_EXP_LNKSTA_CLS | PCI_EXP_LNKSTA_NLW);
        pci_set_word(pci_dev->config + pos + PCI_EXP_LNKSTA, lnksta);

        if (version >= 2) {
            /* Slot capabilities, control, status - not needed for endpoints */
            pci_set_long(pci_dev->config + pos + PCI_EXP_SLTCAP, 0);
            pci_set_word(pci_dev->config + pos + PCI_EXP_SLTCTL, 0);
            pci_set_word(pci_dev->config + pos + PCI_EXP_SLTSTA, 0);

            /* Root control, capabilities, status - not needed for endpoints */
            pci_set_word(pci_dev->config + pos + PCI_EXP_RTCTL, 0);
            pci_set_word(pci_dev->config + pos + PCI_EXP_RTCAP, 0);
            pci_set_long(pci_dev->config + pos + PCI_EXP_RTSTA, 0);

            /* Device capabilities/control 2, pass existing read-only copy */
            /* Link control 2, pass existing read-only copy */
        }
    }

    if ((pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_PCIX, 0))) {
        uint16_t cmd;
        uint32_t status;

        /* Only expose the minimum, 8 byte capability */
        if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_PCIX, pos, 8)) < 0) {
            return ret;
        }

        /* Command register, clear upper bits, including extended modes */
        cmd = pci_get_word(pci_dev->config + pos + PCI_X_CMD);
        cmd &= (PCI_X_CMD_DPERR_E | PCI_X_CMD_ERO | PCI_X_CMD_MAX_READ |
                PCI_X_CMD_MAX_SPLIT);
        pci_set_word(pci_dev->config + pos + PCI_X_CMD, cmd);

        /* Status register, update with emulated PCI bus location, clear
         * error bits, leave the rest. */
        status = pci_get_long(pci_dev->config + pos + PCI_X_STATUS);
        status &= ~(PCI_X_STATUS_BUS | PCI_X_STATUS_DEVFN);
        status |= (pci_bus_num(pci_dev->bus) << 8) | pci_dev->devfn;
        status &= ~(PCI_X_STATUS_SPL_DISC | PCI_X_STATUS_UNX_SPL |
                    PCI_X_STATUS_SPL_ERR);
        pci_set_long(pci_dev->config + pos + PCI_X_STATUS, status);
    }

    if ((pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_VPD, 0))) {
        /* Direct R/W passthrough */
        if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_VPD, pos, 8)) < 0) {
            return ret;
        }
    }

    /* Devices can have multiple vendor capabilities, get them all */
    for (pos = 0; (pos = pci_find_cap_offset(pci_dev, PCI_CAP_ID_VNDR, pos));
        pos += PCI_CAP_LIST_NEXT) {
        uint8_t len = pci_get_byte(pci_dev->config + pos + PCI_CAP_FLAGS);
        /* Direct R/W passthrough */
        if ((ret = pci_add_capability(pci_dev, PCI_CAP_ID_VNDR,
                                      pos, len)) < 0) {
            return ret;
        }
    }

    return 0;
}

/*
static uint32_t msix_mmio_readl(void *opaque, target_phys_addr_t addr)
{
    AssignedDevice *adev = opaque;
    unsigned int offset = addr & 0xfff;
    void *page = adev->msix_table_page;
    uint32_t val = 0;

    memcpy(&val, (void *)((char *)page + offset), 4);

    return val;
}
*/

/*
static uint32_t msix_mmio_readb(void *opaque, target_phys_addr_t addr)
{
    return ((msix_mmio_readl(opaque, addr & ~3)) >>
            (8 * (addr & 3))) & 0xff;
}
*/

/*
static uint32_t msix_mmio_readw(void *opaque, target_phys_addr_t addr)
{
    return ((msix_mmio_readl(opaque, addr & ~3)) >>
            (8 * (addr & 3))) & 0xffff;
}
*/

/*
static void msix_mmio_writel(void *opaque,
                             target_phys_addr_t addr, uint32_t val)
{
    AssignedDevice *adev = opaque;
    unsigned int offset = addr & 0xfff;
    void *page = adev->msix_table_page;

    DEBUG("write to MSI-X entry table mmio offset 0x%lx, val 0x%x\n",
		    addr, val);
    memcpy((void *)((char *)page + offset), &val, 4);
}
*/

/*
static void msix_mmio_writew(void *opaque,
                             target_phys_addr_t addr, uint32_t val)
{
    msix_mmio_writel(opaque, addr & ~3,
                     (val & 0xffff) << (8*(addr & 3)));
}

static void msix_mmio_writeb(void *opaque,
                             target_phys_addr_t addr, uint32_t val)
{
    msix_mmio_writel(opaque, addr & ~3,
                     (val & 0xff) << (8*(addr & 3)));
}
*/

/*
static CPUWriteMemoryFunc *msix_mmio_write[] = {
    msix_mmio_writeb,	msix_mmio_writew,	msix_mmio_writel
};

*/
/*
static CPUReadMemoryFunc *msix_mmio_read[] = {
    msix_mmio_readb,	msix_mmio_readw,	msix_mmio_readl
};
*/

/*
static int assigned_dev_register_msix_mmio(AssignedDevice *dev)
{
    dev->msix_table_page = mmap(NULL, 0x1000,
                                PROT_READ|PROT_WRITE,
                                MAP_ANONYMOUS|MAP_PRIVATE, 0, 0);
    if (dev->msix_table_page == MAP_FAILED) {
        fprintf(stderr, "fail allocate msix_table_page! %s\n",
                strerror(errno));
        return -EFAULT;
    }
    memset(dev->msix_table_page, 0, 0x1000);
    dev->mmio_index = cpu_register_io_memory(
                        msix_mmio_read, msix_mmio_write, dev,
                        DEVICE_NATIVE_ENDIAN);
    return 0;
}
*/

/*
static void assigned_dev_unregister_msix_mmio(AssignedDevice *dev)
{
    if (!dev->msix_table_page)
        return;

    cpu_unregister_io_memory(dev->mmio_index);
    dev->mmio_index = 0;

    if (munmap(dev->msix_table_page, 0x1000) == -1) {
        fprintf(stderr, "error unmapping msix_table_page! %s\n",
                strerror(errno));
    }
    dev->msix_table_page = NULL;
}
*/

static const VMStateDescription vmstate_assigned_device = {
    .name = "pci-assign",
    .fields = (VMStateField []) {
        VMSTATE_END_OF_LIST()
    }
};

static void reset_assigned_device(DeviceState *dev)
{
    PCIDevice *d = DO_UPCAST(PCIDevice, qdev, dev);

    /*
     * When a 0 is written to the command register, the device is logically
     * disconnected from the PCI bus. This avoids further DMA transfers.
     */
    assigned_dev_pci_write_config(d, PCI_COMMAND, 0, 2);
}

static int assigned_initfn(struct PCIDevice *pci_dev)
{
    	AssignedDevice *dev = DO_UPCAST(AssignedDevice, dev, pci_dev);
    	int r;

	struct stat stbuff;

    	if (!kvm_enabled()) {
        	error_report("pci-assign: error: requires KVM support");
        	return -1;
    	}

	if (stat(dev->host.upci_path, &stbuff) != 0) {
        	error_report("pci-assign: error: no host device specified");
        	return -1;
	}

    	if (get_real_device(dev, dev->host.upci_path) != 0) {
		error_report("pci-assign: Error: Couldn't "
		    "get real device (%s)!", dev->dev.qdev.id);
		goto out;
	}


	/* handle real device's MMIO/PIO BARs */
	if (assigned_dev_register_regions(dev->real_device.regions,
	    dev->real_device.region_number, dev) != 0) {
        	goto out;
	}

/* CONTINUE HERE */
    /* handle interrupt routing */

    dev->intpin = dev->dev.config[0x3d] - 1;
    dev->run = 0;
    dev->girq = -1;
/*
    dev->h_segnr = dev->host.seg;
    dev->h_busnr = dev->host.bus;
    dev->h_devfn = PCI_DEVFN(dev->host.dev, dev->host.func);
*/

    if (assigned_device_pci_cap_init(pci_dev) < 0)
        goto out;

    /* assign device to guest */
    r = assign_device(dev);
    if (r < 0)
        goto out;

    /* assign irq for the device */
    r = assign_irq(dev);
    if (r < 0)
        goto assigned_out;

    /* intercept MSI-X entry page in the MMIO */
	/*
    if (dev->cap.available & ASSIGNED_DEVICE_CAP_MSIX)
        if (assigned_dev_register_msix_mmio(dev))
            goto assigned_out;
	* No MSI-X for now
	*/
	upci_start_intx_puller(dev->real_device.upci_fd,
	    assigned_dev_intx_intr_cb, dev);

    assigned_dev_load_option_rom(dev);
    QLIST_INSERT_HEAD(&devs, dev, next);

    add_boot_device_path(dev->bootindex, &pci_dev->qdev, NULL);

    /* Register a vmsd so that we can mark it unmigratable. */
    vmstate_register(&dev->dev.qdev, 0, &vmstate_assigned_device, dev);
    register_device_unmigratable(&dev->dev.qdev,
                                 vmstate_assigned_device.name, dev);

    return 0;

assigned_out:
    deassign_device(dev);
out:
    free_assigned_device(dev);
    return -1;
}

static int assigned_exitfn(struct PCIDevice *pci_dev)
{
    AssignedDevice *dev = DO_UPCAST(AssignedDevice, dev, pci_dev);

    vmstate_unregister(&dev->dev.qdev, &vmstate_assigned_device, dev);
    QLIST_REMOVE(dev, next);
    deassign_device(dev);
    free_assigned_device(dev);
    return 0;
}

static int parse_hostaddr(DeviceState *dev, Property *prop, const char *str)
{
	PCIHostDevice *ptr = qdev_get_prop_ptr(dev, prop);
	strcpy(ptr->upci_path, str);
	return (0);
}

static int print_hostaddr(DeviceState *dev, Property *prop, char *dest, size_t len)
{
    PCIHostDevice *ptr = qdev_get_prop_ptr(dev, prop);

    return snprintf(dest, len, "%s", ptr->upci_path);
}

PropertyInfo qdev_prop_hostaddr = {
    .name  = "pci-hostaddr",
    .type  = -1,
    .size  = sizeof(PCIHostDevice),
    .parse = parse_hostaddr,
    .print = print_hostaddr,
};

static PCIDeviceInfo assign_info = {
    .qdev.name    = "pci-assign",
    .qdev.desc    = "pass through host pci devices to the guest",
    .qdev.size    = sizeof(AssignedDevice),
    .qdev.reset   = reset_assigned_device,
    .init         = assigned_initfn,
    .exit         = assigned_exitfn,
    .config_read  = assigned_dev_pci_read_config,
    .config_write = assigned_dev_pci_write_config,
    .qdev.props   = (Property[]) {
        DEFINE_PROP("host", AssignedDevice, host, qdev_prop_hostaddr, PCIHostDevice),
        DEFINE_PROP_BIT("iommu", AssignedDevice, features,
                        ASSIGNED_DEVICE_USE_IOMMU_BIT, true),
        DEFINE_PROP_BIT("prefer_msi", AssignedDevice, features,
                        ASSIGNED_DEVICE_PREFER_MSI_BIT, true),
        DEFINE_PROP_INT32("bootindex", AssignedDevice, bootindex, -1),
        DEFINE_PROP_STRING("configfd", AssignedDevice, configfd_name),
        DEFINE_PROP_END_OF_LIST(),
    },
};

static void assign_register_devices(void)
{
    pci_qdev_register(&assign_info);
}

device_init(assign_register_devices)

/*
 * Scan the assigned devices for the devices that have an option ROM, and then
 * load the corresponding ROM data to RAM. If an error occurs while loading an
 * option ROM, we just ignore that option ROM and continue with the next one.
 */
static void assigned_dev_load_option_rom(AssignedDevice *dev)
{
}
