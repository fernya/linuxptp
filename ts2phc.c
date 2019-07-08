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
#ifdef ENABLE_GPS
#include <gps.h>
#endif

#include "missing.h"
#include "util.h"

#define KP 0.7
#define KI 0.3
#define NS_PER_SEC 1000000000LL

#define max_ppb  512000
#define min_ppb -512000

int phc_fd;
uint64_t tai_diff;
#ifdef ENABLE_GPS
static struct gps_data_t gpsdata;
#endif
int extts_index;
int pps_delay = 0;

/* This function maps the SDP0
 * to the channel 1 periodic output */


static void perout_close(void)
{
	struct ptp_extts_request extts;

	memset(&extts, 0, sizeof(extts));
	extts.index = extts_index;
	extts.flags = 0;
	if (ioctl(phc_fd, PTP_EXTTS_REQUEST, &extts)) {
		perror("PTP_EXTTS_REQUEST failed");
	}
}

static int perout_conf(int fd)
{
	struct ptp_pin_desc perout = {
		.index = 0,
		.func  = PTP_PF_EXTTS,
		.chan  = 1,
	};
	int err = ioctl(fd, PTP_PIN_SETFUNC, &perout);
	if (err < 0) {
		perror("PTP_PIN_SETFUNC request failed");
		return err ? errno : 0;
	}

	return 0;
}

static void clock_ppb(clockid_t clkid, double ppb)
{
	struct timex tx;
	memset(&tx, 0, sizeof(tx));
	tx.modes = ADJ_FREQUENCY;
	tx.freq = (long) (ppb * 65.536);
	if (clock_adjtime(clkid, &tx) < 0)
		fprintf(stderr, "failed to adjust the clock: %m\n");
}

static void clock_step(clockid_t clkid, int64_t ns)
{
	struct timex tx;
	int sign = 1;
	if (ns < 0) {
		sign = -1;
		ns *= -1;
	}
	memset(&tx, 0, sizeof(tx));
	tx.modes = ADJ_SETOFFSET | ADJ_NANO;
	tx.time.tv_sec  = sign * (ns / NS_PER_SEC);
	tx.time.tv_usec = sign * (ns % NS_PER_SEC);
	/*
	 * The value of a timeval is the sum of its fields, but the
	 * field tv_usec must always be non-negative.
	 */
	if (tx.time.tv_usec < 0) {
		tx.time.tv_sec  -= 1;
		tx.time.tv_usec += 1000000000;
	}
	if (clock_adjtime(clkid, &tx) < 0)
		fprintf(stderr, "failed to step clock: %m\n");
}

struct servo {
	uint64_t saved_ts;
	int64_t saved_offset;
	double drift;
	enum {
		SAMPLE_0, SAMPLE_1, SAMPLE_2, SAMPLE_3, SAMPLE_N
	} state;
};

static struct servo servo;

static void show_servo(FILE *fp, const char *label, int64_t offset, uint64_t ts)
{
	fprintf(fp, "%s %9lld s%d %lld.%09llu drift %.2f\n", label, (long long)offset,
		servo.state, ts / NS_PER_SEC, ts % NS_PER_SEC, servo.drift);
	fflush(fp);
}

static void do_servo(struct servo *srv, clockid_t dst,
		     int64_t offset, uint64_t ts, double kp, double ki)
{
	double ki_term, ppb;
	struct timespec tspec;

	tspec.tv_sec = ts / NS_PER_SEC;
	tspec.tv_nsec = 0;

	switch (srv->state) {
	case SAMPLE_0:
		clock_ppb(dst, 0.0);
		srv->saved_offset = offset;
		srv->saved_ts = ts;
		clock_settime(dst, &tspec);
		srv->state = SAMPLE_1;
		break;
	case SAMPLE_1:
		srv->state = SAMPLE_2;
		break;
	case SAMPLE_2:
		srv->state = SAMPLE_3;
		break;
	case SAMPLE_3:
		srv->drift = (offset - srv->saved_offset) * 1e9 /
			(ts - srv->saved_ts);
		clock_ppb(dst, -srv->drift);
		clock_step(dst, -offset);
		srv->state = SAMPLE_N;
		break;
	case SAMPLE_N:
		ki_term = ki * offset;
		ppb = kp * offset + srv->drift + ki_term;
		if (ppb < min_ppb) {
			ppb = min_ppb;
		} else if (ppb > max_ppb) {
			ppb = max_ppb;
		} else {
			srv->drift += ki_term;
		}
		clock_ppb(dst, -ppb);
		break;
	}
}

static int read_extts(int fd, int64_t *offset, uint64_t *ts)
{

	struct ptp_extts_event event;

	if (!read(fd, &event, sizeof(event))) {
		perror("read extts event");
		return 0;
	}

	if (event.index == extts_index) {
		*ts = event.t.sec * NS_PER_SEC;
		*ts += event.t.nsec;

		*offset = *ts % (NS_PER_SEC);
		if (*offset > NS_PER_SEC / 2)
			*offset -= (NS_PER_SEC);

		return 1;
	}
	return 0;
}

static int do_extts_loop(char *extts_device, double kp, double ki,
			 clockid_t dst, int servo_active)
{
	int64_t extts_offset;
	uint64_t extts_ts;
	int err;
	struct ptp_extts_request extts;

	phc_fd = open(extts_device, O_RDWR);
	if (phc_fd < 0) {
		fprintf(stderr, "cannot open '%s': %m\n", extts_device);
		return -1;
	}
	// Set the pinmux
	if (perout_conf(phc_fd)) {
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
		if (!read_extts(phc_fd, &extts_offset, &extts_ts)) {
			continue;
		}
		if (servo_active) {
			do_servo(&servo, FD_TO_CLOCKID(phc_fd), extts_offset,
				 extts_ts, kp, ki);
		}
		show_servo(stdout, "extts", extts_offset, extts_ts);
	}
	close(phc_fd);
	return 0;
}

#ifdef ENABLE_GPS
static int do_extts_loop_gps(char *extts_device, double kp, double ki,
			     clockid_t dst, int servo_active)
{
	int64_t extts_offset;
	uint64_t extts_ts;
	int err;
	struct ptp_extts_request extts;

	phc_fd = open(extts_device, O_RDWR);
	if (phc_fd < 0) {
		fprintf(stderr, "cannot open '%s': %m\n", extts_device);
		return -1;
	}
	// Set the pinmux
	if (perout_conf(phc_fd)) {
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
		if (!read_extts(phc_fd, &extts_offset, &extts_ts)) {
			continue;
		} else {
			gps_read(&gpsdata);
		}
		uint64_t utctime = (gpsdata.fix.time + 1.0) * NS_PER_SEC;
		fprintf(stdout, "fix.time: %.0lf\t", gpsdata.fix.time);
		if (servo_active) {
			do_servo(&servo, FD_TO_CLOCKID(phc_fd),
				 extts_offset - pps_delay, utctime + tai_diff, kp, ki);
		}
		show_servo(stdout, "extts", extts_offset, extts_ts);
	}
	close(phc_fd);
	return 0;
}
#endif

static void usage(char *progname)
{
	fprintf(stderr,
		"\n"
		"usage: %s [options]\n\n"
		" -I [ki]        integration constant (0.3)\n"
		" -P [kp]        proportional constant (0.7)\n"
		" -d [dev]       external timestamp source\n"
		" -e [delay]     delay of the PPS signal from the receiver\n"
		" -f             disable servo (useful in multiple instances)\n"
		" -g             attach to gpsd\n"
		" -h             prints this message and exits\n"
		" -i [channel]   index of event source (1)\n"
		" -t [offset]    UTC-TAI offset at the time of start\n"
		"\n",
		progname);
}

int main(int argc, char *argv[])
{
	int c, err, junk, servo_active = 1, use_gpsd = 0;
	char *device = NULL, *progname;
	double kp = KP, ki = KI;

	handle_term_signals();

	extts_index = 1;

	/* Process the command line arguments. */
	progname = strrchr(argv[0], '/');
	progname = progname ? 1+progname : argv[0];
	while (EOF != (c = getopt(argc, argv, "I:P:d:e:fghi:t:"))) {
		switch (c) {
		case 'I':
			ki = atof(optarg);
			break;
		case 'P':
			kp = atof(optarg);
			break;
		case 'd':
			device = optarg;
			break;
		case 'e':
			pps_delay = atoi(optarg);
			break;
		case 'f':
			servo_active = 0;
			break;
		case 'g':
#ifdef ENABLE_GPS
			use_gpsd = 1;
#else
			use_gpsd = 0;
#endif
			break;
		case 'i':
			extts_index = atoi(optarg);
			break;
		case 't':
			tai_diff = atoi(optarg);
			tai_diff *= NS_PER_SEC;
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

	if (!device) {
		usage(progname);
		exit(EXIT_FAILURE);
	}

	clockid_t dev = posix_clock_open(device, &junk);
	if (dev == CLOCK_INVALID) {
		return -1;
	}
#ifdef ENABLE_GPS
	if (use_gpsd) {
		if (err = gps_open(GPSD_SHARED_MEMORY, GPSD_SHARED_MEMORY, &gpsdata) == -1) {
			fprintf(stderr, "%s: %d\n", gps_errstr(err), err);
			exit(EXIT_FAILURE);
		}

		err = do_extts_loop_gps(device, kp, ki, dev, servo_active);
	}
#else
	(void) use_gpsd;
#endif
	err = do_extts_loop(device, kp, ki, dev, servo_active);
	perout_close();
#ifdef ENABLE_GPS
	gps_close(&gpsdata);
#endif
	close(phc_fd);
	return err;
}
