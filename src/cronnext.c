/*
    cronnext - calculate the time cron will execute the next job
    Copyright (C) 2016 Marco Migliori <sgerwk@aol.com>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 
    The GNU General Public License can also be found in the file
    `COPYING' that comes with the Anacron source distribution.
*/

#include "config.h"

#define MAIN_PROGRAM

#include <stdlib.h>
#include <string.h>
#include <pwd.h>

#include "globals.h"
#include "funcs.h"
#include "cron-paths.h"

/* what to print: entries, crontabs, both */
#define ENTRIES  0x01
#define CRONTABS 0x02

#ifdef WITH_INOTIFY
void set_cron_watched(int fd) {
/* empty stub */
	(void)fd;
}
#endif

void do_command(entry *e, user *u) {
/* empty stub */
	(void)e;
	(void)u;
}

#ifdef WITH_SELINUX
int get_security_context(const char *name, int crontab_fd,
			 security_context_t *rcontext, const char *tabname) {
/* empty stub */
	(void)name;
	(void)crontab_fd;
	(void)tabname;
	*rcontext = NULL;
	return 0;
}

void free_security_context(security_context_t *scontext) {
/* empty stub */
	(void)scontext;
}
#endif

/*
 * print entry flags
 */
char *flagname[]= {
	[MIN_STAR] =	"MIN_STAR",
	[HR_STAR] =	"HR_STAR",
	[DOM_STAR] =	"DOM_STAR",
	[DOW_STAR] =	"DOW_STAR",
	[WHEN_REBOOT] =	"WHEN_REBOOT",
	[DONT_LOG] =	"DONT_LOG"
};

void printflags(int flags) {
	int f;
	int first = 1;

	printf("flags: 0x%d = ", flags);
	for (f = 1; f < sizeof(flagname);  f = f << 1)
		if (flags & f) {
			printf("%s%s", first ? " " : "|", flagname[f]);
			first = 0;
		}
	printf("\n");
}

/*
 * print a crontab entry
 */
void printentry(entry *e, int system, time_t next) {
	if (system)
		printf("entry user: %s\n", e->pwd->pw_name);
	printf("cmd: %s\n", e->cmd);
	printflags(e->flags);
	printf("delay: %d\n", e->delay);
	printf("next: %ld = ", (long)next);
	printf("%s", asctime(localtime(&next)));
}

/*
 * print a crontab data
 */
void printcrontab(user *u) {
	printf("==========================\n");
	printf("user: %s\n", u->name);
	printf("crontab: %s\n", u->tabname);
	printf("system: %d\n", u->system);
}

/*
 * basic algorithm: iterate over time from now to 8 year ahead in default steps
 * of 1 minute, checking whether time matches a crontab entry at each step (8
 * years is the largest interval between two crontab matches)
 *
 * to save iterations, use larger steps if month or day don't match the entry:
 * - if the month doesn't match, skip to 00:00 of the first day of next month
 * - for the day, avoid the complication of the different length of months: if
 *   neither the day nor the next day match, increase time of one day
 */

/*
 * check whether time matches day of month and/or day of week; this requires
 * checking dom if dow=*, dow if dom=*, either one otherwise; see comment "the
 * dom/dow situation is odd..." in cron.c
 */
int matchday(entry *e, time_t time) {
	struct tm current;

	localtime_r(&time, &current);

	if (e->flags & DOW_STAR)
		return bit_test(e->dom, current.tm_mday - 1);
	if (e->flags & DOM_STAR) 
		return bit_test(e->dow, current.tm_wday);
	return bit_test(e->dom, current.tm_mday - 1) ||
		bit_test(e->dow, current.tm_wday);
}

/*
 * next time matching a crontab entry
 */
time_t nextmatch(entry *e, time_t start, time_t end) {
	time_t time;
	struct tm current;

	for (time = start; time <= end; ) {
		localtime_r(&time, &current);

		/* month doesn't match: move to 1st of next month */
		if (!bit_test(e->month, current.tm_mon)) {
			current.tm_mon++;
			if (current.tm_mon >= 12) {
				current.tm_year++;
				current.tm_mon = 0;
			}
			current.tm_mday = 1;
			current.tm_hour = 0;
			current.tm_min = 0;
			time = mktime(&current);
			continue;
		}

		/* neither time nor time+1day match day: increase 1 day */
		if (!matchday(e, time) && !matchday(e, time + 24 * 60 * 60)) {
			time += 24 * 60 * 60;
			continue;
		}

		/* if time matches, return time;
		 * check for month is redudant, but check for day is
		 * necessary because we only know that either time
		 * or time+1day match */
		if (bit_test(e->month, current.tm_mon) &&
			matchday(e, time) &&
			bit_test(e->hour, current.tm_hour) &&
			bit_test(e->minute, current.tm_min)
		)
			return time;

		/* skip to next minute */
		time += 60;
	}

	return -1;
}

/*
 * match a user against a list
 */
int matchuser(char *user, char *list) {
	char *pos;
	size_t l = strlen(user);

	for (pos = list; (pos = strstr(pos, user)) != NULL; pos += l) {
		if ((pos != list) && (*(pos - 1) != ','))
			continue;
		if ((pos[l] != '\0') && (pos[l] != ','))
			continue;
		return 1;
	}
	return 0;
}

/*
 * find next sheduled job
 */
time_t cronnext(cron_db database,
		time_t start, time_t end,
		char *include, char *exclude, int system,
		int verbose) {
	time_t closest, next;
	user *u;
	entry *e;

	/* find next sheduled time */
	closest = -1;
	for (u = database.head; u; u = u->next) {
		if (include && !matchuser(u->name, include))
			continue;
		if (exclude && matchuser(u->name, exclude))
			continue;
		if (!system && u->system)
			continue;

		if (verbose & CRONTABS)
			printcrontab(u);

		for (e = u->crontab; e; e = e->next) {
			next = nextmatch(e, start, end);
			if (next < 0)
				continue;
			if ((closest < 0) || (next < closest))
				closest = next;
			if (verbose & ENTRIES)
				printentry(e, u->system, next);
		}
	}

	return closest;
}

/*
 * load installed crontabs and/or crontab files
 */
cron_db database(int installed, char **additional) {
	cron_db db = {NULL, NULL, (time_t) 0};
	struct passwd pw;
	int fd;
	user *u;

	if (installed)
		load_database(&db);

	for ( ; *additional != NULL; additional++) {
		fd = open(*additional, O_RDONLY);
		if (fd == -1) {
			perror(*additional);
			continue;
		}
		memset(&pw, 0, sizeof(pw));
		pw.pw_name = *additional;
		pw.pw_passwd = "";
		pw.pw_dir = ".";
		u = load_user(fd, &pw, *additional, *additional, *additional);
		if (u == NULL) {
			printf("cannot load crontab %s\n", *additional);
			continue;
		}
		link_user(&db, u);
	}

	return db;
}

void usage() {
	fprintf(stderr, "Find the time of the next scheduled cron job.\n");
	fprintf(stderr, "Usage:\n");
	fprintf(stderr, " cronnext [options] [file ...]\n");
	fprintf(stderr, "\n");
	fprintf(stderr, "Options:\n");
	fprintf(stderr, " -i users  include only the crontab of these users\n");
	fprintf(stderr, " -e users  exclude the crontab of these users\n");
	fprintf(stderr, " -s        do not include the system crontab\n");
	fprintf(stderr, " -a        examine installed crontabs even if files are given\n");
	fprintf(stderr, " -t time   start from this time (seconds since epoch)\n");
	fprintf(stderr, " -q time   end check at this time (seconds since epoch)\n");
	fprintf(stderr, " -l        print next jobs to be executed\n");
	fprintf(stderr, " -v        verbose mode\n");
	fprintf(stderr, " -h        this help\n");
	fprintf(stderr, " -V        print version and exit\n");
}

/*
 * main
 */
int main(int argn, char *argv[]) {
	int opt;
	char *include, *exclude;
	int system, verbose, endtime, printjobs;
	time_t start, end, next;

	include = NULL;
	exclude = NULL;
	system = 1;
	endtime = 0;
	printjobs = 0;
	start = time(NULL);
	int installed = 0;
	verbose = 0;

	cron_db db;

	while (-1 != (opt = getopt(argn, argv, "i:e:ast:vhV"))) {
		switch (opt) {
		case 'i':
			include = optarg;
			break;
		case 'e':
			exclude = optarg;
			break;
		case 'a':
			installed = 1;
			break;
		case 's':
			system = 0;
			break;
		case 't':
			start = atoi(optarg);
			break;
		case 'q':
			end = atoi(optarg);
			endtime = 1;
			break;
		case 'l':
			printjobs = 1;
			break;
		case 'v':
			verbose = ENTRIES | CRONTABS;
			break;
		case 'h':
			usage();
			return EXIT_SUCCESS;
		case 'V':
			puts(PACKAGE_STRING);
			return EXIT_SUCCESS;
		default:
			fprintf(stderr, "unrecognized option: %s\n",
				argv[optind - 1]);
			usage();
			exit(EXIT_FAILURE);
		}
	}

	/* maximum match interval is 8 years:
	 * crontab has '* * 29 2 *' and we are on 1 March 2096:
	 * next matching time will be 29 February 2104 */
	if (!endtime)
		end = start + 8 * 12 * 31 * 24 * 60 * 60;

	/* debug cron */
	if (verbose) {
		printf("SPOOL_DIR=%s\n", SPOOL_DIR);
		set_debug_flags("load");
	}
	/* "load,pars" for debugging loading and parsing, "" for nothing
	   see globals.h for symbolic names and macros.h for meaning */

	/* load database */
	db = database(installed || argv[optind] == NULL, argv + optind);

	/* print time of next scheduled command */
	next = cronnext(db, start, end, include, exclude, system, verbose);
	if (next == -1) {
		if (verbose)
			printf("no job scheduled\n");
		return EXIT_FAILURE;
	}
	else if (verbose || printjobs) {
		printf("next jobs:\n");
		cronnext(db, next, next, include, exclude, system, ENTRIES);
	}
	else
		printf("%ld\n", (long) next);

	return EXIT_SUCCESS;
}

