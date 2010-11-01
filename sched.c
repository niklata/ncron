/*
 * sched.c - ncron job scheduling
 *
 * (C) 2003-2008 Nicholas J. Kain <njk@aerifal.cx>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License version 2.1 as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <time.h>
#include <sys/time.h>
#include <string.h>

#include "defines.h"
#include "log.h"
#include "sched.h"

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
    int t, smallunit = MAXINT, dist = MAXINT, tinyunit = MAXINT;

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
    if (tinyunit == MAXINT)
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
    int t, smallunit = MAXINT, dist = MAXINT, tinyunit = MAXINT;

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
    int dist = MAXINT, t;
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
        case 1:
        case 3:
        case 5:
        case 7:
        case 8:   /* if only augustus hadn't been so arrogant */
        case 10:
        case 12:
            break;
        case 4:
        case 6:
        case 9:
        case 11:
            ret.h = 30;
            break;
        case 2:
            /* we follow the gregorian calendar */
            if (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0))
                ret.h = 29;
            else
                ret.h = 28;
            break;
    }
    return ret;
}

/* entry is obvious, stime is the time we're constraining
 * returns a time value that has been appropriately constrained */
time_t constrain_time(cronentry_t *entry, time_t stime)
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

void force_to_constraint(cronentry_t *entry)
{
    if (!entry)
        return;
    entry->exectime = constrain_time(entry, entry->exectime);
}

/* Used when jobs without exectimes are first loaded. */
time_t get_first_time(cronentry_t *entry)
{
    time_t etime;

    if (!entry)
        return 0;
    etime = time(NULL);
    etime = constrain_time(entry, etime);
    return etime;
}

/* stupidly advances to next time of execution; performs constraint.  */
time_t get_next_time(cronentry_t *entry)
{
    time_t etime, ctime;

    if (!entry)
        return 0;
    etime = ctime = time(NULL);
    etime += (time_t)entry->interval;
    etime = constrain_time(entry, etime);
    if (etime > ctime)
        return etime;
    else
        return 0;
}

/* inserts item into the sorted stack */
void stack_insert(cronentry_t *item, cronentry_t **stack)
{
    cronentry_t *p;
    if (!item || !stack)
        return;

    /* Null stack; stack <- item */
    if (*stack == NULL) {
        item->next = NULL;
        *stack = item;
        return;
    }

    p = *stack;

    /* if item < top of stack, insert item as head element */
    if (item->exectime < p->exectime) {
        item->next = p;
        *stack = item;
        return;
    }

    /* if only two elements in stack, insert item as tail */
    if (p->next == NULL) {
        item->next = NULL;
        p->next = item;
        return;
    }

    /* try to insert in the interior of stack */
    while (((cronentry_t *)p->next)->next != NULL) {
        /* if item < next element, insert before next element */
        if (item->exectime < ((cronentry_t *)p->next)->exectime) {
            item->next = p->next;
            p->next = item;
            return;
        }
        p = p->next;
    }

    /* implicit: p->next->next == NULL; we're at the end of the stack */
    if (item->exectime < ((cronentry_t *)p->next)->exectime) {
        /* insert before end */
        item->next = p->next;
        p->next = item;
    } else {
        /* place item as the last element */
        item->next = NULL;
        ((cronentry_t *)p->next)->next = item;
    }
}

void save_stack(char *file, cronentry_t *stack, cronentry_t *deadstack)
{
    FILE *f;
    cronentry_t *p;
    char buf[MAXLINE];

    f = fopen(file, "w");

    if (!f) {
        log_line("FATAL - failed to open history file %s for write\n", file);
        exit(EXIT_FAILURE);
    }

    p = stack;

    while (p) {
        snprintf(buf, sizeof buf, "%i=%i:%i|%i\n", p->id, (int)p->exectime,
                (unsigned int)p->numruns, (int)p->lasttime);
        fwrite(buf, sizeof(char), strlen(buf), f);

        p = p->next;
    }

    p = deadstack;
    while (p) {
        snprintf(buf, sizeof buf, "%i=%i:%i|%i\n", p->id, (int)p->exectime,
                (unsigned int)p->numruns, (int)p->lasttime);
        fwrite(buf, sizeof(char), strlen(buf), f);

        p = p->next;
    }

    fclose(f);
}

void free_ipair_node_list (ipair_node_t *list)
{
    ipair_node_t *p;

    while (list) {
        p = list->next;
        free(list);
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

    free(q->command);
    free(q->args);
    free(q->chroot);
    if (q->limits) {
        free(q->limits->cpu);
        free(q->limits->fsize);
        free(q->limits->data);
        free(q->limits->stack);
        free(q->limits->core);
        free(q->limits->rss);
        free(q->limits->nproc);
        free(q->limits->nofile);
        free(q->limits->memlock);
        free(q->limits->as);
        free(q->limits);
    }
    free_ipair_node_list(q->month);
    free_ipair_node_list(q->day);
    free_ipair_node_list(q->weekday);
    free_ipair_node_list(q->hour);
    free_ipair_node_list(q->minute);
    free(q);

    q = NULL;
}

/* frees the entire stack and all related resources; stack <- NULL */
void free_stack (cronentry_t **stack)
{
    cronentry_t *p, *q;

    if (!stack)
        return;
    p = *stack;

    while (p) {
        q = p->next;
        free_cronentry(&p);
        p = q;
    }

    *stack = NULL;
}

