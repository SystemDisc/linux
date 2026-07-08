// SPDX-License-Identifier: GPL-2.0-only OR MIT
/* Copyright 2021 Alyssa Rosenzweig */

#include <linux/align.h>
#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/iommu.h>
#include <linux/kref.h>
#include <linux/module.h>
#include <linux/of_device.h>
#include <linux/ratelimit.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/soc/apple/rtkit.h>

#include <drm/drm_edid.h>
#include <drm/drm_fb_dma_helper.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_framebuffer.h>
#include <drm/drm_gem_dma_helper.h>
#include <drm/drm_probe_helper.h>
#include <drm/drm_vblank.h>

#include "dcp.h"
#include "dcp-internal.h"
#include "iomfb.h"
#include "iomfb_internal.h"
#include "iomfb_plane.h"
#include "parser.h"
#include "trace.h"

static uint iomfb_shmem_flag = IOMFB_SHMEM_FLAG_VALUE;
module_param(iomfb_shmem_flag, uint, 0644);
MODULE_PARM_DESC(iomfb_shmem_flag,
		 "IOMFB SET_SHMEM flag nibble; default matches supported DCP firmware");

static u64 iomfb_shmem_dva_or;
module_param(iomfb_shmem_dva_or, ullong, 0644);
MODULE_PARM_DESC(iomfb_shmem_dva_or,
		 "Debug mask ORed into the IOMFB SET_SHMEM DVA before sending");

bool iomfb_trace_ipc;
module_param(iomfb_trace_ipc, bool, 0644);
MODULE_PARM_DESC(iomfb_trace_ipc,
		 "Trace IOMFB method and callback tags in dmesg");

static bool iomfb_fw14_method_map;
module_param(iomfb_fw14_method_map, bool, 0644);
MODULE_PARM_DESC(iomfb_fw14_method_map,
		 "Use traced firmware-14 IOMFB method tags for T8122 probing");

static bool iomfb_fw14_m1n1_method_map;
module_param(iomfb_fw14_m1n1_method_map, bool, 0644);
MODULE_PARM_DESC(iomfb_fw14_m1n1_method_map,
		 "Use m1n1 V>=13.5 IOMFB method tags for T8122 probing");

static bool iomfb_fw14_legacy_method_map;
module_param(iomfb_fw14_legacy_method_map, bool, 0644);
MODULE_PARM_DESC(iomfb_fw14_legacy_method_map,
		 "Use legacy/V<13.5 IOMFB method tags for T8122 probing");

static char iomfb_first_client_open_tag[5];
module_param_string(iomfb_first_client_open_tag, iomfb_first_client_open_tag,
		    sizeof(iomfb_first_client_open_tag), 0644);
MODULE_PARM_DESC(iomfb_first_client_open_tag,
		 "Override dcpep_first_client_open tag, e.g. A456, for firmware probing");

static char iomfb_set_power_state_tag[5];
module_param_string(iomfb_set_power_state_tag, iomfb_set_power_state_tag,
		    sizeof(iomfb_set_power_state_tag), 0644);
MODULE_PARM_DESC(iomfb_set_power_state_tag,
		 "Override dcpep_set_power_state tag, e.g. A468, for firmware probing");

static char iomfb_set_parameter_dcp_tag[5];
module_param_string(iomfb_set_parameter_dcp_tag, iomfb_set_parameter_dcp_tag,
		    sizeof(iomfb_set_parameter_dcp_tag), 0644);
MODULE_PARM_DESC(iomfb_set_parameter_dcp_tag,
		 "Override dcpep_set_parameter_dcp tag, e.g. A439, for firmware probing");

bool iomfb_d121_force_true;
module_param(iomfb_d121_force_true, bool, 0644);
MODULE_PARM_DESC(iomfb_d121_force_true,
		 "Return true from D121 for probing firmware-14 callback tables");

bool iomfb_d121_run_boot_sequence;
module_param(iomfb_d121_run_boot_sequence, bool, 0644);
MODULE_PARM_DESC(iomfb_d121_run_boot_sequence,
		 "Handle D121 as start_hardware_boot by running the DCP boot sequence");

bool iomfb_top_level_dfb_setup;
module_param(iomfb_top_level_dfb_setup, bool, 0644);
MODULE_PARM_DESC(iomfb_top_level_dfb_setup,
		 "Run A373/A445 default framebuffer setup after A401 from command context");

bool iomfb_d123_force_true = true;
module_param(iomfb_d123_force_true, bool, 0644);
MODULE_PARM_DESC(iomfb_d123_force_true,
		 "Return true from D123 for probing firmware-14 callback tables");

bool iomfb_skip_first_client_open;
module_param(iomfb_skip_first_client_open, bool, 0644);
MODULE_PARM_DESC(iomfb_skip_first_client_open,
		 "Skip first_client_open during DCP startup for firmware-14 probing");

bool iomfb_skip_is_main_display;
module_param(iomfb_skip_is_main_display, bool, 0644);
MODULE_PARM_DESC(iomfb_skip_is_main_display,
		 "Skip is_main_display during DCP startup for firmware-14 probing");

bool iomfb_poll_during_modeset_wait;
module_param(iomfb_poll_during_modeset_wait, bool, 0644);
MODULE_PARM_DESC(iomfb_poll_during_modeset_wait,
		 "Poll RTKit while waiting for IOMFB set_digital_out_mode");

bool iomfb_poll_during_power_wait;
module_param(iomfb_poll_during_power_wait, bool, 0644);
MODULE_PARM_DESC(iomfb_poll_during_power_wait,
		 "Poll RTKit while waiting for IOMFB display power state");

bool iomfb_set_parameter_on_main;
module_param(iomfb_set_parameter_on_main, bool, 0644);
MODULE_PARM_DESC(iomfb_set_parameter_on_main,
		 "Call set_parameter_dcp on the main display power-on path");

uint iomfb_set_parameter_count_override;
module_param(iomfb_set_parameter_count_override, uint, 0644);
MODULE_PARM_DESC(iomfb_set_parameter_count_override,
		 "Override set_parameter_dcp count for firmware probing");

bool iomfb_compact_set_parameter;
module_param(iomfb_compact_set_parameter, bool, 0644);
MODULE_PARM_DESC(iomfb_compact_set_parameter,
		 "Use compact V>=13.5 set_parameter_dcp ABI for firmware probing");

bool iomfb_power_before_display_device;
module_param(iomfb_power_before_display_device, bool, 0644);
MODULE_PARM_DESC(iomfb_power_before_display_device,
		 "Request display power before set_display_device during DCP power-on probing");

bool iomfb_update_notify_clients;
module_param(iomfb_update_notify_clients, bool, 0644);
MODULE_PARM_DESC(iomfb_update_notify_clients,
		 "Send update_notify_clients_dcp during DCP startup probing");

bool iomfb_create_default_fb_before_first_client;
module_param(iomfb_create_default_fb_before_first_client, bool, 0644);
MODULE_PARM_DESC(iomfb_create_default_fb_before_first_client,
		 "Send do_create_default_frame_buffer before first_client_open during firmware probing");

bool iomfb_update_dfb_before_first_client;
module_param(iomfb_update_dfb_before_first_client, bool, 0644);
MODULE_PARM_DESC(iomfb_update_dfb_before_first_client,
		 "Send update_dfb with the boot framebuffer surface before first_client_open");

uint iomfb_update_dfb_surface_id = 3;
module_param(iomfb_update_dfb_surface_id, uint, 0644);
MODULE_PARM_DESC(iomfb_update_dfb_surface_id,
		 "Surface ID used for the boot framebuffer update_dfb probe");

uint iomfb_update_dfb_colorspace = DCP_COLORSPACE_NATIVE;
module_param(iomfb_update_dfb_colorspace, uint, 0644);
MODULE_PARM_DESC(iomfb_update_dfb_colorspace,
		 "DCP colorspace used for the boot framebuffer update_dfb probe");

bool iomfb_call_is_keep_on_screen;
module_param(iomfb_call_is_keep_on_screen, bool, 0644);
MODULE_PARM_DESC(iomfb_call_is_keep_on_screen,
		 "Call isKeepOnScreen after first_client_open during firmware probing");

bool iomfb_frame_sync_copy_input;
module_param(iomfb_frame_sync_copy_input, bool, 0644);
MODULE_PARM_DESC(iomfb_frame_sync_copy_input,
		 "Copy D006 frame-sync inout input back to output for firmware probing");

bool iomfb_dfb_allocated;
module_param(iomfb_dfb_allocated, bool, 0644);
MODULE_PARM_DESC(iomfb_dfb_allocated,
		 "Return true from IOMobileFramebufferAP::isDFBAllocated for firmware probing");

uint iomfb_create_default_fb_surface_ret = 1;
module_param(iomfb_create_default_fb_surface_ret, uint, 0644);
MODULE_PARM_DESC(iomfb_create_default_fb_surface_ret,
		 "Return value for create_default_fb_surface callback during firmware probing");

bool iomfb_vid_clock_factor_override_enable;
module_param(iomfb_vid_clock_factor_override_enable, bool, 0644);
MODULE_PARM_DESC(iomfb_vid_clock_factor_override_enable,
		 "Override vid-clock-to-disp-clock-factor EDT value for firmware probing");

uint iomfb_vid_clock_factor_override;
module_param(iomfb_vid_clock_factor_override, uint, 0644);
MODULE_PARM_DESC(iomfb_vid_clock_factor_override,
		 "Replacement vid-clock-to-disp-clock-factor EDT value when override is enabled");

bool iomfb_vid_clock_factor_success;
module_param(iomfb_vid_clock_factor_success, bool, 0644);
MODULE_PARM_DESC(iomfb_vid_clock_factor_success,
		 "Return success for vid-clock-to-disp-clock-factor EDT reads");

bool iomfb_skip_invalid_modeset;
module_param(iomfb_skip_invalid_modeset, bool, 0644);
MODULE_PARM_DESC(iomfb_skip_invalid_modeset,
		 "Skip set_digital_out_mode while DCP hotplug has not reported a valid mode");

bool iomfb_skip_modeset_mark_valid;
module_param(iomfb_skip_modeset_mark_valid, bool, 0644);
MODULE_PARM_DESC(iomfb_skip_modeset_mark_valid,
		 "Diagnostic: skip set_digital_out_mode and mark the current DCP mode valid");

bool iomfb_skip_flush_invalid_mode;
module_param(iomfb_skip_flush_invalid_mode, bool, 0644);
MODULE_PARM_DESC(iomfb_skip_flush_invalid_mode,
		 "Skip IOMFB flush/swap while DCP hotplug has not reported a valid mode");

bool iomfb_skip_set_matrix;
module_param(iomfb_skip_set_matrix, bool, 0644);
MODULE_PARM_DESC(iomfb_skip_set_matrix,
		 "Diagnostic: skip set_matrix before IOMFB swap_start");

bool iomfb_swap_start_client_flag2;
module_param(iomfb_swap_start_client_flag2, bool, 0644);
MODULE_PARM_DESC(iomfb_swap_start_client_flag2,
		 "Diagnostic: set IOUserClient flag2 in IOMFB swap_start requests");

uint iomfb_swap_start_client_unk;
module_param(iomfb_swap_start_client_unk, uint, 0644);
MODULE_PARM_DESC(iomfb_swap_start_client_unk,
		 "Diagnostic: set IOUserClient unk field in IOMFB swap_start requests");

ullong iomfb_swap_start_client_handle;
module_param(iomfb_swap_start_client_handle, ullong, 0644);
MODULE_PARM_DESC(iomfb_swap_start_client_handle,
		 "Diagnostic: set IOUserClient handle in IOMFB swap_start requests");

uint iomfb_poll_after_swap_start_ms;
module_param(iomfb_poll_after_swap_start_ms, uint, 0644);
MODULE_PARM_DESC(iomfb_poll_after_swap_start_ms,
		 "Diagnostic: poll RTKit after IOMFB swap_start for this many milliseconds");

uint iomfb_poll_after_swap_submit_ms;
module_param(iomfb_poll_after_swap_submit_ms, uint, 0644);
MODULE_PARM_DESC(iomfb_poll_after_swap_submit_ms,
		 "Diagnostic: poll RTKit after IOMFB swap_submit for this many milliseconds");

bool iomfb_fake_pageflip_on_swap_submit_ack;
module_param(iomfb_fake_pageflip_on_swap_submit_ack, bool, 0644);
MODULE_PARM_DESC(iomfb_fake_pageflip_on_swap_submit_ack,
		 "Diagnostic: synthesize a DRM pageflip event after a successful IOMFB swap_submit ACK");

uint iomfb_poll_after_set_matrix_ms;
module_param(iomfb_poll_after_set_matrix_ms, uint, 0644);
MODULE_PARM_DESC(iomfb_poll_after_set_matrix_ms,
		 "Diagnostic: poll RTKit after IOMFB set_matrix for this many milliseconds");

bool iomfb_set_matrix_after_swap_start;
module_param(iomfb_set_matrix_after_swap_start, bool, 0644);
MODULE_PARM_DESC(iomfb_set_matrix_after_swap_start,
		 "Diagnostic: issue set_matrix after swap_start ACK and before swap_submit");

bool iomfb_m1n1_post_swap_init;
module_param(iomfb_m1n1_post_swap_init, bool, 0644);
MODULE_PARM_DESC(iomfb_m1n1_post_swap_init,
		 "Diagnostic: issue m1n1-style brightness/set_parameter calls after swap_start");

bool iomfb_m1n1_pre_swap_init;
module_param(iomfb_m1n1_pre_swap_init, bool, 0644);
MODULE_PARM_DESC(iomfb_m1n1_pre_swap_init,
		 "Diagnostic: issue m1n1-style gamma/contrast/brightness calls before swap_start");

bool iomfb_m1n1_pre_swap_sync;
module_param(iomfb_m1n1_pre_swap_sync, bool, 0644);
MODULE_PARM_DESC(iomfb_m1n1_pre_swap_sync,
		 "Diagnostic: run m1n1-style pre-swap calls synchronously with bounded RTKit polling");

bool iomfb_m1n1_pre_swap_skip_gamma;
module_param(iomfb_m1n1_pre_swap_skip_gamma, bool, 0644);
MODULE_PARM_DESC(iomfb_m1n1_pre_swap_skip_gamma,
		 "Diagnostic: skip A419 get_gamma_table in the m1n1-style pre-swap sequence");

bool iomfb_m1n1_pre_swap_skip_contrast;
module_param(iomfb_m1n1_pre_swap_skip_contrast, bool, 0644);
MODULE_PARM_DESC(iomfb_m1n1_pre_swap_skip_contrast,
		 "Diagnostic: skip A423 set_contrast in the m1n1-style pre-swap sequence");

uint iomfb_poll_after_pre_swap_ms;
module_param(iomfb_poll_after_pre_swap_ms, uint, 0644);
MODULE_PARM_DESC(iomfb_poll_after_pre_swap_ms,
		 "Diagnostic: poll RTKit after each m1n1-style pre-swap call");

bool iomfb_force_swap_brightness;
module_param(iomfb_force_swap_brightness, bool, 0644);
MODULE_PARM_DESC(iomfb_force_swap_brightness,
		 "Diagnostic: force brightness fields in IOMFB swap records");

unsigned long long iomfb_force_swap_bl_unk = 1;
module_param(iomfb_force_swap_bl_unk, ullong, 0644);
MODULE_PARM_DESC(iomfb_force_swap_bl_unk,
		 "Diagnostic: forced IOMFB swap brightness bl_unk value");

uint iomfb_force_swap_bl_value = 0x58f058d0;
module_param(iomfb_force_swap_bl_value, uint, 0644);
MODULE_PARM_DESC(iomfb_force_swap_bl_value,
		 "Diagnostic: forced IOMFB swap brightness DAC value");

uint iomfb_force_swap_bl_power = 0x40;
module_param(iomfb_force_swap_bl_power, uint, 0644);
MODULE_PARM_DESC(iomfb_force_swap_bl_power,
		 "Diagnostic: forced IOMFB swap brightness power value");

bool iomfb_force_swap_background;
module_param(iomfb_force_swap_background, bool, 0644);
MODULE_PARM_DESC(iomfb_force_swap_background,
		 "Diagnostic: force IOMFB_SET_BACKGROUND | 0x7 in visible swap records");

int iomfb_force_swap_layer = -1;
module_param(iomfb_force_swap_layer, int, 0644);
MODULE_PARM_DESC(iomfb_force_swap_layer,
		 "Diagnostic: force visible framebuffer into this IOMFB swap surface slot");

uint iomfb_force_surface_id;
module_param(iomfb_force_surface_id, uint, 0644);
MODULE_PARM_DESC(iomfb_force_surface_id,
		 "Diagnostic: force this IOMFB surface_id for visible framebuffer surfaces");

uint iomfb_force_surface_flags;
module_param(iomfb_force_surface_flags, uint, 0644);
MODULE_PARM_DESC(iomfb_force_surface_flags,
		 "Diagnostic: force this IOMFB surf_flags value for visible framebuffer surfaces");

ullong iomfb_force_swap_flags1;
module_param(iomfb_force_swap_flags1, ullong, 0644);
MODULE_PARM_DESC(iomfb_force_swap_flags1,
		 "Diagnostic: force this IOMFB swap flags1 value when nonzero");

ullong iomfb_force_swap_flags2;
module_param(iomfb_force_swap_flags2, ullong, 0644);
MODULE_PARM_DESC(iomfb_force_swap_flags2,
		 "Diagnostic: force this IOMFB swap flags2 value when nonzero");

uint iomfb_force_surface_pix_size;
module_param(iomfb_force_surface_pix_size, uint, 0644);
MODULE_PARM_DESC(iomfb_force_surface_pix_size,
		 "Diagnostic: force this IOMFB surface pix_size value when nonzero");

int iomfb_force_surface_colorspace = -1;
module_param(iomfb_force_surface_colorspace, int, 0644);
MODULE_PARM_DESC(iomfb_force_surface_colorspace,
		 "Diagnostic: force this IOMFB surface colorspace value when non-negative");

int iomfb_force_surface_plane_count = -1;
module_param(iomfb_force_surface_plane_count, int, 0644);
MODULE_PARM_DESC(iomfb_force_surface_plane_count,
		 "Diagnostic: force IOMFB surface plane_cnt/plane_cnt2 when non-negative");

bool iomfb_set_active_regions;
module_param(iomfb_set_active_regions, bool, 0644);
MODULE_PARM_DESC(iomfb_set_active_regions,
		 "Diagnostic: populate V13.x IOMFB active region fields from the visible plane");

uint iomfb_swap_state_mirror_offset;
module_param(iomfb_swap_state_mirror_offset, uint, 0644);
MODULE_PARM_DESC(iomfb_swap_state_mirror_offset,
		 "Diagnostic: mirror swap_enabled/swap_completed to this byte offset in the swap record");

bool iomfb_swap_submit_unk_u32ptr_present;
module_param(iomfb_swap_submit_unk_u32ptr_present, bool, 0644);
MODULE_PARM_DESC(iomfb_swap_submit_unk_u32ptr_present,
		 "Diagnostic: pass a non-null V13.x swap_submit unkU32Ptr argument");

uint iomfb_swap_submit_unk_u32ptr_value;
module_param(iomfb_swap_submit_unk_u32ptr_value, uint, 0644);
MODULE_PARM_DESC(iomfb_swap_submit_unk_u32ptr_value,
		 "Diagnostic: value for the V13.x swap_submit unkU32Ptr argument");

bool iomfb_swap_submit_unk_u32out_present;
module_param(iomfb_swap_submit_unk_u32out_present, bool, 0644);
MODULE_PARM_DESC(iomfb_swap_submit_unk_u32out_present,
		 "Diagnostic: pass a non-null V13.x swap_submit unkU32out argument");

unsigned long long iomfb_surface_iova_or;
module_param(iomfb_surface_iova_or, ullong, 0644);
MODULE_PARM_DESC(iomfb_surface_iova_or,
		 "Diagnostic: OR this mask into visible IOMFB surface IOVAs");

uint iomfb_default_stride_override;
module_param(iomfb_default_stride_override, uint, 0644);
MODULE_PARM_DESC(iomfb_default_stride_override,
		 "Return this stride from D101 get_display_default_stride instead of zero");

ulong iomfb_clock_frequency_override;
module_param(iomfb_clock_frequency_override, ulong, 0644);
MODULE_PARM_DESC(iomfb_clock_frequency_override,
		 "Return this D408 getClockFrequency value instead of the Linux clock rate");

static int dcp_tx_offset(enum dcp_context_id id)
{
	switch (id) {
	case DCP_CONTEXT_CB:
	case DCP_CONTEXT_CMD:
		return 0x00000;
	case DCP_CONTEXT_OOBCB:
	case DCP_CONTEXT_OOBCMD:
		return 0x08000;
	default:
		return -EINVAL;
	}
}

static int dcp_channel_offset(enum dcp_context_id id)
{
	switch (id) {
	case DCP_CONTEXT_ASYNC:
		return 0x40000;
	case DCP_CONTEXT_OOBASYNC:
		return 0x48000;
	case DCP_CONTEXT_CB:
		return 0x60000;
	case DCP_CONTEXT_OOBCB:
		return 0x68000;
	default:
		return dcp_tx_offset(id);
	}
}

static inline u64 dcpep_set_shmem(u64 dart_va)
{
	return FIELD_PREP(IOMFB_MESSAGE_TYPE, IOMFB_MESSAGE_TYPE_SET_SHMEM) |
	       FIELD_PREP(IOMFB_SHMEM_FLAG, iomfb_shmem_flag & 0xf) |
	       FIELD_PREP(IOMFB_SHMEM_DVA, dart_va);
}

static inline u64 dcpep_msg(enum dcp_context_id id, u32 length, u16 offset)
{
	return FIELD_PREP(IOMFB_MESSAGE_TYPE, IOMFB_MESSAGE_TYPE_MSG) |
		FIELD_PREP(IOMFB_MSG_CONTEXT, id) |
		FIELD_PREP(IOMFB_MSG_OFFSET, offset) |
		FIELD_PREP(IOMFB_MSG_LENGTH, length);
}

static inline u64 dcpep_ack(enum dcp_context_id id)
{
	return dcpep_msg(id, 0, 0) | IOMFB_MSG_ACK;
}

/*
 * A channel is busy if we have sent a message that has yet to be
 * acked. The driver must not sent a message to a busy channel.
 */
static bool dcp_channel_busy(struct dcp_channel *ch)
{
	return (ch->depth != 0);
}

/*
 * Get the context ID passed to the DCP for a command we push. The rule is
 * simple: callback contexts are used when replying to the DCP, command
 * contexts are used otherwise. That corresponds to a non/zero call stack
 * depth. This rule frees the caller from tracking the call context manually.
 */
static enum dcp_context_id dcp_call_context(struct apple_dcp *dcp, bool oob)
{
	u8 depth = oob ? dcp->ch_oobcmd.depth : dcp->ch_cmd.depth;

	if (depth)
		return oob ? DCP_CONTEXT_OOBCB : DCP_CONTEXT_CB;
	else
		return oob ? DCP_CONTEXT_OOBCMD : DCP_CONTEXT_CMD;
}

/* Get a channel for a context */
static struct dcp_channel *dcp_get_channel(struct apple_dcp *dcp,
					   enum dcp_context_id context)
{
	switch (context) {
	case DCP_CONTEXT_CB:
		return &dcp->ch_cb;
	case DCP_CONTEXT_CMD:
		return &dcp->ch_cmd;
	case DCP_CONTEXT_OOBCB:
		return &dcp->ch_oobcb;
	case DCP_CONTEXT_OOBCMD:
		return &dcp->ch_oobcmd;
	case DCP_CONTEXT_ASYNC:
		return &dcp->ch_async;
	case DCP_CONTEXT_OOBASYNC:
		return &dcp->ch_oobasync;
	default:
		return NULL;
	}
}

/* Get the start of a packet: after the end of the previous packet */
static u16 dcp_packet_start(struct dcp_channel *ch, u8 depth)
{
	if (depth > 0)
		return ch->end[depth - 1];
	else
		return 0;
}

/* Pushes and pops the depth of the call stack with safety checks */
static u8 dcp_push_depth(u8 *depth)
{
	u8 ret = (*depth)++;

	WARN_ON(ret >= DCP_MAX_CALL_DEPTH);
	return ret;
}

static u8 dcp_pop_depth(u8 *depth)
{
	WARN_ON((*depth) == 0);

	return --(*depth);
}

static bool iomfb_valid_method_tag(const char *tag, size_t len)
{
	return len == 4 && tag[0] == 'A' &&
	       tag[1] >= '0' && tag[1] <= '9' &&
	       tag[2] >= '0' && tag[2] <= '9' &&
	       tag[3] >= '0' && tag[3] <= '9';
}

static const char *iomfb_method_tag_override(const char *param_name,
					     const char *tag, size_t size)
{
	size_t len = strnlen(tag, size);

	if (!len)
		return NULL;

	if (iomfb_valid_method_tag(tag, len))
		return tag;

	pr_warn_once("appledrm: ignoring invalid %s=%s\n", param_name, tag);
	return NULL;
}

static const char *iomfb_fw14_method_tag(const struct dcp_method_entry *call)
{
	bool first_client = !strcmp(call->name, "dcpep_first_client_open");
	bool set_power = !strcmp(call->name, "dcpep_set_power_state");
	bool set_parameter = !strcmp(call->name, "dcpep_set_parameter_dcp");
	const char *override;

	if (first_client) {
		override = iomfb_method_tag_override("iomfb_first_client_open_tag",
						    iomfb_first_client_open_tag,
						    sizeof(iomfb_first_client_open_tag));
		if (override)
			return override;
	}

	if (set_power) {
		override = iomfb_method_tag_override("iomfb_set_power_state_tag",
						    iomfb_set_power_state_tag,
						    sizeof(iomfb_set_power_state_tag));
		if (override)
			return override;
	}

	if (set_parameter) {
		override = iomfb_method_tag_override("iomfb_set_parameter_dcp_tag",
						    iomfb_set_parameter_dcp_tag,
						    sizeof(iomfb_set_parameter_dcp_tag));
		if (override)
			return override;
	}

	if (iomfb_fw14_legacy_method_map) {
		if (!strcmp(call->name, "dcpep_set_create_dfb"))
			return "A357";
		if (!strcmp(call->name, "dcpep_set_parameter_dcp"))
			return "A439";
		if (!strcmp(call->name, "dcpep_create_default_fb"))
			return "A443";
		if (!strcmp(call->name, "dcpep_enable_disable_video_power_savings"))
			return "A447";
		if (first_client)
			return "A454";
		if (!strcmp(call->name, "iomfbep_last_client_close"))
			return "A455";
		if (!strcmp(call->name, "dcpep_set_display_refresh_properties"))
			return "A460";
		if (!strcmp(call->name, "dcpep_flush_supports_power"))
			return "A463";
		if (!strcmp(call->name, "iomfbep_abort_swaps_dcp"))
			return "A464";
		if (!strcmp(call->name, "dcpep_update_dfb"))
			return "A467";
		if (!strcmp(call->name, "dcpep_set_power_state"))
			return "A468";
		if (!strcmp(call->name, "dcpep_is_keep_on_screen"))
			return "A469";

		return NULL;
	}

	if (iomfb_fw14_m1n1_method_map) {
		if (!strcmp(call->name, "dcpep_set_create_dfb"))
			return "A373";
		if (!strcmp(call->name, "iomfbep_a358_vi_set_temperature_hint"))
			return "A374";
		if (!strcmp(call->name, "dcpep_set_parameter_dcp"))
			return "A441";
		if (!strcmp(call->name, "dcpep_create_default_fb"))
			return "A445";
		if (!strcmp(call->name, "dcpep_enable_disable_video_power_savings"))
			return "A449";
		if (first_client)
			return "A456";
		if (!strcmp(call->name, "iomfbep_last_client_close"))
			return "A457";
		if (!strcmp(call->name, "dcpep_set_display_refresh_properties"))
			return "A463";
		if (!strcmp(call->name, "dcpep_flush_supports_power"))
			return "A466";
		if (!strcmp(call->name, "iomfbep_abort_swaps_dcp"))
			return "A467";
		if (!strcmp(call->name, "dcpep_update_dfb"))
			return "A470";
		if (!strcmp(call->name, "dcpep_set_power_state"))
			return "A472";
		if (!strcmp(call->name, "dcpep_is_keep_on_screen"))
			return "A473";

		return NULL;
	}

	if (!iomfb_fw14_method_map)
		return NULL;

	if (!strcmp(call->name, "dcpep_set_parameter_dcp"))
		return "A438";
	if (!strcmp(call->name, "dcpep_create_default_fb"))
		return "A442";
	if (!strcmp(call->name, "dcpep_enable_disable_video_power_savings"))
		return "A446";
	if (first_client)
		return "A453";
	if (!strcmp(call->name, "iomfbep_last_client_close"))
		return "A454";
	if (!strcmp(call->name, "dcpep_set_display_refresh_properties"))
		return "A459";
	if (!strcmp(call->name, "dcpep_flush_supports_power"))
		return "A462";
	if (!strcmp(call->name, "iomfbep_abort_swaps_dcp"))
		return "A463";
	if (!strcmp(call->name, "dcpep_update_dfb"))
		return "A466";
	if (!strcmp(call->name, "dcpep_set_power_state"))
		return "A467";
	if (!strcmp(call->name, "dcpep_is_keep_on_screen"))
		return "A468";

	return NULL;
}

/* Call a DCP function given by a tag */
void dcp_push(struct apple_dcp *dcp, bool oob, const struct dcp_method_entry *call,
		     u32 in_len, u32 out_len, void *data, dcp_callback_t cb,
		     void *cookie)
{
	enum dcp_context_id context = dcp_call_context(dcp, oob);
	struct dcp_channel *ch = dcp_get_channel(dcp, context);
	const char *fw14_tag = iomfb_fw14_method_tag(call);
	char tag[4];

	if (fw14_tag)
		memcpy(tag, fw14_tag, sizeof(tag));
	else
		memcpy(tag, call->tag, sizeof(tag));

	struct dcp_packet_header header = {
		.in_len = in_len,
		.out_len = out_len,

		/* Tag is reversed due to endianness of the fourcc */
		.tag[0] = tag[3],
		.tag[1] = tag[2],
		.tag[2] = tag[1],
		.tag[3] = tag[0],
	};

	u8 depth = dcp_push_depth(&ch->depth);
	u16 offset = dcp_packet_start(ch, depth);

	void *out = dcp->shmem + dcp_tx_offset(context) + offset;
	void *out_data = out + sizeof(header);
	size_t data_len = sizeof(header) + in_len + out_len;

	memcpy(out, &header, sizeof(header));

	if (in_len > 0)
		memcpy(out_data, data, in_len);

	trace_iomfb_push(dcp, call, context, offset, depth);
	if (iomfb_trace_ipc)
		dev_info(dcp->dev,
			 "IOMFB call ctx=%u tag=%c%c%c%c name=%s in=%u out=%u off=0x%x depth=%u%s\n",
			 context, tag[0], tag[1], tag[2], tag[3],
			 call->name, in_len, out_len, offset, depth,
			 fw14_tag ? " fw14-map" : "");

	ch->callbacks[depth] = cb;
	ch->cookies[depth] = cookie;
	ch->output[depth] = out + sizeof(header) + in_len;
	ch->names[depth] = call->name;
	memcpy(ch->tags[depth], tag, sizeof(ch->tags[depth]));
	ch->in_len[depth] = in_len;
	ch->out_len[depth] = out_len;
	ch->end[depth] = offset + ALIGN(data_len, DCP_PACKET_ALIGNMENT);

	dcp_send_message(dcp, IOMFB_ENDPOINT,
				 dcpep_msg(context, data_len, offset));
}

/* Parse a callback tag "D123" into the ID 123. Returns -EINVAL on failure. */
int dcp_parse_tag(char tag[4])
{
	u32 d[3];
	int i;

	if (tag[3] != 'D')
		return -EINVAL;

	for (i = 0; i < 3; ++i) {
		d[i] = (u32)(tag[i] - '0');

		if (d[i] > 9)
			return -EINVAL;
	}

	return d[0] + (d[1] * 10) + (d[2] * 100);
}

/* Ack a callback from the DCP */
void dcp_ack(struct apple_dcp *dcp, enum dcp_context_id context)
{
	struct dcp_channel *ch = dcp_get_channel(dcp, context);

	dcp_pop_depth(&ch->depth);
	dcp_send_message(dcp, IOMFB_ENDPOINT,
			 dcpep_ack(context));
}

/*
 * Helper to send a DRM hotplug event. The DCP is accessed from a single
 * (RTKit) thread. To handle hotplug callbacks, we need to call
 * drm_kms_helper_hotplug_event, which does an atomic commit (via DCP) and
 * waits for vblank (a DCP callback). That means we deadlock if we call from
 * the RTKit thread! Instead, move the call to another thread via a workqueue.
 */
void dcp_hotplug(struct work_struct *work)
{
	struct apple_connector *connector;
	struct apple_dcp *dcp;

	connector = container_of(work, struct apple_connector, hotplug_wq);

	dcp = platform_get_drvdata(connector->dcp);
	dev_info(dcp->dev, "%s() connected:%d valid_mode:%d nr_modes:%u\n", __func__,
		 connector->connected, dcp->valid_mode, dcp->nr_modes);

	if (!connector->connected) {
		drm_edid_free(connector->drm_edid);
		connector->drm_edid = NULL;
	}

	/*
	 * DCP defers link training until we set a display mode. But we set
	 * display modes from atomic_flush, so userspace needs to trigger a
	 * flush, or the CRTC gets no signal.
	 */
	if (connector->base.state && !dcp->valid_mode && connector->connected)
		drm_connector_set_link_status_property(&connector->base,
						       DRM_MODE_LINK_STATUS_BAD);

	drm_kms_helper_connector_hotplug_event(&connector->base);
}

static void dcpep_handle_cb(struct apple_dcp *dcp, enum dcp_context_id context,
			    void *data, u32 length, u16 offset)
{
	struct device *dev = dcp->dev;
	struct dcp_packet_header *hdr = data;
	void *in, *out;
	int tag;
	struct dcp_channel *ch = dcp_get_channel(dcp, context);
	u32 expected_len;
	u8 depth;

	if (length < sizeof(*hdr)) {
		dev_warn(dev, "received short callback len=%u off=0x%x\n",
			 length, offset);
		return;
	}

	tag = dcp_parse_tag(hdr->tag);
	in = data + sizeof(*hdr);
	out = in + hdr->in_len;
	expected_len = sizeof(*hdr) + hdr->in_len + hdr->out_len;

	if (iomfb_trace_ipc)
		dev_info(dev,
			 "IOMFB callback ctx=%u tag=%c%c%c%c id=%d in=%u out=%u len=%u off=0x%x\n",
			 context, hdr->tag[3], hdr->tag[2], hdr->tag[1],
			 hdr->tag[0], tag, hdr->in_len, hdr->out_len, length,
			 offset);

	if (expected_len > length)
		dev_warn(dev,
			 "callback %c%c%c%c size mismatch: expected=%u len=%u\n",
			 hdr->tag[3], hdr->tag[2], hdr->tag[1], hdr->tag[0],
			 expected_len, length);

	if (tag < 0 || tag >= IOMFB_MAX_CB || !dcp->cb_handlers || !dcp->cb_handlers[tag]) {
		dev_warn(dev, "received unknown callback %c%c%c%c\n",
			 hdr->tag[3], hdr->tag[2], hdr->tag[1], hdr->tag[0]);

		if (iomfb_trace_ipc) {
			u32 avail = length - sizeof(*hdr);
			u32 in_avail = min_t(u32, hdr->in_len, avail);
			u32 out_avail = avail > hdr->in_len ? avail - hdr->in_len : 0;
			u32 in_dump = min_t(u32, in_avail, 64);
			u32 out_dump = min_t(u32, min_t(u32, hdr->out_len, out_avail), 64);

			if (in_dump)
				dev_info(dev, "unknown callback input: %*ph\n",
					 in_dump, in);
			if (out_dump)
				dev_info(dev, "unknown callback output before ack: %*ph\n",
					 out_dump, out);
		}

		return;
	}

	// TODO: verify that in_len and out_len match our prototypes
	// for now just clear the out data to have at least consistent results
	if (hdr->out_len)
		memset(out, 0, hdr->out_len);

	depth = dcp_push_depth(&ch->depth);
	ch->output[depth] = out;
	ch->end[depth] = offset + ALIGN(length, DCP_PACKET_ALIGNMENT);

	dcp->callback_in_len = hdr->in_len;
	dcp->callback_out_len = hdr->out_len;
	if (dcp->cb_handlers[tag](dcp, tag, out, in)) {
		if (iomfb_trace_ipc && hdr->out_len) {
			u32 out_dump = min_t(u32, hdr->out_len, 64);

			dev_info(dev,
				 "IOMFB callback output ctx=%u tag=%c%c%c%c bytes=%*ph\n",
				 context, hdr->tag[3], hdr->tag[2],
				 hdr->tag[1], hdr->tag[0], out_dump, out);
		}
		dcp_ack(dcp, context);
	}
	dcp->callback_in_len = 0;
	dcp->callback_out_len = 0;
}

static void dcpep_handle_ack(struct apple_dcp *dcp, enum dcp_context_id context,
			     void *data, u32 length)
{
	struct dcp_channel *ch = dcp_get_channel(dcp, context);
	void *out;
	void *cookie;
	dcp_callback_t cb;
	u32 out_dump;
	u8 depth;

	if (!ch) {
		dev_warn(dcp->dev, "ignoring ack on context %X\n", context);
		return;
	}

	if (!ch->depth) {
		dev_warn(dcp->dev, "ignoring ack on context %u with empty stack\n",
			 context);
		return;
	}

	dcp_pop_depth(&ch->depth);
	depth = ch->depth;

	cb = ch->callbacks[depth];
	cookie = ch->cookies[depth];
	out = ch->output[depth];

	if (iomfb_trace_ipc) {
		dev_info(dcp->dev,
			 "IOMFB ack ctx=%u tag=%c%c%c%c name=%s in=%u out=%u msg_len=%u\n",
			 context, ch->tags[depth][0], ch->tags[depth][1],
			 ch->tags[depth][2], ch->tags[depth][3],
			 ch->names[depth] ?: "unknown", ch->in_len[depth],
			 ch->out_len[depth], length);

		out_dump = min_t(u32, ch->out_len[depth], 32);
		if (out && out_dump)
			dev_info(dcp->dev, "IOMFB ack output: %*ph\n",
				 out_dump, out);
	}

	ch->callbacks[depth] = NULL;
	ch->cookies[depth] = NULL;
	ch->output[depth] = NULL;
	ch->names[depth] = NULL;
	memset(ch->tags[depth], 0, sizeof(ch->tags[depth]));
	ch->in_len[depth] = 0;
	ch->out_len[depth] = 0;

	if (cb)
		cb(dcp, out, cookie);
}

static void dcpep_got_msg(struct apple_dcp *dcp, u64 message)
{
	enum dcp_context_id ctx_id;
	u16 offset;
	u32 length;
	int channel_offset;
	void *data;

	ctx_id = FIELD_GET(IOMFB_MSG_CONTEXT, message);
	offset = FIELD_GET(IOMFB_MSG_OFFSET, message);
	length = FIELD_GET(IOMFB_MSG_LENGTH, message);

	channel_offset = dcp_channel_offset(ctx_id);

	if (channel_offset < 0) {
		dev_warn(dcp->dev, "invalid context received %u\n", ctx_id);
		return;
	}

	data = dcp->shmem + channel_offset + offset;

	if (FIELD_GET(IOMFB_MSG_ACK, message))
		dcpep_handle_ack(dcp, ctx_id, data, length);
	else
		dcpep_handle_cb(dcp, ctx_id, data, length, offset);
}

int dcp_get_modes(struct drm_connector *connector)
{
	struct apple_connector *apple_connector = to_apple_connector(connector);
	struct platform_device *pdev = apple_connector->dcp;
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	struct drm_device *dev = connector->dev;
	struct drm_display_mode *mode;
	int i;

	dev_info(dcp->dev, "get_modes connected=%d nr_modes=%d dcpav=%d edid=%d\n",
		 apple_connector->connected, dcp->nr_modes,
		 dcp->dcpavserv.enabled, !!apple_connector->drm_edid);

	for (i = 0; i < dcp->nr_modes; ++i) {
		dev_info(dcp->dev, "get_modes add[%d] " DRM_MODE_FMT "\n",
			 i, DRM_MODE_ARG(&dcp->modes[i].mode));
		mode = drm_mode_duplicate(dev, &dcp->modes[i].mode);

		if (!mode) {
			dev_err(dev->dev, "Failed to duplicate display mode\n");
			return 0;
		}

		drm_mode_probed_add(connector, mode);
	}

	if (dcp->nr_modes && dcp->dcpavserv.enabled &&
	    !apple_connector->drm_edid) {
		const struct drm_edid *edid;
		edid = dcpavserv_copy_edid(dcp->dcpavserv.service);
		if (IS_ERR_OR_NULL(edid)) {
			dev_info(dcp->dev, "copy_edid failed: %pe\n", edid);
		} else {
			drm_edid_free(apple_connector->drm_edid);
			apple_connector->drm_edid = edid;
		}
	}
	if (dcp->nr_modes && apple_connector->drm_edid)
		drm_edid_connector_update(connector, apple_connector->drm_edid);

	return dcp->nr_modes;
}

/* The user may own drm_display_mode, so we need to search for our copy */
struct dcp_display_mode *lookup_mode(struct apple_dcp *dcp,
					    const struct drm_display_mode *mode)
{
	int i;

	for (i = 0; i < dcp->nr_modes; ++i) {
		if (drm_mode_match(mode, &dcp->modes[i].mode,
				   DRM_MODE_MATCH_TIMINGS |
					   DRM_MODE_MATCH_CLOCK))
			return &dcp->modes[i];
	}

	return NULL;
}

enum drm_mode_status dcp_mode_valid(struct drm_connector *connector,
				    const struct drm_display_mode *mode)
{
	struct apple_connector *apple_connector = to_apple_connector(connector);
	struct platform_device *pdev = apple_connector->dcp;
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	enum drm_mode_status status;

	status = lookup_mode(dcp, mode) ? MODE_OK : MODE_BAD;
	dev_info(dcp->dev, "mode_valid status=%d " DRM_MODE_FMT "\n",
		 status, DRM_MODE_ARG(mode));

	return status;
}

int dcp_crtc_atomic_modeset(struct drm_crtc *crtc,
			    struct drm_atomic_state *state)
{
	struct apple_crtc *apple_crtc = to_apple_crtc(crtc);
	struct apple_dcp *dcp = platform_get_drvdata(apple_crtc->dcp);
	struct drm_crtc_state *crtc_state;
	int ret = -EIO;
	bool modeset;

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (!crtc_state)
		return 0;

	modeset = drm_atomic_crtc_needs_modeset(crtc_state) || !dcp->valid_mode;
	dev_info(dcp->dev,
		 "dcp_crtc_atomic_modeset entry modeset=%d dcp_valid=%d active=%d "
		 "mode_changed=%d " DRM_MODE_FMT "\n",
		 modeset, dcp->valid_mode, crtc_state->active,
		 crtc_state->mode_changed, DRM_MODE_ARG(&crtc_state->mode));

	if (!modeset)
		return 0;

	/* ignore no mode, poweroff is handled elsewhere */
	if (crtc_state->mode.hdisplay == 0 && crtc_state->mode.vdisplay == 0)
		return 0;

	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		ret = iomfb_modeset_v12_3(dcp, crtc_state);
		break;
	case DCP_FIRMWARE_V_13_5:
		ret = iomfb_modeset_v13_3(dcp, crtc_state);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n",
			  dcp->fw_compat);
		break;
	}

	return ret;
}

bool dcp_crtc_mode_fixup(struct drm_crtc *crtc,
			 const struct drm_display_mode *mode,
			 struct drm_display_mode *adjusted_mode)
{
	struct apple_crtc *apple_crtc = to_apple_crtc(crtc);
	struct platform_device *pdev = apple_crtc->dcp;
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	/* TODO: support synthesized modes through scaling */
	return lookup_mode(dcp, mode) != NULL;
}


void dcp_flush(struct drm_crtc *crtc, struct drm_atomic_state *state)
{
	struct platform_device *pdev = to_apple_crtc(crtc)->dcp;
	struct apple_dcp *dcp = platform_get_drvdata(pdev);
	struct drm_crtc_state *crtc_state;

	if (iomfb_skip_flush_invalid_mode && !dcp->valid_mode) {
		dev_info(dcp->dev,
			 "skipping IOMFB flush/swap because valid_mode is false\n");
		dcp_schedule_vblank(dcp, "flush-invalid-mode");
		return;
	}

	crtc_state = drm_atomic_get_new_crtc_state(state, crtc);
	if (crtc_state)
		dev_info(dcp->dev,
			 "dcp_flush entry active=%d mode_changed=%d planes_changed=%d "
			 "color_mgmt_changed=%d dcp_valid=%d " DRM_MODE_FMT "\n",
			 crtc_state->active, crtc_state->mode_changed,
			 crtc_state->planes_changed,
			 crtc_state->color_mgmt_changed, dcp->valid_mode,
			 DRM_MODE_ARG(&crtc_state->mode));

	if (dcp_channel_busy(&dcp->ch_cmd))
	{
		if (!dcp->ch_cmd.warned_busy) {
			dev_err(dcp->dev, "unexpected busy command channel\n");
			dcp->ch_cmd.warned_busy = true;
		}
		/* HACK: issue a delayed vblank event to avoid timeouts in
		 * drm_atomic_helper_wait_for_vblanks().
		 */
		dcp_schedule_vblank(dcp, "flush-command-channel-busy");
		return;
	} else if (dcp->ch_cmd.warned_busy) {
		dcp->ch_cmd.warned_busy = false;
	}

	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		iomfb_flush_v12_3(dcp, crtc, state);
		break;
	case DCP_FIRMWARE_V_13_5:
		iomfb_flush_v13_3(dcp, crtc, state);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
}

static void iomfb_start(struct apple_dcp *dcp)
{
	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		iomfb_start_v12_3(dcp);
		break;
	case DCP_FIRMWARE_V_13_5:
		iomfb_start_v13_3(dcp);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
}

bool dcp_is_initialized(struct platform_device *pdev)
{
	struct apple_dcp *dcp = platform_get_drvdata(pdev);

	return dcp->active;
}

void iomfb_recv_msg(struct apple_dcp *dcp, u64 message)
{
	enum dcpep_type type = FIELD_GET(IOMFB_MESSAGE_TYPE, message);

	if (type == IOMFB_MESSAGE_TYPE_INITIALIZED)
		iomfb_start(dcp);
	else if (type == IOMFB_MESSAGE_TYPE_MSG)
		dcpep_got_msg(dcp, message);
	else
		dev_warn(dcp->dev, "Ignoring unknown message %llx\n", message);
}

int iomfb_start_rtkit(struct apple_dcp *dcp)
{
	dma_addr_t shmem_iova;
	dma_addr_t shmem_msg_dva;
	phys_addr_t shmem_phys = 0;
	struct iommu_domain *domain;

	apple_rtkit_start_ep(dcp->rtk, IOMFB_ENDPOINT);

	dcp->shmem = dma_alloc_coherent(dcp->dev, DCP_SHMEM_SIZE, &shmem_iova,
					GFP_KERNEL);
	if (!dcp->shmem)
		return -ENOMEM;

	shmem_msg_dva = shmem_iova | iomfb_shmem_dva_or;
	domain = iommu_get_domain_for_dev(dcp->dev);
	if (domain)
		shmem_phys = iommu_iova_to_phys(domain, shmem_iova);

	dev_info(dcp->dev,
		 "IOMFB SET_SHMEM iova=%pad msg_dva=%pad phys=%pa flag=0x%x dva_or=0x%llx\n",
		 &shmem_iova, &shmem_msg_dva, &shmem_phys,
		 iomfb_shmem_flag & 0xf, iomfb_shmem_dva_or);
	dcp_send_message(dcp, IOMFB_ENDPOINT, dcpep_set_shmem(shmem_msg_dva));

	return 0;
}

void iomfb_shutdown(struct apple_dcp *dcp)
{
	/* We're going down */
	dcp->active = false;
	dcp->valid_mode = false;

	switch (dcp->fw_compat) {
	case DCP_FIRMWARE_V_12_3:
		iomfb_shutdown_v12_3(dcp);
		break;
	case DCP_FIRMWARE_V_13_5:
		iomfb_shutdown_v13_3(dcp);
		break;
	default:
		WARN_ONCE(true, "Unexpected firmware version: %u\n", dcp->fw_compat);
		break;
	}
}
