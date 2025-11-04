/*
 * Copyright 2019 Advanced Micro Devices, Inc.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 *
 * Authors: AMD
 *
 */

#include "dc.h"
#include "dc_dmub_srv.h"
#include "../dmub/dmub_srv.h"
#include "clk_mgr.h"

static void dc_dmub_srv_construct(struct dc_dmub_srv *dc_srv, struct dc *dc,
				  struct dmub_srv *dmub)
{
	dc_srv->dmub = dmub;
	dc_srv->ctx = dc->ctx;
}

struct dc_dmub_srv *dc_dmub_srv_create(struct dc *dc, struct dmub_srv *dmub)
{
	struct dc_dmub_srv *dc_srv =
		kzalloc(sizeof(struct dc_dmub_srv), GFP_KERNEL);

	if (dc_srv == NULL) {
		BREAK_TO_DEBUGGER();
		return NULL;
	}

	dc_dmub_srv_construct(dc_srv, dc, dmub);

	return dc_srv;
}

void dc_dmub_srv_destroy(struct dc_dmub_srv **dmub_srv)
{
	if (*dmub_srv) {
		kfree(*dmub_srv);
		*dmub_srv = NULL;
	}
}

void dc_dmub_srv_cmd_queue(struct dc_dmub_srv *dc_dmub_srv,
			   union dmub_rb_cmd *cmd)
{
	struct dmub_srv *dmub = dc_dmub_srv->dmub;
	struct dc_context *dc_ctx = dc_dmub_srv->ctx;
	enum dmub_status status;

	status = dmub_srv_cmd_queue(dmub, cmd);
	if (status == DMUB_STATUS_OK)
		return;

	if (status != DMUB_STATUS_QUEUE_FULL)
		goto error;

	/* Execute and wait for queue to become empty again. */
	dc_dmub_srv_cmd_execute(dc_dmub_srv);
	dc_dmub_srv_wait_idle(dc_dmub_srv);

	/* Requeue the command. */
	status = dmub_srv_cmd_queue(dmub, cmd);
	if (status == DMUB_STATUS_OK)
		return;

error:
	DC_ERROR("Error queuing DMUB command: status=%d\n", status);
}

bool dc_dmub_srv_cmd_run(struct dc_dmub_srv *dc_dmub_srv, union dmub_rb_cmd *cmd,
			 enum dm_dmub_wait_type wait_type)
{
	return dc_dmub_srv_cmd_run_list(dc_dmub_srv, 1, cmd, wait_type);
}

bool dc_dmub_srv_cmd_run_list(struct dc_dmub_srv *dc_dmub_srv, unsigned int count,
			      union dmub_rb_cmd *cmd_list, enum dm_dmub_wait_type wait_type)
{
	struct dc_context *dc_ctx;
	struct dmub_srv *dmub;
	enum dmub_status status;
	int i;

	if (!dc_dmub_srv || !dc_dmub_srv->dmub)
		return false;

	dc_ctx = dc_dmub_srv->ctx;
	dmub = dc_dmub_srv->dmub;

	for (i = 0 ; i < count; i++) {
		// Queue command
		status = dmub_srv_cmd_queue(dmub, &cmd_list[i]);
		if (status == DMUB_STATUS_QUEUE_FULL) {
			/* Execute and wait for queue to become empty again. */
			dmub_srv_cmd_execute(dmub);
			dmub_srv_wait_for_idle(dmub, 100000);

			/* Requeue the command. */
			status = dmub_srv_cmd_queue(dmub, &cmd_list[i]);
		}

		if (status != DMUB_STATUS_OK) {
			DC_ERROR("Error queueing DMUB command: status=%d\n", status);
			return false;
		}
	}

	status = dmub_srv_cmd_execute(dmub);
	if (status != DMUB_STATUS_OK) {
		DC_ERROR("Error starting DMUB execution: status=%d\n", status);
		return false;
	}

	// Wait for DMUB to process command
	if (wait_type != DM_DMUB_WAIT_TYPE_NO_WAIT) {
		status = dmub_srv_wait_for_idle(dmub, 100000);
		if (status != DMUB_STATUS_OK) {
			DC_LOG_DEBUG("No reply for DMUB command: status=%d\n", status);
			return false;
		}

		// Copy data back from ring buffer into command
		if (wait_type == DM_DMUB_WAIT_TYPE_WAIT_WITH_REPLY)
			dmub_rb_get_return_data(&dmub->inbox1_rb, cmd_list);
	}

	return true;
}

void dc_dmub_srv_cmd_execute(struct dc_dmub_srv *dc_dmub_srv)
{
	struct dmub_srv *dmub = dc_dmub_srv->dmub;
	struct dc_context *dc_ctx = dc_dmub_srv->ctx;
	enum dmub_status status;

	status = dmub_srv_cmd_execute(dmub);
	if (status != DMUB_STATUS_OK)
		DC_ERROR("Error starting DMUB execution: status=%d\n", status);
}

void dc_dmub_srv_wait_idle(struct dc_dmub_srv *dc_dmub_srv)
{
	struct dmub_srv *dmub = dc_dmub_srv->dmub;
	struct dc_context *dc_ctx = dc_dmub_srv->ctx;
	enum dmub_status status;

	status = dmub_srv_wait_for_idle(dmub, 100000);
	if (status != DMUB_STATUS_OK)
		DC_ERROR("Error waiting for DMUB idle: status=%d\n", status);
}

void dc_dmub_srv_wait_phy_init(struct dc_dmub_srv *dc_dmub_srv)
{
	struct dmub_srv *dmub = dc_dmub_srv->dmub;
	struct dc_context *dc_ctx = dc_dmub_srv->ctx;
	enum dmub_status status;

	for (;;) {
		/* Wait up to a second for PHY init. */
		status = dmub_srv_wait_for_phy_init(dmub, 1000000);
		if (status == DMUB_STATUS_OK)
			/* Initialization OK */
			break;

		DC_ERROR("DMCUB PHY init failed: status=%d\n", status);
		ASSERT(0);

		if (status != DMUB_STATUS_TIMEOUT)
			/*
			 * Server likely initialized or we don't have
			 * DMCUB HW support - this won't end.
			 */
			break;

		/* Continue spinning so we don't hang the ASIC. */
	}
}

bool dc_dmub_srv_notify_stream_mask(struct dc_dmub_srv *dc_dmub_srv,
				    unsigned int stream_mask)
{
	if (!dc_dmub_srv || !dc_dmub_srv->dmub)
		return false;

	return dc_wake_and_execute_gpint(dc_dmub_srv->ctx, DMUB_GPINT__IDLE_OPT_NOTIFY_STREAM_MASK,
					 stream_mask, NULL, DM_DMUB_WAIT_TYPE_WAIT);
}

bool dc_dmub_srv_is_hw_pwr_up(struct dc_dmub_srv *dc_dmub_srv, bool wait)
{
	struct dc_context *dc_ctx;
	enum dmub_status status;

	if (!dc_dmub_srv || !dc_dmub_srv->dmub)
		return true;

	if (dc_dmub_srv->ctx->dc->debug.dmcub_emulation)
		return true;

	dc_ctx = dc_dmub_srv->ctx;

	if (wait) {
		status = dmub_srv_wait_for_hw_pwr_up(dc_dmub_srv->dmub, 500000);
		if (status != DMUB_STATUS_OK) {
			DC_ERROR("Error querying DMUB hw power up status: error=%d\n", status);
			return false;
		}
	} else {
		return dmub_srv_is_hw_pwr_up(dc_dmub_srv->dmub);
	}

	return true;
}

static void dc_dmub_srv_notify_idle(const struct dc *dc, bool allow_idle)
{
	union dmub_rb_cmd cmd = {0};

	if (dc->debug.dmcub_emulation)
		return;

	memset(&cmd, 0, sizeof(cmd));
	cmd.idle_opt_notify_idle.header.type = DMUB_CMD__IDLE_OPT;
	cmd.idle_opt_notify_idle.header.sub_type = DMUB_CMD__IDLE_OPT_DCN_NOTIFY_IDLE;
	cmd.idle_opt_notify_idle.header.payload_bytes =
		sizeof(cmd.idle_opt_notify_idle) -
		sizeof(cmd.idle_opt_notify_idle.header);

	cmd.idle_opt_notify_idle.cntl_data.driver_idle = allow_idle;

	if (allow_idle) {
		if (dc->hwss.set_idle_state)
			dc->hwss.set_idle_state(dc, true);
	}

	/* NOTE: This does not use the "wake" interface since this is part of the wake path. */
	dm_execute_dmub_cmd(dc->ctx, &cmd, DM_DMUB_WAIT_TYPE_WAIT);
}

static void dc_dmub_srv_exit_low_power_state(const struct dc *dc)
{
	const uint32_t max_num_polls = 10000;
	uint32_t allow_state = 0;
	uint32_t commit_state = 0;
	uint32_t i;

	if (dc->debug.dmcub_emulation)
		return;

	if (!dc->idle_optimizations_allowed)
		return;

	if (!dc->ctx->dmub_srv || !dc->ctx->dmub_srv->dmub)
		return;

	if (dc->hwss.get_idle_state &&
		dc->hwss.set_idle_state &&
		dc->clk_mgr->funcs->exit_low_power_state) {
		allow_state = dc->hwss.get_idle_state(dc);
		dc->hwss.set_idle_state(dc, false);

		if (!(allow_state & DMUB_IPS2_ALLOW_MASK)) {
			// Wait for evaluation time
			udelay(dc->debug.ips2_eval_delay_us);
			commit_state = dc->hwss.get_idle_state(dc);
			if (!(commit_state & DMUB_IPS2_COMMIT_MASK)) {
				// Tell PMFW to exit low power state
				dc->clk_mgr->funcs->exit_low_power_state(dc->clk_mgr);

				// Wait for IPS2 entry upper bound
				udelay(dc->debug.ips2_entry_delay_us);
				dc->clk_mgr->funcs->exit_low_power_state(dc->clk_mgr);

				for (i = 0; i < max_num_polls; ++i) {
					commit_state = dc->hwss.get_idle_state(dc);
					if (commit_state & DMUB_IPS2_COMMIT_MASK)
						break;

					udelay(1);
				}
				ASSERT(i < max_num_polls);

				if (!dc_dmub_srv_is_hw_pwr_up(dc->ctx->dmub_srv, true))
					ASSERT(0);

				/* TODO: See if we can return early here - IPS2 should go
				 * back directly to IPS0 and clear the flags, but it will
				 * be safer to directly notify DMCUB of this.
				 */
				allow_state = dc->hwss.get_idle_state(dc);
			}
		}

		dc_dmub_srv_notify_idle(dc, false);
		if (!(allow_state & DMUB_IPS1_ALLOW_MASK)) {
			for (i = 0; i < max_num_polls; ++i) {
				commit_state = dc->hwss.get_idle_state(dc);
				if (commit_state & DMUB_IPS1_COMMIT_MASK)
					break;

				udelay(1);
			}
			ASSERT(i < max_num_polls);
		}
	}

	if (!dc_dmub_srv_is_hw_pwr_up(dc->ctx->dmub_srv, true))
		ASSERT(0);
}

void dc_dmub_srv_apply_idle_power_optimizations(const struct dc *dc, bool allow_idle)
{
	struct dc_dmub_srv *dc_dmub_srv = dc->ctx->dmub_srv;

	if (!dc_dmub_srv || !dc_dmub_srv->dmub)
		return;

	if (dc_dmub_srv->idle_allowed == allow_idle)
		return;

	/*
	 * Entering a low power state requires a driver notification.
	 * Powering up the hardware requires notifying PMFW and DMCUB.
	 * Clearing the driver idle allow requires a DMCUB command.
	 * DMCUB commands requires the DMCUB to be powered up and restored.
	 *
	 * Exit out early to prevent an infinite loop of DMCUB commands
	 * triggering exit low power - use software state to track this.
	 */
	dc_dmub_srv->idle_allowed = allow_idle;

	if (!allow_idle)
		dc_dmub_srv_exit_low_power_state(dc);
	else
		dc_dmub_srv_notify_idle(dc, allow_idle);
}

static bool dc_dmub_execute_gpint(const struct dc_context *ctx,
				  enum dmub_gpint_command command_code, uint16_t param,
				  uint32_t *response, enum dm_dmub_wait_type wait_type)
{
	struct dc_dmub_srv *dc_dmub_srv = ctx->dmub_srv;
	const uint32_t wait_us = wait_type == DM_DMUB_WAIT_TYPE_NO_WAIT ? 0 : 30;
	enum dmub_status status;

	if (response)
		*response = 0;

	if (!dc_dmub_srv || !dc_dmub_srv->dmub)
		return false;

	status = dmub_srv_send_gpint_command(dc_dmub_srv->dmub, command_code, param, wait_us);
	if (status != DMUB_STATUS_OK) {
		if (status == DMUB_STATUS_TIMEOUT && wait_type == DM_DMUB_WAIT_TYPE_NO_WAIT)
			return true;

		return false;
	}

	if (response && wait_type == DM_DMUB_WAIT_TYPE_WAIT_WITH_REPLY)
		dmub_srv_get_gpint_response(dc_dmub_srv->dmub, response);

	return true;
}

bool dc_wake_and_execute_gpint(const struct dc_context *ctx, enum dmub_gpint_command command_code,
			       uint16_t param, uint32_t *response, enum dm_dmub_wait_type wait_type)
{
	struct dc_dmub_srv *dc_dmub_srv = ctx->dmub_srv;
	bool result = false, reallow_idle = false;

	if (!dc_dmub_srv || !dc_dmub_srv->dmub)
		return false;

	if (dc_dmub_srv->idle_allowed) {
		dc_dmub_srv_apply_idle_power_optimizations(ctx->dc, false);
		reallow_idle = true;
	}

	result = dc_dmub_execute_gpint(ctx, command_code, param, response, wait_type);
	if (result && reallow_idle)
		dc_dmub_srv_apply_idle_power_optimizations(ctx->dc, true);

	return result;
}
