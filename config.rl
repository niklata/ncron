/* config.rl - configure file parser for ncron
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

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <pwd.h>
#include <grp.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <limits.h>
#include "nk/log.h"
#include "nk/malloc.h"

#include "defines.h"
#include "config.h"
#include "sched.h"

/* BSD uses OFILE rather than NOFILE... */
#ifndef RLIMIT_NOFILE
#  define RLIMIT_NOFILE RLIMIT_OFILE
#endif

static int cfg_reload;    /* 0 on first call, 1 on subsequent calls */

struct ParseCfgState
{
    char v_str[1024];

    cronentry_t *stack;
    cronentry_t *deadstack;
    cronentry_t *ce;

    char *execfile;

    char *jobid_st;
    char *time_st;
    char *intv_st;
    char *intv2_st;
    char *strv_st;

    size_t v_strlen;

    unsigned int v_jobid;
    unsigned int v_time;

    int v_int;
    int v_int2;

    int cs;
    int noextime;
    int cmdret;

    bool intv2_exist;
};

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

struct hstm {
    char *st;
    int cs;
    time_t exectime;
    time_t lasttime;
    unsigned int numruns;
    bool got_exectime:1;
    bool got_lasttime:1;
    bool got_numruns:1;
};

%%{
    machine history_m;
    access hstm->;

    action St { hstm->st = p; }
    action LastTimeEn {
        hstm->lasttime = atoi(p);
        hstm->got_lasttime = 1;
    }
    action NumRunsEn {
        hstm->numruns = atoi(p);
        hstm->got_numruns = 1;
    }
    action ExecTimeEn {
        hstm->exectime = atoi(p);
        hstm->got_exectime = 1;
    }
    action IdEn { hstm->id = atoi(p); }

    lasttime = '|' digit+ > St % LastTimeEn;
    numruns = ':' digit+ > St % NumRunsEn;
    exectime = '=' digit+ > St % ExecTimeEn;
    id = digit+ > St % IdEn;
    main := id (numruns | exectime | lasttime)+;
}%%

%% write data;

static int do_get_history(struct hstm *hstm, char *buf, size_t blen)
{
    const char *p = buf;
    const char *pe = buf + blen;
    const char *eof = pe;

    %% write exec;

    if (cs == history_m_error)
        cmdret = -1;
    else if (cs >= history_m_first_final)
        cmdret = 1;
    else
        cmdret = -2;

}

static void get_history(cronentry_t *item, char *path, int noextime)
{
    struct hstm hstm;

    assert(item);
    hstm.exectime = 0;
    hstm.lasttime = 0;
    hstm.numruns = 0;

    if (!noextime && !hstm.got_exectime)
        item->exectime = 0;

    FILE *f = fopen(path, "r");

    if (!f) {
        log_line("failed to open history file \"%s\" for read", path);
        if (!noextime)
            item->exectime = (time_t)0; /* gracefully fail */
        return;
    }

    char buf[MAXLINE];
    memset(buf, 0, sizeof buf);

    while (!feof(f)) {
        char *s = fgets(buf, (int)(sizeof buf), f);
        int r = do_parse_config(&hstm, buf, strlen(buf));
        if (r < 0)
            suicide("%s: do_get_history(%s) failed", __func__, path);
        if (r == 1) {
            if (hstm.id == item->id) {
                if (hstm.got_lasttime)
                    item->lasttime = hstm.lasttime;
                if (hstm.got_numruns)
                    item->numruns = hstm.numruns;
                if (hstm.got_exectime && !noextime)
                    item->exectime = hstm.exectime;
                break;
            }
        }
    }
    if (fclose(f))
        suicide("%s: fclose(%s) failed: %s", __func__, path, strerror(errno));
}

static void setlim(struct ParseCfgState *ncs, int type)
{
    struct rlimit **p = NULL;
    if (!ncs->ce->limits) {
        ncs->ce->limits = xmalloc(sizeof(limit_t));
        nullify_limits(ncs->ce->limits);
    }
    switch (type) {
    case RLIMIT_CPU: *p = ncs->ce->limits->cpu; break;
    case RLIMIT_FSIZE: *p = ncs->ce->limits->fsize; break;
    case RLIMIT_DATA: *p = ncs->ce->limits->data; break;
    case RLIMIT_STACK: *p = ncs->ce->limits->stack; break;
    case RLIMIT_CORE: *p = ncs->ce->limits->core; break;
    case RLIMIT_RSS: *p = ncs->ce->limits->rss; break;
    case RLIMIT_NPROC: *p = ncs->ce->limits->nproc; break;
    case RLIMIT_NOFILE: *p = ncs->ce->limits->nofile; break;
    case RLIMIT_MEMLOCK: *p = ncs->ce->limits->memlock; break;
#ifndef BSD
    case RLIMIT_AS: *p = ncs->ce->limits->as; break;
#endif /* BSD */
    default: return;
    }
    if (!p)
        suicide("%s: unexpected NULL, corruption?", __func__);
    if (!*p)
        *p = xmalloc(sizeof(struct rlimit));
    if (ncs->v_int == 0) (*p)->rlim_cur = RLIM_INFINITY;
    else (*p)->rlim_cur = ncs->v_int;

    if (ncs->v_int2 == 0) (*p)->rlim_max = RLIM_INFINITY;
    else (*p)->rlim_cur = ncs->v_int2;
}

static void addipairlist(struct ParseCfgState *ncs,
                         ipair_node_t **list, int wildcard, int min, int max)
{
    ipair_node_t *l;
    int low, high = wildcard;

    low = ncs->v_int;
    if (ncs->intv2_exist)
        high = ncs->v_int2;

    if (high == wildcard)
        high = low;
    if (low > max || low < min)
        low = wildcard;
    if (high > max || high < min)
        high = wildcard;

    /* we don't allow meaningless 'rules' */
    if (low == wildcard && high == wildcard)
        return;

    if (*list == NULL) {
        *list = xmalloc(sizeof(ipair_node_t));
        l = *list;
    } else {
        l = *list;
        while (l->next)
            l = l->next;

        l->next = xmalloc(sizeof(ipair_node_t));
        l = l->next;
    }

    /* discontinuous range, split into two continuous rules... */
    if (low > high) {
        l->node.l = low;
        l->node.h = max;
        l->next = xmalloc(sizeof(ipair_node_t));
        l = l->next;
        l->node.l = min;
        l->node.h = high;
        l->next = NULL;
    } else {
        /* handle continuous ranges normally */
        l->node.l = low;
        l->node.h = high;
        l->next = NULL;
    }
}

static void setgroupv(struct ParseCfgState *ncs)
{
    struct group *grp = getgrnam(ncs->v_str);
    if (grp)
        ce->group = grp->gr_gid;
}

static void setuserv(struct ParseCfgState *ncs)
{
    struct passwd *pwd = getpwnam(ncs->v_str);
    if (pwd)
        ce->user = pwd->pw_uid;
}

static void parse_assign_str(char **dest, char *src, size_t srclen)
{
    if (srclen > INT_MAX)
        suicide("%s: srclen would overflow int", __func__);
    free(*dest);
    *dest = xmalloc(srclen+1);
    ssize_t l = snprintf(*dest, srclen+1, "%.*s", (int)srclen, src);
    if (l < 0 || (size_t)l >= srclen+1)
        suicide("%s: snprintf failed l=%u srclen+1=%u", __func__, l, srclen+1);
}

struct pkcm {
    char *st;
    int cs;
};

%%{
    machine parse_cmd_key_m;
    access pckm.;

    action St { pkcm.st = p; }
    action CmdEn {
        size_t cmdlen = p - pkcm.st;
        parse_assign_str(&ncs->ce->command, pkcm.st, cmdlen);
        // Unescape "\\" and "\ ".
        int prevsl = 0;
        size_t i = 0;
        while (ncs->ce->command[i]) {
            if (prevsl && (ncs->ce->command[i] == '\\'
                           || ncs->ce->command[i] == ' ')) {
                memmove(ncs->ce->command + i - 1,
                        ncs->ce->command + i, cmdlen--);
                continue;
            }
            if (ncs->ce->command[i] == '\\')
                prevsl = 1;
            ++i;
        }
    }
    action ArgEn {
        parse_assign_str(&ncs->ce->args, pkcm.st, p - pkcm.st);
    }

    sptab = [ \t];
    cmd = sptab+ ([^\0 \\] | '\\\\' | '\\ ')+ > St % CmdEn;
    args = sptab+ ([^\0])* > St % ArgEn;
    main := cmd args;
}%%

%% write data;

// cmdret = 0: Not parsed a command key yet.
// cmdret = 1: Success.  Got a command key.
// cmdret = -1: Error: malformed command key.
// cmdret = -2: Error: incomplete command key.
// cmdret = -3: Error: duplicate command key.
static void parse_command_key(struct ParseCfgState *ncs)
{
    const char *p = ncs->v_str;
    const char *pe = ncs->v_str + ncs->v_strlen;
    const char *eof = pe;

    struct pkcm pkcm;

    if (cmdret != 0) {
        cmdret = -3;
        return;
    }

    %% write exec;

    if (cs == parse_cmd_key_m_error)
        cmdret = -1;
    else if (cs >= parse_cmd_key_m_first_final)
        cmdret = 1;
    else
        cmdret = -2;
}

static void create_ce(struct ParseCfgState *ncs)
{
    assert(!ncs->ce);
    ncs->ce = xmalloc(sizeof(cronentry_t));
    nullify_item(ncs->ce);
    ncs->cmdret = 0;
    ncs->noextime = 0;
}

static void finish_ce(struct ParseCfgState *ncs)
{
    assert(ncs->ce);
    if (ncs->ce->id <= 0
        || (ncs->ce->interval <= 0 && ncs->ce->exectime <= 0)
        || !ncs->ce->command || ncs->cmdret < 1) {
        free_cronentry(&ncs->ce);
        return;
    }

    /* we have a job to insert */
    if (ncs->ce->exectime != 0) { /* runat task */
        get_history(ncs->ce, ncs->execfile, 1);

        /* insert iif we haven't exceeded maxruns */
        if (ncs->ce->maxruns == 0 || ncs->ce->numruns < ncs->ce->maxruns)
            stack_insert(ncs->ce, &ncs->stack);
        else
            stack_insert(ncs->ce, &ncs->deadstack);
    } else { /* interval task */
        get_history(ncs->ce, ncs->execfile, ncs->noextime && !cfg_reload);

        /* compensate for user edits to job constraints */
        ttm = get_first_time(ncs->ce);
        if (ttm - ncs->ce->lasttime >= ncs->ce->interval)
            ncs->ce->exectime = ttm;
        else
            force_to_constraint(ncs->ce);

        /* insert iif numruns < maxruns and no constr error */
        if ((ncs->ce->maxruns == 0 || ncs->ce->numruns < ncs->ce->maxruns)
            && ncs->ce->exectime != 0)
            stack_insert(ncs->cs, &ncs->stack);
        else
            stack_insert(ncs->cs, &ncs->deadstack);
    }
    ncs->cs = NULL;
}

%%{
    machine ncrontab;
    access ncs->;

    eqsep = [ \t]+ '=' [ \t]+;
    cmdterm = '\0';

    action TUnitSt { ncs->time_st = p; ncs->v_time = 0; }
    action TSecEn  { ncs->v_time +=          atoi(ncs->time_st); }
    action TMinEn  { ncs->v_time += 60     * atoi(ncs->time_st); }
    action THrEn   { ncs->v_time += 3600   * atoi(ncs->time_st); }
    action TDayEn  { ncs->v_time += 86400  * atoi(ncs->time_st); }
    action TWeekEn { ncs->v_time += 604800 * atoi(ncs->time_st); }

    action IntValSt {
        ncs->intv_st = p;
        ncs->v_int = ncs->v_int2 = 0;
        ncs->intv2_exist = false;
    }
    action IntValEn { ncs->v_int = atoi(ncs->intv_st); }
    action IntVal2St { ncs->intv2_st = p; }
    action IntVal2En {
        ncs->v_int2 = atoi(ncs->intv2_st);
        ncs->intv2_exist = true;
    }

    action StrValSt { ncs->strv_st = p; ncs->v_strlen = 0; }
    action StrValEn {
        ncs->v_strlen = p - ncs->strv_st;
        ssize_t snl = snprintf(ncs->v_str, sizeof ncs->v_str,
                               "%.*s", ncs->v_strlen, ncs->strv_st);
        if (snl < 0 || (size_t)snl >= sizeof ncs->v_str) {
            // XXX: Error/truncated.
        }
    }

    t_sec  = (digit+ > TUnitSt) 's' % TSecEn;
    t_min  = (digit+ > TUnitSt) 'm' % TMinEn;
    t_hr   = (digit+ > TUnitSt) 'h' % THrEn;
    t_day  = (digit+ > TUnitSt) 'd' % TDayEn;
    t_week = (digit+ > TUnitSt) 'w' % TWeekEn;
    t_any = (t_sec | t_min | t_hr | t_day | t_week);

    intval = (digit+ > IntValSt % IntValEn) cmdterm;
    timeval = t_any ([ \t]* t_any)* cmdterm;
    intrangeval = (digit+ > IntValSt % IntValEn)
                  (',' (digit+ > IntVal2St % IntVal2En))? cmdterm;
    stringval = ([^\0]+ > StrValSt % StrValEn) cmdterm;

    action JournalEn { ncs->ce->journal = 1; }
    action NoExecTimeEn { ncs->noextime = 1; }

    journal = 'journal'i cmdterm % JournalEn;
    noexectime = 'noexectime'i cmdterm % NoExecTimeEn;

    action RunAtEn {
        ncs->ce->exectime = ncs->v_int;
        ncs->ce->maxruns = 1;
        ncs->ce->journal = 1;
    }
    action MaxRunsEn {
        if (ncs->ce->exectime == 0)
            ncs->ce->maxruns = ncs->v_int;
    }

    runat = 'runat'i eqsep intval % RunAtEn;
    maxruns = 'maxruns'i eqsep intval % MaxRunsEn;

    action LimAsEn { setlim(ncs, RLIMIT_AS); }
    action LimMemlockEn { setlim(ncs, RLIMIT_MEMLOCK); }
    action LimNofileEn { setlim(ncs, RLIMIT_NOFILE); }
    action LimNprocEn { setlim(ncs, RLIMIT_NPROC); }
    action LimRssEn { setlim(ncs, RLIMIT_RSS); }
    action LimCoreEn { setlim(ncs, RLIMIT_CORE); }
    action LimStackEn { setlim(ncs, RLIMIT_STACK); }
    action LimDataEn { setlim(ncs, RLIMIT_DATA); }
    action LimFsizeEn { setlim(ncs, RLIMIT_FSIZE); }
    action LimCpuEn { setlim(ncs, RLIMIT_CPU); }

    lim_as = 'l_as'i eqsep intrangeval % LimAsEn;
    lim_memlock = 'l_memlock'i eqsep intrangeval % LimMemlockEn;
    lim_nofile = 'l_nofile'i eqsep intrangeval % LimNofileEn;
    lim_nproc = 'l_nproc'i eqsep intrangeval % LimNprocEn;
    lim_rss = 'l_rss'i eqsep intrangeval % LimRssEn;
    lim_core = 'l_core'i eqsep intrangeval % LimCoreEn;
    lim_stack = 'l_stack'i eqsep intrangeval % LimStackEn;
    lim_data = 'l_data'i eqsep intrangeval % LimDataEn;
    lim_fsize = 'l_fsize'i eqsep intrangeval % LimFsizeEn;
    lim_cpu = 'l_cpu'i eqsep intrangeval % LimCpuEn;

    action IntervalEn { ncs->ce->interval = ncs->v_time; }

    interval = 'interval'i eqsep timeval % IntervalEn;

    action MonthEn { addipairlist(ncs, ncs->ce->month, 0, 1, 12); }
    action DayEn { addipairlist(ncs, ncs->ce->day, 0, 1, 31); }
    action WeekdayEn { addipairlist(ncs, ncs->ce->weekday, 0, 1, 7); }
    action HourEn { addipairlist(ncs, ncs->ce->hour, 24, 0, 23); }
    action MinuteEn { addipairlist(ncs, ncs->ce->minute, 60, 0, 59); }

    month = 'month'i eqsep intrangeval % MonthEn;
    day = 'day'i eqsep intrangeval % DayEn;
    weekday = 'weekday'i eqsep intrangeval % WeekdayEn;
    hour = 'hour'i eqsep intrangeval % HourEn;
    minute = 'minute'i eqsep intrangeval % MinuteEn;

    action GroupEn { setgroupv(ncs); }
    action UserEn { setuserv(ncs); }
    action ChrootEn { parse_assign_str(&ncs->ce->chroot,
                                       ncs->v_str, ncs->v_strlen); }
    action CommandEn { parse_command_key(ncs); }

    group = 'group'i eqsep stringval % GroupEn;
    user = 'user'i eqsep stringval % UserEn;
    chroot = 'chroot'i eqsep stringval % ChrootEn;
    command = 'command'i eqsep stringval % CommandEn;

    cmd = command | chroot | user | group | minute | hour | weekday | day |
          month | interval | lim_cpu | lim_fsize | lim_data | lim_stack |
          lim_core | lim_rss | lim_nproc | lim_nofile | lim_memlock | lim_as |
          maxruns | runat | noexectime | journal;

    action JobIdSt { ncs->jobid_st = p; }
    action JobIdEn { ncs->v_jobid = atoi(ncs->jobid_st); }
    action CreateCe { create_ce(ncs); }

    jobid = ('!' > CreateCe) (digit+ > JobIdSt) cmdterm % JobIdEn;

    action FinishCe { finish_ce(ncs); }

    job = jobid cmds+ % FinishCe;
    main := job+;
}%%

%% write data;

static int do_parse_config(struct ParseCfgState *ncs,
                           char *data, size_t len, bool is_eof)
{
    const char *p = data;
    const char *pe = data + len;
    const char *eof = is_eof ? pe : 0;

    %% write exec;

    if (cs == ncrontab_error)
        return -1;
    if (cs >= ncrontab_first_final)
        return 1;
    return 0;
}

void parse_config(char *path, char *execfile, cronentry_t **stk,
                  cronentry_t **deadstk)
{
    char buf[MAXLINE];
    struct ParseCfgState ncs;
    memset(buf, 0, sizeof buf);
    memset(ncs, 0, sizeof ncs);
    ncs->execfile = execfile;
    ncs->stack = *stk;
    ncs->deadstack = *deadstack;

    FILE *f = fopen(path, "r");
    if (!f)
        suicide("%s: fopen(%s) failed: %s", __func__, path, strerror(errno));

    while (!feof(f)) {
        char *s = fgets(buf, (int)(sizeof buf), f);
        int r = do_parse_config(ncs, buf, strlen(buf), s == NULL);
        if (r < 0)
            suicide("%s: do_parse_config(%s) failed", __func__, path);
        if (r == 1)
            break;
    }
    if (fclose(f))
        suicide("%s: fclose(%s) failed: %s", __func__, path, strerror(errno));

    *stk = ncs->stack;
    *deadstack = ncs->deadstack;
    if (ncs->ce)
        free_cronentry(&ncs->ce); // Free partially built unused item.
    cfg_reload = 1;
}

