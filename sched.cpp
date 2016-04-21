/* sched.c - ncron job scheduling
 *
 * (c) 2003-2016 Nicholas J. Kain <njkain at gmail dot com>
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
#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
#include <format.hpp>
#include <nk/scopeguard.hpp>
extern "C" {
#include "nk/exec.h"
#include "nk/privilege.h"
}
#include "ncron.hpp"
#include "sched.hpp"

#define COUNT_THRESH 500 /* Arbitrary and untested */
#define MAX_CENV 50
#define MAX_ENVBUF 2048

/*
 * returns -1 if below range
 * returns 0 if within range
 * returns 1 if above range
 */
static inline int compare_range(const std::pair<int,int> &r,
                                int v, int wildcard)
{
    if (r.first == wildcard && r.second == wildcard)
        return 0;
    if (r.first == wildcard) {
        if (v < r.second)
            return -1;
        if (v > r.second)
            return 1;
        return 0;
    }
    if (r.second == wildcard) {
        if (v < r.first)
            return -1;
        if (v > r.first)
            return 1;
        return 0;
    }
    if (v < r.first)
        return -1;
    if (v > r.second)
        return 1;
    return 0;
}

/*
 * 1 if range is valid, 0 if not; assumes for constraint and range: (x,y) | x<y
 */
static inline int is_range_valid(const std::pair<int,int> &constraint,
                                 const std::pair<int,int> &range,
                                 int wildcard)
{
    if (range.first < constraint.first && range.first != wildcard)
        return 0;
    if (range.second > constraint.second && range.second != wildcard)
        return 0;
    return 1;
}

/*
 * This fn does the actual constraint.  If our tested value falls within
 * constraint ranges, then nothing is done.  Otherwise, the 'nearest'
 * constraint range that is above the current value is chosen.  If no such
 * range exists, then the next higher unit is incremented by one and the
 * smallest extant valid value is chosen for the constrained unit.
 */
static int compare_list_range_v(const cronentry_t::cst_list &list, int *unit,
                                int *nextunit, const std::pair<int,int> &valid,
                                int wildcard)
{
    int t, smallunit = INT_MAX, dist = INT_MAX, tinyunit = INT_MAX;

    if (list.empty() || !unit || !nextunit)
        return 0;

    /* find the range least distant from our target value */
    for (const auto &i: list) {
        if (i.first < tinyunit)
            tinyunit = i.first;
        if (is_range_valid(valid, i, wildcard)) {
            switch (compare_range(i, *unit, wildcard)) {
                case 1: /* range below our value, avoid if possible */
                    t = *unit - i.second; /* t > 0 */
                    if (t < abs(dist) && dist > 0)
                        dist = t;
                    smallunit = i.first;
                    break;
                default: /* bingo, our value is within a range */
                    return 0;
                case -1: /* range above our value, favor */
                    t = *unit - i.first; /* implicitly, t < 0 */
                    if (abs(t) < abs(dist) || dist > 0)
                        dist = t;
                    smallunit = i.first;
                    break;
            }
        }
    }

    /* All of our constraints are invalid, so act as if all are wildcard. */
    if (tinyunit == INT_MAX)
        return 0;

    if (dist > 0) {
        (*nextunit)++;
        *unit = tinyunit;
    }
    if (dist < 0)
        *unit = smallunit;
    if (dist == 0)
        return 0;
    return 1;
}

/* same as above, but no check on range validity */
static int compare_list_range(const cronentry_t::cst_list &list, int *unit,
                              int *nextunit, int wildcard)
{
    int t, smallunit = INT_MAX, dist = INT_MAX, tinyunit = INT_MAX;

    if (list.empty() || !unit || !nextunit)
        return 0;

    /* find the range least distant from our target value */
    for (const auto &i: list) {
        if (i.first < tinyunit) tinyunit = i.first;
        switch (compare_range(i, *unit, wildcard)) {
            case 1: /* range below our value, avoid if possible */
                t = *unit - i.second; /* t > 0 */
                if (dist > 0 && t < dist)
                    dist = t;
                smallunit = i.first;
                break;
            default: /* bingo, our value is within a range */
                return 0;
            case -1: /* range above our value, favor */
                t = *unit - i.first; /* implicitly, t < 0 */
                if (dist > 0 || t > dist)
                    dist = t;
                smallunit = i.first;
                break;
        }
    }

    if (dist > 0) {
        (*nextunit)++;
        *unit = tinyunit;
    }
    if (dist < 0)
        *unit = smallunit;
    if (dist == 0)
        return 0;
    return 1;
}

static int compare_wday_range(const cronentry_t::cst_list &list, int *unit,
                              int *nextunit)
{
    int dist = INT_MAX, t;
    if (list.empty() || !unit || !nextunit)
        return 0;

    /* find the range least distant from our target value */ 
    for (const auto &i: list) {
        switch (compare_range(i, *unit, 0)) {
            case 1:
                t = *unit - i.second;
                if (dist > 0 && t < dist)
                    dist = t;
                break;
            case 0:
                return 0;
            case -1:
                t = *unit - i.first;
                if (dist > 0 || t > dist)
                    dist = t;
                break;
        }
    }

    if (dist < 0) {
        nextunit -= dist;
        return 1;
    }

    (*nextunit)++;
    return 1;
}

static std::pair<int,int> valid_day_of_month(int month, int year)
{
    auto ret = std::make_pair<int,int>(1, 31);
    switch (month) {
    case 2: /* we follow the gregorian calendar */
        if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
            ret.second = 29;
        else
            ret.second = 28;
        break;
    case 4: case 6: case 9: case 11: ret.second = 30; default: break;
    }
    return ret;
}

/* entry is obvious, stime is the time we're constraining
 * returns a time value that has been appropriately constrained */
static time_t constrain_time(const cronentry_t &entry, time_t stime)
{
    struct tm *rtime;
    time_t t;
    int count = 0;

    rtime = localtime(&stime);

    for (; count < COUNT_THRESH; count++) {
        t = mktime(rtime);
        rtime = localtime(&t);

        if (compare_list_range(entry.minute, &(rtime->tm_min),
                    &(rtime->tm_hour), 60))
            continue;
        if (compare_list_range(entry.hour, &(rtime->tm_hour),
                    &(rtime->tm_mday), 24))
            continue;
        if (compare_list_range_v(entry.day, &(rtime->tm_mday),
                    &(rtime->tm_mon), valid_day_of_month(rtime->tm_mon,
                        rtime->tm_year), 0))
            continue;
        if (compare_wday_range(entry.weekday, &(rtime->tm_wday),
                    &(rtime->tm_mday)))
            continue;
        if (compare_list_range(entry.month, &(rtime->tm_mon),
                    &(rtime->tm_year), 0))
            continue;
        return mktime(rtime);
    }
    /* Failed to find a suitable time. */
    return 0;
}

/* Used when jobs without exectimes are first loaded. */
void set_initial_exectime(cronentry_t &entry)
{
    struct timespec ts;
    clock_or_die(&ts);
    time_t ttm = constrain_time(entry, ts.tv_sec);
    time_t ttd = ttm - entry.lasttime;
    if (ttd < entry.interval) {
        ttm += entry.interval - ttd;
        ttm = constrain_time(entry, ttm);
    }
    entry.exectime = ttm;
}

/* stupidly advances to next time of execution; performs constraint.  */
void cronentry_t::set_next_time()
{
    struct timespec ts;
    clock_or_die(&ts);
    auto etime = constrain_time(*this, ts.tv_sec + interval);
    exectime = etime > ts.tv_sec ? etime : 0;
}

void save_stack(const std::string &file,
                const std::vector<StackItem> &stack,
                const std::vector<StackItem> &deadstack)
{
    char buf[MAXLINE];

    auto f = fopen(file.c_str(), "w");
    if (!f) {
        fmt::print(stderr, "{}: failed to open history file {} for write\n", __func__, file);
        std::exit(EXIT_FAILURE);
    }
    SCOPE_EXIT{ fclose(f); };
    for (auto &i: stack) {
        auto snlen = snprintf(buf, sizeof buf, "%u=%li:%u|%lu\n", i.ce->id,
                              i.ce->exectime, i.ce->numruns, i.ce->lasttime);
        if (snlen < 0 || static_cast<std::size_t>(snlen) >= sizeof buf) {
            fmt::print(stderr, "{}: Would truncate history entry for job {}; skipping.\n",
                       __func__, i.ce->id);
            continue;
        }
        auto bsize = strlen(buf);
        while (!fwrite(buf, bsize, 1, f)) {
            if (ferror(f))
                return;
        }
    }
    for (auto &i: deadstack) {
        auto snlen = snprintf(buf, sizeof buf, "%u=%li:%u|%lu\n", i.ce->id,
                              i.ce->exectime, i.ce->numruns, i.ce->lasttime);
        if (snlen < 0 || static_cast<std::size_t>(snlen) >= sizeof buf) {
            fmt::print(stderr, "{}: Would truncate history entry for job {}; skipping.\n",
                       __func__, i.ce->id);
            continue;
        }
        auto bsize = strlen(buf);
        while (!fwrite(buf, bsize, 1, f)) {
            if (ferror(f))
                return;
        }
    }
}

#ifdef __GNUC__
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-result"
#endif
void cronentry_t::exec(const struct timespec &ts)
{
    char *env[MAX_CENV];
    char envbuf[MAX_ENVBUF];
    switch ((int)fork()) {
        case 0:
            if (nk_generate_env(user, chroot.empty() ? nullptr : chroot.c_str(),
                                path.empty() ? nullptr : path.c_str(),
                                env, MAX_CENV, envbuf, sizeof envbuf) < 0) {
                const char errstr[] = "exec: failed to generate environment\n";
                write(STDERR_FILENO, errstr, sizeof errstr);
                std::exit(EXIT_FAILURE);
            }
            if (limits.exist() && limits.enforce(user, group, command)) {
                const char errstr[] = "exec: rlimits::enforce failed\n";
                write(STDERR_FILENO, errstr, sizeof errstr);
                std::exit(EXIT_FAILURE);
            }
            if (group) {
                if (setresgid(group, group, group)) {
                    const char errstr[] = "exec: setresgid failed\n";
                    write(STDERR_FILENO, errstr, sizeof errstr);
                    std::exit(EXIT_FAILURE);
                }
                if (getgid() == 0) {
                    const char errstr[] = "exec: child is still gid=root after setgid()\n";
                    write(STDERR_FILENO, errstr, sizeof errstr);
                    std::exit(EXIT_FAILURE);
                }
            }
            if (user) {
                if (setresuid(user, user, user)) {
                    const char errstr[] = "exec: setresuid failed\n";
                    write(STDERR_FILENO, errstr, sizeof errstr);
                    std::exit(EXIT_FAILURE);
                }
                if (getuid() == 0) {
                    const char errstr[] = "exec: child is still uid=root after setuid()\n";
                    write(STDERR_FILENO, errstr, sizeof errstr);
                    std::exit(EXIT_FAILURE);
                }
            }
            nk_execute(command.c_str(), args.c_str(), env);
        case -1: {
            const char errstr[] = "exec: fork failed\n";
            write(STDERR_FILENO, errstr, sizeof errstr);
            std::exit(EXIT_FAILURE);
        }
        default:
            ++numruns;
            lasttime = ts.tv_sec;
            set_next_time();
            break;
    }
}
#ifdef __GNUC__
#pragma GCC diagnostic pop
#endif

