#include <linux/kernel.h>
#include <linux/string.h>

#include "ct07_lcm_spi.h"
#include "lcm_drv.h"

#define FRAME_WIDTH  240
#define FRAME_HEIGHT 320

#define REGFLAG_DELAY        0xfe
#define REGFLAG_END_OF_TABLE 0xff

struct lcm_setting_table {
	unsigned char cmd;
	unsigned char count;
	unsigned char para_list[16];
};

static LCM_UTIL_FUNCS lcm_util;

#define SET_RESET_PIN(v) (lcm_util.set_reset_pin((v)))
#define MDELAY(n)        (lcm_util.mdelay(n))

static const struct lcm_setting_table lcm_initialization_setting[] = {
	{0xfd, 2, {0x06, 0x07}},
	{0x66, 1, {0x80}},
	{0x80, 1, {0x05}},
	{0x80, 1, {0x01}},
	{0xb6, 2, {0x02, 0xa2}},
	{0x60, 1, {0x26}},
	{0x63, 1, {0x08}},
	{0x64, 1, {0x0c}},
	{0x68, 1, {0x70}},
	{0x69, 1, {0x1b}},
	{0x6a, 1, {0xc4}},
	{0x6b, 1, {0x0e}},
	{0x6c, 1, {0x18}},
	{0x6d, 1, {0x77}},
	{0x6e, 1, {0x84}},
	{0x6f, 1, {0x48}},
	{0xf7, 1, {0x10}},
	{0x70, 1, {0x44}},
	{0x71, 1, {0x05}},
	{0xed, 6, {0xf9, 0xf9, 0x00, 0x00, 0x11, 0x00}},
	{0xe0, 7, {0x0e, 0x17, 0x13, 0x1b, 0x07, 0x14, 0x19}},
	{0xe1, 2, {0x28, 0x59}},
	{0xe2, 6, {0x1b, 0x25, 0x26, 0x11, 0x09, 0x17}},
	{0xe3, 7, {0x0d, 0x17, 0x05, 0x0f, 0x06, 0x10, 0x07}},
	{0xe4, 2, {0x0f, 0x52}},
	{0xe5, 6, {0x17, 0x26, 0x1f, 0x19, 0x19, 0x1f}},
	{0x66, 1, {0x9a}},
	{0x67, 1, {0x07}},
	{0xb1, 2, {0x00, 0x12}},
	{0xec, 6, {0x33, 0x16, 0x16, 0x00, 0x18, 0x18}},
	{0xf6, 3, {0x01, 0x10, 0x00}},
	{0xfd, 2, {0xfa, 0xfb}},
	{0x11, 0, {0x00}},
	{REGFLAG_DELAY, 200, {0x00}},
	{0x36, 1, {0x08}},
	{0x3a, 1, {0x65}},
	{0x29, 0, {0x00}},
	{REGFLAG_END_OF_TABLE, 0, {0x00}},
};

static const struct lcm_setting_table lcm_suspend_setting[] = {
	{0x28, 1, {0x00}},
	{REGFLAG_DELAY, 120, {0x00}},
	{0x10, 1, {0x00}},
	{REGFLAG_DELAY, 20, {0x00}},
	{REGFLAG_END_OF_TABLE, 0, {0x00}},
};

static void push_table(const struct lcm_setting_table *table,
		       unsigned int count)
{
	unsigned int i;
	int ret;

	for (i = 0; i < count; i++) {
		switch (table[i].cmd) {
		case REGFLAG_DELAY:
			MDELAY(table[i].count);
			break;
		case REGFLAG_END_OF_TABLE:
			return;
		default:
			ret = ct07_lcm_spi_send_cmd(table[i].cmd);
			if (ret) {
				ct07_lcm_diag_stage("nv3029_cmd_fail");
				pr_err("[CT07_LCM] cmd 0x%02x failed: %d\n",
				       table[i].cmd, ret);
			}
			if (table[i].count) {
				ret = ct07_lcm_spi_send_data(table[i].para_list,
							     table[i].count);
				if (ret) {
					ct07_lcm_diag_stage("nv3029_data_fail");
					pr_err("[CT07_LCM] data for 0x%02x failed: %d\n",
					       table[i].cmd, ret);
				}
			}
			break;
		}
	}
}

static void send_ctrl_cmd(unsigned int cmd)
{
	ct07_lcm_spi_send_cmd(cmd);
}

static void send_data_bytes(const unsigned char *data, unsigned int len)
{
	ct07_lcm_spi_send_data(data, len);
}

static void lcm_set_util_funcs(const LCM_UTIL_FUNCS *util)
{
	memcpy(&lcm_util, util, sizeof(LCM_UTIL_FUNCS));
}

static void lcm_get_params(LCM_PARAMS *params)
{
	ct07_lcm_diag_stage("nv3029_get_params");
	memset(params, 0, sizeof(LCM_PARAMS));

	params->type = LCM_TYPE_DSI;
	params->width = FRAME_WIDTH;
	params->height = FRAME_HEIGHT;
	params->dsi.mode = BURST_VDO_MODE;
	params->dsi.LANE_NUM = LCM_ONE_LANE;
	params->dsi.data_format.color_order = LCM_COLOR_ORDER_RGB;
	params->dsi.data_format.trans_seq = LCM_DSI_TRANS_SEQ_MSB_FIRST;
	params->dsi.data_format.padding = LCM_DSI_PADDING_ON_LSB;
	params->dsi.data_format.format = LCM_DSI_FORMAT_RGB888;
	params->dsi.PS = LCM_PACKED_PS_24BIT_RGB888;
	params->dsi.word_count = 0x870;
	params->dsi.packet_size = 0x100;
	params->dsi.vertical_sync_active = 8;
	params->dsi.vertical_backporch = 0x14;
	params->dsi.vertical_frontporch = 0x19;
	params->dsi.vertical_active_line = FRAME_HEIGHT;
	params->dsi.horizontal_sync_active = 0x0a;
	params->dsi.horizontal_backporch = 0x28;
	params->dsi.horizontal_frontporch = 0x54;
	params->dsi.horizontal_active_pixel = FRAME_WIDTH;
	params->dsi.PLL_CLOCK = 0x5a;
	params->dsi.dsi_clock = 0x5a;
	params->dsi.lcm_esd_check_table[0].cmd = 0x0a;
	params->dsi.lcm_esd_check_table[0].count = 1;
	params->dsi.lcm_esd_check_table[0].para_list[0] = 0x9c;

	pr_notice("[CT07_LCM] nv3029 get_params stock-spi v1\n");
}

static void lcm_init(void)
{
	ct07_lcm_diag_stage("nv3029_init_start");
	pr_notice("[CT07_LCM] nv3029 init start\n");
	SET_RESET_PIN(1);
	MDELAY(20);
	SET_RESET_PIN(0);
	MDELAY(20);
	SET_RESET_PIN(1);
	MDELAY(100);

	push_table(lcm_initialization_setting,
		   ARRAY_SIZE(lcm_initialization_setting));
	ct07_lcm_diag_stage("nv3029_init_done");
	pr_notice("[CT07_LCM] nv3029 init done\n");
}

static void lcm_suspend(void)
{
	ct07_lcm_diag_stage("nv3029_suspend");
	pr_notice("[CT07_LCM] nv3029 suspend\n");
	push_table(lcm_suspend_setting, ARRAY_SIZE(lcm_suspend_setting));
}

static void lcm_resume(void)
{
	ct07_lcm_diag_stage("nv3029_resume");
	pr_notice("[CT07_LCM] nv3029 resume\n");
	lcm_init();
}

static void lcm_update(unsigned int x, unsigned int y,
		       unsigned int width, unsigned int height)
{
	unsigned int x0 = x;
	unsigned int y0 = y;
	unsigned int x1 = x0 + width - 1;
	unsigned int y1 = y0 + height - 1;
	unsigned char data[4];

	ct07_lcm_diag_stage("nv3029_update");

	data[0] = (x0 >> 8) & 0xff;
	data[1] = x0 & 0xff;
	data[2] = (x1 >> 8) & 0xff;
	data[3] = x1 & 0xff;
	send_ctrl_cmd(0x2a);
	send_data_bytes(data, sizeof(data));

	data[0] = (y0 >> 8) & 0xff;
	data[1] = y0 & 0xff;
	data[2] = (y1 >> 8) & 0xff;
	data[3] = y1 & 0xff;
	send_ctrl_cmd(0x2b);
	send_data_bytes(data, sizeof(data));

	send_ctrl_cmd(0x2c);
}

static unsigned int lcm_compare_id(void)
{
	ct07_lcm_diag_stage("nv3029_compare_id");
	pr_notice("[CT07_LCM] nv3029 compare_id accept LK-selected panel\n");
	return 1;
}

LCM_DRIVER nv3029g_tianyifu_c5030_boe_spi_qvga_gai_lcm_drv = {
	.name = "NV3029G_TIANYIFU_C5030_BOE_SPI_QVGA_GAI",
	.set_util_funcs = lcm_set_util_funcs,
	.get_params = lcm_get_params,
	.init = lcm_init,
	.suspend = lcm_suspend,
	.resume = lcm_resume,
	.update = lcm_update,
	.compare_id = lcm_compare_id,
};
