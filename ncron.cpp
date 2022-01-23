/* ncron.cpp - secure, minimally-sleeping cron daemon
 *
 * Copyright 2003-2022 Nicholas J. Kain <njkain at gmail dot com>
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
#include <getopt.h>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <nk/from_string.hpp>
extern "C" {
#include "nk/log.h"
#include "nk/pidfile.h"
#include "nk/signals.h"
}
#include "ncron.hpp"
#include "sched.hpp"
#include "crontab.hpp"
#include "rlimit.hpp"

#define CONFIG_FILE_DEFAULT "/var/lib/ncron/crontab"
#define EXEC_FILE_DEFAULT "/var/lib/ncron/exectimes"

#define NCRON_VERSION "0.99"

int gflags_debug;
static volatile sig_atomic_t pending_save_and_exit;
static volatile sig_atomic_t pending_reload_config;
static volatile sig_atomic_t pending_free_children;
static bool gflags_background{false};

/* Time (in msec) to sleep before dispatching events at startup.
   Set to a nonzero value so as not to compete for cpu with init scripts at
   boot time. */
static unsigned g_initial_sleep = 0;

static std::string g_ncron_conf(CONFIG_FILE_DEFAULT);
static std::string g_ncron_execfile(EXEC_FILE_DEFAULT);
static std::string pidfile;
static int g_ncron_execmode = 0;

static std::vector<StackItem> stack;
static std::vector<StackItem> deadstack;

static void reload_config(void)
{
    if (g_ncron_execmode != 2)
        save_stack(g_ncron_execfile, stack, deadstack);

    stack.clear();
    deadstack.clear();
    parse_config(g_ncron_conf, g_ncron_execfile, stack, deadstack);
    log_line("SIGHUP - Reloading config: %s.", g_ncron_conf.c_str());
    std::fflush(stdout);
    pending_reload_config = 0;
}

static void save_and_exit(void)
{
    if (g_ncron_execmode != 2) {
        save_stack(g_ncron_execfile, stack, deadstack);
        log_line("Saving stack to %s.", g_ncron_execfile.c_str());
    }
    log_line("Exited.");
    exit(EXIT_SUCCESS);
}

static void free_children(void)
{
    while (waitpid(-1, nullptr, WNOHANG) > 0);
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

static void fail_on_fdne(const std::string &file, int mode)
{
    if (access(file.c_str(), mode))
        exit(EXIT_FAILURE);
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
                log_line("%s: nanosleep failed: %s", __func__, strerror(errno));
                std::exit(EXIT_FAILURE);
        }
    }
}

void clock_or_die(struct timespec *ts)
{
    if (clock_gettime(CLOCK_REALTIME, ts)) {
        log_line("%s: clock_gettime failed: %s", __func__, strerror(errno));
        std::exit(EXIT_FAILURE);
    }
}

static inline void debug_stack_print(const struct timespec &ts) {
    if (!gflags_debug)
        return;
    log_line("do_work: ts.tv_sec = %lu  stack.front().exectime = %lu", ts.tv_sec, stack.front().exectime);
    for (const auto &i: stack)
        log_line("do_work: job %u exectime = %lu", i.ce->id, i.ce->exectime);
}

static void do_work(unsigned initial_sleep)
{
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec=initial_sleep * 1000000;

    sleep_or_die(&ts, false);

    while (1) {
        bool pending_save = false;

        clock_or_die(&ts);

        while (stack.front().exectime <= ts.tv_sec) {
            auto &i = *stack.front().ce;
            if (gflags_debug)
                log_line("do_work: DISPATCH");

            i.exec(ts);
            stack.front().exectime = i.exectime;
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

        debug_stack_print(ts);
        if (ts.tv_sec <= stack.front().exectime) {
            struct timespec sts;
            sts.tv_sec = stack.front().exectime - ts.tv_sec;
            sts.tv_nsec = 0;
            if (gflags_debug)
                log_line("do_work: SLEEP %zu seconds", sts.tv_sec);
            sleep_or_die(&sts, pending_save);
        }
    }
}

static void usage()
{
    printf("ncron " NCRON_VERSION ", cron/at daemon.\n"
           "Copyright 2003-2022 Nicholas J. Kain\n"
           "Usage: ncron [options]...\n\nOptions:\n"
           "--help         -h    Print usage and exit.\n"
           "--version      -v    Print version and exit.\n"
           "--background   -b    Run as a background daemon.\n"
           "--sleep        -s [] Initial sleep time in seconds.\n"
           "--noexecsave   -0    Don't save execution history at all.\n"
           "--journal      -j    Save exectimes at each job invocation.\n"
           "--crontab      -t [] Path to crontab file.\n"
           "--history      -H [] Path to execution history file.\n"
           "--pidfile      -f [] Path to process id file.\n"
           "--verbose      -V    Log diagnostic information.\n"
    );
}

static void print_version()
{
    log_line("ncron " NCRON_VERSION ", cron/at daemon.\n"
             "Copyright 2003-2022 Nicholas J. Kain\n"
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
             "POSSIBILITY OF SUCH DAMAGE.");
}

static void process_options(int ac, char *av[])
{
    static struct option long_options[] = {
        {"help", 0, (int *)0, 'h'},
        {"version", 0, (int *)0, 'v'},
        {"background", 0, (int *)0, 'b'},
        {"sleep", 1, (int *)0, 's'},
        {"noexecsave", 0, (int *)0, '0'},
        {"journal", 0, (int *)0, 'j'},
        {"crontab", 1, (int *)0, 't'},
        {"history", 1, (int *)0, 'H'},
        {"pidfile", 1, (int *)0, 'f'},
        {"verbose", 0, (int *)0, 'V'},
        {(const char *)0, 0, (int *)0, 0 }
    };
    for (;;) {
        auto c = getopt_long(ac, av, "hvbs:0jt:H:V", long_options, (int *)0);
        if (c == -1) break;
        switch (c) {
            case 'h': usage(); std::exit(EXIT_SUCCESS); break;
            case 'v': print_version(); std::exit(EXIT_SUCCESS); break;
            case 'b': gflags_background = true; break;
            case 's': if (auto t = nk::from_string<unsigned>(optarg)) g_initial_sleep = *t; else {
                          log_line("invalid sleep '%s' specified", optarg);
                          std::exit(EXIT_FAILURE);
                      }
                      break;
            case '0': g_ncron_execmode = 2; break;
            case 'j': g_ncron_execmode = 1; break;
            case 't': g_ncron_conf = optarg; break;
            case 'H': g_ncron_execfile = optarg; break;
            case 'f': pidfile = optarg; break;
            case 'V': gflags_debug = 1; break;
            default: break;
        }
    }
}

int main(int argc, char* argv[])
{
    process_options(argc, argv);
    fail_on_fdne(g_ncron_conf, R_OK);
    fail_on_fdne(g_ncron_execfile, R_OK | W_OK);
    parse_config(g_ncron_conf, g_ncron_execfile, stack, deadstack);

    if (stack.empty()) {
        log_line("%s: no jobs, exiting", __func__);
        std::exit(EXIT_FAILURE);
    }

    if (gflags_background) {
        if (daemon(0,0)) {
            log_line("%s: daemon failed: %s", __func__, strerror(errno));
            std::exit(EXIT_FAILURE);
        }
    }

    if (pidfile.size())
        write_pid(pidfile.c_str());

    umask(077);
    fix_signals();

#ifdef __linux__
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);
    prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
#endif

    do_work(g_initial_sleep);
    exit(EXIT_SUCCESS);
}

