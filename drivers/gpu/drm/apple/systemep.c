// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2022 Sven Peter <sven@svenpeter.dev> */

#include <linux/completion.h>
#include <linux/soc/apple/rtkit.h>

#include "afk.h"
#include "dcp.h"
#include "parser.h"

static bool enable_verbose_logging;
module_param(enable_verbose_logging, bool, 0644);
MODULE_PARM_DESC(enable_verbose_logging, "Enable DCP firmware verbose logging");

static bool systemep_async_start;
module_param(systemep_async_start, bool, 0644);
MODULE_PARM_DESC(systemep_async_start,
		 "Start DCP system endpoint without waiting before IOMFB");

static unsigned int systemep_verbose_timeout_ms = 5000;
module_param(systemep_verbose_timeout_ms, uint, 0644);
MODULE_PARM_DESC(systemep_verbose_timeout_ms,
		 "Timeout in ms for the DCP system endpoint verbose-log property");

/*
 * Serialized setProperty("gAFKConfigLogMask", 0xffff) IPC call which
 * will set the DCP firmware log level to the most verbose setting
 */
#define SYSTEM_SET_PROPERTY 0x43
static const u8 setprop_gAFKConfigLogMask_ffff[] = {
	0x14, 0x00, 0x00, 0x00, 0x67, 0x41, 0x46, 0x4b, 0x43, 0x6f,
	0x6e, 0x66, 0x69, 0x67, 0x4c, 0x6f, 0x67, 0x4d, 0x61, 0x73,
	0x6b, 0x00, 0x00, 0x00, 0xd3, 0x00, 0x00, 0x00, 0x40, 0x00,
	0x00, 0x84, 0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

struct systemep_work {
	struct apple_epic_service *service;
	struct work_struct work;
};

static void system_log_work(struct work_struct *work_)
{
	struct systemep_work *work =
		container_of(work_, struct systemep_work, work);
	struct apple_dcp *dcp = work->service->ep->dcp;
	u32 retcode = 0;
	int ret;

	ret = afk_send_command(work->service, SYSTEM_SET_PROPERTY,
			       setprop_gAFKConfigLogMask_ffff,
			       sizeof(setprop_gAFKConfigLogMask_ffff), NULL, 0,
			       &retcode);
	if (ret)
		dev_warn(dcp->dev,
			 "systemep: verbose log setProperty failed: ret=%d retcode=0x%x\n",
			 ret, retcode);
	else
		dev_info(dcp->dev,
			 "systemep: verbose log setProperty completed retcode=0x%x\n",
			 retcode);

	complete(&dcp->systemep_done);
	kfree(work);
}

static int system_notify(struct apple_epic_service *service, enum epic_subtype type,
			 u16 tag, const void *data, size_t data_size)
{
	struct apple_dcp *dcp = service->ep->dcp;
	void *reply;
	u32 retcode = 0;
	int ret;

	if (type != SYSTEM_SET_PROPERTY)
		return -EOPNOTSUPP;

	if (data_size >= sizeof(retcode))
		memcpy(&retcode, data, sizeof(retcode));

	dev_info(dcp->dev,
		 "systemep: setProperty notify type=0x%x tag=0x%x len=%zu retcode=0x%x\n",
		 type, tag, data_size, retcode);

	reply = kmemdup(data, data_size, GFP_KERNEL);
	if (!reply)
		return -ENOMEM;

	/*
	 * m1n1's generic EPIC notify ACK path marks byte 0x50 as successful
	 * when the payload is large enough. Preserve that behavior for newer
	 * firmware while also accepting shorter firmware-14 payloads.
	 */
	if (data_size >= 0x54) {
		u32 ok = 1;

		memcpy(reply + 0x50, &ok, sizeof(ok));
	}

	ret = afk_send_epic(service->ep, service->channel, tag, EPIC_TYPE_NOTIFY_ACK,
			    EPIC_CAT_REPLY, type, reply, data_size);
	kfree(reply);
	return ret;
}

static void system_init(struct apple_epic_service *service, const char *name,
			const char *class, s64 unit)
{
	struct systemep_work *work;

	if (!enable_verbose_logging)
		return;

	/*
	 * We're called from the service message handler thread and can't
	 * dispatch blocking message from there.
	 */
	work = kzalloc(sizeof(*work), GFP_KERNEL);
	if (!work)
		return;

	work->service = service;
	INIT_WORK(&work->work, system_log_work);
	schedule_work(&work->work);
}

static void powerlog_init(struct apple_epic_service *service, const char *name,
			  const char *class, s64 unit)
{
}

static int powerlog_report(struct apple_epic_service *service, enum epic_subtype type,
			 const void *data, size_t data_size)
{
	struct dcp_system_ev_mnits mnits;
	struct dcp_parse_ctx parse_ctx;
	struct apple_dcp *dcp = service->ep->dcp;
	int ret;

	dev_dbg(dcp->dev, "systemep[ch:%u]: report type:%02x len:%zu\n",
		service->channel, type, data_size);

	if (type != EPIC_SUBTYPE_STD_SERVICE)
		return 0;

	ret = parse(data, data_size, &parse_ctx);
	if (ret) {
		dev_warn(service->ep->dcp->dev, "systemep: failed to parse report: %d\n", ret);
		return ret;
	}

	ret = parse_system_log_mnits(&parse_ctx, &mnits);
	if (ret) {
		/* ignore parse errors in the case dcp sends unknown log events */
		dev_dbg(dcp->dev, "systemep: failed to parse mNits event: %d\n", ret);
		return 0;
	}

	dev_dbg(dcp->dev, "systemep: mNits event: Nits: %u.%03u, iDAC: %u\n",
		mnits.millinits / 1000, mnits.millinits % 1000, mnits.idac);

	dcp->brightness.nits = mnits.millinits / 1000;

	return 0;
}

static const struct apple_epic_service_ops systemep_ops[] = {
		{
			.name = "system",
			.init = system_init,
			.notify = system_notify,
		},
	{
		.name = "powerlog-service",
		.init = powerlog_init,
		.report = powerlog_report,
	},
	{}
};

int systemep_init(struct apple_dcp *dcp)
{
	unsigned long left;
	int ret;

	init_completion(&dcp->systemep_done);

	dcp->systemep = afk_init(dcp, SYSTEM_ENDPOINT, systemep_ops);
	if (systemep_async_start)
		ret = afk_start_nowait(dcp->systemep);
	else
		ret = afk_start(dcp->systemep);
	if (ret)
		dev_warn(dcp->dev, "Failed to start system AFK endpoint: %d\n",
			 ret);

	if (!enable_verbose_logging)
		return 0;

	/*
	 * Timeouts aren't really fatal here: in the worst case we just weren't
	 * able to enable additional debug prints inside DCP
	 */
	left = msecs_to_jiffies(systemep_verbose_timeout_ms ?: MSEC_PER_SEC);
	while (left > 0) {
		unsigned long slice = min_t(unsigned long, left, msecs_to_jiffies(25));
		long waited;
		int polls;

		waited = wait_for_completion_timeout(&dcp->systemep_done, slice);
		if (waited > 0)
			return 0;

		polls = apple_rtkit_poll(dcp->rtk);
		if (polls)
			dev_info(dcp->dev,
				 "systemep: polled %d RTKit message(s) while waiting for verbose setup\n",
				 polls);
		if (dcp->systemep && dcp->systemep->wq)
			flush_workqueue(dcp->systemep->wq);
		if (completion_done(&dcp->systemep_done))
			return 0;

		left -= slice;
	}

	if (!completion_done(&dcp->systemep_done))
		dev_err(dcp->dev, "systemep: couldn't enable verbose logs\n");

	return 0;
}
