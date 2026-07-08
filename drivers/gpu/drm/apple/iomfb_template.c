// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Copyright 2021 Alyssa Rosenzweig
 * Copyright The Asahi Linux Contributors
 */

#include <linux/align.h>
#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/soc/apple/rtkit.h>
#include <linux/string.h>
#include <linux/unaligned.h>

#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "dcp.h"
#include "dcp-internal.h"
#include "iomfb.h"
#include "iomfb_internal.h"
#include "parser.h"
#include "trace.h"
#include "version_utils.h"

/* Register defines used in bandwidth setup structure */
#define REG_DOORBELL_BIT(idx) (2 + (idx))

struct dcp_wait_cookie {
	struct kref refcount;
	struct completion done;
};

static void release_wait_cookie(struct kref *ref)
{
	struct dcp_wait_cookie *cookie;
	cookie = container_of(ref, struct dcp_wait_cookie, refcount);

        kfree(cookie);
}

DCP_THUNK_OUT(iomfb_a131_pmu_service_matched, iomfbep_a131_pmu_service_matched, u32);
DCP_THUNK_OUT(iomfb_a132_backlight_service_matched, iomfbep_a132_backlight_service_matched, u32);
DCP_THUNK_OUT(iomfb_a358_vi_set_temperature_hint, iomfbep_a358_vi_set_temperature_hint, u32);

IOMFB_THUNK_INOUT(set_matrix);
IOMFB_THUNK_INOUT(get_color_remap_mode);
IOMFB_THUNK_INOUT(last_client_close);
IOMFB_THUNK_INOUT(abort_swaps_dcp);
DCP_THUNK_INOUT(dcp_set_brightness_correction,
		iomfbep_set_brightness_correction, u32, u32);

DCP_THUNK_INOUT(dcp_swap_submit, dcpep_swap_submit,
		struct DCP_FW_NAME(dcp_swap_submit_req),
		struct DCP_FW_NAME(dcp_swap_submit_resp));

DCP_THUNK_INOUT(dcp_swap_start, dcpep_swap_start, struct dcp_swap_start_req,
		struct dcp_swap_start_resp);

DCP_THUNK_INOUT(dcp_set_power_state, dcpep_set_power_state,
		struct dcp_set_power_state_req,
		struct dcp_set_power_state_resp);

DCP_THUNK_INOUT(dcp_set_digital_out_mode, dcpep_set_digital_out_mode,
		struct dcp_set_digital_out_mode_req, u32);

DCP_THUNK_INOUT(dcp_set_display_device, dcpep_set_display_device, u32, u32);
DCP_THUNK_INOUT(dcp_get_gamma_table, dcpep_get_gamma_table,
		struct dcp_get_gamma_table_req,
		struct dcp_get_gamma_table_resp);
DCP_THUNK_INOUT(dcp_set_contrast, dcpep_set_contrast,
		struct dcp_set_contrast_req, struct dcp_set_contrast_resp);

DCP_THUNK_OUT(dcp_set_display_refresh_properties,
	      dcpep_set_display_refresh_properties, u32);

#if DCP_FW_VER >= DCP_FW_VERSION(13, 2, 0)
DCP_THUNK_INOUT(dcp_late_init_signal, dcpep_late_init_signal, u32, u32);
#else
DCP_THUNK_OUT(dcp_late_init_signal, dcpep_late_init_signal, u32);
#endif
DCP_THUNK_IN(dcp_flush_supports_power, dcpep_flush_supports_power, u32);
DCP_THUNK_OUT(dcp_create_default_fb, dcpep_create_default_fb, u32);
DCP_THUNK_OUT(dcp_start_signal, dcpep_start_signal, u32);
DCP_THUNK_VOID(dcp_setup_video_limits, dcpep_setup_video_limits);
DCP_THUNK_VOID(dcp_set_create_dfb, dcpep_set_create_dfb);
DCP_THUNK_VOID(dcp_first_client_open, dcpep_first_client_open);
DCP_THUNK_INOUT(dcp_update_dfb, dcpep_update_dfb,
		struct DCP_FW_NAME(dcp_surface), u32);
DCP_THUNK_OUT(dcp_is_keep_on_screen, dcpep_is_keep_on_screen, u32);

static void dcp_set_parameter_dcp(struct apple_dcp *dcp, bool oob,
				  struct dcp_set_parameter_dcp *data,
				  dcp_callback_t cb, void *cookie)
{
	if (iomfb_compact_set_parameter) {
		u8 req[sizeof(u32) + ARRAY_SIZE(data->value) * sizeof(u64) +
		       sizeof(u32)] = {};
		u8 *p = req;
		u32 count = min_t(u32, data->count, ARRAY_SIZE(data->value));
		u32 in_len = sizeof(u32) + count * sizeof(u64) + sizeof(u32);
		u32 i;

		if (count != data->count)
			dev_warn(dcp->dev,
				 "clamping compact set_parameter count from %u to %u\n",
				 data->count, count);

		put_unaligned_le32(data->param, p);
		p += sizeof(u32);
		for (i = 0; i < count; i++) {
			put_unaligned_le64(data->value[i], p);
			p += sizeof(u64);
		}
		put_unaligned_le32(count, p);

		dev_info(dcp->dev,
			 "compact set_parameter_dcp param=%u count=%u in=%u value0=0x%x\n",
			 data->param, count, in_len, data->value[0]);

		dcp_push(dcp, oob, &dcp_methods[dcpep_set_parameter_dcp],
			 in_len, sizeof(u32), req, cb, cookie);
		return;
	}

	dcp_push(dcp, oob, &dcp_methods[dcpep_set_parameter_dcp],
		 sizeof(*data), sizeof(u32), data, cb, cookie);
}

DCP_THUNK_INOUT(dcp_enable_disable_video_power_savings,
		dcpep_enable_disable_video_power_savings, u32, int);

DCP_THUNK_OUT(dcp_is_main_display, dcpep_is_main_display, u32);

DCP_THUNK_IN(dcp_update_notify_clients_dcp, dcpep_update_notify_clients_dcp,
	     struct dcp_update_notify_clients_req);

/* DCP callback handlers */
static void dcpep_cb_nop(struct apple_dcp *dcp)
{
	/* No operation */
}

static u8 dcpep_cb_true(struct apple_dcp *dcp)
{
	return true;
}

static u8 dcpep_cb_false(struct apple_dcp *dcp)
{
	return false;
}

static u32 dcpep_cb_zero(struct apple_dcp *dcp)
{
	return 0;
}

static u32 dcpep_cb_d121(struct apple_dcp *dcp)
{
	if (iomfb_trace_ipc)
		dev_info(dcp->dev, "D121 returning %u\n",
			 iomfb_d121_force_true ? 1 : 0);

	return iomfb_d121_force_true ? 1 : 0;
}

static u32 dcpep_cb_d123(struct apple_dcp *dcp)
{
	if (iomfb_trace_ipc)
		dev_info(dcp->dev, "D123 returning %u\n",
			 iomfb_d123_force_true ? 1 : 0);

	return iomfb_d123_force_true ? 1 : 0;
}

static u8 dcpep_cb_is_dfb_allocated(struct apple_dcp *dcp)
{
	if (iomfb_trace_ipc)
		dev_info(dcp->dev, "D596 isDFBAllocated returning %u\n",
			 iomfb_dfb_allocated ? 1 : 0);

	return iomfb_dfb_allocated ? 1 : 0;
}

static void dcpep_log_key(const struct apple_dcp *dcp, const char *name,
			  const char *key, size_t key_size)
{
	size_t len;

	if (!iomfb_trace_ipc)
		return;

	len = strnlen(key, key_size);
	dev_info(dcp->dev, "%s key='%.*s' key_hex=%*phN\n", name, (int)len,
		 key, (int)min_t(size_t, key_size, 16), key);
}

static const u8 *dcpep_osobj_skip(const u8 *p, const u8 *end, unsigned int depth);

static const u8 *dcpep_osobj_read_string(const u8 *p, const u8 *end,
					 const char **str, u32 *len)
{
	if (end - p < 5 || *p != 's')
		return NULL;

	p++;
	*len = get_unaligned_le32(p);
	p += sizeof(u32);

	if (*len > end - p || *len > U8_MAX)
		return NULL;

	*str = p;
	p += *len;

	if (p >= end || *p)
		return NULL;

	return p + 1;
}

static const u8 *dcpep_osobj_skip(const u8 *p, const u8 *end, unsigned int depth)
{
	u32 count;

	if (!p || p >= end || depth > 4)
		return NULL;

	switch (*p++) {
	case 'n':
		if (end - p < sizeof(u64))
			return NULL;
		return p + sizeof(u64);
	case 's': {
		u32 len;

		if (end - p < sizeof(u32))
			return NULL;
		len = get_unaligned_le32(p);
		p += sizeof(u32);
		if (len > end - p || p + len >= end || p[len])
			return NULL;
		return p + len + 1;
	}
	case 'd':
		if (end - p < sizeof(u32))
			return NULL;
		count = get_unaligned_le32(p);
		p += sizeof(u32);
		for (u32 i = 0; i < count; i++) {
			p = dcpep_osobj_skip(p, end, depth + 1);
			p = dcpep_osobj_skip(p, end, depth + 1);
			if (!p)
				return NULL;
		}
		return p;
	default:
		return NULL;
	}
}

static void dcpep_log_osdict(struct apple_dcp *dcp, const char *name,
			     const u8 *data, size_t size)
{
	const u8 *p = data;
	const u8 *end = data + size;
	u32 count;

	if (!iomfb_trace_ipc)
		return;

	if (end - p < 5 || *p != 'd') {
		dev_info(dcp->dev, "%s osdict: not a dictionary tag=0x%02x\n",
			 name, p < end ? *p : 0);
		return;
	}

	p++;
	count = get_unaligned_le32(p);
	p += sizeof(u32);

	dev_info(dcp->dev, "%s osdict count=%u\n", name, count);

	for (u32 i = 0; i < min_t(u32, count, 24); i++) {
		const char *key;
		u32 key_len;

		p = dcpep_osobj_read_string(p, end, &key, &key_len);
		if (!p) {
			dev_info(dcp->dev, "%s osdict entry[%u]: bad key\n",
				 name, i);
			return;
		}

		if (p >= end) {
			dev_info(dcp->dev, "%s osdict entry[%u] key='%.*s': missing value\n",
				 name, i, (int)key_len, key);
			return;
		}

		switch (*p) {
		case 'n':
			if (end - p < 1 + sizeof(u64)) {
				dev_info(dcp->dev,
					 "%s osdict entry[%u] key='%.*s': short number\n",
					 name, i, (int)key_len, key);
				return;
			}
			dev_info(dcp->dev,
				 "%s osdict entry[%u] key='%.*s' number=0x%llx\n",
				 name, i, (int)key_len, key,
				 get_unaligned_le64(p + 1));
			p += 1 + sizeof(u64);
			break;
		case 's': {
			const char *value;
			u32 value_len;

			p = dcpep_osobj_read_string(p, end, &value, &value_len);
			if (!p) {
				dev_info(dcp->dev,
					 "%s osdict entry[%u] key='%.*s': bad string value\n",
					 name, i, (int)key_len, key);
				return;
			}
			dev_info(dcp->dev,
				 "%s osdict entry[%u] key='%.*s' string='%.*s'\n",
				 name, i, (int)key_len, key,
				 (int)value_len, value);
			break;
		}
		case 'd': {
			const u8 *value = p;
			u32 dict_count;

			if (end - p < 5) {
				dev_info(dcp->dev,
					 "%s osdict entry[%u] key='%.*s': short dict value\n",
					 name, i, (int)key_len, key);
				return;
			}
			dict_count = get_unaligned_le32(p + 1);
			p = dcpep_osobj_skip(p, end, 0);
			if (!p) {
				dev_info(dcp->dev,
					 "%s osdict entry[%u] key='%.*s': bad nested dict\n",
					 name, i, (int)key_len, key);
				return;
			}
			dev_info(dcp->dev,
				 "%s osdict entry[%u] key='%.*s' dict_count=%u data0=%*phN\n",
				 name, i, (int)key_len, key, dict_count,
				 (int)min_t(size_t, (size_t)(p - value), 32), value);
			break;
		}
		default:
			dev_info(dcp->dev,
				 "%s osdict entry[%u] key='%.*s': unknown value tag=0x%02x\n",
				 name, i, (int)key_len, key, *p);
			return;
		}
	}
}

static u8 dcpep_cb_set_number_property(struct apple_dcp *dcp,
				       struct dcp_set_number_property_req *req)
{
	if (iomfb_trace_ipc) {
		dcpep_log_key(dcp, "set_number_property", req->key,
			      sizeof(req->key));
		dev_info(dcp->dev, "set_number_property value=0x%x\n",
			 req->value);
	}

	return true;
}

static u8 dcpep_cb_set_property_dict(struct apple_dcp *dcp,
				     struct dcp_set_property_dict_req *req)
{
	if (iomfb_trace_ipc) {
		dcpep_log_key(dcp, "set_property_dict", req->key,
			      sizeof(req->key));
		dev_info(dcp->dev,
			 "set_property_dict value_null=%u data0=%*phN\n",
			 req->value_null, 64, req->data);
		dcpep_log_osdict(dcp, "set_property_dict", req->data,
				 sizeof(req->data));
	}

	return true;
}

static u8 dcpep_cb_set_property_int(struct apple_dcp *dcp,
				    struct dcp_set_property_int_req *req)
{
	if (iomfb_trace_ipc) {
		dcpep_log_key(dcp, "set_property_int", req->key,
			      sizeof(req->key));
		dev_info(dcp->dev,
			 "set_property_int value=0x%llx value_null=%u padding=%*phN\n",
			 req->value, req->value_null, 3, req->padding);
	}

	return true;
}

static u8 dcpep_cb_set_property_bool(struct apple_dcp *dcp,
				     struct dcp_set_property_bool_req *req)
{
	if (iomfb_trace_ipc) {
		dcpep_log_key(dcp, "set_property_bool", req->key,
			      sizeof(req->key));
		dev_info(dcp->dev, "set_property_bool value=%u value_null=%u\n",
			 req->value, req->value_null);
	}

	return true;
}

static void dcpep_cb_swap_complete(struct apple_dcp *dcp,
				   struct DCP_FW_NAME(dc_swap_complete_resp) *resp)
{
	ktime_t now = ktime_get();
	trace_iomfb_swap_complete(dcp, resp->swap_id);
	dev_info(dcp->dev, "D589 swap_complete swap_id=%u\n", resp->swap_id);
	dcp->last_swap_id = resp->swap_id;

	dcp_drm_crtc_page_flip(dcp, now);
	if (dcp->crc_enabled) {
		u32 crc32 = 0;
		drm_crtc_add_crc_entry(&dcp->crtc->base, true, resp->swap_id, &crc32);
	}
}

static bool __maybe_unused
trampoline_swap_complete_head_of_line(struct apple_dcp *dcp, int tag, void *out,
				      void *in)
{
	u8 *buf = in;
	u32 dump_len = min_t(u32, dcp->callback_in_len, 48);
	u32 swap_id = 0;
	ktime_t now = ktime_get();

	trace_iomfb_callback(dcp, tag, "dcpep_cb_swap_complete_head_of_line");

	if (dcp->callback_in_len >= sizeof(swap_id))
		swap_id = get_unaligned_le32(buf);

	dev_info(dcp->dev,
		 "D581 swap_complete_head_of_line in=%u out=%u swap_id=%u raw=%*phN\n",
		 dcp->callback_in_len, dcp->callback_out_len, swap_id, dump_len,
		 buf);

	if (swap_id) {
		trace_iomfb_swap_complete(dcp, swap_id);
		dcp->last_swap_id = swap_id;
	}

	dcp_drm_crtc_page_flip(dcp, now);
	return true;
}

static bool __maybe_unused
trampoline_batched_swap_complete_ap_gated(struct apple_dcp *dcp, int tag,
					  void *out, void *in)
{
	u8 *buf = in;
	u32 dump_len = min_t(u32, dcp->callback_in_len, 64);
	u32 count = 0;
	ktime_t now = ktime_get();

	trace_iomfb_callback(dcp, tag,
			     "dcpep_cb_batched_swap_complete_ap_gated");

	/*
	 * Firmware passes pointer-rich arguments here, so avoid pretending we
	 * know the full layout. The callback itself is still a swap completion
	 * notification and should release any pending DRM page-flip event.
	 */
	if (dcp->callback_in_len >= 12)
		count = get_unaligned_le32(buf + 8);

	dev_info(dcp->dev,
		 "D590 batched_swap_complete_ap_gated in=%u out=%u count=%u raw=%*phN\n",
		 dcp->callback_in_len, dcp->callback_out_len, count, dump_len,
		 buf);

	dcp_drm_crtc_page_flip(dcp, now);
	return true;
}

/* special */
static void complete_vi_set_temperature_hint(struct apple_dcp *dcp, void *out, void *cookie)
{
	// ack D100 cb_match_pmu_service
	dcp_ack(dcp, DCP_CONTEXT_CB);
}

static bool iomfbep_cb_match_pmu_service(struct apple_dcp *dcp, int tag, void *out, void *in)
{
	trace_iomfb_callback(dcp, tag, __func__);
	iomfb_a358_vi_set_temperature_hint(dcp, false,
					   complete_vi_set_temperature_hint,
					   NULL);

	// return false for deferred ACK
	return false;
}

static void complete_pmu_service_matched(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_channel *ch = &dcp->ch_cb;
	u8 *succ = ch->output[ch->depth - 1];

	*succ = true;

	// ack D206 cb_match_pmu_service_2
	dcp_ack(dcp, DCP_CONTEXT_CB);
}

static bool iomfbep_cb_match_pmu_service_2(struct apple_dcp *dcp, int tag, void *out, void *in)
{
	trace_iomfb_callback(dcp, tag, __func__);

	iomfb_a131_pmu_service_matched(dcp, false, complete_pmu_service_matched,
				       out);

	// return false for deferred ACK
	return false;
}

static void complete_backlight_service_matched(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_channel *ch = &dcp->ch_cb;
	u8 *succ = ch->output[ch->depth - 1];

	*succ = true;

	// ack D206 cb_match_backlight_service
	dcp_ack(dcp, DCP_CONTEXT_CB);
}

static bool iomfbep_cb_match_backlight_service(struct apple_dcp *dcp, int tag, void *out, void *in)
{
	trace_iomfb_callback(dcp, tag, __func__);

	if (!dcp_has_panel(dcp)) {
		u8 *succ = out;
		*succ = true;
		return true;
	}

	iomfb_a132_backlight_service_matched(dcp, false, complete_backlight_service_matched, out);

	// return false for deferred ACK
	return false;
}

static void iomfb_cb_pr_publish(struct apple_dcp *dcp, u32 id, u32 value,
				u32 unk0, u32 unk1, u32 in_len)
{
	if (iomfb_trace_ipc)
		dev_info(dcp->dev,
			 "pr_publish id=%u value=%u unk0=0x%x unk1=0x%x in=%u\n",
			 id, value, unk0, unk1, in_len);

	switch (id) {
	case IOMFB_PROPERTY_NITS:
	{
		if (dcp_has_panel(dcp)) {
			dcp->brightness.nits = value / dcp->brightness.scale;
			/* notify backlight device of the initial brightness */
			if (!dcp->brightness.bl_dev && dcp->brightness.maximum > 0)
				schedule_work(&dcp->bl_register_wq);
			trace_iomfb_brightness(dcp, value);
		}
		break;
	}
	default:
		dev_dbg(dcp->dev, "pr_publish: id: %u = %u\n", id, value);
	}
}

static struct dcp_get_uint_prop_resp
dcpep_cb_get_uint_prop(struct apple_dcp *dcp, struct dcp_get_uint_prop_req *req)
{
	struct dcp_get_uint_prop_resp resp = (struct dcp_get_uint_prop_resp){
	    .value = 0
	};

	if (dcp->panel.has_mini_led &&
	    memcmp(req->obj, "SUMP", sizeof(req->obj)) == 0) { /* "PMUS */
	    if (strncmp(req->key, "Temperature", sizeof(req->key)) == 0) {
		/*
		 * TODO: value from j314c, find out if it is temperature in
		 *       centigrade C and which temperature sensor reports it
		 */
		resp.value = 3029;
		resp.ret = true;
	    }
	}

	return resp;
}

static u8 iomfbep_cb_sr_set_property_int(struct apple_dcp *dcp,
					 struct iomfb_sr_set_property_int_req *req)
{
	if (memcmp(req->obj, "FMOI", sizeof(req->obj)) == 0) { /* "IOMF */
		if (strncmp(req->key, "Brightness_Scale", sizeof(req->key)) == 0) {
			if (!req->value_null)
				dcp->brightness.scale = req->value;
		}
	}

	return 1;
}

static void iomfbep_cb_set_fx_prop(struct apple_dcp *dcp, struct iomfb_set_fx_prop_req *req)
{
    // TODO: trace this, see if there properties which needs to used later
}

/*
 * Callback to map a buffer allocated with allocate_buf for PIODMA usage.
 * PIODMA is separate from the main DCP and uses own IOVA space on a dedicated
 * stream of the display DART, rather than the expected DCP DART.
 */
static struct dcp_map_buf_resp dcpep_cb_map_piodma(struct apple_dcp *dcp,
						   struct dcp_map_buf_req *req)
{
	struct dcp_mem_descriptor *memdesc;
	struct sg_table *map;
	ssize_t ret;

	if (req->buffer >= ARRAY_SIZE(dcp->memdesc))
		goto reject;

	memdesc = &dcp->memdesc[req->buffer];
	map = &memdesc->map;

	if (!map->sgl)
		goto reject;

	/* use the piodma iommu domain to map against the right IOMMU */
	ret = iommu_map_sgtable(dcp->iommu_dom, memdesc->dva, map,
				IOMMU_READ | IOMMU_WRITE);

	/* HACK: expect size to be 16K aligned since the iommu API only maps
	 *       full pages
	 */
	if (ret < 0 || ret != ALIGN(memdesc->size, SZ_16K)) {
		dev_err(dcp->dev, "iommu_map_sgtable() returned %zd instead of expected buffer size of %zu\n", ret, memdesc->size);
		goto reject;
	}

	return (struct dcp_map_buf_resp){ .dva = memdesc->dva };

reject:
	dev_err(dcp->dev, "denying map of invalid buffer %llx for piodma\n",
		req->buffer);
	return (struct dcp_map_buf_resp){ .ret = EINVAL };
}

static void dcpep_cb_unmap_piodma(struct apple_dcp *dcp,
				  struct dcp_unmap_buf_resp *resp)
{
	struct dcp_mem_descriptor *memdesc;

	if (resp->buffer >= ARRAY_SIZE(dcp->memdesc)) {
		dev_warn(dcp->dev, "unmap request for out of range buffer %llu\n",
			 resp->buffer);
		return;
	}

	memdesc = &dcp->memdesc[resp->buffer];

	if (!memdesc->buf) {
		dev_warn(dcp->dev,
			 "unmap for non-mapped buffer %llu iova:0x%08llx\n",
			 resp->buffer, resp->dva);
		return;
	}

	if (memdesc->dva != resp->dva) {
		dev_warn(dcp->dev, "unmap buffer %llu address mismatch "
			 "memdesc.dva:%llx dva:%llx\n", resp->buffer,
			 memdesc->dva, resp->dva);
		return;
	}

	/* use the piodma iommu domain to unmap from the right IOMMU */
	/* HACK: expect size to be 16K aligned since the iommu API only maps
	 *       full pages
	 */
	iommu_unmap(dcp->iommu_dom, memdesc->dva, ALIGN(memdesc->size, SZ_16K));
}

/*
 * Allocate an IOVA contiguous buffer mapped to the DCP. The buffer need not be
 * physically contiguous, however we should save the sgtable in case the
 * buffer needs to be later mapped for PIODMA.
 */
static struct dcp_allocate_buffer_resp
dcpep_cb_allocate_buffer(struct apple_dcp *dcp,
			 struct dcp_allocate_buffer_req *req)
{
	struct dcp_allocate_buffer_resp resp = { 0 };
	struct dcp_mem_descriptor *memdesc;
	size_t size;
	u32 id;

	resp.dva_size = ALIGN(req->size, 4096);
	resp.mem_desc_id =
		find_first_zero_bit(dcp->memdesc_map, DCP_MAX_MAPPINGS);

	if (resp.mem_desc_id >= DCP_MAX_MAPPINGS) {
		dev_warn(dcp->dev, "DCP overflowed mapping table, ignoring\n");
		resp.dva_size = 0;
		resp.mem_desc_id = 0;
		return resp;
	}
	id = resp.mem_desc_id;
	set_bit(id, dcp->memdesc_map);

	memdesc = &dcp->memdesc[id];

	memdesc->size = resp.dva_size;
	/* HACK: align size to 16K since the iommu API only maps full pages */
	size = ALIGN(resp.dva_size, SZ_16K);
	memdesc->buf = dma_alloc_coherent(dcp->dev, size,
					  &memdesc->dva, GFP_KERNEL);

	dma_get_sgtable(dcp->dev, &memdesc->map, memdesc->buf, memdesc->dva,
			size);
	resp.dva = memdesc->dva;

	return resp;
}

static u8 dcpep_cb_release_mem_desc(struct apple_dcp *dcp, u32 *mem_desc_id)
{
	struct dcp_mem_descriptor *memdesc;
	size_t size;
	u32 id = *mem_desc_id;

	if (id >= DCP_MAX_MAPPINGS) {
		dev_warn(dcp->dev,
			 "unmap request for out of range mem_desc_id %u", id);
		return 0;
	}

	if (!test_and_clear_bit(id, dcp->memdesc_map)) {
		dev_warn(dcp->dev, "unmap request for unused mem_desc_id %u\n",
			 id);
		return 0;
	}

	memdesc = &dcp->memdesc[id];
	size = ALIGN(memdesc->size, SZ_16K);
	if (memdesc->buf) {
		dma_free_coherent(dcp->dev, size, memdesc->buf, memdesc->dva);
		memdesc->buf = NULL;
		memset(&memdesc->map, 0, sizeof(memdesc->map));
	} else {
		memdesc->reg = 0;
	}

	memdesc->size = 0;

	return 1;
}

/* Validate that the specified region is a display register */
static bool is_disp_register(struct apple_dcp *dcp, u64 start, u64 end)
{
	int i;

	for (i = 0; i < dcp->nr_disp_registers; ++i) {
		struct resource *r = dcp->disp_registers[i];

		if ((start >= r->start) && (end <= r->end))
			return true;
	}

	return false;
}

/*
 * Map contiguous physical memory into the DCP's address space. The firmware
 * uses this to map the display registers we advertise in
 * sr_map_device_memory_with_index, so we bounds check against that to guard
 * safe against malicious coprocessors.
 */
static struct dcp_map_physical_resp
dcpep_cb_map_physical(struct apple_dcp *dcp, struct dcp_map_physical_req *req)
{
	int size = ALIGN(req->size, 4096);
	dma_addr_t dva;
	u32 id;

	if (!is_disp_register(dcp, req->paddr, req->paddr + size - 1)) {
		dev_err(dcp->dev, "refusing to map phys address %llx size %llx\n",
			req->paddr, req->size);
		return (struct dcp_map_physical_resp){};
	}

	id = find_first_zero_bit(dcp->memdesc_map, DCP_MAX_MAPPINGS);
	set_bit(id, dcp->memdesc_map);
	dcp->memdesc[id].size = size;
	dcp->memdesc[id].reg = req->paddr;

	dva = dma_map_resource(dcp->dev, req->paddr, size, DMA_BIDIRECTIONAL, 0);
	WARN_ON(dva == DMA_MAPPING_ERROR);

	return (struct dcp_map_physical_resp){
		.dva_size = size,
		.mem_desc_id = id,
		.dva = dva,
	};
}

struct dcp_get_frequency_req {
	char obj[4];
	u32 arg;
} __packed;

static u64 dcpep_cb_get_frequency(struct apple_dcp *dcp,
				  struct dcp_get_frequency_req *req)
{
	u64 clk_rate = clk_get_rate(dcp->clk);
	u64 rate = iomfb_clock_frequency_override ?: clk_rate;

	if (iomfb_trace_ipc)
		dev_info(dcp->dev,
			 "getClockFrequency obj='%c%c%c%c' arg=%u clk_rate=%llu override=%lu ret=%llu\n",
			 req->obj[0], req->obj[1], req->obj[2], req->obj[3],
			 req->arg, clk_rate, iomfb_clock_frequency_override,
			 rate);

	return rate;
}

static struct DCP_FW_NAME(dcp_map_reg_resp) dcpep_cb_map_reg(struct apple_dcp *dcp,
						struct DCP_FW_NAME(dcp_map_reg_req) *req)
{
	if (req->index >= dcp->nr_disp_registers) {
		dev_warn(dcp->dev, "attempted to read invalid reg index %u\n",
			 req->index);

		return (struct DCP_FW_NAME(dcp_map_reg_resp)){ .ret = 1 };
	} else {
		struct resource *rsrc = dcp->disp_registers[req->index];
#if DCP_FW_VER >= DCP_FW_VERSION(13, 2, 0)
		dma_addr_t dva = dma_map_resource(dcp->dev, rsrc->start, resource_size(rsrc),
						  DMA_BIDIRECTIONAL, 0);
		WARN_ON(dva == DMA_MAPPING_ERROR);
#endif

		return (struct DCP_FW_NAME(dcp_map_reg_resp)){
			.addr = rsrc->start,
			.length = resource_size(rsrc),
#if DCP_FW_VER >= DCP_FW_VERSION(13, 2, 0)
			.dva = dva,
#endif
		};
	}
}

static struct dcp_read_edt_data_resp
dcpep_cb_read_edt_data(struct apple_dcp *dcp, struct dcp_read_edt_data_req *req)
{
	struct dcp_read_edt_data_resp resp = { .ret = 0 };

	memcpy(resp.value, req->value, sizeof(resp.value));

	if (iomfb_trace_ipc)
		dev_info(dcp->dev,
			 "read_edt_data key='%.*s' count=%u value0=0x%x\n",
			 (int)sizeof(req->key), req->key, req->count,
			 req->value[0]);

	if (iomfb_vid_clock_factor_override_enable && req->count > 0 &&
	    strncmp(req->key, "vid-clock-to-disp-clock-factor",
		    sizeof(req->key)) == 0) {
		resp.value[0] = iomfb_vid_clock_factor_override;
		resp.ret = iomfb_vid_clock_factor_success ? 1 : 0;
		dev_info(dcp->dev,
			 "read_edt_data overriding vid-clock-to-disp-clock-factor: 0x%x -> 0x%x ret=%u\n",
			 req->value[0], resp.value[0], resp.ret);
	}

	return resp;
}

struct dcp_default_fb_surface_req {
	u32 width;
	u32 height;
} __packed;

static u32
dcpep_cb_create_default_fb_surface(struct apple_dcp *dcp,
				   struct dcp_default_fb_surface_req *req)
{
	if (iomfb_trace_ipc) {
		dev_info(dcp->dev,
			 "create_default_fb_surface width=%u height=%u in=%u out=%u ret=%u\n",
			 req->width, req->height, dcp->callback_in_len,
			 dcp->callback_out_len,
			 iomfb_create_default_fb_surface_ret);

		if (dcp->boot_fb.valid) {
			resource_size_t notch_bytes;
			resource_size_t request_size;
			resource_size_t full_start = 0;
			resource_size_t full_end = 0;
			bool full_start_valid = false;
			bool fits_reserved = false;

			notch_bytes = (resource_size_t)dcp->notch_height *
				      dcp->boot_fb.stride;
			request_size = (resource_size_t)req->height *
				       dcp->boot_fb.stride;

			if (notch_bytes <= dcp->boot_fb.visible.start) {
				full_start = dcp->boot_fb.visible.start - notch_bytes;
				full_start_valid = true;

				if (request_size &&
				    full_start >= dcp->boot_fb.reserved.start &&
				    full_start <= dcp->boot_fb.reserved.end &&
				    request_size - 1 <=
					    dcp->boot_fb.reserved.end - full_start) {
					full_end = full_start + request_size - 1;
					fits_reserved = true;
				}
			}

			dev_info(dcp->dev,
				 "create_default_fb_surface bootfb visible=%pR reserved=%pR visible_size=%ux%u stride=%u format=%s notch=%u notch_bytes=0x%llx req_size=0x%llx full_start_valid=%u full_start=%pa full_end=%pa fits_reserved=%u\n",
				 &dcp->boot_fb.visible, &dcp->boot_fb.reserved,
				 dcp->boot_fb.width, dcp->boot_fb.visible_height,
				 dcp->boot_fb.stride, dcp->boot_fb.format,
				 dcp->notch_height, (unsigned long long)notch_bytes,
				 (unsigned long long)request_size, full_start_valid,
				 &full_start, &full_end, fits_reserved);
		}
	}

	return iomfb_create_default_fb_surface_ret;
}

static u32 dcpep_cb_get_display_default_stride(struct apple_dcp *dcp)
{
	u32 stride = iomfb_default_stride_override;

	if (!stride && dcp->boot_fb.valid)
		stride = dcp->boot_fb.stride;

	if (iomfb_trace_ipc)
		dev_info(dcp->dev,
			 "get_display_default_stride returning %u override=%u bootfb_valid=%u\n",
			 stride, iomfb_default_stride_override,
			 dcp->boot_fb.valid);

	return stride;
}

static void iomfbep_cb_enable_backlight_message_ap_gated(struct apple_dcp *dcp,
							 u8 *enabled)
{
	/*
	 * update backlight brightness on next swap, on non mini-LED displays
	 * DCP seems to set an invalid iDAC value after coming out of DPMS.
	 * syslog: "[BrightnessLCD.cpp:743][AFK]nitsToDBV: iDAC out of range"
	 */
	dcp->brightness.update = true;
	schedule_work(&dcp->bl_update_wq);
}

/* Chunked data transfer for property dictionaries */
static u8 dcpep_cb_prop_start(struct apple_dcp *dcp, u32 *length)
{
	if (iomfb_trace_ipc)
		dev_info(dcp->dev, "DCPAV prop start length=0x%x\n", *length);

	if (dcp->chunks.data != NULL) {
		dev_warn(dcp->dev, "ignoring spurious transfer start\n");
		return false;
	}

	dcp->chunks.length = *length;
	dcp->chunks.data = devm_kzalloc(dcp->dev, *length, GFP_KERNEL);

	if (!dcp->chunks.data) {
		dev_warn(dcp->dev, "failed to allocate chunks\n");
		return false;
	}

	return true;
}

static u8 dcpep_cb_prop_chunk(struct apple_dcp *dcp,
			      struct dcp_set_dcpav_prop_chunk_req *req)
{
	if (iomfb_trace_ipc)
		dev_info(dcp->dev, "DCPAV prop chunk offset=0x%x length=0x%x\n",
			 req->offset, req->length);

	if (!dcp->chunks.data) {
		dev_warn(dcp->dev, "ignoring spurious chunk\n");
		return false;
	}

	if (req->offset + req->length > dcp->chunks.length) {
		dev_warn(dcp->dev, "ignoring overflowing chunk\n");
		return false;
	}

	memcpy(dcp->chunks.data + req->offset, req->data, req->length);
	return true;
}

static bool __maybe_unused trampoline_prop_chunk_or_start(struct apple_dcp *dcp,
							  int tag, void *out,
							  void *in)
{
	u32 out_value;
	u8 resp;

	trace_iomfb_callback(dcp, tag, "dcpep_cb_prop_chunk_or_start");

	if (dcp->callback_in_len == sizeof(u32)) {
		u32 length;

		memcpy(&length, in, sizeof(length));
		if (iomfb_trace_ipc)
			dev_info(dcp->dev,
				 "DCPAV D127 using prop-start shape length=0x%x out=%u\n",
				 length, dcp->callback_out_len);
		resp = dcpep_cb_prop_start(dcp, in);
	} else {
		resp = dcpep_cb_prop_chunk(dcp, in);
	}

	out_value = resp ? 1 : 0;
	if (dcp->callback_out_len >= sizeof(out_value))
		memcpy(out, &out_value, sizeof(out_value));
	else if (dcp->callback_out_len)
		memcpy(out, &out_value, dcp->callback_out_len);

	return true;
}

static bool dcpep_process_chunks(struct apple_dcp *dcp,
				 struct dcp_set_dcpav_prop_end_req *req)
{
	struct dcp_parse_ctx ctx;
	int ret;

	if (!dcp->chunks.data) {
		dev_warn(dcp->dev, "ignoring spurious end\n");
		return false;
	}

	/* used just as opaque pointer for tracing */
	ctx.dcp = dcp;

	ret = parse(dcp->chunks.data, dcp->chunks.length, &ctx);

	if (ret) {
		dev_warn(dcp->dev, "bad header on dcpav props\n");
		return false;
	}

	if (!strcmp(req->key, "TimingElements")) {
		dcp->modes = enumerate_modes(&ctx, &dcp->nr_modes,
					     dcp->width_mm, dcp->height_mm,
					     dcp->notch_height);

		if (IS_ERR(dcp->modes)) {
			dev_warn(dcp->dev, "failed to parse modes\n");
			dcp->modes = NULL;
			dcp->nr_modes = 0;
			return false;
		}
		if (dcp->nr_modes == 0)
			dev_warn(dcp->dev, "TimingElements without valid modes!\n");
	} else if (!strcmp(req->key, "DisplayAttributes")) {
		ret = parse_display_attributes(&ctx, &dcp->width_mm,
					&dcp->height_mm);

		if (ret) {
			dev_warn(dcp->dev, "failed to parse display attribs\n");
			return false;
		}

		dcp_set_dimensions(dcp);
	}

	return true;
}

static u8 dcpep_cb_prop_end(struct apple_dcp *dcp,
			    struct dcp_set_dcpav_prop_end_req *req)
{
	u8 resp = dcpep_process_chunks(dcp, req);

	if (iomfb_trace_ipc)
		dev_info(dcp->dev,
			 "DCPAV prop end key=%s length=0x%zx resp=%u nr_modes=%u\n",
			 req->key, dcp->chunks.length, resp, dcp->nr_modes);

	/* move chunked data to connector to provide it via debugfs */
	dcp_connector_update_dict(dcp->connector, req->key, &dcp->chunks);
	dcp->chunks.data = NULL;
	dcp->chunks.length = 0;

	return resp;
}

static bool __maybe_unused trampoline_prop_end_or_chunk(struct apple_dcp *dcp,
							int tag, void *out,
							void *in)
{
	u32 out_value;
	u8 resp;

	trace_iomfb_callback(dcp, tag, "dcpep_cb_prop_end_or_chunk");

	if (dcp->callback_in_len == sizeof(struct dcp_set_dcpav_prop_chunk_req)) {
		if (iomfb_trace_ipc)
			dev_info(dcp->dev,
				 "DCPAV D128 using prop-chunk shape out=%u\n",
				 dcp->callback_out_len);
		resp = dcpep_cb_prop_chunk(dcp, in);
	} else {
		if (iomfb_trace_ipc)
			dev_info(dcp->dev,
				 "DCPAV D128 using prop-end shape in=%u out=%u\n",
				 dcp->callback_in_len, dcp->callback_out_len);
		resp = dcpep_cb_prop_end(dcp, in);
	}

	out_value = resp ? 1 : 0;
	if (dcp->callback_out_len >= sizeof(out_value))
		memcpy(out, &out_value, sizeof(out_value));
	else if (dcp->callback_out_len)
		memcpy(out, &out_value, dcp->callback_out_len);

	return true;
}

/* Boot sequence */
static void boot_done(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_channel *ch = &dcp->ch_cb;
	u8 *succ = ch->output[ch->depth - 1];
	dev_dbg(dcp->dev, "boot done\n");

	*succ = true;
	dcp_ack(dcp, DCP_CONTEXT_CB);
}

static void boot_5(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_set_display_refresh_properties(dcp, false, boot_done, NULL);
}

static void boot_4(struct apple_dcp *dcp, void *out, void *cookie)
{
#if DCP_FW_VER >= DCP_FW_VERSION(13, 2, 0)
	u32 v_true = 1;
	dcp_late_init_signal(dcp, false, &v_true, boot_5, NULL);
#else
	dcp_late_init_signal(dcp, false, boot_5, NULL);
#endif
}

static void boot_3(struct apple_dcp *dcp, void *out, void *cookie)
{
	u32 v_true = true;

	dcp_flush_supports_power(dcp, false, &v_true, boot_4, NULL);
}

static void boot_2(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_setup_video_limits(dcp, false, boot_3, NULL);
}

static void boot_1_5(struct apple_dcp *dcp, void *out, void *cookie)
{
	dcp_create_default_fb(dcp, false, boot_2, NULL);
}

/* Use special function signature to defer the ACK */
static bool dcpep_cb_boot_1(struct apple_dcp *dcp, int tag, void *out, void *in)
{
	trace_iomfb_callback(dcp, tag, __func__);
	dcp_set_create_dfb(dcp, false, boot_1_5, NULL);
	return false;
}

static bool __maybe_unused dcpep_cb_d121_or_boot_1(struct apple_dcp *dcp, int tag,
						   void *out, void *in)
{
	u32 out_value;

	if (iomfb_d121_run_boot_sequence) {
		trace_iomfb_callback(dcp, tag, "dcpep_cb_d121_boot_1");
		dev_info(dcp->dev,
			 "D121 handling as start_hardware_boot; running boot sequence\n");
		dcp_set_create_dfb(dcp, false, boot_1_5, NULL);
		return false;
	}

	trace_iomfb_callback(dcp, tag, "dcpep_cb_d121");
	out_value = iomfb_d121_force_true ? 1 : 0;
	if (iomfb_trace_ipc)
		dev_info(dcp->dev, "D121 returning %u\n", out_value);

	if (dcp->callback_out_len >= sizeof(out_value))
		memcpy(out, &out_value, sizeof(out_value));
	else if (dcp->callback_out_len)
		memcpy(out, &out_value, dcp->callback_out_len);

	return true;
}

static struct dcp_allocate_bandwidth_resp dcpep_cb_allocate_bandwidth(struct apple_dcp *dcp,
						struct dcp_allocate_bandwidth_req *req)
{
	return (struct dcp_allocate_bandwidth_resp){
		.unk1 = req->unk1,
		.unk2 = req->unk2,
		.ret = 1,
	};
}

static bool __maybe_unused trampoline_allocate_bandwidth_or_prop_end(struct apple_dcp *dcp,
								     int tag,
								     void *out,
								     void *in)
{
	trace_iomfb_callback(dcp, tag, "dcpep_cb_allocate_bandwidth_or_prop_end");

	if (dcp->callback_in_len == sizeof(struct dcp_set_dcpav_prop_end_req) &&
	    dcp->callback_out_len <= sizeof(u32)) {
		u32 out_value;
		u8 resp;

		if (iomfb_trace_ipc)
			dev_info(dcp->dev,
				 "DCPAV D129 using prop-end shape out=%u\n",
				 dcp->callback_out_len);
		resp = dcpep_cb_prop_end(dcp, in);
		out_value = resp ? 1 : 0;
		if (dcp->callback_out_len >= sizeof(out_value))
			memcpy(out, &out_value, sizeof(out_value));
		else if (dcp->callback_out_len)
			memcpy(out, &out_value, dcp->callback_out_len);
	} else {
		struct dcp_allocate_bandwidth_resp *typed_out = out;

		if (iomfb_trace_ipc)
			dev_info(dcp->dev,
				 "DCPAV D129 using allocate-bandwidth shape in=%u out=%u\n",
				 dcp->callback_in_len, dcp->callback_out_len);
		*typed_out = dcpep_cb_allocate_bandwidth(dcp, in);
	}

	return true;
}

static struct dcp_rt_bandwidth dcpep_cb_rt_bandwidth(struct apple_dcp *dcp)
{
	struct dcp_rt_bandwidth rt_bw = (struct dcp_rt_bandwidth){
			.reg_scratch = 0,
			.reg_doorbell = 0,
			.doorbell_bit = 0,
	};

	if (dcp->disp_bw_scratch_index) {
		u32 offset = dcp->disp_bw_scratch_offset;
		u32 index = dcp->disp_bw_scratch_index;
		rt_bw.reg_scratch = dcp->disp_registers[index]->start + offset;
	}

	if (dcp->disp_bw_doorbell_index) {
		u32 index = dcp->disp_bw_doorbell_index;
		rt_bw.reg_doorbell = dcp->disp_registers[index]->start;
		rt_bw.doorbell_bit = REG_DOORBELL_BIT(dcp->index);
		/*
		 * This is most certainly not padding. t8103-dcp crashes without
		 * setting this immediately during modeset on 12.3 and 13.5
		 * firmware.
		 */
		rt_bw.padding[3] = 0x4;
	}

	return rt_bw;
}

static struct dcp_set_frame_sync_props_resp
dcpep_cb_set_frame_sync_props(struct apple_dcp *dcp,
			      struct dcp_set_frame_sync_props_req *req)
{
	if (iomfb_trace_ipc)
		dev_info(dcp->dev,
			 "set_frame_sync_props in=%u out=%u data=%*ph\n",
			 dcp->callback_in_len, dcp->callback_out_len,
			 min_t(u32, dcp->callback_in_len, 64), req);

	return (struct dcp_set_frame_sync_props_resp){};
}

/* Callback to get the current time as milliseconds since the UNIX epoch */
static u64 dcpep_cb_get_time(struct apple_dcp *dcp)
{
	return ktime_to_ms(ktime_get_real());
}

static void dcpep_cb_update_backlight_factor_prop(struct apple_dcp *dcp,
						  u32 *value)
{
	if (iomfb_trace_ipc)
		dev_info(dcp->dev,
			 "update_backlight_factor_prop value=0x%x in=%u out=%u\n",
			 value ? *value : 0, dcp->callback_in_len,
			 dcp->callback_out_len);
}

struct dcp_swap_cookie {
	struct kref refcount;
	struct completion done;
	u32 swap_id;
};

static void release_swap_cookie(struct kref *ref)
{
	struct dcp_swap_cookie *cookie;
	cookie = container_of(ref, struct dcp_swap_cookie, refcount);

        kfree(cookie);
}

static void dcp_swap_cleared(struct apple_dcp *dcp, void *data, void *cookie)
{
	struct DCP_FW_NAME(dcp_swap_submit_resp) *resp = data;

	if (cookie) {
		struct dcp_swap_cookie *info = cookie;
		complete(&info->done);
		kref_put(&info->refcount, release_swap_cookie);
	}

	if (resp->ret) {
		dev_err(dcp->dev, "swap_clear failed! status %u\n", resp->ret);
		dcp_schedule_vblank(dcp, "swap-clear-failed");
		return;
	}

	while (!list_empty(&dcp->swapped_out_fbs)) {
		struct dcp_fb_reference *entry;
		entry = list_first_entry(&dcp->swapped_out_fbs,
					 struct dcp_fb_reference, head);
		if (entry->swap_id == dcp->last_swap_id)
			break;
		if (entry->fb)
			drm_framebuffer_put(entry->fb);
		list_del(&entry->head);
		kfree(entry);
	}
}

static void dcp_swap_clear_started(struct apple_dcp *dcp, void *data,
				   void *cookie)
{
	struct dcp_swap_start_resp *resp = data;
	DCP_FW_UNION(dcp->swap).swap.swap_id = resp->swap_id;

	if (cookie) {
		struct dcp_swap_cookie *info = cookie;
		info->swap_id = resp->swap_id;
	}

	dcp_swap_submit(dcp, false, &DCP_FW_UNION(dcp->swap), dcp_swap_cleared, cookie);
}

static void dcp_on_final(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_wait_cookie *wait = cookie;

	if (wait) {
		complete(&wait->done);
		kref_put(&wait->refcount, release_wait_cookie);
	}
}

static struct dcp_wait_cookie *dcp_wait_cookie_alloc(void)
{
	struct dcp_wait_cookie *cookie;

	cookie = kzalloc(sizeof(*cookie), GFP_KERNEL);
	if (!cookie)
		return NULL;

	init_completion(&cookie->done);
	kref_init(&cookie->refcount);
	/* The caller holds one reference; dcp_on_final drops the callback ref. */
	kref_get(&cookie->refcount);

	return cookie;
}

static int wait_for_iomfb_call_polled(struct apple_dcp *dcp,
				      struct dcp_wait_cookie *cookie,
				      const char *name, u32 timeout_ms)
{
	long left = msecs_to_jiffies(timeout_ms);
	int ret = 0;
	int total = 0;

	while (left > 0) {
		long slice = min_t(long, left, msecs_to_jiffies(10));
		long wait_ret;
		int polls;

		wait_ret = wait_for_completion_timeout(&cookie->done, slice);
		if (wait_ret > 0) {
			ret = 1;
			break;
		}

		polls = apple_rtkit_poll(dcp->rtk);
		if (polls) {
			total += polls;
			dev_info(dcp->dev, "%s: polled %d RTKit message(s)\n",
				 name, polls);
		}

		if (completion_done(&cookie->done)) {
			ret = 1;
			break;
		}

		left -= slice;
	}

	dev_info(dcp->dev, "%s: sync wait %s total_polls=%d\n", name,
		 ret ? "complete" : "timed out", total);
	kref_put(&cookie->refcount, release_wait_cookie);

	return ret ? 0 : -ETIMEDOUT;
}

static void dcp_on_set_power_state(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_set_power_state_req req = {
		.unklong = 1,
	};

	dcp_set_power_state(dcp, false, &req, dcp_on_final, cookie);
}

static u32 dcp_set_parameter_count(void)
{
#if DCP_FW_VER >= DCP_FW_VERSION(13, 2, 0)
	u32 count = 3;
#else
	u32 count = 1;
#endif

	if (iomfb_set_parameter_count_override)
		count = iomfb_set_parameter_count_override;

	return count;
}

static void dcp_on_set_parameter(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_set_parameter_dcp param = {
		.param = IOMFBPARAM_ADAPTIVE_SYNC,
		.value = { 0 },
		.count = dcp_set_parameter_count(),
	};

	dev_info(dcp->dev, "set_parameter_dcp adaptive-sync count:%u\n",
		 param.count);
	dcp_set_parameter_dcp(dcp, false, &param, dcp_on_set_power_state, cookie);
}

static void dcp_power_first_on_set_parameter(struct apple_dcp *dcp, void *out,
					     void *cookie)
{
	struct dcp_set_parameter_dcp param = {
		.param = IOMFBPARAM_ADAPTIVE_SYNC,
		.value = { 0 },
		.count = dcp_set_parameter_count(),
	};

	dev_info(dcp->dev, "power-first set_parameter_dcp adaptive-sync count:%u\n",
		 param.count);
	dcp_set_parameter_dcp(dcp, false, &param, dcp_on_final, cookie);
}

static void dcp_power_first_on_set_power_state(struct apple_dcp *dcp, void *out,
					       void *cookie)
{
	dcp_callback_t cb;
	u32 handle;

	handle = dcp->main_display ? 0 : 2;
	cb = (!dcp->main_display || iomfb_set_parameter_on_main) ?
		     dcp_power_first_on_set_parameter :
		     dcp_on_final;

	dev_info(dcp->dev, "power-first set_display_device handle:%u\n", handle);
	dcp_set_display_device(dcp, false, &handle, cb, cookie);
}

void DCP_FW_NAME(iomfb_poweron)(struct apple_dcp *dcp)
{
	struct dcp_wait_cookie *cookie;
	int ret;
	u32 handle;
	dev_info(dcp->dev, "dcp_poweron() starting\n");

	cookie = kzalloc(sizeof(*cookie), GFP_KERNEL);
	if (!cookie)
		return;

	init_completion(&cookie->done);
	kref_init(&cookie->refcount);
	/* increase refcount to ensure the receiver has a reference */
	kref_get(&cookie->refcount);

	if (iomfb_power_before_display_device) {
		struct dcp_set_power_state_req req = {
			.unklong = 1,
		};

		dev_info(dcp->dev,
			 "dcp_poweron: using power-before-display-device probe order\n");
		dcp_set_power_state(dcp, false, &req,
				    dcp_power_first_on_set_power_state, cookie);
	} else if (dcp->main_display) {
		handle = 0;
		dcp_set_display_device(dcp, false, &handle,
				       iomfb_set_parameter_on_main ?
					       dcp_on_set_parameter :
					       dcp_on_set_power_state,
					       cookie);
	} else {
		handle = 2;
		dcp_set_display_device(dcp, false, &handle,
				       dcp_on_set_parameter, cookie);
	}
	if (iomfb_poll_during_power_wait) {
		long left = msecs_to_jiffies(10000);

		ret = 0;
		while (left > 0) {
			long slice = min_t(long, left, msecs_to_jiffies(25));
			long wait_ret;
			int polls;

			wait_ret = wait_for_completion_timeout(&cookie->done,
							       slice);
			if (wait_ret > 0) {
				ret = wait_ret;
				break;
			}

			polls = apple_rtkit_poll(dcp->rtk);
			if (polls)
				dev_info(dcp->dev,
					 "dcp_poweron: polled %d RTKit message(s) while waiting for power\n",
					 polls);
			if (completion_done(&cookie->done)) {
				ret = 1;
				break;
			}

			left -= slice;
		}
	} else {
		ret = wait_for_completion_timeout(&cookie->done,
						  msecs_to_jiffies(10000));
	}

	if (ret == 0) {
		dev_warn(dcp->dev, "wait for power timed out, connector will be broken\n");
	} else if (ret > 0) {
		int msecs = jiffies_to_msecs(ret);
		if (msecs > 6000)
			dev_info(dcp->dev, "dcp_set_power_state_req returned, %d ms remaining\n", msecs);
		else
			dev_warn(dcp->dev, "dcp_set_power_state_req returned, %d ms remaining\n", msecs);
	} else {
		drm_connector_set_link_status_property(&dcp->connector->base,
						       DRM_MODE_LINK_STATUS_BAD);
		dev_warn(dcp->dev, "wait for completion error: %d\n", ret);
	}

	kref_put(&cookie->refcount, release_wait_cookie);;

	/* Force a brightness update after poweron, to restore the brightness */
	dcp->brightness.update = true;
}

static void complete_set_powerstate(struct apple_dcp *dcp, void *out,
				    void *cookie)
{
	struct dcp_wait_cookie *wait = cookie;

	if (wait) {
		complete(&wait->done);
		kref_put(&wait->refcount, release_wait_cookie);
	}
}

static void last_client_closed_poff(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_set_power_state_req power_req = {
		.unklong = 0,
	};
	dcp_set_power_state(dcp, false, &power_req, complete_set_powerstate,
			    cookie);
}

static void aborted_swaps_dcp_poff(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct iomfb_last_client_close_req last_client_req = {};
	iomfb_last_client_close(dcp, false, &last_client_req,
				last_client_closed_poff, cookie);
}

void DCP_FW_NAME(iomfb_poweroff)(struct apple_dcp *dcp)
{
	int ret, swap_id;
	struct iomfb_abort_swaps_dcp_req abort_req = {
		.client = {
			.flag2 = 1,
		},
	};
	struct dcp_swap_cookie *cookie;
	struct dcp_wait_cookie *poff_cookie;
	struct dcp_swap_start_req swap_req = { 0 };
	struct DCP_FW_NAME(dcp_swap_submit_req) *swap = &DCP_FW_UNION(dcp->swap);

	cookie = kzalloc(sizeof(*cookie), GFP_KERNEL);
	if (!cookie)
		return;
	init_completion(&cookie->done);
	kref_init(&cookie->refcount);
	/* increase refcount to ensure the receiver has a reference */
	kref_get(&cookie->refcount);

	// clear surfaces
	memset(swap, 0, sizeof(*swap));

	swap->swap.swap_enabled =
		swap->swap.swap_completed = IOMFB_SET_BACKGROUND | 0x7;
	swap->swap.bg_color = 0xFF000000;

	/*
	 * Turn off the backlight. This matters because the DCP's idea of
	 * backlight brightness gets desynced after a power change, and it
	 * needs to be told it's going to turn off so it will consider the
	 * subsequent update on poweron an actual change and restore the
	 * brightness.
	 */
	if (dcp_has_panel(dcp)) {
		swap->swap.bl_unk = 1;
		swap->swap.bl_value = 0;
		swap->swap.bl_power = 0;
	}

	/* Null all surfaces */
	for (int l = 0; l < SWAP_SURFACES; l++)
		swap->surf_null[l] = true;
#if DCP_FW_VER >= DCP_FW_VERSION(13, 2, 0)
	for (int l = 0; l < 5; l++)
		swap->surf2_null[l] = true;
	swap->unkU32Ptr_null = true;
	swap->unkU32out_null = true;
#endif

	dcp_swap_start(dcp, false, &swap_req, dcp_swap_clear_started, cookie);

	ret = wait_for_completion_timeout(&cookie->done, msecs_to_jiffies(50));
	swap_id = cookie->swap_id;
	kref_put(&cookie->refcount, release_swap_cookie);
	if (ret <= 0) {
		dcp->crashed = true;
		return;
	}

	dev_dbg(dcp->dev, "%s: clear swap submitted: %u\n", __func__, swap_id);

	poff_cookie = kzalloc(sizeof(*poff_cookie), GFP_KERNEL);
	if (!poff_cookie)
		return;
	init_completion(&poff_cookie->done);
	kref_init(&poff_cookie->refcount);
	/* increase refcount to ensure the receiver has a reference */
	kref_get(&poff_cookie->refcount);

	iomfb_abort_swaps_dcp(dcp, false, &abort_req,
				aborted_swaps_dcp_poff, poff_cookie);
	ret = wait_for_completion_timeout(&poff_cookie->done,
					  msecs_to_jiffies(1000));

	if (ret == 0)
		dev_warn(dcp->dev, "setPowerState(0) timeout %u ms\n", 1000);
	else if (ret > 0)
		dev_dbg(dcp->dev,
			"setPowerState(0) finished with %d ms to spare",
			jiffies_to_msecs(ret));

	kref_put(&poff_cookie->refcount, release_wait_cookie);

	dev_info(dcp->dev, "dcp_poweroff() done\n");
}

static void last_client_closed_sleep(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct dcp_set_power_state_req power_req = {
		.unklong = 0,
	};
	dcp_set_power_state(dcp, false, &power_req, complete_set_powerstate, cookie);
}

static void aborted_swaps_dcp_sleep(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct iomfb_last_client_close_req req = { 0 };
	iomfb_last_client_close(dcp, false, &req, last_client_closed_sleep, cookie);
}

void DCP_FW_NAME(iomfb_sleep)(struct apple_dcp *dcp)
{
	int ret;
	struct iomfb_abort_swaps_dcp_req req = {
		.client = {
			.flag2 = 1,
		},
	};

	struct dcp_wait_cookie *cookie;

	cookie = kzalloc(sizeof(*cookie), GFP_KERNEL);
	if (!cookie)
		return;
	init_completion(&cookie->done);
	kref_init(&cookie->refcount);
	/* increase refcount to ensure the receiver has a reference */
	kref_get(&cookie->refcount);

	iomfb_abort_swaps_dcp(dcp, false, &req, aborted_swaps_dcp_sleep,
				cookie);
	ret = wait_for_completion_timeout(&cookie->done,
					  msecs_to_jiffies(1000));

	if (ret == 0)
		dev_warn(dcp->dev, "setDCPPower(0) timeout %u ms\n", 1000);

	kref_put(&cookie->refcount, release_wait_cookie);
	dev_info(dcp->dev, "dcp_sleep() done\n");
}

static void dcpep_cb_hotplug(struct apple_dcp *dcp, u64 *connected)
{
	struct apple_connector *connector = dcp->connector;

	/* DCP issues hotplug_gated callbacks after SetPowerState() calls on
	 * devices with display (macbooks, imacs). This must not result in
	 * connector state changes on DRM side. Some applications won't enable
	 * a CRTC with a connector in disconnected state. Weston after DPMS off
	 * is one example. dcp_is_main_display() returns true on devices with
	 * integrated display. Ignore the hotplug_gated() callbacks there.
	 */
	if (dcp->main_display)
		return;

	if (dcp->during_modeset) {
		dev_info(dcp->dev,
			 "cb_hotplug() ignored during modeset connected:%llu\n",
			 *connected);
		return;
	}

	dev_info(dcp->dev, "cb_hotplug() connected:%llu, valid_mode:%d\n",
		 *connected, dcp->valid_mode);

	/* Hotplug invalidates mode. DRM doesn't always handle this. */
	if (!(*connected)) {
		dcp->valid_mode = false;
		/* after unplug swap will not complete until the next
		 * set_digital_out_mode */
		dcp_schedule_vblank(dcp, "hotplug-disconnect");
	}

	if (connector && connector->connected != !!(*connected)) {
		connector->connected = !!(*connected);
		dcp->valid_mode = false;
		schedule_work(&connector->hotplug_wq);
	}
}

static void
dcpep_cb_swap_complete_intent_gated(struct apple_dcp *dcp,
				    struct dcp_swap_complete_intent_gated *info)
{
	ktime_t now = ktime_get();

	trace_iomfb_swap_complete_intent_gated(dcp, info->swap_id,
		info->width, info->height);

	dev_info(dcp->dev,
		 "D591 swap_complete_intent_gated swap_id=%u unkBool=%u unkInt=0x%x size=%ux%u\n",
		 info->swap_id, info->unkBool, info->unkInt, info->width,
		 info->height);

	dcp->last_swap_id = info->swap_id;
	dcp_drm_crtc_page_flip(dcp, now);
}

static void
dcpep_cb_abort_swap_ap_gated(struct apple_dcp *dcp, u32 *swap_id)
{
	trace_iomfb_abort_swap_ap_gated(dcp, *swap_id);
}

static struct dcpep_get_tiling_state_resp
dcpep_cb_get_tiling_state(struct apple_dcp *dcp,
			  struct dcpep_get_tiling_state_req *req)
{
	return (struct dcpep_get_tiling_state_resp){
		.value = 0,
		.ret = 1,
	};
}

static u8 dcpep_cb_create_backlight_service(struct apple_dcp *dcp)
{
	return dcp_has_panel(dcp);
}

TRAMPOLINE_VOID(trampoline_nop, dcpep_cb_nop);
TRAMPOLINE_OUT(trampoline_true, dcpep_cb_true, u8);
TRAMPOLINE_OUT(trampoline_false, dcpep_cb_false, u8);
TRAMPOLINE_OUT(trampoline_zero, dcpep_cb_zero, u32);
TRAMPOLINE_OUT(trampoline_get_display_default_stride,
	       dcpep_cb_get_display_default_stride, u32);
TRAMPOLINE_OUT(trampoline_d121, dcpep_cb_d121, u32);
TRAMPOLINE_OUT(trampoline_d123, dcpep_cb_d123, u32);
TRAMPOLINE_OUT(trampoline_is_dfb_allocated, dcpep_cb_is_dfb_allocated, u8);
TRAMPOLINE_INOUT(trampoline_set_number_property, dcpep_cb_set_number_property,
		 struct dcp_set_number_property_req, u8);
TRAMPOLINE_INOUT(trampoline_set_property_dict, dcpep_cb_set_property_dict,
		 struct dcp_set_property_dict_req, u8);
TRAMPOLINE_INOUT(trampoline_set_property_int, dcpep_cb_set_property_int,
		 struct dcp_set_property_int_req, u8);
TRAMPOLINE_INOUT(trampoline_set_property_bool, dcpep_cb_set_property_bool,
		 struct dcp_set_property_bool_req, u8);
TRAMPOLINE_IN(trampoline_swap_complete, dcpep_cb_swap_complete,
	      struct DCP_FW_NAME(dc_swap_complete_resp));
TRAMPOLINE_INOUT(trampoline_get_uint_prop, dcpep_cb_get_uint_prop,
		 struct dcp_get_uint_prop_req, struct dcp_get_uint_prop_resp);
TRAMPOLINE_IN(trampoline_set_fx_prop, iomfbep_cb_set_fx_prop,
	      struct iomfb_set_fx_prop_req)
TRAMPOLINE_INOUT(trampoline_map_piodma, dcpep_cb_map_piodma,
		 struct dcp_map_buf_req, struct dcp_map_buf_resp);
TRAMPOLINE_IN(trampoline_unmap_piodma, dcpep_cb_unmap_piodma,
	      struct dcp_unmap_buf_resp);
TRAMPOLINE_INOUT(trampoline_sr_set_property_int, iomfbep_cb_sr_set_property_int,
		 struct iomfb_sr_set_property_int_req, u8);
TRAMPOLINE_INOUT(trampoline_allocate_buffer, dcpep_cb_allocate_buffer,
		 struct dcp_allocate_buffer_req,
		 struct dcp_allocate_buffer_resp);
TRAMPOLINE_INOUT(trampoline_map_physical, dcpep_cb_map_physical,
		 struct dcp_map_physical_req, struct dcp_map_physical_resp);
TRAMPOLINE_INOUT(trampoline_release_mem_desc, dcpep_cb_release_mem_desc, u32,
		 u8);
TRAMPOLINE_INOUT(trampoline_map_reg, dcpep_cb_map_reg,
		 struct DCP_FW_NAME(dcp_map_reg_req),
		 struct DCP_FW_NAME(dcp_map_reg_resp));
TRAMPOLINE_INOUT(trampoline_read_edt_data, dcpep_cb_read_edt_data,
		 struct dcp_read_edt_data_req, struct dcp_read_edt_data_resp);
TRAMPOLINE_INOUT(trampoline_prop_start, dcpep_cb_prop_start, u32, u8);
TRAMPOLINE_INOUT(trampoline_prop_chunk, dcpep_cb_prop_chunk,
		 struct dcp_set_dcpav_prop_chunk_req, u8);
TRAMPOLINE_INOUT(trampoline_prop_end, dcpep_cb_prop_end,
		 struct dcp_set_dcpav_prop_end_req, u8);
TRAMPOLINE_INOUT(trampoline_create_default_fb_surface,
		 dcpep_cb_create_default_fb_surface,
		 struct dcp_default_fb_surface_req, u32);
TRAMPOLINE_INOUT(trampoline_allocate_bandwidth, dcpep_cb_allocate_bandwidth,
	       struct dcp_allocate_bandwidth_req, struct dcp_allocate_bandwidth_resp);
TRAMPOLINE_OUT(trampoline_rt_bandwidth, dcpep_cb_rt_bandwidth,
	       struct dcp_rt_bandwidth);
static bool __maybe_unused trampoline_set_frame_sync_props(struct apple_dcp *dcp,
							   int tag, void *out,
							   void *in)
{
	struct dcp_set_frame_sync_props_resp resp;
	u32 copy_len;

	trace_iomfb_callback(dcp, tag, "dcpep_cb_set_frame_sync_props");
	resp = dcpep_cb_set_frame_sync_props(dcp, in);

	if (iomfb_frame_sync_copy_input) {
		copy_len = min(dcp->callback_in_len, dcp->callback_out_len);
		if (copy_len)
			memcpy(out, in, copy_len);
		if (iomfb_trace_ipc)
			dev_info(dcp->dev,
				 "set_frame_sync_props copied %u input byte(s) to output\n",
				 copy_len);
	} else {
		memcpy(out, &resp, min_t(u32, sizeof(resp),
					 dcp->callback_out_len));
	}

	return true;
}
TRAMPOLINE_INOUT(trampoline_get_frequency, dcpep_cb_get_frequency,
		 struct dcp_get_frequency_req, u64);
TRAMPOLINE_OUT(trampoline_get_time, dcpep_cb_get_time, u64);
TRAMPOLINE_IN(trampoline_update_backlight_factor_prop,
	      dcpep_cb_update_backlight_factor_prop, u32);
TRAMPOLINE_IN(trampoline_hotplug, dcpep_cb_hotplug, u64);
TRAMPOLINE_IN(trampoline_swap_complete_intent_gated,
	      dcpep_cb_swap_complete_intent_gated,
	      struct dcp_swap_complete_intent_gated);
TRAMPOLINE_IN(trampoline_abort_swap_ap_gated, dcpep_cb_abort_swap_ap_gated, u32);
TRAMPOLINE_IN(trampoline_enable_backlight_message_ap_gated,
	      iomfbep_cb_enable_backlight_message_ap_gated, u8);
static bool __maybe_unused trampoline_pr_publish(struct apple_dcp *dcp, int tag,
						 void *out, void *in)
{
	u8 *buf = in;
	u32 id = 0;
	u32 value = 0;
	u32 unk0 = 0;
	u32 unk1 = 0;

	trace_iomfb_callback(dcp, tag, "iomfb_cb_pr_publish");

	if (dcp->callback_in_len >= sizeof(id))
		memcpy(&id, buf, sizeof(id));
	if (dcp->callback_in_len >= sizeof(id) + sizeof(value))
		memcpy(&value, buf + sizeof(id), sizeof(value));
	if (dcp->callback_in_len >= sizeof(id) + sizeof(value) + sizeof(unk0))
		memcpy(&unk0, buf + sizeof(id) + sizeof(value), sizeof(unk0));
	if (dcp->callback_in_len >= sizeof(id) + sizeof(value) +
				    sizeof(unk0) + sizeof(unk1))
		memcpy(&unk1, buf + sizeof(id) + sizeof(value) + sizeof(unk0),
		       sizeof(unk1));

	iomfb_cb_pr_publish(dcp, id, value, unk0, unk1, dcp->callback_in_len);

	return true;
}
TRAMPOLINE_INOUT(trampoline_get_tiling_state, dcpep_cb_get_tiling_state,
		 struct dcpep_get_tiling_state_req, struct dcpep_get_tiling_state_resp);
TRAMPOLINE_OUT(trampoline_create_backlight_service, dcpep_cb_create_backlight_service, u8);

/*
 * Callback for swap requests. If a swap failed, we'll never get a swap
 * complete event so we need to fake a vblank event early to avoid a hang.
 */

static void poll_after_iomfb_call(struct apple_dcp *dcp, const char *name,
				  u32 timeout_ms);

static void dcp_swapped(struct apple_dcp *dcp, void *data, void *cookie)
{
	struct DCP_FW_NAME(dcp_swap_submit_resp) *resp = data;

	dev_info(dcp->dev, "swap_submit ack ret=%u unkoutbool=%u\n",
		 resp->ret, resp->unkoutbool);

	if (resp->ret) {
		dev_err(dcp->dev, "swap failed! status %u\n", resp->ret);
		dcp_schedule_vblank(dcp, "swap-submit-failed");
		return;
	}
	dcp->swap_start = ktime_get();
	if (iomfb_poll_after_swap_submit_ack_ms)
		poll_after_iomfb_call(dcp, "swap_submit_ack",
				      iomfb_poll_after_swap_submit_ack_ms);
	if (iomfb_fake_pageflip_on_swap_submit_ack) {
		dev_warn(dcp->dev,
			 "diagnostic: faking DRM pageflip on successful swap_submit ACK\n");
		dcp_drm_crtc_page_flip(dcp, dcp->swap_start);
	}

	while (!list_empty(&dcp->swapped_out_fbs)) {
		struct dcp_fb_reference *entry;
		entry = list_first_entry(&dcp->swapped_out_fbs,
					 struct dcp_fb_reference, head);
		if (entry->swap_id == dcp->last_swap_id)
			break;
		if (entry->fb)
			drm_framebuffer_put(entry->fb);
		list_del(&entry->head);
		kfree(entry);
	}
}

struct swap_matrix_cookie {
	bool set_matrix_before_submit;
	struct iomfb_set_matrix_req mat;
};

static void log_swap_submit_req(struct apple_dcp *dcp,
				struct DCP_FW_NAME(dcp_swap_submit_req) *req)
{
	size_t off_swap_enabled = offsetof(struct DCP_FW_NAME(dcp_swap),
					   swap_enabled);
	size_t off_swap_completed = offsetof(struct DCP_FW_NAME(dcp_swap),
					     swap_completed);
	size_t off_bl_unk = offsetof(struct DCP_FW_NAME(dcp_swap), bl_unk);
	int i;

	dev_info(dcp->dev,
		 "swap_submit layout req=0x%zx swap=0x%zx surface=0x%zx "
		 "off_enabled=0x%zx off_completed=0x%zx off_bl_unk=0x%zx\n",
		 sizeof(*req), sizeof(req->swap), sizeof(req->surf[0]),
		 off_swap_enabled, off_swap_completed, off_bl_unk);

	dev_info(dcp->dev,
		 "swap_submit nulls swap=%u surf=%u,%u,%u,%u unkout=%u clear=%u "
		 "unkbool=%u unkdouble=0x%llx\n",
		 req->swap_null, req->surf_null[0], req->surf_null[1],
		 req->surf_null[2], req->surf_null[3], req->unkoutbool_null,
		 req->clear, req->unkbool, req->unkdouble);

#if DCP_FW_VER >= DCP_FW_VERSION(13, 2, 0)
	dev_info(dcp->dev,
		 "swap_submit v13 extra unkU64=0x%llx unkbool2=%u unkU32Ptr=0x%x "
		 "unkU32Ptr_null=%u unkU32out_null=%u surf2_null=%u,%u,%u,%u,%u\n",
		 req->unkU64, req->unkbool2, req->unkU32Ptr,
		 req->unkU32Ptr_null, req->unkU32out_null, req->surf2_null[0],
		 req->surf2_null[1], req->surf2_null[2], req->surf2_null[3],
		 req->surf2_null[4]);

	dev_info(dcp->dev,
		 "swap_submit v13 unk_u64_a=%llx,%llx,%llx,%llx "
		 "surf2_iova=%llx,%llx,%llx,%llx,%llx\n",
		 req->unk_u64_a[0], req->unk_u64_a[1], req->unk_u64_a[2],
		 req->unk_u64_a[3], req->surf2_iova[0], req->surf2_iova[1],
		 req->surf2_iova[2], req->surf2_iova[3], req->surf2_iova[4]);
#endif

	for (i = 0; i < SWAP_SURFACES; i++) {
		dev_info(dcp->dev,
			 "swap_submit slot%d surf_null=%u surf_id=%u surf_flag=0x%x "
			 "surf_unk=0x%x iova=0x%llx src=%u,%u %ux%u dst=%u,%u %ux%u\n",
			 i, req->surf_null[i], req->swap.surf_ids[i],
			 req->swap.surf_flags[i], req->swap.surf_unk[i],
			 req->surf_iova[i], req->swap.src_rect[i].x,
			 req->swap.src_rect[i].y, req->swap.src_rect[i].w,
			 req->swap.src_rect[i].h, req->swap.dst_rect[i].x,
			 req->swap.dst_rect[i].y, req->swap.dst_rect[i].w,
			 req->swap.dst_rect[i].h);
	}
}

static void submit_started_swap(struct apple_dcp *dcp)
{
	struct DCP_FW_NAME(dcp_swap_submit_req) *req = &DCP_FW_UNION(dcp->swap);
	u32 swap_id = req->swap.swap_id;

	trace_iomfb_swap_submit(dcp, swap_id);
	dev_info(dcp->dev, "submitting swap_id=%u\n", swap_id);
	log_swap_submit_req(dcp, req);
	dcp_swap_submit(dcp, false, req, dcp_swapped, NULL);

	if (iomfb_poll_after_swap_submit_ms)
		poll_after_iomfb_call(dcp, "swap_submit",
				      iomfb_poll_after_swap_submit_ms);
}

static void dcp_post_swap_param6_done(struct apple_dcp *dcp, void *data,
				      void *cookie)
{
	u32 ret = data ? *(u32 *)data : 0;

	dev_info(dcp->dev, "post-swap_start set_parameter_dcp(6) ret=%u\n",
		 ret);
	submit_started_swap(dcp);
	kfree(cookie);
}

static void dcp_post_swap_param3_done(struct apple_dcp *dcp, void *data,
				      void *cookie)
{
	struct dcp_set_parameter_dcp param = {
		.param = 6,
		.value = { 65536 },
		.count = 1,
	};
	u32 ret = data ? *(u32 *)data : 0;

	dev_info(dcp->dev, "post-swap_start set_parameter_dcp(3) ret=%u\n",
		 ret);
	dev_info(dcp->dev, "post-swap_start set_parameter_dcp param=6 value=65536\n");
	dcp_set_parameter_dcp(dcp, false, &param, dcp_post_swap_param6_done,
			      cookie);
}

static void dcp_post_swap_start_param3(struct apple_dcp *dcp, void *cookie)
{
	struct dcp_set_parameter_dcp param = {
		.param = 3,
		.value = { 65536 },
		.count = 1,
	};

	dev_info(dcp->dev, "post-swap_start set_parameter_dcp param=3 value=65536\n");
	dcp_set_parameter_dcp(dcp, false, &param, dcp_post_swap_param3_done,
			      cookie);
}

static void dcp_post_swap_brightness_done(struct apple_dcp *dcp, void *data,
					  void *cookie)
{
	u32 ret = data ? *(u32 *)data : 0;

	dev_info(dcp->dev, "post-swap_start setBrightnessCorrection ret=%u\n",
		 ret);
	dcp_post_swap_start_param3(dcp, cookie);
}

static void submit_after_post_swap_init(struct apple_dcp *dcp, void *cookie)
{
	u32 value = 65536;

	if (!iomfb_m1n1_post_swap_init) {
		submit_started_swap(dcp);
		kfree(cookie);
		return;
	}

	if (iomfb_m1n1_post_swap_skip_brightness) {
		dev_info(dcp->dev,
			 "skipping post-swap_start setBrightnessCorrection for probe\n");
		dcp_post_swap_start_param3(dcp, cookie);
		return;
	}

	dev_info(dcp->dev,
		 "post-swap_start setBrightnessCorrection value=65536\n");
	dcp_set_brightness_correction(dcp, false, &value,
				      dcp_post_swap_brightness_done, cookie);
}

static void dcp_set_matrix_then_submit(struct apple_dcp *dcp, void *data,
				       void *cookie)
{
	struct iomfb_set_matrix_resp *resp = data;

	dev_info(dcp->dev, "post-swap_start set_matrix ack ret=%u\n",
		 resp->ret);

	submit_after_post_swap_init(dcp, cookie);
}

static void dcp_swap_started(struct apple_dcp *dcp, void *data, void *cookie)
{
	struct dcp_swap_start_resp *resp = data;
	struct swap_matrix_cookie *swap_cookie = cookie;

	DCP_FW_UNION(dcp->swap).swap.swap_id = resp->swap_id;

	dev_info(dcp->dev,
		 "swap_start ack swap_id=%u ret=%u client=0x%llx unk=0x%x flag1=%u flag2=%u\n",
		 resp->swap_id, resp->ret, resp->client.handle,
		 resp->client.unk, resp->client.flag1, resp->client.flag2);

	if (resp->ret) {
		dev_err(dcp->dev, "swap_start failed! status %u\n", resp->ret);
		kfree(swap_cookie);
		dcp_schedule_vblank(dcp, "swap-start-failed");
		return;
	}

	if (swap_cookie && swap_cookie->set_matrix_before_submit) {
		dev_info(dcp->dev,
			 "issuing set_matrix after swap_start for probe\n");
		iomfb_set_matrix(dcp, false, &swap_cookie->mat,
				 dcp_set_matrix_then_submit, swap_cookie);
		return;
	}

	submit_after_post_swap_init(dcp, swap_cookie);
}

static void poll_after_iomfb_call(struct apple_dcp *dcp, const char *name,
				  u32 timeout_ms)
{
	u32 remaining = timeout_ms;
	int total = 0;

	while (remaining) {
		u32 interval = min_t(u32, remaining, 5);
		int polls = apple_rtkit_poll(dcp->rtk);

		if (polls) {
			total += polls;
			dev_info(dcp->dev,
				 "%s: polled %d RTKit message(s)\n",
				 name, polls);
		}

		msleep(interval);
		remaining -= interval;
	}

	dev_info(dcp->dev, "%s: poll window complete, total=%d\n",
		 name, total);
}

static void poll_after_pre_swap_call(struct apple_dcp *dcp, const char *name)
{
	static bool pre_swap_polling;

	if (!iomfb_poll_after_pre_swap_ms)
		return;

	if (pre_swap_polling) {
		dev_info(dcp->dev,
			 "%s: pre-swap poll already active, using outer window\n",
			 name);
		return;
	}

	pre_swap_polling = true;
	poll_after_iomfb_call(dcp, name, iomfb_poll_after_pre_swap_ms);
	pre_swap_polling = false;
}

static void start_swap_after_preinit(struct apple_dcp *dcp,
				     struct swap_matrix_cookie *swap_cookie)
{
	struct dcp_swap_start_req start_req = { 0 };

	start_req.client.handle = iomfb_swap_start_client_handle;
	start_req.client.unk = iomfb_swap_start_client_unk;

	if (iomfb_swap_start_client_flag2) {
		start_req.client.flag2 = 1;
	}

	if (iomfb_swap_start_client_handle || iomfb_swap_start_client_unk ||
	    iomfb_swap_start_client_flag2)
		dev_info(dcp->dev,
			 "swap_start client probe: handle=0x%llx unk=0x%x flag1=%u flag2=%u\n",
			 start_req.client.handle, start_req.client.unk,
			 start_req.client.flag1,
			 start_req.client.flag2);

	if (dcp->connector && dcp->connector->connected) {
		dcp_swap_start(dcp, false, &start_req, dcp_swap_started,
			       swap_cookie);

		if (iomfb_poll_after_swap_start_ms)
			poll_after_iomfb_call(dcp, "swap_start",
					      iomfb_poll_after_swap_start_ms);
	} else {
		kfree(swap_cookie);
		dcp_schedule_vblank(dcp, "swap-no-connector");
	}
}

static void dcp_m1n1_pre_swap_param2_done(struct apple_dcp *dcp, void *data,
					  void *cookie)
{
	u32 ret = data ? *(u32 *)data : 0;

	dev_info(dcp->dev, "pre-swap set_parameter_dcp(14) second ret=%u\n",
		 ret);
	start_swap_after_preinit(dcp, cookie);
}

static void dcp_m1n1_pre_swap_display2_done(struct apple_dcp *dcp, void *data,
					    void *cookie)
{
	struct dcp_set_parameter_dcp param = {
		.param = IOMFBPARAM_ADAPTIVE_SYNC,
		.value = { 0 },
		.count = 1,
	};
	u32 ret = data ? *(u32 *)data : 0;

	dev_info(dcp->dev, "pre-swap set_display_device second ret=%u\n", ret);
	dev_info(dcp->dev, "pre-swap set_parameter_dcp param=14 value=0 count=1 second\n");
	dcp_set_parameter_dcp(dcp, false, &param,
			      dcp_m1n1_pre_swap_param2_done, cookie);
	poll_after_pre_swap_call(dcp, "pre-swap set_parameter_dcp second");
}

static void dcp_m1n1_pre_swap_brightness_done(struct apple_dcp *dcp,
					      void *data, void *cookie)
{
	u32 ret = data ? *(u32 *)data : 0;
	u32 handle = dcp->main_display ? 0 : 2;

	dev_info(dcp->dev, "pre-swap setBrightnessCorrection ret=%u\n", ret);
	dev_info(dcp->dev, "pre-swap set_display_device handle=%u second\n",
		 handle);
	dcp_set_display_device(dcp, false, &handle,
			       dcp_m1n1_pre_swap_display2_done, cookie);
	poll_after_pre_swap_call(dcp, "pre-swap set_display_device second");
}

static void dcp_m1n1_pre_swap_contrast_done(struct apple_dcp *dcp, void *data,
					    void *cookie)
{
	struct dcp_set_contrast_resp *resp = data;
	u32 value = 65536;

	if (resp)
		dev_info(dcp->dev,
			 "pre-swap set_contrast ret=%u value=0x%x\n",
			 resp->ret, resp->value);
	else
		dev_info(dcp->dev, "pre-swap set_contrast returned no data\n");

	dev_info(dcp->dev, "pre-swap setBrightnessCorrection value=65536\n");
	dcp_set_brightness_correction(dcp, false, &value,
				      dcp_m1n1_pre_swap_brightness_done,
				      cookie);
	poll_after_pre_swap_call(dcp, "pre-swap setBrightnessCorrection");
}

static void dcp_m1n1_pre_swap_gamma_done(struct apple_dcp *dcp, void *data,
					 void *cookie)
{
	struct dcp_get_gamma_table_resp *resp = data;
	struct dcp_set_contrast_req req = { 0 };

	if (resp)
		dev_info(dcp->dev,
			 "pre-swap get_gamma_table ret=%u first=%*ph\n",
			 resp->ret, 16, resp->table);
	else
		dev_info(dcp->dev, "pre-swap get_gamma_table returned no data\n");

	dev_info(dcp->dev, "pre-swap set_contrast value=0\n");
	dcp_set_contrast(dcp, false, &req, dcp_m1n1_pre_swap_contrast_done,
			 cookie);
	poll_after_pre_swap_call(dcp, "pre-swap set_contrast");
}

static void dcp_m1n1_pre_swap_start_contrast(struct apple_dcp *dcp,
					     void *cookie)
{
	struct dcp_set_contrast_req req = { 0 };

	if (iomfb_m1n1_pre_swap_skip_contrast) {
		u32 value = 65536;

		dev_info(dcp->dev, "pre-swap skipping set_contrast for probe\n");
		dev_info(dcp->dev,
			 "pre-swap setBrightnessCorrection value=65536\n");
		dcp_set_brightness_correction(dcp, false, &value,
					      dcp_m1n1_pre_swap_brightness_done,
					      cookie);
		poll_after_pre_swap_call(dcp,
					 "pre-swap setBrightnessCorrection");
		return;
	}

	dev_info(dcp->dev, "pre-swap set_contrast value=0\n");
	dcp_set_contrast(dcp, false, &req, dcp_m1n1_pre_swap_contrast_done,
			 cookie);
	poll_after_pre_swap_call(dcp, "pre-swap set_contrast");
}

static void dcp_m1n1_pre_swap_param1_done(struct apple_dcp *dcp, void *data,
					  void *cookie)
{
	struct dcp_get_gamma_table_req *req;
	u32 ret = data ? *(u32 *)data : 0;

	dev_info(dcp->dev, "pre-swap set_parameter_dcp(14) first ret=%u\n",
		 ret);

	if (iomfb_m1n1_pre_swap_skip_gamma) {
		dev_info(dcp->dev, "pre-swap skipping get_gamma_table for probe\n");
		dcp_m1n1_pre_swap_start_contrast(dcp, cookie);
		return;
	}

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req) {
		dev_warn(dcp->dev,
			 "pre-swap get_gamma_table allocation failed; continuing swap\n");
		dcp_m1n1_pre_swap_start_contrast(dcp, cookie);
		return;
	}

	dev_info(dcp->dev, "pre-swap get_gamma_table\n");
	dcp_get_gamma_table(dcp, false, req, dcp_m1n1_pre_swap_gamma_done,
			    cookie);
	poll_after_pre_swap_call(dcp, "pre-swap get_gamma_table");
	kfree(req);
}

static void dcp_m1n1_pre_swap_display1_done(struct apple_dcp *dcp, void *data,
					    void *cookie)
{
	struct dcp_set_parameter_dcp param = {
		.param = IOMFBPARAM_ADAPTIVE_SYNC,
		.value = { 0 },
		.count = 1,
	};
	u32 ret = data ? *(u32 *)data : 0;

	dev_info(dcp->dev, "pre-swap set_display_device first ret=%u\n", ret);
	dev_info(dcp->dev, "pre-swap set_parameter_dcp param=14 value=0 count=1 first\n");
	dcp_set_parameter_dcp(dcp, false, &param,
			      dcp_m1n1_pre_swap_param1_done, cookie);
	poll_after_pre_swap_call(dcp, "pre-swap set_parameter_dcp first");
}

static void dcp_m1n1_pre_swap_start(struct apple_dcp *dcp,
				    struct swap_matrix_cookie *swap_cookie)
{
	u32 handle = dcp->main_display ? 0 : 2;

	dev_info(dcp->dev,
		 "pre-swap m1n1-style init: set_display_device handle=%u first\n",
		 handle);
	dcp_set_display_device(dcp, false, &handle,
			       dcp_m1n1_pre_swap_display1_done, swap_cookie);
	poll_after_pre_swap_call(dcp, "pre-swap set_display_device first");
}

static int dcp_wait_display_device(struct apple_dcp *dcp, u32 handle,
				   const char *name)
{
	struct dcp_wait_cookie *cookie;
	int ret;

	cookie = dcp_wait_cookie_alloc();
	if (!cookie)
		return -ENOMEM;

	dev_info(dcp->dev, "%s handle=%u\n", name, handle);
	dcp_set_display_device(dcp, false, &handle, dcp_on_final, cookie);
	ret = wait_for_iomfb_call_polled(dcp, cookie, name, 1000);
	return ret;
}

static int dcp_wait_set_parameter(struct apple_dcp *dcp, u32 param_id,
				  u64 value, u32 count, const char *name)
{
	struct dcp_set_parameter_dcp param = {
		.param = param_id,
		.value = { value },
		.count = count,
	};
	struct dcp_wait_cookie *cookie;
	int ret;

	cookie = dcp_wait_cookie_alloc();
	if (!cookie)
		return -ENOMEM;

	dev_info(dcp->dev, "%s param=%u value=%llu count=%u\n", name,
		 param_id, value, count);
	dcp_set_parameter_dcp(dcp, false, &param, dcp_on_final, cookie);
	ret = wait_for_iomfb_call_polled(dcp, cookie, name, 1000);
	return ret;
}

static int dcp_wait_get_gamma_table(struct apple_dcp *dcp)
{
	struct dcp_get_gamma_table_req *req;
	struct dcp_wait_cookie *cookie;
	int ret;

	req = kzalloc(sizeof(*req), GFP_KERNEL);
	if (!req)
		return -ENOMEM;

	cookie = dcp_wait_cookie_alloc();
	if (!cookie) {
		kfree(req);
		return -ENOMEM;
	}

	dev_info(dcp->dev, "pre-swap sync get_gamma_table\n");
	dcp_get_gamma_table(dcp, false, req, dcp_on_final, cookie);
	ret = wait_for_iomfb_call_polled(dcp, cookie,
					 "pre-swap sync get_gamma_table", 1000);
	kfree(req);
	return ret;
}

static int dcp_wait_set_contrast(struct apple_dcp *dcp)
{
	struct dcp_set_contrast_req req = { 0 };
	struct dcp_wait_cookie *cookie;
	int ret;

	cookie = dcp_wait_cookie_alloc();
	if (!cookie)
		return -ENOMEM;

	dev_info(dcp->dev, "pre-swap sync set_contrast value=0\n");
	dcp_set_contrast(dcp, false, &req, dcp_on_final, cookie);
	ret = wait_for_iomfb_call_polled(dcp, cookie,
					 "pre-swap sync set_contrast", 1000);
	return ret;
}

static int dcp_wait_brightness_correction(struct apple_dcp *dcp)
{
	struct dcp_wait_cookie *cookie;
	u32 value = 65536;
	int ret;

	cookie = dcp_wait_cookie_alloc();
	if (!cookie)
		return -ENOMEM;

	dev_info(dcp->dev, "pre-swap sync setBrightnessCorrection value=65536\n");
	dcp_set_brightness_correction(dcp, false, &value, dcp_on_final,
				      cookie);
	ret = wait_for_iomfb_call_polled(dcp, cookie,
					 "pre-swap sync setBrightnessCorrection",
					 1000);
	return ret;
}

static int dcp_m1n1_pre_swap_sync_run(struct apple_dcp *dcp)
{
	u32 handle = dcp->main_display ? 0 : 2;
	u32 count = dcp_set_parameter_count();
	int ret;

	dev_info(dcp->dev,
		 "pre-swap sync m1n1-style init: handle=%u count=%u skip_gamma=%u skip_contrast=%u\n",
		 handle, count, iomfb_m1n1_pre_swap_skip_gamma,
		 iomfb_m1n1_pre_swap_skip_contrast);

	ret = dcp_wait_display_device(dcp, handle,
				      "pre-swap sync set_display_device first");
	if (ret)
		return ret;

	ret = dcp_wait_set_parameter(dcp, IOMFBPARAM_ADAPTIVE_SYNC, 0, count,
				     "pre-swap sync set_parameter_dcp first");
	if (ret)
		return ret;

	if (!iomfb_m1n1_pre_swap_skip_gamma) {
		ret = dcp_wait_get_gamma_table(dcp);
		if (ret)
			return ret;
	} else {
		dev_info(dcp->dev, "pre-swap sync skipping get_gamma_table\n");
	}

	if (!iomfb_m1n1_pre_swap_skip_contrast) {
		ret = dcp_wait_set_contrast(dcp);
		if (ret)
			return ret;
	} else {
		dev_info(dcp->dev, "pre-swap sync skipping set_contrast\n");
	}

	ret = dcp_wait_brightness_correction(dcp);
	if (ret)
		return ret;

	ret = dcp_wait_display_device(dcp, handle,
				      "pre-swap sync set_display_device second");
	if (ret)
		return ret;

	ret = dcp_wait_set_parameter(dcp, IOMFBPARAM_ADAPTIVE_SYNC, 0, count,
				     "pre-swap sync set_parameter_dcp second");
	if (ret)
		return ret;

	return 0;
}

/* Helpers to modeset and swap, used to flush */
static void do_swap(struct apple_dcp *dcp, void *data, void *cookie)
{
	struct swap_matrix_cookie *swap_cookie = cookie;

	if (iomfb_m1n1_pre_swap_init &&
	    dcp->connector && dcp->connector->connected) {
		if (iomfb_m1n1_pre_swap_sync) {
			int ret = dcp_m1n1_pre_swap_sync_run(dcp);

			if (ret)
				dev_warn(dcp->dev,
					 "pre-swap sync sequence failed: %d; continuing swap\n",
					 ret);
			start_swap_after_preinit(dcp, swap_cookie);
			return;
		}
		dcp_m1n1_pre_swap_start(dcp, swap_cookie);
		return;
	}

	start_swap_after_preinit(dcp, swap_cookie);
}

static void complete_set_digital_out_mode(struct apple_dcp *dcp, void *data,
					  void *cookie)
{
	struct dcp_wait_cookie *wait = cookie;

	if (wait) {
		complete(&wait->done);
		kref_put(&wait->refcount, release_wait_cookie);
	}
}

int DCP_FW_NAME(iomfb_modeset)(struct apple_dcp *dcp,
			       struct drm_crtc_state *crtc_state)
{
	struct dcp_display_mode *mode;
	struct dcp_wait_cookie *cookie;
	struct dcp_color_mode *cmode = NULL;
	int ret;

	mode = lookup_mode(dcp, &crtc_state->mode);
	if (!mode) {
		dev_err(dcp->dev, "no match for " DRM_MODE_FMT "\n",
			DRM_MODE_ARG(&crtc_state->mode));
		return -EIO;
	}

	if (iomfb_skip_modeset_mark_valid) {
		dcp->mode = (struct dcp_set_digital_out_mode_req){
			.color_mode_id = mode->color_mode_id,
			.timing_mode_id = mode->timing_mode_id
		};
		dcp->use_timestamps = mode->vrr;
		dcp->valid_mode = true;
		dev_info(dcp->dev,
			 "skipping set_digital_out_mode and marking mode valid "
			 "color:%d timing:%d " DRM_MODE_FMT "\n",
			 mode->color_mode_id, mode->timing_mode_id,
			 DRM_MODE_ARG(&crtc_state->mode));
		return 0;
	}

	if (iomfb_skip_invalid_modeset && !dcp->valid_mode) {
		dev_info(dcp->dev,
			 "skipping set_digital_out_mode for invalid hotplug mode color:%d timing:%d "
			 DRM_MODE_FMT "\n",
			 mode->color_mode_id, mode->timing_mode_id,
			 DRM_MODE_ARG(&crtc_state->mode));
		return -EIO;
	}

	dev_info(dcp->dev,
		 "set_digital_out_mode(color:%d timing:%d valid:%d) " DRM_MODE_FMT "\n",
		 mode->color_mode_id, mode->timing_mode_id,
		 dcp->valid_mode,
		 DRM_MODE_ARG(&crtc_state->mode));
	if (mode->color_mode_id == mode->sdr_rgb.id)
		cmode = &mode->sdr_rgb;
	else if (mode->color_mode_id == mode->sdr_444.id)
		cmode = &mode->sdr_444;
	else if (mode->color_mode_id == mode->sdr.id)
		cmode = &mode->sdr;
	else if (mode->color_mode_id == mode->best.id)
		cmode = &mode->best;
	if (cmode)
		dev_info(dcp->dev,
			"set_digital_out_mode() color mode depth:%hhu format:%u "
			"colorimetry:%u eotf:%u range:%u vrr:%u\n", cmode->depth,
			cmode->format, cmode->colorimetry, cmode->eotf,
			cmode->range, mode->vrr);

	dcp->mode = (struct dcp_set_digital_out_mode_req){
		.color_mode_id = mode->color_mode_id,
		.timing_mode_id = mode->timing_mode_id
	};

	/* Keep track of suspected vrr modes */
	dcp->use_timestamps = mode->vrr;

	cookie = kzalloc(sizeof(*cookie), GFP_KERNEL);
	if (!cookie) {
		return -ENOMEM;
	}

	init_completion(&cookie->done);
	kref_init(&cookie->refcount);
	/* increase refcount to ensure the receiver has a reference */
	kref_get(&cookie->refcount);

	dcp->during_modeset = true;

	dcp_set_digital_out_mode(dcp, false, &dcp->mode,
				 complete_set_digital_out_mode, cookie);

	/*
	 * The DCP firmware has an internal timeout of ~8 seconds for
	 * modesets. Add an extra 500ms to safe side that the modeset
	 * call has returned.
	 */
	if (iomfb_poll_during_modeset_wait) {
		long left = msecs_to_jiffies(8500);

		ret = 0;
		while (left > 0) {
			long slice = min_t(long, left, msecs_to_jiffies(25));
			long wait_ret;
			int polls;

			wait_ret = wait_for_completion_timeout(&cookie->done,
							       slice);
			if (wait_ret > 0) {
				ret = wait_ret;
				break;
			}

			polls = apple_rtkit_poll(dcp->rtk);
			if (polls)
				dev_info(dcp->dev,
					 "set_digital_out_mode: polled %d RTKit message(s) while waiting\n",
					 polls);
			if (completion_done(&cookie->done)) {
				ret = 1;
				break;
			}

			left -= slice;
		}
	} else {
		ret = wait_for_completion_timeout(&cookie->done,
						  msecs_to_jiffies(8500));
	}

	kref_put(&cookie->refcount, release_wait_cookie);
	dcp->during_modeset = false;
	dev_info(dcp->dev, "set_digital_out_mode finished:%d\n", ret);

	if (ret == 0) {
		dev_info(dcp->dev, "set_digital_out_mode timed out\n");
		if (iomfb_modeset_timeout_mark_valid) {
			dev_warn(dcp->dev,
				 "diagnostic: marking mode valid after set_digital_out_mode timeout\n");
			dcp->valid_mode = true;
			return 0;
		}
		return -EIO;
	} else if (ret < 0) {
		dev_info(dcp->dev,
			 "waiting on set_digital_out_mode failed:%d\n", ret);
		return -EIO;

	} else if (ret > 0) {
		dev_dbg(dcp->dev,
			"set_digital_out_mode finished with %d to spare\n",
			jiffies_to_msecs(ret));
	}
	dcp->valid_mode = true;

	return 0;
}

void DCP_FW_NAME(iomfb_flush)(struct apple_dcp *dcp, struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct drm_plane *plane;
	struct drm_plane_state *new_state, *old_state;
	struct drm_crtc_state *crtc_state;
	struct DCP_FW_NAME(dcp_swap_submit_req) *req = &DCP_FW_UNION(dcp->swap);
	int plane_idx, l;
	int has_surface = 0;

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);

	if (crtc_state)
		dev_info(dcp->dev,
			 "iomfb_flush entry active=%d mode_changed=%d planes_changed=%d "
			 "color_mgmt_changed=%d dcp_valid=%d surfaces_cleared=%d "
			 DRM_MODE_FMT "\n",
			 crtc_state->active, crtc_state->mode_changed,
			 crtc_state->planes_changed,
			 crtc_state->color_mgmt_changed,
			 dcp->valid_mode, dcp->surfaces_cleared,
			 DRM_MODE_ARG(&crtc_state->mode));
	else
		dev_info(dcp->dev,
			 "iomfb_flush entry without new crtc_state dcp_valid=%d "
			 "surfaces_cleared=%d " DRM_MODE_FMT "\n",
			 dcp->valid_mode, dcp->surfaces_cleared,
			 DRM_MODE_ARG(&crtc->state->mode));

	/* Reset all surfaces to defaults */
	memset(req, 0, sizeof(*req));
	for (l = 0; l < SWAP_SURFACES; l++)
		req->surf_null[l] = true;
#if DCP_FW_VER >= DCP_FW_VERSION(13, 2, 0)
	for (l = 0; l < 5; l++)
		req->surf2_null[l] = true;
	req->unkU32Ptr_null = true;
	req->unkU32out_null = true;
#endif

	/*
	 * Clear all surfaces on startup. The boot framebuffer in surface 0
	 * sticks around.
	 */
	if (!dcp->surfaces_cleared) {
		req->swap.swap_enabled = IOMFB_SET_BACKGROUND | 0x7;
		req->swap.bg_color = 0xFF000000;
		dcp->surfaces_cleared = true;
	}

	for_each_oldnew_plane_in_state(state, plane, old_state, new_state, plane_idx) {
		struct apple_plane_state *apple_state = to_apple_plane_state(new_state);

		/* skip planes not for this crtc */
		if (old_state->crtc != crtc && new_state->crtc != crtc)
			continue;

		/*
		 * Plane order is nondeterministic for this iterator. DCP will
		 * almost always crash at some point if the z order of planes
		 * flip-flops around. Make sure we are always blending them
		 * in the correct order.
		 *
		 * Despite having 4 surfaces, we can only blend two. Surface 0 is
		 * also unusable on some machines, so ignore it.
		 */

		l = MAX_BLEND_SURFACES - new_state->normalized_zpos;
		if (iomfb_force_swap_layer >= 0) {
			if (iomfb_force_swap_layer < SWAP_SURFACES) {
				dev_info(dcp->dev,
					 "forcing swap surface slot from %d to %d for probe\n",
					 l, iomfb_force_swap_layer);
				l = iomfb_force_swap_layer;
			} else {
				dev_warn(dcp->dev,
					 "ignoring invalid forced swap surface slot %d\n",
					 iomfb_force_swap_layer);
			}
		}

		WARN_ON(l > MAX_BLEND_SURFACES);

		req->swap.swap_enabled |= BIT(l);

		if (old_state->fb && new_state->fb != old_state->fb) {
			/*
			 * Race condition between a framebuffer unbind getting
			 * swapped out and GEM unreferencing a framebuffer. If
			 * we lose the race, the display gets IOVA faults and
			 * the DCP crashes. We need to extend the lifetime of
			 * the drm_framebuffer (and hence the GEM object) until
			 * after we get a swap complete for the swap unbinding
			 * it.
			 */
			struct dcp_fb_reference *entry =
				kzalloc(sizeof(*entry), GFP_KERNEL);
			if (entry) {
				entry->fb = old_state->fb;
				entry->swap_id = dcp->last_swap_id;
				list_add_tail(&entry->head,
					      &dcp->swapped_out_fbs);
			}
			drm_framebuffer_get(old_state->fb);
		}

		if (!new_state->fb || !new_state->visible) {
			continue;
		}
		req->surf_null[l] = false;
		has_surface = 1;

		dev_info(dcp->dev,
			 "iomfb_flush plane=%u layer=%d fb=%u visible=%d "
			 "src=%ux%u dst=%ux%u iova=%pad\n",
			 plane->base.id, l, new_state->fb->base.id,
			 new_state->visible,
			 drm_rect_width(&new_state->src) >> 16,
			 drm_rect_height(&new_state->src) >> 16,
			 drm_rect_width(&new_state->dst),
			 drm_rect_height(&new_state->dst),
			 &apple_state->iova);

		req->swap.src_rect[l] = apple_state->src_rect;
		req->swap.dst_rect[l] = apple_state->dst_rect;

		if (dcp->notch_height > 0)
			req->swap.dst_rect[l].y += dcp->notch_height;

#if DCP_FW_VER >= DCP_FW_VERSION(13, 2, 0)
			if (iomfb_set_active_regions) {
				req->swap.active_region_enable[l] = 1;
				req->swap.active_region[l].max_x =
					req->swap.dst_rect[l].x + req->swap.dst_rect[l].w;
				req->swap.active_region[l].max_y =
					req->swap.dst_rect[l].y + req->swap.dst_rect[l].h;
				req->swap.active_region[l].min_w = 0;
				req->swap.active_region[l].max_w =
					req->swap.dst_rect[l].w;
				dev_info(dcp->dev,
					 "active_region layer=%d enable=1 max=%ux%u min_w=%u max_w=%u\n",
					 l, req->swap.active_region[l].max_x,
					 req->swap.active_region[l].max_y,
					 req->swap.active_region[l].min_w,
					 req->swap.active_region[l].max_w);
			}
#endif

		req->surf_iova[l] = apple_state->iova;
		if (iomfb_surface_iova_or) {
			u64 orig = req->surf_iova[l];

			req->surf_iova[l] |= iomfb_surface_iova_or;
			dev_info(dcp->dev,
				 "forcing surface IOVA OR layer=%d orig=0x%llx mask=0x%llx final=0x%llx\n",
				 l, orig, iomfb_surface_iova_or,
				 req->surf_iova[l]);
		}
		req->surf[l].base = apple_state->surf;
		if (iomfb_force_surface_id) {
			req->swap.surf_ids[l] = iomfb_force_surface_id;
			req->surf[l].base.surface_id = iomfb_force_surface_id;
		}
		if (iomfb_force_surface_id || iomfb_force_surface_flags)
			req->swap.surf_flags[l] = iomfb_force_surface_flags;
		if (iomfb_force_surface_pix_size)
			req->surf[l].base.pix_size = iomfb_force_surface_pix_size;
		if (iomfb_force_surface_colorspace >= 0)
			req->surf[l].base.colorspace = iomfb_force_surface_colorspace;
		if (iomfb_force_surface_plane_count >= 0) {
			u32 plane_count = min_t(u32, iomfb_force_surface_plane_count,
						DCP_SURF_MAX_PLANES);

			req->surf[l].base.plane_cnt = plane_count;
			req->surf[l].base.plane_cnt2 = plane_count;
			if (!plane_count) {
				memset(req->surf[l].base.comp_types, 0,
				       sizeof(req->surf[l].base.comp_types));
				memset(req->surf[l].base.planes, 0,
				       sizeof(req->surf[l].base.planes));
				memset(req->surf[l].base.compression_info, 0,
				       sizeof(req->surf[l].base.compression_info));
			}
		}

		dev_info(dcp->dev,
			 "iomfb_flush surface payload layer=%d "
			 "surf_id=%u surf_flags=0x%x "
			 "surface_id=%u flags1=0x%llx flags2=0x%llx "
			 "format=0x%x stride=%u pix_size=%u colorspace=%u "
			 "plane_cnt=%u plane_cnt2=%u has_comp=0x%llx "
			 "has_planes=0x%llx buf_size=%u "
			 "surf_null=%u iova=%pad\n",
			 l, req->swap.surf_ids[l], req->swap.surf_flags[l],
			 req->surf[l].base.surface_id, req->swap.flags1,
			 req->swap.flags2, req->surf[l].base.format,
			 req->surf[l].base.stride, req->surf[l].base.pix_size,
			 req->surf[l].base.colorspace,
			 req->surf[l].base.plane_cnt,
			 req->surf[l].base.plane_cnt2,
			 req->surf[l].base.has_comp,
			 req->surf[l].base.has_planes,
			 req->surf[l].base.buf_size, req->surf_null[l],
			 &req->surf_iova[l]);

		/* Use sRGB colorspace only for internal panels. External
		 * displays are expected to have EDID and user space can use
		 * the contained colorimetry information to provide native
		 * colors.
		 */
		if (dcp->connector_type == DRM_MODE_CONNECTOR_eDP &&
		    req->surf[l].base.colorspace == DCP_COLORSPACE_BG_SRGB)
			req->surf[l].base.colorspace = DCP_COLORSPACE_NATIVE;
	}

	if (!has_surface && !crtc_state->color_mgmt_changed) {
		if (crtc_state->enable && crtc_state->active &&
		    !crtc_state->planes_changed) {
			dcp_schedule_vblank(dcp, "flush-no-surface");
			return;
		}

		/* Set black background */
		req->swap.swap_enabled |= IOMFB_SET_BACKGROUND;
		req->swap.bg_color = 0xFF000000;
		req->clear = 1;
	}

	if (has_surface && dcp->use_timestamps) {
		/*
		 * Fake timstamps to get 120hz refresh rate. It looks
		 * like the actual value does not matter, as long  as it is non zero.
		 */
		req->swap.ts1 = 120;
		req->swap.ts2 = 120;
		req->swap.ts3 = 120;
	}

	/* These fields should be set together */
	if (iomfb_force_swap_flags1)
		req->swap.flags1 = iomfb_force_swap_flags1;
	if (iomfb_force_swap_flags2)
		req->swap.flags2 = iomfb_force_swap_flags2;
	if (has_surface && iomfb_force_swap_background) {
		dev_info(dcp->dev,
			 "forcing swap background bits for probe\n");
		req->swap.swap_enabled |= IOMFB_SET_BACKGROUND | 0x7;
		req->swap.bg_color = 0xFF000000;
	}
	req->swap.swap_completed = req->swap.swap_enabled;
	if (iomfb_swap_state_mirror_offset) {
		u32 off = iomfb_swap_state_mirror_offset;
		u32 native = offsetof(struct DCP_FW_NAME(dcp_swap),
				      swap_enabled);

		if (off + sizeof(req->swap.swap_enabled) +
			  sizeof(req->swap.swap_completed) <=
		    sizeof(req->swap)) {
			u8 *swap_bytes = (u8 *)&req->swap;

			put_unaligned_le32(req->swap.swap_enabled,
					   swap_bytes + off);
			put_unaligned_le32(req->swap.swap_completed,
					   swap_bytes + off +
					   sizeof(req->swap.swap_enabled));
			dev_info(dcp->dev,
				 "mirrored swap state native=0x%x mirror=0x%x enabled=0x%x completed=0x%x\n",
				 native, off, req->swap.swap_enabled,
				 req->swap.swap_completed);
		} else {
			dev_warn(dcp->dev,
				 "invalid swap state mirror offset 0x%x for swap size 0x%zx\n",
				 off, sizeof(req->swap));
		}
	}
#if DCP_FW_VER >= DCP_FW_VERSION(13, 2, 0)
	if (iomfb_swap_submit_unk_u32ptr_present) {
		req->unkU32Ptr = iomfb_swap_submit_unk_u32ptr_value;
		req->unkU32Ptr_null = false;
		dev_info(dcp->dev,
			 "forcing swap_submit unkU32Ptr present value=0x%x\n",
			 req->unkU32Ptr);
	}
	if (iomfb_swap_submit_unk_u32out_present) {
		req->unkU32out_null = false;
		dev_info(dcp->dev,
			 "forcing swap_submit unkU32out present\n");
	}
#endif

	/* update brightness if changed */
	if (iomfb_force_swap_brightness) {
		req->swap.bl_unk = iomfb_force_swap_bl_unk;
		req->swap.bl_value = iomfb_force_swap_bl_value;
		req->swap.bl_power = (u8)iomfb_force_swap_bl_power;
		dev_info(dcp->dev,
			 "forcing swap brightness bl_unk=0x%llx bl_value=0x%x bl_power=0x%x\n",
			 req->swap.bl_unk, req->swap.bl_value,
			 req->swap.bl_power);
	} else if (dcp_has_panel(dcp) && dcp->brightness.update) {
		req->swap.bl_unk = 1;
		req->swap.bl_value = dcp->brightness.dac;
		req->swap.bl_power = 0x40;
		dcp->brightness.update = false;
	}

	dev_info(dcp->dev,
		"iomfb_flush swap payload final flags1=0x%llx flags2=0x%llx "
		"swap_enabled=0x%x swap_completed=0x%x clear=%u "
		"surf_ids=%u,%u,%u,%u surf_flags=0x%x,0x%x,0x%x,0x%x "
		"surf_null=%u,%u,%u,%u bl_unk=0x%llx bl_value=0x%x bl_power=0x%x\n",
		req->swap.flags1, req->swap.flags2, req->swap.swap_enabled,
		req->swap.swap_completed, req->clear, req->swap.surf_ids[0],
		req->swap.surf_ids[1], req->swap.surf_ids[2],
		req->swap.surf_ids[3], req->swap.surf_flags[0],
		req->swap.surf_flags[1], req->swap.surf_flags[2],
		req->swap.surf_flags[3], req->surf_null[0],
		req->surf_null[1], req->surf_null[2], req->surf_null[3],
		req->swap.bl_unk, req->swap.bl_value, req->swap.bl_power);

	if (crtc_state->color_mgmt_changed && iomfb_skip_set_matrix) {
		dev_info(dcp->dev,
			 "skipping set_matrix before swap_start for probe\n");
		do_swap(dcp, NULL, NULL);
	} else if (crtc_state->color_mgmt_changed &&
		   iomfb_set_matrix_after_swap_start) {
		struct swap_matrix_cookie *swap_cookie;

		swap_cookie = kzalloc(sizeof(*swap_cookie), GFP_KERNEL);
		if (!swap_cookie) {
			dcp_schedule_vblank(dcp, "swap-matrix-cookie-alloc-failed");
			return;
		}

		swap_cookie->set_matrix_before_submit = true;
		swap_cookie->mat.location = 9;

		if (crtc_state->ctm) {
			struct drm_color_ctm *ctm = (struct drm_color_ctm *)crtc_state->ctm->data;
			memcpy(swap_cookie->mat.matrix, ctm->matrix,
			       sizeof(swap_cookie->mat.matrix));
		} else {
			swap_cookie->mat.matrix[0] = 1LLU << 32;
			swap_cookie->mat.matrix[4] = 1LLU << 32;
			swap_cookie->mat.matrix[8] = 1LLU << 32;
		}

		dev_info(dcp->dev,
			 "deferring set_matrix until after swap_start for probe\n");
		do_swap(dcp, NULL, swap_cookie);
	} else if (crtc_state->color_mgmt_changed) {
		struct iomfb_set_matrix_req mat = {
			.location = 9,
		};

		if (crtc_state->ctm) {
			struct drm_color_ctm *ctm = (struct drm_color_ctm *)crtc_state->ctm->data;
			memcpy(mat.matrix, ctm->matrix, sizeof(mat.matrix));
		} else {
			mat.matrix[0] = mat.matrix[4] = mat.matrix[8] = 1LLU << 32;
		}

		iomfb_set_matrix(dcp, false, &mat, do_swap, NULL);
		if (iomfb_poll_after_set_matrix_ms)
			poll_after_iomfb_call(dcp, "set_matrix",
					      iomfb_poll_after_set_matrix_ms);
	} else
		do_swap(dcp, NULL, NULL);
}

static void res_is_main_display(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct apple_connector *connector;
	int result = *(int *)out;
	dev_info(dcp->dev, "DCP is_main_display: %d\n", result);

	dcp->main_display = result != 0;

	connector = dcp->connector;
	if (connector) {
		connector->connected = dcp->nr_modes > 0;
		schedule_work(&connector->hotplug_wq);
	}

	dcp->active = true;
	complete(&dcp->start_done);
}

static void init_3(struct apple_dcp *dcp, void *out, void *cookie)
{
	if (iomfb_skip_is_main_display) {
		u32 result = 1;

		dev_info(dcp->dev,
			 "skipping dcpep_is_main_display for probe; assuming main display\n");
		res_is_main_display(dcp, &result, NULL);
		return;
	}

	dcp_is_main_display(dcp, false, res_is_main_display, NULL);
}

static u32 dcp_boot_fb_format(struct apple_dcp *dcp)
{
	if (!strcmp(dcp->boot_fb.format, "x2r10g10b10") ||
	    !strcmp(dcp->boot_fb.format, "a2r10g10b10"))
		return DCP_FORMAT_L10R;

	if (!strcmp(dcp->boot_fb.format, "x8r8g8b8") ||
	    !strcmp(dcp->boot_fb.format, "a8r8g8b8"))
		return DCP_FORMAT_BGRA;

	dev_warn(dcp->dev,
		 "unknown boot framebuffer format %s for update_dfb; using BGRA\n",
		 dcp->boot_fb.format);
	return DCP_FORMAT_BGRA;
}

static void dcp_build_boot_dfb_surface(struct apple_dcp *dcp,
				       struct DCP_FW_NAME(dcp_surface) *surf)
{
	u32 extra_height = iomfb_update_dfb_include_hidden_height ?
		dcp->boot_fb.hidden_height : dcp->notch_height;
	u32 height = dcp->boot_fb.visible_height + extra_height;

	memset(surf, 0, sizeof(*surf));

	surf->base.format = dcp_boot_fb_format(dcp);
	surf->base.xfer_func = DCP_XFER_FUNC_SDR;
	surf->base.colorspace = iomfb_update_dfb_colorspace;
	surf->base.stride = dcp->boot_fb.stride;
	surf->base.pix_size = 4;
	surf->base.pel_w = 1;
	surf->base.pel_h = 1;
	surf->base.width = dcp->boot_fb.width;
	surf->base.height = height;
	surf->base.buf_size = height * dcp->boot_fb.stride;
	surf->base.surface_id = iomfb_update_dfb_surface_id;
	surf->base.has_comp = 1;
	surf->base.has_planes = 1;

	dev_info(dcp->dev,
		 "update_dfb boot surface id=%u format=0x%x %ux%u stride=%u buf_size=0x%x colorspace=%u valid_bootfb=%u visible_height=%u hidden_height=%u notch_height=%u include_hidden=%u\n",
		 surf->base.surface_id, surf->base.format, surf->base.width,
		 surf->base.height, surf->base.stride, surf->base.buf_size,
		 surf->base.colorspace, dcp->boot_fb.valid,
		 dcp->boot_fb.visible_height, dcp->boot_fb.hidden_height,
		 dcp->notch_height, iomfb_update_dfb_include_hidden_height);
}

static void init_2(struct apple_dcp *dcp, void *out, void *cookie);

static void init_after_update_dfb(struct apple_dcp *dcp, void *out, void *cookie)
{
	u32 ret = out ? *(u32 *)out : 0;

	dev_info(dcp->dev, "update_dfb boot surface returned:0x%x\n", ret);
	init_2(dcp, NULL, NULL);
}

static void init_maybe_update_dfb(struct apple_dcp *dcp, void *out, void *cookie)
{
	struct DCP_FW_NAME(dcp_surface) surf;

	if (!iomfb_update_dfb_before_first_client) {
		init_2(dcp, NULL, NULL);
		return;
	}

	if (!dcp->boot_fb.valid) {
		dev_warn(dcp->dev,
			 "skipping update_dfb probe because boot framebuffer info is missing\n");
		init_2(dcp, NULL, NULL);
		return;
	}

	dcp_build_boot_dfb_surface(dcp, &surf);
	dcp_update_dfb(dcp, false, &surf, init_after_update_dfb, NULL);
}

static void init_after_create_default_fb(struct apple_dcp *dcp, void *out,
					 void *cookie)
{
	u32 ret = out ? *(u32 *)out : 0;

	dev_info(dcp->dev, "startup create_default_fb returned:0x%x\n", ret);
	init_maybe_update_dfb(dcp, NULL, NULL);
}

static void init_maybe_create_default_fb(struct apple_dcp *dcp, void *out,
					 void *cookie)
{
	if (!iomfb_create_default_fb_before_first_client) {
		init_maybe_update_dfb(dcp, NULL, NULL);
		return;
	}

	dev_info(dcp->dev,
		 "calling startup do_create_default_frame_buffer before first_client_open\n");
	dcp_create_default_fb(dcp, false, init_after_create_default_fb, NULL);
}

static void init_after_is_keep_on_screen(struct apple_dcp *dcp, void *out,
					 void *cookie)
{
	u32 ret = out ? *(u32 *)out : 0;

	dev_info(dcp->dev, "isKeepOnScreen returned:0x%x\n", ret);
	init_3(dcp, NULL, NULL);
}

static void init_after_first_client_open(struct apple_dcp *dcp, void *out,
					 void *cookie)
{
	if (!iomfb_call_is_keep_on_screen) {
		init_3(dcp, NULL, NULL);
		return;
	}

	dcp_is_keep_on_screen(dcp, false, init_after_is_keep_on_screen, NULL);
}

static void init_2(struct apple_dcp *dcp, void *out, void *cookie)
{
	if (iomfb_skip_first_client_open) {
		dev_info(dcp->dev, "skipping dcpep_first_client_open for probe\n");
		init_3(dcp, NULL, NULL);
		return;
	}

	dcp_first_client_open(dcp, false, init_after_first_client_open, NULL);
}

static void init_update_notify_clients(struct apple_dcp *dcp, void *out,
				       void *cookie)
{
	struct dcp_update_notify_clients_req req = {
		.notify = { 0, 0, 0, 0, 0, 0, 1, 1, 1, 0, 1, 1, 1, 1 },
	};

	dev_info(dcp->dev,
		 "sending update_notify_clients_dcp for firmware-14 probe\n");
	dcp_update_notify_clients_dcp(dcp, false, &req, init_maybe_create_default_fb,
				      NULL);
}

static void init_1(struct apple_dcp *dcp, void *out, void *cookie)
{
	u32 val = 0;

	dcp_enable_disable_video_power_savings(dcp, false, &val,
					       iomfb_update_notify_clients ?
						       init_update_notify_clients :
						       init_maybe_create_default_fb,
					       NULL);
}

static void dcp_started_continue(struct apple_dcp *dcp, void *cookie)
{
	struct iomfb_get_color_remap_mode_req color_remap =
		(struct iomfb_get_color_remap_mode_req){
			.mode = 6,
		};

	iomfb_get_color_remap_mode(dcp, false, &color_remap, init_1, cookie);
}

static void top_level_dfb_create_done(struct apple_dcp *dcp, void *out, void *cookie)
{
	u32 ret = out ? *(u32 *)out : 0;

	dev_info(dcp->dev, "top-level create_default_fb returned:0x%x\n", ret);
	dcp_started_continue(dcp, cookie);
}

static void top_level_dfb_create(struct apple_dcp *dcp, void *out, void *cookie)
{
	dev_info(dcp->dev, "top-level set_create_dfb returned; creating default FB\n");
	dcp_create_default_fb(dcp, false, top_level_dfb_create_done, cookie);
}

static void dcp_started(struct apple_dcp *dcp, void *data, void *cookie)
{
	dev_info(dcp->dev, "DCP booted\n");

	if (iomfb_top_level_dfb_setup) {
		dev_info(dcp->dev,
			 "running top-level A373/A445 default framebuffer setup\n");
		dcp_set_create_dfb(dcp, false, top_level_dfb_create, cookie);
		return;
	}

	dcp_started_continue(dcp, cookie);
}

void DCP_FW_NAME(iomfb_shutdown)(struct apple_dcp *dcp)
{
	struct dcp_set_power_state_req req = {
		/* defaults are ok */
	};

	dcp_set_power_state(dcp, false, &req, NULL, NULL);
}
