/* Copyright (c) 2011, Code Aurora Forum. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 *
 */

#include <linux/module.h>
#include <linux/delay.h>
#include <linux/debugfs.h>
#include <linux/types.h>
#include <linux/i2c.h>
#include <linux/uaccess.h>
#include <linux/miscdevice.h>
#include <linux/slab.h>
#include <media/msm_camera.h>
#include <mach/gpio.h>
#include <mach/camera.h>
#include "mt9e013.h"

/*=============================================================
	SENSOR REGISTER DEFINES
==============================================================*/
#define MT9E013_REG_MODEL_ID 0x0000
#define MT9E013_MODEL_ID 0x4B00
#define MODULE_LITEON 1
#define MODULE_SUNNY 2
static uint16_t mt9e013_model_id = MODULE_LITEON;

#define REG_GROUPED_PARAMETER_HOLD		0x0104
#define GROUPED_PARAMETER_HOLD_OFF		0x00
#define GROUPED_PARAMETER_HOLD			0x01
/* Integration Time */
#define REG_COARSE_INTEGRATION_TIME		0x3012
/* Gain */
#define REG_GLOBAL_GAIN				0x305E
/* PLL registers */
#define REG_FRAME_LENGTH_LINES			0x0340
/* Test Pattern */
#define REG_TEST_PATTERN_MODE			0x0601
#define REG_VCM_NEW_CODE			0x30F2

/*============================================================================
							 TYPE DECLARATIONS
============================================================================*/

/* 16bit address - 8 bit context register structure */
#define Q8	0x00000100
#define Q10	0x00000400
#define MT9E013_MASTER_CLK_RATE 24000000
/*tunnig the auto focus params, change the total step, and tune each step that it has*/
/* AF Total steps parameters */
static uint16_t mt9e013_linear_total_step = 41;
static uint16_t mt9e013_step_jump = 4;
#define MT9E013_TOTAL_STEPS_NEAR_TO_FAR_MAX 41

static uint16_t mt9e013_step_position_table[MT9E013_TOTAL_STEPS_NEAR_TO_FAR_MAX+1];
static uint16_t mt9e013_nl_region_boundary1 = 2;
static uint16_t mt9e013_nl_region_code_per_step1 = 25;
static uint16_t mt9e013_l_region_code_per_step = 3;
static uint16_t mt9e013_damping_threshold = 10;
static uint16_t mt9e013_sw_damping_time_wait = 30;
static uint16_t mt9e013_sw_damping_small_step = 10;
static uint16_t mt9e013_sw_damping_time_wait_coarse = 30;
static uint16_t mt9e013_sw_damping_small_step_coarse = 2;
static uint16_t mt9e013_sw_damping_time_wait_fwd = 9;
static uint16_t mt9e013_sw_damping_small_step_fwd = 5;
static uint16_t mt9e013_sw_damping_time_wait_fine = 60;
static uint16_t mt9e013_sw_damping_small_step_fine = 1;
static uint16_t mt9e013_sw_damping_time_wait_snap = 9;
static uint16_t mt9e013_sw_damping_small_step_snap = 5;
static uint16_t mt9e013_enable_damping = 1;
static uint16_t mt9e013_af_initial_value = 25;

static struct i2c_client *mt9e013_client;

struct mt9e013_ctrl_t {
	const struct  msm_camera_sensor_info *sensordata;

	uint32_t sensormode;
	uint32_t fps_divider;/* init to 1 * 0x00000400 */
	uint32_t pict_fps_divider;/* init to 1 * 0x00000400 */
	uint16_t fps;

	uint16_t curr_lens_pos;
	uint16_t curr_step_pos;
	uint16_t my_reg_gain;
	uint32_t my_reg_line_count;
	uint16_t total_lines_per_frame;

	enum mt9e013_resolution_t prev_res;
	enum mt9e013_resolution_t pict_res;
	enum mt9e013_resolution_t curr_res;
	enum mt9e013_test_mode_t  set_test;
};


static bool CSI_CONFIG;
static struct mt9e013_ctrl_t *mt9e013_ctrl;
static DECLARE_WAIT_QUEUE_HEAD(mt9e013_wait_queue);
static DEFINE_MUTEX(mt9e013_mut);

static int cam_debug_init(void);
static struct dentry *debugfs_base;
/*=============================================================*/

static int mt9e013_i2c_rxdata(unsigned short saddr, unsigned char *rxdata,
	int length)
{
	int rc = 0;
	struct i2c_msg msgs[] = {
		{
			.addr  = saddr,
			.flags = 0,
			.len   = 2,
			.buf   = rxdata,
		},
		{
			.addr  = saddr,
			.flags = I2C_M_RD,
			.len   = 2,
			.buf   = rxdata,
		},
	};

	if (i2c_transfer(mt9e013_client->adapter, msgs, ARRAY_SIZE(msgs)) < 0) {
		pr_err("%s: i2c_transfer failed rc=%d\n", __func__, rc);
		rc = -EIO;
	}

	return rc;
}

static int mt9e013_i2c_txdata(unsigned short saddr, unsigned char *txdata,
	int length)
{
	int rc = 0;
	struct i2c_msg msg[] = {
		{
			.addr = saddr,
			.flags = 0,
			.len = length,
			.buf = txdata,
		 },
	};
	if (i2c_transfer(mt9e013_client->adapter, msg, ARRAY_SIZE(msg)) < 0) {
		pr_err("%s: i2c_transfer failed rc=%d\n", __func__, rc);
		rc = -EIO;
	}

	return rc;
}

static int mt9e013_i2c_read(unsigned short raddr, unsigned short *rdata,
	int rlen)
{
	int rc = 0;
	unsigned char buf[2];
	if (!rdata)
		return -EIO;
	memset(buf, 0, sizeof(buf));
	buf[0] = (raddr & 0xFF00) >> 8;
	buf[1] = (raddr & 0x00FF);
	rc = mt9e013_i2c_rxdata(mt9e013_client->addr << 1, buf, rlen);
	if (rc < 0) {
		CDBG("%s: mt9e013_i2c_rxdata failed raddr 0x%x\n",
			__func__, raddr);
		return rc;
	}
	*rdata = (rlen == 2 ? buf[0] << 8 | buf[1] : buf[0]);
	CDBG("%s: raddr 0x%x val 0x%x\n", __func__, raddr, *rdata);
	return rc;
}

static int mt9e013_i2c_write_w_sensor(unsigned short waddr, uint16_t wdata)
{
	int rc = -EFAULT;
	unsigned char buf[4];
	memset(buf, 0, sizeof(buf));
	buf[0] = (waddr & 0xFF00) >> 8;
	buf[1] = (waddr & 0x00FF);
	buf[2] = (wdata & 0xFF00) >> 8;
	buf[3] = (wdata & 0x00FF);
	CDBG("%s: 0x%x val 0x%x\n", __func__, waddr, wdata);
	rc = mt9e013_i2c_txdata(mt9e013_client->addr << 1, buf,
		ARRAY_SIZE(buf));
	if (rc < 0) {
		CDBG("%s: mt9e013_i2c_txdata failed waddr 0x%x wdata 0x%x\n",
			__func__, waddr, wdata);
	}
	return rc;
}

static int mt9e013_i2c_write_b_sensor(unsigned short waddr, uint8_t bdata)
{
	int rc = -EFAULT;
	unsigned char buf[3];
	memset(buf, 0, sizeof(buf));
	buf[0] = (waddr & 0xFF00) >> 8;
	buf[1] = (waddr & 0x00FF);
	buf[2] = bdata;
	CDBG("%s: 0x%x val 0x%x\n", __func__, waddr, bdata);
	rc = mt9e013_i2c_txdata(mt9e013_client->addr << 1, buf,
		ARRAY_SIZE(buf));
	if (rc < 0) {
		CDBG("%s: mt9e013_i2c_txdata failed waddr 0x%x bdata 0x%x\n",
			__func__, waddr, bdata);
	}
	return rc;
}

static int mt9e013_i2c_write_w_table(
	struct mt9e013_i2c_reg_conf const *reg_conf_tbl, int num)
{
	int i;
	int rc = -EIO;
	for (i = 0; i < num; i++) {
		rc = mt9e013_i2c_write_w_sensor(reg_conf_tbl->waddr,
			reg_conf_tbl->wdata);
		if (rc < 0)
			break;
		reg_conf_tbl++;
	}
	return rc;
}

static void mt9e013_group_hold_on(void)
{
	mt9e013_i2c_write_b_sensor(REG_GROUPED_PARAMETER_HOLD,
		GROUPED_PARAMETER_HOLD);
}

static void mt9e013_group_hold_off(void)
{
	mt9e013_i2c_write_b_sensor(REG_GROUPED_PARAMETER_HOLD,
		GROUPED_PARAMETER_HOLD_OFF);
}

static void mt9e013_start_stream(void)
{
	mt9e013_i2c_write_w_sensor(0x301A, 0x8250);
	mt9e013_i2c_write_w_sensor(0x301A, 0x8650);
	mt9e013_i2c_write_w_sensor(0x301A, 0x8658);
	mt9e013_i2c_write_b_sensor(0x0104, 0x00);
	mt9e013_i2c_write_w_sensor(0x301A, 0x065C);
}

static void mt9e013_stop_stream(void)
{
	mt9e013_i2c_write_w_sensor(0x301A, 0x0058);
	mt9e013_i2c_write_w_sensor(0x301A, 0x0050);
	mt9e013_i2c_write_b_sensor(0x0104, 0x01);
}

static void mt9e013_get_pict_fps(uint16_t fps, uint16_t *pfps)
{
	/* input fps is preview fps in Q8 format */
	uint32_t divider, d1, d2;

	d1 = mt9e013_regs.reg_prev[E013_FRAME_LENGTH_LINES].wdata
		* 0x00000400/
		mt9e013_regs.reg_snap[E013_FRAME_LENGTH_LINES].wdata;
	d2 = mt9e013_regs.reg_prev[E013_LINE_LENGTH_PCK].wdata
		* 0x00000400/
		mt9e013_regs.reg_snap[E013_LINE_LENGTH_PCK].wdata;
	divider = d1 * d2 / 0x400;

	/*Verify PCLK settings and frame sizes.*/
	*pfps = (uint16_t) (fps * divider / 0x400);
	/* 2 is the ratio of no.of snapshot channels
	 * to number of preview channels */
}

static uint16_t mt9e013_get_prev_lines_pf(void)
{
	if (mt9e013_ctrl->prev_res == QTR_SIZE)
		return mt9e013_regs.reg_prev[E013_FRAME_LENGTH_LINES].wdata;
	else
		return mt9e013_regs.reg_snap[E013_FRAME_LENGTH_LINES].wdata;
}

static uint16_t mt9e013_get_prev_pixels_pl(void)
{
	if (mt9e013_ctrl->prev_res == QTR_SIZE)
		return mt9e013_regs.reg_prev[E013_LINE_LENGTH_PCK].wdata;
	else
		return mt9e013_regs.reg_snap[E013_LINE_LENGTH_PCK].wdata;
}

static uint16_t mt9e013_get_pict_lines_pf(void)
{
	if (mt9e013_ctrl->pict_res == QTR_SIZE)
		return mt9e013_regs.reg_prev[E013_FRAME_LENGTH_LINES].wdata;
	else
		return mt9e013_regs.reg_snap[E013_FRAME_LENGTH_LINES].wdata;
}

static uint16_t mt9e013_get_pict_pixels_pl(void)
{
	if (mt9e013_ctrl->pict_res == QTR_SIZE)
		return mt9e013_regs.reg_prev[E013_LINE_LENGTH_PCK].wdata;
	else
		return mt9e013_regs.reg_snap[E013_LINE_LENGTH_PCK].wdata;
}

static uint32_t mt9e013_get_pict_max_exp_lc(void)
{
	if (mt9e013_ctrl->pict_res == QTR_SIZE)
		return mt9e013_regs.reg_prev[E013_FRAME_LENGTH_LINES].wdata
			* 24;
	else
		return mt9e013_regs.reg_snap[E013_FRAME_LENGTH_LINES].wdata
			* 24;
}

static int mt9e013_set_fps(struct fps_cfg *fps)
{
	uint16_t total_lines_per_frame;
	int rc = 0;
	if (mt9e013_ctrl->sensormode == SENSOR_PREVIEW_MODE) {
		total_lines_per_frame = (uint16_t)
		((mt9e013_regs.reg_prev[E013_FRAME_LENGTH_LINES].wdata)
		* mt9e013_ctrl->fps_divider/0x400);
	} else {
		total_lines_per_frame = (uint16_t)
		((mt9e013_regs.reg_snap[E013_FRAME_LENGTH_LINES].wdata)
		 * mt9e013_ctrl->pict_fps_divider/0x400);
	}
	mt9e013_ctrl->fps_divider = fps->fps_div;
	mt9e013_ctrl->pict_fps_divider = fps->pict_fps_div;

	mt9e013_group_hold_on();
	rc = mt9e013_i2c_write_w_sensor(REG_FRAME_LENGTH_LINES,
		total_lines_per_frame);
	mt9e013_group_hold_off();
	return rc;
}

static int mt9e013_write_exp_gain(uint16_t gain, uint32_t line)
{
	uint16_t max_legal_gain = 0xE7F;
	int rc = 0;
	if (gain > max_legal_gain) {
		CDBG("%s gain 0x%x > 0x%x\n", __func__, gain, max_legal_gain);
		gain = max_legal_gain;
	}

	if (mt9e013_ctrl->sensormode == SENSOR_PREVIEW_MODE) {
		mt9e013_ctrl->my_reg_gain = gain;
		mt9e013_ctrl->my_reg_line_count = (uint16_t) line;
		line = (uint32_t) (line * mt9e013_ctrl->fps_divider /
			0x00000400);
	} else {
		line = (uint32_t) (line * mt9e013_ctrl->pict_fps_divider /
			0x00000400);
	}

	gain |= 0x1000;

	mt9e013_group_hold_on();
	rc = mt9e013_i2c_write_w_sensor(REG_GLOBAL_GAIN, gain);
	rc = mt9e013_i2c_write_w_sensor(REG_COARSE_INTEGRATION_TIME, line);
	mt9e013_group_hold_off();
	return rc;
}

static int mt9e013_set_pict_exp_gain(uint16_t gain, uint32_t line)
{
	int rc = 0;
	rc = mt9e013_write_exp_gain(gain, line);
	mt9e013_i2c_write_w_sensor(0x301A, 0x065C | 0x2);
	return rc;
}

static int mt9e013_move_focus(int direction, int32_t num_steps)
{
	int16_t step_direction, dest_lens_position, dest_step_position;
	int16_t target_dist, small_step, next_lens_position;
	uint16_t sw_damping_time_wait = 1;

	if (direction == MOVE_NEAR)
		step_direction = 1;
	else
		step_direction = -1;

	dest_step_position = mt9e013_ctrl->curr_step_pos +
		(step_direction * num_steps);

	if (dest_step_position < 0)
		dest_step_position = 0;
	else if (dest_step_position > mt9e013_linear_total_step)
		dest_step_position = mt9e013_linear_total_step;

	if (dest_step_position == mt9e013_ctrl->curr_step_pos)
		return 0;

	dest_lens_position = mt9e013_step_position_table[dest_step_position];
	CDBG("%s: step=%d lens=%d\n", __func__, dest_step_position,
		dest_lens_position);
	if (mt9e013_enable_damping) {
		target_dist = step_direction *
			(dest_lens_position - mt9e013_ctrl->curr_lens_pos);

		if (step_direction < 0) {
			if (num_steps >= mt9e013_damping_threshold) {
				/* sweeping towards all the way in infinity direction between snaps */
				small_step = mt9e013_sw_damping_small_step_snap;
				sw_damping_time_wait = mt9e013_sw_damping_time_wait_snap;
			} else if (num_steps <= 4) {
				/* reverse search during macro mode */
				small_step = mt9e013_sw_damping_small_step_fine;
				sw_damping_time_wait = mt9e013_sw_damping_time_wait_fine;
			} else {
				small_step = mt9e013_sw_damping_small_step;
				sw_damping_time_wait = mt9e013_sw_damping_time_wait;
			}
		} else {
			if (num_steps > mt9e013_step_jump) {
				small_step = mt9e013_sw_damping_small_step_fwd;
				sw_damping_time_wait = mt9e013_sw_damping_time_wait_fwd;
			} else {
				small_step = mt9e013_sw_damping_small_step_coarse;
				sw_damping_time_wait = mt9e013_sw_damping_time_wait_coarse;
			}
		}

		for (next_lens_position = mt9e013_ctrl->curr_lens_pos
			+ (step_direction * small_step);
			(step_direction * next_lens_position) <=
			(step_direction * dest_lens_position);
			next_lens_position += (step_direction * small_step)) {
			mt9e013_i2c_write_w_sensor(REG_VCM_NEW_CODE,
				next_lens_position);
			mt9e013_ctrl->curr_lens_pos = next_lens_position;
			usleep(sw_damping_time_wait * 50);
		}
	}

	if (mt9e013_ctrl->curr_lens_pos != dest_lens_position) {
		mt9e013_i2c_write_w_sensor(REG_VCM_NEW_CODE,
			dest_lens_position);
		usleep(sw_damping_time_wait * 50);
	}
	mt9e013_ctrl->curr_lens_pos = dest_lens_position;
	mt9e013_ctrl->curr_step_pos = dest_step_position;
	return 0;
}

static int mt9e013_set_default_focus(uint8_t af_step)
{
	int rc = 0;
	if (mt9e013_ctrl->curr_step_pos != 0)
		rc = mt9e013_move_focus(MOVE_FAR, mt9e013_ctrl->curr_step_pos);
	else
		rc = mt9e013_i2c_write_w_sensor(REG_VCM_NEW_CODE, 0x00);

	mt9e013_ctrl->curr_lens_pos = 0;
	mt9e013_ctrl->curr_step_pos = 0;

	return rc;
}

static void mt9e013_init_focus(void)
{
	uint8_t i;
	mt9e013_step_position_table[0] = 0;
	mt9e013_step_position_table[1] = mt9e013_af_initial_value;
	for (i = 2; i <= mt9e013_linear_total_step; i++) {
		if (i <= mt9e013_nl_region_boundary1) {
			mt9e013_step_position_table[i] =
				mt9e013_step_position_table[i-1]
				+ mt9e013_nl_region_code_per_step1;
		} else {
			mt9e013_step_position_table[i] =
				mt9e013_step_position_table[i-1]
				+ mt9e013_l_region_code_per_step;
		}

		if (mt9e013_step_position_table[i] > 255)
			mt9e013_step_position_table[i] = 255;
	}
}

static int mt9e013_test(enum mt9e013_test_mode_t mo)
{
	int rc = 0;
	if (mo == TEST_OFF)
		return rc;
	else {
		/* REG_0x30D8[4] is TESBYPEN: 0: Normal Operation,
		 * 1: Bypass Signal Processing
		 * REG_0x30D8[5] is EBDMASK: 0:
		 * Output Embedded data, 1: No output embedded data */
		if (mt9e013_i2c_write_b_sensor(REG_TEST_PATTERN_MODE,
			(uint8_t) mo) < 0) {
			return rc;
		}
	}
	return rc;
}

static int mt9e013_save_otp(void)
{
	int rc = -1;
	unsigned short i = 0;
	unsigned short addr = 0x3800;
	unsigned short value = 0;

	for (i = 0; i < (mt9e013_regs.reg_otp_size - 8); i++) {
		addr = 0x3800 + (2 * i);
		rc = mt9e013_i2c_read(addr, &value, 2);
		if (rc < 0)
			break;
		mt9e013_regs.reg_otp[i].wdata = value;
	}
	return rc;
}

static int mt9e013_is_otp(void)
{
	int i;
	int rc = -1;
	unsigned short value = 0;

	for(i = 0; i < 5; i++) {
    		msleep(5);
    		mt9e013_i2c_read(0x304A, &value, 2);
		if((value | 0xFF9F) == 0xffff) {
			rc = 1;
			mt9e013_save_otp();
			break;
		}
	}

	return rc;
}

static bool mt9e013_otp_valid = false;

static int mt9e013_read_otp(void)
{
	int rc = -1;

	mt9e013_i2c_write_w_sensor(0x301A, 0x0610);
	mt9e013_i2c_write_w_sensor(0x3134, 0xCD95);

	mt9e013_i2c_write_w_sensor(0x304C, 0x3500);
	mt9e013_i2c_write_w_sensor(0x304A, 0x0010);
	rc = mt9e013_is_otp();
	if (rc > 0) {
		CDBG("%s: type 35\n", __func__);
		goto otp_probe_check;
	}

	mt9e013_i2c_write_w_sensor(0x304C, 0x3400);
	mt9e013_i2c_write_w_sensor(0x304A, 0x0010);
	rc = mt9e013_is_otp();
	if (rc > 0) {
		CDBG("%s: type 34\n", __func__);
		goto otp_probe_check;
	}

	mt9e013_i2c_write_w_sensor(0x304C, 0x3300);
	mt9e013_i2c_write_w_sensor(0x304A, 0x0010);
	rc = mt9e013_is_otp();
	if (rc > 0) {
		CDBG("%s: type 33\n", __func__);
		goto otp_probe_check;
	}

	mt9e013_i2c_write_w_sensor(0x304C, 0x3200);
	mt9e013_i2c_write_w_sensor(0x304A, 0x0010);
	rc = mt9e013_is_otp();
	if (rc > 0) {
		CDBG("%s: type 32\n", __func__);
		goto otp_probe_check;
	}

	mt9e013_i2c_write_w_sensor(0x304C, 0x3100);
	mt9e013_i2c_write_w_sensor(0x304A, 0x0010);
	rc = mt9e013_is_otp();
	if(rc > 0) {
		CDBG("%s: type 31\n", __func__);
		goto otp_probe_check;
	}

	mt9e013_i2c_write_w_sensor(0x304C, 0x3000);
	mt9e013_i2c_write_w_sensor(0x304A, 0x0010);
	rc = mt9e013_is_otp();
	if (rc > 0) {
		CDBG("%s: type 30\n", __func__);
		goto otp_probe_check;
	}

	pr_err("%s: type reading failed\n", __func__);
	return -1;

otp_probe_check:
	if (mt9e013_regs.reg_otp[7].wdata != 0) {
		CDBG("The OTP reading failed addr = %0x, data = %0x\n",
			mt9e013_regs.reg_otp[7].waddr,
			mt9e013_regs.reg_otp[7].wdata);
		rc = -1;
		return rc;
	}

	if (mt9e013_regs.reg_otp[114].wdata != 0xffff) {
		CDBG("The OTP reading failed addr = %0x, data = %0x\n",
			mt9e013_regs.reg_otp[114].waddr,
			mt9e013_regs.reg_otp[114].wdata);
		rc = -1;
		return rc;
	}

	mt9e013_otp_valid = true;
	return 0;
}

static int mt9e013_shading_setting(void)
{
	int rc = 0;
	int i = 0;

	mt9e013_i2c_write_w_sensor(0x3780, 0);

	if (mt9e013_regs.reg_otp[8].wdata == 0 || !mt9e013_otp_valid) {
		CDBG("%s: Invalid shading data\n", __func__);
		for (i = 0; i < mt9e013_regs.reg_shading_size; i++) {
			rc = mt9e013_i2c_write_w_sensor(
				mt9e013_regs.reg_shading[i].waddr,
				mt9e013_regs.reg_shading[i].wdata);
		}
		mt9e013_i2c_write_w_sensor(0x3780, 0x8000);
		return rc;
	}

	for(i = 0; i < mt9e013_regs.reg_shading_size; i++) {
		rc = mt9e013_i2c_write_w_sensor(
			mt9e013_regs.reg_shading[i].waddr,
			mt9e013_regs.reg_otp[8+i].wdata);
		if(rc < 0) {
			pr_err("%s: write failed, retrying\n", __func__);
			mt9e013_i2c_write_w_sensor(
				mt9e013_regs.reg_shading[i].waddr,
				mt9e013_regs.reg_otp[8+i].wdata);
		}
	}

	mt9e013_i2c_write_w_sensor(0x3780, 0x8000);

	return rc;
}

static int mt9e013_read_awb_data(struct sensor_cfg_data *cfg)
{
	int rc = 0;

	if(mt9e013_regs.reg_otp[4].wdata == 0 || !mt9e013_otp_valid) {
		CDBG("Invalid AWB data\n");
		return -ENODEV;
	}

	cfg->cfg.calib_info.r_over_g = mt9e013_regs.reg_otp[4].wdata;
	if (mt9e013_model_id == MODULE_LITEON) {
		cfg->cfg.calib_info.gr_over_gb = mt9e013_regs.reg_otp[5].wdata;
		cfg->cfg.calib_info.b_over_g = mt9e013_regs.reg_otp[6].wdata;
	} else {
		cfg->cfg.calib_info.b_over_g = mt9e013_regs.reg_otp[5].wdata;
		cfg->cfg.calib_info.gr_over_gb = mt9e013_regs.reg_otp[6].wdata;
	}

	return rc;
}

static int mt9e013_sensor_setting(int update_type, int rt)
{
	int rc = 0;
	struct msm_camera_csi_params mt9e013_csi_params;
	uint8_t stored_af_step = 0;

	stored_af_step = mt9e013_ctrl->curr_step_pos;
	mt9e013_set_default_focus(0);
	mt9e013_stop_stream();
	msleep(15);
	if (update_type == REG_INIT) {
		mt9e013_i2c_write_w_table(mt9e013_regs.reg_mipi,
			mt9e013_regs.reg_mipi_size);
		mt9e013_i2c_write_w_table(mt9e013_regs.rec_settings,
			mt9e013_regs.rec_size);
		mt9e013_shading_setting();
		mt9e013_i2c_write_w_table(mt9e013_regs.reg_pll,
			mt9e013_regs.reg_pll_size);
		cam_debug_init();
		CSI_CONFIG = 0;
	} else if (update_type == UPDATE_PERIODIC) {
		msleep(100);
		if (rt == RES_PREVIEW) {
			mt9e013_i2c_write_w_table(mt9e013_regs.reg_prev,
				mt9e013_regs.reg_prev_size);
		} else {
			mt9e013_i2c_write_w_table(mt9e013_regs.reg_snap,
				mt9e013_regs.reg_snap_size);
		}
		if (!CSI_CONFIG) {
			msm_camio_vfe_clk_rate_set(192000000);
			mt9e013_csi_params.data_format = CSI_10BIT;
			mt9e013_csi_params.lane_cnt = 2;
			mt9e013_csi_params.lane_assign = 0xe4;
			mt9e013_csi_params.dpcm_scheme = 0;
			mt9e013_csi_params.settle_cnt = 0x18;
			rc = msm_camio_csi_config(&mt9e013_csi_params);
			msleep(10);
			CSI_CONFIG = 1;
		}
		mt9e013_start_stream();
		mt9e013_move_focus(MOVE_NEAR, stored_af_step);
	}
	return rc;
}

static int mt9e013_video_config(int mode)
{

	int rc = 0;
	int rt;
	CDBG("video config\n");

	if (mt9e013_ctrl->prev_res == QTR_SIZE)
		rt = RES_PREVIEW;
	else
		rt = RES_CAPTURE;

	if (mt9e013_sensor_setting(UPDATE_PERIODIC, rt) < 0)
		return rc;
	if (mt9e013_ctrl->set_test) {
		if (mt9e013_test(mt9e013_ctrl->set_test) < 0)
			return rc;
	}

	mt9e013_ctrl->curr_res = mt9e013_ctrl->prev_res;
	mt9e013_ctrl->sensormode = mode;
	return rc;
}

static int mt9e013_snapshot_config(int mode)
{
	int rc = 0;
	int rt;

	if (mt9e013_ctrl->curr_res != mt9e013_ctrl->pict_res) {
		if (mt9e013_ctrl->pict_res == QTR_SIZE)
			rt = RES_PREVIEW;
		else
			rt = RES_CAPTURE;
	if (mt9e013_sensor_setting(UPDATE_PERIODIC, rt) < 0)
		return rc;
	}

	mt9e013_ctrl->curr_res = mt9e013_ctrl->pict_res;
	mt9e013_ctrl->sensormode = mode;
	return rc;
}

static int mt9e013_raw_snapshot_config(int mode)
{
	int rc = 0;
	int rt;

	if (mt9e013_ctrl->curr_res != mt9e013_ctrl->pict_res) {
		if (mt9e013_ctrl->pict_res == QTR_SIZE)
			rt = RES_PREVIEW;
		else
			rt = RES_CAPTURE;
		if (mt9e013_sensor_setting(UPDATE_PERIODIC, rt) < 0)
			return rc;
	}

	mt9e013_ctrl->curr_res = mt9e013_ctrl->pict_res;
	mt9e013_ctrl->sensormode = mode;
	return rc;
}

static int mt9e013_set_sensor_mode(int mode, int res)
{
	int rc = 0;
	switch (mode) {
	case SENSOR_PREVIEW_MODE:
		rc = mt9e013_video_config(mode);
		break;
	case SENSOR_SNAPSHOT_MODE:
		rc = mt9e013_snapshot_config(mode);
		break;
	case SENSOR_RAW_SNAPSHOT_MODE:
		rc = mt9e013_raw_snapshot_config(mode);
		break;
	default:
		rc = -EINVAL;
		break;
	}
	return rc;
}

static int mt9e013_send_wb_info(struct wb_info_cfg *wb)
{
	return 0;
}

static int mt9e013_power_down(void)
{
	return 0;
}

static int mt9e013_sensor_init_done(const struct msm_camera_sensor_info *data)
{
	gpio_set_value_cansleep(data->sensor_reset, 0);
	gpio_free(data->sensor_reset);
	return 0;
}

static int mt9e013_probe_init_sensor(const struct msm_camera_sensor_info *data)
{
	int rc = 0;
	unsigned short chipid;

	rc = gpio_request(data->sensor_reset, "mt9e013");
	if (rc) {
		pr_err("%s: failed to request gpio rc=%d\n", __func__, rc);
		return rc;
	}

	gpio_direction_output(data->sensor_reset, 0);
	msleep(10);
	gpio_set_value_cansleep(data->sensor_reset, 1);
	msleep(10);

	mt9e013_read_otp();

	rc = mt9e013_i2c_read(MT9E013_REG_MODEL_ID, &chipid, 2);
	if (rc < 0) {
		pr_err("%s: failed to read model id rc=%d\n", __func__, rc);
		goto init_probe_fail;
	}

	if (chipid != MT9E013_MODEL_ID) {
		pr_err("%s: model id does not match 0x%x!=0x%x\n",
			__func__, chipid, MT9E013_MODEL_ID);
		rc = -ENODEV;
		goto init_probe_fail;
	}

	return 0;

init_probe_fail:
	mt9e013_sensor_init_done(data);
	return rc;
}

static int mt9e013_sensor_open_init(const struct msm_camera_sensor_info *data)
{
	int rc = 0;

	mt9e013_ctrl = kzalloc(sizeof(struct mt9e013_ctrl_t), GFP_KERNEL);
	if (!mt9e013_ctrl) {
		pr_err("%s: failed to allocate memory\n", __func__);
		rc = -ENOMEM;
		return rc;
	}

	mt9e013_ctrl->fps_divider = 1 * 0x00000400;
	mt9e013_ctrl->pict_fps_divider = 1 * 0x00000400;
	mt9e013_ctrl->set_test = TEST_OFF;
	mt9e013_ctrl->prev_res = QTR_SIZE;
	mt9e013_ctrl->pict_res = FULL_SIZE;

	if (data)
		mt9e013_ctrl->sensordata = data;

	msm_camio_clk_rate_set(MT9E013_MASTER_CLK_RATE);
	mdelay(20);

	rc = mt9e013_probe_init_sensor(data);
	if (rc < 0)
		goto init_fail;

	if (mt9e013_ctrl->prev_res == QTR_SIZE)
		rc = mt9e013_sensor_setting(REG_INIT, RES_PREVIEW);
	else
		rc = mt9e013_sensor_setting(REG_INIT, RES_CAPTURE);
	if (rc < 0)
		goto init_fail;

	mt9e013_ctrl->fps = 30 * Q8;
	mt9e013_init_focus();

	return 0;

init_fail:
	kfree(mt9e013_ctrl);
	mt9e013_ctrl = NULL;
	return rc;
}

static int mt9e013_init_client(struct i2c_client *client)
{
	/* Initialize the MSM_CAMI2C Chip */
	init_waitqueue_head(&mt9e013_wait_queue);
	return 0;
}

static int mt9e013_sensor_config(void __user *argp)
{
	struct sensor_cfg_data cdata;
	int rc = 0;

	if (copy_from_user(&cdata, (void *)argp, sizeof(cdata)))
		return -EFAULT;

	mutex_lock(&mt9e013_mut);

	CDBG("%s: cfgtype %d\n", __func__, cdata.cfgtype);
	switch (cdata.cfgtype) {
	case CFG_GET_PICT_FPS:
		mt9e013_get_pict_fps(cdata.cfg.gfps.prevfps,
			&(cdata.cfg.gfps.pictfps));

		if (copy_to_user((void *)argp, &cdata, sizeof(cdata)))
			rc = -EFAULT;
		break;
	case CFG_GET_PREV_L_PF:
		cdata.cfg.prevl_pf = mt9e013_get_prev_lines_pf();
		if (copy_to_user((void *)argp, &cdata, sizeof(cdata)))
			rc = -EFAULT;
		break;
	case CFG_GET_PREV_P_PL:
		cdata.cfg.prevp_pl = mt9e013_get_prev_pixels_pl();
		if (copy_to_user((void *)argp, &cdata, sizeof(cdata)))
			rc = -EFAULT;
		break;
	case CFG_GET_PICT_L_PF:
		cdata.cfg.pictl_pf = mt9e013_get_pict_lines_pf();
		if (copy_to_user((void *)argp, &cdata, sizeof(cdata)))
			rc = -EFAULT;
		break;
	case CFG_GET_PICT_P_PL:
		cdata.cfg.pictp_pl = mt9e013_get_pict_pixels_pl();
		if (copy_to_user((void *)argp, &cdata, sizeof(cdata)))
			rc = -EFAULT;
		break;
	case CFG_GET_PICT_MAX_EXP_LC:
		cdata.cfg.pict_max_exp_lc = mt9e013_get_pict_max_exp_lc();
		if (copy_to_user((void *)argp, &cdata, sizeof(cdata)))
			rc = -EFAULT;
		break;
	case CFG_SET_FPS:
	case CFG_SET_PICT_FPS:
		rc = mt9e013_set_fps(&(cdata.cfg.fps));
		break;
	case CFG_SET_EXP_GAIN:
		rc = mt9e013_write_exp_gain(cdata.cfg.exp_gain.gain,
			cdata.cfg.exp_gain.line);
		break;
	case CFG_SET_PICT_EXP_GAIN:
		rc = mt9e013_set_pict_exp_gain(cdata.cfg.exp_gain.gain,
			cdata.cfg.exp_gain.line);
		break;
	case CFG_SET_MODE:
		rc = mt9e013_set_sensor_mode(cdata.mode, cdata.rs);
		break;
	case CFG_PWR_DOWN:
		rc = mt9e013_power_down();
		break;
	case CFG_MOVE_FOCUS:
		rc = mt9e013_move_focus(cdata.cfg.focus.dir,
			cdata.cfg.focus.steps);
		break;
	case CFG_SET_DEFAULT_FOCUS:
		rc = mt9e013_set_default_focus(cdata.cfg.focus.steps);
		break;
	case CFG_GET_AF_MAX_STEPS:
		cdata.max_steps = mt9e013_linear_total_step;
		if (copy_to_user((void *)argp, &cdata, sizeof(cdata)))
			rc = -EFAULT;
		break;
	case CFG_SET_EFFECT:
		rc = mt9e013_set_default_focus(cdata.cfg.effect);
		break;
	case CFG_SEND_WB_INFO:
		rc = mt9e013_send_wb_info(&(cdata.cfg.wb_info));
		break;
	case CFG_GET_CALIB_DATA:
		rc = mt9e013_read_awb_data(&cdata);
		if (rc < 0)
			break;
		if (copy_to_user((void *)argp, &cdata, sizeof(cdata)))
			rc = -EFAULT;
		break;
	default:
		rc = -EFAULT;
		break;
	}

	mutex_unlock(&mt9e013_mut);

	return rc;
}

static int mt9e013_sensor_release(void)
{
	int rc = -EBADF;

	mutex_lock(&mt9e013_mut);

	/* Push lens to default addr. */
	mt9e013_set_default_focus(0);
	msleep(100);

	mt9e013_power_down();

	mt9e013_sensor_init_done(mt9e013_ctrl->sensordata);

	kfree(mt9e013_ctrl);
	mt9e013_ctrl = NULL;

	mutex_unlock(&mt9e013_mut);

	return rc;
}

static int mt9e013_i2c_probe(struct i2c_client *client,
	const struct i2c_device_id *id)
{
	if (!i2c_check_functionality(client->adapter, I2C_FUNC_I2C))
		return -ENOTSUPP;

	mt9e013_init_client(client);
	mt9e013_client = client;

	return 0;
}

static const struct i2c_device_id mt9e013_i2c_id[] = {
	{"mt9e013", 0},
	{ }
};

static struct i2c_driver mt9e013_i2c_driver = {
	.id_table	= mt9e013_i2c_id,
	.probe		= mt9e013_i2c_probe,
	.remove		= __exit_p(mt9v114_i2c_remove),
	.driver		= {
		.name	= "mt9e013",
	},
};

static int mt9e013_sensor_probe(const struct msm_camera_sensor_info *info,
		struct msm_sensor_ctrl *s)
{
	/* We expect this driver to match with the i2c device registered
	 * in the board file immediately. */
	int rc = i2c_add_driver(&mt9e013_i2c_driver);
	if (rc < 0 || mt9e013_client == NULL) {
		return -ENOTSUPP;
	}

	mdelay(5);
	msm_camio_clk_rate_set(MT9E013_MASTER_CLK_RATE);
	mdelay(20);

	rc = mt9e013_probe_init_sensor(info);
	if (rc < 0) {
		i2c_del_driver(&mt9e013_i2c_driver);
		pr_err("%s: mt9e013_probe_init_sensor failed rc=%d\n",
			__func__, rc);
		return rc;
	}

	/* Read model ID */
	if (mt9e013_i2c_read(0x3806, &mt9e013_model_id, 2) < 0)
		mt9e013_model_id = (mt9e013_model_id & 0xF000) >> 12;

	s->s_init = mt9e013_sensor_open_init;
	s->s_release = mt9e013_sensor_release;
	s->s_config  = mt9e013_sensor_config;
	s->s_camera_type = BACK_CAMERA_2D;
	if (info->sensor_platform_info)
		s->s_mount_angle = info->sensor_platform_info->mount_angle;
	else
		s->s_mount_angle = 0;
	mt9e013_sensor_init_done(info);

	return 0;
}

static int __mt9e013_probe(struct platform_device *pdev)
{
	return msm_camera_drv_start(pdev, mt9e013_sensor_probe);
}

static struct platform_driver msm_camera_driver = {
	.probe		= __mt9e013_probe,
	.driver		= {
		.name	= "msm_camera_mt9e013",
		.owner	= THIS_MODULE,
	},
};

static int __init mt9e013_init(void)
{
	return platform_driver_register(&msm_camera_driver);
}

module_init(mt9e013_init);

static bool streaming = 1;

static int mt9e013_set_sw_damping(void *data, u64 val)
{
	mt9e013_enable_damping = val;
	return 0;
}

static int mt9e013_get_sw_damping(void *data, u64 *val)
{
	*val = mt9e013_enable_damping;
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(af_damping, mt9e013_get_sw_damping,
	mt9e013_set_sw_damping, "%llu\n");

static int mt9e013_set_af_codestep(void *data, u64 val)
{
	mt9e013_l_region_code_per_step = val;
	mt9e013_init_focus();
	return 0;
}

static int mt9e013_get_af_codestep(void *data, u64 *val)
{
	*val = mt9e013_l_region_code_per_step;
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(af_codeperstep, mt9e013_get_af_codestep,
	mt9e013_set_af_codestep, "%llu\n");

static int mt9e013_set_af_nonlinear(void *data, u64 val)
{
	mt9e013_nl_region_code_per_step1 = val & 0xFFFF;
	mt9e013_nl_region_boundary1 = (val >> 16) & 0xFFFF;
	return 0;
}

static int mt9e013_get_af_nonlinear(void *data, u64 *val)
{
	*val = mt9e013_nl_region_code_per_step1 |
		(mt9e013_nl_region_boundary1 << 16);
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(cam_nonlinear, mt9e013_get_af_nonlinear,
	mt9e013_set_af_nonlinear, "%llu\n");

static int mt9e013_set_af_stepparam(void *data, u64 val)
{
	mt9e013_sw_damping_small_step = val & 0xFF;
	mt9e013_sw_damping_small_step_coarse = (val >> 8) & 0xFF;
	mt9e013_sw_damping_small_step_fine = (val >> 16) & 0xFF;
	mt9e013_sw_damping_small_step_snap = (val >> 24) & 0xFF;
	mt9e013_sw_damping_small_step_fwd = mt9e013_sw_damping_small_step_snap;
	printk("%s: step:%d, coarse:%d, fine:%d, snap:%d, fwd:%d\n", __func__,
		mt9e013_sw_damping_small_step, mt9e013_sw_damping_small_step_coarse,
		mt9e013_sw_damping_small_step_fine, mt9e013_sw_damping_small_step_snap,
		mt9e013_sw_damping_small_step_fwd);
	return 0;
}

static int mt9e013_get_af_stepparam(void *data, u64 *val)
{
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(cam_stepparam, mt9e013_get_af_stepparam,
	mt9e013_set_af_stepparam, "%llu\n");

static int mt9e013_set_af_timeparam(void *data, u64 val)
{
	mt9e013_sw_damping_time_wait = val & 0xFF;
	mt9e013_sw_damping_time_wait_coarse = (val >> 8) & 0xFF;
	mt9e013_sw_damping_time_wait_fine = (val >> 16) & 0xFF;
	mt9e013_sw_damping_time_wait_snap = (val >> 24) & 0xFF;
	mt9e013_sw_damping_time_wait_fwd = mt9e013_sw_damping_time_wait_snap;
	printk("%s: time:%d, coarse:%d, fine:%d, snap:%d, fwd:%d\n", __func__,
		mt9e013_sw_damping_time_wait, mt9e013_sw_damping_time_wait_coarse,
		mt9e013_sw_damping_time_wait_fine, mt9e013_sw_damping_time_wait_snap,
		mt9e013_sw_damping_time_wait_fwd);
	return 0;
}

static int mt9e013_get_af_timeparam(void *data, u64 *val)
{
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(cam_timeparam, mt9e013_get_af_timeparam,
	mt9e013_set_af_timeparam, "%llu\n");

static int mt9e013_set_linear_total_step(void *data, u64 val)
{
	mt9e013_linear_total_step = val & 0xFF;
	mt9e013_damping_threshold = (val >> 8) & 0xFF;
	mt9e013_step_jump = (val >> 16) & 0xFF;
	return 0;
}
static int mt9e013_focus_test(void *data, u64 *val)
{
	int i = 0;
	mt9e013_set_default_focus(0);

	msleep(3000);
	for (i = 0; i < mt9e013_linear_total_step; i++) {
		mt9e013_move_focus(MOVE_NEAR, 1);
		CDBG("moved to index =[%d]\n", i);
		msleep(1000);
	}

	for (i = 0; i < mt9e013_linear_total_step; i++) {
		mt9e013_move_focus(MOVE_FAR, 1);
		CDBG("moved to index =[%d]\n", i);
		msleep(1000);
	}
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(cam_focus, mt9e013_focus_test,
	mt9e013_set_linear_total_step, "%lld\n");

static int mt9e013_step_test(void *data, u64 *val)
{
	int i = 0;
	mt9e013_set_default_focus(0);

	for (i = 0; i < mt9e013_linear_total_step; i+=mt9e013_step_jump) {
		mt9e013_move_focus(MOVE_NEAR, mt9e013_step_jump);
		msleep(1000);
	}

	mt9e013_move_focus(MOVE_FAR, mt9e013_linear_total_step);
	msleep(1000);
	return 0;
}

static int mt9e013_set_af_initial(void *data, u64 val)
{
	mt9e013_af_initial_value = val & 0xFF;
	mt9e013_sw_damping_small_step_fwd = (val >> 8) & 0xFF;
	mt9e013_sw_damping_time_wait_fwd = (val >> 16) & 0xFF;
	printk("af_initial:%d, step:%d, wait:%d\n", mt9e013_af_initial_value,
		mt9e013_sw_damping_small_step_fwd, mt9e013_sw_damping_time_wait_fwd);
	return 0;
}

DEFINE_SIMPLE_ATTRIBUTE(cam_step, mt9e013_step_test,
	mt9e013_set_af_initial, "%lld\n");

static int cam_debug_stream_set(void *data, u64 val)
{
	int rc = 0;

	if (val) {
		mt9e013_start_stream();
		streaming = 1;
	} else {
		mt9e013_stop_stream();
		streaming = 0;
	}

	return rc;
}

static int cam_debug_stream_get(void *data, u64 *val)
{
	*val = streaming;
	return 0;
}
DEFINE_SIMPLE_ATTRIBUTE(cam_stream, cam_debug_stream_get,
			cam_debug_stream_set, "%llu\n");

static int cam_debug_init(void)
{
	struct dentry *cam_dir;
	debugfs_base = debugfs_create_dir("sensor", NULL);
	if (!debugfs_base)
		return -ENOMEM;

	cam_dir = debugfs_create_dir("mt9e013", debugfs_base);
	if (!cam_dir)
		return -ENOMEM;
	if (!debugfs_create_file("af_codeperstep", S_IRUGO | S_IWUSR, cam_dir,
		NULL, &af_codeperstep))
		return -ENOMEM;
	if (!debugfs_create_file("af_damping", S_IRUGO | S_IWUSR, cam_dir,
		NULL, &af_damping))
		return -ENOMEM;
	if (!debugfs_create_file("af_linear", S_IRUGO | S_IWUSR, cam_dir,
		NULL, &cam_focus))
		return -ENOMEM;
	if (!debugfs_create_file("af_ringing", S_IRUGO | S_IWUSR, cam_dir,
		NULL, &cam_step))
		return -ENOMEM;
	if (!debugfs_create_file("af_nonlinear", S_IRUGO | S_IWUSR, cam_dir,
		NULL, &cam_nonlinear))
		return -ENOMEM;
	if (!debugfs_create_file("af_stepparam", S_IRUGO | S_IWUSR, cam_dir,
		NULL, &cam_stepparam))
		return -ENOMEM;
	if (!debugfs_create_file("af_timeparam", S_IRUGO | S_IWUSR, cam_dir,
		NULL, &cam_timeparam))
		return -ENOMEM;
	if (!debugfs_create_file("stream", S_IRUGO | S_IWUSR, cam_dir,
		NULL, &cam_stream))
		return -ENOMEM;

	return 0;
}
