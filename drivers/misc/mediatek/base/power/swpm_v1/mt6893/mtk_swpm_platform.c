/*
 * Copyright (C) 2020 MediaTek Inc.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See http://www.gnu.org/licenses/gpl-2.0.html for more details.
 */

#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/notifier.h>
#include <linux/perf_event.h>
#include <trace/events/mtk_events.h>
#ifdef CONFIG_MTK_QOS_FRAMEWORK
#include <mtk_qos_ipi.h>
#endif
#ifdef CONFIG_MTK_STATIC_POWER
#include <mtk_common_static_power.h>
#endif
#ifdef CONFIG_THERMAL
#include <mtk_thermal.h>
#endif
#ifdef CONFIG_MTK_CPU_FREQ
#include <mtk_cpufreq_common_api.h>
#endif
#ifndef __CHECKER__
#define CREATE_TRACE_POINTS
#include <mtk_swpm_tracker_trace.h>
#endif
#ifdef CONFIG_MTK_TINYSYS_SSPM_SUPPORT
#include <sspm_reservedmem.h>
#endif
#include <mtk_swpm_common.h>
#include <mtk_swpm_platform.h>
#include <mtk_swpm_sp_platform.h>
#include <mtk_swpm_interface.h>

#undef swpm_pmu_enable

/****************************************************************************
 *  Macro Definitions
 ****************************************************************************/
/* #define LOG_LOOP_TIME_PROFILE */

/****************************************************************************
 *  Type Definitions
 ****************************************************************************/

/****************************************************************************
 *  Local Variables
 ****************************************************************************/
static unsigned int swpm_init_state;

/* index snapshot */
static struct mutex swpm_snap_lock;
static struct mem_swpm_index mem_idx_snap;

#ifdef CONFIG_MTK_TINYSYS_SSPM_SUPPORT
/* share sram for average power index */
static struct share_index *share_idx_ref;
static struct share_ctrl *share_idx_ctrl;
/* type of unsigned int pointer for share_index iterator */
static unsigned int *idx_ref_uint_ptr;
/* numbers of unsigned int output to LTR with iterator */
static unsigned int idx_output_size;

/* share dram for subsys related table communication */
static phys_addr_t rec_phys_addr, rec_virt_addr;
static unsigned long long rec_size;
#endif

#define swpm_pmu_get_sta(u)  ((swpm_pmu_sta & (1 << u)) >> u)
#define swpm_pmu_set_sta(u)  (swpm_pmu_sta |= (1 << u))
#define swpm_pmu_clr_sta(u)  (swpm_pmu_sta &= ~(1 << u))
static struct mutex swpm_pmu_mutex;
static unsigned int swpm_pmu_sta;

__weak int mt_spower_get_leakage_uW(int dev, int voltage, int deg)
{
	return 0;
}
struct core_swpm_rec_data *core_ptr;
struct mem_swpm_rec_data *mem_ptr;
struct me_swpm_rec_data *me_ptr;

static DEFINE_PER_CPU(struct perf_event *, l3dc_events);
static DEFINE_PER_CPU(struct perf_event *, inst_spec_events);
static DEFINE_PER_CPU(struct perf_event *, cycle_events);
static struct perf_event_attr l3dc_event_attr = {
	.type           = PERF_TYPE_RAW,
	.config         = 0x2B,
	.size           = sizeof(struct perf_event_attr),
	.pinned         = 1,
/*	.disabled       = 1, */
	.sample_period  = 0, /* 1000000000, */ /* ns ? */
};
static struct perf_event_attr inst_spec_event_attr = {
	.type           = PERF_TYPE_RAW,
	.config         = 0x1B,
	.size           = sizeof(struct perf_event_attr),
	.pinned         = 1,
/*	.disabled       = 1, */
	.sample_period  = 0, /* 1000000000, */ /* ns ? */
};
static struct perf_event_attr cycle_event_attr = {
	.type           = PERF_TYPE_HARDWARE,
	.config         = PERF_COUNT_HW_CPU_CYCLES,
	.size           = sizeof(struct perf_event_attr),
	.pinned         = 1,
/*	.disabled       = 1, */
	.sample_period  = 0, /* 1000000000, */ /* ns ? */
};

/* rt => /100000, uA => *1000, res => 100 */
#define CORE_DEFAULT_DEG (30)
#define CORE_DEFAULT_LKG (115)
#define CORE_LKG_RT_RES (100)
static unsigned int core_lkg;
static unsigned int core_lkg_replaced;
static unsigned short core_lkg_rt[NR_CORE_LKG_TYPE] = {
	7547, 4198, 5670, 11214, 16155, 19466,
	5042, 9301, 12999, 2085, 6321
};
static unsigned short core_volt_tbl[NR_CORE_VOLT] = {
	575, 600, 650, 725, 750,
};
static unsigned short ddr_opp_freq[NR_DDR_FREQ] = {
	400, 600, 800, 933, 1200, 1600, 1866, 2133,
};
static struct aphy_core_bw_data aphy_ref_core_bw_tbl[] = {
	[DDR_400] = {
	.bw = {63, 142, 300, 566, 1300, 1929,
	2566, 3176, 3847, 4471, 5296, 5769},
	},
	[DDR_600] = {
	.bw = {93, 213, 797, 2026, 3859, 4493,
	5042, 6171, 6701, 7262, 7892, 8796},
	},
	[DDR_800] = {
	.bw = {120, 283, 1077, 2697, 5145, 5990,
	6738, 8266, 8899, 9650, 10442, 11793},
	},
	[DDR_933] = {
	.bw = {141, 345, 1253, 3150, 6005, 6995,
	7859, 9607, 10375, 11222, 12230, 13759},
	},
	[DDR_1200] = {
	.bw = {175, 427, 1621, 4039, 7716, 8999,
	10061, 12290, 13249, 14410, 15652, 17662},
	},
	[DDR_1600] = {
	.bw = {237, 583, 2218, 5530, 10540, 12232,
	13670, 16677, 17946, 19335, 21059, 24038},
	},
	[DDR_1866] = {
	.bw = {273, 670, 2535, 6338, 12021, 13892,
	15507, 18910, 20276, 22132, 23896, 27176},
	},
	[DDR_2133] = {
	.bw = {321, 786, 2957, 7416, 13995, 16105,
	17957, 21886, 23383, 25861, 27678, 31358},
	},
};
static struct aphy_core_pwr_data aphy_def_core_pwr_tbl[] = {
	[APHY_VCORE] = {
	.pwr = {
		[DDR_400] = {
			.read_coef = {8, 19, 39, 74, 156, 228,
	296, 368, 411, 445, 445, 454},
			.write_coef = {15, 34, 71, 133, 295, 429,
	560, 705, 834, 889, 889, 889},
		},
		[DDR_600] = {
			.read_coef = {10, 26, 96, 228, 411, 491,
	518, 537, 541, 544, 546, 546},
			.write_coef = {26, 61, 231, 566, 1088, 1191,
	1328, 1330, 1330, 1330, 1330, 1333},
		},
		[DDR_800] = {
			.read_coef = {92, 231, 657, 1059, 1503, 1629,
	1722, 1723, 1726, 1728, 1728, 1728},
			.write_coef = {107, 242, 789, 1390, 2198, 2396,
	2482, 2482, 2482, 2482, 2485, 2489},
		},
		[DDR_933] = {
			.read_coef = {121, 306, 859, 1387, 1932, 2098,
	2206, 2214, 2214, 2214, 2214, 2214},
			.write_coef = {127, 324, 998, 1754, 2766, 3018,
	3231, 3235, 3241, 3241, 3241, 3246},
		},
		[DDR_1200] = {
			.read_coef = {204, 489, 1269, 2022, 2706, 2899,
	3039, 3074, 3074, 3074, 3074, 3074},
			.write_coef = {210, 450, 1444, 2537, 3910, 4289,
	4422, 4422, 4422, 4422, 4433, 4433},
		},
		[DDR_1600] = {
			.read_coef = {329, 822, 2136, 3266, 4377, 4740,
	4893, 4896, 4899, 4900, 4917, 4967},
			.write_coef = {335, 804, 2369, 4031, 6032, 6582,
	7028, 7040, 7049, 7049, 7049, 7147},
		},
		[DDR_1866] = {
			.read_coef = {530, 1235, 3202, 4920, 6384, 6862,
	7076, 7132, 7135, 7163, 7163, 7163},
			.write_coef = {543, 1211, 3528, 6097, 8797, 9573,
	10237, 10274, 10274, 10284, 10320, 10355},
		},
		[DDR_2133] = {
			.read_coef = {686, 1492, 3862, 5961, 7489, 7989,
	8228, 8378, 8401, 8422, 8422, 8422},
			.write_coef = {709, 1469, 4225, 7417, 10319, 11197,
	11992, 12059, 12059, 12121, 12121, 12121},
		},
	},
	.coef_idle = {1090, 1282, 1474, 1778, 1984, 2817, 3993, 4460},
	},
};
static struct aphy_others_bw_data aphy_ref_others_bw_tbl[] = {
	[DDR_400] = {
	.bw = {64, 128, 192, 256,
	320, 640, 960, 1280, 1600, 1920,
	2547, 3187, 3814, 4480, 5069, 5722},
	},
	[DDR_600] = {
	.bw = {96, 192, 288, 384,
	480, 960, 1440, 1920, 2390, 2880,
	3821, 4781, 5664, 6653, 7613, 8621},
	},
	[DDR_800] = {
	.bw = {128, 256, 384, 512,
	640, 1280, 1920, 2547, 3200, 3802,
	5069, 6336, 7603, 8947, 10150, 11482},
	},
	[DDR_933] = {
	.bw = {149, 299, 448, 597,
	746, 1493, 2239, 2986, 3702, 4434,
	5971, 7315, 8867, 10450, 11868, 13241},
	},
	[DDR_1200] = {
	.bw = {192, 384, 576, 768,
	960, 1920, 2861, 3840, 4762, 5683,
	7584, 9542, 11386, 13440, 14976, 17280},
	},
	[DDR_1600] = {
	.bw = {256, 512, 768, 1024,
	1280, 2560, 3840, 5069, 6323, 7603,
	10138, 12672, 15206, 17894, 19866, 22810},
	},
	[DDR_1866] = {
	.bw = {299, 597, 896, 1194,
	1493, 2986, 4478, 5971, 7315, 8867,
	11704, 14629, 17227, 20899, 23736, 26363},
	},
	[DDR_2133] = {
	.bw = {341, 683, 1024, 1365,
	1706, 3413, 5119, 6826, 8361, 10136,
	13378, 16723, 19692, 23890, 27132, 30135},
	},
};
static struct aphy_others_pwr_data aphy_def_others_pwr_tbl[] = {
	[APHY_VDDQ_0P6V] = {
	.pwr = {
		[DDR_400] = {
			.read_coef = {18, 18, 18, 18,
	18, 18, 18, 18, 18, 18,
	18, 24, 24, 24, 24, 24},
			.write_coef = {19, 44, 70, 95,
	120, 246, 354, 432, 552, 630,
	804, 978, 1128, 1284, 1410, 1530},
		},
		[DDR_600] = {
			.read_coef = {45, 50, 55, 59,
	64, 76, 106, 112, 112, 112,
	112, 112, 112, 112, 112, 112},
			.write_coef = {34, 82, 130, 178,
	226, 406, 580, 748, 904, 1048,
	1282, 1522, 1726, 1924, 2110, 2314},
		},
		[DDR_800] = {
			.read_coef = {46, 58, 70, 82,
	106, 142, 166, 178, 178, 178,
	178, 178, 178, 178, 178, 178},
			.write_coef = {70, 130, 190, 250,
	310, 562, 784, 988, 1186, 1312,
	1696, 1996, 2284, 2554, 2764, 3034},
		},
		[DDR_933] = {
			.read_coef = {51, 79, 106, 125,
	145, 194, 221, 227, 239, 239,
	239, 239, 239, 239, 239, 239},
			.write_coef = {93, 178, 262, 334,
	393, 682, 928, 1210, 1444, 1624,
	2014, 2350, 2698, 3016, 3358, 3580},
		},
		[DDR_1200] = {
			.read_coef = {67, 103, 139, 165,
	207, 270, 302, 327, 327, 327,
	327, 327, 327, 327, 327, 327},
			.write_coef = {144, 222, 300, 402,
	468, 840, 1128, 1488, 1734, 1968,
	2430, 2874, 3258, 3648, 3930, 4320},
		},
		[DDR_1600] = {
			.read_coef = {42, 54, 66, 72,
	78, 84, 96, 114, 144, 300,
	864, 1584, 2268, 3024, 3730, 4427},
			.write_coef = {126, 240, 354, 468,
	564, 1062, 1476, 1896, 2304, 2664,
	3432, 4116, 4788, 5424, 5868, 6468},
		},
		[DDR_1866] = {
			.read_coef = {107, 110, 120, 129,
	138, 146, 168, 175, 206, 370,
	911, 1618, 2236, 3121, 3836, 4474},
			.write_coef = {216, 390, 522, 732,
	846, 1338, 1764, 2172, 2550, 2934,
	3636, 4308, 4896, 5676, 6204, 6516},
		},
		[DDR_2133] = {
			.read_coef = {166, 167, 174, 187,
	198, 208, 222, 236, 268, 439,
	959, 1653, 2203, 3218, 3925, 4562},
			.write_coef = {306, 540, 690, 996,
	1128, 1614, 2052, 2448, 2796, 3204,
	3840, 4500, 5004, 5928, 6540, 6564},
		},
	},
	.coef_idle = {102, 104, 104, 104, 104, 1416, 1428, 1440},
	},
	[APHY_VM_0P75V] = {
	.pwr = {
		[DDR_400] = {
			.read_coef = {50, 77, 104, 131,
	158, 293, 413, 518, 653, 750,
	960, 1133, 1305, 1500, 1650, 1785},
			.write_coef = {32, 48, 65, 81,
	98, 180, 248, 300, 375, 420,
	525, 623, 713, 803, 870, 923},
		},
		[DDR_600] = {
			.read_coef = {75, 113, 150, 188,
	225, 405, 593, 765, 923, 1073,
	1290, 1538, 1755, 1980, 2212, 2427},
			.write_coef = {75, 98, 120, 143,
	165, 285, 398, 510, 608, 690,
	833, 975, 1095, 1208, 1305, 1410},
		},
		[DDR_800] = {
			.read_coef = {90, 143, 195, 248,
	300, 533, 743, 953, 1133, 1305,
	1628, 1763, 2213, 2483, 2740, 2989},
			.write_coef = {128, 143, 158, 173,
	240, 398, 548, 683, 803, 878,
	1125, 1298, 1463, 1620, 1733, 1875},
		},
		[DDR_933] = {
			.read_coef = {108, 176, 243, 311,
	363, 641, 889, 1098, 1361, 1511,
	1908, 2208, 2583, 2846, 3092, 3293},
			.write_coef = {86, 146, 206, 251,
	296, 491, 648, 843, 993, 1106,
	1346, 1548, 1766, 1946, 2133, 2261},
		},
		[DDR_1200] = {
			.read_coef = {135, 210, 285, 368,
	443, 788, 1080, 1380, 1620, 1883,
	2288, 2723, 3143, 3525, 3874, 4240},
			.write_coef = {135, 188, 240, 315,
	360, 623, 818, 1065, 1238, 1395,
	1688, 1965, 2205, 2445, 2603, 2805},
		},
		[DDR_1600] = {
			.read_coef = {195, 315, 435, 540,
	645, 1103, 1538, 1898, 2168, 2483,
	3075, 3623, 4125, 4628, 5097, 5534},
			.write_coef = {180, 270, 360, 458,
	540, 908, 1178, 1440, 1665, 1823,
	2235, 2550, 2850, 3113, 3278, 3510},
		},
		[DDR_1866] = {
			.read_coef = {386, 619, 776, 1076,
	1106, 1706, 2111, 2614, 2869, 3191,
	3746, 4331, 4834, 5494, 6027, 6443},
			.write_coef = {364, 574, 716, 1016,
	1114, 1519, 1774, 1999, 2239, 2426,
	2741, 3071, 3356, 3761, 3986, 4129},
		},
		[DDR_2133] = {
			.read_coef = {578, 923, 1118, 1343,
	1568, 2310, 2685, 3330, 3570, 3900,
	4418, 5040, 5543, 6360, 6946, 7402},
			.write_coef = {548, 878, 1073, 1575,
	1688, 2130, 2370, 2558, 2813, 3030,
	3248, 3593, 3863, 4410, 4695, 4748},
		},
	},
	.coef_idle = {75, 75, 75, 80, 80, 87, 94, 101},
	},
	[APHY_VIO_1P2V] = {
	.pwr = {
		[DDR_400] = {
			.read_coef = {74, 125, 175, 226,
	276, 528, 792, 1032, 1284, 1476,
	1932, 2244, 2520, 2904, 3204, 3336},
			.write_coef = {2, 5, 7, 10,
	12, 24, 24, 24, 24, 24,
	24, 24, 24, 24, 24, 24},
		},
		[DDR_600] = {
			.read_coef = {103, 151, 199, 247,
	295, 583, 895, 1207, 1495, 1747,
	1999, 2371, 2683, 2995, 3317, 3614},
			.write_coef = {5, 10, 14, 18,
	25, 31, 31, 31, 31, 31,
	31, 31, 31, 31, 31, 31},
		},
		[DDR_800] = {
			.read_coef = {72, 120, 168, 216,
	348, 684, 984, 1296, 1512, 1740,
	2148, 2760, 2796, 3084, 3359, 3623},
			.write_coef = {18, 24, 30, 36,
	48, 48, 48, 48, 48, 48,
	48, 48, 48, 48, 48, 48},
		},
		[DDR_933] = {
			.read_coef = {72, 144, 216, 288,
	348, 684, 998, 1236, 1584, 1716,
	2124, 2388, 2784, 3096, 3389, 3631},
			.write_coef = {12, 12, 12, 24,
	48, 48, 48, 48, 48, 48,
	48, 48, 48, 48, 48, 48},
		},
		[DDR_1200] = {
			.read_coef = {72, 144, 216, 288,
	348, 684, 996, 1296, 1548, 1800,
	2064, 2412, 2796, 3072, 3323, 3588},
			.write_coef = {12, 12, 12, 24,
	60, 60, 60, 60, 60, 60,
	60, 60, 60, 60, 60, 60},
		},
		[DDR_1600] = {
			.read_coef = {96, 192, 288, 360,
	444, 852, 1248, 1572, 1788, 1992,
	2328, 2760, 3144, 3432, 3701, 3943},
			.write_coef = {60, 60, 60, 60,
	60, 60, 60, 60, 60, 60,
	60, 60, 60, 60, 60, 60},
		},
		[DDR_1866] = {
			.read_coef = {108, 204, 288, 384,
	456, 852, 1236, 1620, 1788, 2028,
	2340, 2736, 3120, 3420, 3662, 3834},
			.write_coef = {12, 24, 36, 60,
	60, 60, 60, 60, 60, 60,
	60, 60, 60, 60, 60, 60},
		},
		[DDR_2133] = {
			.read_coef = {120, 216, 288, 408,
	468, 852, 1224, 1668, 1788, 2064,
	2352, 2712, 3096, 3408, 3617, 3760},
			.write_coef = {12, 24, 36, 60,
	60, 60, 60, 60, 60, 60,
	60, 60, 60, 60, 60, 60},
		},
	},
	.coef_idle = {444, 449, 482, 516, 972, 1056, 1272, 1489},
	},
};

static struct dram_pwr_conf dram_def_pwr_conf[] = {
	[DRAM_VDD1_1P8V] = {
		/* SW co-relation */
		.i_dd0 = 3000,
		.i_dd2p = 350,
		.i_dd2n = 430,
		.i_dd4r = 5280,
		.i_dd4w = 5580,
		.i_dd5 = 800,
		.i_dd6 = 218,
	},
	[DRAM_VDD2_1P1V] = {
		/* SW co-relation */
		.i_dd0 = 27714,
		.i_dd2p = 1380,
		.i_dd2n = 4012,
		.i_dd4r = 120000,
		.i_dd4w = 115619,
		.i_dd5 = 10600,
		.i_dd6 = 266,
	},
	[DRAM_VDDQ_0P6V] = {
		.i_dd0 = 980,
		.i_dd2p = 75,
		.i_dd2n = 75,
		.i_dd4r = 58138,
		.i_dd4w = 511,
		.i_dd5 = 600,
		.i_dd6 = 36,
	},
};

/* subsys share mem reference table */
static struct swpm_mem_ref_tbl mem_ref_tbl[NR_POWER_METER] = {
	[CPU_POWER_METER] = {0, NULL},
	[GPU_POWER_METER] = {0, NULL},
	[CORE_POWER_METER] = {0, NULL},
	[MEM_POWER_METER] = {0, NULL},
	[ISP_POWER_METER] = {0, NULL},
	[ME_POWER_METER] = {0, NULL},
};

/****************************************************************************
 *  Static Function
 ****************************************************************************/
static void swpm_send_enable_ipi(unsigned int type, unsigned int enable)
{
#if defined(CONFIG_MTK_TINYSYS_SSPM_SUPPORT) &&		\
	defined(CONFIG_MTK_QOS_FRAMEWORK)
	struct qos_ipi_data qos_d;

	qos_d.cmd = QOS_IPI_SWPM_ENABLE;
	qos_d.u.swpm_enable.type = type;
	qos_d.u.swpm_enable.enable = enable;
	qos_ipi_to_sspm_command(&qos_d, 3);
#endif
}

static void swpm_pmu_start(int cpu)
{
	struct perf_event *l3_event = per_cpu(l3dc_events, cpu);
	struct perf_event *i_event = per_cpu(inst_spec_events, cpu);
	struct perf_event *c_event = per_cpu(cycle_events, cpu);

	if (l3_event)
		perf_event_enable(l3_event);
	if (i_event)
		perf_event_enable(i_event);
	if (c_event)
		perf_event_enable(c_event);
}

static void swpm_pmu_stop(int cpu)
{
	struct perf_event *l3_event = per_cpu(l3dc_events, cpu);
	struct perf_event *i_event = per_cpu(inst_spec_events, cpu);
	struct perf_event *c_event = per_cpu(cycle_events, cpu);

	if (l3_event)
		perf_event_disable(l3_event);
	if (i_event)
		perf_event_disable(i_event);
	if (c_event)
		perf_event_disable(c_event);
}

static void swpm_pmu_set_enable(int cpu, int enable)
{
	struct perf_event *event;
	struct perf_event *l3_event = per_cpu(l3dc_events, cpu);
	struct perf_event *i_event = per_cpu(inst_spec_events, cpu);
	struct perf_event *c_event = per_cpu(cycle_events, cpu);

	if (enable) {
		if (!l3_event) {
			event = perf_event_create_kernel_counter(
				&l3dc_event_attr, cpu, NULL, NULL, NULL);
			if (IS_ERR(event)) {
				swpm_err("create l3dc counter error!\n");
				return;
			}
			per_cpu(l3dc_events, cpu) = event;
		}
		if (!i_event && cpu >= NR_CPU_L_CORE) {
			event = perf_event_create_kernel_counter(
				&inst_spec_event_attr, cpu, NULL, NULL, NULL);
			if (IS_ERR(event)) {
				swpm_err("create inst_spec counter error!\n");
				return;
			}
			per_cpu(inst_spec_events, cpu) = event;
		}
		if (!c_event && cpu >= NR_CPU_L_CORE) {
			event = perf_event_create_kernel_counter(
				&cycle_event_attr, cpu,	NULL, NULL, NULL);
			if (IS_ERR(event)) {
				swpm_err("create cycle counter error!\n");
				return;
			}
			per_cpu(cycle_events, cpu) = event;
		}

		swpm_pmu_start(cpu);
	} else {
		swpm_pmu_stop(cpu);

		if (l3_event) {
			per_cpu(l3dc_events, cpu) = NULL;
			perf_event_release_kernel(l3_event);
		}
		if (i_event) {
			per_cpu(inst_spec_events, cpu) = NULL;
			perf_event_release_kernel(i_event);
		}
		if (c_event) {
			per_cpu(cycle_events, cpu) = NULL;
			perf_event_release_kernel(c_event);
		}
	}
}

static void swpm_pmu_set_enable_all(unsigned int enable)
{
	int i;
	unsigned int swpm_pmu_user, swpm_pmu_en;

	swpm_pmu_user = enable >> SWPM_CODE_USER_BIT;
	swpm_pmu_en = enable & 0x1;

	if (swpm_pmu_user > NR_SWPM_PMU_USER) {
		swpm_err("pmu_user invalid = %d\n",
			 swpm_pmu_user);
		return;
	}

	swpm_lock(&swpm_pmu_mutex);
	get_online_cpus();
	if (swpm_pmu_en) {
		if (!swpm_pmu_sta) {
#ifdef CONFIG_MTK_CACHE_CONTROL
			ca_force_stop_set_in_kernel(1);
#endif
			for (i = 0; i < num_possible_cpus(); i++)
				swpm_pmu_set_enable(i, swpm_pmu_en);
		}
		if (!swpm_pmu_get_sta(swpm_pmu_user))
			swpm_pmu_set_sta(swpm_pmu_user);

	} else {
		if (swpm_pmu_get_sta(swpm_pmu_user))
			swpm_pmu_clr_sta(swpm_pmu_user);

		if (!swpm_pmu_sta) {
			for (i = 0; i < num_possible_cpus(); i++)
				swpm_pmu_set_enable(i, swpm_pmu_en);
#ifdef CONFIG_MTK_CACHE_CONTROL
			ca_force_stop_set_in_kernel(0);
#endif
		}
	}
	put_online_cpus();
	swpm_unlock(&swpm_pmu_mutex);

	swpm_err("pmu_enable: %d, user_sta: %d\n",
		 swpm_pmu_en, swpm_pmu_sta);
}

#ifdef CONFIG_MTK_TINYSYS_SSPM_SUPPORT
static void swpm_send_init_ipi(unsigned int addr, unsigned int size,
			      unsigned int ch_num)
{
#if defined(CONFIG_MTK_QOS_FRAMEWORK)
	struct qos_ipi_data qos_d;
	struct share_wrap *wrap_d;
	unsigned int offset;

	qos_d.cmd = QOS_IPI_SWPM_INIT;
	qos_d.u.swpm_init.dram_addr = addr;
	qos_d.u.swpm_init.dram_size = size;
	qos_d.u.swpm_init.dram_ch_num = ch_num;
	offset = qos_ipi_to_sspm_command(&qos_d, 4);

	if (offset == -1) {
		swpm_err("qos ipi not ready init fail\n");
		goto error;
	} else if (offset == 0) {
		swpm_err("swpm share sram init fail\n");
		goto error;
	}

	/* get wrapped sram address */
	wrap_d = (struct share_wrap *)
		sspm_sbuf_get(offset);

	/* exception control for illegal sbuf request */
	if (!wrap_d) {
		swpm_err("swpm share sram offset fail\n");
		goto error;
	}

	/* get sram power index and control address from wrap data */
	share_idx_ref = (struct share_index *)
		sspm_sbuf_get(wrap_d->share_index_addr);
	share_idx_ctrl = (struct share_ctrl *)
		sspm_sbuf_get(wrap_d->share_ctrl_addr);

	/* init extension index address */
	swpm_sp_init(sspm_sbuf_get(wrap_d->share_index_ext_addr),
		     sspm_sbuf_get(wrap_d->share_ctrl_ext_addr));

	/* init unsigned int ptr for share index iterator */
	idx_ref_uint_ptr = (unsigned int *)share_idx_ref;
	/* init output size to LTR except window_cnt with decrease 1 */
	idx_output_size =
		(sizeof(struct share_index)/sizeof(unsigned int)) - 1;

	swpm_err("share_index size check = %zu\n",
		 sizeof(struct share_index));
#if SWPM_TEST
	swpm_err("wrap_d = 0x%p, wrap index addr = 0x%p, wrap ctrl addr = 0x%p\n",
		 wrap_d, wrap_d->share_index_addr, wrap_d->share_ctrl_addr);
	swpm_err("share_idx_ref = 0x%p, share_idx_ctrl = 0x%p\n",
		 share_idx_ref, share_idx_ctrl);
#endif
#endif
	swpm_init_state = 1;
	return;

error:
	swpm_init_state = 0;
	share_idx_ref = NULL;
	share_idx_ctrl = NULL;
	idx_ref_uint_ptr = NULL;
	idx_output_size = 0;
}

static inline void swpm_pass_to_sspm(void)
{
#ifdef CONFIG_MTK_DRAMC
	swpm_send_init_ipi((unsigned int)(rec_phys_addr & 0xFFFFFFFF),
		(unsigned int)(rec_size & 0xFFFFFFFF), get_emi_ch_num());
#else
	swpm_send_init_ipi((unsigned int)(rec_phys_addr & 0xFFFFFFFF),
		(unsigned int)(rec_size & 0xFFFFFFFF), 2);
#endif
}

static void swpm_core_thermal_cb(void)
{
#ifdef CONFIG_THERMAL
#if CFG_THERM_LVTS
	int temp, infra_temp, cam_temp;

	if (!core_ptr)
		return;

	infra_temp = get_immediate_tslvts6_0_wrap();
	cam_temp = get_immediate_tslvts6_1_wrap();

	temp = (infra_temp + cam_temp) / 2;

	/* truncate negative deg */
	temp = (temp < 0) ? 0 : temp;
	core_ptr->thermal = (unsigned int)temp;

#if SWPM_TEST
	swpm_err("swpm_core infra lvts6_0 = %d\n", infra_temp);
	swpm_err("swpm_core cam lvts6_1 = %d\n", cam_temp);
	swpm_err("swpm_core cpuL = %d\n", get_immediate_cpuL_wrap());
	swpm_err("swpm_core cpuB = %d\n", get_immediate_cpuB_wrap());
#endif
#endif /* CFG_THERM_LVTS */
#endif
}

#ifdef CONFIG_THERMAL
static unsigned int swpm_get_cpu_temp(enum cpu_lkg_type type)
{
	unsigned int temp = 0;

	switch (type) {
	case CPU_L_LKG:
		temp = get_immediate_cpuL_wrap() / 1000;
		break;
	case CPU_B_LKG:
		temp = get_immediate_cpuB_wrap() / 1000;
		break;
	case DSU_LKG:
	default:
		temp = get_immediate_cpuL_wrap() / 1000;
		break;
	}

	return temp;
}
#endif

#ifdef CONFIG_MTK_STATIC_POWER
static int swpm_get_spower_devid(enum cpu_lkg_type type)
{
	int devid;

	switch (type) {
	case CPU_L_LKG:
		devid = MTK_SPOWER_CPULL;
		break;
	case CPU_B_LKG:
		devid = MTK_SPOWER_CPUL;
		break;
	case CPU_BB_LKG:
		devid = MTK_SPOWER_CPUB;
		break;
	case DSU_LKG:
	default:
		devid = MTK_SPOWER_CCI;
		break;
	}

	return devid;
}
#endif

static void swpm_update_lkg_table(void)
{
	int temp, dev_id, volt, i, j;
	unsigned int lkg = 0;

	for (i = 0; i < NR_CPU_LKG_TYPE; i++) {
#ifdef CONFIG_THERMAL
		temp = swpm_get_cpu_temp((enum cpu_lkg_type)i);
#else
		temp = 85;
#endif
		for (j = 0; j < NR_CPU_OPP; j++) {
#ifdef CONFIG_MTK_CPU_FREQ
			volt = mt_cpufreq_get_volt_by_idx(i, j) / 100;
#else
			volt = 1000;
#endif
#ifdef CONFIG_MTK_STATIC_POWER
			dev_id = swpm_get_spower_devid((enum cpu_lkg_type)i);
#ifdef GET_UW_LKG
			lkg = mt_spower_get_leakage_uW(dev_id, volt, temp);
#else
			lkg = mt_spower_get_leakage(dev_id, volt, temp) * 1000;
#endif
#else
			dev_id = 0;
			lkg = 5000;
#endif
			if (swpm_info_ref)
				swpm_info_ref->cpu_lkg_pwr[i][j] = lkg;
		}
	}
#ifdef CONFIG_THERMAL
	//get gpu lkg and thermal
	if (swpm_info_ref) {
		swpm_info_ref->gpu_reserved[gthermal + 1] =
			get_immediate_gpu_wrap() / 1000;
		swpm_info_ref->gpu_reserved[glkg + 1] =
			mt_gpufreq_get_leakage_no_lock();
	}
#endif

	swpm_core_thermal_cb();
}

static void swpm_idx_snap(void)
{
	if (share_idx_ref) {
		swpm_lock(&swpm_snap_lock);
		/* directly copy due to 8 bytes alignment problem */
		mem_idx_snap.read_bw[0] = share_idx_ref->mem_idx.read_bw[0];
		mem_idx_snap.read_bw[1] = share_idx_ref->mem_idx.read_bw[1];
		mem_idx_snap.write_bw[0] = share_idx_ref->mem_idx.write_bw[0];
		mem_idx_snap.write_bw[1] = share_idx_ref->mem_idx.write_bw[1];
		swpm_unlock(&swpm_snap_lock);
	}
}

static char idx_buf[POWER_INDEX_CHAR_SIZE] = { 0 };

static void swpm_log_loop(unsigned long data)
{
	char buf[256] = {0};
	char *ptr = buf;
	char *idx_ptr = idx_buf;
	int i;
#ifdef LOG_LOOP_TIME_PROFILE
	ktime_t t1, t2;
	unsigned long long diff, diff2;

	t1 = ktime_get();
#endif

	/* initialization retry */
	if (!swpm_init_state) {
		swpm_pass_to_sspm();
	}

	for (i = 0; i < NR_POWER_RAIL; i++) {
		if ((1 << i) & swpm_log_mask) {
			ptr += snprintf(ptr, 256, "%s/",
				swpm_power_rail_to_string((enum power_rail)i));
		}
	}
	ptr--;
	ptr += sprintf(ptr, " = ");

	for (i = 0; i < NR_POWER_RAIL; i++) {
		if ((1 << i) & swpm_log_mask) {
			ptr += snprintf(ptr, 256, "%d/",
				swpm_get_avg_power((enum power_rail)i, 50));
		}
	}
	ptr--;
	ptr += sprintf(ptr, " uA");

	/* for LTR */
	if (share_idx_ref && share_idx_ctrl) {
		memset(idx_buf, 0, sizeof(char) * POWER_INDEX_CHAR_SIZE);
		/* exclude window_cnt */
		for (i = 0; i < idx_output_size; i++) {
			idx_ptr += snprintf(idx_ptr, POWER_INDEX_CHAR_SIZE,
					    "%d,", *(idx_ref_uint_ptr+i));
		}
#if SWPM_TEST
		idx_ptr--;
		idx_ptr += snprintf(idx_ptr, POWER_INDEX_CHAR_SIZE,
				    " window_cnt = %d",
				    share_idx_ref->window_cnt);
#endif

		/* snapshot the last completed average index data */
		swpm_idx_snap();

		/* set share sram clear flag and release lock */
		share_idx_ctrl->clear_flag = 1;

		/* put power index data to ftrace */
		trace_swpm_power_idx(idx_buf);
	}
	/* put power data to ftrace */
	trace_swpm_power(buf);

#ifdef LOG_LOOP_TIME_PROFILE
	t2 = ktime_get();
#endif

	swpm_update_lkg_table();

#ifdef LOG_LOOP_TIME_PROFILE
	diff = ktime_to_us(ktime_sub(t2, t1));
	diff2 = ktime_to_us(ktime_sub(ktime_get(), t2));
	swpm_err("exe time = %llu/%lluus\n", diff, diff2);
#endif

	swpm_update_periodic_timer();
}

#endif /* #ifdef CONFIG_MTK_TINYSYS_SSPM_SUPPORT : 573 */

static void swpm_core_static_data_init(void)
{
#ifdef CONFIG_MTK_STATIC_POWER
	unsigned int lkg, lkg_scaled;
#endif
	unsigned int i, j;

	if (!core_ptr)
		return;

#ifdef CONFIG_MTK_STATIC_POWER
	/* init core static power once */
	lkg = mt_spower_get_efuse_lkg(MTK_SPOWER_VCORE);
#else
	lkg = 0;
#endif

	/* default CORE_DEFAULT_LKG mA, efuse default mW to mA */
	lkg = (!lkg) ? CORE_DEFAULT_LKG
			: (lkg * 1000 / V_OF_FUSE_VCORE);
	/* recording default lkg data, and check replacement data */
	core_lkg = lkg;
	lkg = (!core_lkg_replaced) ? lkg : core_lkg_replaced;

	/* efuse static power unit mW with voltage scaling */
	for (i = 0; i < NR_CORE_VOLT; i++) {
		lkg_scaled = lkg * core_volt_tbl[i] / V_OF_FUSE_VCORE;
		for (j = 0; j < NR_CORE_LKG_TYPE; j++) {
			/* unit (uA) */
			core_ptr->core_lkg_pwr[i][j] =
				lkg_scaled * core_lkg_rt[j] / CORE_LKG_RT_RES;
		}
	}
}

static void swpm_core_pwr_data_init(void)
{
	if (!core_ptr)
		return;

	/* copy core volt data */
	memcpy(core_ptr->core_volt_tbl, core_volt_tbl,
	       sizeof(core_volt_tbl));
	/* copy aphy core pwr data */
	memcpy(core_ptr->aphy_core_bw_tbl, aphy_ref_core_bw_tbl,
	       sizeof(aphy_ref_core_bw_tbl));
	memcpy(core_ptr->aphy_core_pwr_tbl, aphy_def_core_pwr_tbl,
	       sizeof(aphy_def_core_pwr_tbl));

	swpm_info("aphy core_bw[%ld]/core[%ld]/core_volt[%ld]\n",
		  (unsigned long)sizeof(aphy_ref_core_bw_tbl),
		  (unsigned long)sizeof(aphy_def_core_pwr_tbl),
		  (unsigned long)sizeof(core_volt_tbl));
}

static void swpm_mem_pwr_data_init(void)
{
	if (!mem_ptr)
		return;

	/* copy aphy others pwr data */
	memcpy(mem_ptr->aphy_others_bw_tbl, aphy_ref_others_bw_tbl,
	       sizeof(aphy_ref_others_bw_tbl));
	memcpy(mem_ptr->aphy_others_pwr_tbl, aphy_def_others_pwr_tbl,
	       sizeof(aphy_def_others_pwr_tbl));
	/* copy dram pwr data */
	memcpy(mem_ptr->dram_conf, dram_def_pwr_conf,
	       sizeof(dram_def_pwr_conf));
	memcpy(mem_ptr->ddr_opp_freq, ddr_opp_freq,
	       sizeof(ddr_opp_freq));
}

static void swpm_me_pwr_data_init(void)
{
	if (!me_ptr)
		return;

	me_ptr->disp_resolution = 0;
	me_ptr->disp_fps = 0;
	me_ptr->disp_active = 1;
	me_ptr->venc_freq = 0;
	me_ptr->venc_active = 0;
	me_ptr->vdec_freq = 0;
	me_ptr->vdec_active = 0;


	swpm_info("ME disp_resolution=%d disp_fps=%d\n",
		me_ptr->disp_resolution, me_ptr->disp_fps);
}

static void swpm_init_pwr_data(void)
{
	int ret;
	phys_addr_t *ptr = NULL;

	ret = swpm_mem_addr_request(CORE_SWPM_TYPE, &ptr);
	if (!ret)
		core_ptr = (struct core_swpm_rec_data *)ptr;

	ret = swpm_mem_addr_request(MEM_SWPM_TYPE, &ptr);
	if (!ret)
		mem_ptr = (struct mem_swpm_rec_data *)ptr;

	ret = swpm_mem_addr_request(ME_SWPM_TYPE, &ptr);
	if (!ret)
		me_ptr = (struct me_swpm_rec_data *)ptr;

	swpm_core_static_data_init();
	swpm_core_pwr_data_init();
	swpm_mem_pwr_data_init();
	swpm_me_pwr_data_init();
}

#if SWPM_TEST
/* self value checking for mem reference table */
static inline void swpm_interface_unit_test(void)
{
	int i, ret;
	phys_addr_t *ptr = NULL;
	struct isp_swpm_rec_data *isp_ptr;

	for (i = 0; i < NR_SWPM_TYPE; i++) {
		ret = swpm_mem_addr_request((enum swpm_type)i, &ptr);

		if (!ret) {
			swpm_err("swpm_mem_tbl[%d] = 0x%x\n", i, ptr);

			/* data access test */
			switch (i) {
			case ISP_POWER_METER:
				isp_ptr = (struct isp_swpm_rec_data *)ptr;
				isp_ptr->isp_data[2] = 0x123;
				break;
			}
		} else {
			swpm_err("swpm_mem_tbl[%d] = NULL\n", i);
		}
	}
}
#endif

/* init share mem reference table for subsys */
static inline void swpm_subsys_data_ref_init(void)
{
	swpm_lock(&swpm_mutex);

	mem_ref_tbl[MEM_POWER_METER].valid = true;
	mem_ref_tbl[MEM_POWER_METER].virt =
		(phys_addr_t *)&swpm_info_ref->mem_reserved;
	mem_ref_tbl[CORE_POWER_METER].valid = true;
	mem_ref_tbl[CORE_POWER_METER].virt =
		(phys_addr_t *)&swpm_info_ref->core_reserved;
	mem_ref_tbl[GPU_POWER_METER].valid = true;
	mem_ref_tbl[GPU_POWER_METER].virt =
		(phys_addr_t *)&swpm_info_ref->gpu_reserved;
	mem_ref_tbl[ISP_POWER_METER].valid = true;
	mem_ref_tbl[ISP_POWER_METER].virt =
		(phys_addr_t *)&swpm_info_ref->isp_reserved;
	mem_ref_tbl[ME_POWER_METER].valid = true;
	mem_ref_tbl[ME_POWER_METER].virt =
		(phys_addr_t *)&swpm_info_ref->me_reserved;

	swpm_unlock(&swpm_mutex);
}

static char *_copy_from_user_for_proc(const char __user *buffer, size_t count)
{
	static char buf[64];
	unsigned int len = 0;

	len = (count < (sizeof(buf) - 1)) ? count : (sizeof(buf) - 1);

	if (copy_from_user(buf, buffer, len))
		return NULL;

	buf[len] = '\0';

	return buf;
}

static int dram_bw_proc_show(struct seq_file *m, void *v)
{
	swpm_lock(&swpm_snap_lock);
	seq_printf(m, "DRAM BW [N]R/W=%d/%d,[S]R/W=%d/%d\n",
		   mem_idx_snap.read_bw[0],
		   mem_idx_snap.write_bw[0],
		   mem_idx_snap.read_bw[1],
		   mem_idx_snap.write_bw[1]);
	swpm_unlock(&swpm_snap_lock);

	return 0;
}

static int idd_tbl_proc_show(struct seq_file *m, void *v)
{
	int i;

	if (!mem_ptr)
		return 0;

	for (i = 0; i < NR_DRAM_PWR_TYPE; i++) {
		seq_puts(m, "==========================\n");
		seq_printf(m, "idx %d i_dd0 = %d\n", i,
			mem_ptr->dram_conf[i].i_dd0);
		seq_printf(m, "idx %d i_dd2p = %d\n", i,
			mem_ptr->dram_conf[i].i_dd2p);
		seq_printf(m, "idx %d i_dd2n = %d\n", i,
			mem_ptr->dram_conf[i].i_dd2n);
		seq_printf(m, "idx %d i_dd4r = %d\n", i,
			mem_ptr->dram_conf[i].i_dd4r);
		seq_printf(m, "idx %d i_dd4w = %d\n", i,
			mem_ptr->dram_conf[i].i_dd4w);
		seq_printf(m, "idx %d i_dd5 = %d\n", i,
			mem_ptr->dram_conf[i].i_dd5);
		seq_printf(m, "idx %d i_dd6 = %d\n", i,
			mem_ptr->dram_conf[i].i_dd6);
	}
	seq_puts(m, "==========================\n");

	return 0;
}
static ssize_t idd_tbl_proc_write(struct file *file,
	const char __user *buffer, size_t count, loff_t *pos)
{
	unsigned int type, idd_idx, val;

	char *buf = _copy_from_user_for_proc(buffer, count);

	if (!buf)
		return -EINVAL;

	if (!mem_ptr)
		goto end;

	if (swpm_status) {
		swpm_err("disable swpm for data change, need to restart manually\n");
		swpm_set_enable(ALL_METER_TYPE, 0);
	}

	if (sscanf(buf, "%d %d %d", &type, &idd_idx, &val) == 3) {
		if (type < (NR_DRAM_PWR_TYPE) &&
		    idd_idx < (sizeof(struct dram_pwr_conf) /
			       sizeof(unsigned int)))
			*(&mem_ptr->dram_conf[type].i_dd0 + idd_idx) = val;
	} else {
		swpm_err("echo <type> <idx> <val> > /proc/swpm/idd_tbl\n");
	}

end:
	return count;
}

static unsigned int pmu_ms_mode;
static int pmu_ms_mode_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "%d\n", pmu_ms_mode);
	return 0;
}
static ssize_t pmu_ms_mode_proc_write(struct file *file,
	const char __user *buffer, size_t count, loff_t *pos)
{
	unsigned int enable = 0;
	char *buf = _copy_from_user_for_proc(buffer, count);

	if (!buf)
		return -EINVAL;

	if (!kstrtouint(buf, 10, &enable)) {
		pmu_ms_mode = enable;

		/* TODO: remove this path after qos commander ready */
		swpm_set_update_cnt(0, (0x1 << SWPM_CODE_USER_BIT) |
				    pmu_ms_mode);
	} else
		swpm_err("echo <0/1> > /proc/swpm/pmu_ms_mode\n");

	return count;
}

static int core_static_replace_proc_show(struct seq_file *m, void *v)
{
	seq_printf(m, "default: %d, replaced %d (valid:0~999)\n",
		   core_lkg,
		   core_lkg_replaced);
	return 0;
}
static ssize_t core_static_replace_proc_write(struct file *file,
	const char __user *buffer, size_t count, loff_t *pos)
{
	unsigned int val = 0;
	char *buf = _copy_from_user_for_proc(buffer, count);

	if (!buf)
		return -EINVAL;

	if (!kstrtouint(buf, 10, &val)) {
		core_lkg_replaced = (val < 1000) ? val : core_lkg_replaced;

		/* reset core static power data */
		swpm_core_static_data_init();
	} else
		swpm_err("echo <val> > /proc/swpm/core_static_replace\n");

	return count;
}

PROC_FOPS_RW(idd_tbl);
PROC_FOPS_RO(dram_bw);
PROC_FOPS_RW(pmu_ms_mode);
PROC_FOPS_RW(core_static_replace);

static void swpm_cmd_dispatcher(unsigned int type,
				unsigned int val)
{
	switch (type) {
	case SET_PMU:
		swpm_pmu_set_enable_all(val);
		break;
	}
}

static struct swpm_core_internal_ops plat_ops = {
	.cmd = swpm_cmd_dispatcher,
};

/***************************************************************************
 *  API
 ***************************************************************************/
void swpm_set_enable(unsigned int type, unsigned int enable)
{
	if (!swpm_init_state
	    || (type != ALL_METER_TYPE && type >= NR_POWER_METER))
		return;

	if (type == ALL_METER_TYPE) {
		int i;

		for_each_pwr_mtr(i) {
			if (enable) {
				if (swpm_get_status(i))
					continue;

				if (i == CPU_POWER_METER)
					swpm_pmu_enable(SWPM_PMU_INTERNAL, 1);
				swpm_set_status(i);
			} else {
				if (!swpm_get_status(i))
					continue;

				if (i == CPU_POWER_METER)
					swpm_pmu_enable(SWPM_PMU_INTERNAL, 0);
				swpm_clr_status(i);
			}
		}
		swpm_send_enable_ipi(type, enable);
	} else if (type < NR_POWER_METER) {
		if (enable && !swpm_get_status(type)) {
			if (type == CPU_POWER_METER)
				swpm_pmu_enable(SWPM_PMU_INTERNAL, 1);
			swpm_set_status(type);
		} else if (!enable && swpm_get_status(type)) {
			if (type == CPU_POWER_METER)
				swpm_pmu_enable(SWPM_PMU_INTERNAL, 0);
			swpm_clr_status(type);
		}
		swpm_send_enable_ipi(type, enable);
	}
}

/* TODO: move to static internal use */
char *swpm_power_rail_to_string(enum power_rail p)
{
	char *s;

	switch (p) {
	case VPROC2:
		s = "VPROC2";
		break;
	case VPROC1:
		s = "VPROC1";
		break;
	case VGPU:
		s = "VGPU";
		break;
	case VCORE:
		s = "VCORE";
		break;
	case VDRAM:
		s = "VDRAM";
		break;
	case VIO18_DDR:
		s = "VIO18_DDR";
		break;
	case VIO18_DRAM:
		s = "VIO18_DRAM";
		break;
	default:
		s = "None";
		break;
	}

	return s;
}

void swpm_set_update_cnt(unsigned int type, unsigned int cnt)
{
	if (!swpm_init_state
	    || (type != ALL_METER_TYPE && type >= NR_POWER_METER))
		return;

	swpm_lock(&swpm_mutex);
#if defined(CONFIG_MTK_TINYSYS_SSPM_SUPPORT) &&		\
	defined(CONFIG_MTK_QOS_FRAMEWORK)
	{
		struct qos_ipi_data qos_d;

		qos_d.cmd = QOS_IPI_SWPM_SET_UPDATE_CNT;
		qos_d.u.swpm_set_update_cnt.type = type;
		qos_d.u.swpm_set_update_cnt.cnt = cnt;
		qos_ipi_to_sspm_command(&qos_d, 3);
	}
#endif
	swpm_unlock(&swpm_mutex);
}

static void swpm_platform_procfs(void)
{
	struct swpm_entry idd_tbl = PROC_ENTRY(idd_tbl);
	struct swpm_entry dram_bw = PROC_ENTRY(dram_bw);
	struct swpm_entry pmu_mode = PROC_ENTRY(pmu_ms_mode);
	struct swpm_entry core_lkg_rp = PROC_ENTRY(core_static_replace);

	swpm_append_procfs(&idd_tbl);
	swpm_append_procfs(&dram_bw);
	swpm_append_procfs(&pmu_mode);
	swpm_append_procfs(&core_lkg_rp);
}

static int __init swpm_platform_init(void)
{
	int ret = 0;

#ifdef BRINGUP_DISABLE
	swpm_err("swpm is disabled\n");
	goto end;
#endif

	swpm_create_procfs();

	swpm_platform_procfs();

#ifdef CONFIG_MTK_TINYSYS_SSPM_SUPPORT
	swpm_get_rec_addr(&rec_phys_addr,
			  &rec_virt_addr,
			  &rec_size);

	swpm_info_ref = (struct swpm_rec_data *)(uintptr_t)rec_virt_addr;
#endif

	if (!swpm_info_ref) {
		swpm_err("get sspm dram addr failed\n");
		ret = -1;
		goto end;
	}

#ifdef CONFIG_MTK_TINYSYS_SSPM_SUPPORT
	ret |= swpm_reserve_mem_init(&rec_virt_addr, &rec_size);
#endif

	swpm_subsys_data_ref_init();

	ret |= swpm_interface_manager_init(mem_ref_tbl, NR_SWPM_TYPE);

	swpm_init_pwr_data();

	swpm_core_ops_register(&plat_ops);

#if SWPM_TEST
	swpm_interface_unit_test();
#endif

	swpm_pmu_enable(SWPM_PMU_CPU_DVFS, 1);

#ifdef CONFIG_MTK_TINYSYS_SSPM_SUPPORT
	swpm_pass_to_sspm();

	/* set preiodic timer task */
	swpm_set_periodic_timer(swpm_log_loop);
#endif

#if SWPM_TEST
	/* enable all pwr meter and set swpm timer to start */
	swpm_set_enable(ALL_METER_TYPE, EN_POWER_METER_ONLY);
	swpm_update_periodic_timer();
#endif
end:
	return ret;
}
late_initcall_sync(swpm_platform_init)

static void __exit swpm_platform_exit(void)
{
	swpm_set_enable(ALL_METER_TYPE, 0);
	swpm_sp_exit();
}
module_exit(swpm_platform_exit)

