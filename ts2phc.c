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
#include <stdlib.h>

#include "config.h"
#include "print.h"
#include "ts2phc_master.h"
#include "ts2phc_slave.h"
#include "version.h"

static void ts2phc_cleanup(struct config *cfg, struct ts2phc_master *master)
{
	ts2phc_slave_cleanup();
	if (master) {
		ts2phc_master_destroy(master);
	}
	if (cfg) {
		config_destroy(cfg);
	}
}

static void usage(char *progname)
{
	fprintf(stderr,
		"\n"
		"usage: %s [options]\n\n"
		" -c [dev|name]  phc slave clock (like /dev/ptp0 or eth0)\n"
		" -f [file]      read configuration from 'file'\n"
		" -h             prints this message and exits\n"
		" -m             print messages to stdout\n"
		" -q             do not print messages to the syslog\n"
		" -s [dev|name]  source of the PPS signal\n"
		"                may take any of the following forms:\n"
		"                    generic   - an external 1-PPS without ToD information\n"
		"                    /dev/ptp0 - a local PTP Hardware Clock (PHC)\n"
		"                    eth0      - a local PTP Hardware Clock (PHC)\n"
		" -v             prints the software version and exits\n"
		"\n",
		progname);
}

int main(int argc, char *argv[])
{
	char *config = NULL, *pps_source = NULL, *progname;
	int c, err = 0, have_slave = 0, index;
	struct ts2phc_master *master = NULL;
	enum ts2phc_master_type pps_type;
	struct config *cfg = NULL;
	struct option *opts;

	handle_term_signals();

	cfg = config_create();
	if (!cfg) {
		ts2phc_cleanup(cfg, master);
		return -1;
	}

	opts = config_long_options(cfg);

	/* Process the command line arguments. */
	progname = strrchr(argv[0], '/');
	progname = progname ? 1+progname : argv[0];
	while (EOF != (c = getopt_long(argc, argv, "c:f:hi:mqs:v", opts, &index))) {
		switch (c) {
		case 0:
			if (config_parse_option(cfg, opts[index].name, optarg)) {
				ts2phc_cleanup(cfg, master);
				return -1;
			}
			break;
		case 'c':
			if (!ts2phc_slave_add(cfg, optarg)) {
				ts2phc_cleanup(cfg, master);
				return -1;
			}
			have_slave = 1;
			break;
		case 'f':
			config = optarg;
			break;
		case 'm':
			config_set_int(cfg, "verbose", 1);
			break;
		case 'q':
			config_set_int(cfg, "use_syslog", 0);
			break;
		case 's':
			pps_source = optarg;
			break;
		case 'v':
			ts2phc_cleanup(cfg, master);
			version_show(stdout);
			return 0;
		case 'h':
			ts2phc_cleanup(cfg, master);
			usage(progname);
			return -1;
		case '?':
		default:
			ts2phc_cleanup(cfg, master);
			usage(progname);
			return -1;
		}
	}

	if (config && (c = config_read(config, cfg))) {
		ts2phc_cleanup(cfg, master);
		return -1;
	}
	if (!have_slave) {
		fprintf(stderr, "no slave clocks specified\n");
		ts2phc_cleanup(cfg, master);
		usage(progname);
		return -1;
	}
	if (!pps_source) {
		fprintf(stderr, "no PPS source specified\n");
		ts2phc_cleanup(cfg, master);
		usage(progname);
		return -1;
	}

	print_set_progname(progname);
	print_set_tag(config_get_string(cfg, NULL, "message_tag"));
	print_set_verbose(config_get_int(cfg, NULL, "verbose"));
	print_set_syslog(config_get_int(cfg, NULL, "use_syslog"));
	print_set_level(config_get_int(cfg, NULL, "logging_level"));

	if (!strcasecmp(pps_source, "generic")) {
		pps_type = TS2PHC_MASTER_GENERIC;
	} else {
		pps_type = TS2PHC_MASTER_PHC;
	}

	master = ts2phc_master_create(cfg, pps_source, pps_type);
	if (!master) {
		ts2phc_cleanup(cfg, master);
		return -1;
	}
	while (is_running()) {
		err = ts2phc_slave_poll();
		if (err) {
			break;
		}
	}
	ts2phc_cleanup(cfg, master);

	return err;
}
