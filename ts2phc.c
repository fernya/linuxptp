/**
 * @file ts2phc.c
 * @brief Utility program to synchronize the PHC clock to external events
 * @note Copyright (C) 2013 Balint Ferencz <fernya@sch.bme.hu>
 * @note Based on the phc2sys utility
 * @note Copyright (C) 2012 Richard Cochran <richardcochran@gmail.com>
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
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <math.h>

#include <linux/ptp_clock.h>

#include "config.h"
#include "clockadj.h"
#include "missing.h"
#include "servo.h"
#include "util.h"

#define NS_PER_SEC 1000000000LL

/* This function maps the SDP0
 * to the channel 1 periodic output */

static int enable_input_pin(int fd)
{
	struct ptp_pin_desc perout = {
		.index = 0,
		.func  = PTP_PF_EXTTS,
		.chan  = 1,
	};
	int err = ioctl(fd, PTP_PIN_SETFUNC, &perout);
	if (err < 0) {
		perror("PTP_PIN_SETFUNC request failed");
		return -1;
	}
	return 0;
}

/*
 * Returns the time on the PPS source device at which the most recent
 * PPS event was generated.
 *
 * This implementation assumes that the system time is approximately
 * correct, and it simply drops the nanoseconds field past the full
 * second.
 *
 * TODO: convert this into a proper interface that depends on the PSS
 * source device.
 */
static uint64_t pps_source_gettime(void)
{
	struct timespec now;
	uint64_t result;

	clock_gettime(CLOCK_TAI, &now);
	result = now.tv_sec * NS_PER_SEC;

	return result;
}

static int read_extts(int fd, int64_t *offset, uint64_t *local_ts, int extts_index)
{
	uint64_t event_tns, pps_source_time;
	struct ptp_extts_event event;
	int cnt;

	cnt = read(fd, &event, sizeof(event));
	if (cnt != sizeof(event)) {
		perror("read extts event");
		return -1;
	}
	if (event.index != extts_index) {
		return -1;
	}
	pps_source_time = pps_source_gettime();
	event_tns = event.t.sec * NS_PER_SEC;
	event_tns += event.t.nsec;
	*offset = event_tns - pps_source_time;
	*local_ts = event_tns;

	return 0;
}

static int do_extts_loop(clockid_t clkid, struct servo *servo, int extts_index)
{
	struct ptp_extts_request extts;
	enum servo_state state;
	uint64_t extts_ts;
	int err, phc_fd;
	int64_t offset;
	double adj;

	phc_fd = CLOCKID_TO_FD(clkid);
	if (enable_input_pin(phc_fd)) {
		return -1;
	}

	memset(&extts, 0, sizeof(extts));
	extts.index = extts_index;
	extts.flags = PTP_RISING_EDGE | PTP_ENABLE_FEATURE;

	err = ioctl(phc_fd, PTP_EXTTS_REQUEST, &extts);
	if (err < 0) {
		perror("PTP_EXTTS_REQUEST failed");
		return err ? errno : 0;
	}

	while (is_running()) {
		if (read_extts(phc_fd, &offset, &extts_ts, extts_index)) {
			continue;
		}
		adj = servo_sample(servo, offset, extts_ts, 0.0, &state);
		switch (state) {
		case SERVO_UNLOCKED:
			break;
		case SERVO_JUMP:
			clockadj_set_freq(clkid, -adj);
			clockadj_step(clkid, -offset);
			break;
		case SERVO_LOCKED:
		case SERVO_LOCKED_STABLE:
			clockadj_set_freq(clkid, -adj);
			break;
		}
	}

	memset(&extts, 0, sizeof(extts));
	extts.index = extts_index;
	extts.flags = 0;
	if (ioctl(phc_fd, PTP_EXTTS_REQUEST, &extts)) {
		perror("PTP_EXTTS_REQUEST failed");
	}

	close(phc_fd);
	return 0;
}

static void usage(char *progname)
{
	fprintf(stderr,
		"\n"
		"usage: %s [options]\n\n"
		" -c [dev]       slave clock\n"
		" -f [file]      read configuration from 'file'\n"
		" -h             prints this message and exits\n"
		" -i [channel]   index of event source (1)\n"
		"\n",
		progname);
}

int main(int argc, char *argv[])
{
	char *config = NULL, *progname, *slave_clock_device = NULL;
	int c, err = 0, extts_index = 1, index, junk;
	struct servo *servo;
	struct option *opts;
	struct config *cfg;
	clockid_t clkid;

	handle_term_signals();

	cfg = config_create();
	if (!cfg) {
		return -1;
	}

	opts = config_long_options(cfg);

	/* Process the command line arguments. */
	progname = strrchr(argv[0], '/');
	progname = progname ? 1+progname : argv[0];
	while (EOF != (c = getopt_long(argc, argv, "c:f:hi:", opts, &index))) {
		switch (c) {
		case 0:
			if (config_parse_option(cfg, opts[index].name, optarg)) {
				return -1;
			}
			break;
		case 'c':
			slave_clock_device = optarg;
			break;
		case 'f':
			config = optarg;
			break;
		case 'i':
			extts_index = atoi(optarg);
			break;
		case 'h':
			usage(progname);
			exit(EXIT_SUCCESS);
		case '?':
		default:
			usage(progname);
			exit(EXIT_FAILURE);
		}
	}

	if (config && (c = config_read(config, cfg))) {
		config_destroy(cfg);
		return -1;
	}

	if (!slave_clock_device) {
		usage(progname);
		exit(EXIT_FAILURE);
	}

	clkid = posix_clock_open(slave_clock_device, &junk);
	if (clkid == CLOCK_INVALID) {
		return -1;
	}
	servo = servo_create(cfg, CLOCK_SERVO_PI, 0, 100000, 0);
	if (!servo) {
		return -1;
	}
	servo_sync_interval(servo, 1.0);

	err = do_extts_loop(clkid, servo, extts_index);

	return err;
}
