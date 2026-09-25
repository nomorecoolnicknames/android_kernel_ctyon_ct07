#include <linux/delay.h>
#include <linux/err.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_platform.h>
#include <linux/pinctrl/consumer.h>
#include <linux/platform_device.h>
#include <linux/reboot.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>
#include <linux/workqueue.h>

#include <mt_spi.h>

#include "ct07_lcm_spi.h"

#define CT07_LCM_SPI_BUS 0
#define CT07_LCM_SPI_CS 0
#define CT07_LCM_SPI_HZ 48000000

struct ct07_lcm_spi_ctx {
	struct spi_device *spi;
	struct pinctrl *pinctrl;
	struct pinctrl_state *lcd_cs_mode;
	struct pinctrl_state *lcd_clk_mode;
	struct pinctrl_state *lcd_data_mode;
	struct pinctrl_state *lcd_rs_low;
	struct pinctrl_state *lcd_rs_high;
};

static DEFINE_MUTEX(ct07_lcm_spi_lock);
/*
 * A register table (panel init / sleep) and a window write (CASET, RASET,
 * RAMWR, pixels) must not interleave: the frame pusher and the LCM
 * suspend/resume path run in different threads, and a command landing
 * between RAMWR and its data would be taken as pixels or the other way
 * round. ct07_lcm_spi_lock only covers single transfers.
 */
static DEFINE_MUTEX(ct07_lcm_spi_seq_lock);
/* bumped after every panel (re)initialisation: its GRAM content is unknown then */
unsigned int ct07_lcm_spi_epoch;
EXPORT_SYMBOL_GPL(ct07_lcm_spi_epoch);
/*
 * Commands and register parameters come from the stack or from const
 * tables at any byte address, but mt_spi DMA wants a 4-byte aligned TX
 * buffer ("mt-spi: Warning! Tx_DMA address should be 4Byte alignment,
 * buf:db2f7eef" on every frame otherwise). Short transfers go
 * through this bounce buffer; frames are already 64-byte aligned.
 */
#define CT07_LCM_SPI_BOUNCE 64
static u8 ct07_lcm_spi_bounce[CT07_LCM_SPI_BOUNCE] __aligned(L1_CACHE_BYTES);
static struct ct07_lcm_spi_ctx *ct07_lcm_spi;
#ifdef CONFIG_CT07_BRINGUP
static const char *ct07_lcm_diag_stage_name = "pre_init";
#endif
static struct mt_chip_conf ct07_lcm_spi_conf = {
	.setuptime = 2,
	.holdtime = 2,
	.high_time = 1,
	.low_time = 1,
	.cs_idletime = 2,
	.ulthgh_thrsh = 0,
	.sample_sel = POSEDGE,
	.cs_pol = ACTIVE_LOW,
	.cpol = SPI_CPOL_0,
	.cpha = SPI_CPHA_0,
	.tx_mlsb = SPI_MSB,
	.rx_mlsb = SPI_MSB,
	.tx_endian = SPI_LENDIAN,
	.rx_endian = SPI_LENDIAN,
	/* CT07: stock lcm_spi chip conf (c0dbe0b8): DMA + pause. FIFO mode
	 * rejects anything over 32 bytes (spi.c:753), i.e. every frame. */
	.com_mod = DMA_TRANSFER,
	.pause = PAUSE_MODE_ENABLE,
	.finish_intr = FINISH_INTR_EN,
	.deassert = DEASSERT_DISABLE,
	.ulthigh = ULTRA_HIGH_DISABLE,
	.tckdly = TICK_DLY0,
};

static struct spi_board_info ct07_lcm_spi_board_info[] __initdata = {
	{
		.modalias = "lcm_spi",
		.bus_num = CT07_LCM_SPI_BUS,
		.chip_select = CT07_LCM_SPI_CS,
		.max_speed_hz = CT07_LCM_SPI_HZ,
		.mode = SPI_MODE_0,
		.controller_data = &ct07_lcm_spi_conf,
	},
};

void ct07_lcm_diag_stage(const char *stage)
{
#ifdef CONFIG_CT07_BRINGUP
	ct07_lcm_diag_stage_name = stage ? stage : "null";
	pr_notice("[CT07_DIAG] stage=%s\n", ct07_lcm_diag_stage_name);
#endif
}
EXPORT_SYMBOL_GPL(ct07_lcm_diag_stage);

#ifdef CONFIG_CT07_BRINGUP
static int ct07_lcm_reboot_notify(struct notifier_block *nb,
				  unsigned long action, void *data)
{
	pr_emerg("[CT07_DIAG] rb act=%lu cmd=%s st=%s spi=%d\n",
		 action, data ? (char *)data : "-", ct07_lcm_diag_stage_name,
		 ct07_lcm_spi ? 1 : 0);
	return NOTIFY_DONE;
}

static struct notifier_block ct07_lcm_reboot_nb = {
	.notifier_call = ct07_lcm_reboot_notify,
};

static void ct07_lcm_diag_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(ct07_lcm_diag_work, ct07_lcm_diag_workfn);

static void ct07_lcm_diag_workfn(struct work_struct *work)
{
	pr_notice("[CT07_DIAG] heartbeat st=%s spi=%d\n",
		  ct07_lcm_diag_stage_name, ct07_lcm_spi ? 1 : 0);
	schedule_delayed_work(&ct07_lcm_diag_work, 5 * HZ);
}
#endif

static int ct07_lcm_spi_lookup_state(struct device *dev,
				     struct pinctrl *pinctrl,
				     struct pinctrl_state **state,
				     const char *name)
{
	*state = pinctrl_lookup_state(pinctrl, name);
	if (IS_ERR(*state)) {
		ct07_lcm_diag_stage("spi_pinctrl_state_missing");
		dev_err(dev, "[CT07_LCM_SPI] missing pinctrl state %s: %ld\n",
			name, PTR_ERR(*state));
		return PTR_ERR(*state);
	}

	return 0;
}

static struct pinctrl *ct07_lcm_spi_get_pinctrl(struct device *dev)
{
	struct device_node *node;
	struct platform_device *pdev;
	struct pinctrl *pinctrl;

	node = of_find_compatible_node(NULL, NULL, "mediatek,lcm_mode");
	if (!node) {
		ct07_lcm_diag_stage("spi_lcm_mode_missing");
		dev_err(dev, "[CT07_LCM_SPI] mediatek,lcm_mode node missing\n");
		return ERR_PTR(-ENODEV);
	}

	pdev = of_find_device_by_node(node);
	of_node_put(node);
	if (!pdev) {
		ct07_lcm_diag_stage("spi_lcm_pdev_missing");
		dev_err(dev, "[CT07_LCM_SPI] lcm_mode platform device missing\n");
		return ERR_PTR(-EPROBE_DEFER);
	}

	pinctrl = devm_pinctrl_get(&pdev->dev);
	put_device(&pdev->dev);
	if (IS_ERR(pinctrl)) {
		ct07_lcm_diag_stage("spi_pinctrl_get_failed");
		dev_err(dev, "[CT07_LCM_SPI] devm_pinctrl_get failed: %ld\n",
			PTR_ERR(pinctrl));
	}

	return pinctrl;
}

static int ct07_lcm_spi_select(struct ct07_lcm_spi_ctx *ctx,
			       struct pinctrl_state *state)
{
	if (!ctx || !ctx->pinctrl || IS_ERR(state))
		return -ENODEV;

	return pinctrl_select_state(ctx->pinctrl, state);
}

static int ct07_lcm_spi_xfer(const unsigned char *data, unsigned int len,
				     bool rs_high)
{
	struct spi_transfer xfer = {
		.tx_buf = data,
		.len = len,
		.speed_hz = CT07_LCM_SPI_HZ,
		.bits_per_word = 8,
	};
	struct spi_message msg;
	struct ct07_lcm_spi_ctx *ctx;
	int ret;

	if (!data || !len)
		return 0;

	mutex_lock(&ct07_lcm_spi_lock);
	ctx = ct07_lcm_spi;
	if (!ctx || !ctx->spi) {
		mutex_unlock(&ct07_lcm_spi_lock);
		ct07_lcm_diag_stage("spi_xfer_before_probe");
		pr_err("[CT07_LCM_SPI] transfer before probe\n");
		return -ENODEV;
	}

	ret = ct07_lcm_spi_select(ctx,
				  rs_high ? ctx->lcd_rs_high : ctx->lcd_rs_low);
	if (ret) {
		ct07_lcm_diag_stage("spi_rs_select_failed");
		dev_err(&ctx->spi->dev, "[CT07_LCM_SPI] rs select failed: %d\n", ret);
		mutex_unlock(&ct07_lcm_spi_lock);
		return ret;
	}

	if (((unsigned long)data & 3) && len <= CT07_LCM_SPI_BOUNCE) {
		memcpy(ct07_lcm_spi_bounce, data, len);
		xfer.tx_buf = ct07_lcm_spi_bounce;
	}

	spi_message_init(&msg);
	spi_message_add_tail(&xfer, &msg);
	ret = spi_sync(ctx->spi, &msg);
	if (ret) {
		ct07_lcm_diag_stage("spi_sync_failed");
		dev_err(&ctx->spi->dev, "[CT07_LCM_SPI] spi_sync failed: %d\n", ret);
	}

	mutex_unlock(&ct07_lcm_spi_lock);
	return ret;
}

int ct07_lcm_spi_send_cmd(unsigned int cmd)
{
	unsigned char value = cmd & 0xff;

	return ct07_lcm_spi_xfer(&value, 1, false);
}
EXPORT_SYMBOL_GPL(ct07_lcm_spi_send_cmd);

int ct07_lcm_spi_send_data(const unsigned char *data, unsigned int len)
{
	return ct07_lcm_spi_xfer(data, len, true);
}
EXPORT_SYMBOL_GPL(ct07_lcm_spi_send_data);

/* Stock SpiSendData (c03c0588): RAMWR, the whole RGB565 frame, DISPON. */
int ct07_lcm_spi_send_frame(const unsigned char *buf, unsigned int len)
{
	int ret = ct07_lcm_spi_send_cmd(0x2c);

	if (!ret)
		ret = ct07_lcm_spi_send_data(buf, len);
	ct07_lcm_spi_send_cmd(0x29);
	return ret;
}
EXPORT_SYMBOL_GPL(ct07_lcm_spi_send_frame);

void ct07_lcm_spi_seq_begin(void)
{
	mutex_lock(&ct07_lcm_spi_seq_lock);
}
EXPORT_SYMBOL_GPL(ct07_lcm_spi_seq_begin);

void ct07_lcm_spi_seq_end(void)
{
	mutex_unlock(&ct07_lcm_spi_seq_lock);
}
EXPORT_SYMBOL_GPL(ct07_lcm_spi_seq_end);

/*
 * After a reset + init the GRAM holds no picture, so the panel stays
 * display-off and the backlight stays held (ct07_bl_hold) until the first
 * whole frame is written; then DISPON, then the backlight. If no frame
 * comes, the fallback work turns both on after CT07_LCM_DISPON_WAIT_MS.
 */
#define CT07_LCM_DISPON_WAIT_MS 500
static bool ct07_lcm_spi_dispon;	/* DISPON sent since the last init */
static void ct07_lcm_spi_dispon_workfn(struct work_struct *work);
static DECLARE_DELAYED_WORK(ct07_lcm_spi_dispon_work, ct07_lcm_spi_dispon_workfn);

/*
 * DISPON takes effect at the next panel frame; until then the panel still
 * inserts the display-off blank page, white on this normally-white LCD
 * (a very short white flash on unlock). The init sets
 * B1h rtni = 18 clocks/line and 66h = 0x9a (fosc >= 575 kHz), i.e. a panel
 * frame of about 10 ms or less; LK waits 10 ms after 0x29. Wait three.
 */
#define CT07_LCM_DISPON_SETTLE_MS 30

/* sequence lock held */
static void ct07_lcm_spi_dispon_locked(void)
{
	if (ct07_lcm_spi_dispon)
		return;
	ct07_lcm_spi_send_cmd(0x29);
	ct07_lcm_spi_dispon = true;
	msleep(CT07_LCM_DISPON_SETTLE_MS);
	pr_info("[CT07_LCM_SPI] display on, backlight released\n");
	ct07_bl_hold(0);
}

static void ct07_lcm_spi_dispon_workfn(struct work_struct *work)
{
	ct07_lcm_spi_seq_begin();
	if (!ct07_lcm_spi_dispon)
		pr_notice("[CT07_LCM_SPI] no frame %d ms after panel init, display on anyway\n",
			  CT07_LCM_DISPON_WAIT_MS);
	ct07_lcm_spi_dispon_locked();
	ct07_lcm_spi_seq_end();
}

/* the panel was reset and initialised without DISPON (lcm resume) */
void ct07_lcm_spi_panel_reset(void)
{
	ct07_lcm_spi_seq_begin();
	ct07_lcm_spi_dispon = false;
	ct07_bl_hold(1);
	ct07_lcm_spi_epoch++;	/* the frame pusher resends the whole frame */
	ct07_lcm_spi_seq_end();
	mod_delayed_work(system_wq, &ct07_lcm_spi_dispon_work,
			 msecs_to_jiffies(CT07_LCM_DISPON_WAIT_MS));
}
EXPORT_SYMBOL_GPL(ct07_lcm_spi_panel_reset);

/*
 * Window write (DCS CASET/RASET/RAMWR) of rows y0..y0+rows-1 of a
 * byte-swapped RGB565 frame. rows * width * 2 must stay a multiple of
 * 1024 above 1 KiB (mt_spi DMA packet rule). DISPON follows the first
 * whole frame after boot or a panel init, not every frame as in stock.
 */
int ct07_lcm_spi_send_rows(const unsigned char *buf, unsigned int y0,
			   unsigned int rows, unsigned int width, bool whole)
{
	unsigned int x1 = width - 1, y1 = y0 + rows - 1;
	unsigned char win[4];
	int ret;

	ct07_lcm_spi_seq_begin();
	win[0] = 0; win[1] = 0; win[2] = x1 >> 8; win[3] = x1 & 0xff;
	ct07_lcm_spi_send_cmd(0x2a);
	ct07_lcm_spi_send_data(win, 4);
	win[0] = y0 >> 8; win[1] = y0 & 0xff; win[2] = y1 >> 8; win[3] = y1 & 0xff;
	ct07_lcm_spi_send_cmd(0x2b);
	ct07_lcm_spi_send_data(win, 4);
	ret = ct07_lcm_spi_send_cmd(0x2c);
	if (!ret)
		ret = ct07_lcm_spi_send_data(buf + y0 * width * 2, rows * width * 2);
	if (!ret && whole)
		ct07_lcm_spi_dispon_locked();
	ct07_lcm_spi_seq_end();
	return ret;
}
EXPORT_SYMBOL_GPL(ct07_lcm_spi_send_rows);

static int ct07_lcm_spi_probe(struct spi_device *spi)
{
	struct ct07_lcm_spi_ctx *ctx;
	int ret;

	ct07_lcm_diag_stage("spi_probe");

	ctx = devm_kzalloc(&spi->dev, sizeof(*ctx), GFP_KERNEL);
	if (!ctx) {
		ct07_lcm_diag_stage("spi_probe_nomem");
		return -ENOMEM;
	}

	ctx->spi = spi;
	ctx->pinctrl = ct07_lcm_spi_get_pinctrl(&spi->dev);
	if (IS_ERR(ctx->pinctrl))
		return PTR_ERR(ctx->pinctrl);

	ret = ct07_lcm_spi_lookup_state(&spi->dev, ctx->pinctrl,
					&ctx->lcd_cs_mode, "lcd_cs_mode");
	if (ret)
		return ret;
	ret = ct07_lcm_spi_lookup_state(&spi->dev, ctx->pinctrl,
					&ctx->lcd_clk_mode, "lcd_clk_mode");
	if (ret)
		return ret;
	ret = ct07_lcm_spi_lookup_state(&spi->dev, ctx->pinctrl,
					&ctx->lcd_data_mode, "lcd_data_mode");
	if (ret)
		return ret;
	ret = ct07_lcm_spi_lookup_state(&spi->dev, ctx->pinctrl,
					&ctx->lcd_rs_low, "lcd_rs_low");
	if (ret)
		return ret;
	ret = ct07_lcm_spi_lookup_state(&spi->dev, ctx->pinctrl,
					&ctx->lcd_rs_high, "lcd_rs_high");
	if (ret)
		return ret;

	ret = ct07_lcm_spi_select(ctx, ctx->lcd_cs_mode);
	if (ret) {
		dev_err(&spi->dev, "[CT07_LCM_SPI] cs mode select failed: %d\n", ret);
		return ret;
	}
	ret = ct07_lcm_spi_select(ctx, ctx->lcd_clk_mode);
	if (ret) {
		dev_err(&spi->dev, "[CT07_LCM_SPI] clk mode select failed: %d\n", ret);
		return ret;
	}
	ret = ct07_lcm_spi_select(ctx, ctx->lcd_data_mode);
	if (ret) {
		dev_err(&spi->dev, "[CT07_LCM_SPI] data mode select failed: %d\n", ret);
		return ret;
	}

	spi->controller_data = &ct07_lcm_spi_conf;
	spi->mode = SPI_MODE_0;
	spi->bits_per_word = 8;
	spi->max_speed_hz = CT07_LCM_SPI_HZ;

	ret = spi_setup(spi);
	if (ret) {
		ct07_lcm_diag_stage("spi_setup_failed");
		dev_err(&spi->dev, "[CT07_LCM_SPI] spi_setup failed: %d\n", ret);
		return ret;
	}

	mutex_lock(&ct07_lcm_spi_lock);
	ct07_lcm_spi = ctx;
	mutex_unlock(&ct07_lcm_spi_lock);

	spi_set_drvdata(spi, ctx);
	ct07_lcm_diag_stage("spi_probe_ok");
	dev_notice(&spi->dev, "[CT07_LCM_SPI] probe ok bus=%d cs=%d hz=%d\n",
		   CT07_LCM_SPI_BUS, CT07_LCM_SPI_CS, CT07_LCM_SPI_HZ);
	return 0;
}

static int ct07_lcm_spi_remove(struct spi_device *spi)
{
	mutex_lock(&ct07_lcm_spi_lock);
	if (ct07_lcm_spi == spi_get_drvdata(spi))
		ct07_lcm_spi = NULL;
	mutex_unlock(&ct07_lcm_spi_lock);
	return 0;
}

static const struct spi_device_id ct07_lcm_spi_id[] = {
	{ "lcm_spi", 0 },
	{ }
};

static struct spi_driver ct07_lcm_spi_driver = {
	.driver = {
		.name = "lcm_spi",
		.bus = &spi_bus_type,
		.owner = THIS_MODULE,
	},
	.probe = ct07_lcm_spi_probe,
	.remove = ct07_lcm_spi_remove,
	.id_table = ct07_lcm_spi_id,
};

static int __init ct07_lcm_spi_init(void)
{
	int ret;

	ct07_lcm_diag_stage("spi_initcall");
#ifdef CONFIG_CT07_BRINGUP
	register_reboot_notifier(&ct07_lcm_reboot_nb);
	schedule_delayed_work(&ct07_lcm_diag_work, 5 * HZ);
#endif
	spi_register_board_info(ct07_lcm_spi_board_info,
				ARRAY_SIZE(ct07_lcm_spi_board_info));
	ct07_lcm_diag_stage("spi_board_info");
	ret = spi_register_driver(&ct07_lcm_spi_driver);
	if (ret)
		ct07_lcm_diag_stage("spi_driver_reg_failed");
	else
		ct07_lcm_diag_stage("spi_driver_registered");
	return ret;
}

subsys_initcall(ct07_lcm_spi_init);

MODULE_DESCRIPTION("CT07 NV3029 LCM SPI transport");
MODULE_LICENSE("GPL");
