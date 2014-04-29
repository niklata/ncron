/* ncron.cpp - secure, minimally-sleeping cron daemon
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

#include <memory>
#include <algorithm>

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

extern "C" {
#include "nk/log.h"
#include "nk/pidfile.h"
#include "nk/signals.h"
#include "nk/exec.h"
#include "nk/privilege.h"
#include "nk/copy_cmdarg.h"
}

#include "ncron.hpp"
#include "sched.hpp"
#include "cfg.hpp"
#include "crontab.hpp"
#include "rlimit.hpp"

#define CONFIG_FILE_DEFAULT "/var/lib/ncron/crontab"
#define EXEC_FILE_DEFAULT "/var/lib/ncron/exectimes"
#define PID_FILE_DEFAULT "/var/run/ncron.pid"
#define LOG_FILE_DEFAULT "/var/log/ncron.log"

#define NCRON_VERSION "0.99"

static volatile sig_atomic_t pending_save_and_exit;
static volatile sig_atomic_t pending_reload_config;
static volatile sig_atomic_t pending_free_children;

/* Time (in msec) to sleep before dispatching events at startup.
   Set to a nonzero value so as not to compete for cpu with init scripts at
   boot time. */
int g_initial_sleep = 0;

char g_ncron_conf[PATH_MAX] = CONFIG_FILE_DEFAULT;
char g_ncron_execfile[PATH_MAX] = EXEC_FILE_DEFAULT;
char g_ncron_pidfile[PATH_MAX] = PID_FILE_DEFAULT;
int g_ncron_execmode = 0;

static std::vector<std::unique_ptr<cronentry_t>> stack;
static std::vector<std::unique_ptr<cronentry_t>> deadstack;

static void reload_config(void)
{
    if (g_ncron_execmode != 2)
        save_stack(g_ncron_execfile, stack, deadstack);

    stack.clear();
    deadstack.clear();
    parse_config(g_ncron_conf, g_ncron_execfile, stack, deadstack); // XXX
    log_line("SIGHUP - Reloading config: %s.", g_ncron_conf);
    pending_reload_config = 0;
}

static void save_and_exit(void)
{
    if (g_ncron_execmode != 2) {
        save_stack(g_ncron_execfile, stack, deadstack);
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

static void fail_on_fdne(const char *file, const char *mode)
{
    if (file_exists(file, mode))
        exit(EXIT_FAILURE);
}

static void exec_and_fork(uid_t uid, gid_t gid, const std::string &command,
                          const std::string &args, const std::string &chroot,
                          rlimits *limits)
{
    switch ((int)fork()) {
        case 0:
            if (!chroot.empty())
                nk_set_chroot(chroot.c_str());
            if (limits && limits->enforce(uid, gid, command))
                suicide("%s: rlimits::enforce failed", __func__);
            if (gid) {
                if (setresgid(gid, gid, gid))
                    suicide("%s: setgid(%i) failed for \"%s\": %s",
                            __func__, gid, command.c_str(), strerror(errno));
                if (getgid() == 0)
                    suicide("%s: child is still gid=root after setgid()",
                            __func__);
            }
            if (uid) {
                if (setresuid(uid, uid, uid))
                    suicide("%s: setuid(%i) failed for \"%s\": %s",
                            __func__, uid, command.c_str(), strerror(errno));
                if (getuid() == 0)
                    suicide("%s: child is still uid=root after setuid()",
                            __func__);
                nk_fix_env(uid, true);
            }
            nk_execute(command.c_str(), args.c_str());
        case -1:
            suicide("%s: fork failed: %s", __func__, strerror(errno));
        default:
            break;
    }
}

static void sleep_or_die(struct timespec *ts, bool pending_save)
{
    struct timespec rem;
sleep:
    if (nanosleep(ts, &rem)) {
        switch (errno) {
            case EINTR:
                if (pending_free_children)
                    free_children();
                if (pending_save_and_exit)
                    save_and_exit();

                if (g_ncron_execmode == 1 || pending_save)
                    save_stack(g_ncron_execfile, stack, deadstack);

                if (pending_reload_config)
                    reload_config();

                memcpy(ts, &rem, sizeof(struct timespec));
                goto sleep;
            default:
                suicide("%s: nanosleep failed: %s", __func__, strerror(errno));
        }
    }
}

void clock_or_die(struct timespec *ts)
{
    if (clock_gettime(CLOCK_REALTIME, ts))
        suicide("%s: clock_gettime failed: %s", __func__, strerror(errno));
}

static void do_work(unsigned int initial_sleep)
{
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec=initial_sleep * 1000000;

    sleep_or_die(&ts, false);

    while (1) {
        log_line("%s: LOOP", __func__);
        bool pending_save = false;

        clock_or_die(&ts);

        while (stack.front()->exectime <= ts.tv_sec) {
            auto &i = *stack.front();
            log_line("%s: DISPATCH", __func__);

            exec_and_fork((uid_t)i.user, (gid_t)i.group,
                          i.command, i.args, i.chroot,
                          i.limits ? i.limits.get() : nullptr);

            i.numruns++;
            i.lasttime = ts.tv_sec;
            i.exectime = get_next_time(*stack.front());
            if (i.journal)
                pending_save = true;

            if ((i.numruns < i.maxruns || i.maxruns == 0)
                && i.exectime != 0) {
                std::make_heap(stack.begin(), stack.end(), GtCronEntry);
            } else {
                std::pop_heap(stack.begin(), stack.end(), GtCronEntry);
                deadstack.emplace_back(std::move(stack.back()));
                stack.pop_back();
            }
            if (stack.empty())
                save_and_exit();

            clock_or_die(&ts);
        }

        log_line("%s: ts.tv_sec = %lu  stack.front()->exectime = %lu",
                 __func__, ts.tv_sec, stack.front()->exectime);
        for (const auto &i: stack)
            log_line("%s: job %u exectime = %lu", __func__,
                     i->id, i->exectime);
        if (ts.tv_sec <= stack.front()->exectime) {
            struct timespec sts;
            sts.tv_sec = stack.front()->exectime - ts.tv_sec;
            sts.tv_nsec = 0;
            log_line("%s: SLEEP %lu seconds", __func__, sts.tv_sec);
            sleep_or_die(&sts, pending_save);
        }
    }
}

void show_usage(void)
{
    printf("ncron %s, secure cron/at daemon.\n", NCRON_VERSION);
    printf(
"Copyright (C) 2003-2014 Nicholas J. Kain\n"
"Usage: ncron [OPTIONS]\n"
"  -c, --config=FILE    use FILE as the configuration file\n"
"  -b, --background     fork to the background\n"
"  -s, --sleep=MSEC     time to wait in msec to wait before processing\n"
"                       jobs at startup\n");
    printf(
"  -0, --noexecsave     don't save execution history at all\n"
"  -j, --journal        saves exectimes at each job invocation\n"
"  -t, --crontab=FILE   use FILE for crontab info\n"
"  -H, --history=FILE   save execution history in FILE\n"
"  -p, --pidfile=FILE   write pid to FILE\n"
"  -q, --quiet          don't log to syslog\n"
"  -h, --help           print this help and exit\n"
"  -v, --version        print version information and exit\n"
          );
}

void print_version(void)
{
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
}

int main(int argc, char* argv[])
{
    parse_cmdline(argc, argv);

    fail_on_fdne(g_ncron_conf, "r");
    fail_on_fdne(g_ncron_execfile, "rw");
    fail_on_fdne(g_ncron_pidfile, "w");

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
    parse_config(g_ncron_conf, g_ncron_execfile, stack, deadstack);

    if (stack.empty())
        suicide("%s: no jobs, exiting", __func__);

    write_pid(g_ncron_pidfile);

    do_work(g_initial_sleep);
    exit(EXIT_SUCCESS);
}

