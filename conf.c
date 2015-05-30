/* Parser for finit.conf
 *
 * Copyright (c) 2012-2015  Joachim Nilsson <troglobit@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <ctype.h>
#include <dirent.h>
#include <string.h>

#include "finit.h"
#include "svc.h"
#include "tty.h"
#include "libite/lite.h"
#include "helpers.h"

#define MATCH_CMD(l, c, x) \
	(!strncasecmp(l, c, strlen(c)) && (x = (l) + strlen(c)))

static int parse_conf(char *file);


static char *strip_line(char *line)
{
	char *ptr;

	/* Trim leading whitespace */
	while (*line && isblank(*line))
		line++;

	/* Strip any comment at end of line */
	ptr = line;
	while (*ptr && *ptr != '#')
		ptr++;
	*ptr = 0;

	return line;
}

/* Convert optional "[!123456789S]" string into a bitmask */
int parse_runlevels(char *runlevels)
{
	int i, not = 0, bitmask = 0;

	if (!runlevels)
		runlevels = "[234]";
	i = 1;
	while (i) {
		int level;
		char lvl = runlevels[i++];

		if (']' == lvl || 0 == lvl)
			break;
		if ('!' == lvl) {
			not = 1;
			bitmask = 0x3FE;
			continue;
		}

		if ('s' == lvl || 'S' == lvl)
			lvl = '0';

		level = lvl - '0';
		if (level > 9 || level < 0)
			continue;

		if (not)
			CLRBIT(bitmask, level);
		else
			SETBIT(bitmask, level);
	}

	return bitmask;
}

static void parse_static(char *line)
{
	char *x;
	char cmd[CMD_SIZE];

	/* Do this before mounting / read-write
	 * XXX: Move to plugin which checks /etc/fstab instead */
	if (MATCH_CMD(line, "check ", x)) {
		char *dev = strip_line(x);

		strcpy(cmd, "/sbin/fsck -C -a ");
		strlcat(cmd, dev, sizeof(cmd));
		run_interactive(cmd, "Checking file system %s", dev);

		return;
	}

	if (MATCH_CMD(line, "user ", x)) {
		if (username) free(username);
		username = strdup(strip_line(x));
		return;
	}

	if (MATCH_CMD(line, "host ", x)) {
		if (hostname) free(hostname);
		hostname = strdup(strip_line(x));
		return;
	}

	if (MATCH_CMD(line, "module ", x)) {
		char *mod = strip_line(x);

		strcpy(cmd, "/sbin/modprobe ");
		strlcat(cmd, mod, sizeof(cmd));
		run_interactive(cmd, "Loading kernel module %s", mod);

		return;
	}

	if (MATCH_CMD(line, "mknod ", x)) {
		char *dev = strip_line(x);

		strcpy(cmd, "/bin/mknod ");
		strlcat(cmd, dev, sizeof(cmd));
		run_interactive(cmd, "Creating device node %s", dev);

		return;
	}

	if (MATCH_CMD(line, "network ", x)) {
		if (network) free(network);
		network = strdup(strip_line(x));
		return;
	}

	/* This is the directory from which executable scripts and
	 * any finit include files are read, default /etc/finit.d/ */
	if (MATCH_CMD(line, "runparts ", x)) {
		if (runparts) free(runparts);
		runparts = strdup(strip_line(x));
		return;
	}

	/* Parse any include file, use rcsd if absolute path not given */
	if (MATCH_CMD(line, "include ", x)) {
		char *file = strip_line(x);

		strlcpy(cmd, file, sizeof(cmd));
		if (!fexist(cmd)) {
			/* ... try /etc/finit.d/ as prefix */
			snprintf(cmd, sizeof(cmd), "%s/%s", rcsd, file);
			if (!fexist(cmd))
				return;
		}

		parse_conf(cmd);
		return;
	}

	if (MATCH_CMD(line, "startx ", x)) {
		svc_register(SVC_TYPE_SERVICE, strip_line(x), 0, username);
		return;
	}

	if (MATCH_CMD(line, "shutdown ", x)) {
		if (sdown) free(sdown);
		sdown = strdup(strip_line(x));
		return;
	}

	/* The desired runlevel to start when leaving
	 * bootstrap (S).  Finit supports 1-9, but most
	 * systems only use 1-6, where 6 is reserved for
	 * reboot */
	if (MATCH_CMD(line, "runlevel ", x)) {
		char *token = strip_line(x);
		const char *err = NULL;

		cfglevel = strtonum(token, 1, 9, &err);
		if (err)
			cfglevel = RUNLEVEL;
		if (cfglevel < 1 || cfglevel > 9 || cfglevel == 6)
			cfglevel = 2; /* Fallback */
		return;
	}

	/* TODO: Make console & tty dynamically loadable from /etc/finit.d */
	if (MATCH_CMD(line, "console ", x)) {
		if (console) free(console);
		console = strdup(strip_line(x));
		return;
	}

	/* TODO: Make console & tty dynamically loadable from /etc/finit.d */
	if (MATCH_CMD(line, "tty ", x)) {
		tty_register(strip_line(x));
		return;
	}
}

static void parse_dynamic(char *line, time_t mtime)
{
	char *x;

	/* Skip comments, i.e. lines beginning with # */
	if (MATCH_CMD(line, "#", x))
		return;

	/* Monitored daemon, will be respawned on exit, as
	 * long as the (optional) service callback returns
	 * non-zero */
	if (MATCH_CMD(line, "service ", x)) {
		svc_register(SVC_TYPE_SERVICE, x, mtime, NULL);
		return;
	}

	/* One-shot task, will not be respawned. Only runs if
	 * the (optional) service callback returns true */
	if (MATCH_CMD(line, "task ", x)) {
		svc_register(SVC_TYPE_TASK, x, mtime, NULL);
		return;
	}

	/* Like task but waits for completion, useful w/ [S] */
	if (MATCH_CMD(line, "run ", x)) {
		svc_register(SVC_TYPE_RUN, x, mtime, NULL);
		return;
	}

	/* Classic inetd service */
	if (MATCH_CMD(line, "inetd ", x)) {
#ifndef INETD_DISABLED
		svc_register(SVC_TYPE_INETD, x, mtime, NULL);
#else
		_e("Finit built with inetd support disabled, cannot register service inetd %s!", x);
#endif
		return;
	}
}

static int parse_conf_dynamic(char *file, time_t mtime)
{
	FILE *fp = fopen(file, "r");

	if (!fp) {
		_pe("Failed opening %s", file);
		return 1;
	}

	while (!feof(fp)) {
		char line[LINE_SIZE] = "";

		if (!fgets(line, sizeof(line), fp))
			continue;

		chomp(line);
		_d("dyn conf: %s", line);

		parse_dynamic(line, mtime);
	}

	fclose(fp);

	return 0;
}

static int parse_conf(char *file)
{
	FILE *fp;
	char line[LINE_SIZE] = "";
	char *x;

	fp = fopen(file, "r");
	if (!fp)
		return 1;

	/*
	 * If not standard finit.conf, then we want to show just the base name
	 * Loading configuration ............. vs
	 * Loading services configuration ....
	 */
	if (!string_match (file, FINIT_CONF)) {
		/* Remove leading path */
		x = strrchr(file, '/');
		if (!x) x = file;
		else	x++;

		/* Remove ending .conf */
		strlcpy(line, x, sizeof(line));
		x = strstr(line, ".conf");
		if (x) *x = 0;

		/* Add empty space. */
		strcat(line, " ");
	}

	print(0, "Loading %sconfiguration", line);
	while (!feof(fp)) {
		if (!fgets(line, sizeof(line), fp))
			continue;
		chomp(line);

		_d("conf: %s", line);
		parse_static(line);
		parse_dynamic(line, 0);
	}

	fclose(fp);

	return 0;
}

int parse_finit_d(char *dir)
{
	int i, num;
	struct dirent **e;

	num = scandir(dir, &e, NULL, alphasort);
	if (num < 0) {
		_d("No files found in %s, skipping ...", dir);
		return -1;
	}

	for (i = 0; i < num; i++) {
		char *name = e[i]->d_name;
		char  path[CMD_SIZE];
		struct stat st;

		snprintf(path, sizeof(path), "%s/%s", dir, name);

		/* Check that it's an actual file ... */
		if (stat(path, &st)) {
			_d("Cannot even read .conf file %s, skipping ...", path);
			continue;
		}
		if (S_ISEXEC(st.st_mode) || S_ISDIR(st.st_mode))
			continue;

		/* Check that file ends with '.conf' */
		if (strcmp(&path[strlen(path) - 5], ".conf")) {
			_d("File %s is not a .conf, skipping ... ", path);
			continue;
		}

		parse_conf_dynamic(path, st.st_mtime);
	}

	while (num--)
		free(e[num]);
	free(e);

	return 0;
}

int parse_finit_conf(char *file)
{
	int result;

	username = strdup(DEFUSER);
	hostname = strdup(DEFHOST);

	result = parse_conf(file);
	if (!tty_num()) {
		char *fallback = FALLBACK_SHELL;

		if (console)
			fallback = console;

		tty_register(fallback);
	}

	return result;
}


/**
 * Local Variables:
 *  version-control: t
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
