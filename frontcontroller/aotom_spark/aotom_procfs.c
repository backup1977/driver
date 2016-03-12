/*
 * aotom_procfs.c
 *
 * (c) ? Gustav Gans
 * (c) 2014 skl
 * (c) 2015 Audioniek
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
 *
 *
 * procfs for Fulan front panel driver.
 *
 *
 ****************************************************************************
 *
 * Changes
 *
 * Date     By              Description
 * --------------------------------------------------------------------------
 * 20151116 Audioniek       Initial version based on tffpprocfs.c and code
 *                          by skl.
 * 20151202 Audioniek       Progress display only done once, after reaching
 *                          99% set inhibit flag.
 * 
 ****************************************************************************/

#include <linux/proc_fs.h>      /* proc fs */
#include <asm/uaccess.h>        /* copy_from_user */
#include <linux/time.h>
#include <linux/kernel.h>
#include "aotom_main.h"

/*
 *  /proc/------
 *             |
 *             +--- progress (rw)           Progress of E2 startup in %
 *
 *  /proc/stb/fp
 *             |
 *             +--- aotom (w)               Direct control of LEDs and icons
 *             +--- version (r)             SW version of front processor (hundreds = major, ten/units = minor)
 *             +--- rtc (rw)                RTC time (local, seconds since Unix epoch))
 *             +--- rtc_offset (rw)         RTC offset in seconds from UTC (not used)
 *             +--- wakeup_time (rw)        Next wakeup time (absolute, local, seconds since Unix epoch)
 *             +--- was_timer_wakeup (r)    Wakeup reason (1 = timer, 0 = other)
 *             +--- led0_pattern (rw)       Blink pattern for red LED (currently limited to on (0xffffffff) or off (0))
 *             +--- led1_pattern (rw)       Blink pattern for green LED (currently limited to on (0xffffffff) or off (0))
 *             +--- led_patternspeed (rw)   Blink speed for pattern (not implemented)
 *             +--- oled_brightness (w)     Direct control of display brightness ((D)VFD only)
 *             +--- displaytype (r)         Type of display (0=LED, 1=VFD, 2=DVFD)
 *             +--- timemode (rw)           Time display on/off (write on DVFD only)
 *             +--- text (w)                Direct writing of display text
 *
 *  /proc/stb/power/
 *             |
 *             +--- standbyled (w)          Standby LED enable
 */

//paramDebug: debug print level is zero as default (0=nothing, 1= open/close functions, 5=some detail, 10=all)
#define TAGDEBUG "[aotom] "
#define dprintk(level, x...) do { \
		if ((paramDebug) && (paramDebug > level)) printk(TAGDEBUG x); \
	} while (0)

/* from e2procfs */
extern int install_e2_procs(char *name, read_proc_t *read_proc, write_proc_t *write_proc, void *data);
extern int remove_e2_procs(char *name, read_proc_t *read_proc, write_proc_t *write_proc);

/* from other aotom modules */
extern int aotomSetIcon(int which, int on);
extern void flashLED(int led, int ms);
extern void VFD_set_all_icons(int onoff);
extern int aotomWriteText(char *buf, size_t len);
extern int aotomSetBrightness(int level);
extern void clear_display(void);

/* Globals */
static int rtc_offset = 3600; //TODO: set this automatically
static u32 wakeup_time;
static int progress = 0;
static int progress_done = 0;
static u32 led0_pattern = 0;
static u32 led1_pattern = 0;
static int led_pattern_speed = 20;


int aotomEnableLed(int which, int on)
{
	int res = 0;

	if (which < 0 || which >= LASTLED)
	{
		dprintk(5, "LED number out of range %d\n", which);
		return -EINVAL;
	}
	led_state[which].enable = on;
	return res;
}

static int text_write(struct file *file, const char __user *buf, unsigned long count, void *data)
{
	char *page;
	int ret = -ENOMEM;

	page = (char *)__get_free_page(GFP_KERNEL);

	if (page)
	{
		ret = -EFAULT;
		if (copy_from_user(page, buf, count) == 0)
		{
			ret = aotomWriteText(page, count);
			if (ret >= 0)
			{
				ret = count;
			}
		}
		free_page((unsigned long)page);
	}
	return ret;
}

static int progress_write(struct file *file, const char __user *buf, unsigned long count, void *data)
{
	char* page;
	char* myString;
	ssize_t ret = -ENOMEM;

	page = (char*)__get_free_page(GFP_KERNEL);

	if (page)
	{
		ret = -EFAULT;

		if (copy_from_user(page, buf, count))
		{
			goto out;
		}

		myString = (char*) kmalloc(count + 1, GFP_KERNEL);
		strncpy(myString, page, count);
		myString[count - 1] = '\0';

		sscanf(myString, "%d", &progress);
		kfree(myString);

		if (progress > 98 && progress_done == 0)
		{
			progress_done = 1;
			clear_display();
			if (fp_type == FP_VFD) //VFD display
			{
				led_state[LED_SPINNER].state = 0;
//				aotomSetIcon(ICON_SPINNER, 0);
			}
			ret = YWPANEL_width;
			goto out;
		}
		if (progress_done == 0 && fp_type == FP_VFD && led_state[LED_SPINNER].state == 0) //VFD display
		{
			led_state[LED_SPINNER].state = 1;
			flashLED(LED_SPINNER, 200); // start spinner thread
//			aotomSetIcon(ICON_SPINNER, 1);
		}
		if (progress > 19 && progress < 99 && progress_done == 0)
		{
			if (fp_type == FP_LED) //LED display
			{
				ret = aotomWriteText(page, count);
			}
			else
			{
				myString = (char*)kmalloc(count + ((fp_type == FP_VFD) ? 6 : 8), GFP_KERNEL);
				if (myString)
				{
					if (fp_type == FP_VFD) //VFD display
					{
						strcpy(myString, "Start ");
						strncat(myString, page, count);
						ret = aotomWriteText(myString, (count + 6) < 8 ? (count + 6) : 8);
					}
					else
					{
						strcpy(myString, "Start: ");
						strncat(myString, page, count - 2);
						strcat(myString, "%");
						ret = aotomWriteText(myString, (count + 8) < 10 ? (count + 8) : 10);
					}
					kfree(myString);
				}
			}
		}
//		else
//		{
//			clear_display();
//			ret = YWPANEL_width;
//		}
		
		if (ret >= 0)
		{
			ret = count;
		}
	}
out:
	free_page((unsigned long)page);
	return ret;
}

static int progress_read(char *page, char **start, off_t off, int count, int *eof, void *data)
{
	int len = 0;

	if (NULL != page)
	{
		len = sprintf(page,"%d", progress);
	}
	return len;
}

// Proc for accessing quick control of aotom (by skl)
// String format: fxy
// f is "l" (for led) or "i" (for icons)
// x is 0/1 and indicates if the led/icon must be off or on
// y is the led/icon number (between 0 and LASTLED-1 for leds; between 1 and 46, for icons, with y==46, all the icons are set)
static int aotom_write(struct file *file, const char __user *buf, unsigned long count, void *data)
{
	char *page;
	int which, on;
	int ret = -ENOMEM;

	page = (char *)__get_free_page(GFP_KERNEL);
	if (page)
	{
		ret = -EFAULT;

		if (copy_from_user(page, buf, count) == 0)
		{
			page[count - 1] = '\0';

			if (count > 3)
			{
				which = (int)simple_strtol(page + 2, NULL, 10);
				on = (page[1] == '0') ? 0 : 1;

				switch (page[0])
				{
					case 'l':
					{
						if ((which >= 0) && (which < LASTLED))
						{
							YWPANEL_FP_SetLed(which, on);
						}
						break;
					}
					case 'i':
					{
						if (which == ICON_ALL)
						{
							VFD_set_all_icons(on);
						}
						else if ((fp_type == FP_VFD && ((which >= ICON_FIRST) && (which <= ICON_SPINNER)))
						|| (fp_type == FP_DVFD && ((which >= ICON_SPINNER) && (which <= DICON_LAST))))
						{
							aotomSetIcon(which, on);
						}
						break;
					}
				}
			}
			ret = count;
		}
		free_page((unsigned long)page);
	}
	dprintk(10, "%s <\n", __func__);

	return ret;
}

static int read_rtc(char *page, char **start, off_t off, int count, int *eof, void *data)
{
	int len = 0;
	u32 rtc_time = YWPANEL_FP_GetTime(); // UTC

	if (NULL != page)
	{
		len = sprintf(page,"%u\n", rtc_time - rtc_offset);
	}
	return len;
}

static int write_rtc(struct file *file, const char __user *buffer, unsigned long count, void *data)
{
	char *page = NULL;
	ssize_t ret = -ENOMEM;
	u32 argument = 0;
	int test = -1;
	char *myString = kmalloc(count + 1, GFP_KERNEL);

	page = (char *)__get_free_page(GFP_KERNEL);

	if (page)
	{
		ret = -EFAULT;

		if (copy_from_user(page, buffer, count))
		{
			goto out;
		}
		strncpy(myString, page, count);
		myString[count] = '\0';
		dprintk(5, "%s > %s\n", __func__, myString);

		test = sscanf (myString, "%u", &argument);

		if (0 < test)
		{
			YWPANEL_FP_SetTime(argument + rtc_offset);
			YWPANEL_FP_ControlTimer(true);
		}
		/* always return count to avoid endless loop */
		ret = count;
	}
out:
	free_page((unsigned long)page);
	kfree(myString);
	dprintk(10, "%s <\n", __func__);
	return ret;
}

static int read_rtc_offset(char *page, char **start, off_t off, int count, int *eof, void *data)
{
	int len = 0;

	if (NULL != page)
	{
		len = sprintf(page, "%d\n", rtc_offset);
	}
	return len;
}

static int write_rtc_offset(struct file *file, const char __user *buffer, unsigned long count, void *data)
{
	char *page = NULL;
	ssize_t ret = -ENOMEM;
	int test = -1;
	char *myString = kmalloc(count + 1, GFP_KERNEL);

	page = (char *)__get_free_page(GFP_KERNEL);

	if (page)
	{
		ret = -EFAULT;
		if (copy_from_user(page, buffer, count))
		{
			goto out;
		}
		strncpy(myString, page, count);
		myString[count] = '\0';

		test = sscanf (myString, "%d", &rtc_offset);

		/* always return count to avoid endless loop */
		ret = count;
	}
out:
	free_page((unsigned long)page);
	kfree(myString);
	dprintk(10, "%s <\n", __func__);
	return ret;
}

static int wakeup_time_write(struct file *file, const char __user *buffer, unsigned long count, void *data)
{
	char *page = NULL;
	ssize_t ret = -ENOMEM;
	int test = -1;
	char *myString = kmalloc(count + 1, GFP_KERNEL);

	page = (char *)__get_free_page(GFP_KERNEL);

	if (page)
	{
		ret = -EFAULT;

		if (copy_from_user(page, buffer, count))
		{
			goto out;
		}
		strncpy(myString, page, count);
		myString[count] = '\0';
		dprintk(5, "%s > %s\n", __func__, myString);

		test = sscanf(myString, "%u", &wakeup_time);

		if (0 < test)
		{
			YWPANEL_FP_SetPowerOnTime(wakeup_time + rtc_offset);
//			YWPANEL_FP_SetPowerOnTime(wakeup_time); //RTC is in local time
		}
		/* always return count to avoid endless loop */
		ret = count;
	}
out:
	free_page((unsigned long)page);
	kfree(myString);
	dprintk(10, "%s <\n", __func__);
	return ret;
}

static int wakeup_time_read(char *page, char **start, off_t off, int count, int *eof, void *data)
{
	int len = 0;

	if (NULL != page)
	{
		wakeup_time = YWPANEL_FP_GetPowerOnTime();
		len = sprintf(page, "%u\n", wakeup_time - rtc_offset);
//		len = sprintf(page, "%u\n", wakeup_time); //RTC uses local time?
	}
	return len;
}

static int was_timer_wakeup_read(char *page, char **start, off_t off, int count, int *eof, void *data)
{
	int len = 0;
	int res = 0;
	YWPANEL_STARTUPSTATE_t State;

	if (NULL != page)
	{
		YWPANEL_FP_GetStartUpState(&State);
		if (State == YWPANEL_STARTUPSTATE_TIMER)
		{
			res = 1;
		}
		len = sprintf(page,"%d\n", res);
	}
	return len;
}

static int fp_version_read(char *page, char **start, off_t off, int count, int *eof, void *data_unused)
{
	int len = 0;
	int version;
	YWPANEL_Version_t fpanel_version;

	memset(&fpanel_version, 0, sizeof(YWPANEL_Version_t));

	if (YWPANEL_FP_GetVersion(&fpanel_version))
	{
		version = (int)(fpanel_version.swMajorVersion * 100 + fpanel_version.swSubVersion);
	}
	else
	{
		return -EFAULT;
	}

	len = sprintf(page, "%d\n", version);
	return len;
}

static int led_pattern_write(struct file *file, const char __user *buf, unsigned long count, void *data, int which)
{
	char *page;
	long pattern;
	int ret = -ENOMEM;

	page = (char *)__get_free_page(GFP_KERNEL);

	if (page)
	{
		ret = -EFAULT;

		if (copy_from_user(page, buf, count) == 0)
		{
			page[count - 1] = '\0';
			pattern = simple_strtol(page, NULL, 16);

			if (which == 1)
			{
				led1_pattern = pattern;
			}
			else
			{
				led0_pattern = pattern;
			}

//TODO: Not implemented completely; only the cases 0 and 0xffffffff (ever off/on) are handled
//Other patterns are blink patternd in time, so handling those should be done in a timer

			if (pattern == 0)
			{
				YWPANEL_FP_SetLed(which, 0);
			}
			else if (pattern == 0xffffffff)
			{
				YWPANEL_FP_SetLed(which, 1);
			}
			ret = count;
		}
		free_page((unsigned long)page);
	}
	return ret;
}

static int led0_pattern_read(char *page, char **start, off_t off, int count, int *eof, void *data_unused)
{
	int len = 0;

	if (NULL != page)
	{
		len = sprintf(page, "%08x\n", led0_pattern);
	}
	return len;
}

static int led0_pattern_write(struct file *file, const char __user *buf, unsigned long count, void *data)
{
	return led_pattern_write(file, buf, count, data, 0);
}

static int led1_pattern_read(char *page, char **start, off_t off, int count, int *eof, void *data_unused)
{
	int len = 0;

	if (NULL != page)
	{
		len = sprintf(page, "%08x\n", led1_pattern);
	}
	return len;
}
static int led1_pattern_write(struct file *file, const char __user *buf, unsigned long count, void *data)
{
	return led_pattern_write(file, buf, count, data, 1);
}

static int led_pattern_speed_write(struct file *file, const char __user *buf, unsigned long count, void *data)
{
	char *page;
	int ret = -ENOMEM;

	page = (char *)__get_free_page(GFP_KERNEL);

	if (page)
	{
		ret = -EFAULT;

		if (copy_from_user(page, buf, count) == 0)
		{
			page[count - 1] = '\0';
			led_pattern_speed = (int)simple_strtol(page, NULL, 10);

			ret = count;
		}
		free_page((unsigned long)page);
	}
	return ret;
}

static int led_pattern_speed_read(char *page, char **start, off_t off, int count, int *eof, void *data_unused)
{
	int len = 0;

	if (NULL != page)
	{
		len = sprintf(page, "%d\n", led_pattern_speed);
	}
	return len;
}

static int oled_brightness_write(struct file *file, const char __user *buf, unsigned long count, void *data)
{
	char *page;
	long level;
	int ret = -ENOMEM;

	page = (char *)__get_free_page(GFP_KERNEL);

	if (page)
	{
		ret = -EFAULT;

		if (copy_from_user(page, buf, count) == 0)
		{
			page[count - 1] = '\0';

			level = simple_strtol(page, NULL, 10);
			aotomSetBrightness((int)level);
			ret = count;
		}
		free_page((unsigned long)page);
	}
	return ret;
}

static int power_standbyled_write(struct file *file, const char __user *buf, unsigned long count, void *data)
{
	char *page;
	int ret = -ENOMEM;

	page = (char *)__get_free_page(GFP_KERNEL);

	if (page)
	{
		ret = -EFAULT;

		if (copy_from_user(page, buf, count) == 0)
		{
			page[count - 1] = '\0';

			if (strcmp(page, "on") == 0)
			{
				aotomEnableLed(0, 1);
			}
			else if (strcmp(page, "off") == 0)
			{
				aotomEnableLed(0, 0);
			}
			ret = count;
		}
		free_page((unsigned long)page);
	}
	return ret;
}

static int displaytype_read(char *page, char **start, off_t off, int count, int *eof, void *data)
{
	int len = 0;

	if (NULL != page)
	{
		len = sprintf(page,"%d\n", fp_type);
	}
	return len;
}

static int timemode_read(char *page, char **start, off_t off, int count, int *eof, void *data)
{
	int len = 0;
	int Mode;

	if (NULL != page)
	{
		if ((!bTimeMode && fp_type == FP_DVFD) || (fp_type == FP_LED))
		{
			Mode = 0;
		}
		else
		{
			Mode = 1;
		}
		len = sprintf(page,"%d\n", Mode);
	}
	return len;
}

static int timemode_write(struct file *file, const char __user *buf, unsigned long count, void *data)
{
	char *page;
	int ret = -ENOMEM;
	int Mode;

	page = (char *)__get_free_page(GFP_KERNEL);

	if (page)
	{
		ret = -EFAULT;

		if (copy_from_user(page, buf, count) == 0)
		{
			page[count - 1] = '\0';

			if (fp_type == FP_DVFD)
			{
				if (simple_strtol(page, NULL, 10) != 0)
				{
					Mode = 1;
				}
				else
				{
					Mode = 0;
				}
				YWPANEL_FP_DvfdSetTimeMode(Mode);
			}
			ret = count;
		}
		free_page((unsigned long)page);
	}
	return ret;
}

//static int null_write(struct file *file, const char __user *buf, unsigned long count, void *data)
//{
//	return count;
//}

struct fp_procs
{
	char *name;
	read_proc_t *read_proc;
	write_proc_t *write_proc;
} fp_procs[] =
{
	{ "progress", progress_read, progress_write },
	{ "stb/fp/rtc", read_rtc, write_rtc },
	{ "stb/fp/rtc_offset", read_rtc_offset, write_rtc_offset },
	{ "stb/fp/aotom", NULL, aotom_write },
	{ "stb/fp/displaytype", displaytype_read, NULL },
	{ "stb/fp/led0_pattern", led0_pattern_read, led0_pattern_write },
	{ "stb/fp/led1_pattern", led1_pattern_read, led1_pattern_write },
	{ "stb/fp/led_pattern_speed", led_pattern_speed_read, led_pattern_speed_write },
	{ "stb/fp/oled_brightness", NULL, oled_brightness_write },
	{ "stb/fp/text", NULL, text_write },
	{ "stb/fp/timemode", timemode_read, timemode_write },
	{ "stb/fp/wakeup_time", wakeup_time_read, wakeup_time_write },
	{ "stb/fp/was_timer_wakeup", was_timer_wakeup_read, NULL },
	{ "stb/fp/version", fp_version_read, NULL },
	{ "stb/power/standbyled", NULL, power_standbyled_write },
};

void create_proc_fp(void)
{
	int i;

	for (i = 0; i < sizeof(fp_procs)/sizeof(fp_procs[0]); i++)
	{
		install_e2_procs(fp_procs[i].name, fp_procs[i].read_proc, fp_procs[i].write_proc, NULL);
	}
}

void remove_proc_fp(void)
{
	int i;

	for (i = sizeof(fp_procs)/sizeof(fp_procs[0]) - 1; i >= 0; i--)
	{
		remove_e2_procs(fp_procs[i].name, fp_procs[i].read_proc, fp_procs[i].write_proc);
	}
}

// vim:ts=4

