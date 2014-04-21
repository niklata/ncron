/* ncron.c - secure, minimally-sleeping cron daemon
 *
 * (c) 2003-2014 Nicholas J. Kain <njkain at gmail dot com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * - Redistributions of source code must retain the above copyright notice,
 *   this list of conditions and the following disclaimer.
 *
 * - Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#define _GNU_SOURCE
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
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
#include <limits.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <getopt.h>

#include "nk/log.h"
#include "nk/pidfile.h"
#include "nk/signals.h"
#include "nk/exec.h"
#include "nk/privilege.h"
#include "nk/copy_cmdarg.h"

#include "sched.h"
#include "crontab.h"
#include "rlimit.h"

#define CONFIG_FILE_DEFAULT "/var/lib/ncron/crontab"
#define EXEC_FILE_DEFAULT "/var/lib/ncron/exectimes"
#define PID_FILE_DEFAULT "/var/run/ncron.pid"
#define LOG_FILE_DEFAULT "/var/log/ncron.log"

#define NCRON_VERSION "0.99"

static volatile sig_atomic_t pending_save_and_exit = 0;
static volatile sig_atomic_t pending_reload_config = 0;
static volatile sig_atomic_t pending_free_children = 0;

static char g_ncron_conf[PATH_MAX] = CONFIG_FILE_DEFAULT;
static char g_ncron_execfile[PATH_MAX] = EXEC_FILE_DEFAULT;
static int g_ncron_execmode = 0;

static void reload_config(cronentry_t **stack, cronentry_t **deadstack)
{
    if (g_ncron_execmode != 2)
        save_stack(g_ncron_execfile, *stack, *deadstack);

    free_stack(stack);
    free_stack(deadstack);
    parse_config(g_ncron_conf, g_ncron_execfile, stack, deadstack);
    log_line("SIGHUP - Reloading config: %s.", g_ncron_conf);
    pending_reload_config = 0;
}

static void save_and_exit(cronentry_t **stack, cronentry_t **deadstack)
{
    if (g_ncron_execmode != 2) {
        save_stack(g_ncron_execfile, *stack, *deadstack);
        log_line("Saving stack to %s.", g_ncron_execfile);
    }
    log_line("Exited.");
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
    if (file_exists(file, mode))
        exit(EXIT_FAILURE);
}

static void exec_and_fork(uid_t uid, gid_t gid, char *command, char *args,
                          char *chroot, limit_t *limits)
{
    switch ((int)fork()) {
        case 0:
            if (chroot)
                nk_set_chroot(chroot);
            if (enforce_limits(limits, uid, gid, command))
                suicide("%s: enforce_limits failed", __func__);
            if (gid) {
                if (setresgid(gid, gid, gid))
                    suicide("%s: setgid(%i) failed for \"%s\": %s",
                            __func__, gid, command, strerror(errno));
                if (getgid() == 0)
                    suicide("%s: child is still gid=root after setgid()",
                            __func__);
            }
            if (uid) {
                if (setresuid(uid, uid, uid))
                    suicide("%s: setuid(%i) failed for \"%s\": %s",
                            __func__, uid, command, strerror(errno));
                if (getuid() == 0)
                    suicide("%s: child is still uid=root after setuid()",
                            __func__);
                nk_fix_env(uid, true);
            }
            nk_execute(command, args);
        case -1:
            suicide("%s: fork failed: %s", __func__, strerror(errno));
        default:
            break;
    }
}

static void sleep_or_die(struct timespec *ts, cronentry_t **stack,
                         cronentry_t **deadstack, bool pending_save)
{
    struct timespec rem;
sleep:
    if (nanosleep(ts, &rem)) {
        switch (errno) {
            case EINTR:
                if (pending_free_children)
                    free_children();
                if (pending_save_and_exit)
                    save_and_exit(stack, deadstack);

                if (g_ncron_execmode == 1 || pending_save)
                    save_stack(g_ncron_execfile, *stack, *deadstack);

                if (pending_reload_config)
                    reload_config(stack, deadstack);

                memcpy(ts, &rem, sizeof(struct timespec));
                goto sleep;
            default:
                suicide("%s: nanosleep failed: %s", __func__, strerror(errno));
        }
    }
}

static void clock_or_die(struct timespec *ts)
{
    if (clock_gettime(CLOCK_REALTIME, ts))
        suicide("%s: clock_gettime failed: %s", __func__, strerror(errno));
}

static void do_work(unsigned int initial_sleep, cronentry_t *stack,
                    cronentry_t *deadstack)
{
    struct timespec ts = { .tv_sec=initial_sleep, .tv_nsec=0 };

    sleep_or_die(&ts, &stack, &deadstack, false);

    while (1) {
        bool pending_save = false;

        clock_or_die(&ts);

        while (stack->exectime <= ts.tv_sec) {
            cronentry_t *t;

            exec_and_fork((uid_t)stack->user, (gid_t)stack->group,
                    stack->command, stack->args, stack->chroot, stack->limits);

            stack->numruns++;
            stack->lasttime = ts.tv_sec;
            stack->exectime = get_next_time(stack);
            if (stack->journal)
                pending_save = true;

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
                save_and_exit(&stack, &deadstack);

            clock_or_die(&ts);
        }

        if (ts.tv_sec <= stack->exectime) {
            struct timespec sts = { .tv_sec = stack->exectime - ts.tv_sec };
            sleep_or_die(&sts, &stack, &deadstack, pending_save);
        }
    }
}

int main(int argc, char** argv)
{
    /* Time (in seconds) to sleep before dispatching events at startup.
       Set this macro to a nonzero value so as not to compete for cpu with init
       scripts at boot time. */
    int initial_sleep = 0;
    int c;
    char pidfile[PATH_MAX] = PID_FILE_DEFAULT;

    cronentry_t *stack = NULL, *deadstack = NULL;

    while (1) {
        int option_index = 0;
        static struct option long_options[] = {
            {"detach", 0, 0, 'b'},
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

        c = getopt_long(argc, argv, "bs:c:f:p:0jqhv",
                long_options, &option_index);
        if (c == -1)
            break;

        switch (c) {

            case 'h':
                printf("ncron %s, secure cron/at daemon.\n", NCRON_VERSION);
                printf(
                       "Copyright (C) 2003-2014 Nicholas J. Kain\n"
                       "Usage: ncron [OPTIONS]\n"
                       "  -b, --background     fork to the background\n"
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
                      );
                exit(EXIT_FAILURE);
                break;

            case 'v':
                printf("ncron %s, secure single-user cron.\n", NCRON_VERSION);
                printf(
                       "Copyright (c) 2003-2014 Nicholas J. Kain\n"
                       "All rights reserved.\n\n"
                       "Redistribution and use in source and binary forms, with or without\n"
                       "modification, are permitted provided that the following conditions are met:\n\n"
                       "- Redistributions of source code must retain the above copyright notice,\n"
                       "  this list of conditions and the following disclaimer.\n"
                       "- Redistributions in binary form must reproduce the above copyright notice,\n"
                       "  this list of conditions and the following disclaimer in the documentation\n"
                       "  and/or other materials provided with the distribution.\n\n"
                       "THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \"AS IS\"\n"
                       "AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE\n"
                       "IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE\n"
                       "ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE\n"
                       "LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR\n"
                       "CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF\n"
                       "SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS\n"
                       "INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN\n"
                       "CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)\n"
                       "ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE\n"
                       "POSSIBILITY OF SUCH DAMAGE.\n"
                        );
                exit(EXIT_FAILURE);
                break;

            case 'b':
                gflags_detach = 1;
                break;

            case 's':
                initial_sleep = atoi(optarg);
                if (initial_sleep < 0)
                    initial_sleep = 0;
                break;

            /*
             * g_ncron_execmode=0: default, save exectimes on exit
             * g_ncron_execmode=1: journal, save exectimes at job invocation
             * g_ncron_execmode=2: none, don't save exectimes at all
             */
            case '0':
                g_ncron_execmode = 2;
                break;

            case 'j':
                g_ncron_execmode = 1;
                break;

            case 'q':
                gflags_quiet = 1;
                break;

            case 'c':
                copy_cmdarg(g_ncron_conf, optarg, sizeof g_ncron_conf,
                            "conf");
                break;

            case 'f':
                copy_cmdarg(g_ncron_execfile, optarg, sizeof g_ncron_execfile,
                            "history");
                break;

            case 'p':
                copy_cmdarg(pidfile, optarg, sizeof pidfile, "pidfile");
                break;
        }
    }

    fail_on_fdne(g_ncron_conf, "r");
    fail_on_fdne(g_ncron_execfile, "rw");
    fail_on_fdne(pidfile, "w");

    if (gflags_detach != 0) {
        if (daemon(0,0))
            suicide("%s: daemon failed: %s", __func__, strerror(errno));
    }

    umask(077);

#ifdef __linux__
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);
    prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
#endif

    fix_signals();
    parse_config(g_ncron_conf, g_ncron_execfile, &stack, &deadstack);

    if (stack == NULL)
        suicide("%s: no jobs, exiting", __func__);

    write_pid(pidfile);

    do_work(initial_sleep, stack, deadstack);
    exit(EXIT_SUCCESS);
}

