// SPDX-License-Identifier: GPL-2.0-only
/* exynos_drm_hibernation.c
 *
 * Copyright (C) 2020 Samsung Electronics Co.Ltd
 * Authors:
 *	Jiun Yu <jiun.yu@samsung.com>
 *
 * This program is free software; you can redistribute  it and/or modify it
 * under  the terms of  the GNU General  Public License as published by the
 * Free Software Foundation;  either version 2 of the  License, or (at your
 * option) any later version.
 *
 */

#include <linux/device.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/mutex.h>
#include <linux/pm_runtime.h>
#include <linux/sched.h>
#include <linux/err.h>
#include <linux/atomic.h>

#include <trace/dpu_trace.h>

#include "exynos_drm_decon.h"
#include "exynos_drm_hibernation.h"
#include "exynos_drm_writeback.h"

#define HIBERNATION_ENTRY_DEFAULT_FPS		60
#define HIBERNATION_ENTRY_MIN_TIME_MS		50
#define HIBERNATION_ENTRY_MIN_ENTRY_CNT		1

#define CAMERA_OPERATION_MASK	0xF
static bool is_camera_operating(struct exynos_hibernation *hiber)
{
	/* No need to check camera operation status. It depends on SoC */
	if (!hiber->cam_op_reg)
		return false;

	return (readl(hiber->cam_op_reg) & CAMERA_OPERATION_MASK);
}

static void exynos_hibernation_trig_reset(struct exynos_hibernation *hiber)
{
	const struct decon_device *decon = hiber->decon;
	const int fps = decon->bts.fps ? : HIBERNATION_ENTRY_DEFAULT_FPS;
	int entry_cnt = DIV_ROUND_UP(fps * HIBERNATION_ENTRY_MIN_TIME_MS, MSEC_PER_SEC);

	if (entry_cnt < HIBERNATION_ENTRY_MIN_ENTRY_CNT)
		entry_cnt = HIBERNATION_ENTRY_MIN_ENTRY_CNT;

	atomic_set(&hiber->trig_cnt, entry_cnt);
}

static bool exynos_hibernation_check(struct exynos_hibernation *hiber)
{
	pr_debug("%s +\n", __func__);

	return (!is_hibernaton_blocked(hiber) &&
		!is_camera_operating(hiber) &&
		atomic_dec_and_test(&hiber->trig_cnt));
}

static void exynos_hibernation_enter(struct exynos_hibernation *hiber)
{
	struct decon_device *decon = hiber->decon;

	pr_debug("%s +\n", __func__);

	if (!decon)
		return;

	DPU_ATRACE_BEGIN("exynos_hibernation_enter");
	mutex_lock(&hiber->lock);
	hibernation_block(hiber);

	if (decon->state != DECON_STATE_ON)
		goto ret;

	DPU_EVENT_LOG(DPU_EVT_ENTER_HIBERNATION_IN, decon->id, NULL);

	hiber->wb = decon_get_wb(decon);
	if (hiber->wb)
		writeback_enter_hibernation(hiber->wb);

	decon_enter_hibernation(decon);

	hiber->dsim = decon_get_dsim(decon);
	if (hiber->dsim)
		dsim_enter_ulps(hiber->dsim);

	decon->bts.ops->release_bw(decon);

	pm_runtime_put_sync(decon->dev);

	DPU_EVENT_LOG(DPU_EVT_ENTER_HIBERNATION_OUT, decon->id, NULL);

ret:
	hibernation_unblock(hiber);
	mutex_unlock(&hiber->lock);
	DPU_ATRACE_END("exynos_hibernation_enter");

	pr_debug("%s: DPU power %s -\n", __func__,
			pm_runtime_active(decon->dev) ? "on" : "off");
}

static int exynos_hibernation_exit(struct exynos_hibernation *hiber)
{
	struct decon_device *decon = hiber->decon;
	int ret = -EBUSY;

	pr_debug("%s +\n", __func__);

	if (!decon)
		return -ENODEV;

	hibernation_block(hiber);

	/*
	 * Cancel and/or wait for finishing previous queued hibernation entry work. It only
	 * goes to sleep when work is currently executing. If not, there is no operation here.
	 */
	kthread_cancel_work_sync(&hiber->work);

	mutex_lock(&hiber->lock);

	exynos_hibernation_trig_reset(hiber);

	if (decon->state != DECON_STATE_HIBERNATION)
		goto ret;

	DPU_ATRACE_BEGIN("exynos_hibernation_exit");

	DPU_EVENT_LOG(DPU_EVT_EXIT_HIBERNATION_IN, decon->id, NULL);

	pm_runtime_get_sync(decon->dev);

	if (hiber->dsim) {
		dsim_exit_ulps(hiber->dsim);
		hiber->dsim = NULL;
	}

	decon_exit_hibernation(decon);

	if (hiber->wb) {
		writeback_exit_hibernation(hiber->wb);
		hiber->wb = NULL;
	}

	DPU_EVENT_LOG(DPU_EVT_EXIT_HIBERNATION_OUT, decon->id, NULL);
	DPU_ATRACE_END("exynos_hibernation_exit");

	ret = 0;
ret:
	mutex_unlock(&hiber->lock);
	hibernation_unblock(hiber);

	pr_debug("%s: DPU power %s -\n", __func__,
			pm_runtime_active(decon->dev) ? "on" : "off");

	return ret;
}

bool hibernation_block_exit(struct exynos_hibernation *hiber)
{
	const struct exynos_hibernation_funcs *funcs;

	if (!hiber)
		return false;

	hibernation_block(hiber);

	funcs = hiber->funcs;

	return !funcs || !funcs->exit(hiber);
}

static const struct exynos_hibernation_funcs hibernation_funcs = {
	.check	= exynos_hibernation_check,
	.enter	= exynos_hibernation_enter,
	.exit	= exynos_hibernation_exit,
};

static void exynos_hibernation_handler(struct kthread_work *work)
{
	struct exynos_hibernation *hibernation =
		container_of(work, struct exynos_hibernation, work);
	const struct exynos_hibernation_funcs *funcs = hibernation->funcs;

	pr_debug("Display hibernation handler is called(trig_cnt:%d)\n",
			atomic_read(&hibernation->trig_cnt));

	/* If hibernation entry condition does NOT meet, just return here */
	if (!funcs->check(hibernation))
		return;

	funcs->enter(hibernation);
}

struct exynos_hibernation *
exynos_hibernation_register(struct decon_device *decon)
{
	struct device_node *np, *cam_np;
	struct exynos_hibernation *hibernation;
	struct device *dev = decon->dev;

	np = dev->of_node;
	if (!of_property_read_bool(np, "hibernation")) {
		pr_info("display hibernation is not supported\n");
		return NULL;
	}

	hibernation = devm_kzalloc(dev, sizeof(struct exynos_hibernation),
			GFP_KERNEL);
	if (!hibernation)
		return NULL;

	cam_np = of_get_child_by_name(np, "camera-operation");
	if (!cam_np) {
		pr_info("doesn't need to get camera operation register\n");
		hibernation->cam_op_reg = NULL;
	} else {
		hibernation->cam_op_reg = of_iomap(cam_np, 0);
		if (!hibernation->cam_op_reg) {
			pr_err("failed to map camera operation register\n");
			kfree(hibernation);
			return NULL;
		}
	}

	hibernation->decon = decon;
	hibernation->funcs = &hibernation_funcs;

	mutex_init(&hibernation->lock);

	exynos_hibernation_trig_reset(hibernation);

	atomic_set(&hibernation->block_cnt, 0);

	kthread_init_work(&hibernation->work, exynos_hibernation_handler);

	pr_info("display hibernation is supported\n");

	return hibernation;
}

void exynos_hibernation_destroy(struct exynos_hibernation *hiber)
{
	if (!hiber)
		return;

	if (hiber->cam_op_reg)
		iounmap(hiber->cam_op_reg);
}
