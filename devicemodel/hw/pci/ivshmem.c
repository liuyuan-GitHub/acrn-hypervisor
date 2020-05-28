/*
 * Copyright (C) 2019 Intel Corporation. All rights reserved.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */
/*
 *  ACRN Inter-VM Virtualizaiton based on ivshmem-v1 device
 *
 *  +----------+    +-----------------------------------------+    +----------+
 *  |Postlaunch|    |               Service OS                |    |Postlaunch|
 *  |    VM    |    |                                         |    |    VM    |
 *  |          |    |                Interrupt                |    |          |
 *  |+--------+|    |+----------+     Foward      +----------+|    |+--------+|
 *  ||  App   ||    || acrn-dm  |    +-------+    | acrn-dm  ||    ||  App   ||
 *  ||        ||    ||+--------+|    |ivshmem|    |+--------+||    ||        ||
 *  |+---+----+|    |||ivshmem ||<---+server +--->||ivshmem |||    |+---+----+|
 *  |    |     |  +-+++   dm   ||    +-------+    ||   dm   +++-+  |    |     |
 *  |    |     |  | ||+---+----+|                 |+----+---+|| |  |    |     |
 *  |+---+----+|  | |+----^-----+                 +-----^----+| |  |+---+----+|
 *  ||UIO     ||  | |     +---------------+-------------+     | |  ||UIO     ||
 *  ||driver  ||  | |                     v                   | |  ||driver  ||
 *  |+---+----+|  | |            +--------+-------+           | |  |+---+----+|
 *  |    |     |  | |            |    /dev/shm    |           | |  |    |     |
 *  |    |     |  | |            +--------+-------+           | |  |    |     |
 *  |+---+----+|  | |                     |                   | |  |+---+----+|
 *  ||ivshmem ||  | |            +--------+-------+           | |  ||ivshmem ||
 *  ||device  ||  | |            | Shared Memory  |           | |  ||device  ||
 *  |+---+----+|  | |            +----------------+           | |  |+---+----+|
 *  +----+-----+  | +-----------------------------------------+ |  +----+-----+
 *       +--------+                                             +-------+
 *
 */

#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>

#include "pci_core.h"
#include "vmmapi.h"
#include "dm_string.h"
#include "log.h"

#define	IVSHMEM_MMIO_BAR	0
#define	IVSHMEM_MEM_BAR		2

#define	IVSHMEM_VENDOR_ID	0x1af4
#define	IVSHMEM_DEVICE_ID	0x1110
#define	IVSHMEM_CLASS		0x05
#define	IVSHMEM_REV		0x01


/* IVSHMEM MMIO Registers */
#define	IVSHMEM_REG_SIZE	0x100
#define	IVSHMEM_IRQ_MASK_REG	0x00
#define	IVSHMEM_IRQ_STA_REG	0x04
#define	IVSHMEM_IV_POS_REG	0x08
#define	IVSHMEM_DOORBELL_REG	0x0c
#define	IVSHMEM_RESERVED_REG	0x0f

struct pci_ivshmem_vdev {
	struct pci_vdev	*dev;
	char		*name;
	int		fd;
	void		*addr;
	uint32_t	size;
};

static int
create_shared_memory(struct vmctx *ctx, struct pci_ivshmem_vdev *vdev,
		const char *name, uint32_t size, uint64_t bar_addr)
{
	struct stat st;
	int fd = -1;
	void *addr = NULL;
	bool is_shm_creator = false;

	fd = shm_open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
	if (fd > 0)
		is_shm_creator = true;
	else if (fd < 0 && errno == EEXIST)
		fd = shm_open(name, O_RDWR, 0600);

	if (fd < 0) {
		pr_warn("failed to get %s status, error %s\n",
				name, strerror(errno));
		goto err;
	}
	if (is_shm_creator) {
		if (ftruncate(fd, size) < 0) {
			pr_warn("can't resize the shm size %u\n", size);
			goto err;
		}
	} else {
		if ((fstat(fd, &st) < 0) || st.st_size != size) {
			pr_warn("shm size is different, cur %u, creator %ld\n",
				size, st.st_size);
			goto err;
		}
	}

	addr = (void *)mmap(NULL, size, PROT_READ | PROT_WRITE,
			MAP_SHARED, fd, 0);
	pr_dbg("shm configuration, vma 0x%lx, ivshmem bar 0x%lx, size 0x%x\n",
			(uint64_t)addr, bar_addr, size);
	if (!addr || vm_map_memseg_vma(ctx, size, bar_addr,
				(uint64_t)addr, PROT_RW) < 0) {
		pr_warn("failed to map shared memory\n");
		goto err;
	}

	vdev->name = strdup(name);
	if (!vdev->name) {
		pr_warn("No memory for shm_name\n");
		goto err;
	}
	vdev->fd = fd;
	vdev->addr = addr;
	vdev->size = size;
	return 0;
err:
	if (addr)
		munmap(addr, size);
	if (fd > 0)
		close(fd);
	return -1;
}

static void
pci_ivshmem_write(struct vmctx *ctx, int vcpu, struct pci_vdev *dev,
	       int baridx, uint64_t offset, int size, uint64_t value)
{
	pr_dbg("%s: baridx %d, offset = %lx, value = 0x%lx\n",
			__func__, baridx, offset, value);

	if (baridx == IVSHMEM_MMIO_BAR) {
		switch (offset) {
		/*
		 * Following registers are used to support
		 * notification/interrupt in future.
		 */
		case IVSHMEM_IRQ_MASK_REG:
		case IVSHMEM_IRQ_STA_REG:
			break;
		case IVSHMEM_DOORBELL_REG:
			pr_warn("Doorbell capability doesn't support for now, ignore vectors 0x%lx, peer id %lu\n",
					value & 0xff, ((value >> 16) & 0xff));
			break;
		default:
			pr_dbg("%s: invalid device register 0x%lx\n",
					__func__, offset);
			break;
		}
	}
}

uint64_t
pci_ivshmem_read(struct vmctx *ctx, int vcpu, struct pci_vdev *dev,
	      int baridx, uint64_t offset, int size)
{
	uint64_t val = ~0;

	pr_dbg("%s: baridx %d, offset = 0x%lx, size = 0x%x\n",
			__func__, baridx, offset, size);

	if (baridx == IVSHMEM_MMIO_BAR) {
		switch (offset) {
		/*
		 * Following registers are used to support
		 * notification/interrupt in future.
		 */
		case IVSHMEM_IRQ_MASK_REG:
		case IVSHMEM_IRQ_STA_REG:
			val = 0;
			break;
		/*
		 * If ivshmem device doesn't support interrupt,
		 * The IVPosition is zero. otherwise, it is Peer ID.
		 */
		case IVSHMEM_IV_POS_REG:
			val = 0;
			break;
		default:
			pr_dbg("%s: invalid device register 0x%lx\n",
					__func__, offset);
			break;
		}
	}

	switch (size) {
	case 1:
		val &= 0xFF;
		break;
	case 2:
		val &= 0xFFFF;
		break;
	case 4:
		val &= 0xFFFFFFFF;
		break;
	}

	return val;
}

static int
pci_ivshmem_init(struct vmctx *ctx, struct pci_vdev *dev, char *opts)
{
	uint32_t size;
	uint64_t addr;
	char *tmp, *name, *orig;
	struct pci_ivshmem_vdev *ivshmem_vdev = NULL;

	/* ivshmem device usage: "-s N,ivshmem,shm_name,shm_size" */
	tmp = orig = strdup(opts);
	if (!orig) {
		pr_warn("No memory for strdup\n");
		goto err;
	}
	name = strsep(&tmp, ",");
	if (!name) {
		pr_warn("the shared memory size is not set\n");
		goto err;
	}
	if (dm_strtoui(tmp, &tmp, 10, &size) != 0) {
		pr_warn("the shared memory size is incorrect, %s\n", tmp);
		goto err;
	}
	if (size < 4096 || size > 128 * 1024 * 1024 ||
			(size & (size - 1)) != 0) {
		pr_warn("invalid shared memory size %u, the size range is [4K,128M] bytes and value must be a power of 2\n",
			size);
		goto err;
	}

	ivshmem_vdev = calloc(1, sizeof(struct pci_ivshmem_vdev));
	if (!ivshmem_vdev) {
		pr_warn("failed to allocate ivshmem device\n");
		goto err;
	}

	ivshmem_vdev->dev = dev;
	dev->arg = ivshmem_vdev;

	/* initialize config space */
	pci_set_cfgdata16(dev, PCIR_VENDOR, IVSHMEM_VENDOR_ID);
	pci_set_cfgdata16(dev, PCIR_DEVICE, IVSHMEM_DEVICE_ID);
	pci_set_cfgdata16(dev, PCIR_REVID, IVSHMEM_REV);
	pci_set_cfgdata8(dev, PCIR_CLASS, IVSHMEM_CLASS);

	pci_emul_alloc_bar(dev, IVSHMEM_MMIO_BAR, PCIBAR_MEM32, IVSHMEM_REG_SIZE);
	pci_emul_alloc_bar(dev, IVSHMEM_MEM_BAR, PCIBAR_MEM64, size);

	addr = pci_get_cfgdata32(dev, PCIR_BAR(IVSHMEM_MEM_BAR));
	addr |= ((uint64_t)pci_get_cfgdata32(dev, PCIR_BAR(IVSHMEM_MEM_BAR + 1)) << 32);
	addr &= PCIM_BAR_MEM_BASE;

	/*
	 * TODO: If UOS reprograms ivshmem BAR2, the shared memory will be
	 * unavailable for UOS, so we need to remap GPA and HPA of shared
	 * memory in this case.
	 */
	if (create_shared_memory(ctx, ivshmem_vdev, name, size, addr) < 0)
		goto err;

	free(orig);
	return 0;
err:
	if (orig)
		free(orig);
	if (ivshmem_vdev) {
		free(ivshmem_vdev);
		dev->arg = NULL;
	}
	return -1;
}

static void
pci_ivshmem_deinit(struct vmctx *ctx, struct pci_vdev *dev, char *opts)
{
	struct pci_ivshmem_vdev *vdev;

	vdev = (struct pci_ivshmem_vdev *)dev->arg;
	if (!vdev) {
		pr_warn("%s, invalid ivshmem instance\n", __func__);
		return;
	}
	if (vdev->addr && vdev->size)
		munmap(vdev->addr, vdev->size);
	if (vdev->fd > 0)
		close(vdev->fd);
	if (vdev->name) {
		/*
		 * unlink will only remove the shared memory file object,
		 * the shared memory will be released until all processes
		 * which opened the shared memory file close the file.
		 */
		shm_unlink(vdev->name);
		free(vdev->name);
	}
	free(vdev);
	dev->arg = NULL;
}

struct pci_vdev_ops pci_ops_ivshmem = {
	.class_name	= "ivshmem",
	.vdev_init	= pci_ivshmem_init,
	.vdev_deinit	= pci_ivshmem_deinit,
	.vdev_barwrite	= pci_ivshmem_write,
	.vdev_barread	= pci_ivshmem_read
};
DEFINE_PCI_DEVTYPE(pci_ops_ivshmem);
