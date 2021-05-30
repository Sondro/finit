/* Finit - Fast /sbin/init replacement w/ I/O, hook & service plugins
 *
 * Copyright (c) 2008-2010  Claudio Matsuoka <cmatsuoka@gmail.com>
 * Copyright (c) 2008-2021  Joachim Wiberg <troglobit@gmail.com>
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

#include "config.h"		/* Generated by configure script */

#include <ctype.h>
#include <dirent.h>
#ifdef HAVE_FSTAB_H
#include <fstab.h>
#endif
#include <getopt.h>
#include <mntent.h>
#include <time.h>		/* tzet() */
#include <sys/klog.h>
#include <sys/mount.h>
#include <sys/stat.h>		/* umask(), mkdir() */
#include <sys/wait.h>
#include <lite/lite.h>

#include "finit.h"
#include "cgroup.h"
#include "cond.h"
#include "conf.h"
#include "helpers.h"
#include "private.h"
#include "plugin.h"
#include "service.h"
#include "sig.h"
#include "sm.h"
#include "tty.h"
#include "util.h"
#include "utmp-api.h"
#include "schedule.h"
#include "watchdog.h"

int   runlevel  = 0;		/* Bootstrap 'S' */
int   cfglevel  = RUNLEVEL;	/* Fallback if no configured runlevel */
int   prevlevel = -1;
int   debug     = 0;		/* debug mode from kernel cmdline */
int   rescue    = 0;		/* rescue mode from kernel cmdline */
int   single    = 0;		/* single user mode from kernel cmdline */
int   bootstrap = 1;		/* set while bootrapping (for TTYs) */
char *sdown     = NULL;
char *network   = NULL;
char *hostname  = NULL;
char *rcsd      = FINIT_RCSD;
char *runparts  = NULL;

uev_ctx_t *ctx  = NULL;		/* Main loop context */
svc_t *wdog     = NULL;		/* No watchdog by default */


/*
 * Show user configured banner before service bootstrap progress
 */
static void banner(void)
{
	/*
	 * Silence kernel logs, assuming users have sysklogd or
	 * similar enabled to start emptying /dev/kmsg, but for
	 * our progress we want to own the console.
	 */
	if (!debug)
		klogctl(6, NULL, 0);

	/*
	 * First level hooks, if you want to run here, you're
	 * pretty much on your own.  Nothing's up yet ...
	 */
	plugin_run_hooks(HOOK_BANNER);

	print_banner(INIT_HEADING);
}

/*
 * Check all filesystems in /etc/fstab with a fs_passno > 0
 */
static int fsck(int pass)
{
	struct fstab *fs;
	int rc = 0;

	if (!setfsent()) {
		_pe("Failed opening fstab");
		return 1;
	}

	while ((fs = getfsent())) {
		char cmd[80];
		struct stat st;

		if (fs->fs_passno != pass)
			continue;

		errno = 0;
		if (stat(fs->fs_spec, &st) || !S_ISBLK(st.st_mode)) {
			if (!string_match(fs->fs_spec, "UUID=") && !string_match(fs->fs_spec, "LABEL=")) {
				_d("Cannot fsck %s, not a block device: %s", fs->fs_spec, strerror(errno));
				continue;
			}
		}

		if (ismnt("/proc/mounts", fs->fs_file, "rw")) {
			_d("Skipping fsck of %s, already mounted rw on %s.", fs->fs_spec, fs->fs_file);
			continue;
		}

		snprintf(cmd, sizeof(cmd), "fsck -a %s", fs->fs_spec);
		rc += run_interactive(cmd, "Checking filesystem %.13s", fs->fs_spec);
	}

	endfsent();

	return rc;
}

static int fsck_all(void)
{
	int pass, rc = 0;

	for (pass = 1; pass < 10; pass++) {
		rc = fsck(pass);
		if (rc)
			break;
	}

	return rc;
}

#ifndef SYSROOT
static void fs_remount_root(int fsckerr)
{
	struct fstab *fs;

	if (!setfsent())
		return;

	while ((fs = getfsent())) {
		if (!strcmp(fs->fs_file, "/"))
			break;
	}

	/* If / is not listed in fstab, or listed as 'ro', leave it alone */
	if (!fs || !strcmp(fs->fs_type, "ro"))
		goto out;

	if (fsckerr)
		print(1, "Cannot remount / as read-write, fsck failed before");
	else
		run_interactive("mount -n -o remount,rw /",
				"Remounting / as read-write");

out:
	endfsent();
}
#else
static void fs_remount_root(int fsckerr)
{
	/*
	 * XXX: Untested, in the initramfs age we should
	 *      probably use switch_root instead.
	 */
	if (mount(SYSROOT, "/", NULL, MS_MOVE, NULL))
		_pe("Failed %s / MS_MOVE");
}
#endif	/* SYSROOT */

/*
 * Opinionated file system setup.  Checks for critical mount points and
 * mounts them as most users expect.  All file systems are checked with
 * /proc/mounts before mounting.
 *
 * Embedded systems, and other people who want full control, can set up
 * their system with /etc/fstab, which is handled before this function
 * is called.  For systems like Debian/Ubuntu, who only have / and swap
 * in their /etc/fstab, this function does all the magic necessary.
 */
static void fs_finalize(void)
{
	/*
	 * Some systems rely on us to both create /dev/shm and, to mount
	 * a tmpfs there.  Any system with dbus needs shared memory, so
	 * mount it, unless its already mounted, but not if listed in
	 * the /etc/fstab file already.
	 */
	if (!fismnt("/dev/shm")) {
		makedir("/dev/shm", 0777);
		mount("shm", "/dev/shm", "tmpfs", 0, "mode=0777");
	}

	/* Modern systems use /dev/pts */
	if (!fismnt("/dev/pts")) {
		char opts[32];
		int mode;
		int gid;

		gid = getgroup("tty");
		if (gid == -1)
			gid = 0;

		/* 0600 is default on Debian, use 0620 to get mesg y by default */
		mode = 0620;
		snprintf(opts, sizeof(opts), "gid=%d,mode=%d,ptmxmode=0666", gid, mode);

		makedir("/dev/pts", 0755);
		mount("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, opts);
	}

	/*
	 * Modern systems use tmpfs for /run.  Fallback to /var/run if
	 * /run doesn't exist is handled by the bootmisc plugin.  It
	 * also sets up compat symlinks.
	 *
	 * The unconditional mount of /run/lock is for DoS prevention.
	 * To override any of this behavior, add entries to /etc/fstab
	 * for /run (and optionally /run/lock).
	 */
	if (fisdir("/run") && !fismnt("/run")) {
		mount("tmpfs", "/run", "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RELATIME, "mode=0755,size=10%");

		/* This prevents user DoS of /run by filling /run/lock at the expense of another tmpfs, max 5MiB */
		makedir("/run/lock", 1777);
		mount("tmpfs", "/run/lock", "tmpfs", MS_NOSUID | MS_NODEV | MS_NOEXEC | MS_RELATIME, "mode=0777,size=5252880");
	}

	/* Modern systems use tmpfs for /tmp */
	if (!fismnt("/tmp"))
		mount("tmpfs", "/tmp", "tmpfs", MS_NOSUID | MS_NODEV, "mode=1777");
}

static void fs_mount(void)
{
	if (!rescue)
		fs_remount_root(fsck_all());

	_d("Root FS up, calling hooks ...");
	plugin_run_hooks(HOOK_ROOTFS_UP);

	if (run_interactive("mount -na", "Mounting filesystems"))
		plugin_run_hooks(HOOK_MOUNT_ERROR);

	_d("Calling extra mount hook, after mount -a ...");
	plugin_run_hooks(HOOK_MOUNT_POST);

	run("swapon -ea");

	_d("Finalize, ensure common file systems are available ...");
	fs_finalize();
}

/*
 * We need /proc for rs_remount_root() and conf_parse_cmdline(), /dev
 * for early multi-console, and /sys for the cgroups support.  Any
 * occurrence of these file systems in /etc/fstab will replace these
 * mounts later in fs_mount()
 *
 * Ignore any mount errors with EBUSY, kernel likely alread mounted
 * the filesystem for us automatically, e.g., CONFIG_DEVTMPFS_MOUNT.
 */
static void fs_init(void)
{
	struct {
		char *spec, *file, *type;
	} fs[] = {
		{ "proc",     "/proc", "proc"     },
		{ "devtmpfs", "/dev",  "devtmpfs" },
		{ "sysfs",    "/sys",  "sysfs"    },
	};
	size_t i;

	/* mask writable bit for g and o */
	umask(022);

	for (i = 0; i < NELEMS(fs); i++) {
		int rc;

		rc = mount(fs[i].spec, fs[i].file, fs[i].type, 0, NULL);
		if (rc && errno != EBUSY)
			_pe("Failed mounting %s on %s", fs[i].spec, fs[i].file);
	}
}


/*
 * Handle bootstrap transition to configured runlevel, start TTYs
 *
 * This is the final stage of bootstrap.  It changes to the default
 * (configured) runlevel, calls all external start scripts and final
 * bootstrap hooks before bringing up TTYs.
 *
 * We must ensure that all declared `task [S]` and `run [S]` jobs in
 * finit.conf, or *.conf in finit.d/, run to completion before we
 * finalize the bootstrap process by calling this function.
 */
static void finalize(void *unused)
{
	/* Clean up bootstrap-only tasks/services that never started */
	_d("Clean up all bootstrap-only tasks/services ...");
	svc_prune_bootstrap();

	/* All services/tasks/etc. in configure runlevel have started */
	_d("Running svc up hooks ...");
	plugin_run_hooks(HOOK_SVC_UP);
	service_step_all(SVC_TYPE_ANY);

	/* Convenient SysV compat for when you just don't care ... */
	if (!access(FINIT_RC_LOCAL, X_OK) && !rescue)
		run_interactive(FINIT_RC_LOCAL, "Calling %s", FINIT_RC_LOCAL);

	/* Hooks that should run at the very end */
	_d("Calling all system up hooks ...");
	plugin_run_hooks(HOOK_SYSTEM_UP);
	service_step_all(SVC_TYPE_ANY);

	/* Disable progress output at normal runtime */
	enable_progress(0);

	/* System bootrapped, launch TTYs et al */
	bootstrap = 0;
	service_step_all(SVC_TYPE_RESPAWN);
}

/*
 * Start cranking the big state machine
 */
static void crank_worker(void *unused)
{
	/*
	 * Initalize state machine and start all bootstrap tasks
	 * NOTE: no network available!
	 */
	sm_init(&sm);
	sm_step(&sm);
}

/*
 * Wait for system bootstrap to complete, all SVC_TYPE_RUNTASK must be
 * allowed to complete their work in [S], or timeout, before we switch
 * to the configured runlevel and call finalize(), should not take more
 * than 120 sec.
 */
static void bootstrap_worker(void *work)
{
	static int cnt = 120;
	static struct wq final = {
		.cb = finalize,
		.delay = 10
	};

	_d("Step all services ...");
	service_step_all(SVC_TYPE_ANY);

	if (cnt-- > 0 && !service_completed()) {
		_d("Not all bootstrap run/tasks have completed yet ... %d", cnt);
		schedule_work(work);
		return;
	}

	if (cnt > 0)
		_d("All run/task have completed, resuming bootstrap.");
	else
		_d("Timeout, resuming bootstrap.");

	_d("Starting runlevel change finalize ...");
	schedule_work(&final);

	/*
	 * Run startup scripts in the runparts directory, if any.
	 */
	if (runparts && fisdir(runparts) && !rescue)
		run_parts(runparts, NULL);

	/*
	 * Start all tasks/services in the configured runlevel
	 */
	_d("Change to default runlevel(%d), starting all services ...", cfglevel);
	service_runlevel(cfglevel);
}

static int version(int rc)
{
	puts(PACKAGE_STRING);
	printf("Bug report address: %-40s\n", PACKAGE_BUGREPORT);
#ifdef PACKAGE_URL
	printf("Project homepage: %s\n", PACKAGE_URL);
#endif

	return rc;
}

static int usage(int rc)
{
	printf("Usage: %s [OPTIONS] [q | Q | 0-9]\n\n"
	       "Options:\n"
//	       "  -a       Ignored, compat SysV init\n"
//	       "  -b       Ignored, compat SysV init\n"
//	       "  -e arg   Ignored, compat SysV init\n"
	       "  -h       This help text\n"
//	       "  -s       Ignored, compat SysV init\n"
//	       "  -t sec   Ignored, compat SysV init\n"
	       "  -v       Show Finit version\n"
//	       "  -z xxx   Ignored, compat SysV init\n"
	       "\n"
	       "Commands:\n"
	       "  0        Power-off the system, same as initctl poweroff\n"
	       "  6        Reboot the system, same as initctl reboot\n"
	       "  2-9      Change runlevel\n"
	       "  q, Q     Reload /etc/finit.conf and/or any *.conf in /etc/finit.d/\n"
	       "           if modified, same as initctl reload or SIGHUP to PID 1\n"
	       "  1, s, S  Enter system rescue mode, runlevel 1\n"
	       "\n", prognm);

	return rc;
}

/*
 * wrapper for old-style init/telinit commands, for compat with
 * /usr/bin/shutdown from sysvinit, and old fingers
 */
static int telinit(int argc, char *argv[])
{
	int c;

	progname(argv[0]);
	while ((c = getopt(argc, argv, "abe:h?st:vVz:")) != EOF) {
		switch(c) {
		case 'a': case 'b': case 'e': case 's': case 'z':
			break;		/* ign, compat */

		case 't':		/* optarg == killdelay */
			break;

		case 'v': case 'V':
			return version(0);

		case 'h':
		case '?':
			return usage(0);
		}
	}

	if (optind < argc) {
		int req = (int)argv[optind][0];

		if (isdigit(req))
			return systemf("initctl -b runlevel %c", req);

		if (req == 'q' || req == 'Q')
			return systemf("initctl -b reload");

		if (req == 's' || req == 'S')
			return systemf("initctl -b runlevel %c", req);
	}

	/* XXX: add non-pid1 process monitor here
	 *
	 *       finit -f ~/.config/finit.conf &
	 *
	 */

	return usage(1);
}

int main(int argc, char *argv[])
{
	struct wq crank = {
		.cb = crank_worker,
		.delay = 10
	};
	struct wq bootstrap = {
		.cb = bootstrap_worker,
		.delay = 100
	};
	uev_ctx_t loop;

	/* telinit or stand-alone process monitor */
	if (getpid() != 1)
		return telinit(argc, argv);

	/*
	 * Need /dev, /proc, and /sys for console=, remount and cgroups
	 */
	fs_init();

	/*
	 * Parse /proc/cmdline (debug, rescue, console=, etc.)
	 * Also calls log_init() to set correct log level
	 */
	conf_parse_cmdline(argc, argv);

	/*
	 * Figure out system console(s)
	 */
	console_init();

	/*
	 * Initalize event context.
	 */
	uev_init1(&loop, 1);
	ctx = &loop;

	/*
	 * Set PATH, SHELL, and PWD early to something sane
	 */
	setenv("PATH", _PATH_STDPATH, 1);
	setenv("SHELL", _PATH_BSHELL, 1);
	setenv("LOGNAME", "root", 1);
	setenv("USER", "root", 1);

	if (chdir("/"))
		_pe("Failed cd /");

	/*
	 * In case of emergency.
	 */
	if (rescue) {
		char *sulogin[] = {
			_PATH_SULOGIN,
			"sulogin",
		};
		size_t i;

		for (i = 0; i < NELEMS(sulogin); i++) {
			if (systemf(sulogin[i]))
				continue;

			rescue = 0;
			break;
		}
	}

	/*
	 * Load plugins early, the first hook is in banner(), so we
	 * need plugins loaded before calling it.
	 */
	plugin_init(&loop);

	/*
	 * Hello world.
	 */
	enable_progress(1);	/* Allow progress, if enabled */
	banner();

	/*
	 * Initial setup of signals, ignore all until we're up.
	 */
	sig_init();

	/*
	 * Initialize default control groups, if available
	 */
	cgroup_init(&loop);

	/* Check and mount filesystems. */
	fs_mount();

	/* Bootstrap conditions, needed for hooks */
	cond_init();

	/* Emit conditions for early hooks that ran before the
	 * condition system was initialized in case anyone . */
	cond_set_oneshot(plugin_hook_str(HOOK_BANNER));
	cond_set_oneshot(plugin_hook_str(HOOK_ROOTFS_UP));

	/*
	 * Initialize .conf system and load static /etc/finit.conf.
	 */
	conf_init(&loop);

	/*
	 * Start built-in watchdogd as soon as possible, if enabled
	 */
	if (whichp(FINIT_LIBPATH_ "/watchdogd") && fexist(WDT_DEVNODE)) {
		service_register(SVC_TYPE_SERVICE, "[123456789] cgroup.init name:watchdog :finit " FINIT_LIBPATH_ "/watchdogd -- Finit watchdog daemon", global_rlimit, NULL);
		wdog = svc_find_by_nameid("watchdog", "finit");
	}

	/*
	 * Start kernel event daemon as soon as possible, if enabled
	 */
	if (whichp(FINIT_LIBPATH_ "/keventd"))
		service_register(SVC_TYPE_SERVICE, "[123456789] cgroup.init " FINIT_LIBPATH_ "/keventd -- Finit kernel event daemon", global_rlimit, NULL);

	/* Base FS up, enable standard SysV init signals */
	sig_setup(&loop);

	_d("Base FS up, calling hooks ...");
	plugin_run_hooks(HOOK_BASEFS_UP);

	/*
	 * Set up inotify watcher for /etc/finit.conf, /etc/finit.d, and
	 * their deps, to figure out how to bootstrap the system.
	 */
	conf_monitor();

	_d("Starting initctl API responder ...");
	api_init(&loop);

	_d("Starting the big state machine ...");
	schedule_work(&crank);

	_d("Starting bootstrap finalize timer ...");
	schedule_work(&bootstrap);

	/*
	 * Enter main loop to monitor /dev/initctl and services
	 */
	_d("Entering main loop ...");
	return uev_run(&loop, 0);
}

/**
 * Local Variables:
 *  indent-tabs-mode: t
 *  c-file-style: "linux"
 * End:
 */
