// SPDX-License-Identifier: GPL-2.0-only
/*
 * GXP power management.
 *
 * Copyright (C) 2021 Google LLC
 */

#include <linux/bits.h>
#include <linux/io.h>
#include <linux/moduleparam.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/spinlock.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include <gcip/gcip-pm.h>

#include "gxp-bpm.h"
#include "gxp-client.h"
#include "gxp-config.h"
#include "gxp-dma.h"
#include "gxp-doorbell.h"
#include "gxp-firmware.h"
#include "gxp-internal.h"
#include "gxp-lpm.h"
#include "gxp-pm.h"
#include "mobile-soc.h"

#if GXP_HAS_MCU
#include <gcip/gcip-kci.h>

#include "gxp-kci.h"
#include "gxp-mcu.h"
#endif /* GXP_HAS_MCU */

/* Don't attempt to touch the device when @busy_count equals this value. */
#define BUSY_COUNT_OFF (~0ull)

#define DEBUGFS_BLK_POWERSTATE "blk_powerstate"
#define DEBUGFS_WAKELOCK "wakelock"
#define DEBUGFS_MIN_FREQ "min_freq"
#define DEBUGFS_MAX_FREQ "max_freq"

#define SHUTDOWN_DELAY_US_MIN 200
#define SHUTDOWN_DELAY_US_MAX 400

static bool gxp_slow_clk_on_idle = true;
module_param_named(slow_clk, gxp_slow_clk_on_idle, bool, 0660);

const uint aur_power_state2rate[] = {
	AUR_OFF_RATE,	AUR_UUD_RATE,	   AUR_SUD_RATE,      AUR_UD_RATE,	AUR_NOM_RATE,
	AUR_READY_RATE, AUR_UUD_PLUS_RATE, AUR_SUD_PLUS_RATE, AUR_UD_PLUS_RATE,
};
const struct gxp_power_states off_states = { AUR_OFF, AUR_MEM_UNDEFINED, false };
const struct gxp_power_states uud_states = { AUR_UUD, AUR_MEM_UNDEFINED, false };

/*
 * The order of this array decides the voting priority, should be increasing in
 * frequencies.
 */
static const enum aur_power_state aur_state_array[] = {
	AUR_OFF,      AUR_READY, AUR_UUD,     AUR_UUD_PLUS, AUR_SUD,
	AUR_SUD_PLUS, AUR_UD,	 AUR_UD_PLUS, AUR_NOM
};
static const uint aur_memory_state_array[] = {
	AUR_MEM_UNDEFINED, AUR_MEM_MIN,	      AUR_MEM_VERY_LOW, AUR_MEM_LOW,
	AUR_MEM_HIGH,	   AUR_MEM_VERY_HIGH, AUR_MEM_MAX
};

static int gxp_pm_blkpwr_up(struct gxp_dev *gxp)
{
	int ret;

	/*
	 * This function is equivalent to pm_runtime_get_sync, but will prevent
	 * the pm_runtime refcount from increasing if the call fails. It also
	 * only returns either 0 for success or an errno on failure.
	 */
	ret = pm_runtime_resume_and_get(gxp->dev);
	if (ret) {
		dev_err(gxp->dev,
			"pm_runtime_resume_and_get returned %d during blk up\n",
			ret);
		return ret;
	}
	if (gxp->power_mgr->ops->after_blk_power_up) {
		ret = gxp->power_mgr->ops->after_blk_power_up(gxp);
		if (ret) {
			pm_runtime_put_sync(gxp->dev);
			dev_err(gxp->dev, "after blk power up failed: %d", ret);
			return ret;
		}
	}
	return 0;
}

static int gxp_pm_blkpwr_down(struct gxp_dev *gxp)
{
	int ret;

	if (gxp->power_mgr->ops->before_blk_power_down) {
		ret = gxp->power_mgr->ops->before_blk_power_down(gxp);
		if (ret) {
			dev_err(gxp->dev, "before blk power down failed: %d", ret);
			return ret;
		}
	}

	ret = pm_runtime_put_sync(gxp->dev);
	if (ret)
		/*
		 * pm_runtime_put_sync() returns the device's usage counter.
		 * Negative values indicate an error, while any positive values
		 * indicate the device is still in use somewhere. The only
		 * expected value here is 0, indicating no remaining users.
		 */
		dev_err(gxp->dev,
			"pm_runtime_put_sync returned %d during blk down\n",
			ret);
	/* Remove our vote for INT/MIF state (if any) */
	gxp_soc_pm_reset(gxp);
	return ret;
}

static int gxp_pm_blk_set_state_acpm(struct gxp_dev *gxp, unsigned long state)
{
	unsigned long rate;

	rate = aur_power_state2rate[state];
	if (gxp->power_mgr->thermal_limit &&
	    gxp->power_mgr->thermal_limit < rate)
		dev_warn(
			gxp->dev,
			"Requesting power state higher than current thermal limit (%lu)\n",
			rate);
	return gxp_pm_blk_set_rate_acpm(gxp, rate);
}

int gxp_pm_blk_set_rate_acpm(struct gxp_dev *gxp, unsigned long rate)
{
	int ret = gxp_soc_pm_set_rate(AUR_DVFS_DOMAIN, rate);

	dev_dbg(gxp->dev, "set blk rate %lu, ret %d\n", rate, ret);
	return ret;
}

static void set_cmu_noc_user_mux_state(struct gxp_dev *gxp, u32 val)
{
	if (!IS_ERR_OR_NULL(gxp->cmu.vaddr))
		writel(val << 4, gxp->cmu.vaddr + PLL_CON0_NOC_USER);
}

static void set_cmu_pll_aur_mux_state(struct gxp_dev *gxp, u32 val)
{
	if (!IS_ERR_OR_NULL(gxp->cmu.vaddr))
		writel(val << 4, gxp->cmu.vaddr + PLL_CON0_PLL_AUR);
}

static void reset_cmu_mux_state(struct gxp_dev *gxp)
{
	set_cmu_pll_aur_mux_state(gxp, AUR_CMU_MUX_NORMAL);
	set_cmu_noc_user_mux_state(gxp, AUR_CMU_MUX_NORMAL);
}

static void gxp_pm_can_busy(struct gxp_power_manager *mgr)
{
	unsigned long flags;

	spin_lock_irqsave(&mgr->busy_lock, flags);
	mgr->busy_count = 0;
	spin_unlock_irqrestore(&mgr->busy_lock, flags);
}

static void gxp_pm_no_busy(struct gxp_power_manager *mgr)
{
	unsigned long flags;

	spin_lock_irqsave(&mgr->busy_lock, flags);
	mgr->busy_count = BUSY_COUNT_OFF;
	spin_unlock_irqrestore(&mgr->busy_lock, flags);
}

void gxp_pm_force_clkmux_normal(struct gxp_dev *gxp)
{
	mutex_lock(&gxp->power_mgr->pm_lock);
	if (gxp->power_mgr->curr_low_clkmux) {
		set_cmu_pll_aur_mux_state(gxp, AUR_CMU_MUX_NORMAL);
		set_cmu_noc_user_mux_state(gxp, AUR_CMU_MUX_NORMAL);
	}
	gxp->power_mgr->force_mux_normal_count++;
	mutex_unlock(&gxp->power_mgr->pm_lock);
}

void gxp_pm_resume_clkmux(struct gxp_dev *gxp)
{
	mutex_lock(&gxp->power_mgr->pm_lock);
	gxp->power_mgr->force_mux_normal_count--;
	if (gxp->power_mgr->force_mux_normal_count == 0) {
		if (gxp->power_mgr->curr_low_clkmux) {
			set_cmu_pll_aur_mux_state(gxp, AUR_CMU_MUX_LOW);
			set_cmu_noc_user_mux_state(gxp, AUR_CMU_MUX_LOW);
		}
	}
	mutex_unlock(&gxp->power_mgr->pm_lock);
}

static void gxp_pm_blk_set_state_acpm_async(struct work_struct *work)
{
	struct gxp_set_acpm_state_work *set_acpm_state_work =
		container_of(work, struct gxp_set_acpm_state_work, work);
	struct gxp_dev *gxp = set_acpm_state_work->gxp;
	struct gxp_power_manager *mgr = gxp->power_mgr;
	bool scheduled_low_clkmux, prev_low_clkmux;
	bool is_core_booting;

	mutex_lock(&mgr->pm_lock);
	if (mgr->curr_state == AUR_OFF)
		goto out;

	scheduled_low_clkmux = set_acpm_state_work->low_clkmux;
	prev_low_clkmux = set_acpm_state_work->prev_low_clkmux;
	is_core_booting = mgr->force_mux_normal_count != 0;

	/* Don't change clkmux states when any core is booting */
	if (scheduled_low_clkmux != prev_low_clkmux && !is_core_booting) {
		if (prev_low_clkmux) {
			set_cmu_pll_aur_mux_state(gxp, AUR_CMU_MUX_NORMAL);
			set_cmu_noc_user_mux_state(gxp, AUR_CMU_MUX_NORMAL);
		} else if (scheduled_low_clkmux) {
			set_cmu_pll_aur_mux_state(gxp, AUR_CMU_MUX_LOW);
			set_cmu_noc_user_mux_state(gxp, AUR_CMU_MUX_LOW);
		}
	}
	mgr->curr_low_clkmux = scheduled_low_clkmux;

	gxp_pm_blk_set_state_acpm(set_acpm_state_work->gxp,
				  set_acpm_state_work->state);
out:
	set_acpm_state_work->using = false;
	mutex_unlock(&set_acpm_state_work->gxp->power_mgr->pm_lock);
}

#define AUR_DVFS_DEBUG_REQ BIT(31)
#define AUR_DEBUG_CORE_FREQ (AUR_DVFS_DEBUG_REQ | (3 << 27))

int gxp_pm_blk_get_state_acpm(struct gxp_dev *gxp)
{
	int ret = gxp_soc_pm_get_rate(AUR_DVFS_DOMAIN, AUR_DEBUG_CORE_FREQ);

	dev_dbg(gxp->dev, "current blk state %d\n", ret);
	return ret;
}

int gxp_pm_blk_on(struct gxp_dev *gxp)
{
	int ret;

	dev_info(gxp->dev, "Powering on BLK ...\n");
	mutex_lock(&gxp->power_mgr->pm_lock);
	ret = gxp_pm_blkpwr_up(gxp);
	if (ret)
		goto out;
	gxp_pm_blk_set_state_acpm(gxp, AUR_INIT_DVFS_STATE);
	gxp->power_mgr->curr_state = AUR_INIT_DVFS_STATE;
	gxp_iommu_setup_shareability(gxp);
	gxp_soc_lpm_init(gxp);
	gxp->power_mgr->blk_switch_count++;
	gxp_pm_can_busy(gxp->power_mgr);
out:
	mutex_unlock(&gxp->power_mgr->pm_lock);

	return ret;
}

int gxp_pm_blk_off(struct gxp_dev *gxp)
{
	int ret = 0;

	dev_info(gxp->dev, "Powering off BLK ...\n");
	mutex_lock(&gxp->power_mgr->pm_lock);
	/*
	 * Shouldn't happen unless this function has been called twice without blk_on
	 * first.
	 */
	if (gxp->power_mgr->curr_state == AUR_OFF) {
		mutex_unlock(&gxp->power_mgr->pm_lock);
		return ret;
	}
	gxp_pm_no_busy(gxp->power_mgr);
	/* Above has checked device is powered, it's safe to access the CMU regs. */
	reset_cmu_mux_state(gxp);

	gxp_soc_lpm_destroy(gxp);

	ret = gxp_pm_blkpwr_down(gxp);
	if (!ret)
		gxp->power_mgr->curr_state = AUR_OFF;
	mutex_unlock(&gxp->power_mgr->pm_lock);
	return ret;
}

static bool gxp_pm_is_blk_down_timeout(struct gxp_dev *gxp, uint timeout_ms)
{
	int timeout_cnt = 0, max_delay_count;
	int curr_state;

	if (!gxp->power_mgr->aur_status)
		return gxp->power_mgr->curr_state == AUR_OFF;

	max_delay_count = (timeout_ms * 1000) / SHUTDOWN_DELAY_US_MIN;

	do {
		/* Delay 200~400us per retry till blk shutdown finished */
		usleep_range(SHUTDOWN_DELAY_US_MIN, SHUTDOWN_DELAY_US_MAX);
		curr_state = readl(gxp->power_mgr->aur_status);
		if (!curr_state)
			return true;
		timeout_cnt++;
	} while (timeout_cnt < max_delay_count);

	return false;
}

int gxp_pm_blk_reboot(struct gxp_dev *gxp, uint timeout_ms)
{
	int ret;

	ret = gxp_pm_blk_off(gxp);
	if (ret) {
		dev_err(gxp->dev, "Failed to turn off BLK_AUR (ret=%d)\n", ret);
		return ret;
	}

	if (!gxp_pm_is_blk_down_timeout(gxp, timeout_ms)) {
		dev_err(gxp->dev, "BLK_AUR hasn't been turned off");
		return -EBUSY;
	}

	ret = gxp_pm_blk_on(gxp);
	if (ret)
		dev_err(gxp->dev, "Failed to turn on BLK_AUR (ret=%d)\n", ret);

	return ret;
}

int gxp_pm_get_blk_switch_count(struct gxp_dev *gxp)
{
	int ret;

	mutex_lock(&gxp->power_mgr->pm_lock);
	ret = gxp->power_mgr->blk_switch_count;
	mutex_unlock(&gxp->power_mgr->pm_lock);

	return ret;
}

int gxp_pm_get_blk_state(struct gxp_dev *gxp)
{
	int ret;

	mutex_lock(&gxp->power_mgr->pm_lock);
	ret = gxp->power_mgr->curr_state;
	mutex_unlock(&gxp->power_mgr->pm_lock);

	return ret;
}

int gxp_pm_core_on(struct gxp_dev *gxp, uint core, bool verbose)
{
	int ret;

	if (!gxp_lpm_is_initialized(gxp, LPM_PSM_TOP)) {
		dev_err(gxp->dev, "unable to power on core without TOP powered");
		return -EINVAL;
	}

	mutex_lock(&gxp->power_mgr->pm_lock);
	ret = gxp_lpm_up(gxp, core);
	if (ret) {
		dev_err(gxp->dev, "Core %d on fail\n", core);
		mutex_unlock(&gxp->power_mgr->pm_lock);
		return ret;
	}

	mutex_unlock(&gxp->power_mgr->pm_lock);

	if (verbose)
		dev_notice(gxp->dev, "Core %d powered up\n", core);
	return ret;
}

void gxp_pm_core_off(struct gxp_dev *gxp, uint core)
{
	if (!gxp_lpm_is_initialized(gxp, LPM_PSM_TOP))
		return;

	mutex_lock(&gxp->power_mgr->pm_lock);
	gxp_lpm_down(gxp, core);
	mutex_unlock(&gxp->power_mgr->pm_lock);
	dev_notice(gxp->dev, "Core %d powered down\n", core);
}

static int gxp_pm_req_state_locked(struct gxp_dev *gxp,
				   enum aur_power_state state,
				   bool low_clkmux_vote)
{
	uint i;

	if (state > AUR_MAX_ALLOW_STATE) {
		dev_err(gxp->dev, "Invalid state %d\n", state);
		return -EINVAL;
	}
	if (gxp->power_mgr->curr_state == AUR_OFF) {
		dev_warn(gxp->dev, "Cannot request power state when BLK is off\n");
		return -EBUSY;
	}
	if (state == AUR_OFF)
		return 0;

	if (state != gxp->power_mgr->curr_state ||
	    low_clkmux_vote != gxp->power_mgr->last_scheduled_low_clkmux) {
		mutex_lock(&gxp->power_mgr->set_acpm_state_work_lock);

		/* Look for an available worker */
		for (i = 0; i < AUR_NUM_POWER_STATE_WORKER; i++) {
			if (!gxp->power_mgr->set_acpm_state_work[i].using)
				break;
		}

		/*
		 * If the workqueue is full, cancel the last scheduled worker
		 * and use it for this request instead.
		 */
		if (i == AUR_NUM_POWER_STATE_WORKER) {
			dev_dbg(gxp->dev,
				"The workqueue for power state transition was full");
			i = gxp->power_mgr->last_set_acpm_state_worker;
			/*
			 * The last worker's `prev_state` and `prev_low_clkmux`
			 * fields are already set to the values this request
			 * will be changing from.
			 */
		} else {
			gxp->power_mgr->set_acpm_state_work[i].prev_state =
				gxp->power_mgr->curr_state;
			gxp->power_mgr->set_acpm_state_work[i].prev_low_clkmux =
				gxp->power_mgr->last_scheduled_low_clkmux;
		}

		gxp->power_mgr->set_acpm_state_work[i].state = state;
		gxp->power_mgr->set_acpm_state_work[i].low_clkmux =
			low_clkmux_vote;

		/*
		 * Schedule work to request the change, if not reusing an
		 * already scheduled worker.
		 */
		if (!gxp->power_mgr->set_acpm_state_work[i].using) {
			gxp->power_mgr->set_acpm_state_work[i].using = true;
			queue_work(
				gxp->power_mgr->wq,
				&gxp->power_mgr->set_acpm_state_work[i].work);
		}

		/* Change the internal state */
		gxp->power_mgr->curr_state = state;
		gxp->power_mgr->last_scheduled_low_clkmux = low_clkmux_vote;
		gxp->power_mgr->last_set_acpm_state_worker = i;

		mutex_unlock(&gxp->power_mgr->set_acpm_state_work_lock);
	}

	return 0;
}

/* Caller must hold pm_lock */
static void gxp_pm_revoke_power_state_vote(struct gxp_dev *gxp,
					   enum aur_power_state revoked_state,
					   bool origin_requested_low_clkmux)
{
	unsigned int i;
	uint *pwr_state_req_count;

	if (revoked_state == AUR_OFF)
		return;
	if (!origin_requested_low_clkmux)
		pwr_state_req_count = gxp->power_mgr->pwr_state_req_count;
	else
		pwr_state_req_count =
			gxp->power_mgr->low_clkmux_pwr_state_req_count;

	for (i = 0; i < AUR_NUM_POWER_STATE; i++) {
		if (aur_state_array[i] == revoked_state) {
			if (pwr_state_req_count[i] == 0)
				dev_err(gxp->dev, "Invalid state %d\n",
					revoked_state);
			else
				pwr_state_req_count[i]--;
			return;
		}
	}
}

/* Caller must hold pm_lock */
static void gxp_pm_vote_power_state(struct gxp_dev *gxp,
				    enum aur_power_state state,
				    bool requested_low_clkmux)
{
	unsigned int i;
	uint *pwr_state_req_count;

	if (state == AUR_OFF)
		return;
	if (!requested_low_clkmux)
		pwr_state_req_count = gxp->power_mgr->pwr_state_req_count;
	else
		pwr_state_req_count =
			gxp->power_mgr->low_clkmux_pwr_state_req_count;

	for (i = 0; i < AUR_NUM_POWER_STATE; i++) {
		if (aur_state_array[i] == state) {
			pwr_state_req_count[i]++;
			return;
		}
	}
}

/* Caller must hold pm_lock */
static void gxp_pm_get_max_voted_power_state(struct gxp_dev *gxp,
					     unsigned long *state,
					     bool *low_clkmux_vote)
{
	int i;

	*state = AUR_OFF;
	for (i = AUR_NUM_POWER_STATE - 1; i >= 0; i--) {
		if (gxp->power_mgr->pwr_state_req_count[i] > 0) {
			*low_clkmux_vote = false;
			*state = aur_state_array[i];
			break;
		}
	}
	if (*state == AUR_OFF) {
		/*
		 * All votes requested with low frequency CLKMUX flag, check low
		 * frequency CLKMUX vote counts.
		 */
		*low_clkmux_vote = true;
		for (i = AUR_NUM_POWER_STATE - 1; i >= 0; i--) {
			if (gxp->power_mgr->low_clkmux_pwr_state_req_count[i] > 0) {
				*state = aur_state_array[i];
				break;
			}
		}
	}
}

static int gxp_pm_update_requested_power_state(
	struct gxp_dev *gxp, enum aur_power_state origin_state,
	bool origin_requested_low_clkmux, enum aur_power_state requested_state,
	bool requested_low_clkmux)
{
	int ret;
	unsigned long max_state = AUR_OFF;
	bool low_clkmux_vote = false;

	lockdep_assert_held(&gxp->power_mgr->pm_lock);
	if (gxp->power_mgr->curr_state == AUR_OFF &&
	    requested_state != AUR_OFF) {
		dev_warn(gxp->dev,
			 "The client vote power state %d when BLK is off\n",
			 requested_state);
	}
	gxp_pm_revoke_power_state_vote(gxp, origin_state, origin_requested_low_clkmux);
	gxp_pm_vote_power_state(gxp, requested_state, requested_low_clkmux);
	gxp_pm_get_max_voted_power_state(gxp, &max_state, &low_clkmux_vote);
	ret = gxp_pm_req_state_locked(gxp, max_state, low_clkmux_vote);
	return ret;
}

static void gxp_pm_req_pm_qos_async(struct work_struct *work)
{
	struct gxp_req_pm_qos_work *req_pm_qos_work =
		container_of(work, struct gxp_req_pm_qos_work, work);

	mutex_lock(&req_pm_qos_work->gxp->power_mgr->pm_lock);
	if (req_pm_qos_work->gxp->power_mgr->curr_state != AUR_OFF)
		gxp_soc_pm_set_request(req_pm_qos_work->gxp, req_pm_qos_work->pm_value);
	req_pm_qos_work->using = false;
	mutex_unlock(&req_pm_qos_work->gxp->power_mgr->pm_lock);
}

static int gxp_pm_req_memory_state_locked(struct gxp_dev *gxp,
					  enum aur_memory_power_state state)
{
	uint i;

	if (state > AUR_MAX_ALLOW_MEMORY_STATE) {
		dev_err(gxp->dev, "Invalid memory state %d\n", state);
		return -EINVAL;
	}
	if (gxp->power_mgr->curr_state == AUR_OFF) {
		dev_err(gxp->dev,
			"Cannot request memory power state when BLK is off\n");
		return -EBUSY;
	}

	if (state != gxp->power_mgr->curr_memory_state) {
		mutex_lock(&gxp->power_mgr->req_pm_qos_work_lock);

		/* Look for an available worker */
		for (i = 0; i < AUR_NUM_POWER_STATE_WORKER; i++) {
			if (!gxp->power_mgr->req_pm_qos_work[i].using)
				break;
		}

		/*
		 * If the workqueue is full, cancel the last scheduled worker
		 * and use it for this request instead.
		 */
		if (i == AUR_NUM_POWER_STATE_WORKER) {
			dev_dbg(gxp->dev,
				"The workqueue for memory power state transition was full");
			i = gxp->power_mgr->last_req_pm_qos_worker;
		}

		gxp_soc_set_pm_arg_from_state(&gxp->power_mgr->req_pm_qos_work[i], state);

		/*
		 * Schedule work to request the change, if not reusing an
		 * already scheduled worker.
		 */
		if (!gxp->power_mgr->req_pm_qos_work[i].using) {
			gxp->power_mgr->req_pm_qos_work[i].using = true;
			queue_work(gxp->power_mgr->wq,
				   &gxp->power_mgr->req_pm_qos_work[i].work);
		}

		/* Change the internal state */
		gxp->power_mgr->curr_memory_state = state;
		gxp->power_mgr->last_req_pm_qos_worker = i;

		mutex_unlock(&gxp->power_mgr->req_pm_qos_work_lock);
	}

	return 0;
}

/* Caller must hold pm_lock */
static void
gxp_pm_revoke_memory_power_state_vote(struct gxp_dev *gxp,
				      enum aur_memory_power_state revoked_state)
{
	unsigned int i;

	if (revoked_state == AUR_MEM_UNDEFINED)
		return;
	for (i = 0; i < AUR_NUM_MEMORY_POWER_STATE; i++) {
		if (aur_memory_state_array[i] == revoked_state) {
			if (gxp->power_mgr->mem_pwr_state_req_count[i] == 0)
				dev_err_ratelimited(
					gxp->dev,
					"Invalid memory state %d with zero count\n",
					revoked_state);
			else
				gxp->power_mgr->mem_pwr_state_req_count[i]--;
			return;
		}
	}
}

/* Caller must hold pm_lock */
static void gxp_pm_vote_memory_power_state(struct gxp_dev *gxp,
				    enum aur_memory_power_state state)
{
	unsigned int i;

	if (state == AUR_MEM_UNDEFINED)
		return;
	for (i = 0; i < AUR_NUM_MEMORY_POWER_STATE; i++) {
		if (aur_memory_state_array[i] == state) {
			gxp->power_mgr->mem_pwr_state_req_count[i]++;
			return;
		}
	}
}

/* Caller must hold pm_lock */
static unsigned long gxp_pm_get_max_voted_memory_power_state(struct gxp_dev *gxp)
{
	int i;
	unsigned long state = AUR_MEM_UNDEFINED;

	for (i = AUR_NUM_MEMORY_POWER_STATE - 1; i >= 0; i--) {
		if (gxp->power_mgr->mem_pwr_state_req_count[i] > 0) {
			state = aur_memory_state_array[i];
			break;
		}
	}
	return state;
}

static int gxp_pm_update_requested_memory_power_state(
	struct gxp_dev *gxp, enum aur_memory_power_state origin_state,
	enum aur_memory_power_state requested_state)
{
	int ret;
	unsigned long max_state;

	lockdep_assert_held(&gxp->power_mgr->pm_lock);
	gxp_pm_revoke_memory_power_state_vote(gxp, origin_state);
	gxp_pm_vote_memory_power_state(gxp, requested_state);
	max_state = gxp_pm_get_max_voted_memory_power_state(gxp);
	ret = gxp_pm_req_memory_state_locked(gxp, max_state);
	return ret;
}

int gxp_pm_update_requested_power_states(
	struct gxp_dev *gxp, struct gxp_power_states origin_vote,
	struct gxp_power_states requested_states)
{
	int ret = 0;

	mutex_lock(&gxp->power_mgr->pm_lock);
	if (origin_vote.power != requested_states.power ||
	    origin_vote.low_clkmux != requested_states.low_clkmux) {
		ret = gxp_pm_update_requested_power_state(
			gxp, origin_vote.power, origin_vote.low_clkmux,
			requested_states.power, requested_states.low_clkmux);
		if (ret)
			goto out;
	}
	if (origin_vote.memory != requested_states.memory)
		ret = gxp_pm_update_requested_memory_power_state(
			gxp, origin_vote.memory, requested_states.memory);
out:
	mutex_unlock(&gxp->power_mgr->pm_lock);
	return ret;
}

#if GXP_HAS_MCU
static int gxp_pm_update_freq_limits_locked(struct gxp_dev *gxp)
{
	struct gxp_power_manager *mgr = gxp->power_mgr;
	struct gxp_kci *kci = &(gxp_mcu_of(gxp)->kci);
	int ret;

	lockdep_assert_held(&gxp->power_mgr->freq_limits_lock);

	ret = gxp_kci_set_freq_limits(kci, mgr->min_freq_limit, mgr->max_freq_limit);
	if (ret) {
		dev_warn(gxp->dev, "Set frequency limit request failed with error %d.", ret);
		if (ret == GCIP_KCI_ERROR_INVALID_ARGUMENT) {
			dev_warn(gxp->dev,
				 "Invalid values within debugfs frequency limits: [%u, %u]\n",
				 mgr->min_freq_limit, mgr->max_freq_limit);
			ret = -EINVAL;
		} else {
			ret = -EIO;
		}
		mgr->min_freq_limit = 0;
		mgr->max_freq_limit = 0;
	} else {
		dev_info(gxp->dev, "BLK frequency to remain in [%u, %u]kHz frequency limit.\n",
			 mgr->min_freq_limit, mgr->max_freq_limit);
	}

	return ret;
}

static int gxp_pm_set_freq_limit(struct gxp_dev *gxp, u32 val, u32 *limit_to_set)
{
	int ret = 0;

	if (val == *limit_to_set)
		return ret;
	/*
	 * Need to hold pm lock to prevent races with power up/down when checking block state and
	 * sending the KCI command to update limits.
	 *
	 * Since power_up will also acquire freq_limits_lock to send initial limits, pm lock must be
	 * held first to avoid lock inversion.
	 */
	gcip_pm_lock(gxp->power_mgr->pm);
	mutex_lock(&gxp->power_mgr->freq_limits_lock);

	*limit_to_set = val;
	if (!gxp_pm_is_blk_down(gxp))
		ret = gxp_pm_update_freq_limits_locked(gxp);

	mutex_unlock(&gxp->power_mgr->freq_limits_lock);
	gcip_pm_unlock(gxp->power_mgr->pm);
	return ret;
}

static int debugfs_min_freq_limit_get(void *data, u64 *val)
{
	struct gxp_dev *gxp = (struct gxp_dev *)data;
	struct gxp_power_manager *mgr = gxp->power_mgr;

	mutex_lock(&mgr->freq_limits_lock);
	*val = mgr->min_freq_limit;
	mutex_unlock(&mgr->freq_limits_lock);
	return 0;
}

static int debugfs_min_freq_limit_set(void *data, u64 val)
{
	struct gxp_dev *gxp = (struct gxp_dev *)data;
	struct gxp_power_manager *mgr = gxp->power_mgr;

	if (val > UINT_MAX) {
		dev_err(gxp->dev, "Requested debugfs min freq %llu must be <= %u (UINT_MAX)\n", val,
			UINT_MAX);
		return -EINVAL;
	}

	return gxp_pm_set_freq_limit(gxp, (u32)val, &mgr->min_freq_limit);
}

DEFINE_DEBUGFS_ATTRIBUTE(debugfs_min_freq_limit_fops, debugfs_min_freq_limit_get,
			 debugfs_min_freq_limit_set, "%llu\n");

static int debugfs_max_freq_limit_get(void *data, u64 *val)
{
	struct gxp_dev *gxp = (struct gxp_dev *)data;
	struct gxp_power_manager *mgr = gxp->power_mgr;

	mutex_lock(&mgr->freq_limits_lock);
	*val = mgr->max_freq_limit;
	mutex_unlock(&mgr->freq_limits_lock);
	return 0;
}

static int debugfs_max_freq_limit_set(void *data, u64 val)
{
	struct gxp_dev *gxp = (struct gxp_dev *)data;
	struct gxp_power_manager *mgr = gxp->power_mgr;

	if (val > UINT_MAX) {
		dev_err(gxp->dev, "Requested debugfs max freq %llu must be <= %u (UINT_MAX)\n", val,
			UINT_MAX);
		return -EINVAL;
	}

	return gxp_pm_set_freq_limit(gxp, (u32)val, &mgr->max_freq_limit);
}

DEFINE_DEBUGFS_ATTRIBUTE(debugfs_max_freq_limit_fops, debugfs_max_freq_limit_get,
			 debugfs_max_freq_limit_set, "%llu\n");
#endif /* GXP_HAS_MCU */

static int debugfs_wakelock_set(void *data, u64 val)
{
	struct gxp_dev *gxp = (struct gxp_dev *)data;
	int ret = 0;

	mutex_lock(&gxp->debugfs_client_lock);

	if (val > 0) {
		/* Wakelock Acquire */
		if (gxp->debugfs_wakelock_held) {
			dev_warn(gxp->dev,
				 "Debugfs wakelock is already held.\n");
			ret = -EBUSY;
			goto out;
		}

		ret = gcip_pm_get(gxp->power_mgr->pm);
		if (ret) {
			dev_err(gxp->dev, "gcip_pm_get failed ret=%d\n", ret);
			goto out;
		}
		gxp->debugfs_wakelock_held = true;
		gxp_pm_update_requested_power_states(gxp, off_states,
						     uud_states);
	} else {
		/* Wakelock Release */
		if (!gxp->debugfs_wakelock_held) {
			dev_warn(gxp->dev, "Debugfs wakelock not held.\n");
			ret = -EIO;
			goto out;
		}

		gcip_pm_put(gxp->power_mgr->pm);
		gxp->debugfs_wakelock_held = false;
		gxp_pm_update_requested_power_states(gxp, uud_states,
						     off_states);
	}

out:
	mutex_unlock(&gxp->debugfs_client_lock);

	return ret;
}

DEFINE_DEBUGFS_ATTRIBUTE(debugfs_wakelock_fops, NULL, debugfs_wakelock_set,
			 "%llx\n");

static int debugfs_blk_powerstate_set(void *data, u64 val)
{
	struct gxp_dev *gxp = (struct gxp_dev *)data;
	int ret = 0;

	if (gxp_pm_get_blk_state(gxp) == AUR_OFF) {
		dev_warn(
			gxp->dev,
			"Cannot set block power state when the block is off. Obtain a wakelock to power it on.\n");
		return -ENODEV;
	}

	if (val >= AUR_DVFS_MIN_RATE) {
		ret = gxp_pm_blk_set_rate_acpm(gxp, val);
	} else {
		ret = -EINVAL;
		dev_err(gxp->dev, "Incorrect state %llu\n", val);
	}
	return ret;
}

static int debugfs_blk_powerstate_get(void *data, u64 *val)
{
	struct gxp_dev *gxp = (struct gxp_dev *)data;

	if (gxp_pm_get_blk_state(gxp) == AUR_OFF) {
		dev_warn(
			gxp->dev,
			"Cannot get block power state when the block is off.\n");
		return -ENODEV;
	}

	*val = gxp_pm_blk_get_state_acpm(gxp);
	return 0;
}

DEFINE_DEBUGFS_ATTRIBUTE(debugfs_blk_powerstate_fops,
			 debugfs_blk_powerstate_get, debugfs_blk_powerstate_set,
			 "%llx\n");

static int gxp_pm_power_up(void *data)
{
	struct gxp_dev *gxp = data;
	int ret = gxp_pm_blk_on(gxp);

	if (ret) {
		dev_err(gxp->dev, "Failed to power on BLK_AUR (ret=%d)\n", ret);
		return ret;
	}

	if (gxp->pm_after_blk_on) {
		ret = gxp->pm_after_blk_on(gxp);
		if (ret) {
			gxp_pm_blk_off(gxp);
			return ret;
		}
	}

#if GXP_HAS_MCU
	mutex_lock(&gxp->power_mgr->freq_limits_lock);
	if (gxp->power_mgr->min_freq_limit || gxp->power_mgr->max_freq_limit)
		gxp_pm_update_freq_limits_locked(gxp);
	mutex_unlock(&gxp->power_mgr->freq_limits_lock);
#endif /* GXP_HAS_MCU */
	return 0;
}

static int gxp_pm_power_down(void *data)
{
	struct gxp_dev *gxp = data;

	if (gxp->pm_before_blk_off)
		gxp->pm_before_blk_off(gxp);
	return gxp_pm_blk_off(gxp);
}

static void gxp_pm_on_busy(struct gxp_dev *gxp)
{
	set_cmu_pll_aur_mux_state(gxp, AUR_CMU_MUX_NORMAL);
	set_cmu_noc_user_mux_state(gxp, AUR_CMU_MUX_NORMAL);
}

static void gxp_pm_on_idle(struct gxp_dev *gxp)
{
	if (gxp_slow_clk_on_idle) {
		set_cmu_pll_aur_mux_state(gxp, AUR_CMU_MUX_LOW);
		set_cmu_noc_user_mux_state(gxp, AUR_CMU_MUX_LOW);
	}
}

/**
 * gxp_pm_parse_pmu_base() - Parse the pmu-aur-status from device tree.
 * @gxp: The gxp_dev object.
 *
 * Parse the pmu status from property or fall back to try finding from reg instead.
 */
static void gxp_pm_parse_pmu_base(struct gxp_dev *gxp)
{
	u32 reg;
	struct resource *r;
	struct platform_device *pdev =
		container_of(gxp->dev, struct platform_device, dev);
	struct device *dev = gxp->dev;
	void __iomem *aur_status = ERR_PTR(-ENOENT);

	/* "pmu-aur-status" DT property takes precedence over reg entry */
	if (of_find_property(dev->of_node, "pmu-aur-status", NULL) &&
	    !of_property_read_u32_index(dev->of_node, "pmu-aur-status", 0, &reg)) {
		aur_status = devm_ioremap(dev, reg, 0x4);
	}

	if (!IS_ERR(aur_status))
		goto out;

	/* Fall back to try pasring from resource if the property not found. */
	r = platform_get_resource_byname(pdev, IORESOURCE_MEM, "pmu_aur_status");
	if (r)
		aur_status = devm_ioremap_resource(gxp->dev, r);

out:
	if (IS_ERR(aur_status))
		dev_warn(gxp->dev, "Failed to get PMU register base, ret=%ld\n",
			 PTR_ERR(aur_status));

	gxp->power_mgr->aur_status = IS_ERR(aur_status) ? NULL : aur_status;
}

int gxp_pm_init(struct gxp_dev *gxp)
{
	struct gxp_power_manager *mgr;
	const struct gcip_pm_args args = {
		.dev = gxp->dev,
		.data = gxp,
		.power_up = gxp_pm_power_up,
		.power_down = gxp_pm_power_down,
	};
	uint i;
	int ret;

	mgr = devm_kzalloc(gxp->dev, sizeof(*mgr), GFP_KERNEL);
	if (!mgr)
		return -ENOMEM;
	mgr->gxp = gxp;

	mgr->pm = gcip_pm_create(&args);
	if (IS_ERR(mgr->pm)) {
		ret = PTR_ERR(mgr->pm);
		goto err_free_mgr;
	}

	mutex_init(&mgr->pm_lock);
	mgr->curr_state = AUR_OFF;
	mgr->curr_memory_state = AUR_MEM_UNDEFINED;
	mgr->curr_low_clkmux = false;
	mgr->last_scheduled_low_clkmux = false;
	gxp_pm_chip_set_ops(mgr);
	gxp->power_mgr = mgr;
	for (i = 0; i < AUR_NUM_POWER_STATE_WORKER; i++) {
		mgr->set_acpm_state_work[i].gxp = gxp;
		mgr->set_acpm_state_work[i].using = false;
		mgr->req_pm_qos_work[i].gxp = gxp;
		mgr->req_pm_qos_work[i].using = false;
		INIT_WORK(&mgr->set_acpm_state_work[i].work,
			  gxp_pm_blk_set_state_acpm_async);
		INIT_WORK(&mgr->req_pm_qos_work[i].work,
			  gxp_pm_req_pm_qos_async);
	}
	mutex_init(&mgr->set_acpm_state_work_lock);
	mutex_init(&mgr->req_pm_qos_work_lock);
	gxp->power_mgr->wq = create_singlethread_workqueue("gxp_power_work_queue");
	if (!gxp->power_mgr->wq) {
		ret = -ENOMEM;
		goto err_pm_destroy;
	}
	gxp->power_mgr->force_mux_normal_count = 0;
	gxp->power_mgr->blk_switch_count = 0l;
	spin_lock_init(&gxp->power_mgr->busy_lock);
	gxp->power_mgr->busy_count = BUSY_COUNT_OFF;

	gxp_pm_parse_pmu_base(gxp);

	pm_runtime_enable(gxp->dev);
	gxp_soc_pm_init(gxp);
	gxp_pm_chip_init(gxp);

	gxp->debugfs_wakelock_held = false;
#if GXP_HAS_MCU
	mutex_init(&mgr->freq_limits_lock);
	debugfs_create_file(DEBUGFS_MIN_FREQ, 0600, gxp->d_entry, gxp,
			    &debugfs_min_freq_limit_fops);
	debugfs_create_file(DEBUGFS_MAX_FREQ, 0600, gxp->d_entry, gxp,
			    &debugfs_max_freq_limit_fops);
#endif /* GXP_HAS_MCU */
	debugfs_create_file(DEBUGFS_WAKELOCK, 0200, gxp->d_entry, gxp, &debugfs_wakelock_fops);
	debugfs_create_file(DEBUGFS_BLK_POWERSTATE, 0600, gxp->d_entry, gxp,
			    &debugfs_blk_powerstate_fops);

	return 0;

err_pm_destroy:
	gcip_pm_destroy(mgr->pm);
err_free_mgr:
	devm_kfree(gxp->dev, mgr);
	return ret;
}

int gxp_pm_destroy(struct gxp_dev *gxp)
{
	struct gxp_power_manager *mgr = gxp->power_mgr;

	if (IS_GXP_TEST && !mgr)
		return 0;

#if GXP_HAS_MCU
	debugfs_remove(debugfs_lookup(DEBUGFS_MIN_FREQ, gxp->d_entry));
	debugfs_remove(debugfs_lookup(DEBUGFS_MAX_FREQ, gxp->d_entry));
#endif /* GXP_HAS_MCU */
	debugfs_remove(debugfs_lookup(DEBUGFS_BLK_POWERSTATE, gxp->d_entry));
	debugfs_remove(debugfs_lookup(DEBUGFS_WAKELOCK, gxp->d_entry));

	gxp_pm_chip_exit(gxp);
	gcip_pm_destroy(mgr->pm);

	gxp_soc_pm_exit(gxp);
	pm_runtime_disable(gxp->dev);
	flush_workqueue(mgr->wq);
	destroy_workqueue(mgr->wq);
	mutex_destroy(&mgr->pm_lock);
	return 0;
}

void gxp_pm_set_thermal_limit(struct gxp_dev *gxp, unsigned long thermal_limit)
{
	mutex_lock(&gxp->power_mgr->pm_lock);

	if (thermal_limit >= aur_power_state2rate[AUR_NOM]) {
		dev_warn(gxp->dev, "Thermal limit on DVFS removed\n");
	} else if (thermal_limit >= aur_power_state2rate[AUR_UD_PLUS]) {
		dev_warn(gxp->dev, "Thermals limited to UD+\n");
	} else if (thermal_limit >= aur_power_state2rate[AUR_UD]) {
		dev_warn(gxp->dev, "Thermals limited to UD\n");
	} else if (thermal_limit >= aur_power_state2rate[AUR_SUD_PLUS]) {
		dev_warn(gxp->dev, "Thermals limited to SUD+\n");
	} else if (thermal_limit >= aur_power_state2rate[AUR_SUD]) {
		dev_warn(gxp->dev, "Thermal limited to SUD\n");
	} else if (thermal_limit >= aur_power_state2rate[AUR_UUD_PLUS]) {
		dev_warn(gxp->dev, "Thermals limited to UUD+\n");
	} else if (thermal_limit >= aur_power_state2rate[AUR_UUD]) {
		dev_warn(gxp->dev, "Thermal limited to UUD\n");
	} else if (thermal_limit >= aur_power_state2rate[AUR_READY]) {
		dev_warn(gxp->dev, "Thermal limited to READY\n");
	} else {
		dev_warn(gxp->dev,
			 "Thermal limit disallows all valid DVFS states\n");
	}

	gxp->power_mgr->thermal_limit = thermal_limit;

	mutex_unlock(&gxp->power_mgr->pm_lock);
}

void gxp_pm_busy(struct gxp_dev *gxp)
{
	unsigned long flags;
	struct gxp_power_manager *mgr = gxp->power_mgr;

	spin_lock_irqsave(&mgr->busy_lock, flags);
	/*
	 * We don't need to check BUSY_COUNT_OFF here, caller ensures the block is powered before
	 * calling this function.
	 */
	++mgr->busy_count;
	if (mgr->busy_count == 1)
		gxp_pm_on_busy(gxp);
	spin_unlock_irqrestore(&mgr->busy_lock, flags);
}

void gxp_pm_idle(struct gxp_dev *gxp)
{
	unsigned long flags;
	struct gxp_power_manager *mgr = gxp->power_mgr;

	spin_lock_irqsave(&mgr->busy_lock, flags);
	if (mgr->busy_count == BUSY_COUNT_OFF)
		goto out;
	--mgr->busy_count;
	if (mgr->busy_count == 0)
		gxp_pm_on_idle(gxp);
out:
	spin_unlock_irqrestore(&mgr->busy_lock, flags);
}
