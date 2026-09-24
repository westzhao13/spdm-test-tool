/*
 * Copyright (C) 2021 Avery Design Systems, Inc.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the LICENSE file in the top-level directory.
 */

#ifndef DOE_API_H
#define DOE_API_H


/*
 * The mailbox payload can be up to PCI_DOE_MAX_DW_SIZE DWs (~1 MB), which does
 * not fit the 14-bit _IOC size field. The driver copies a fixed req_size from
 * userspace regardless of _IOC_SIZE, so encode the direction/type only.
 */
#define DOE_IOCTL_MBOX_CMD      _IO('N', 0xb7)
#define DOE_IOCTL_INIT_MSIX     _IO('N', 0xb8)
#define DOE_IOCTL_INIT_MSI      _IO('N', 0xb9)

#define MSIX_CNT        (16)
#if __ZEBU__ == 1
#define DOE_TIMEOUT_MS   (10 * 1000) // 10s
#define __MSIX__ 	 	(1)
#define __MSI__ 	 	(0)
#define MSI_CNT         (1)
#else
#define DOE_TIMEOUT_MS   (990) // 1s
#define __MSIX__ 	 	(1)
#define __MSI__ 	 	(1)
#define MSI_CNT         (32)
#endif

#endif /* DOE_API_H */
