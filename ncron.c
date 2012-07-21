/* ncron.c - secure, minimally-sleeping cron daemon
 *
 * (c) 2003-2012 Nicholas J. Kain <njkain at gmail dot com>
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
#include "chroot.h"
#include "rlimit.h"
#include "strl.h"

#define CONFIG_FILE_DEFAULT "/var/lib/ncron/crontab"
#define EXEC_FILE_DEFAULT "/var/lib/ncron/exectimes"
#define PID_FILE_DEFAULT "/var/run/ncron.pid"
#define LOG_FILE_DEFAULT "/var/log/ncron.log"

#define NCRON_VERSION "0.99"

static volatile sig_atomic_t pending_save_and_exit = 0;
static volatile sig_atomic_t pending_reload_config = 0;
static volatile sig_atomic_t pending_free_children = 0;

static char g_ncron_conf[MAX_PATH_LENGTH] = CONFIG_FILE_DEFAULT;
static char g_ncron_execfile[MAX_PATH_LENGTH] = EXEC_FILE_DEFAULT;
static int g_ncron_execmode = 0;

static void write_pid(char *file)
{
    FILE *f;
    char buf[MAXLINE];
    size_t bsize;

    f = fopen(file, "w");
    if (!f)
        suicide("%s: fopen(%s) failed: %s", __func__, file, strerror(errno));

    snprintf(buf, sizeof buf, "%i", (unsigned int)getpid());
    bsize = strlen(buf);
    while (!fwrite(buf, bsize, 1, f)) {
         if (ferror(f))
             break;
    }

    if (fclose(f))
        suicide("%s: fclose(%s) failed: %s", __func__, file, strerror(errno));
}

static void reload_config(cronentry_t **stack, cronentry_t **deadstack)
{
    if (g_ncron_execmode != 2)
        save_stack(g_ncron_execfile, *stack, *deadstack);

    free_stack(stack);
    free_stack(deadstack);
    parse_config(g_ncron_conf, g_ncron_execfile, stack, deadstack);
    log_line("SIGHUP - Reloading config: %s.\n", g_ncron_conf);
    pending_reload_config = 0;
}

static void save_and_exit(cronentry_t **stack, cronentry_t **deadstack)
{
    if (g_ncron_execmode != 2) {
        save_stack(g_ncron_execfile, *stack, *deadstack);
        log_line("Saving stack to %s.\n", g_ncron_execfile);
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

    if (sigaction(signum, &new_action, NULL))
        suicide("%s: sigaction(%d, ...) failed: %s", __func__, signum,
                strerror(errno));
}

static void disable_signal(int signum)
{
    struct sigaction new_action;

    new_action.sa_handler = SIG_IGN;
    sigemptyset(&new_action.sa_mask);

    if (sigaction(signum, &new_action, NULL))
        suicide("%s: sigaction(%d, ...) failed: %s", __func__, signum,
                strerror(errno));
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

    if (!file || !mode)
        suicide("%s: file or mode were NULL", __func__);

    f = fopen(file, mode);
    if (!f)
        suicide("%s: fopen(%s, %o) failed: %s", __func__, file, mode,
                strerror(errno));
    if (fclose(f))
        suicide("%s: fclose(%s) failed: %s", __func__, file, strerror(errno));
}

static void exec_and_fork(uid_t uid, gid_t gid, char *command, char *args,
                          char *chroot, limit_t *limits)
{
    switch ((int)fork()) {
        case 0:
            imprison(chroot);
            if (enforce_limits(limits, uid, gid, command))
                suicide("%s: enforce_limits failed", __func__);
            if (gid != 0) {
                if (setgid(gid))
                    suicide("%s: setgid(%i) failed for \"%s\"",
                             __func__, gid, command);
                if (getgid() == 0)
                    suicide("%s: child is still gid=root after setgid()",
                            __func__);
            }
            if (uid != 0) {
                if (setuid(uid))
                    suicide("%s: setuid(%i) failed for \"%s\"",
                             __func__, uid, command);
                if (getuid() == 0)
                    suicide("%s: child is still uid=root after setuid()",
                            __func__);
                ncm_fix_env(uid, true); // sanitize environment
            }
            ncm_execute(command, args);
            suicide("%s: execl failed: %s", __func__, strerror(errno));
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
    struct timespec ts = {0};

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
    int initial_sleep = 1;
    int c;
    char pidfile[MAX_PATH_LENGTH] = PID_FILE_DEFAULT;

    cronentry_t *stack = NULL, *deadstack = NULL;

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
                printf("ncron %s, secure cron/at daemon.\n", NCRON_VERSION);
                printf(
                       "Copyright (C) 2003-2012 Nicholas J. Kain\n"
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
                      );
                exit(EXIT_FAILURE);
                break;

            case 'v':
                printf("ncron %s, secure single-user cron.\n", NCRON_VERSION);
                printf(
                       "Copyright (c) 2003-2012 Nicholas J. Kain\n"
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
                strlcpy(g_ncron_conf, optarg, MAX_PATH_LENGTH);
                break;

            case 'f':
                strlcpy(g_ncron_execfile, optarg, MAX_PATH_LENGTH);
                break;

            case 'p':
                strlcpy(pidfile, optarg, MAX_PATH_LENGTH);
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

#ifdef LINUX
    prctl(PR_SET_DUMPABLE, 0);
    prctl(PR_SET_KEEPCAPS, 0);
#endif

    fix_signals();
    parse_config(g_ncron_conf, g_ncron_execfile, &stack, &deadstack);

    if (stack == NULL)
        suicide("%s: no jobs, exiting", __func__);

    write_pid(pidfile);

    do_work(initial_sleep, stack, deadstack);
    exit(EXIT_SUCCESS);
}

