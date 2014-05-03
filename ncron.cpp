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
#include <fstream>
#include <boost/program_options.hpp>

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
#include "nk/copy_cmdarg.h"
}

#include "ncron.hpp"
#include "sched.hpp"
#include "crontab.hpp"
#include "rlimit.hpp"

namespace po = boost::program_options;

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
static int g_initial_sleep = 0;

static std::string g_ncron_conf(CONFIG_FILE_DEFAULT);
static std::string g_ncron_execfile(EXEC_FILE_DEFAULT);
static std::string pidfile(PID_FILE_DEFAULT);
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

static void fail_on_fdne(const std::string &file, const char *mode)
{
    if (file_exists(file.c_str(), mode))
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
                suicide("%s: nanosleep failed: %s", __func__, strerror(errno));
        }
    }
}

void clock_or_die(struct timespec *ts)
{
    if (clock_gettime(CLOCK_REALTIME, ts))
        suicide("%s: clock_gettime failed: %s", __func__, strerror(errno));
}

static inline void debug_stack_print(const struct timespec &ts) {
    if (!gflags_debug)
        return;
    log_debug("do_work: ts.tv_sec = %lu  stack.front().exectime = %lu",
              ts.tv_sec, stack.front().exectime);
    for (const auto &i: stack)
        log_debug("do_work: job %u exectime = %lu", i.ce->id, i.ce->exectime);
}

static void do_work(unsigned int initial_sleep)
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
            log_debug("do_work: DISPATCH");

            i.exec_and_fork(ts);
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
            log_debug("do_work: SLEEP %lu seconds", sts.tv_sec);
            sleep_or_die(&sts, pending_save);
        }
    }
}

static void print_version(void)
{
    printf("ncron %s, cron/at daemon.\n", NCRON_VERSION);
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

static po::variables_map fetch_options(int ac, char *av[])
{
    std::string config_file;

    po::options_description cli_opts("Command-line-exclusive options");
    cli_opts.add_options()
        ("config,c", po::value<std::string>(&config_file),
         "path to configuration file")
        ("background,b", "run as a background daemon")
        ("help,h", "print help message")
        ("version,v", "print version information")
        ;

    po::options_description gopts("Options");
    gopts.add_options()
        ("sleep,s", po::value<int>(), "initial sleep time")
        ("noexecsave,0", "don't save execution history at all")
        ("journal,j", "save exectimes at each job invocation")
        ("crontab,t", po::value<std::string>(), "path to crontab file")
        ("history,H", po::value<std::string>(),
         "path to execution history file")
        ("pidfile,f", po::value<std::string>(), "path to process id file")
        ("quiet,q", "don't log to syslog")
        ("verbose", "log diagnostic information")
        ;

    po::options_description cmdline_options;
    cmdline_options.add(cli_opts).add(gopts);
    po::options_description cfgfile_options;
    cfgfile_options.add(gopts);

    po::positional_options_description p;
    po::variables_map vm;
    try {
        po::store(po::command_line_parser(ac, av).
                  options(cmdline_options).positional(p).run(), vm);
    } catch (const std::exception& e) {
        std::cerr << e.what() << std::endl;
    }
    po::notify(vm);

    if (config_file.size()) {
        std::ifstream ifs(config_file.c_str());
        if (!ifs) {
            std::cerr << "Could not open config file: " << config_file << "\n";
            std::exit(EXIT_FAILURE);
        }
        po::store(po::parse_config_file(ifs, cfgfile_options), vm);
        po::notify(vm);
    }

    if (vm.count("help")) {
        std::cout << "ncron " << NCRON_VERSION << ", cron/at daemon.\n"
                  << "Copyright (c) 2003-2014 Nicholas J. Kain\n"
                  << av[0] << " [options]...\n"
                  << cmdline_options << std::endl;
        std::exit(EXIT_FAILURE);
    }
    if (vm.count("version")) {
        print_version();
        std::exit(EXIT_FAILURE);
    }
    return vm;
}

static void process_options(int ac, char *av[]) {
    auto vm(fetch_options(ac, av));

    if (vm.count("background"))
        gflags_detach = 1;
    if (vm.count("sleep"))
        g_initial_sleep = vm["sleep"].as<int>();
    if (vm.count("noexecsave"))
        g_ncron_execmode = 2;
    if (vm.count("journal"))
        g_ncron_execmode = 1;
    if (vm.count("quiet"))
        gflags_quiet = 1;
    if (vm.count("verbose"))
        gflags_debug = 1;
    if (vm.count("crontab"))
        g_ncron_conf = vm["crontab"].as<std::string>();
    if (vm.count("history"))
        g_ncron_execfile = vm["history"].as<std::string>();
    if (vm.count("pidfile"))
        pidfile = vm["pidfile"].as<std::string>();

}

int main(int argc, char* argv[])
{
    process_options(argc, argv);
    fail_on_fdne(g_ncron_conf, "r");
    fail_on_fdne(g_ncron_execfile, "rw");
    parse_config(g_ncron_conf, g_ncron_execfile, stack, deadstack);

    if (stack.empty())
        suicide("%s: no jobs, exiting", __func__);

    if (gflags_detach) {
        if (daemon(0,0))
            suicide("%s: daemon failed: %s", __func__, strerror(errno));
    }

    if (pidfile.size() && file_exists(pidfile.c_str(), "w"))
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

