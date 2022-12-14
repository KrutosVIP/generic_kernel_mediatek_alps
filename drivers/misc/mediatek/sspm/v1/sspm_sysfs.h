// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2016 MediaTek Inc.
 */

#ifndef __SSPM_SYSFS_H__
#define __SSPM_SYSFS_H__

#include <linux/sysfs.h>
#include <linux/device.h>

extern void sspm_log_if_wake(void);
extern int __init sspm_sysfs_init(void);
extern int sspm_sysfs_create_file(const struct device_attribute *attr);
extern int sspm_sysfs_create_bin_file(const struct bin_attribute *attr);
#endif
