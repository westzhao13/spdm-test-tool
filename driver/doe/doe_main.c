/*
 * Copyright (C) 2021 Avery Design Systems, Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE file in the top-level directory.
 */

#define DEBUG
#include <linux/sched/clock.h>
#include <linux/module.h>
#include <linux/vmalloc.h>
#include <linux/mutex.h>
#include <linux/cdev.h>
#include <linux/idr.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <uapi/linux/pci_regs.h>
#include <linux/version.h>

#include "libdoe/pci_regs.h"
#include "libdoe/pcie-doe.h"
#include "doe.h"
#include "doe_api.h"

#define PCI_CLASS_MEMORY_CXL	0x0502
#define CXL_MEMORY_PROGIF 0x10
#define PCIE_EXT_CAP_OFFSET 0x100

#define DOE_NAME "doe"

#define DOE_INSTANCE_CNT 	(32) // 12

static DEFINE_IDA(doe_instance_ida);

struct doe_node {
    struct pcie_doe doe;
    struct doe_node *next;
};

struct doe_dev {
    int id;
    struct pci_dev *pdev;
    struct cdev cdev;
    struct doe_node *doe_head;
};

static int doe_major;
static struct class *doe_class = NULL;

#if 0
/* Discovery in kernel space */
static void do_doe_discovery(struct doe_dev *ddev) {
    int ret = -2;
    int idx = 0;
    doe_discovery_rsp response = { 0 };
    struct doe_node *dn = NULL;

    for (dn = ddev->doe_head; dn; dn = dn->next) {
        dev_info(&ddev->pdev->dev, "cap_offset=%x\n", dn->doe.cap_offset);
        do {
            idx = response.next_index;
            doe_discovery request = {
                .header = {
                    .vendor_id = PCI_DOE_PCI_SIG_VID,
                    .doe_type = PCI_SIG_DOE_DISCOVERY,
                    .length = DIV_ROUND_UP(sizeof(request), sizeof(uint32_t)),
                },
                .index = idx,
            };

            //printk("&ddev->doe_head->doe: %p\n", &ddev->doe_head->doe);
            ret = pcie_doe_exchange(&dn->doe, (u32 *)&request, sizeof(request), (u32 *)&response, sizeof(response));
            dev_info(&ddev->pdev->dev, "ret = %x, len %x, vid %x, type %x, next idx %x\n",
                    ret, response.header.length, response.vendor_id, response.doe_type, response.next_index);
        } while (response.next_index != 0);
    }
}
#endif

static void doe_free_nodes(struct doe_dev *ddev, struct pci_dev *pdev)
{
    struct doe_node *dn = ddev->doe_head;
    struct doe_node *dn_next;

    while (dn) {
        dn_next = dn->next;
        pcie_doe_unregister_irq(&dn->doe, pdev);
        kfree(dn);
        dn = dn_next;
    }
    ddev->doe_head = NULL;
}

static int doe_register_irq(struct doe_dev *ddev)
{
    bool found = false;
    u32 cap_offset = 0;
    u32 reg_val = 0;
    struct doe_node **dnp = NULL;
    struct pci_dev *pdev = NULL;
    int rt = -1;

    pdev = ddev->pdev;
    dnp = &ddev->doe_head;
    for (cap_offset = PCIE_EXT_CAP_OFFSET; cap_offset; cap_offset = PCI_EXT_CAP_NEXT(reg_val)) {
        pci_read_config_dword(pdev, cap_offset, &reg_val);
        if (PCI_EXT_CAP_ID(reg_val) != PCI_EXT_CAP_ID_DOE)
            continue;

        dev_info(&pdev->dev, "cap = %x\n", cap_offset);
        found = true;

        *dnp = kzalloc(sizeof(struct doe_node), GFP_KERNEL);
        if (!*dnp) {
            doe_free_nodes(ddev, pdev);
            return -ENOMEM;
        }
        if ((rt = pcie_doe_register_irq(&(*dnp)->doe, pdev, cap_offset)) != 0) {
            doe_free_nodes(ddev, pdev);
            return rt;
        }

        dnp = &(*dnp)->next;
    }

    if (found != true)
        return -1;

    return 0;
}

static void doe_unregister_irq(struct doe_dev *ddev)
{
    doe_free_nodes(ddev, ddev->pdev);
}

static int doe_alloc_msix_irq_vec(struct doe_dev *ddev)
{
    struct pci_dev *pdev = NULL;
    int rc = -1;

    pdev = ddev->pdev;
    rc = pci_alloc_irq_vectors(pdev, MSIX_CNT, MSIX_CNT, PCI_IRQ_MSIX);
    if (rc <= 0) {
        dev_info(&pdev->dev, "alloc %d msix vecs failed\n", rc);
        return -1;
    }

    dev_info(&pdev->dev, "allocated %d msix vecs success\n", rc);
    return 0;
}

static int doe_alloc_msi_irq_vec(struct doe_dev *ddev)
{
    struct pci_dev *pdev = NULL;
    int rc = -1;

    pdev = ddev->pdev;
    rc = pci_alloc_irq_vectors(pdev, MSI_CNT, MSI_CNT, PCI_IRQ_MSI);
    if (rc <= 0) {
        dev_info(&pdev->dev, "alloc %d msi vecs failed\n", rc);
        return -1;
    }

    dev_info(&pdev->dev, "allocated %d msi vecs success\n", rc);
    return 0;
}

static int doe_alloc_irq_vector(struct doe_dev *ddev)
{
    struct pci_dev *pdev = ddev->pdev;

#if __MSIX__ == 1 && __MSI__ == 1
    if (pdev->msi_cap || pdev->msix_cap)
        if (doe_alloc_msix_irq_vec(ddev))
            if (doe_alloc_msi_irq_vec(ddev))
                return -1;

    return 0;
#elif __MSIX__ == 1 && __MSI__ == 0
    if (pdev->msix_cap)
        if (doe_alloc_msix_irq_vec(ddev))
            return -1;

    return 0;
#elif __MSIX__ == 0 && __MSI__ == 1
    if (pdev->msi_cap)
        if (doe_alloc_msi_irq_vec(ddev))
            return -1;

    return 0;
#else
    return -1;
#endif
}

static void doe_free_irq_vector(struct doe_dev *ddev)
{
    struct pci_dev *pdev = ddev->pdev;
    if (pdev->msi_cap || pdev->msix_cap)
        pci_free_irq_vectors(pdev);
}

static long doe_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    u32 *req_buf = NULL;
    u32 *rsp_buf = NULL;
    int req_size = (PCI_DOE_MAX_DW_SIZE + 1) * sizeof(u32);
    int rsp_size = PCI_DOE_MAX_DW_SIZE * sizeof(u32);
    struct inode *inode = NULL;
    struct doe_dev *ddev = NULL;
    struct doe_node *dn = NULL;
    DOEHeader *doe_hdr = NULL;
    struct pci_dev *pdev = NULL;

    inode = file_inode(file);
    ddev = container_of(inode->i_cdev, typeof(*ddev), cdev);
    pdev = ddev->pdev;

    switch (cmd) {
        case DOE_IOCTL_MBOX_CMD: {
            /* Add one DW for cap_offset. Users should maintain the mappings for the
             * DOE cap offsets and their protocols */
            int ret = 0;
            req_buf = kmalloc(req_size, GFP_KERNEL);
            rsp_buf = kmalloc(rsp_size, GFP_KERNEL);
            if (!req_buf || !rsp_buf) {
                ret = -ENOMEM;
                goto out_free;
            }

            if (copy_from_user(req_buf, (void __user *)arg, req_size)) {
                dev_info(&pdev->dev, "can not copy from user\n");
                ret = -EINVAL;
                goto out_free;
            }
            dev_info(&pdev->dev, "buf: %08x %08x %08x %08x %08x %08x, req_size=%x\n", 
                    req_buf[0], req_buf[1], req_buf[2], req_buf[3], req_buf[4], req_buf[5], req_size);
            /* Find the struct doe_node corresponding to the offset info from user */
            u32 cap_offset = req_buf[0];
            if (cap_offset == NORMAL_DOE_CAP_OFF)
                dev_info(&pdev->dev, "Normal DOE: cap_offset=%x\n", cap_offset);
            else if (cap_offset == SECURITY_DOE_CAP_OFF)
                dev_info(&pdev->dev, "Security DOE: cap_offset=%x\n", cap_offset);
            else {
                dev_info(&pdev->dev, "invalid doe cap offset=%x\n", cap_offset);
                ret = -EINVAL;
                goto out_free;
            }

            for (dn = ddev->doe_head; dn; dn = dn->next) {
                if (dn->doe.cap_offset == cap_offset)
                    break;
            }
            if (dn == NULL) {
                printk (KERN_NOTICE "can't find the required capability 0x%x", req_buf[0]);
                ret = -ENOTTY;
                goto out_free;
            }

            doe_hdr = (DOEHeader *)(req_buf + 1);
            int rt = pcie_doe_exchange(&dn->doe, req_buf + 1, doe_hdr->length * sizeof(u32), rsp_buf, rsp_size);
            if (rt != 0) {
                dev_err(&pdev->dev, "doe send req failed\n");
                ret = -EBUSY;
                goto out_free;
            }

            doe_hdr = (DOEHeader *)rsp_buf;
            dev_info(&pdev->dev, "vendor %x, type %x, len %lx\n",
                    doe_hdr->vendor_id, doe_hdr->doe_type,
                    doe_hdr->length * sizeof(u32));
            if (copy_to_user((void __user *)arg, rsp_buf, doe_hdr->length * sizeof(u32))) {
                dev_info(&pdev->dev, "can not copy to user\n");
                ret = -EINVAL;
                goto out_free;
            }

out_free:
            kfree(req_buf);
            kfree(rsp_buf);
            return ret;
        }
        case DOE_IOCTL_INIT_MSIX:
#if __MSIX__ == 0
            return -EINVAL;
#else
            if (!pdev->msix_cap)
                return -EINVAL;

            doe_unregister_irq(ddev);
            doe_free_irq_vector(ddev);
            if (doe_alloc_msix_irq_vec(ddev)) {
                return -EINVAL;
            }
            if (doe_register_irq(ddev)) {
                doe_free_irq_vector(ddev);
                return -EIO;
            }
            return 0;
#endif
        case DOE_IOCTL_INIT_MSI:
#if __MSI__ == 0
            return -EINVAL;
#else
            if (!pdev->msi_cap)
                return -EINVAL;

            doe_unregister_irq(ddev);
            doe_free_irq_vector(ddev);
            if (doe_alloc_msi_irq_vec(ddev)) {
                return -EINVAL;
            }
            if (doe_register_irq(ddev)) {
                pci_free_irq_vectors(pdev);
                return -EIO;
            }
            return 0;
#endif
        default:
            return -ENOTTY;
    }
}

static const struct file_operations doe_fops = {
    .owner = THIS_MODULE,
    .unlocked_ioctl = doe_ioctl,
    .compat_ioctl = compat_ptr_ioctl,
    .llseek = noop_llseek,
};

static int doe_create_cdev(struct doe_dev *ddev)
{
    struct cdev *cdev = &ddev->cdev;
    int devno, rc = 0;
    struct pci_dev *pdev = ddev->pdev;

    cdev_init(cdev, &doe_fops);
    rc = ida_alloc(&doe_instance_ida, GFP_KERNEL);
    if (rc < 0) {
        dev_err(&pdev->dev, "get doe ida failed, rc=%d\n", rc);
        return rc;
    }

    ddev->id = rc;
    devno = MKDEV(doe_major, ddev->id);
    dev_info(&pdev->dev, "get doe ida=%d success, devno=0x%x\n", rc, devno);
    rc = cdev_add(cdev, devno, 1);
    if (rc) {
        ida_free(&doe_instance_ida, ddev->id);
        dev_err(&pdev->dev, "add cdev failed, rc=%d\n", rc);
        return rc;
    }

    struct device *device = NULL;
    device = device_create(doe_class, NULL, devno, NULL, "doe%d", ddev->id);
    if (IS_ERR(device)) {
        cdev_del(cdev);
        ida_free(&doe_instance_ida, ddev->id);
        dev_err(&pdev->dev, "device create doe%d failed\n", ddev->id);
        return PTR_ERR(device);
    }
    dev_info(&pdev->dev, "device create doe%d success\n", ddev->id);

    return 0;
}

static void doe_destroy_cdev(struct doe_dev *ddev)
{
    device_destroy(doe_class, MKDEV(doe_major, ddev->id));
    cdev_del(&ddev->cdev);
    ida_free(&doe_instance_ida, ddev->id);
}


#define PCI_CAP_ID(header)      (header & 0x000000ff)
#define PCI_CAP_NEXT(header)    ((header >> 8) & 0xff) 
static int doe_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
    struct doe_dev *ddev = NULL;

    //printk("doe_probe\n");
    if (pcim_enable_device(pdev)) {
        dev_info(&pdev->dev, "pcim_enable_device failed\n");
        return -1;
    }
    pci_set_master(pdev);

    ddev = kzalloc(sizeof(struct doe_dev), GFP_KERNEL);
    if (!ddev)
        return -ENOMEM;
    ddev->pdev = pdev;
    if (doe_alloc_irq_vector(ddev)) {
        kfree(ddev);
        return -1;
    }
    if (doe_register_irq(ddev)) {
        doe_free_irq_vector(ddev);
        kfree(ddev);
        return -1;
    }

    if (doe_create_cdev(ddev)) {
        doe_unregister_irq(ddev);
        doe_free_irq_vector(ddev);
        kfree(ddev);
        return -1;
    }

    dev_set_drvdata(&pdev->dev, ddev);

    return 0;
}

static void doe_remove(struct pci_dev *pdev)
{
    struct doe_dev *ddev = NULL;

    ddev = dev_get_drvdata(&pdev->dev);
    doe_destroy_cdev(ddev);
    doe_unregister_irq(ddev);
    doe_free_irq_vector(ddev);
    kfree(ddev);
    pci_set_drvdata(pdev, NULL);
}

static const struct pci_device_id doe_pci_tbl[] = {
    /* PCI class code for CXL.mem Type-3 Devices */
    { PCI_DEVICE_CLASS((PCI_CLASS_MEMORY_CXL << 8 | CXL_MEMORY_PROGIF), ~0)},
    { /* terminate list */ },
};
MODULE_DEVICE_TABLE(pci, doe_pci_tbl);

static struct pci_driver doe_driver = {
    .name			= KBUILD_MODNAME,
    .id_table		= doe_pci_tbl,
    .probe			= doe_probe,
    .remove			= doe_remove,
};

static __init int doe_init(void)
{
    int rc;
    dev_t devt;

    rc = alloc_chrdev_region(&devt, 0, DOE_INSTANCE_CNT, DOE_NAME);
    if (rc) {
        printk("alloc_chrdev_region failed, rc=%x\n", rc);
        return rc;
    }

    doe_major = MAJOR(devt);
#if LINUX_VERSION_CODE >= KERNEL_VERSION(6, 4, 0)
    doe_class = class_create(DOE_NAME);
#else
    doe_class = class_create(THIS_MODULE, DOE_NAME);
#endif
    if (doe_class == NULL) {
        printk("class create doe failed\n");
        unregister_chrdev_region(MKDEV(doe_major, 0), DOE_INSTANCE_CNT);
        return -1;
    }

    rc = pci_register_driver(&doe_driver);
    if (rc) {
        printk("pci_register_driver failed, rc=%x\n", rc);
        class_destroy(doe_class);
        unregister_chrdev_region(MKDEV(doe_major, 0), DOE_INSTANCE_CNT);
        return rc;
    }

    return 0;
}

static __exit void doe_exit(void)
{
    pci_unregister_driver(&doe_driver);
    class_destroy(doe_class);
    unregister_chrdev_region(MKDEV(doe_major, 0), DOE_INSTANCE_CNT);
    ida_destroy(&doe_instance_ida);
}

MODULE_LICENSE("GPL v2");
module_init(doe_init);
module_exit(doe_exit);
