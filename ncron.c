/*
 * ncron.c - secure and minimalistic single user cron daemon
 *
 * (C) 2003-2007 Nicholas J. Kain <njk@aerifal.cx>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation.
 *
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <signal.h>
#include <errno.h>

#ifdef LINUX
#include <linux/prctl.h>
#endif

#define _GNU_SOURCE
#include <getopt.h>

#include "defines.h"
#include "log.h"
#include "sched.h"
#include "config.h"
#include "exec.h"
#include "nstrl.h"

static volatile sig_atomic_t pending_save_and_exit = 0;
static volatile sig_atomic_t pending_reload_config = 0;
static volatile sig_atomic_t pending_free_children = 0;
static int pending_save = 0;

static void write_pid(char *file)
{
    FILE *f;
    char buf[MAXLINE];

    f = fopen(file, "w");
    if (f == NULL) {
        log_line("FATAL - failed to open pid file \"%s\"!\n", file);
        exit(EXIT_FAILURE);
    }

    snprintf(buf, sizeof buf, "%i", (unsigned int)getpid());
    fwrite(buf, sizeof (char), strlen(buf), f);

    if (fclose(f) != 0) {
        log_line("FATAL - failed to close pid file \"%s\"!\n", file);
        exit(EXIT_FAILURE);
    }
}

static void reload_config(char *conf, char *execfile, cronentry_t **stack,
        cronentry_t **deadstack, int execmode)
{
    if (execmode != 2) save_stack(execfile, *stack, *deadstack);

    free_stack(stack);
    free_stack(deadstack);
    parse_config(conf, execfile, stack, deadstack);
    log_line("SIGHUP - Reloading config: %s.\n", conf);
    pending_reload_config = 0;
}

static void save_and_exit(char *execfile, cronentry_t **stack, cronentry_t
        **deadstack, int execmode)
{
    if (execmode != 2) {
        save_stack(execfile, *stack, *deadstack);
        log_line("Saving stack to %s.\n", execfile);
    }
    log_line("Exited.\n");
    exit(EXIT_SUCCESS);
}

static void free_children(void)
{
    while (waitpid(-1, NULL, WNOHANG) > 0);
    pending_free_children = 0;
}

static void sighandler(int sig)
{
    switch (sig) {
        case SIGTERM:
        case SIGINT:
            pending_save_and_exit = 1;
            break;
        case SIGHUP:
            pending_reload_config = 1;
            break;
        case SIGCHLD:
            pending_free_children = 1;
            break;
    }
}

static void hook_signal(int signum, void (*fn)(int), int flags)
{
    struct sigaction new_action;

    new_action.sa_handler = fn;
    sigemptyset(&new_action.sa_mask);
    new_action.sa_flags = flags;

    if (sigaction(signum, &new_action, NULL)) {
        log_line("FATAL - failed to hook signal %i\n", signum);
        exit(EXIT_FAILURE);
    }
}

static void disable_signal(int signum)
{
    struct sigaction new_action;

    new_action.sa_handler = SIG_IGN;
    sigemptyset(&new_action.sa_mask);

    if (sigaction(signum, &new_action, NULL)) {
        log_line("FATAL - failed to ignore signal %i\n", signum);
        exit(EXIT_FAILURE);
    }
}

static void fix_signals(void)
{
    disable_signal(SIGPIPE);
    disable_signal(SIGUSR1);
    disable_signal(SIGUSR2);
    disable_signal(SIGTSTP);
    disable_signal(SIGTTIN);

    hook_signal(SIGCHLD, sighandler, SA_NOCLDSTOP);
    hook_signal(SIGHUP, sighandler, 0);
    hook_signal(SIGINT, sighandler, 0);
    hook_signal(SIGTERM, sighandler, 0);
}

static void fail_on_fdne(char *file, char *mode)
{
    FILE *f;

    if (file == NULL || mode == NULL) {
        log_line("fail_on_fdne: FATAL - coding bug: NULL passed\n");
        exit(EXIT_FAILURE);
    }

    f = fopen(file, mode);
    if (f == NULL) {
        log_line("FATAL - can't open file %s with mode %s!\n",
                file, mode);
        exit(EXIT_FAILURE); 
    }
    fclose(f);
}

int main(int argc, char** argv)
{
    /* Time (in seconds) to sleep before dispatching events at startup.
       Set this macro to a nonzero value so as not to compete for cpu with init
       scripts at boot time. */
    int initial_sleep = 1;
    int c, tmp;
    char pidfile[MAX_PATH_LENGTH] = PID_FILE_DEFAULT;

    cronentry_t *stack = NULL, *deadstack = NULL;
    int execmode = 0;
    char conf[MAX_PATH_LENGTH] = CONFIG_FILE_DEFAULT;
    char execfile[MAX_PATH_LENGTH] = EXEC_FILE_DEFAULT;

    time_t curtime;
    cronentry_t *t;

    while (1) {
        int option_index = 0;
        static struct option long_options[] = {
            {"detach", 0, 0, 'd'},
            {"nodetach", 0, 0, 'n'},
            {"sleep", 1, 0, 's'},
            {"conf", 1, 0, 'c'},
            {"history", 1, 0, 'f'},
            {"pidfile", 1, 0, 'p'},
            {"noexecsave", 0, 0, '0'},
            {"journal", 0, 0, 'j'},
            {"quiet", 0, 0, 'q'},
            {"help", 0, 0, 'h'},
            {"version", 0, 0, 'v'},
            {0, 0, 0, 0}
        };

        c = getopt_long(argc, argv, "dns:c:f:p:0jqhv",
                long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {

            case 'h':
                printf("ncron %s, secure cron/at daemon.  Licensed under GNU LGPL.\n", NCRON_VERSION);
                printf(
                        "Copyright (C) 2003-2007 Nicholas J. Kain\n"
                        "Usage: ncron [OPTIONS]\n"
                        "  -d, --detach         detach from foreground and daemonize\n"
                        "  -n, --nodetach       stay in foreground\n"
                        "  -s, --sleep          time to wait in seconds to wait before processing\n"
                        "                         jobs at startup\n");
                printf(
                        "  -0, --noexecsave     don't save execution history at all\n"
                        "  -j, --journal        saves exectimes at each job invocation\n"
                        "  -c, --conf=FILE      use FILE for configuration info\n"
                        "  -f, --history=FILE   save execution history in FILE\n"
                        "  -p, --pidfile=FILE   write pid to FILE\n"
                        "  -q, --quiet          don't log to syslog\n"
                        "  -h, --help           print this help and exit\n"
                        "  -v, --version        print version information and exit\n"
                        "Read documentation and FAQ for more information.\n");
                exit(EXIT_FAILURE);
                break;

            case 'v':
                printf("ncron %s, secure single-user cron.  Licensed under GNU LGPL.\n", NCRON_VERSION);
                printf(
                        "Copyright (C) 2003-2007 Nicholas J. Kain\n"
                        "This is free software; see the source for copying conditions.  There is NO\n"
                        "WARRANTY; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.\n");
                exit(EXIT_FAILURE);
                break;

            case 'd':
                gflags_detach = 1;
                break;

            case 'n':
                gflags_detach = 0;
                break;

            case 's':
                initial_sleep = atoi(optarg);
                if (initial_sleep < 0)
                    initial_sleep = 0;
                break;

            /*
             * execmode=0: default, save exectimes on exit
             * execmode=1: journal, save exectimes on each job invocation
             * execmode=2: none, don't save exectimes at all
             */
            case '0':
                execmode = 2;
                break;

            case 'j':
                execmode = 1;
                break;

            case 'q':
                gflags_quiet = 1;
                break;

            case 'c':
                strlcpy(conf, optarg, MAX_PATH_LENGTH);
                break;

            case 'f':
                strlcpy(execfile, optarg, MAX_PATH_LENGTH);
                break;

            case 'p':
                strlcpy(pidfile, optarg, MAX_PATH_LENGTH);
                break;
        }
    }

    fail_on_fdne(conf, "r");
    fail_on_fdne(execfile, "rw");
    fail_on_fdne(pidfile, "w");

    if (gflags_detach != 0) {
        if (daemon(0,0)) {
            log_line("FATAL - detaching fork failed\n");
            exit(EXIT_FAILURE);
        }
    }

    umask(077);

#ifdef LINUX
    prctl(PR_SET_DUMPABLE, 0);
    prctl(PR_SET_KEEPCAPS, 0);
#endif

    fix_signals();
    parse_config(conf, execfile, &stack, &deadstack);

    if (stack == NULL) {
        log_line("FATAL - no jobs, exiting\n");
        exit(EXIT_FAILURE);
    }

    write_pid(pidfile);

    /* initial sleep time is NOT guaranteed; if awakened, prog will proceed */
    if (initial_sleep < 0)
        initial_sleep = 0;
    sleep((unsigned int)initial_sleep);

    while (1) {
        curtime = time(NULL);

        while (stack->exectime <= curtime) {
            exec_and_fork((uid_t)stack->user, (gid_t)stack->group,
                    stack->command, stack->args, stack->chroot, stack->limits);

            stack->numruns++;
            stack->lasttime = curtime;
            stack->exectime = get_next_time(stack);
            if (stack->journal)
                pending_save = 1;

            if ((stack->numruns < stack->maxruns || stack->maxruns == 0)
                    && stack->exectime != 0) {
                /* reinsert in sorted stack at the right place for next run */
                if (stack->next) {
                    t = stack;
                    stack = stack->next;
                    t->next = NULL;
                    stack_insert(t, &stack);
                }
            } else {
                /* remove the job */
                t = stack;
                stack = stack->next;
                stack_insert(t, &deadstack);
            }
            if (!stack)
                save_and_exit(execfile, &stack, &deadstack, execmode);

            curtime = time(NULL);
        }

        if (pending_free_children)
            free_children();
        if (pending_save_and_exit)
            save_and_exit(execfile, &stack, &deadstack, execmode);

        if (execmode == 1 || pending_save) {
            save_stack(execfile, stack, deadstack);
            pending_save = 0;
        }

        if (pending_reload_config)
            reload_config(conf, execfile, &stack, &deadstack, execmode);

        tmp = (int)(stack->exectime - curtime);
        if (tmp < 0)
            tmp = 0;

        sleep((unsigned int)tmp);
    }
}
