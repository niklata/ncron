// Copyright 2003-2024 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#include <unistd.h>
#include <stdio.h>
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

extern "C" {
#include "nk/log.h"
#include "nk/io.h"
#include "xmalloc.h"
#include "strconv.h"
}
#include "ncron.hpp"
#include "sched.hpp"

#define CONFIG_FILE_DEFAULT "/var/lib/ncron/crontab"
#define EXEC_FILE_DEFAULT "/var/lib/ncron/exectimes"

#define NCRON_VERSION "3.0"

int gflags_debug;
static volatile sig_atomic_t pending_save_and_exit;
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

size_t g_njobs;
Job *g_jobs;
static Job * stackl;
static Job * deadstackl;

static bool do_save_stack(FILE *f, Job *j)
{
    for (; j; j = j->next_) {
        if (fprintf(f, "%d=%li:%u|%lu\n", j->id_, j->exectime_, j->numruns_, j->lasttime_) < 0) {
            log_line("Failed to write to history file %s", g_ncron_execfile_tmp);
            return false;
        }
    }
    return true;
}

[[nodiscard]] static bool save_stack()
{
    FILE *f = fopen(g_ncron_execfile_tmp, "w");
    if (!f) {
        log_line("Failed to open history file %s for write", g_ncron_execfile_tmp);
        return false;
    }
    if (!do_save_stack(f, stackl)) goto err1;
    if (!do_save_stack(f, deadstackl)) goto err1;
    fclose(f);

    if (rename(g_ncron_execfile_tmp, g_ncron_execfile)) {
        log_line("Failed to update history file (%s => %s): %s",
                 g_ncron_execfile_tmp, g_ncron_execfile, strerror(errno));
        goto err0;
    }
    return true;
err1:
    fclose(f);
err0:
    unlink(g_ncron_execfile_tmp);
    return false;
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
    // Get rid of leak sanitizer noise.
    for (size_t i = 0; i < g_njobs; ++i) g_jobs[i].~Job();
    log_line("Exited.");
    exit(EXIT_SUCCESS);
}

static void signal_handler(int sig)
{
    int serrno = errno;
    if (sig == SIGTERM || sig == SIGINT || sig == SIGHUP) {
        pending_save_and_exit = 1;
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
    if (access(file, mode)) {
        log_line("File '%s' does not exist or is not %s",
                 file, (mode & W_OK) ? "writable" : "readable");
        exit(EXIT_FAILURE);
    }
}

static void sleep_or_die(struct timespec *ts)
{
retry:
    int r = clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, ts, NULL);
    if (r) {
        if (r == EINTR) {
            if (pending_save_and_exit) save_and_exit();
            goto retry;
        }
        log_line("clock_nanosleep failed: %s", strerror(r));
        exit(EXIT_FAILURE);
    }
}

void clock_or_die(struct timespec *ts)
{
    if (clock_gettime(CLOCK_REALTIME, ts)) {
        log_line("clock_gettime failed: %s", strerror(errno));
        exit(EXIT_FAILURE);
    }
}

static inline void debug_stack_print(const struct timespec &ts) {
    if (!gflags_debug)
        return;
    if (stackl)
        log_line("ts.tv_sec = %lu  stack.front().exectime = %lu", ts.tv_sec, stackl->exectime_);
    for (Job *j = stackl; j; j = j->next_)
        log_line("job %d exectime = %lu", j->id_, j->exectime_);
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

        while (stackl->exectime_ <= ts.tv_sec) {
            Job *j = stackl;
            if (gflags_debug)
                log_line("DISPATCH %d (%lu <= %lu)", j->id_, j->exectime_, ts.tv_sec);

            j->exec(ts);
            if (j->journal_ || g_ncron_execmode == Execmode::journal)
                pending_save = true;

            if ((j->numruns_ < j->maxruns_ || j->maxruns_ == 0) && j->exectime_ != 0) {
                if (stackl->next_) {
                    Job *t = stackl;
                    stackl = stackl->next_;
                    job_insert(&stackl, t);
                }
            } else {
                Job *t = stackl;
                stackl = stackl->next_;
                job_insert(&deadstackl, t);
            }
            if (!stackl)
                save_and_exit();
        }

        debug_stack_print(ts);
        {
            Job *j = stackl;
            if (ts.tv_sec <= j->exectime_) {
                time_t tdelta = j->exectime_ - ts.tv_sec;
                ts.tv_sec = j->exectime_;
                ts.tv_nsec = 0;
                if (gflags_debug)
                    log_line("SLEEP %zu seconds", tdelta);
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

static char *xstrdup(const char *s)
{
    char *r = strdup(s);
    if (!r) exit(EXIT_FAILURE);
    return r;
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
        int c = getopt_long(ac, av, "hvbs:0jt:H:d:V", long_options, nullptr);
        if (c == -1) break;
        switch (c) {
            case 'h': usage(); exit(EXIT_SUCCESS); break;
            case 'v': print_version(); exit(EXIT_SUCCESS); break;
            case 's': if (!strconv_to_u32(optarg, optarg + strlen(optarg), &g_initial_sleep)) {
                          log_line("invalid sleep '%s' specified", optarg);
                          exit(EXIT_FAILURE);
                      }
                      break;
            case '0': g_ncron_execmode = Execmode::nosave; break;
            case 'j': g_ncron_execmode = Execmode::journal; break;
            case 't': g_ncron_conf = xstrdup(optarg); break;
            case 'H': {
                size_t l = strlen(optarg);
                g_ncron_execfile = xstrdup(optarg);
                char *tmpf = static_cast<char *>(xmalloc(l + 2));
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
    parse_config(g_ncron_conf, g_ncron_execfile, &stackl, &deadstackl);

    if (!stackl) {
        log_line("No jobs, exiting.");
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

