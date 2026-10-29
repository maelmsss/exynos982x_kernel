// SPDX-License-Identifier: GPL-2.0
/*
 * Opt-in Exynos 9820/9825 OPP unlock.
 *
 * The stock GPU top stays in the DVFS table. 806 MHz is an extra row at
 * the same voltage. Its PLL multiplier is written only while that clock
 * is selected, then restored. Firmware rate rows are not renamed.
 * Either path aborts unless the firmware table matches. A missing 3016
 * row does not leave the M4 at the 2080 DTS cap: the highest fused rate
 * already in the table is used instead, and the -5% big margin stays.
 * A 2912/3016 row keeps its own voltage only while that clock is selected.
 * Every slower big clock keeps the -5% margin. GPU does the same: -3%
 * below 806 MHz, and the stock top voltage at 806 MHz.
 * No rail value is invented.
 */

#include <linux/kernel.h>
#include <linux/of.h>
#include <linux/io.h>
#include <linux/slab.h>
#include <linux/kobject.h>
#include <linux/sysfs.h>

#include "fvmap.h"
#include "cmucal.h"
#include "vclk.h"
#include "exynos9820-oc.h"

#define OC_FVMAP_SIZE		(SZ_8K)

static void __iomem *oc_fvmap;
static void __iomem *oc_sram;

static bool cpucl2_oc_requested;
static bool cpucl2_oc_enabled;
static bool cpucl2_keep_uv = true;
static bool cpucl2_uv_switch;
static unsigned int cpucl2_oc_target = EXYNOS9820_M4_OC_KHZ;
static unsigned int cpucl2_stock_khz;
static unsigned int cpucl2_target_uv;
static const char *cpucl2_status = "not-initialised";

static bool cluster_oc_requested;
static unsigned int gpu_oc_requested;
static bool gpu_oc_enabled;
static unsigned int gpu_oc_from;
static unsigned int gpu_oc_voltage;
static const char *gpu_status = "disabled";
static void __iomem *gpu_pms_slot;
static u32 gpu_pms_stock;
static u32 gpu_pms_oc;
static bool gpu_pms_latched;

struct cluster_oc_state {
	unsigned int margin_id;
	const char *name;
	unsigned int target_khz;
	unsigned int stock_khz;
	unsigned int target_uv;
	bool enabled;
	const char *status;
};

static struct cluster_oc_state cluster_oc[] = {
	{ .margin_id = MARGIN_LIT, .name = "little", .target_khz = 2106000,
	  .status = "not-initialised" },
	{ .margin_id = MARGIN_MID, .name = "mid", .target_khz = 2400000,
	  .status = "not-initialised" },
};

static int __init get_gpu_oc(char *str)
{
	return kstrtouint(str, 0, &gpu_oc_requested);
}
early_param("exynos9820_gpu_oc", get_gpu_oc);

static bool m4_target_valid(unsigned int target)
{
	return target == EXYNOS9820_M4_OC_KHZ ||
		target == EXYNOS9820_M4_OC_HIGH_KHZ;
}

static int __init get_cpucl2_oc(char *str)
{
	unsigned int value;
	int ret = kstrtouint(str, 10, &value);

	if (ret)
		return ret;
	if (value > 1 && !m4_target_valid(value))
		return -EINVAL;
	cpucl2_oc_requested = value != 0;
	if (value > 1)
		cpucl2_oc_target = value;
	return 0;
}
early_param("exynos9820_cpu_oc", get_cpucl2_oc);

static int __init get_cluster_oc(char *str)
{
	return kstrtobool(str, &cluster_oc_requested);
}
early_param("exynos9820_cluster_oc", get_cluster_oc);

bool exynos9820_oc_defer_big(void)
{
	return cpucl2_oc_requested;
}

bool exynos9820_oc_defer_g3d(void)
{
	return gpu_oc_requested != 0;
}

bool exynos9820_oc_big_enabled(void)
{
	return cpucl2_oc_enabled;
}

bool exynos9820_oc_big_keep_uv(void)
{
	return cpucl2_keep_uv;
}

bool exynos9820_oc_gpu_enabled(void)
{
	return gpu_oc_enabled;
}

static bool oc_machine_ok(void)
{
	return of_machine_is_compatible("samsung,exynos9820") ||
		of_machine_is_compatible("samsung,exynos9825");
}

static int margin_vclk_id(int margin_id)
{
	int size = cmucal_get_list_size(ACPM_VCLK_TYPE);
	int i;
	struct vclk *vclk;

	for (i = 0; i < size; i++) {
		vclk = cmucal_get_node(ACPM_VCLK_TYPE | i);
		if (vclk && vclk->margin_id == margin_id)
			return i;
	}
	return -EINVAL;
}

static struct vclk *oc_domain(unsigned int margin_id, struct rate_volt_header **rates,
		const char **status)
{
	struct fvmap_header *header = oc_fvmap;
	struct vclk *vclk;
	int id, level;

	if (!oc_machine_ok()) {
		*status = "unsupported-soc";
		return NULL;
	}
	id = margin_vclk_id(margin_id);
	if (id < 0 || !header) {
		*status = "missing-domain";
		return NULL;
	}
	vclk = cmucal_get_node(ACPM_VCLK_TYPE | id);
	if (!vclk || !vclk->lut || header[id].num_of_lv < 2 ||
	    vclk->num_rates != header[id].num_of_lv ||
	    header[id].o_ratevolt + header[id].num_of_lv *
	    sizeof(struct rate_volt) > OC_FVMAP_SIZE) {
		*status = "inconsistent-firmware-table";
		return NULL;
	}
	*rates = oc_fvmap + header[id].o_ratevolt;
	for (level = 0; level < header[id].num_of_lv; level++) {
		if (vclk->lut[level].rate != (*rates)->table[level].rate) {
			*status = "ect-firmware-rate-mismatch";
			return NULL;
		}
		if (level && (*rates)->table[level - 1].rate <=
		    (*rates)->table[level].rate) {
			*status = "unordered-firmware-table";
			return NULL;
		}
	}
	return vclk;
}

static int rate_index(struct rate_volt_header *rates, int count, unsigned int khz)
{
	int i;

	for (i = 0; i < count; i++) {
		if (rates->table[i].rate == khz)
			return i;
	}
	return -1;
}

static void init_cpu_oc(void)
{
	struct rate_volt_header *rates = NULL;
	struct vclk *vclk;
	struct cluster_oc_state *state;
	unsigned int top_khz, top_uv, levels;
	int i, idx, id;

	cpucl2_keep_uv = true;
	if (!cpucl2_oc_requested) {
		cpucl2_status = "disabled";
		goto clusters;
	}
	vclk = oc_domain(MARGIN_BIG, &rates, &cpucl2_status);
	if (!vclk)
		goto clusters;
	id = margin_vclk_id(MARGIN_BIG);
	levels = oc_fvmap ? ((struct fvmap_header *)oc_fvmap)[id].num_of_lv : 0;
	top_khz = rates->table[0].rate;
	top_uv = rates->table[0].volt;
	cpucl2_stock_khz = top_khz;
	cpucl2_target_uv = top_uv;
	if (top_uv < 500000 || top_uv > 1300000 || top_khz < 2000000) {
		cpucl2_status = "unsupported-stock-or-voltage";
		goto clusters;
	}
	idx = m4_target_valid(cpucl2_oc_target) ?
		rate_index(rates, levels, cpucl2_oc_target) : -1;
	if (idx >= 0 && rates->table[idx].volt >= 500000 &&
	    rates->table[idx].volt <= 1300000 &&
	    rates->table[idx].volt >= top_uv &&
	    cpucl2_oc_target > EXYNOS9820_M4_STOCK_KHZ) {
		vclk->max_freq = cpucl2_oc_target;
		cpucl2_target_uv = rates->table[idx].volt;
		cpucl2_oc_enabled = true;
		cpucl2_keep_uv = true;
		cpucl2_uv_switch = true;
		cpucl2_status = "enabled-on-demand";
	} else if (top_khz >= 2400000 && top_khz <= EXYNOS9820_M4_STOCK_KHZ) {
		cpucl2_oc_target = top_khz;
		cpucl2_oc_enabled = true;
		cpucl2_keep_uv = true;
		cpucl2_status = "enabled-firmware-max";
	} else {
		cpucl2_status = "target-not-in-firmware";
	}
	pr_info("cpucl2-oc: %s (requested=%u firmware=%u target=%u uv=%u keep_uv=%u)\n",
		cpucl2_status, cpucl2_oc_requested, cpucl2_stock_khz,
		cpucl2_oc_target, cpucl2_target_uv, cpucl2_keep_uv);

clusters:
	if (!cluster_oc_requested) {
		cluster_oc[0].status = "disabled";
		cluster_oc[1].status = "disabled";
		return;
	}
	for (i = 0; i < ARRAY_SIZE(cluster_oc); i++) {
		state = &cluster_oc[i];
		vclk = oc_domain(state->margin_id, &rates, &state->status);
		if (!vclk)
			continue;
		id = margin_vclk_id(state->margin_id);
		levels = ((struct fvmap_header *)oc_fvmap)[id].num_of_lv;
		top_khz = rates->table[0].rate;
		top_uv = rates->table[0].volt;
		state->stock_khz = top_khz;
		state->target_uv = top_uv;
		idx = rate_index(rates, levels, state->target_khz);
		if (idx < 0 || top_khz < state->target_khz) {
			state->status = "target-not-in-firmware";
		} else if (top_uv < 500000 || top_uv > 1300000) {
			state->status = "unsupported-stock-or-voltage";
		} else {
			if (vclk->max_freq < state->target_khz)
				vclk->max_freq = state->target_khz;
			state->enabled = true;
			state->status = top_khz == state->target_khz ?
				"enabled-firmware-max" : "enabled-existing-row";
		}
		pr_info("cluster-oc: %s %s (firmware=%u target=%u)\n",
			state->name, state->status, state->stock_khz,
			state->target_khz);
	}
}

int exynos9820_cluster_oc_override(unsigned int id, unsigned int dt_max)
{
	struct cluster_oc_state *state;
	int i, index;

	for (i = 0; i < ARRAY_SIZE(cluster_oc); i++) {
		state = &cluster_oc[i];
		if (!state->enabled || dt_max >= state->target_khz)
			continue;
		index = margin_vclk_id(state->margin_id);
		if (index >= 0 && id == (ACPM_VCLK_TYPE | index))
			return true;
	}
	return false;
}

int exynos9820_m4_oc_override(unsigned int id, unsigned int dt_max)
{
	int index;

	if (!cpucl2_oc_enabled || dt_max >= cpucl2_oc_target)
		return false;
	index = margin_vclk_id(MARGIN_BIG);
	return index >= 0 && id == (ACPM_VCLK_TYPE | index);
}

unsigned int exynos9820_gpu_oc_rate(unsigned int original_khz)
{
	if (gpu_oc_enabled && original_khz == gpu_oc_from)
		return gpu_oc_requested;
	return original_khz;
}

static bool gpu_pms_write(u32 pms)
{
	if (!gpu_pms_slot)
		return false;
	if (readl(gpu_pms_slot) == pms)
		return true;
	writel(pms, gpu_pms_slot);
	wmb();
	if (readl(gpu_pms_slot) == pms)
		return true;
	writel(gpu_pms_stock, gpu_pms_slot);
	wmb();
	gpu_pms_latched = false;
	return false;
}

static bool gpu_is_g3d(unsigned int id)
{
	int index = margin_vclk_id(MARGIN_G3D);

	return index >= 0 && id == (ACPM_VCLK_TYPE | index);
}

unsigned long exynos9820_gpu_cal_rate(unsigned int id, unsigned long rate)
{
	if (!gpu_oc_enabled || !gpu_is_g3d(id))
		return rate;
	if (rate == gpu_oc_requested) {
		if (!gpu_pms_write(gpu_pms_oc))
			return rate;
		gpu_pms_latched = true;
		return gpu_oc_from;
	}
	if (gpu_pms_latched) {
		gpu_pms_write(gpu_pms_stock);
		gpu_pms_latched = false;
	}
	return rate;
}

unsigned long exynos9820_gpu_cal_get(unsigned int id, unsigned long rate)
{
	if (!gpu_pms_latched || !gpu_is_g3d(id))
		return rate;
	if (rate == gpu_oc_from)
		return gpu_oc_requested;
	return rate;
}

bool exynos9820_cpu_margin_for_freq(unsigned int margin_id, unsigned int khz,
		int *percent)
{
	if (!percent || margin_id != MARGIN_BIG || !cpucl2_uv_switch)
		return false;
	*percent = khz >= cpucl2_oc_target ? 0 : -5;
	return true;
}

bool exynos9820_gpu_margin_for_freq(unsigned int khz, int *percent)
{
	if (!percent || !gpu_oc_enabled)
		return false;
	*percent = khz >= gpu_oc_requested ? 0 : -3;
	return true;
}

static void gpu_arm(struct vclk *vclk, unsigned int volt, unsigned int from_khz,
		u32 stock_pms, u32 oc_pms, void __iomem *slot, const char *status)
{
	gpu_pms_slot = slot;
	gpu_pms_stock = stock_pms;
	gpu_pms_oc = oc_pms;
	gpu_pms_latched = false;
	gpu_oc_from = from_khz;
	gpu_oc_voltage = volt;
	gpu_oc_enabled = true;
	gpu_status = status;
	vclk->max_freq = gpu_oc_requested;
}

static void init_gpu_s10(struct vclk *vclk, struct fvmap_header *header)
{
	struct rate_volt_header *rates = oc_fvmap + header->o_ratevolt;
	unsigned int pll_offset, level;
	u32 pll_count, candidate, top, index, target_pms;
	struct device_node *node;
	u32 dt_max = 0, dt_first = 0;

	node = of_find_compatible_node(NULL, NULL, "arm,mali");
	if (!node)
		return;
	of_property_read_u32(node, "gpu_max_clock", &dt_max);
	of_property_read_u32_index(node, "gpu_dvfs_table", 0, &dt_first);
	of_node_put(node);
	if (dt_max != 702000 || dt_first != 702000 ||
	    (gpu_oc_requested != 754000 && gpu_oc_requested != 806000))
		return;
	if (header->num_of_members != 1 || header->num_of_pll != 1 ||
	    header->num_of_lv != 12 || vclk->num_rates != 12 ||
	    vclk->max_freq != 702000 || rates->table[0].rate != 702000)
		return;
	if (rates->table[0].volt < 600000 || rates->table[0].volt > 800000)
		return;
	pll_offset = readw(oc_sram + header->o_members);
	if (pll_offset + offsetof(struct pll_header, pms) > OC_FVMAP_SIZE)
		return;
	pll_count = readw(oc_sram + pll_offset + offsetof(struct pll_header, level));
	if (pll_count < 2 || pll_offset + offsetof(struct pll_header, pms) +
	    pll_count * sizeof(u32) > OC_FVMAP_SIZE)
		return;
	candidate = readl(oc_sram + pll_offset + offsetof(struct pll_header, pms));
	top = readl(oc_sram + pll_offset + offsetof(struct pll_header, pms) + 4);
	index = readb(oc_sram + header->o_tables);
	if (candidate != EXYNOS9820_GPU_PMS_754 || top != EXYNOS9820_GPU_PMS_702 ||
	    index != 1)
		return;
	for (level = 0; level < 12; level++) {
		if (readb(oc_sram + header->o_tables + level) != level + 1)
			return;
	}
	target_pms = gpu_oc_requested == 754000 ?
		EXYNOS9820_GPU_PMS_754 : EXYNOS9820_GPU_PMS_806;
	/* Keep the 702 kHz row. Retarget its PLL only while the extra clock runs. */
	gpu_arm(vclk, rates->table[0].volt, 702000, top, target_pms,
		oc_sram + pll_offset + offsetof(struct pll_header, pms) +
		sizeof(u32), "s10-extra-row");
	pr_info("gpu-oc: s10 keeps 702000, adds %u kHz at %u uV\n",
		gpu_oc_requested, gpu_oc_voltage);
}

static void init_gpu_n10(struct vclk *vclk, struct fvmap_header *header)
{
	struct rate_volt_header *rates = oc_fvmap + header->o_ratevolt;
	struct device_node *node;
	unsigned int pll_offset;
	u32 pll_count, top_index, top_pms, dt_max = 0, dt_first = 0;

	if (gpu_oc_requested != 806000)
		return;
	node = of_find_compatible_node(NULL, NULL, "arm,mali");
	if (!node)
		return;
	of_property_read_u32(node, "gpu_max_clock", &dt_max);
	of_property_read_u32_index(node, "gpu_dvfs_table", 0, &dt_first);
	of_node_put(node);
	if (dt_max != 754000 || dt_first != 754000)
		return;
	if (header->num_of_members != 1 || header->num_of_pll != 1 ||
	    header->num_of_lv < 8 || header->num_of_lv > 16 ||
	    vclk->num_rates != header->num_of_lv ||
	    vclk->max_freq != 754000 || rates->table[0].rate != 754000)
		return;
	if (rates->table[0].volt < 600000 || rates->table[0].volt > 900000)
		return;
	pll_offset = readw(oc_sram + header->o_members);
	if (pll_offset + offsetof(struct pll_header, pms) > OC_FVMAP_SIZE)
		return;
	pll_count = readw(oc_sram + pll_offset + offsetof(struct pll_header, level));
	top_index = readb(oc_sram + header->o_tables);
	if (!pll_count || top_index >= pll_count ||
	    pll_offset + offsetof(struct pll_header, pms) +
	    pll_count * sizeof(u32) > OC_FVMAP_SIZE)
		return;
	top_pms = readl(oc_sram + pll_offset + offsetof(struct pll_header, pms) +
			top_index * sizeof(u32));
	if (top_pms != EXYNOS9820_GPU_PMS_754) {
		gpu_status = "blocked-pms";
		pr_info("gpu-oc: n10 top pms=%#x index=%u\n", top_pms, top_index);
		return;
	}
	gpu_arm(vclk, rates->table[0].volt, 754000, top_pms,
		EXYNOS9820_GPU_PMS_806,
		oc_sram + pll_offset + offsetof(struct pll_header, pms) +
		top_index * sizeof(u32), "n10-extra-row");
	pr_info("gpu-oc: n10 keeps 754000, adds %u kHz at %u uV\n",
		gpu_oc_requested, gpu_oc_voltage);
}

static void init_gpu_oc(void)
{
	struct fvmap_header *headers = oc_fvmap;
	struct vclk *vclk;
	unsigned int id;

	if (!gpu_oc_requested || !oc_machine_ok())
		return;
	gpu_status = "blocked-layout";
	for (id = 0; id < cmucal_get_list_size(ACPM_VCLK_TYPE); id++) {
		vclk = cmucal_get_node(ACPM_VCLK_TYPE | id);
		if (!vclk || vclk->margin_id != MARGIN_G3D)
			continue;
		init_gpu_s10(vclk, &headers[id]);
		if (!gpu_oc_enabled)
			init_gpu_n10(vclk, &headers[id]);
		return;
	}
}

static ssize_t oc_status_show(struct kobject *kobj, struct kobj_attribute *attr,
			      char *buf)
{
	return scnprintf(buf, PAGE_SIZE,
		"cpu %s requested=%u enabled=%u keep_uv=%u stock=%u target=%u uv=%u\n"
		"little %s enabled=%u stock=%u target=%u\n"
		"mid %s enabled=%u stock=%u target=%u\n"
		"gpu %s requested=%u enabled=%u from=%u to=%u uv=%u latched=%u\n",
		cpucl2_status, cpucl2_oc_requested, cpucl2_oc_enabled,
		cpucl2_keep_uv, cpucl2_stock_khz, cpucl2_oc_target, cpucl2_target_uv,
		cluster_oc[0].status, cluster_oc[0].enabled,
		cluster_oc[0].stock_khz, cluster_oc[0].target_khz,
		cluster_oc[1].status, cluster_oc[1].enabled,
		cluster_oc[1].stock_khz, cluster_oc[1].target_khz,
		gpu_status, gpu_oc_requested, gpu_oc_enabled,
		gpu_oc_from, gpu_oc_requested, gpu_oc_voltage, gpu_pms_latched);
}
static struct kobj_attribute oc_status_attr = __ATTR(status, 0444, oc_status_show, NULL);

void exynos9820_oc_init(void __iomem *fvmap, void __iomem *sram)
{
	struct kobject *kobj;

	oc_fvmap = fvmap;
	oc_sram = sram;
	init_cpu_oc();
	init_gpu_oc();
	kobj = kobject_create_and_add("exynos_oc", kernel_kobj);
	if (kobj)
		sysfs_create_file(kobj, &oc_status_attr.attr);
}

static unsigned int little_cold_limit(int enabled, unsigned int stock_khz,
		unsigned int target_khz, unsigned int range, int temperature,
		unsigned int limit_khz)
{
	(void)stock_khz;
	if (enabled && target_khz == 2106000 && range == 0 && temperature == 20 &&
	    limit_khz && limit_khz < target_khz)
		return target_khz;
	return limit_khz;
}

unsigned int exynos9820_cluster_oc_thermal_limit(const char *zone,
		unsigned int range, int temperature, unsigned int limit_khz)
{
	int i;

	if (!zone)
		return limit_khz;
	if (!strcmp(zone, "G3D") && range == 0 && temperature == 20)
		return exynos9820_gpu_oc_rate(limit_khz);
	if (strcmp(zone, "LITTLE"))
		return limit_khz;
	for (i = 0; i < ARRAY_SIZE(cluster_oc); i++) {
		if (cluster_oc[i].margin_id == MARGIN_LIT)
			return little_cold_limit(cluster_oc[i].enabled,
				cluster_oc[i].stock_khz, cluster_oc[i].target_khz,
				range, temperature, limit_khz);
	}
	return limit_khz;
}
