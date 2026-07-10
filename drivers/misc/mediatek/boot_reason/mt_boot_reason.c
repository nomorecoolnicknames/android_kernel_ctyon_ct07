#define pr_fmt(fmt) "["KBUILD_MODNAME"] " fmt
#include <linux/module.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/cdev.h>
#include <linux/interrupt.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/mm.h>
#include <linux/kfifo.h>

#include <linux/firmware.h>
#include <linux/syscalls.h>
#include <linux/uaccess.h>
#include <linux/platform_device.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/of.h>
#include <asm/setup.h>
#include <linux/atomic.h>
#include <mt-plat/mt_boot_reason.h>


enum {
	BOOT_REASON_UNINIT = 0,
	BOOT_REASON_INITIALIZING = 1,
	BOOT_REASON_INITIALIZED = 2,
} BOOT_REASON_STATE;

enum boot_reason_t g_boot_reason __nosavedata = BR_UNKNOWN;

static atomic_t g_br_state = ATOMIC_INIT(BOOT_REASON_UNINIT);
static atomic_t g_br_errcnt = ATOMIC_INIT(0);
static atomic_t g_br_status = ATOMIC_INIT(0);

void init_boot_reason(unsigned int line)
{
#ifdef CONFIG_OF
	struct device_node *chosen;
	const char *ptr;
	char *br_ptr;

	if (BOOT_REASON_INITIALIZING == atomic_read(&g_br_state)) {
		pr_warn("%s (%d) state(%d)\n", __func__, line, atomic_read(&g_br_state));
		atomic_inc(&g_br_errcnt);
		return;
	}

	if (BOOT_REASON_UNINIT == atomic_read(&g_br_state))
		atomic_set(&g_br_state, BOOT_REASON_INITIALIZING);
	else
		return;

	if (BR_UNKNOWN != g_boot_reason) {
		atomic_set(&g_br_state, BOOT_REASON_INITIALIZED);
		pr_warn("boot_reason = %d\n", g_boot_reason);
		return;
	}

	pr_debug("%s %d %d %d\n", __func__, line, g_boot_reason, atomic_read(&g_br_state));
	chosen = of_find_node_by_path("/chosen");
	if (!chosen)
		chosen = of_find_node_by_path("/chosen@0");
	if (!chosen) {
		pr_warn("chosen node is not found\n");
		atomic_set(&g_br_state, BOOT_REASON_UNINIT);
		return;
	}

	ptr = of_get_property(chosen, "bootargs", NULL);
	if (ptr) {
		br_ptr = strstr(ptr, "boot_reason=");
		if (br_ptr) {
			g_boot_reason = br_ptr[12] - '0';
			atomic_set(&g_br_status, 1);
		} else {
			pr_warn("'boot_reason=' is not found\n");
		}
		pr_debug("%s\n", ptr);
	} else {
		pr_warn("'bootargs' is not found\n");
	}
	of_node_put(chosen);
	atomic_set(&g_br_state, BOOT_REASON_INITIALIZED);
	pr_debug("%s %d %d %d\n", __func__, line, g_boot_reason, atomic_read(&g_br_state));
#endif
}

/* return boot reason */
enum boot_reason_t get_boot_reason(void)
{
	init_boot_reason(__LINE__);
	return g_boot_reason;
}

static int __init boot_reason_core(void)
{
	init_boot_reason(__LINE__);
	return 0;
}

static int __init boot_reason_init(void)
{
	pr_debug("boot_reason = %d, state(%d,%d,%d)", g_boot_reason,
		 atomic_read(&g_br_state), atomic_read(&g_br_errcnt), atomic_read(&g_br_status));
	return 0;
}

early_initcall(boot_reason_core);
module_init(boot_reason_init);
MODULE_DESCRIPTION("Mediatek Boot Reason Driver");
MODULE_LICENSE("GPL");
