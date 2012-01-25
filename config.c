/*
 * config.c - configure file parser for ncron
 *
 * (C) 2003-2007 Nicholas J. Kain <njk@aerifal.cx>
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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>

#include "defines.h"
#include "sched.h"
#include "log.h"
#include "strl.h"

/* BSD uses OFILE rather than NOFILE... */
#ifndef RLIMIT_NOFILE
#  define RLIMIT_NOFILE RLIMIT_OFILE
#endif


static void nullify_item(cronentry_t *item)
{
    if (!item)
        return;
    item->id = 0;
    item->command = NULL;
    item->args = NULL;
    item->chroot = NULL;
    item->numruns = 0;
    item->maxruns = 0;
    item->journal = 0;
    item->user = 0;
    item->group = 0;
    item->month = NULL;
    item->day = NULL;
    item->weekday = NULL;
    item->hour = NULL;
    item->minute = NULL;
    item->interval = 0;
    item->exectime = 0;
    item->lasttime = 0;
    item->limits = NULL;
    item->next = NULL;
}

static void nullify_limits(limit_t *l)
{
    if (!l)
        return;
    l->cpu = NULL;
    l->fsize = NULL;
    l->data = NULL;
    l->stack = NULL;
    l->core = NULL;
    l->rss = NULL;
    l->nproc = NULL;
    l->nofile = NULL;
    l->memlock = NULL;
    l->as = NULL;
}


/*
 * history file looks like:
 *
 * id=exectime:numruns
 * ...
 */
void get_history(cronentry_t *item, char *path, int noextime)
{
    FILE *f;
    time_t exectm = 0;
    int fid = -1;
    char buf[MAXLINE];
    char *split = NULL;

    if (!item)
        return;

    f = fopen(path, "r");

    if (!f) {
        log_line("failed to open history file \"%s\" for read\n", path);
        if (!noextime)
            item->exectime = (time_t)0; /* gracefully fail */
        return;
    }

    memset(buf, 0, sizeof buf);

    while (fid != item->id && !feof(f)) {
        if (!fgets(buf, (int)(sizeof buf), f))
            break;
        fid = atoi(buf);
    }

    /* found the right line */
    if (fid == item->id) {
        split = strchr(buf, '=');
        if (split)
            exectm = (time_t)atoi(++split);
        split = strchr(buf, ':');
        if (split)
            item->numruns = (unsigned int)atoi(++split);
        split = strchr(buf, '|');
        if (split)
            item->lasttime = (time_t)atoi(++split);
    }

    if (fclose(f)) {
        log_line("FATAL - failed to close \"%s\"\n", path);
        exit(EXIT_FAILURE);
    }

    if (exectm < 0)
        exectm = 0;

    if (!noextime)
        item->exectime = exectm;
}

static void parse_command_key(char *value, cronentry_t *item)
{
    int n, cmdstart, cmdend, noargs, escaped_space, ret;
    if (!strlen(value))
        return; /* empty */

    /* skip leading spaces */
    for (n=0; isspace(value[n]); n++);
    if (value[n] == '\0')
        return;

    cmdstart = n;
    escaped_space = 0;

    /* get all of the command; we handle escaped spaces of the form:
     * "\ " as being part of the command, hence the added complexity */
    do {
        for (++n; !isspace(value[n]) && value[n] != '\0'; n++) {
            if (value[n] == '\0') {
                /* command with no arguments, assign to item and exit */
                ret = (int)strlen(&value[cmdstart]) + 1;
                if (ret == 1)
                    return; /* empty */

                free(item->command);
                item->command = malloc(ret);
                if (!item->command)
                    goto parse_command_key_failed;

                strlcpy(item->command, &value[cmdstart], (size_t)ret);
                return;
            }
        }
        if (value[n-1] == '\\')
            escaped_space = 1;
        else
            escaped_space = 0;
    } while (escaped_space == 1);

    noargs = 0;
    cmdend = n;

    /* skip leading spaces from our arguments */
    for (; isspace(value[n]); n++);
    if (value[n] == '\0')
        noargs = 1;

    /* okay, at this point, we have value[cmdstart] is the start of the
     * actual command.  n will be the start of our argument.  */

    /* assign our command to item */
    free(item->command);
    item->command = malloc(cmdend - cmdstart + 1);
    if (!item->command)
        goto parse_command_key_failed;

    strlcpy(item->command, &value[cmdstart], (size_t)(cmdend - cmdstart + 1));

    /* if we have arguments, assign them to item */
    if (!noargs) {
        ret = strlen(&value[n]) + 1;

        free(item->args);
        item->args = malloc(ret);
        if (item->args == NULL)
            goto parse_command_key_failed;

        strlcpy(item->args, &value[n], (size_t)ret);
    }

    return;

parse_command_key_failed:
    log_line("parse_config: FATAL - malloc failed while parsing \"command=%s\"!\n",
            value);
    exit(EXIT_FAILURE);
}

static void parse_chroot_key(char *value, cronentry_t *item)
{
    char *p;
    int l;

    if (!value)
        return; /* empty */
    p = value;

    /* skip leading spaces */
    while (isspace(*p))
        p++;
    if (*p == '\0')
        return;

    l = strlen(p) + 1;

    /* assign our chroot path to item */
    free(item->chroot);
    item->chroot = malloc(l);

    if (!item->chroot) {
        log_line("parse_config: FATAL - malloc failed while parsing \"command=%s\"!\n",
                value);
        exit(EXIT_FAILURE);
    }

    strlcpy(item->chroot, p, l);
}

static void add_to_ipair_list(ipair_node_t **list, char *value, int wildcard,
        int min, int max)
{
    ipair_node_t *l;
    int low, high = wildcard;

    if (list == NULL || value == NULL)
        return;

    low = (int)strtol(value, (char **)NULL, 10);
    value = strchr(value, ',');
    if (value++ != NULL)
        high = (int)strtol(value, (char **)NULL, 10);
    if (high == wildcard)
        high = low;
    if (low > max || low < min)
        low = wildcard;
    if (high > max || high < min)
        high = wildcard;

    /* we don't allow meaningless 'rules' */
    if (low == wildcard && high == wildcard)
        return;

    /* discontinuous range, split into two continuous rules... */
    if (low > high) {
        if (*list == NULL) {
            (*list) = malloc((size_t)sizeof (ipair_node_t));
            if (*list == NULL)
                goto add_to_ipair_list_failed;
            l = *list;
            goto add_to_ipair_list_dc_first;
        }
        l = *list;
        while (l->next)
            l = l->next;

        l->next = malloc((size_t)sizeof (ipair_node_t));
        if (l->next == NULL)
            goto add_to_ipair_list_failed;
        l = l->next;
add_to_ipair_list_dc_first:
        l->node.l = low;
        l->node.h = max;
        l->next = malloc((size_t)sizeof (ipair_node_t));
        if (l->next == NULL)
            goto add_to_ipair_list_failed;
        l = l->next;
        l->node.l = min;
        l->node.h = high;
        l->next = NULL;
        return;
    }

    /* handle continuous ranges normally */
    if (*list == NULL) {
        *list = malloc((size_t)sizeof (ipair_node_t));
        if (*list == NULL)
            goto add_to_ipair_list_failed;
        l = *list;
        l->node.l = low;
        l->node.h = high;
        l->next = NULL;
        return;
    }

    l = *list;
    while (l->next)
        l = l->next;

    l->next = malloc((size_t)sizeof (ipair_node_t));
    if (!*list)
        goto add_to_ipair_list_failed;
    l = l->next;
    l->node.l = low;
    l->node.h = high;
    l->next = NULL;
    return;

add_to_ipair_list_failed:
    log_line("FATAL - add_to_ipair_list: malloc failed\n");
    exit(EXIT_FAILURE);
}

static void parse_rlimit_key(int type, char *value, cronentry_t *item)
{
    int low = 0, high = 0;
    struct rlimit **p = NULL;

    if (!value || !item)
        return;

    low = (int)strtol(value, (char **)NULL, 10);
    value = strchr(value, ',');
    if (value++)
        high = (int)strtol(value, (char **)NULL, 10);

    if (!item->limits) {
        item->limits = malloc((size_t)sizeof (limit_t));
        if (!item->limits) {
            log_line("FATAL - parse_rlimit_key: malloc_failed\n");
            exit(EXIT_FAILURE);
        }
        nullify_limits(item->limits);
    }

    switch (type) {
        case RLIMIT_CPU:
            *p = item->limits->cpu;
            break;
        case RLIMIT_FSIZE:
            *p = item->limits->fsize;
            break;
        case RLIMIT_DATA:
            *p = item->limits->data;
            break;
        case RLIMIT_STACK:
            *p = item->limits->stack;
            break;
        case RLIMIT_CORE:
            *p = item->limits->core;
            break;
        case RLIMIT_RSS:
            *p = item->limits->rss;
            break;
        case RLIMIT_NPROC:
            *p = item->limits->nproc;
            break;
        case RLIMIT_NOFILE:
            *p = item->limits->nofile;
            break;
        case RLIMIT_MEMLOCK:
            *p = item->limits->memlock;
            break;
#ifndef BSD
        case RLIMIT_AS:
            *p = item->limits->as;
            break;
#endif /* BSD */
        default:
            return;
            break;
    }
    if (!p) {
        log_line("parse_rlimit_key: FATAL - a pointer that should never be NULL was NULL; program corruption?\n");
        exit(EXIT_FAILURE);
    }

    if (!*p) {
        *p = malloc((size_t)sizeof (struct rlimit));
        if (!*p) {
            log_line("parse_rlimit_key: FATAL - malloc failed\n");
            exit(EXIT_FAILURE);
        }
    }

    if (low == 0)
        (*p)->rlim_cur = RLIM_INFINITY;
    else
        (*p)->rlim_cur = low;

    if (high == 0)
        (*p)->rlim_max = RLIM_INFINITY;
    else
        (*p)->rlim_cur = high;
}

/*
 * Parses a given interval value, applying units of time as necessary.
 * Any number before a valid unit (s,m,h,d) == (seconds, minutes, hours, days)
 * will be associated with the next valid unit specifier.  Multiple units
 * are allowed to be in sequence and will be implicitly added.  No unit
 * specifier (a bare number) implies a unit of seconds.
 */
static unsigned int parse_interval(char *value)
{
    unsigned int ret, i, len;
    char *p, *start;
    char const units[] = "smhdw";

    if (value == NULL)
        return 0;

    len = strlen(value);

    for (i=0; i<len; i++)
        value[i] = tolower(value[i]);

    p = strpbrk(value, units);
    if (!p)
        return (unsigned int)strtol(value, (char **)NULL, 10);

    i = 0;
    ret = 0;
    start = value;
    do {
        if (*p == 's')
            ret += (unsigned int)strtol(value + i, (char **)NULL, 10);
        if (*p == 'm')
            ret += 60 * (unsigned int)strtol(value + i, (char **)NULL, 10);
        if (*p == 'h')
            ret += 3600 * (unsigned int)strtol(value + i, (char **)NULL, 10);
        if (*p == 'd')
            ret += 86400 * (unsigned int)strtol(value + i, (char **)NULL, 10);
        if (*p == 'w')
            ret += 604800 * (unsigned int)strtol(value + i, (char **)NULL, 10);
        i += (unsigned int)(p - start) + 1;
        p = strpbrk(value + i, units);
    } while (p && i < len);

    return ret;
}

/*
 * does the real work of creating a new stack from the config
 * file and the execfile
 */
void parse_config(char *path, char *execfile, cronentry_t **stk,
        cronentry_t **deadstk)
{
    FILE *f;
    int n, trash, noextime=0;
    static int reload = 0;    /* 0 on first call, 1 on subsequent calls */
    cronentry_t *item = NULL;
    char buf[MAXLINE], key[MAXLINE], value[MAXLINE];
    char *split;
    time_t ttm;
    struct passwd *pwd;
    struct group *grp;
    cronentry_t *stack, *deadstack;

    stack = *stk;
    deadstack = *deadstk;

    f = fopen(path, "r");

    if (f == NULL) {
        log_line("parse_config: FATAL - failed to open configure file \"%s\"!\n", path);
        exit(EXIT_FAILURE);
    }

    memset(buf, 0, sizeof buf);

    while (!feof(f)) {
        if (!fgets(buf, (int)(sizeof buf), f))
            break;

        for (n=0; isspace((unsigned char)buf[n]); ++n);

        if (buf[n] == '#')
            continue; /* skip comments */

        if (buf[n] == '!') { /* new item, id must be > 0*/

            if (item) {
                /* we have a job to insert */
                if (item->id > 0 && (item->interval > 0
                            || item->exectime != 0) && item->command)
                {
                    if (item->exectime != 0) { /* runat task */
                        get_history(item, execfile, 1);

                        /* insert iif we haven't exceeded maxruns */
                        if (item->maxruns == 0 || item->numruns <
                                item->maxruns)
                            stack_insert(item, &stack);
                        else
                            stack_insert(item, &deadstack);
                    } else { /* interval task */
                        get_history(item, execfile, noextime && !reload);

                        /* compensate for user edits to job constraints */
                        ttm = get_first_time(item);
                        if (ttm - item->lasttime >= item->interval)
                            item->exectime = ttm;
                        else
                            force_to_constraint(item);

                        /* insert iif numruns < maxruns and no constr error */
                        if ((item->maxruns == 0 || item->numruns <
                                    item->maxruns) && item->exectime != 0)
                            stack_insert(item, &stack);
                        else
                            stack_insert(item, &deadstack);
                    }
                } else
                    free_cronentry(&item);
            }

            /* create a new, blank item */
            item = malloc((size_t)sizeof (cronentry_t));
            if (!item) {
                log_line("parse_config: FATAL - malloc failed to create new cronentry_t\n");
                exit(EXIT_FAILURE);
            } else {
                nullify_item(item);
                item->id = (int)strtol(&buf[n+1], (char **)NULL, 10);
            }
            continue;
        }

        if (!item)
            continue; /* haven't found a !-entry yet */

        /* strip trailing \n or \r */
        trash = strcspn(&buf[n], "\n\r");
        buf[n+trash] = '\0';

        /* verify line as key/value pair */
        split = strchr(buf, '=');
        if (split == NULL)
            continue; /* not a key/value pair */

        /* split line into key/value pair */
        strlcpy(value, split + 1, MAXLINE);
        *split = '\0';
        strlcpy(key, &buf[n], MAXLINE);

        /* after this point, we handle keys by name */

        if (strncmp("command", key, 7) == 0) {
            parse_command_key(value, item);
            continue;
        }

        if (strncmp("chroot", key, 6) == 0) {
            parse_chroot_key(value, item);
            continue;
        }

        if (strncmp("l_cpu", key, 5) == 0) {
            parse_rlimit_key(RLIMIT_CPU, value, item);
            continue;
        }

        if (strncmp("l_fsize", key, 7) == 0) {
            parse_rlimit_key(RLIMIT_FSIZE, value, item);
            continue;
        }

        if (strncmp("l_data", key, 6) == 0) {
            parse_rlimit_key(RLIMIT_DATA, value, item);
            continue;
        }

        if (strncmp("l_stack", key, 7) == 0) {
            parse_rlimit_key(RLIMIT_STACK, value, item);
            continue;
        }

        if (strncmp("l_core", key, 6) == 0) {
            parse_rlimit_key(RLIMIT_CORE, value, item);
            continue;
        }

        if (strncmp("l_rss", key, 5) == 0) {
            parse_rlimit_key(RLIMIT_RSS, value, item);
            continue;
        }

        if (strncmp("l_nproc", key, 7) == 0) {
            parse_rlimit_key(RLIMIT_NPROC, value, item);
            continue;
        }

        if (strncmp("l_nofile", key, 8) == 0) {
            parse_rlimit_key(RLIMIT_NOFILE, value, item);
            continue;
        }

        if (strncmp("l_memlock", key, 9) == 0) {
            parse_rlimit_key(RLIMIT_MEMLOCK, value, item);
            continue;
        }

#ifndef BSD
        if (strncmp("l_as", key, 4) == 0) {
            parse_rlimit_key(RLIMIT_AS, value, item);
            continue;
        }
#endif

        if (strncmp("user", key, 4) == 0) {
            pwd = getpwnam(value);
            if (pwd != NULL) item->user = pwd->pw_uid;
            continue;
        }

        if (strncmp("group", key, 5) == 0) {
            grp = getgrnam(value);
            if (grp != NULL) item->group = grp->gr_gid;
            continue;
        }

        if (strncmp("month", key, 5) == 0) {
            add_to_ipair_list(&(item->month), value, 0, 1, 12);
            continue;
        }

        if (strncmp("day", key, 3) == 0) {
            add_to_ipair_list(&(item->day), value, 0, 1, 31);
            continue;
        }

        if (strncmp("weekday", key, 7) == 0) {
            add_to_ipair_list(&(item->weekday), value, 0, 1, 7);
            continue;
        }

        if (strncmp("hour", key, 4) == 0) {
            add_to_ipair_list(&(item->hour), value, 24, 0, 23);
            continue;
        }

        if (strncmp("minute", key, 6) == 0) {
            add_to_ipair_list(&(item->minute), value, 60, 0, 59);
            continue;
        }

        if (strncmp("interval", key, 8) == 0) {
            /* failure yields 0 which denies the event anyway */
            item->interval = parse_interval(value);
            continue;
        }

        if (strncmp("maxruns", key, 7) == 0) {
            /* again, failure yields 0, which indicates no limit */
            if (item->exectime == 0) /* don't modify jobs with runat */
                item->maxruns = (unsigned int)strtol(value, (char **)NULL, 10);
            continue;
        }

        if (strncmp("runat", key, 5) == 0) {
            /* 0 is handled explicitly so that the job won't spam */
            item->exectime = (unsigned int)strtol(value, (char **)NULL, 10);
            item->maxruns = 1;
            item->journal = 1;
            continue;
        }

        if (strncmp ("journal", key, 7) == 0) {
            item->journal = 1;
            continue;
        }

        if (strncmp ("noexectime", key, 10) == 0) {
            noextime = 1;
            continue;
        }
    }

    if (fclose(f)) {
        log_line("parse_config: FATAL - failed to close \"%s\"!\n", path);
        exit(EXIT_FAILURE);
    }

    *stk = stack;
    if (item)
        free_cronentry(&item); /* free partially built unused item */
    reload = 1;
}

