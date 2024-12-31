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

#include "nk/log.h"
#include "nk/io.h"
#include "strconv.h"
#include "ncron.h"
#include "sched.h"

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
enum Execmode
{
    Execmode_normal = 0,
    Execmode_journal,
    Execmode_nosave,
};
static enum Execmode g_ncron_execmode = Execmode_normal;

size_t g_njobs;
struct Job *g_jobs;
static struct Job * stackl;
static struct Job * deadstackl;

static bool do_save_stack(FILE *f, struct Job *j)
{
    for (; j; j = j->next_) {
        if (fprintf(f, "%d=%li:%u|%lu\n", j->id_, j->exectime_, j->numruns_, j->lasttime_) < 0) {
            log_line("Failed to write to history file %s\n", g_ncron_execfile_tmp);
            return false;
        }
    }
    return true;
}

static bool save_stack(void)
{
    FILE *f = fopen(g_ncron_execfile_tmp, "w");
    if (!f) {
        log_line("Failed to open history file %s for write\n", g_ncron_execfile_tmp);
        return false;
    }
    if (!do_save_stack(f, stackl)) goto err1;
    if (!do_save_stack(f, deadstackl)) goto err1;
    fclose(f);

    if (rename(g_ncron_execfile_tmp, g_ncron_execfile)) {
        log_line("Failed to update history file (%s => %s): %s\n",
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
    if (g_ncron_execmode != Execmode_nosave) {
        if (save_stack()) {
            log_line("Saved stack to %s.\n", g_ncron_execfile);
        } else {
            log_line("Failed to save stack to %s; some jobs may run again.\n",
                     g_ncron_execfile);
        }
    }
    // Get rid of leak sanitizer noise.
    for (size_t i = 0; i < g_njobs; ++i) job_destroy(&g_jobs[i]);
    log_line("Exited.\n");
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
        SIGHUP, SIGINT, SIGTERM, SIGPIPE, SIGKILL
    };
    sigset_t mask;
    if (sigprocmask(0, 0, &mask) < 0)
        suicide("sigprocmask failed\n");
    for (int i = 0; ss[i] != SIGKILL; ++i)
        if (sigdelset(&mask, ss[i]))
            suicide("sigdelset failed\n");
    if (sigprocmask(SIG_SETMASK, &mask, NULL) < 0)
        suicide("sigprocmask failed\n");

    struct sigaction sa = { .sa_handler = signal_handler, .sa_flags = SA_RESTART };
    if (sigemptyset(&sa.sa_mask))
        suicide("sigemptyset failed\n");
    for (int i = 0; ss[i] != SIGKILL; ++i)
        if (sigaction(ss[i], &sa, NULL))
            suicide("sigaction failed\n");
    sa.sa_handler = SIG_IGN;
    sa.sa_flags = SA_NOCLDWAIT;
    if (sigaction(SIGCHLD, &sa, NULL))
        suicide("sigaction failed\n");
}

static void fail_on_fdne(char const *file, int mode)
{
    if (access(file, mode))
        suicide("File '%s' does not exist or is not %s\n",
                file, (mode & W_OK) ? "writable" : "readable");
}

static void sleep_or_die(struct timespec *ts)
{
    for (;;) {
        int r = clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, ts, NULL);
        if (r) {
            if (r == EINTR) {
                if (pending_save_and_exit) save_and_exit();
                continue;
            }
            suicide("clock_nanosleep failed: %s\n", strerror(r));
        }
        break;
    }
}

void clock_or_die(struct timespec *ts)
{
    if (clock_gettime(CLOCK_REALTIME, ts))
        suicide("clock_gettime failed: %s\n", strerror(errno));
}

static inline void debug_stack_print(const struct timespec *ts) {
    if (!gflags_debug)
        return;
    if (stackl)
        log_line("ts.tv_sec = %lu  stack.front().exectime = %lu\n", ts->tv_sec, stackl->exectime_);
    for (struct Job *j = stackl; j; j = j->next_)
        log_line("job %d exectime = %lu\n", j->id_, j->exectime_);
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
                log_line("Failed to save stack to %s for a journalled job.\n",
                         g_ncron_execfile);
            } else {
                pending_save = false;
            }
        }
        sleep_or_die(&ts);

        while (stackl->exectime_ <= ts.tv_sec) {
            struct Job *j = stackl;
            if (gflags_debug)
                log_line("DISPATCH %d (%lu <= %lu)\n", j->id_, j->exectime_, ts.tv_sec);

            job_exec(j, &ts);
            if (j->journal_ || g_ncron_execmode == Execmode_journal)
                pending_save = true;

            if ((j->numruns_ < j->maxruns_ || j->maxruns_ == 0) && j->exectime_ != 0) {
                if (stackl->next_) {
                    struct Job *t = stackl;
                    stackl = stackl->next_;
                    job_insert(&stackl, t);
                }
            } else {
                struct Job *t = stackl;
                stackl = stackl->next_;
                job_insert(&deadstackl, t);
            }
            if (!stackl)
                save_and_exit();
        }

        debug_stack_print(&ts);
        {
            struct Job *j = stackl;
            if (ts.tv_sec <= j->exectime_) {
                time_t tdelta = j->exectime_ - ts.tv_sec;
                ts.tv_sec = j->exectime_;
                ts.tv_nsec = 0;
                if (gflags_debug)
                    log_line("SLEEP %zu seconds\n", tdelta);
            }
        }
    }
}

static void usage(void)
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

static void print_version(void)
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
        {"help", 0, NULL, 'h'},
        {"version", 0, NULL, 'v'},
        {"sleep", 1, NULL, 's'},
        {"noexecsave", 0, NULL, '0'},
        {"journal", 0, NULL, 'j'},
        {"crontab", 1, NULL, 't'},
        {"history", 1, NULL, 'H'},
        {"s6-notify", 1, NULL, 'd'},
        {"verbose", 0, NULL, 'V'},
        {NULL, 0, NULL, 0 }
    };
    for (;;) {
        int c = getopt_long(ac, av, "hvbs:0jt:H:d:V", long_options, NULL);
        if (c == -1) break;
        switch (c) {
            case 'h': usage(); exit(EXIT_SUCCESS); break;
            case 'v': print_version(); exit(EXIT_SUCCESS); break;
            case 's': if (!strconv_to_u32(optarg, optarg + strlen(optarg), &g_initial_sleep))
                          suicide("invalid sleep '%s' specified\n", optarg);
                      break;
            case '0': g_ncron_execmode = Execmode_nosave; break;
            case 'j': g_ncron_execmode = Execmode_journal; break;
            case 't': g_ncron_conf = strdup(optarg); if (!g_ncron_conf) abort(); break;
            case 'H': {
                size_t l = strlen(optarg);
                g_ncron_execfile = strdup(optarg);
                if (!g_ncron_execfile) abort();
                char *tmpf = malloc(l + 2);
                if (!tmpf) abort();
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

    if (!stackl)
        suicide("No jobs, exiting.\n");

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

