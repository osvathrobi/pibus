#include <termios.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <linux/input.h>
#include <sys/select.h>
#include <stdint.h>
#include <errno.h>
#include <stdarg.h>

#include "keyboard.h"
#include "gpio.h"
#include "mainloop.h"
#include "ibus-send.h"
#include "ibus.h"

#define SOURCE 0
#define LENGTH 1
#define DEST 2
#define DATA 3

/* Based on PiBUS V4.01 hardware */

#define GPIO_NSLP_CTL		22
#define GPIO_PIN17_CTL		23
#define GPIO_LED_CTL		24
#define GPIO_RELAY_CTL		27


typedef enum
{
	VIDEO_SRC_BMW = 0,
	VIDEO_SRC_PI = 1,
	VIDEO_SRC_CAMERA = 2,
	VIDEO_SRC_LAST = 2
}
videoSource_t;

static struct
{
	bool have_time;
	bool have_date;
	bool playing;
	bool send_window_open;
	bool keyboard_blocked;
	bool cd_polled;
	bool bluetooth;
	bool have_camera;
	bool mk3_announce;

	uint64_t last_byte;
	int bufPos;
	unsigned char buf[64];
	int ifd;
	int radio_msgs;
	int cdc_info_tag;
	int cdc_info_interval;
	int gpio_number;
	int hw_version;

	videoSource_t videoSource;
	time_t start;

	char hhmm[24];
	char yyyymmdd[24];
}
ibus =
{
	.have_time = FALSE,
	.have_date = FALSE,
	.playing = FALSE,
	.send_window_open = FALSE,
	.keyboard_blocked = TRUE,
	.cd_polled = FALSE,
	.bluetooth = FALSE,
	.have_camera = TRUE,
	.mk3_announce = TRUE,

	.last_byte = 0,
	.bufPos = 0,
	.buf = {0,},
	.ifd = -1,
	.radio_msgs = 0,
	.cdc_info_tag = -1,
	.cdc_info_interval = 0,
	.gpio_number = 0,
	.hw_version = 0,

	.videoSource = VIDEO_SRC_BMW,
	.start = 0,

	.hhmm = {0,},
	.yyyymmdd = {0,},
};

FILE *flog;

void ibus_log(char *fmt, ...)
{
	static char buf[512];
	va_list args;
	struct timespec ts;
	int len;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	sprintf(buf, "%6.6lu ", ts.tv_sec - ibus.start);
	len = 7;

	va_start(args, fmt);
	len += vsnprintf(buf + 7, sizeof(buf) - 8, fmt, args);
	va_end(args);

	buf[sizeof(buf) - 1] = '\0';
	if (len < 0 || len > (sizeof(buf) - 1))
		len = strlen (buf);

	fwrite(buf, len, 1, flog);
}

static void power_off(void)
{
	fflush(flog);
	fclose(flog);
	flog = NULL;

	system("/bin/sync");
	sleep(1);

	if (access("/usr/sbin/poweroff", F_OK) == 0)
		system("/usr/sbin/poweroff");
	else
		system("/sbin/poweroff");
}

static void ibus_set_video(videoSource_t src)
{
	switch (src)
	{
		case VIDEO_SRC_BMW:
			gpio_write(GPIO_RELAY_CTL, 0);	/* relay off */
			gpio_write(GPIO_PIN17_CTL, 0);	/* pin17 off */
			break;

		case VIDEO_SRC_PI:
			gpio_write(GPIO_RELAY_CTL, 0);	/* relay off */
			gpio_write(GPIO_PIN17_CTL, 1);	/* pin17 on */
			break;

		case VIDEO_SRC_CAMERA:
			gpio_write(GPIO_RELAY_CTL, 1);	/* relay on */
			gpio_write(GPIO_PIN17_CTL, 1);	/* pin17 on */
			break;
	}
}

static void ibus_handle_phone(const unsigned char *msg, int length)
{
	if (ibus.hw_version >= 4 && !ibus.bluetooth)
	{
		ibus.videoSource++;
		if (ibus.videoSource > VIDEO_SRC_LAST)
		{
			ibus.videoSource = 0;
		}
		ibus_set_video(ibus.videoSource);
	}
}

static void ibus_handle_ike_sensor(const unsigned char *msg, int length)
{
	if (ibus.hw_version >= 4 && ibus.have_camera)
	{
		switch (msg[5] >> 4)
		{
			case 1:
				ibus_set_video(VIDEO_SRC_CAMERA);
				break;
			default:
				ibus_set_video(ibus.videoSource);
				break;
		}
	}
}

static bool ibus_good_checksum(const unsigned char *msg, int length)
{
	unsigned char sum;
	int i;

	sum = msg[0];
	for (i = 1; i < (length - 1); i++)
	{
		sum ^= msg[i];
	}

	if (sum != msg[length - 1])
	{
		return FALSE;
	}

	return TRUE;
}

void ibus_dump_hex(FILE *out, const unsigned char *data, int length, bool check_the_sum)
{
	int i;

	for (i = 0; i < length; i++)
	{
		fprintf(out, "%02x ", data[i]);
	}

	if (check_the_sum && (!ibus_good_checksum(data, length)))
	{
		fprintf(out, "(corrupt)\n");
	}
	else
	{
		fprintf(out, "\n");
	}
}

static void ibus_request_time(void)
{
	/* CDChanger asks IKE for Time */
	RODATA rt[] = "\x18\x05\x80\x41\x01\x01\xDC";

	ibus_send(ibus.ifd, rt, 7, ibus.gpio_number);
}

static void ibus_request_date(void)
{
	/* CDChanger asks IKE for Date */
	RODATA rd[] = "\x18\x05\x80\x41\x02\x01\xDF";

	ibus_send(ibus.ifd, rd, 7, ibus.gpio_number);
}

static void ibus_set_time_and_date(void)
{
	char buf[64];

	if (ibus.have_time && ibus.have_date)
	{
		snprintf(buf, sizeof(buf), "date -s \"%s %s\"", ibus.yyyymmdd, ibus.hhmm);
		system(buf);

		ibus_log("setting: %s\n", buf);
	}
}

static void ibus_handle_date(const unsigned char *msg, int length)
{
	if (length > 15 && !ibus.have_date)
	{
		ibus.have_date = TRUE;
		snprintf(ibus.yyyymmdd, sizeof(ibus.yyyymmdd), "%c%c%c%c-%c%c-%c%c", msg[12], msg[13], msg[14], msg[15], msg[9], msg[10], msg[6], msg[7]);
		ibus_set_time_and_date();
	}
}

static void ibus_handle_time(const unsigned char *msg, int length)
{
/*
11/5/2009 4:13:52 PM.251:  A4 05 80 41 01 01 60
11/5/2009 4:13:52 PM.251:  ACM  --> IKE : On-board computer data request: Time: current value request
11/5/2009 4:13:52 PM.281:  80 0C FF 24 01 00 20 34 3A 30 38 50 4D 6D
11/5/2009 4:13:52 PM.281:  IKE  --> LOC : Update Text:  Layout=Time  Fld0,EndTx=" 4:08PM"
11/5/2009 4:13:52 PM.291:  A4 05 80 41 02 01 63
11/5/2009 4:13:52 PM.291:  ACM  --> IKE : On-board computer data request: Date: current value request
11/5/2009 4:13:52 PM.332:  80 0F FF 24 02 00 2D 2D 2F 2D 2D 2F 32 30 30 32 56
11/5/2009 4:13:52 PM.332:  IKE  --> LOC : Update Text:  Layout=Date  Fld0,EndTx="--/--/2002"

1/26/2010 5:04:11 PM.213:  3B 05 80 41 01 01 FF
1/26/2010 5:04:11 PM.213:  GT   --> IKE : On-board computer data request: Time: current value request
1/26/2010 5:04:11 PM.233:  3B 05 80 41 02 01 FC
1/26/2010 5:04:11 PM.233:  GT   --> IKE : On-board computer data request: Date: current value request
1/26/2010 5:04:11 PM.253:  80 0C E7 24 01 00 20 35 3A 30 34 50 4D 78
1/26/2010 5:04:11 PM.253:  IKE  --> ANZV: Update Text:  Layout=Time  Fld0,EndTx=" 5:04PM"
1/26/2010 5:04:11 PM.303:  80 0F E7 24 02 00 30 31 2F 32 36 2F 32 30 31 30 48
1/26/2010 5:04:11 PM.303:  IKE  --> ANZV: Update Text:  Layout=Date  Fld0,EndTx="01/26/2010"
*/
	int hour;

	if (length > 12 && !ibus.have_time)
	{
		hour = atoi((const char *)msg + 6);

		if (msg[11] == 'P')
		{
			hour += 12;
		}

		ibus.have_time = TRUE;
		snprintf(ibus.hhmm, sizeof(ibus.hhmm), "%02d:%c%c", hour, msg[9], msg[10]);
		ibus_set_time_and_date();
	}
}

static void ibus_handle_rotary(const unsigned char *msg, int length)
{
	int i, key;

	if (length < 5 || ibus.keyboard_blocked)
	{
		return;
	}

	switch (msg[4] & 0xF0)
	{
		case 0x80:
			key = KEY_UP;
			break;

		case 0x00:
			key = KEY_DOWN;
			break;

		default:
			return;
	}

	for (i = 0; i < (msg[4] & 0x0F); i++)
	{
		keyboard_generate(key);
	}
}

static void ibus_handle_outsidekey(const unsigned char *msg, int length)
{
	ibus.keyboard_blocked = TRUE;

	if (ibus.hw_version >= 4)
	{
		ibus.videoSource = VIDEO_SRC_BMW;
		ibus_set_video(ibus.videoSource);
	}
}

static void ibus_handle_tonekey(const unsigned char *msg, int length)
{
	if (ibus.hw_version >= 4)
	{
		ibus_handle_outsidekey(msg, length);
	}
}

static void ibus_handle_screen(const unsigned char *msg, int length)
{
	if (length > 5)
	{
		ibus_log("\033[31munknown screen 0x%02X\033[m\n", msg[4]);
	}
}

static void ibus_handle_speak(const unsigned char *msg, int length)
{
	if (!ibus.keyboard_blocked && !ibus.bluetooth)
	{
		keyboard_generate(KEY_SPACE);
	}
}

static void ibus_handle_immobilized(const unsigned char *msg, int length)
{
	if (ibus.cdc_info_tag != -1)
	{
		mainloop_timeout_remove(ibus.cdc_info_tag);
		ibus.cdc_info_tag = -1;
	}
}


RODATA not_playing[]   = "\x18\x0a\x68\x39\x00\x02\x00\x01\x00\x01\x04\x45";
RODATA start_playing[] = "\x18\x0a\x68\x39\x02\x09\x00\x01\x00\x01\x04\x4c";
RODATA pause_playing[] = "\x18\x0a\x68\x39\x01\x0c\x00\x01\x00\x01\x04\x4a";

static void cdchanger_send_inforeq(void)
{
	if (ibus.playing)
	{
		/* This un-mutes the line-in */
		ibus_send(ibus.ifd, start_playing, 12, ibus.gpio_number);
	}
	else
	{
		ibus_send(ibus.ifd, not_playing, 12, ibus.gpio_number);
	}

	/* No more announcements */
	ibus.cd_polled = TRUE;
}

static int cdchanger_interval_timeout(void *unused)
{
	ibus_log("cdc interval timeout (%d s)\n", ibus.cdc_info_interval);
	cdchanger_send_inforeq();
	return 1;
}

static void cdchanger_handle_inforeq(const unsigned char *msg, int length)
{
	cdchanger_send_inforeq();

	if (ibus.cdc_info_interval > 0)
	{
		if (ibus.cdc_info_tag != -1)
		{
			mainloop_timeout_remove(ibus.cdc_info_tag);
		}

		ibus.cdc_info_tag = mainloop_timeout_add(ibus.cdc_info_interval * 1000, cdchanger_interval_timeout, NULL);
	}
}

static void cdchanger_handle_cdcmode(const unsigned char *msg, int length)
{
	ibus.keyboard_blocked = FALSE;
	ibus.playing = TRUE;

	if (ibus.hw_version >= 4)
	{
		ibus.videoSource = VIDEO_SRC_PI;
		ibus_set_video(ibus.videoSource);
	}
}

static void cdchanger_handle_stop(const unsigned char *msg, int length)
{
	ibus_send(ibus.ifd, not_playing, 12, ibus.gpio_number);
	ibus.playing = FALSE;
}

static void cdchanger_handle_pause(const unsigned char *msg, int length)
{
	ibus_send(ibus.ifd, pause_playing, 12, ibus.gpio_number);
	ibus.playing = FALSE;
}

static void cdchanger_handle_start(const unsigned char *msg, int length)
{
	ibus_send(ibus.ifd, start_playing, 12, ibus.gpio_number);
	ibus.playing = TRUE;
}

static void cdchanger_handle_diskchange(const unsigned char *msg, int length)
{
	if (length != 7 || msg[6] != (0x4b ^ msg[5]))
	{
		return;
	}

	ibus_send(ibus.ifd, start_playing, 12, ibus.gpio_number);
}

static void cdchanger_handle_poll(const unsigned char *msg, int length)
{
	RODATA cdc_im_here[] = "\x18\x04\xFF\x02\x00\xE1";

	ibus_send(ibus.ifd, cdc_im_here, 6, ibus.gpio_number);
	
	ibus.cd_polled = TRUE;
}

static bool is_cdc_message(const unsigned char *buf, int length)
{
	/* Copied from attiny code */

	if (length == 20 &&
		buf[0] == 0x68 &&
		buf[6] == 0x43 &&
		buf[13] == 0x34 &&
		buf[19] == 0x4c)
	{
		ibus_log("ibus event: \033[32m%s\033[m\n", "CDC 1-04");
		return TRUE;
	}

	if (length >= 16 &&
		buf[0] == 0x68 &&
		buf[6] == 0x54 &&
		buf[7] == 0x52 &&
		buf[8] == 0x20 &&
		buf[9] == 0x30 &&
		buf[10] == 0x34)
	{
		ibus_log("ibus event: \033[32m%s\033[m\n", "TR 04");
		return TRUE;
	}

	if (length == 25 &&
		buf[0] == 0x68 &&
		buf[15] == 0x43 &&
		buf[16] == 0x44 &&
		buf[18] == 0x31 &&
		buf[20] == 0x30 &&
		buf[21] == 0x34 &&
		buf[24] == 0x25)
	{
		ibus_log("ibus event: \033[32m%s\033[m\n", "CD 1-04");
		return TRUE;
	}

	return FALSE;
}

static const struct
{
	int match_length;
	char *ibusmsg;
	char *desc;
	char *command;
	unsigned int key;
	void (*function)(const unsigned char *msg, int length);
}
events[] =
{
	
{6, "\xF0\x05\xFF\x47\x00\x38\x75", "info", NULL, KEY_I},	
{6, "\xF0\x04\x3B\x48\x05\x82", "enter", NULL, KEY_ENTER},
{7, "\xF0\x05\xFF\x47\x00\x0F\x42", "sel", NULL, KEY_TAB},
{4, "\xF0\x04\x3B\x49", "rotary", NULL, 0, ibus_handle_rotary},
{6, "\xF0\x04\x68\x48\x40\x94", "FF", NULL, KEY_RIGHT|_CTRL_BIT},
{6, "\xF0\x04\x68\x48\x50\x84", "RR", NULL, KEY_LEFT|_CTRL_BIT},
{6, "\xF0\x04\x68\x48\x11\xC5", "1", NULL, KEY_ESC},
{6, "\xF0\x04\x68\x48\x01\xD5", "2", NULL, KEY_SPACE},
{6, "\xF0\x04\x68\x48\x12\xC6", "3", NULL, KEY_Z},
{6, "\xF0\x04\x68\x48\x02\xD6", "4", NULL, KEY_X},
{6, "\xF0\x04\x68\x48\x13\xC7", "5", NULL, KEY_LEFT},
{6, "\xF0\x04\x68\x48\x03\xD7", "6", NULL, KEY_RIGHT}, 

{6, "\xF0\x04\x68\x48\x10\xC4", "cd-prev", NULL, KEY_COMMA, cdchanger_handle_start},
{6, "\xF0\x04\x68\x48\x00\xD4", "cd-next", NULL, KEY_DOT, cdchanger_handle_start},
// steering wheel
{6, "\x50\x04\x68\x3B\x08\x0F", "cd-prev", NULL, KEY_COMMA, cdchanger_handle_start},
{6, "\x50\x04\x68\x3B\x01\x06", "cd-next", NULL, KEY_DOT, cdchanger_handle_start}, 

{4, "\x80\x06\xBF\x19", "coolant-temp", NULL, 0, ibus_handle_coolant_temp},
{4, "\x80\x09\xFF\x24", "fuel-consumption", NULL, 0, ibus_handle_fc},
{4, "\x80\x0A\xFF\x24", "outside-temp", NULL, 0, ibus_handle_outside_temp},
{4, "\x7F\x20\x3F\xA0", "battery-voltage", NULL, 0, ibus_handle_battery_voltage},
{5, "\x7F\x03\x3F\xA1\xE2", "re-battery-voltage", NULL, 0, ibus_request_battery_voltage2},

#if 0
	/* The most common CDC message */
	{20,"\x68\x12\x3b\x23\x62\x10\x43\x44\x43\x20\x31\x2d\x30\x34\x20\x20\x20\x20\x20\x4c", "CDC 1-04", NULL, 0, cdchanger_handle_cdcmode},

	/* This one was seen on an Adelaide M3 (onefifty370) */
	{20,"\x68\x12\x3b\x23\x62\x10\x54\x52\x20\x30\x34\x20\x20\x20\x20\x20\x20\x20\x20\x32", "TR 04L", NULL, 0, cdchanger_handle_cdcmode},

	/* This one was seen on a Lithuanian M3 (realvtk) */
	{16,"\x68\x0e\x3b\x23\x62\x10\x54\x52\x20\x30\x34\x20\x20\x20\x20\x2e", "TR 04S", NULL, 0, cdchanger_handle_cdcmode},

	/* This one was seen on a German E39 525i 05/2001 MK3 BM24 (DK) */
	{25,"\x68\x17\x3b\x23\x62\x30\x20\x20\x07\x20\x20\x20\x20\x20\x08\x43\x44\x20\x31\x2d\x30\x34\x20\x20\x25", "CD 1-04", NULL, 0, cdchanger_handle_cdcmode},
#endif
};


static void ibus_handle_message(const unsigned char *msg, int length)
{
	int i;

	ibus_log("");
	ibus_dump_hex(flog, msg, length, TRUE);

	/* are we entering the CDC screen? */
	if (is_cdc_message(msg, length))
	{
		cdchanger_handle_cdcmode(msg, length);
	}

	/* got a message from the radio */
	if (msg[0] == 0x68)
	{
		ibus.radio_msgs++;
	}

	for (i = 0; i < sizeof(events) / sizeof(events[0]); i++)
	{
		if (events[i].match_length > length)
		{
			continue;
		}

		if (memcmp(msg, events[i].ibusmsg, events[i].match_length) == 0)
		{
			if (events[i].key && !ibus.keyboard_blocked)
			{
				keyboard_generate(events[i].key);
			}

			ibus_log("ibus event: \033[32m%s\033[m\n", events[i].desc);

			if (events[i].command != NULL)
			{
				system(events[i].command);
			}

			if (events[i].function != NULL)
			{
				events[i].function(msg, length);
			}

			return;
		}
	}

	ibus_remove_from_queue(msg, length);
}

static void ibus_read(int condition, void *unused)
{
	unsigned char c;
	uint64_t now = mainloop_get_millisec();
	int r;

	while (1)
	{
		if ((r = read(ibus.ifd, &c, 1)) != 1)
		{
			if (r == -1)
			{
				int e = errno;
				if (e != EWOULDBLOCK)
				{
					printf("ifd=%d e=%d %s\n", ibus.ifd, e, strerror(e));
					exit(1);
				}
			}
			return;
		}

		if (now - ibus.last_byte > 64)
		{
			ibus.bufPos = 0;
		}
		ibus.last_byte = now;

		ibus.buf[ibus.bufPos] = c;
		if (ibus.bufPos < (sizeof(ibus.buf) - 1))
		{
			ibus.bufPos++;
		}

		ibus.send_window_open = FALSE;

		if (ibus.bufPos >= 4 && ibus.buf[LENGTH] + 2 == ibus.bufPos)
		{
			ibus_handle_message(ibus.buf, ibus.bufPos);
			ibus.bufPos = 0;
		}
	}

}

/*
	When the I-Bus wakes up, the CD player starts to announce it-self ("02 01" msg) every 30 secondes 
	until the radio poll ("01"). At the first poll, the CD will send a poll response ("02 00"), 
	then will respond to each next poll (every 30 secondes).
	If the CD doesn't respond to the poll, the radio considers that there is no CD Player (or not anymore).
*/
static void announce_cdc()
{
	if (!ibus.cd_polled)
	{
		/* If the radio is silent, don't do this announcement */
		if (ibus.radio_msgs != 0)
		{
			RODATA cdc_announce[] = "\x18\x04\xFF\x02\x01\xE0";
			ibus_send(ibus.ifd, cdc_announce, 6, ibus.gpio_number);
			ibus.radio_msgs = 0;
		}
	}
}

/* every 50ms */

static int ibus_tick(void *unused)
{
	static int i = 0;
	static int j = 0;

	i++;
	if (i >= 20)
	{
		i = 0;
		/* 5 minute idle timeout */
		if (mainloop_get_millisec() - ibus.last_byte > 300000)
		{
			ibus_log("idle timeout\n");
			power_off();
		}
	}

	if (ibus.hw_version >= 4)
	{
		if (i < 2)
		{
			gpio_write(GPIO_LED_CTL, 1);
		}
		else
		{
			gpio_write(GPIO_LED_CTL, 0);
		}
	}

	j++;
	if (j >= 600)
	{
		j = 0;
		/* flush log & announce CD-changer every 30s */
		fflush(flog);
		if (ibus.mk3_announce)
		{
			announce_cdc();
		}

	}

	/* every 15s */
	if (j == 0 || j == 300)
	{
		if (!ibus.have_time)
		{
			ibus_request_time();
		}
		if (!ibus.have_date)
		{
			ibus_request_date();
		}
	}

	if (ibus.gpio_number > 0)
	{
		ibus_service_queue(ibus.ifd, ibus.send_window_open, ibus.gpio_number);

		/* If >50ms (2 ticks) has passed without receiving any bytes,
		 * we have an opportunity to transmit (bus is quiet).
		 */

		if (!ibus.send_window_open && ibus.bufPos == 0)
		{
			ibus.send_window_open = TRUE;
		}
	}

	return 1;
}

static void ibus_send_ascii(const char *cmd)
{
	char byte[4];
	unsigned char data[64];
	int len = strlen(cmd);
	int i, j;

	if (len >= (sizeof(data) * 2))
	{
		return;
	}

	for (i = 0, j = 0; i < len; i += 2, j++)
	{
		byte[0] = cmd[i];
		byte[1] = cmd[i+1];
		byte[2] = 0;
		data[j] = strtoul(byte, NULL, 16);
	}

	ibus_send(ibus.ifd, data, j, ibus.gpio_number);
	fflush(flog);
}

int ibus_init(const char *port, char *startup, bool bluetooth, bool camera, bool mk3, int cdc_info_interval, int gpio_number, int hw_version)
{
	struct termios newtio;
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	ibus.start = ts.tv_sec;

	ibus.ifd = open(port, O_RDWR | O_NOCTTY);
	if (ibus.ifd == -1)
	{
		fprintf(stderr, "Can't open ibus [%s] %s\n", port, strerror(errno));
		return -1;
	}

	memset(&newtio, 0, sizeof(newtio)); /* clear struct for new port settings */
 	newtio.c_cflag = B9600 | CS8 | CLOCAL | CREAD | PARENB;
	newtio.c_iflag = IGNPAR | IGNBRK;
	newtio.c_oflag = 0;
	newtio.c_lflag = 0;

	newtio.c_cc[VTIME] = 0;   /* inter-character timer unused */
	newtio.c_cc[VMIN] = 0;    /* !blocking read until 1 chars received */

	tcflush(ibus.ifd, TCIFLUSH);
	tcsetattr(ibus.ifd, TCSANOW, &newtio);

#ifdef __i386__
	flog = fopen("./ibus.txt", "a");
#else
	flog = fopen("/storage/ibus.txt", "a");
#endif
	if (flog == NULL)
	{
		fprintf(stderr, "Cannot write to log: %s\n", strerror(errno));
		close(ibus.ifd);
		ibus.ifd = -1;
		return -2;
	}

	ibus_log("startup bt=%d cam=%d mk3=%d cdci=%d gpio=%d hwv=%d [" __DATE__ "]\n", bluetooth, camera, mk3, cdc_info_interval, gpio_number, hw_version);
	fflush(flog);

	ibus.last_byte = mainloop_get_millisec();
	ibus.bluetooth = bluetooth;
	ibus.have_camera = camera;
	ibus.mk3_announce = mk3;
	ibus.cdc_info_interval = cdc_info_interval;
	ibus.gpio_number = gpio_number;
	ibus.hw_version = hw_version;

	mainloop_input_add(ibus.ifd, FIA_READ, ibus_read, NULL);
	mainloop_timeout_add(50, ibus_tick, NULL);

	/* gpio 15 is the UART RX, don't change its direction. */
	if (gpio_number != 15 && gpio_number != 0)
	{
		/* The IBUS monitor pin must be an input (should already be) */
		gpio_set_input(gpio_number);

		if (hw_version >= 4)
		{
			gpio_set_pull(gpio_number, PULL_UP);
		}
	}

	if (hw_version >= 4)
	{
		gpio_write(GPIO_NSLP_CTL, 1);	/* Wake up the transceiver */
		gpio_write(GPIO_PIN17_CTL, 0);
		gpio_write(GPIO_LED_CTL, 1);
		gpio_write(GPIO_RELAY_CTL, 0);

		gpio_set_output(GPIO_NSLP_CTL);
		gpio_set_output(GPIO_PIN17_CTL);
		gpio_set_output(GPIO_LED_CTL);
		gpio_set_output(GPIO_RELAY_CTL);
	}
	else if (bluetooth || (!camera))
	{
		unsigned char set[] = "\xd7\x04\xd8\x70\x00\x00";

		if (bluetooth)
		{
			/* Tell the ATtiny to ignore the Phone button */
			set[4] |= 1;
		}

		if (!camera)
		{
			/* Tell the ATtiny to ignore reverse gear */
			set[4] |= 2;
		}

		set[5] = set[0] ^ set[1] ^ set[2] ^ set[3] ^ set[4];
		ibus_send(ibus.ifd, set, 6, ibus.gpio_number);
	}

	if (startup)
	{
		ibus_send_ascii(startup);
		free(startup);
	}

	return 0;
}

void ibus_cleanup(void)
{
	/*if (ibus.ifd != -1)
	{
		close(ibus.ifd);
		ibus.ifd = -1;
	}*/
}

