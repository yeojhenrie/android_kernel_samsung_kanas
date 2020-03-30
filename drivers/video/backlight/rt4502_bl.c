
/*******************************************************************************
* Copyright 2013 Samsung Electronics Corporation.  All rights reserved.
*
* 	@file	drivers/video/backlight/rt4502_bl.c
*
* Unless you and Broadcom execute a separate written software license agreement
* governing use of this software, this software is licensed to you under the
* terms of the GNU General Public License version 2, available at
* http://www.gnu.org/copyleft/gpl.html (the "GPL").
*
* Notwithstanding the above, under no circumstances may you combine this
* software in any way with any other Broadcom software provided under a license
* other than the GPL, without Broadcom's express prior written consent.
*******************************************************************************/

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/init.h>
#ifdef CONFIG_HAS_EARLYSUSPEND
#include <linux/earlysuspend.h>
#endif
#include <linux/platform_device.h>
#include <linux/fb.h>
#include <linux/backlight.h>
#include <linux/err.h>
#include <linux/rt4502_bl.h>
#include <linux/gpio.h>
#include <linux/delay.h>
#include <linux/spinlock.h>
#include <linux/rtc.h>

/*
 * The technical documentation can be found online at
 * https://www.richtek.com/Design%20Support/Technical%20Document/AN046?sc_lang=zh-TW
 * but the source is in Chinese. Fortunately, machine translations should be able to
 * give a intelligible English version of the document.
 */

#ifdef CONFIG_MACH_NEVISTD
static int backlight_pin = 138;
#else
static int backlight_pin = 214;
#endif

#if defined(CONFIG_FB_LCD_NT35502_MIPI) || defined(CONFIG_FB_LCD_HX8369B_MIPI_DTC)
extern unsigned int lpm_charge;
#endif
extern uint32_t lcd_id_from_uboot;
int current_intensity;
int real_level = 18;
EXPORT_SYMBOL(real_level);

#define MAX_BRIGHTNESS_IN_BLU	33
#define DIMMING_VALUE		31
#define MAX_BRIGHTNESS_VALUE	255
#define MIN_BRIGHTNESS_VALUE	20
#define BACKLIGHT_DEBUG 0

static enum {
	BACKLIGHT_SUSPEND = 0,
	BACKLIGHT_RESUME,
} backlight_mode = BACKLIGHT_RESUME;

#if BACKLIGHT_DEBUG
#define BLDBG(fmt, args...) printk(fmt, ## args)
#else
#define BLDBG(fmt, args...)
#endif

struct rt4502_bl_data {
	struct platform_device *pdev;
	unsigned int ctrl_pin;
#ifdef CONFIG_HAS_EARLYSUSPEND
	struct early_suspend early_suspend_desc;
#endif
};

struct brt_value {
	int level;		// Platform setting values
	int tune_level;		// Chip Setting values
};

#if defined (CONFIG_MACH_RHEA_SS_LUCAS)
struct brt_value brt_table_ktd[] = {
	{MIN_BRIGHTNESS_VALUE, 31},	// Min pulse 32
	{32, 31},
	{46, 30},
	{60, 29},
	{73, 28},
	{86, 27},
	{98, 26},
	{105, 25},
	{110, 24},
	{115, 23},
	{120, 22},
	{125, 21},
	{130, 20},
	{140, 19},		//default value
	{155, 18},
	{165, 17},
	{176, 16},
	{191, 15},
	{207, 14},
	{214, 13},
	{221, 12},
	{228, 10},
	{235, 8},
	{242, 7},
	{249, 5},
	{MAX_BRIGHTNESS_VALUE, 5},	// Max pulse 1
};
#else
struct brt_value brt_table_ktd[] = {
	{MIN_BRIGHTNESS_VALUE, 31},	// Min pulse 32
	{28, 31},
	{36, 30},
	{44, 29},
	{52, 28},
	{60, 27},
	{68, 26},
	{76, 25},
	{84, 24},
	{92, 23},
	{100, 22},
	{108, 21},
	{116, 20},
	{124, 19},
	{132, 18},
	{140, 17},
	{148, 16},		//default value
	{156, 15},
	{164, 14},
	{172, 13},
	{180, 12},
	{188, 11},
	{196, 10},
	{204, 9},
	{212, 8},
	{220, 7},
	{228, 6},
	{236, 5},
	{244, 4},
	{252, 3},
	{MAX_BRIGHTNESS_VALUE, 2},
};

struct brt_value brt_table_ktd_dtc[] = {
	{MIN_BRIGHTNESS_VALUE, 30},	// Min pulse 32
	{ 19, 30},
	{ 27, 29},
	{ 35, 28},
	{ 44, 27},
	{ 52, 26},
	{ 60, 25},
	{ 68, 24},
	{ 76, 23},
	{ 84, 22},
	{ 92, 21},
	{100, 20},
	{109, 19},
	{117, 18},
	{125, 17},
	{133, 16},
	{141, 15},		//default value
	{149, 14},
	{157, 13},
	{166, 12},
	{174, 11},
	{182, 10},
	{190, 9},
	{198, 8},
	{206, 7},
	{214, 6},
	{222, 5},
	{231, 4},
	{239, 3},
	{247, 2},
	{MAX_BRIGHTNESS_VALUE, 1},
};

#endif
#define MAX_BRT_STAGE_KTD (int)(sizeof(brt_table_ktd)/sizeof(struct brt_value))

static DEFINE_SPINLOCK(bl_ctrl_lock);

/*
 * Contrary to the name, this turns the backlight
 * driver on (1) or off (0), any invalid value is
 * coerced to on (1).
 */
void lcd_backlight_off(int num)
{
	spin_lock(&bl_ctrl_lock);

	num = !!num;
	gpio_set_value(backlight_pin, num);

	// Power on should take at least 50 us
	// Power off should take at least 1000 us
	// Slightly increase delays to ensure stability
	if (num == 1) {
		udelay(100);
	} else {
		udelay(1500);
		real_level = 0;
	}

	spin_unlock(&bl_ctrl_lock);
}

static void lcd_backlight_control(int num)
{
	BLDBG("[BACKLIGHT] lcd_backlight_control ==> pulse  : %d\n", num);
	spin_lock(&bl_ctrl_lock);
	for (; num > 0; num--) {
		udelay(10);
		gpio_set_value(backlight_pin, 0);
		udelay(10);
		gpio_set_value(backlight_pin, 1);
	}
	spin_unlock(&bl_ctrl_lock);
}

/* input: intensity in percentage 0% - 100% */
static int rt4502_backlight_update_status(struct backlight_device *bd)
{
	int user_intensity = bd->props.brightness;
	int tune_level = 0;
	int pulse;
	int i = MAX_BRT_STAGE_KTD;

	struct brt_value *table = NULL;

	printk("[BACKLIGHT] rt4502_backlight_update_status ==> user_intensity  : %d\n", user_intensity);

	if ((bd->props.power != FB_BLANK_UNBLANK) ||
	    (bd->props.fb_blank != FB_BLANK_UNBLANK) ||
	    (bd->props.state & BL_CORE_SUSPENDED))
		user_intensity = 0;

	if (backlight_mode == BACKLIGHT_SUSPEND)
		return 0;

	table = (lcd_id_from_uboot == 0x554cc0) ? brt_table_ktd : brt_table_ktd_dtc;

	if (user_intensity > 0) {
		for (; (i-- > 0) && user_intensity < table[i].level; )
			;

		tune_level = (i >= 0) ? table[i].tune_level : DIMMING_VALUE;
	} // if user_intensity == 0, but tune_level is already 0

	printk("[BACKLIGHT] rt4502_backlight_update_status ==> tune_level : %d\n", tune_level);
	if (real_level == tune_level)
		return 0;

	if (tune_level <= 0) {
		lcd_backlight_off(0);
	} else {
		if (real_level == 0) {
			lcd_backlight_off(1);
			BLDBG("[BACKLIGHT] rt4502_backlight_earlyresume -> Control Pin Enable\n");
		}

		pulse = tune_level - real_level; // Delta
		if (real_level > tune_level)
			pulse = 32 + pulse;

		//pulse = MAX_BRIGHTNESS_IN_BLU -tune_level;
		if (pulse == 0)
			return 0;

		lcd_backlight_control(pulse);
	}

	spin_lock(&bl_ctrl_lock);
	real_level = tune_level;
	spin_unlock(&bl_ctrl_lock);

	return 0;
}

static int rt4502_backlight_get_brightness(struct backlight_device *bl)
{
	BLDBG("[BACKLIGHT] rt4502_backlight_get_brightness\n");

	return current_intensity;
}

static struct backlight_ops rt4502_backlight_ops = {
	.update_status = rt4502_backlight_update_status,
	.get_brightness = rt4502_backlight_get_brightness,
};

#ifdef CONFIG_LCD_ESD_RECOVERY
struct backlight_device *bl_global;
void rt4502_backlight_on(void)
{
	rt4502_backlight_update_status(bl_global);
}

void rt4502_backlight_off(void)
{
	lcd_backlight_off(0);
}
#endif
#ifdef CONFIG_HAS_EARLYSUSPEND
static void rt4502_backlight_earlysuspend(struct early_suspend *desc)
{
	backlight_mode = BACKLIGHT_SUSPEND;

	lcd_backlight_off(0);

	printk("[BACKLIGHT] earlysuspend\n");
}

static void rt4502_backlight_earlyresume(struct early_suspend *desc)
{
	struct rt4502_bl_data *rt4502 =
	    container_of(desc, struct rt4502_bl_data,
			 early_suspend_desc);
	struct backlight_device *bl = platform_get_drvdata(rt4502->pdev);
#if defined(CONFIG_FB_LCD_NT35502_MIPI) || defined(CONFIG_FB_LCD_HX8369B_MIPI_DTC)
	if (lpm_charge == 1) {
		mdelay(250);/*fix for whitescreen in kanas in LPM charging mode*/
	} else {
		/*mdelay(120);*//*fix for whitescreen in kanas*/
	}
#endif
	backlight_mode = BACKLIGHT_RESUME;
	printk("earlyresume\n");
	backlight_update_status(bl);
}
#else
#ifdef CONFIG_PM
static int rt4502_backlight_suspend(struct platform_device *pdev,
				    pm_message_t state)
{
	struct backlight_device *bl = platform_get_drvdata(pdev);
	struct rt4502_bl_data *rt4502 = dev_get_drvdata(&bl->dev);

	BLDBG("[BACKLIGHT] rt4502_backlight_suspend, no-op\n");

	return 0;
}

static int rt4502_backlight_resume(struct platform_device *pdev)
{
	struct backlight_device *bl = platform_get_drvdata(pdev);
	BLDBG("[BACKLIGHT] rt4502_backlight_resume\n");

	backlight_update_status(bl);

	return 0;
}
#else
#define rt4502_backlight_suspend  NULL
#define rt4502_backlight_resume   NULL
#endif
#endif
static int rt4502_backlight_probe(struct platform_device *pdev)
{
	struct platform_rt4502_backlight_data *data = pdev->dev.platform_data;
	struct backlight_device *bl;
	struct rt4502_bl_data *rt4502;
	struct backlight_properties props;
	int ret;
	BLDBG("[BACKLIGHT] rt4502_backlight_probe\n");
	if (!data) {
		dev_err(&pdev->dev, "failed to find platform data\n");
		return -EINVAL;
	}
	rt4502 = kzalloc(sizeof(*rt4502), GFP_KERNEL);
	if (!rt4502) {
		dev_err(&pdev->dev, "no memory for state\n");
		ret = -ENOMEM;
		goto err_alloc;
	}
	rt4502->ctrl_pin = data->ctrl_pin;

	memset(&props, 0, sizeof(struct backlight_properties));
	props.max_brightness = data->max_brightness;
	props.type = BACKLIGHT_PLATFORM;
	bl = backlight_device_register(pdev->name, &pdev->dev,
				       rt4502, &rt4502_backlight_ops, &props);
	if (IS_ERR(bl)) {
		dev_err(&pdev->dev, "failed to register backlight\n");
		ret = PTR_ERR(bl);
		goto err_bl;
	}
#ifdef CONFIG_LCD_ESD_RECOVERY
	bl_global = bl;
#endif
#ifdef CONFIG_HAS_EARLYSUSPEND
	rt4502->pdev = pdev;
	rt4502->early_suspend_desc.level = EARLY_SUSPEND_LEVEL_BLANK_SCREEN + 1;
	rt4502->early_suspend_desc.suspend = rt4502_backlight_earlysuspend;
	rt4502->early_suspend_desc.resume = rt4502_backlight_earlyresume;
	register_early_suspend(&rt4502->early_suspend_desc);
#endif
	bl->props.max_brightness = data->max_brightness;
	bl->props.brightness = data->dft_brightness;
	platform_set_drvdata(pdev, bl);

//	rt4502_backlight_update_status(bl);

	return 0;
err_bl:
	kfree(rt4502);
err_alloc:
	return ret;
}

static int rt4502_backlight_remove(struct platform_device *pdev)
{
	struct backlight_device *bl = platform_get_drvdata(pdev);
	struct rt4502_bl_data *rt4502 = dev_get_drvdata(&bl->dev);
	backlight_device_unregister(bl);

#ifdef CONFIG_HAS_EARLYSUSPEND
	unregister_early_suspend(&rt4502->early_suspend_desc);
#endif
	kfree(rt4502);
	return 0;
}

static void rt4502_backlight_shutdown(struct platform_device *pdev)
{

	printk("[BACKLIGHT] rt4502_backlight_shutdown\n");

	lcd_backlight_off(0);
	return;
}

static struct platform_driver rt4502_backlight_driver = {
	.driver = {
		   .name = "panel",
		   .owner = THIS_MODULE,
		   },
	.probe = rt4502_backlight_probe,
	.remove = rt4502_backlight_remove,
	.shutdown = rt4502_backlight_shutdown,
#ifndef CONFIG_HAS_EARLYSUSPEND
	.suspend = rt4502_backlight_suspend,
	.resume = rt4502_backlight_resume,
#endif
};

static int __init rt4502_backlight_init(void)
{
	return platform_driver_register(&rt4502_backlight_driver);
}

module_init(rt4502_backlight_init);
static void __exit rt4502_backlight_exit(void)
{
	platform_driver_unregister(&rt4502_backlight_driver);
}

module_exit(rt4502_backlight_exit);
MODULE_DESCRIPTION("rt4502 based Backlight Driver");
MODULE_LICENSE("GPL");
MODULE_ALIAS("platform:rt4502-backlight");
