/*
 * Copyright (C) 2017 MediaTek Inc.
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#define pr_fmt(fmt) "[cpu_ctrl]"fmt
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/module.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <linux/cpufreq.h>

#ifdef CONFIG_MTK_CORE_CTL
#include <mt-plat/core_ctl.h>
#endif
#include "cpu_ctrl.h"
#include "boost_ctrl.h"
#include "mtk_perfmgr_internal.h"
#include "topo_ctrl.h"

#ifdef CONFIG_MTK_CPU_CTRL_CFP
#include "cpu_ctrl_cfp.h"
#endif

#ifdef CONFIG_TRACING
#include <linux/kallsyms.h>
#include <linux/trace_events.h>
#endif

#define MAX_CLUSTER_COUNT 3

static struct mutex boost_freq;
static struct mutex isolate_lock;
static struct ppm_limit_data *current_freq;
static struct ppm_limit_data *freq_set[CPU_MAX_KIR];
static int log_enable;
static unsigned long *policy_mask;
static int num_cpu;
static int *cpu_isolation[CPU_ISO_MAX_KIR];
static int perfserv_isolation_cpu;
struct cluster_data {
	int core_min;
	int core_max;
};
static struct cluster_data core_set[CPU_ISO_MAX_KIR][MAX_CLUSTER_COUNT];
static struct cluster_data default_core_set[MAX_CLUSTER_COUNT];
static int isolation_used[CPU_ISO_MAX_KIR];
static int all_cpu_deisolated;
static int isBooting;

#ifdef CONFIG_MTK_CPU_CTRL_CFP
static int cfp_init_ret;
#endif

int powerhal_tid;

/*******************************************/
void update_isolation_cpu(int kicker, int enable, int cpu)
{
	int i, final = -1;

	mutex_lock(&isolate_lock);

	if (kicker < 0 || kicker >= CPU_ISO_MAX_KIR) {
		pr_debug("kicker:%d, error\n", kicker);
		mutex_unlock(&isolate_lock);
		return;
	}

	if (cpu < 0 || cpu >= num_cpu) {
		pr_debug("cpu:%d, error\n", cpu);
		mutex_unlock(&isolate_lock);
		return;
	}

	if (enable == cpu_isolation[kicker][cpu]) {
		mutex_unlock(&isolate_lock);
		return;
	}

	cpu_isolation[kicker][cpu] = enable;

	for (i = 0; i < CPU_ISO_MAX_KIR; i++) {
		if (cpu_isolation[i][cpu] == 0) {
			final = 0;
			break;
		} else if (cpu_isolation[i][cpu] == 1)
			final = 1;
	}

#ifdef CONFIG_TRACING
	perfmgr_trace_count(enable, "cpu_ctrl_isolation_%d_%d", kicker, cpu);
#endif

	if (final > 0)
		sched_isolate_cpu(cpu);
	else
		sched_deisolate_cpu(cpu);

	mutex_unlock(&isolate_lock);
}
EXPORT_SYMBOL(update_isolation_cpu);

int update_userlimit_cpu_freq(int kicker, int num_cluster
		, struct ppm_limit_data *freq_limit)
{
	struct ppm_limit_data *final_freq;
	int retval = 0;
	int i, j, len = 0, len1 = 0;
	char msg[LOG_BUF_SIZE * 2];
	char msg1[LOG_BUF_SIZE];


	mutex_lock(&boost_freq);

	final_freq = kcalloc(perfmgr_clusters
			, sizeof(struct ppm_limit_data), GFP_KERNEL);
	if (!final_freq) {
		retval = -1;
		perfmgr_trace_printk("cpu_ctrl", "!final_freq\n");
		goto ret_update;
	}
	if (num_cluster != perfmgr_clusters) {
		pr_debug(
				"num_cluster : %d perfmgr_clusters: %d, doesn't match\n",
				num_cluster, perfmgr_clusters);
		retval = -1;
		perfmgr_trace_printk("cpu_ctrl",
			"num_cluster != perfmgr_clusters\n");
		goto ret_update;
	}

	if (kicker < 0 || kicker >= CPU_MAX_KIR) {
		pr_debug("kicker: %d errro\n", kicker);
		retval = -1;
		goto ret_update;
	}

	for_each_perfmgr_clusters(i) {
		final_freq[i].min = -1;
		final_freq[i].max = -1;
	}

	len += snprintf(msg + len, sizeof(msg) - len, "[%d] ", kicker);
	if (len < 0) {
		retval = -EIO;
		perfmgr_trace_printk("cpu_ctrl", "return -EIO 1\n");
		goto ret_update;
	}
	for_each_perfmgr_clusters(i) {
		freq_set[kicker][i].min = freq_limit[i].min >= -1 ?
			freq_limit[i].min : -1;
		freq_set[kicker][i].max = freq_limit[i].max >= -1 ?
			freq_limit[i].max : -1;

		len += snprintf(msg + len, sizeof(msg) - len, "(%d)(%d) ",
		freq_set[kicker][i].min, freq_set[kicker][i].max);
		if (len < 0) {
			retval = -EIO;
			perfmgr_trace_printk("cpu_ctrl", "return -EIO 2\n");
			goto ret_update;
		}

		if (freq_set[kicker][i].min == -1 &&
				freq_set[kicker][i].max == -1)
			clear_bit(kicker, &policy_mask[i]);
		else
			set_bit(kicker, &policy_mask[i]);

		len1 += snprintf(msg1 + len1, sizeof(msg1) - len1,
				"[0x %lx] ", policy_mask[i]);
		if (len1 < 0) {
			retval = -EIO;
			perfmgr_trace_printk("cpu_ctrl", "return -EIO 3\n");
			goto ret_update;
		}
	}

	for (i = 0; i < CPU_MAX_KIR; i++) {
		for_each_perfmgr_clusters(j) {
			final_freq[j].min
				= MAX(freq_set[i][j].min, final_freq[j].min);
#ifdef CONFIG_MTK_CPU_CTRL_CFP
			final_freq[j].max
				= final_freq[j].max != -1 &&
				freq_set[i][j].max != -1 ?
				MIN(freq_set[i][j].max, final_freq[j].max) :
				MAX(freq_set[i][j].max, final_freq[j].max);
#else
			final_freq[j].max
				= MAX(freq_set[i][j].max, final_freq[j].max);
#endif
			if (final_freq[j].min > final_freq[j].max &&
					final_freq[j].max != -1)
				final_freq[j].max = final_freq[j].min;
		}
	}

	for_each_perfmgr_clusters(i) {
		current_freq[i].min = final_freq[i].min;
		current_freq[i].max = final_freq[i].max;
		len += snprintf(msg + len, sizeof(msg) - len, "{%d}{%d} ",
				current_freq[i].min, current_freq[i].max);
		if (len < 0) {
			retval = -EIO;
			perfmgr_trace_printk("cpu_ctrl", "return -EIO 4\n");
			goto ret_update;
		}
	}

	if (strlen(msg) + strlen(msg1) < LOG_BUF_SIZE)
		strncat(msg, msg1, strlen(msg1));

	if (log_enable)
		pr_debug("%s", msg);

#ifdef CONFIG_TRACING
	perfmgr_trace_printk("cpu_ctrl", msg);
#endif


#ifdef CONFIG_MTK_CPU_CTRL_CFP
	if (!cfp_init_ret)
		cpu_ctrl_cfp(final_freq);
	else
#endif
	{
		/* use mtk proprietary ppm API */
		if (mt_ppm_userlimit_cpu_freq)
			retval = mt_ppm_userlimit_cpu_freq(perfmgr_clusters, final_freq);
		else
			retval = perfmgr_common_userlimit_cpu_freq(perfmgr_clusters, final_freq);
	}
ret_update:

#ifndef CONFIG_MTK_CORE_CTL
	if (final_freq) {
		for_each_perfmgr_clusters(i) {
			struct cpumask cluster_cpus;
			int cpu, iso;

			iso = (final_freq[i].min != -1) ? 0 : -1;
			arch_get_cluster_cpus(&cluster_cpus, i);
			for_each_cpu(cpu, &cluster_cpus)
				update_isolation_cpu(CPU_ISO_KIR_CPU_CTRL,
					iso, cpu);
		}
	}
#endif

	kfree(final_freq);
	mutex_unlock(&boost_freq);
	return retval;
}
EXPORT_SYMBOL(update_userlimit_cpu_freq);

int perfmgr_common_userlimit_cpu_freq(unsigned int cluster_num, struct ppm_limit_data *final_freq)
{
	int i = 0, retval = 0;
	struct cpufreq_policy **policy;
	struct cpumask *cpus_mask;

	policy = kcalloc(perfmgr_clusters,
		sizeof(struct cpufreq_policy *), GFP_KERNEL);
	cpus_mask = kcalloc(1, sizeof(struct cpumask), GFP_KERNEL);

	if (!policy || !cpus_mask) {
		retval = -ENOMEM;
		goto free;
	}

	for_each_perfmgr_clusters(i) {
		arch_get_cluster_cpus(cpus_mask, i);
		policy[i] = cpufreq_cpu_get(
			cpumask_first(cpus_mask));

		if (final_freq[i].min == -1)
			policy[i]->user_policy.min =
				policy[i]->cpuinfo.min_freq;
		else
			policy[i]->user_policy.min =
				final_freq[i].min;

		if (final_freq[i].max == -1)
			policy[i]->user_policy.max =
				policy[i]->cpuinfo.max_freq;
		else
			policy[i]->user_policy.max =
				final_freq[i].max;

		cpufreq_cpu_put(policy[i]);
		cpufreq_update_policy(cpumask_first(cpus_mask));
	}

free:
	kfree(policy);
	kfree(cpus_mask);

	return retval;
}
EXPORT_SYMBOL(perfmgr_common_userlimit_cpu_freq);


int update_cpu_core_limit(int kicker, int cid, int min, int max)
{
	int i, final_min, final_max;

	perfmgr_trace_count(kicker,
		"update_cpu_core_limit_%d_%d_%d_%d", kicker, cid, min, max);
	mutex_lock(&boost_freq);

	core_set[kicker][cid].core_min = min;
	core_set[kicker][cid].core_max = max;

	final_min = -1;
	final_max = default_core_set[cid].core_max;

	for (i = 0; i <= CPU_ISO_KIR_FPSGO; i++) {
		if (core_set[i][cid].core_max >= 0 &&
			final_min <= core_set[i][cid].core_max &&
			core_set[i][cid].core_max <= final_max)
			final_max = core_set[i][cid].core_max;
		if (final_min < core_set[i][cid].core_min) {
			if (core_set[i][cid].core_min <= final_max)
				final_min = core_set[i][cid].core_min;
			else
				final_min = (final_max < 0) ?
					core_set[i][cid].core_min : final_max;
		}
	}

	if (final_max < 0)
		final_max = default_core_set[cid].core_max;
	if (final_min < 0)
		final_min = MIN(default_core_set[cid].core_min, final_max);
#ifdef CONFIG_MTK_CORE_CTL
	perfmgr_trace_count(kicker,
		"core_ctl_set_limit_cpus_%d_%d_%d", cid, final_min, final_max);
	core_ctl_set_limit_cpus(cid, final_min, final_max);
#endif
	mutex_unlock(&boost_freq);

	return 0;
}
EXPORT_SYMBOL(update_cpu_core_limit);

/***************************************/
static ssize_t perfmgr_perfserv_freq_proc_write(struct file *filp
		, const char __user *ubuf, size_t cnt, loff_t *pos)
{
	int i = 0, data;
	struct ppm_limit_data *freq_limit;
	unsigned int arg_num = perfmgr_clusters * 2; /* for min and max */
	char *tok, *tmp;
	char *buf = perfmgr_copy_from_user_for_proc(ubuf, cnt);

	if (!buf) {
		pr_debug("buf is null\n");
		goto out1;
	}
	freq_limit = kcalloc(perfmgr_clusters, sizeof(struct ppm_limit_data),
			GFP_KERNEL);
	if (!freq_limit)
		goto out;

	tmp = buf;
	pr_debug("freq write_to_file\n");
	while ((tok = strsep(&tmp, " ")) != NULL) {
		if (i == arg_num) {
			pr_debug(
					"@%s: number of arguments > %d!\n",
					__func__, arg_num);
			goto out;
		}

		if (kstrtoint(tok, 10, &data)) {
			pr_debug("@%s: Invalid input: %s\n",
					__func__, tok);
			goto out;
		} else {
			if (i % 2) /* max */
				freq_limit[i/2].max = data;
			else /* min */
				freq_limit[i/2].min = data;
			i++;
		}
	}

	if (i < arg_num) {
		pr_debug(
				"@%s: number of arguments < %d!\n",
				__func__, arg_num);
	} else {
		powerhal_tid = current->pid;
		update_userlimit_cpu_freq(CPU_KIR_PERF
				, perfmgr_clusters, freq_limit);
	}
out:
	free_page((unsigned long)buf);
	kfree(freq_limit);
out1:
	return cnt;

}

static int perfmgr_perfserv_freq_proc_show(struct seq_file *m, void *v)
{
	int i;

	for_each_perfmgr_clusters(i)
		seq_printf(m, "cluster %d min:%d max:%d\n",
				i, freq_set[CPU_KIR_PERF][i].min,
				freq_set[CPU_KIR_PERF][i].max);
	return 0;
}
/***************************************/
#define MAX_NR_FREQ 16
static ssize_t perfmgr_boot_freq_proc_write(struct file *filp,
		const char __user *ubuf, size_t cnt, loff_t *pos)
{
	int i = 0, data, cid;
	struct ppm_limit_data *freq_limit;
	unsigned int arg_num = perfmgr_clusters * 2; /* for min and max */
	char *tok, *tmp;
	char *buf = perfmgr_copy_from_user_for_proc(ubuf, cnt);

	freq_limit = kcalloc(perfmgr_clusters,
			sizeof(struct ppm_limit_data), GFP_KERNEL);
	if (!freq_limit)
		goto out;

	if (!buf) {
		pr_debug("buf is null\n");
		goto out;
	}

	tmp = buf;
	while ((tok = strsep(&tmp, " ")) != NULL) {
		if (i == arg_num) {
			pr_debug("@%s: number of arguments > %d!\n",
					__func__, arg_num);
			break;
		}

		if (kstrtoint(tok, 10, &data)) {
			pr_debug("@%s: Invalid input: %s\n", __func__, tok);
			goto out;
		} else {
#if defined(CONFIG_MTK_CPU_FREQ) || defined(CONFIG_MACH_MT6739)
			if (i % 2) /* max */
				freq_limit[i/2].max =
					(data < 0 || data > MAX_NR_FREQ - 1)
					? -1 :
					mt_cpufreq_get_freq_by_idx(i / 2, data);
			else /* min */
				freq_limit[i/2].min =
					(data < 0 || data > MAX_NR_FREQ - 1)
					? -1 :
					mt_cpufreq_get_freq_by_idx(i / 2, data);
			i++;
#endif
		}
	}

	isBooting = 0;
	for_each_perfmgr_clusters(cid)
		if (freq_limit[cid].min != -1 || freq_limit[cid].max != -1) {
			isBooting = 1;
#ifdef CONFIG_MTK_CORE_CTL
			core_ctl_set_boost(1);
#endif
			break;
		}

#ifdef CONFIG_MTK_CORE_CTL
	if (!isBooting)
		core_ctl_set_boost(all_cpu_deisolated);
#endif

	if (i < arg_num)
		pr_debug("@%s: number of arguments < %d!\n",
				__func__, arg_num);
	else
		update_userlimit_cpu_freq(CPU_KIR_BOOT,
				perfmgr_clusters, freq_limit);

out:
	free_page((unsigned long)buf);
	kfree(freq_limit);
	return cnt;

}

static int perfmgr_boot_freq_proc_show(struct seq_file *m, void *v)
{
	int i;

	for_each_perfmgr_clusters(i)
		seq_printf(m, "cluster %d min:%d max:%d\n",
				i, freq_set[CPU_KIR_BOOT][i].min,
				freq_set[CPU_KIR_BOOT][i].max);

	return 0;
}

/***************************************/
static int perfmgr_current_freq_proc_show(struct seq_file *m, void *v)
{
	int i;

	for_each_perfmgr_clusters(i)
		seq_printf(m, "cluster %d min:%d max:%d\n",
				i, current_freq[i].min, current_freq[i].max);

	return 0;
}

/*******************************************/
static ssize_t perfmgr_perfmgr_log_proc_write(struct file *filp,
		const char __user *ubuf, size_t cnt, loff_t *pos)
{
	int data = 0;

	int rv = check_proc_write(&data, ubuf, cnt);

	if (rv != 0)
		return rv;
	log_enable = data > 0 ? 1 : 0;

	return cnt;
}

static int perfmgr_perfmgr_log_proc_show(struct seq_file *m, void *v)
{
	if (m)
		seq_printf(m, "%d\n", log_enable);
	return 0;
}

/*******************************************/
static ssize_t perfmgr_perfserv_iso_cpu_proc_write(struct file *filp,
		const char __user *ubuf, size_t cnt, loff_t *pos)
{
	int i, data = 0, cid;
	int rv = check_proc_write(&data, ubuf, cnt);

	if (rv != 0)
		return rv;

	perfserv_isolation_cpu = data;
	cid = 0;

#ifdef CONFIG_MTK_CORE_CTL
	isolation_used[CPU_ISO_KIR_PERF_ISO] = (data > 0) ? 1 : 0;

	for_each_perfmgr_clusters(i) {
		core_set[CPU_ISO_KIR_PERF_ISO][i].core_min = -1;
		core_set[CPU_ISO_KIR_PERF_ISO][i].core_max =
			default_core_set[i].core_max;
	}

	for (i = 0; i < num_cpu; i++) {
		if ((perfserv_isolation_cpu & (1 << i)) > 0) {
			core_set[CPU_ISO_KIR_PERF_ISO][cid].core_max--;
			core_ctl_set_not_preferred(cid, i, 1);
		} else {
			core_ctl_set_not_preferred(cid, i, 0);
		}
		if (i+1 >= topo_ctrl_get_cluster_cpu_id(cid+1))
			cid++;
	}

	for_each_perfmgr_clusters(i)
		update_cpu_core_limit(CPU_ISO_KIR_PERF_ISO, i,
			core_set[CPU_ISO_KIR_PERF_ISO][i].core_min,
			core_set[CPU_ISO_KIR_PERF_ISO][i].core_max);
#else
	for (i = 0; i < num_cpu; i++) {
		if ((perfserv_isolation_cpu & (1 << i)) > 0)
			update_isolation_cpu(CPU_ISO_KIR_PERF_ISO, 1, i);
		else
			update_isolation_cpu(CPU_ISO_KIR_PERF_ISO, -1, i);
	}
#endif

	return cnt;
}

static int perfmgr_perfserv_iso_cpu_proc_show(struct seq_file *m, void *v)
{
	if (m)
		seq_printf(m, "0x%x\n", perfserv_isolation_cpu);
	return 0;
}


static ssize_t perfmgr_perfserv_core_proc_write(struct file *filp
		, const char __user *ubuf, size_t cnt, loff_t *pos)
{
	int i = 0, data, isDefault = 1;
	unsigned int arg_num = perfmgr_clusters * 2; /* for min and max */
	char *tok, *tmp;
	char *buf = perfmgr_copy_from_user_for_proc(ubuf, cnt);

	if (!buf) {
		pr_debug("buf is null\n");
		goto out1;
	}

	tmp = buf;
	pr_debug("freq write_to_file\n");
	while ((tok = strsep(&tmp, " ")) != NULL) {
		if (i == arg_num) {
			pr_debug(
					"@%s: number of arguments > %d!\n",
					__func__, arg_num);
			goto out;
		}

		if (kstrtoint(tok, 10, &data)) {
			pr_debug("@%s: Invalid input: %s\n",
					__func__, tok);
			goto out;
		} else {
			if (i % 2)
				core_set[CPU_ISO_KIR_PERF_CORE][i/2].core_max = data;
			else
				core_set[CPU_ISO_KIR_PERF_CORE][i/2].core_min = data;
			i++;
		}
	}

	if (i < arg_num) {
		pr_debug(
				"@%s: number of arguments < %d!\n",
				__func__, arg_num);
	} else {
		powerhal_tid = current->pid;
		for_each_perfmgr_clusters(i) {
			if (core_set[CPU_ISO_KIR_PERF_CORE][i].core_min != -1 ||
				core_set[CPU_ISO_KIR_PERF_CORE][i].core_max != -1) {
				isDefault = 0;
				break;
			}
		}
		isolation_used[CPU_ISO_KIR_PERF_CORE] = (isDefault) ? 0 : 1;
		for_each_perfmgr_clusters(i)
			update_cpu_core_limit(CPU_ISO_KIR_PERF_CORE, i,
				core_set[CPU_ISO_KIR_PERF_CORE][i].core_min,
				core_set[CPU_ISO_KIR_PERF_CORE][i].core_max);
	}

out:
	free_page((unsigned long)buf);
out1:
	return cnt;
}

static int perfmgr_perfserv_core_proc_show(struct seq_file *m, void *v)
{
	int i;

	for_each_perfmgr_clusters(i)
		seq_printf(m, "cluster %d min:%d max:%d\n",
				i, core_set[CPU_ISO_KIR_PERF_CORE][i].core_min,
				core_set[CPU_ISO_KIR_PERF_CORE][i].core_max);
	return 0;
}

/*******************************************/
static ssize_t perfmgr_perfserv_all_cpu_deisolated_proc_write
	(struct file *filp, const char __user *ubuf, size_t cnt, loff_t *pos)
{
	int data = 0;
	int rv = check_proc_write(&data, ubuf, cnt);

	if (rv != 0)
		return rv;

	all_cpu_deisolated = (data > 0) ? 1 : 0;

#ifdef CONFIG_MTK_CORE_CTL
	if (isBooting)
		core_ctl_set_boost(1);
	else
		core_ctl_set_boost(all_cpu_deisolated);
#endif

	return cnt;
}

static int perfmgr_perfserv_all_cpu_deisolated_proc_show
	(struct seq_file *m, void *v)
{
	if (m)
		seq_printf(m, "%d\n", all_cpu_deisolated);
	return 0;
}


PROC_FOPS_RW(perfserv_freq);
PROC_FOPS_RW(boot_freq);
PROC_FOPS_RO(current_freq);
PROC_FOPS_RW(perfmgr_log);
PROC_FOPS_RW(perfserv_iso_cpu);
PROC_FOPS_RW(perfserv_core);
PROC_FOPS_RW(perfserv_all_cpu_deisolated);

/************************************************/
int cpu_ctrl_init(struct proc_dir_entry *parent)
{
	struct proc_dir_entry *boost_dir = NULL;
	int i, j, ret = 0;

	struct pentry {
		const char *name;
		const struct file_operations *fops;
	};

	const struct pentry entries[] = {
		PROC_ENTRY(perfserv_freq),
		PROC_ENTRY(boot_freq),
		PROC_ENTRY(current_freq),
		PROC_ENTRY(perfmgr_log),
		PROC_ENTRY(perfserv_iso_cpu),
		PROC_ENTRY(perfserv_core),
		PROC_ENTRY(perfserv_all_cpu_deisolated),
	};
	mutex_init(&boost_freq);
	mutex_init(&isolate_lock);

	boost_dir = proc_mkdir("cpu_ctrl", parent);

	if (!boost_dir)
		pr_debug("boost_dir null\n ");

	/* create procfs */
	for (i = 0; i < ARRAY_SIZE(entries); i++) {
		if (!proc_create(entries[i].name, 0644,
					boost_dir, entries[i].fops)) {
			pr_debug("%s(), create /cpu_ctrl%s failed\n",
					__func__, entries[i].name);
			ret = -EINVAL;
			goto out;
		}
	}

#ifdef CONFIG_MTK_CPU_CTRL_CFP
	cfp_init_ret = cpu_ctrl_cfp_init(boost_dir);
#endif

	current_freq = kcalloc(perfmgr_clusters, sizeof(struct ppm_limit_data),
			GFP_KERNEL);

	policy_mask = kcalloc(perfmgr_clusters, sizeof(unsigned long),
			GFP_KERNEL);


	for (i = 0; i < CPU_MAX_KIR; i++)
		freq_set[i] = kcalloc(perfmgr_clusters
				, sizeof(struct ppm_limit_data)
				, GFP_KERNEL);

	for_each_perfmgr_clusters(i) {
		current_freq[i].min = -1;
		current_freq[i].max = -1;
		policy_mask[i] = 0;
	}

	for (i = 0; i < CPU_MAX_KIR; i++) {
		for_each_perfmgr_clusters(j) {
			freq_set[i][j].min = -1;
			freq_set[i][j].max = -1;
		}
	}

	num_cpu = num_possible_cpus();
	for (i = 0; i < CPU_ISO_MAX_KIR; i++) {
		cpu_isolation[i] = kcalloc(num_cpu, sizeof(int),
					GFP_KERNEL);
		for (j = 0; j < num_cpu; j++)
			cpu_isolation[i][j] = -1;
		isolation_used[i] = 0;

		for_each_perfmgr_clusters(j) {
			core_set[i][j].core_min = -1;
			core_set[i][j].core_max = -1;
		}
	}

	perfserv_isolation_cpu = 0;
	all_cpu_deisolated = 0;

	// sync with core_ctl
	for_each_perfmgr_clusters(i) {
		if (i == 0) {
			default_core_set[i].core_min = 4;
			default_core_set[i].core_max = 4;
		} else if (i == 1) {
			default_core_set[i].core_min = 2;
			default_core_set[i].core_max = 3;
		} else if (i == 2) {
			default_core_set[i].core_min = 0;
			default_core_set[i].core_max = 1;
		}
	}

	isBooting = 1;

out:
	return ret;

}

void cpu_ctrl_exit(void)
{
	int i;

	kfree(current_freq);
	kfree(policy_mask);
	for (i = 0; i < CPU_MAX_KIR; i++)
		kfree(freq_set[i]);
	for (i = 0; i < CPU_ISO_MAX_KIR; i++)
		kfree(cpu_isolation[i]);

#ifdef CONFIG_MTK_CPU_CTRL_CFP
	if (!cfp_init_ret)
		cpu_ctrl_cfp_exit();
#endif
}
