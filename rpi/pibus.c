/*
 * pibus Copyright (c) 2013,2014 Peter Zelezny
 * All Rights Reserved
 */

#include <unistd.h>
#include <stdio.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

#include "keyboard.h"
#include "mainloop.h"
#include "ibus.h"
#include "gpio.h"



int main(int argc, char **argv)
{
	int opt;
	bool bluetooth = FALSE;
	bool mk3 = TRUE;
	bool camera = TRUE;
	int hw_version = 0;
	int gpio_number = 18;
	const char *port = "/dev/ttyAMA0";
	char *startup = NULL;
	int cdcinterval = 0;
	bool gpio_changed = FALSE;

	mainloop_init();

	while ((opt = getopt(argc, argv, "c:g:s:v:bhmr")) != -1)
	{
		switch (opt)
		{
			case 'b':
				bluetooth = 1;
				break;
			case 'c':
				cdcinterval = atoi(optarg);
				break;
			case 'g':
				gpio_number = atoi(optarg);
				gpio_changed = TRUE;
				break;
			case 'm':
				mk3 = 0;
				break;
			case 'r':
				camera = 0;
				break;
			case 's':
				startup = strdup(optarg);
				break;
			case 'v':
				hw_version = atoi(optarg);
				break;
			case 'h':
			default:
				fprintf(stderr,
					"Usage: %s [flags] [serial-port]\n"
					"\n"
					"Flags:\n"
					"\t-b           Car has bluetooth, don't use Phone and Speak buttons\n"
					"\t-c <time>    Force CDC-info replies every <time> seconds\n"
					"\t-g <number>  GPIO number to use for IBUS line monitor (0 = Use TH3122)\n"
					"\t-m           Do not do MK3 style CDC announcements\n"
					"\t-r           Do not switch to camera in reverse gear\n"
					"\t-s <string>  Send extra string to IBUS at startup\n"
					"\t-v <number>  Set PiBUS hardware version\n"
					"\n",
					argv[0]);
				return -1;
		}
	}

	if (argc > optind)
	{
		port = argv[optind];
	}

	if (!gpio_changed && hw_version >= 4)
	{
		gpio_number = 17;
	}

	if (gpio_init() != 0)
	{
		fprintf(stderr, "Can't init gpio\r\n");
		return -4;
	}

	if (ibus_init(port, startup, bluetooth, camera, mk3, cdcinterval, gpio_number, hw_version) != 0)
	{
		return -2;
	}

	if (keyboard_init() != 0)
	{
		fprintf(stderr, "Can't open keyboard\r\n");
		return -3;
	}

	mainloop();

	gpio_cleanup();
	ibus_cleanup();
	keyboard_cleanup();

	return 0;
}
