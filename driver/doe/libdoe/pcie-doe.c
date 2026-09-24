// SPDX-License-Identifier: GPL-2.0
/*
 * Data Object Exchange was added to the PCI spec as an ECN to 5.0.
 *
 * Copyright (C) 2021 Huawei
 *     Jonathan Cameron <Jonathan.Cameron@huawei.com>
 */

#include <linux/bitfield.h>
#include <linux/delay.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/pci.h>
#include "pci_regs.h"
#include "pcie-doe.h"
#include "../doe_api.h"
//#include <pcie-doe.h>


#define __POLLING__ 0

static irqreturn_t doe_irq(int irq, void *data)
{
    struct pcie_doe *doe = data;
    struct pci_dev *pdev = doe->pdev;
    u32 val;

    dev_info(&pdev->dev, "[%s] received doe irq\n", __func__);

    pci_read_config_dword(pdev, doe->cap_offset + PCI_DOE_STATUS, &val);
    if (FIELD_GET(PCI_DOE_STATUS_INT_STATUS, val)) {
#if 0
        pci_write_config_dword(pdev, doe->cap_offset + PCI_DOE_STATUS, val & (~PCI_DOE_STATUS_INT_STATUS));
#endif
        //dev_info(&pdev->dev, "[%s] clear doe intr status bit\n", __func__);
    }

    /* Leave the error case to be handled outside irq */
    if (FIELD_GET(PCI_DOE_STATUS_ERROR, val)) {
        dev_info(&pdev->dev, "[%s] doe err bit is set\n", __func__);
    }

    if (!FIELD_GET(PCI_DOE_STATUS_BUSY, val)) {
        dev_info(&pdev->dev, "[%s] doe busy bit is clear\n", __func__);
    }

    if (FIELD_GET(PCI_DOE_STATUS_DATA_OBJECT_READY, val)) {
        dev_info(&pdev->dev, "[%s] doe ready bit is set\n", __func__);
        complete(&doe->c);
    }
    return IRQ_HANDLED;
}

static int pcie_doe_abort(struct pcie_doe *doe)
{
    struct pci_dev *pdev = doe->pdev;
    int retry = 0;
    u32 val;

    pci_write_config_dword(pdev, doe->cap_offset + PCI_DOE_CTRL, PCI_DOE_CTRL_ABORT | PCI_DOE_CTRL_INT_EN);
    /* Abort is allowed to take up to 1 second */
    do {
        retry++;
        pci_read_config_dword(pdev, doe->cap_offset + PCI_DOE_STATUS, &val);
        if (!FIELD_GET(PCI_DOE_STATUS_ERROR, val) && !FIELD_GET(PCI_DOE_STATUS_BUSY, val))
            return 0;
        usleep_range(1000, 2000);
    } while (retry < DOE_TIMEOUT_MS);

    return -EIO;
}

/**
 * pcie_doe_init() - Initialise a Data Object Exchange mailbox
 * @doe: state structure for the DOE mailbox
 * @pdev: pci device which has this DOE mailbox
 * @doe_offset: offset in configuration space of the DOE extended capability.
 * Returns: 0 on success, <0 on error
 *
 * Caller responsible for calling pci_alloc_irq_vectors() including DOE
 * interrupt.
 */
int pcie_doe_register_irq(struct pcie_doe *doe, struct pci_dev *pdev, int doe_offset)
{
    u32 val = 0;
    int rc = -1;
    int vec = -1;

    pci_read_config_dword(pdev, doe_offset + PCI_DOE_CAP, &val);
    dev_info(&pdev->dev, "doe cap reg[0x4]= 0x%x\n", val);
    if ((pdev->msi_cap || pdev->msix_cap) && FIELD_GET(PCI_DOE_CAP_INT_SUP, val)) {
        vec = pci_irq_vector(pdev, FIELD_GET(PCI_DOE_CAP_INT_MSG_NUM, val));
        if (vec < 0) {
            dev_info(&pdev->dev, "pci_irq_vector failed, vec=0x%x\n", vec);
            return -1;
        }

        doe->vec = vec;
        dev_info(&pdev->dev, "doe_int_msg_num = 0x%x, doe->vec = 0x%x\n",
                FIELD_GET(PCI_DOE_CAP_INT_MSG_NUM, val), doe->vec);
        if (doe_offset == NORMAL_DOE_CAP_OFF)
            rc = devm_request_irq(&pdev->dev, doe->vec, doe_irq, 0, "Normal DOE", doe);
        else if (doe_offset == SECURITY_DOE_CAP_OFF)
            rc = devm_request_irq(&pdev->dev, doe->vec, doe_irq, 0, "Security DOE", doe);
        else {
            dev_info(&pdev->dev, "invalid doe cap off=0x%x\n", doe_offset);
            return -1;
        }

        if (rc) {
            dev_info(&pdev->dev, "devm_request_irq failed rc=0x%x\n", rc);
            return rc;
        }

        dev_info(&pdev->dev, "devm_request_irq success vec=0x%x\n", doe->vec);
        doe->use_int = true;
        pci_write_config_dword(pdev, doe_offset + PCI_DOE_CTRL, FIELD_PREP(PCI_DOE_CTRL_INT_EN, 1));
    }

    mutex_init(&doe->lock);
    init_completion(&doe->c);
    doe->cap_offset = doe_offset;
    doe->pdev = pdev;

    /* Reset the mailbox by issuing an abort */
    rc = pcie_doe_abort(doe);
    if (rc) {
        dev_info(&pdev->dev, "doe abort failed\n");
        return rc;
    }

    return 0;
}

void pcie_doe_unregister_irq(struct pcie_doe *doe, struct pci_dev *pdev)
{
    if (doe->use_int) {
        devm_free_irq(&pdev->dev, doe->vec, doe);
        dev_info(&pdev->dev, "devm_free_irq 0x%x\n", doe->vec);
    }

    /* Reset DOE control register*/
    pci_write_config_dword(pdev, doe->cap_offset + PCI_DOE_CTRL, 0);
}



/**
 * pcie_doe_exchange() - Send a request and receive a response
 * @doe: DOE mailbox state structure
 * @request: request data to be sent
 * @request_sz: size of request in bytes
 * @response: buffer into which to place the response
 * @response_sz: size of available response buffer in bytes
 *
 * Return: 0 on success, < 0 on error
 * Excess data will be discarded.
 */
int pcie_doe_exchange(struct pcie_doe *doe, u32 *request, size_t request_sz,
        u32 *response, size_t response_sz)
{
    struct pci_dev *pdev = doe->pdev;
    int ret = 0;
    int i;
    u32 val;
    int retry = -1;
    size_t length;

    /* DOE requests must be a whole number of DW */
    if (request_sz % sizeof(u32)) {
        return -EINVAL;
    }

    /* Need at least 2 DW to get the length */
    if (response_sz < 2 * sizeof(u32))
        return -EINVAL;

    mutex_lock(&doe->lock);
    /*
     * Check the DOE busy bit is not set.
     * If it is set, this could indicate someone other than Linux is
     * using the mailbox.
     */
    pci_read_config_dword(pdev, doe->cap_offset + PCI_DOE_STATUS, &val);
    if (FIELD_GET(PCI_DOE_STATUS_BUSY, val)) {
        dev_info(&pdev->dev, "!!!!!!!!!!!! busy !!!!!!!!!!\n");
        pci_write_config_dword(pdev, doe->cap_offset + PCI_DOE_STATUS, 0);
        ret = -EBUSY;
        goto unlock;
    }

    if (FIELD_GET(PCI_DOE_STATUS_ERROR, val)) {
        dev_info(&pdev->dev, "!!!!!!!!!!!! err !!!!!!!!!!\n");
        pci_write_config_dword(pdev, doe->cap_offset + PCI_DOE_STATUS, 0);
        ret = pcie_doe_abort(doe);
        if (ret)
            goto unlock;
    }

    dev_info(&pdev->dev, "================ start ==============\n");
    for (i = 0; i < request_sz / 4; i++) {
        pci_write_config_dword(pdev, doe->cap_offset + PCI_DOE_WRITE, request[i]);
        dev_dbg(&pdev->dev, "request[%d] = %x\n", i, request[i]);
    }

    reinit_completion(&doe->c);
    pci_write_config_dword(pdev, doe->cap_offset + PCI_DOE_CTRL, PCI_DOE_CTRL_GO | PCI_DOE_CTRL_INT_EN);

#if __POLLING__ == 1
    bool use_int = 0;
#else
    bool use_int = doe->use_int;
#endif
    if (use_int) {
        dev_info(&pdev->dev, "wait for doe irq\n");
        /*
         * Timeout of 1 second from 6.xx.1 ECN - Data Object Exchange
         * Note a protocol is allowed to specify a different timeout, so
         * that may need supporting in future.
         */
        if (!wait_for_completion_timeout(&doe->c, msecs_to_jiffies(DOE_TIMEOUT_MS))) {
            ret = -ETIMEDOUT;
            dev_info(&pdev->dev, "irq: doe rdy timeout\n");
            goto unlock;
        }

        pci_read_config_dword(pdev, doe->cap_offset + PCI_DOE_STATUS, &val);
        if (FIELD_GET(PCI_DOE_STATUS_ERROR, val)) {
            pcie_doe_abort(doe);
            ret = -EIO;
            goto unlock;
        }
    } else {
        dev_info(&pdev->dev, "polling doe rdy\n");
        do {
            retry++;
            pci_read_config_dword(pdev, doe->cap_offset + PCI_DOE_STATUS, &val);
            if (FIELD_GET(PCI_DOE_STATUS_ERROR, val)) {
                pcie_doe_abort(doe);
                ret = -EIO;
                goto unlock;
            }

            if (FIELD_GET(PCI_DOE_STATUS_DATA_OBJECT_READY, val))
                break;
            usleep_range(1000, 2000);
        } while (retry < DOE_TIMEOUT_MS);
        if (!FIELD_GET(PCI_DOE_STATUS_DATA_OBJECT_READY, val)) {
            dev_info(&pdev->dev, "polling: doe rdy timeout\n");
            ret = -ETIMEDOUT;
            goto unlock;
        }
    }

    /* Read the first two dwords to get the length */
    pci_read_config_dword(pdev, doe->cap_offset + PCI_DOE_READ, &response[0]);
    dev_info(&pdev->dev, "rep[0] = %x\n", response[0]);

    pci_write_config_dword(pdev, doe->cap_offset + PCI_DOE_READ, 0);
    pci_read_config_dword(pdev, doe->cap_offset + PCI_DOE_READ, &response[1]);
    dev_info(&pdev->dev, "rep[1] = %x\n", response[1]);
    pci_write_config_dword(pdev, doe->cap_offset + PCI_DOE_READ, 0);
    length = FIELD_GET(PCI_DOE_DATA_OBJECT_HEADER_2_LENGTH, response[1]);
    if (length * sizeof(u32) > SZ_1M) {
        ret = -EIO;
        goto unlock;
    }

    for (i = 2; i < min(length, response_sz / 4); i++) {
        pci_read_config_dword(pdev, doe->cap_offset + PCI_DOE_READ, &response[i]);
        dev_dbg(&pdev->dev, "rep[%d] = %x\n", i, response[i]);
        pci_write_config_dword(pdev, doe->cap_offset + PCI_DOE_READ, 0);
    }
    /* flush excess length */
    for (; i < length; i++) {
        pci_read_config_dword(pdev, doe->cap_offset + PCI_DOE_READ, &val);
        pci_write_config_dword(pdev, doe->cap_offset + PCI_DOE_READ, 0);
    }
    /* Final error check to pick up on any since Data Object Ready */
    pci_read_config_dword(pdev, doe->cap_offset + PCI_DOE_STATUS, &val);
    if (FIELD_GET(PCI_DOE_STATUS_ERROR, val)) {
        pcie_doe_abort(doe);
        ret = -EIO;
    }

    dev_info(&pdev->dev, "================ end ==============\n");
unlock:
    mutex_unlock(&doe->lock);
    return ret;
}


static int pcie_doe_discovery(struct pcie_doe *doe, u8 *index, u16 *vid, u8 *protocol)
{
    u32 request[3] = {
        [0] = FIELD_PREP(PCI_DOE_DATA_OBJECT_HEADER_1_VID, 0001) |
            FIELD_PREP(PCI_DOE_DATA_OBJECT_HEADER_1_TYPE, 0),
        [1] = FIELD_PREP(PCI_DOE_DATA_OBJECT_HEADER_2_LENGTH, 3),
        [2] = FIELD_PREP(PCI_DOE_DATA_OBJECT_DISC_REQ_3_INDEX, *index)
    };
    u32 response[3];
    int ret;

    ret = pcie_doe_exchange(doe, request, sizeof(request), response, sizeof(response));
    if (ret)
        return ret;

    *vid = FIELD_GET(PCI_DOE_DATA_OBJECT_DISC_RSP_3_VID, response[2]);
    *protocol = FIELD_GET(PCI_DOE_DATA_OBJECT_DISC_RSP_3_PROTOCOL, response[2]);
    *index = FIELD_GET(PCI_DOE_DATA_OBJECT_DISC_RSP_3_NEXT_INDEX, response[2]);

    return 0;
}

/**
 * pcie_doe_protocol_check() - check if this DOE mailbox supports specific protocol
 * @doe: DOE state structure
 * @vid: Vendor ID
 * @protocol: Protocol number as defined by Vendor
 * Returns: 0 on success, <0 on error
 */
int pcie_doe_protocol_check(struct pcie_doe *doe, u16 vid, u8 protocol)
{
    u8 index = 0;

    do {
        u8 this_protocol;
        u16 this_vid;
        int ret;

        ret = pcie_doe_discovery(doe, &index, &this_vid, &this_protocol);
        if (ret)
            return ret;
        if (this_vid == vid && this_protocol == protocol)
            return 0;
    } while (index);

    return -ENODEV;
}
