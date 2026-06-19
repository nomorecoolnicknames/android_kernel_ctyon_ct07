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
static struct ct07_lcm_spi_ctx *ct07_lcm_spi;
static const char *ct07_lcm_diag_stage_name = "pre_init";
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
	.com_mod = FIFO_TRANSFER,
	.pause = PAUSE_MODE_DISABLE,
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
	ct07_lcm_diag_stage_name = stage ? stage : "null";
	pr_notice("[CT07_DIAG] stage=%s\n", ct07_lcm_diag_stage_name);
}
EXPORT_SYMBOL_GPL(ct07_lcm_diag_stage);

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
			     struct pinctrl_state *rs_state)
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

	ret = ct07_lcm_spi_select(ctx, rs_state);
	if (ret) {
		mutex_unlock(&ct07_lcm_spi_lock);
		ct07_lcm_diag_stage("spi_rs_select_failed");
		dev_err(&ctx->spi->dev, "[CT07_LCM_SPI] rs select failed: %d\n", ret);
		return ret;
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
	struct ct07_lcm_spi_ctx *ctx = ct07_lcm_spi;

	if (!ctx) {
		ct07_lcm_diag_stage("spi_cmd_no_ctx");
		return -ENODEV;
	}

	return ct07_lcm_spi_xfer(&value, 1, ctx->lcd_rs_low);
}
EXPORT_SYMBOL_GPL(ct07_lcm_spi_send_cmd);

int ct07_lcm_spi_send_data(const unsigned char *data, unsigned int len)
{
	struct ct07_lcm_spi_ctx *ctx = ct07_lcm_spi;

	if (!ctx) {
		ct07_lcm_diag_stage("spi_data_no_ctx");
		return -ENODEV;
	}

	return ct07_lcm_spi_xfer(data, len, ctx->lcd_rs_high);
}
EXPORT_SYMBOL_GPL(ct07_lcm_spi_send_data);

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

	ct07_lcm_spi_select(ctx, ctx->lcd_cs_mode);
	ct07_lcm_spi_select(ctx, ctx->lcd_clk_mode);
	ct07_lcm_spi_select(ctx, ctx->lcd_data_mode);

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
	register_reboot_notifier(&ct07_lcm_reboot_nb);
	schedule_delayed_work(&ct07_lcm_diag_work, 5 * HZ);
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
