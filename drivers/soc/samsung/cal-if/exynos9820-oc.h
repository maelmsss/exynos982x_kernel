/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __EXYNOS9820_OC_H__
#define __EXYNOS9820_OC_H__

#define EXYNOS9820_M4_STOCK_KHZ		2730000U
#define EXYNOS9820_M4_OC_KHZ		2912000U
#define EXYNOS9820_M4_OC_HIGH_KHZ	3016000U

/* 26 MHz reference, P=4, S=0. M is the high byte pair. */
#define EXYNOS9820_GPU_PMS_702		0x006c0400U
#define EXYNOS9820_GPU_PMS_754		0x00740400U
#define EXYNOS9820_GPU_PMS_806		0x007c0400U

bool exynos9820_oc_defer_big(void);
bool exynos9820_oc_defer_g3d(void);
bool exynos9820_oc_big_enabled(void);
bool exynos9820_oc_big_keep_uv(void);
bool exynos9820_oc_gpu_enabled(void);
void exynos9820_oc_init(void __iomem *fvmap, void __iomem *sram);
unsigned int exynos9820_gpu_oc_rate(unsigned int original_khz);
unsigned int exynos9820_gpu_oc_steps(unsigned int top_khz, unsigned int *steps,
		unsigned int max);
unsigned long exynos9820_gpu_cal_rate(unsigned int id, unsigned long rate);
unsigned long exynos9820_gpu_cal_done(unsigned int id, unsigned long programmed);
void exynos9820_gpu_cal_abort(void);
unsigned long exynos9820_gpu_cal_get(unsigned int id, unsigned long rate);
int exynos9820_cluster_oc_override(unsigned int id, unsigned int dt_max);
int exynos9820_m4_oc_override(unsigned int id, unsigned int dt_max);
unsigned int exynos9820_oc_cpu_floor(void);
unsigned int exynos9820_cluster_oc_thermal_limit(const char *zone,
		unsigned int range, int temperature, unsigned int limit_khz);
bool exynos9820_cpu_margin_for_freq(unsigned int margin_id, unsigned int khz,
		int *percent);
bool exynos9820_gpu_margin_for_freq(unsigned int khz, int *percent);

#endif
