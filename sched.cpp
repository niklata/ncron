/* sched.c - ncron job scheduling
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

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>
#include <limits.h>
#include <assert.h>
extern "C" {
#include "nk/log.h"
}
#include "ncron.hpp"
#include "sched.hpp"

#define COUNT_THRESH 500 /* Arbitrary and untested */

/*
 * returns -1 if below range
 * returns 0 if within range
 * returns 1 if above range
 */
static inline int compare_range(ipair_t range, int v, int wildcard)
{
    if (range.l == wildcard)
        range.l = range.h;
    if (range.h == wildcard || range.l == range.h) {
        if (range.l == v)
            return 0;
        if (v < range.l)
            return -1;
        if (v > range.l)
            return 1;
    }

    if (v < range.l)
        return -1;
    if (v > range.h)
        return 1;
    return 0;
}

/*
 * 1 if range is valid, 0 if not; assumes for constraint and range: (x,y) | x<y
 */
static inline int is_range_valid(ipair_t constraint, ipair_t range,
        int wildcard)
{
    if (range.l < constraint.l && range.l != wildcard)
        return 0;
    if (range.h > constraint.h && range.h != wildcard)
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
static int compare_list_range_v(ipair_node_t *list, int *unit, int *nextunit,
        ipair_t valid, int wildcard)
{
    int t, smallunit = INT_MAX, dist = INT_MAX, tinyunit = INT_MAX;

    if (!list || !unit || !nextunit)
        return 0;

    /* find the range least distant from our target value */
    while (list) {
        if (list->node.l < tinyunit)
            tinyunit = list->node.l;
        if (is_range_valid(valid, list->node, wildcard)) {
            switch (compare_range(list->node, *unit, wildcard)) {
                case 1: /* range below our value, avoid if possible */
                    t = *unit - list->node.h; /* t > 0 */
                    if (t < abs(dist) && dist > 0)
                        dist = t;
                    smallunit = list->node.l;
                    break;
                default: /* bingo, our value is within a range */
                    return 0;
                case -1: /* range above our value, favor */
                    t = *unit - list->node.l; /* implicitly, t < 0 */
                    if (abs(t) < abs(dist) || dist > 0)
                        dist = t;
                    smallunit = list->node.l;
                    break;
            }
        }
        list = list->next;
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
static int compare_list_range(ipair_node_t *list, int *unit, int *nextunit,
        int wildcard)
{
    int t, smallunit = INT_MAX, dist = INT_MAX, tinyunit = INT_MAX;

    if (!list || !unit || !nextunit) return 0;

    /* find the range least distant from our target value */
    while (list) {
        if (list->node.l < tinyunit) tinyunit = list->node.l;
        switch (compare_range(list->node, *unit, wildcard)) {
            case 1: /* range below our value, avoid if possible */
                t = *unit - list->node.h; /* t > 0 */
                if (dist > 0 && t < dist)
                    dist = t;
                smallunit = list->node.l;
                break;
            default: /* bingo, our value is within a range */
                return 0;
            case -1: /* range above our value, favor */
                t = *unit - list->node.l; /* implicitly, t < 0 */
                if (dist > 0 || t > dist)
                    dist = t;
                smallunit = list->node.l;
                break;
        }
        list = list->next;
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

static int compare_wday_range(ipair_node_t *list, int *unit, int *nextunit)
{
    int dist = INT_MAX, t;
    if (!list || !unit || !nextunit) return 0;

    /* find the range least distant from our target value */ 
    while (list) {
        switch (compare_range(list->node, *unit, 0)) {
            case 1:
                t = *unit - list->node.h;
                if (dist > 0 && t < dist)
                    dist = t;
                break;
            case 0:
                return 0;
            case -1:
                t = *unit - list->node.l;
                if (dist > 0 || t > dist)
                    dist = t;
                break;
        }
        list = list->next;
    }

    if (dist < 0) {
        nextunit -= dist;
        return 1;
    }

    (*nextunit)++;
    return 1;
}

static ipair_t valid_day_of_month(int month, int year)
{
    ipair_t ret = { 1, 31 };
    switch (month) {
    case 2: /* we follow the gregorian calendar */
        if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
            ret.h = 29;
        else
            ret.h = 28;
        break;
    case 4: case 6: case 9: case 11: ret.h = 30; default: break;
    }
    return ret;
}

/* entry is obvious, stime is the time we're constraining
 * returns a time value that has been appropriately constrained */
static time_t constrain_time(const cronentry_t *entry, time_t stime)
{
    struct tm *rtime;
    time_t t;
    int count = 0;

    if (!entry)
        return 0;

    rtime = localtime(&stime);

    for (; count < COUNT_THRESH; count++) {
        t = mktime(rtime);
        rtime = localtime(&t);

        if (compare_list_range(entry->minute, &(rtime->tm_min),
                    &(rtime->tm_hour), 60))
            continue;
        if (compare_list_range(entry->hour, &(rtime->tm_hour),
                    &(rtime->tm_mday), 24))
            continue;
        if (compare_list_range_v(entry->day, &(rtime->tm_mday),
                    &(rtime->tm_mon), valid_day_of_month(rtime->tm_mon,
                        rtime->tm_year), 0))
            continue;
        if (compare_wday_range(entry->weekday, &(rtime->tm_wday),
                    &(rtime->tm_mday)))
            continue;
        if (compare_list_range(entry->month, &(rtime->tm_mon),
                    &(rtime->tm_year), 0))
            continue;
        return mktime(rtime);
    }
    /* Failed to find a suitable time. */
    return 0;
}

/* Used when jobs without exectimes are first loaded. */
void set_initial_exectime(cronentry_t *entry)
{
    struct timespec ts;
    clock_or_die(&ts);
    time_t ttm = constrain_time(entry, ts.tv_sec);
    time_t ttd = ttm - entry->lasttime;
    if (ttd < entry->interval) {
        ttm += entry->interval - ttd;
        ttm = constrain_time(entry, ttm);
    }
    entry->exectime = ttm;
}

/* stupidly advances to next time of execution; performs constraint.  */
time_t get_next_time(const cronentry_t &entry)
{
    struct timespec ts;
    clock_or_die(&ts);
    time_t etime = constrain_time(&entry, ts.tv_sec + entry.interval);
    return etime > ts.tv_sec ? etime : 0;
}

void save_stack(const char *file,
                const std::vector<std::unique_ptr<cronentry_t>> &stack,
                const std::vector<std::unique_ptr<cronentry_t>> &deadstack)
{
    char buf[MAXLINE];

    FILE *f = fopen(file, "w");
    if (!f)
        suicide("%s: failed to open history file %s for write", __func__, file);

    for (auto &i: stack) {
        auto snlen = snprintf(buf, sizeof buf, "%u=%li:%u|%lu\n", i->id,
                              i->exectime, i->numruns, i->lasttime);
        if (snlen < 0 || static_cast<std::size_t>(snlen) >= sizeof buf) {
            log_error("%s: Would truncate history entry for job %u; skipping.",
                      __func__, i->id);
            continue;
        }
        auto bsize = strlen(buf);
        while (!fwrite(buf, bsize, 1, f)) {
            if (ferror(f))
                goto fail;
        }
    }
    for (auto &i: deadstack) {
        auto snlen = snprintf(buf, sizeof buf, "%u=%li:%u|%lu\n", i->id,
                              i->exectime, i->numruns, i->lasttime);
        if (snlen < 0 || static_cast<std::size_t>(snlen) >= sizeof buf) {
            log_error("%s: Would truncate history entry for job %u; skipping.",
                      __func__, i->id);
            continue;
        }
        auto bsize = strlen(buf);
        while (!fwrite(buf, bsize, 1, f)) {
            if (ferror(f))
                goto fail;
        }
    }
fail:
    fclose(f);
}

void free_ipair_node_list (ipair_node_t *list)
{
    ipair_node_t *p;

    while (list) {
        p = list->next;
        delete list;
        list = p;
    }
}

void free_cronentry (cronentry_t **p)
{
    cronentry_t *q;

    if (!p)
        return;
    q = *p;
    if (!q)
        return;

    delete q->command;
    delete q->args;
    delete q->chroot;
    if (q->limits)
        delete q->limits;
    free_ipair_node_list(q->month);
    free_ipair_node_list(q->day);
    free_ipair_node_list(q->weekday);
    free_ipair_node_list(q->hour);
    free_ipair_node_list(q->minute);
    delete q;

    q = NULL;
}

