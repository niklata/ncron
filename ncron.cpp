/* ncron.cpp - secure, minimally-sleeping cron daemon
 *
 * Copyright 2003-2016 Nicholas J. Kain <njkain at gmail dot com>
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
#include <fmt/format.h>

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

#include <nk/from_string.hpp>
#include <nk/optionarg.hpp>
extern "C" {
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

static volatile sig_atomic_t pending_save_and_exit;
static volatile sig_atomic_t pending_reload_config;
static volatile sig_atomic_t pending_free_children;
extern int gflags_debug;
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
    fmt::print("SIGHUP - Reloading config: {}.\n", g_ncron_conf);
    std::fflush(stdout);
    pending_reload_config = 0;
}

static void save_and_exit(void)
{
    if (g_ncron_execmode != 2) {
        save_stack(g_ncron_execfile, stack, deadstack);
        fmt::print("Saving stack to {}.\n", g_ncron_execfile);
    }
    fmt::print("Exited.\n");
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
                fmt::print(stderr, "{}: nanosleep failed: {}\n",
                           __func__, strerror(errno));
                std::exit(EXIT_FAILURE);
        }
    }
}

void clock_or_die(struct timespec *ts)
{
    if (clock_gettime(CLOCK_REALTIME, ts)) {
        fmt::print(stderr, "{}: clock_gettime failed: {}\n",
                   __func__, strerror(errno));
        std::exit(EXIT_FAILURE);
    }
}

static inline void debug_stack_print(const struct timespec &ts) {
    if (!gflags_debug)
        return;
    fmt::print(stderr, "do_work: ts.tv_sec = {}  stack.front().exectime = {}",
               ts.tv_sec, stack.front().exectime);
    for (const auto &i: stack)
        fmt::print(stderr, "do_work: job {} exectime = {}",
                   i.ce->id, i.ce->exectime);
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
                fmt::print(stderr, "do_work: DISPATCH\n");

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
                fmt::print(stderr, "do_work: SLEEP {} seconds\n", sts.tv_sec);
            sleep_or_die(&sts, pending_save);
        }
    }
}

static void print_version(void)
{
    fmt::print("ncron " NCRON_VERSION ", cron/at daemon.\n"
               "Copyright 2003-2016 Nicholas J. Kain\n"
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
               "POSSIBILITY OF SUCH DAMAGE.\n");
}

enum OpIdx {
    OPT_UNKNOWN, OPT_HELP, OPT_VERSION, OPT_BACKGROUND, OPT_SLEEP,
    OPT_NOEXECSAVE, OPT_JOURNAL, OPT_CRONTAB, OPT_HISTORY, OPT_PIDFILE,
    OPT_VERBOSE
};
static const option::Descriptor usage[] = {
    { OPT_UNKNOWN,    0,  "",           "", Arg::Unknown,
        "ncron " NCRON_VERSION ", cron/at daemon.\n"
        "Copyright 2003-2016 Nicholas J. Kain\n"
        "Usage: ncron [options]...\n\nOptions:" },
    { OPT_HELP,       0, "h",       "help",    Arg::None, "\t-h, \t--help  \tPrint usage and exit." },
    { OPT_VERSION,    0, "v",    "version",    Arg::None, "\t-v, \t--version  \tPrint version and exit." },
    { OPT_BACKGROUND, 0, "b", "background",    Arg::None, "\t-b, \t--background  \tRun as a background daemon." },
    { OPT_SLEEP,      0, "s",      "sleep", Arg::Integer, "\t-s, \t--sleep  \tInitial sleep time in seconds." },
    { OPT_NOEXECSAVE, 0, "0", "noexecsave",    Arg::None, "\t-0, \t--noexecsave  \tDon't save execution history at all." },
    { OPT_JOURNAL,    0, "j",    "journal",    Arg::None, "\t-j, \t--journal  \tSave exectimes at each job invocation." },
    { OPT_CRONTAB,    0, "t",    "crontab",  Arg::String, "\t-t, \t--crontab  \tPath to crontab file." },
    { OPT_HISTORY,    0, "H",    "history",  Arg::String, "\t-H, \t--history  \tPath to execution history file." },
    { OPT_PIDFILE,    0, "f",    "pidfile",  Arg::String, "\t-f, \t--pidfile  \tPath to process id file." },
    { OPT_VERBOSE,    0,  "",    "verbose",    Arg::None, "\t    \t--verbose  \tLog diagnostic information." },
    {0,0,0,0,0,0}
};

static void process_options(int ac, char *av[]) {
    ac-=ac>0; av+=ac>0;
    option::Stats stats(usage, ac, av);
#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wvla"
    option::Option options[stats.options_max], buffer[stats.buffer_max];
#pragma GCC diagnostic pop
    option::Parser parse(usage, ac, av, options, buffer);
#else
    auto options = std::make_unique<option::Option[]>(stats.options_max);
    auto buffer = std::make_unique<option::Option[]>(stats.buffer_max);
    option::Parser parse(usage, ac, av, options.get(), buffer.get());
#endif
    if (parse.error())
        std::exit(EXIT_FAILURE);
    if (options[OPT_HELP]) {
        uint16_t col{80};
        const auto cols = getenv("COLUMNS");
        if (cols) col = nk::from_string<uint16_t>(cols);
        option::printUsage(fwrite, stdout, usage, col);
        std::exit(EXIT_FAILURE);
    }
    if (options[OPT_VERSION]) {
        print_version();
        std::exit(EXIT_FAILURE);
    }
    for (int i = 0; i < parse.optionsCount(); ++i) {
        option::Option &opt = buffer[i];
        switch (opt.index()) {
            case OPT_BACKGROUND: gflags_background = true; break;
            case OPT_SLEEP:
                try {
                    g_initial_sleep = nk::from_string<unsigned>(opt.arg);
                } catch (...) {
                    fmt::print(stderr, "invalid sleep '{}' specified\n", opt.arg);
                    std::exit(EXIT_FAILURE);
                }
                break;
            case OPT_NOEXECSAVE: g_ncron_execmode = 2; break;
            case OPT_JOURNAL: g_ncron_execmode = 1; break;
            case OPT_CRONTAB: g_ncron_conf = std::string(opt.arg); break;
            case OPT_HISTORY: g_ncron_execfile = std::string(opt.arg); break;
            case OPT_PIDFILE: pidfile = std::string(opt.arg); break;
            case OPT_VERBOSE: gflags_debug = 1; break;
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
        fmt::print(stderr, "{}: no jobs, exiting\n", __func__);
        std::exit(EXIT_FAILURE);
    }

    if (gflags_background) {
        if (daemon(0,0)) {
            fmt::print(stderr, "{}: daemon failed: {}\n", __func__, strerror(errno));
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

