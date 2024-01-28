// Copyright 2003-2024 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#include <algorithm>

#include <unistd.h>
#include <stdio.h>

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
#include <nk/defer.hpp>
extern "C" {
#include "nk/log.h"
#include "nk/io.h"
}
#include "ncron.hpp"
#include "sched.hpp"
#include "crontab.hpp"

#define CONFIG_FILE_DEFAULT "/var/lib/ncron/crontab"
#define EXEC_FILE_DEFAULT "/var/lib/ncron/exectimes"

#define NCRON_VERSION "2.0"

int gflags_debug;
static volatile sig_atomic_t pending_save_and_exit;
static volatile sig_atomic_t pending_reload_config;
static int s6_notify_fd = -1;

/* Time (in msec) to sleep before dispatching events at startup.
   Set to a nonzero value so as not to compete for cpu with init scripts at
   boot time. */
static unsigned g_initial_sleep = 0;

static char const *g_ncron_conf = CONFIG_FILE_DEFAULT;
static char const *g_ncron_execfile = EXEC_FILE_DEFAULT;
static char const *g_ncron_execfile_tmp = EXEC_FILE_DEFAULT "~";
enum class Execmode
{
    normal = 0,
    journal,
    nosave,
};
static Execmode g_ncron_execmode = Execmode::normal;

static std::vector<StackItem> stack;
static std::vector<StackItem> deadstack;

[[nodiscard]] static bool save_stack()
{
    auto f = fopen(g_ncron_execfile_tmp, "w");
    if (!f) {
        log_line("%s: failed to open history file %s for write", __func__, g_ncron_execfile_tmp);
        return false;
    }
    auto do_save = [&f](const std::vector<StackItem> &s) -> bool {
        for (auto &i: s) {
            const auto &j = g_jobs[i.jidx];
            if (fprintf(f, "%d=%li:%u|%lu\n", j.id, j.exectime, j.numruns, j.lasttime) < 0) {
                log_line("%s: failed writing to history file %s", __func__, g_ncron_execfile_tmp);
                return false;
            }
        }
        return true;
    };
    nk::scope_guard remove_ftmp = []{ unlink(g_ncron_execfile_tmp); };
    {
        defer [&f]{ fclose(f); };
        if (!do_save(stack)) return false;
        if (!do_save(deadstack)) return false;
    }

    if (rename(g_ncron_execfile_tmp, g_ncron_execfile)) {
        log_line("%s: failed to update to new history file (%s => %s): %s", __func__,
                 g_ncron_execfile_tmp, g_ncron_execfile, strerror(errno));
        return false;
    }
    remove_ftmp.dismiss();
    return true;
}

static void reload_config(void)
{
    if (g_ncron_execmode != Execmode::nosave) {
        if (!save_stack()) {
            log_line("SIGHUP - Failed to save exectimes; some jobs may run again.");
        }
    }

    g_jobs.clear();
    stack.clear();
    deadstack.clear();
    parse_config(g_ncron_conf, g_ncron_execfile, &stack, &deadstack);
    log_line("SIGHUP - Reloading config: %s.", g_ncron_conf);
    fflush(stdout);
    pending_reload_config = 0;
}

static void save_and_exit(void)
{
    if (g_ncron_execmode != Execmode::nosave) {
        if (save_stack()) {
            log_line("Saved stack to %s.", g_ncron_execfile);
        } else {
            log_line("Failed to save stack to %s; some jobs may run again.",
                     g_ncron_execfile);
        }
    }
    log_line("Exited.");
    exit(EXIT_SUCCESS);
}

static void signal_handler(int sig)
{
    int serrno = errno;
    if (sig == SIGTERM || sig == SIGINT) {
        pending_save_and_exit = 1;
    } else if (sig == SIGHUP) {
        pending_reload_config = 1;
    }
    errno = serrno;
}

static void fix_signals(void)
{
    static const int ss[] = {
        SIGHUP, SIGINT, SIGTERM, SIGKILL
    };
    sigset_t mask;
    if (sigprocmask(0, 0, &mask) < 0)
        suicide("sigprocmask failed");
    for (int i = 0; ss[i] != SIGKILL; ++i)
        if (sigdelset(&mask, ss[i]))
            suicide("sigdelset failed");
    if (sigaddset(&mask, SIGPIPE))
        suicide("sigaddset failed");
    if (sigprocmask(SIG_SETMASK, &mask, nullptr) < 0)
        suicide("sigprocmask failed");

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = signal_handler;
    sa.sa_flags = SA_RESTART;
    if (sigemptyset(&sa.sa_mask))
        suicide("sigemptyset failed");
    for (int i = 0; ss[i] != SIGKILL; ++i)
        if (sigaction(ss[i], &sa, NULL))
            suicide("sigaction failed");
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = SA_NOCLDWAIT;
    if (sigaction(SIGCHLD, &sa, NULL))
        suicide("sigaction failed");
}

static void fail_on_fdne(char const *file, int mode)
{
    if (access(file, mode))
        exit(EXIT_FAILURE);
}

static void sleep_or_die(struct timespec *ts)
{
retry:
    auto r = clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, ts, NULL);
    if (r) {
        if (r == EINTR) {
            if (pending_save_and_exit) save_and_exit();
            if (pending_reload_config) reload_config();
            goto retry;
        }
        log_line("%s: clock_nanosleep failed: %s", __func__, strerror(r));
        exit(EXIT_FAILURE);
    }
}

void clock_or_die(struct timespec *ts)
{
    if (clock_gettime(CLOCK_REALTIME, ts)) {
        log_line("%s: clock_gettime failed: %s", __func__, strerror(errno));
        exit(EXIT_FAILURE);
    }
}

static inline void debug_stack_print(const struct timespec &ts) {
    if (!gflags_debug)
        return;
    log_line("do_work: ts.tv_sec = %lu  stack.front().exectime = %lu", ts.tv_sec, g_jobs[stack.front().jidx].exectime);
    for (const auto &i: stack)
        log_line("do_work: job %d exectime = %lu", g_jobs[i.jidx].id, g_jobs[i.jidx].exectime);
}

static void do_work(unsigned initial_sleep)
{
    struct timespec ts;
    clock_or_die(&ts);
    ts.tv_sec += initial_sleep / 1000;
    ts.tv_nsec += (initial_sleep % 1000) * 1000000;

    bool pending_save = false;
    for (;;) {
        if (pending_save) {
            if (!save_stack()) {
                log_line("Failed to save stack to %s for a journalled job.",
                         g_ncron_execfile);
            } else {
                pending_save = false;
            }
        }
        sleep_or_die(&ts);

        while (g_jobs[stack.front().jidx].exectime <= ts.tv_sec) {
            auto &i = g_jobs[stack.front().jidx];
            if (gflags_debug)
                log_line("do_work: DISPATCH %d (%lu <= %lu)", i.id, i.exectime, ts.tv_sec);

            i.exec(ts);
            if (i.journal || g_ncron_execmode == Execmode::journal)
                pending_save = true;

            if ((i.numruns < i.maxruns || i.maxruns == 0) && i.exectime != 0) {
                if (stack.size() > 1) {
                    StackItem t = stack.front();
                    stack.erase(stack.begin());
                    stack.insert(std::upper_bound(stack.begin(), stack.end(), t, LtCronEntry), t);
                }
            } else {
                deadstack.emplace_back(stack.front());
                stack.erase(stack.begin());
            }
            if (stack.empty())
                save_and_exit();
        }

        debug_stack_print(ts);
        {
            const auto &i = g_jobs[stack.front().jidx];
            if (ts.tv_sec <= i.exectime) {
                auto tdelta = i.exectime - ts.tv_sec;
                ts.tv_sec = i.exectime;
                ts.tv_nsec = 0;
                if (gflags_debug)
                    log_line("do_work: SLEEP %zu seconds", tdelta);
            }
        }
    }
}

static void usage()
{
    printf("ncron " NCRON_VERSION ", cron/at daemon.\n"
           "Copyright 2003-2024 Nicholas J. Kain\n"
           "Usage: ncron [options]...\n\nOptions:\n"
           "--help         -h    Print usage and exit.\n"
           "--version      -v    Print version and exit.\n"
           "--sleep        -s [] Initial sleep time in seconds.\n"
           "--noexecsave   -0    Don't save execution history at all.\n"
           "--journal      -j    Save exectimes at each job invocation.\n"
           "--crontab      -t [] Path to crontab file.\n"
           "--history      -H [] Path to execution history file.\n"
           "--verbose      -V    Log diagnostic information.\n"
    );
}

static void print_version()
{
    log_line("ncron " NCRON_VERSION ", cron/at daemon.\n"
             "Copyright 2003-2024 Nicholas J. Kain\n\n"
"Permission is hereby granted, free of charge, to any person obtaining\n"
"a copy of this software and associated documentation files (the\n"
"\"Software\"), to deal in the Software without restriction, including\n"
"without limitation the rights to use, copy, modify, merge, publish,\n"
"distribute, sublicense, and/or sell copies of the Software, and to\n"
"permit persons to whom the Software is furnished to do so, subject to\n"
"the following conditions:\n\n"
"The above copyright notice and this permission notice shall be\n"
"included in all copies or substantial portions of the Software.\n\n"
"THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND,\n"
"EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF\n"
"MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND\n"
"NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE\n"
"LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION\n"
"OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION\n"
"WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.\n"
             );
}

static void process_options(int ac, char *av[])
{
    static struct option long_options[] = {
        {"help", 0, nullptr, 'h'},
        {"version", 0, nullptr, 'v'},
        {"sleep", 1, nullptr, 's'},
        {"noexecsave", 0, nullptr, '0'},
        {"journal", 0, nullptr, 'j'},
        {"crontab", 1, nullptr, 't'},
        {"history", 1, nullptr, 'H'},
        {"s6-notify", 1, nullptr, 'd'},
        {"verbose", 0, nullptr, 'V'},
        {nullptr, 0, nullptr, 0 }
    };
    for (;;) {
        auto c = getopt_long(ac, av, "hvbs:0jt:H:d:V", long_options, nullptr);
        if (c == -1) break;
        switch (c) {
            case 'h': usage(); exit(EXIT_SUCCESS); break;
            case 'v': print_version(); exit(EXIT_SUCCESS); break;
            case 's': if (auto t = nk::from_string<unsigned>(optarg)) {
                          g_initial_sleep = *t;
                      } else {
                          log_line("invalid sleep '%s' specified", optarg);
                          exit(EXIT_FAILURE);
                      }
                      break;
            case '0': g_ncron_execmode = Execmode::nosave; break;
            case 'j': g_ncron_execmode = Execmode::journal; break;
            case 't': g_ncron_conf = strdup(optarg); break;
            case 'H': {
                auto l = strlen(optarg);
                g_ncron_execfile = strdup(optarg);
                auto tmpf = static_cast<char *>(malloc(l + 2));
                memcpy(tmpf, optarg, l);
                tmpf[l] = '~';
                tmpf[l+1] = 0;
                g_ncron_execfile_tmp = tmpf;
                break;
            }
            case 'd': s6_notify_fd = atoi(optarg); break;
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
    parse_config(g_ncron_conf, g_ncron_execfile, &stack, &deadstack);

    if (stack.empty()) {
        log_line("%s: no jobs, exiting", __func__);
        exit(EXIT_FAILURE);
    }

    umask(077);
    fix_signals();

#ifdef __linux__
    prctl(PR_SET_DUMPABLE, 0, 0, 0, 0);
    prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);
    prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0);
#endif

    if (s6_notify_fd >= 0) {
        char buf[] = "\n";
        safe_write(s6_notify_fd, buf, 1);
        close(s6_notify_fd);
    }

    do_work(g_initial_sleep);
    exit(EXIT_SUCCESS);
}

