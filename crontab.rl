// Copyright 2003-2024 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <assert.h>
#include "nk/log.h"
#include "xmalloc.h"
#include "strconv.h"
#include "ncron.h"
#include "sched.h"

#define MAX_LINE 2048

extern int gflags_debug;
extern size_t g_njobs;
extern struct Job *g_jobs;

struct item_history {
    time_t exectime;
    time_t lasttime;
    unsigned int numruns;
};

struct ParseCfgState
{
    char v_str[MAX_LINE];

    struct Job **stackl;
    struct Job **deadstackl;

    struct Job *ce;

    const char *jobid_st;
    const char *time_st;
    const char *intv_st;
    const char *intv2_st;
    const char *strv_st;

    size_t v_strlen;
    size_t linenum;

    unsigned int v_time;

    int v_int1;
    int v_int2;
    int v_int3;
    int v_int4;

    int cs;
    bool have_command;
    bool intv2_exist;
    bool seen_cst_hhmm;
    bool seen_cst_wday;
    bool seen_cst_mday;
    bool seen_cst_mon;
    bool seen_job;
};

static void ParseCfgState_init(struct ParseCfgState *self, struct Job **stk, struct Job **dstk)
{
    *self = (struct ParseCfgState){
        .stackl = stk,
        .deadstackl = dstk,
        .v_int3 = -1,
        .v_int4 = -1,
    };
}

static void ParseCfgState_create_ce(struct ParseCfgState *self)
{
    if (self->ce == g_jobs + g_njobs) {
        suicide("job count mismatch\n");
    }
    job_init(self->ce);
    self->seen_job = true;
    self->have_command = false;
    self->seen_cst_hhmm = false;
    self->seen_cst_wday = false;
    self->seen_cst_mday = false;
    self->seen_cst_mon = false;
}

static void ParseCfgState_debug_print_ce(const struct ParseCfgState *self)
{
    if (!gflags_debug) return;
    const struct Job *j = self->ce;
    log_line("id=%d:\tcommand: %s\n", j->id_, j->command_ ? j->command_ : "");
    log_line("\targs: %s\n", j->args_ ? j->args_ : "");
    log_line("\tnumruns: %u\n\tmaxruns: %u\n", j->numruns_, j->maxruns_);
    log_line("\tjournal: %s\n", j->journal_ ? "true" : "false");
    log_line("\trunat: %s\n", j->runat_ ? "true" : "false");
    log_line("\tinterval: %u\n\texectime: %lu\n\tlasttime: %lu\n", j->interval_, j->exectime_, j->lasttime_);
}

static void ParseCfgState_finish_ce(struct ParseCfgState *self)
{
    if (!self->seen_job) return;

    ParseCfgState_debug_print_ce(self);

    if (self->ce->id_ < 0
        || (self->ce->interval_ <= 0 && self->ce->exectime_ <= 0)
        || !self->ce->command_ || !self->have_command) {
        suicide("ERROR IN CRONTAB: invalid id, command, or interval for job %d\n", self->ce->id_);
    }

    // XXX: O(n^2) might be nice to avoid.
    for (struct Job *i = g_jobs, *iend = self->ce; i != iend; ++i) {
        if (i->id_ == self->ce->id_) {
            suicide("ERROR IN CRONTAB: duplicate entry for job %d\n", self->ce->id_);
        }
    }

    // Preserve this job and work on the next one.
    ++self->ce;
}

struct hstm {
    const char *st;
    int cs;
    int id;
    struct item_history h;
    bool parse_error;
};

static void hstm_print(const struct hstm *self)
{
    if (!gflags_debug) return;
    log_line("id=%d:\tnumruns = %u\n\texectime = %lu\n\tlasttime = %lu\n",
             self->id, self->h.numruns, self->h.exectime, self->h.lasttime);
}

%%{
    machine history_m;
    access hst->;

    action St { hst->st = p; }
    action LastTimeEn {
        if (!strconv_to_i64(hst->st, p, &hst->h.lasttime)) {
            hst->parse_error = true;
            fbreak;
        }
    }
    action NumRunsEn {
        if (!strconv_to_u32(hst->st, p, &hst->h.numruns)) {
            hst->parse_error = true;
            fbreak;
        }
    }
    action ExecTimeEn {
        if (!strconv_to_i64(hst->st, p, &hst->h.exectime)) {
            hst->parse_error = true;
            fbreak;
        }
    }
    action IdEn {
        if (!strconv_to_i32(hst->st, p, &hst->id)) {
            hst->parse_error = true;
            fbreak;
        }
    }

    lasttime = '|' digit+ > St % LastTimeEn;
    numruns = ':' digit+ > St % NumRunsEn;
    exectime = '=' digit+ > St % ExecTimeEn;
    id = digit+ > St % IdEn;
    main := id exectime numruns lasttime;
}%%

%% write data;

static int do_parse_history(struct hstm *hst, const char *p, size_t plen)
{
    const char *pe = p + plen;
    const char *eof = pe;

    %% write init;
    %% write exec;

    if (hst->parse_error) return -1;
    if (hst->cs >= history_m_first_final)
        return 1;
    if (hst->cs == history_m_error)
        return -1;
    return -2;
}

static void parse_history(char const *path)
{
    char buf[MAX_LINE];
    FILE *f = fopen(path, "r");
    if (!f) {
        log_line("Failed to open history file '%s' for read: %s\n", path, strerror(errno));
        return;
    }
    size_t linenum = 0;
    while (!feof(f)) {
        if (!fgets(buf, sizeof buf, f)) {
            if (!feof(f)) log_line("IO error reading history file '%s'\n", path);
            break;
        }
        size_t llen = strlen(buf);
        if (llen == 0)
            continue;
        if (buf[llen-1] == '\n')
            buf[--llen] = 0;
        ++linenum;
        struct hstm hst = { .st = NULL, .cs = 0, .id = -1, .parse_error = false };
        int r = do_parse_history(&hst, buf, llen);
        if (r < 0) {
            log_line("%s history entry at line %zu; ignoring\n",
                     r == -2 ? "Incomplete" : "Malformed", linenum);
            continue;
        }

        for (struct Job *j = g_jobs, *jend = g_jobs + g_njobs; j != jend; ++j) {
            if (j->id_ == hst.id) {
                hstm_print(&hst);
                j->numruns_ = hst.h.numruns;
                j->lasttime_ = hst.h.lasttime;
                if (!j->runat_) {
                    j->exectime_ = hst.h.exectime;
                    job_set_initial_exectime(j);
                } else {
                    if (j->interval_ > 0) {
                        suicide("ERROR IN CRONTAB: interval is unused when runat is set: job %d\n", j->id_);
                    }
                }
            }
        }
    }
    fclose(f);
    for (struct Job *j = g_jobs, *jend = g_jobs + g_njobs; j != jend; ++j) {
        // Periodic jobs that never ran in the past should run ASAP.
        if (!j->runat_ && !j->exectime_) {
           j->exectime_ = time(NULL);
        }
    }
}

static bool ParseCfgState_add_cst_mon(struct ParseCfgState *self)
{
    int min = self->v_int1;
    int max = self->intv2_exist ? self->v_int2 : -1;
    if (max < 0) max = min;
    assert(min > 0 && min <= 12);
    assert(max > 0 && max <= 12);
    if (max < min) return false;
    if (min <= 0 || min > 12) return false;
    if (max <= 0 || max > 12) return false;
    if (!self->seen_cst_mon) {
        memset(&self->ce->cst_mon_, 0, sizeof self->ce->cst_mon_);
        self->seen_cst_mon = true;
    }
    for (int i = min; i <= max; ++i)
        self->ce->cst_mon_[i - 1] = true;
    return true;
}

static bool ParseCfgState_add_cst_mday(struct ParseCfgState *self)
{
    int min = self->v_int1;
    int max = self->intv2_exist ? self->v_int2 : -1;
    if (max < 0) max = min;
    assert(min > 0 && min <= 31);
    assert(max > 0 && max <= 31);
    if (max < min) return false;
    if (min <= 0 || min > 31) return false;
    if (max <= 0 || max > 31) return false;
    if (!self->seen_cst_mday) {
        memset(&self->ce->cst_mday_, 0, sizeof self->ce->cst_mday_);
        self->seen_cst_mday = true;
    }
    for (int i = min; i <= max; ++i)
        self->ce->cst_mday_[i - 1] = true;
    return true;
}

static bool ParseCfgState_add_cst_wday(struct ParseCfgState *self)
{
    int min = self->v_int1;
    int max = self->intv2_exist ? self->v_int2 : -1;
    if (max < 0) max = min;
    assert(min > 0 && min <= 7);
    assert(max > 0 && max <= 7);
    if (max < min) return false;
    if (min <= 0 || min > 7) return false;
    if (max <= 0 || max > 7) return false;
    if (!self->seen_cst_wday) {
        memset(&self->ce->cst_wday_, 0, sizeof self->ce->cst_wday_);
        self->seen_cst_wday = true;
    }
    for (int i = min; i <= max; ++i)
        self->ce->cst_wday_[i - 1] = true;
    return true;
}

static bool ParseCfgState_add_cst_time(struct ParseCfgState *self)
{
    bool single_value = self->v_int3 == -1 && self->v_int4 == -1;
    // Enforce that range is low-high.
    if (!single_value) {
        if (self->v_int3 < self->v_int1) return false;
        if (self->v_int3 == self->v_int1) {
            if (self->v_int4 < self->v_int2) return false;
        }
    }
    if (!self->seen_cst_hhmm) {
        memset(&self->ce->cst_hhmm_, 0, sizeof self->ce->cst_hhmm_);
        self->seen_cst_hhmm = true;
    }
    int min = self->v_int1 * 60 + self->v_int2;
    int max = self->v_int3 * 60 + self->v_int4;
    assert(min >= 0 && min < 1440);
    assert(max >= 0 && max < 1440);
    for (int i = min; i <= max; ++i)
        self->ce->cst_hhmm_[i] = true;
    return true;
}

struct Pckm {
    char *st;
    int cs;
};

%%{
    machine parse_cmd_key_m;
    access pckm.;

    action St { pckm.st = p; }
    action CmdEn {
        size_t l = p > pckm.st ? (size_t)(p - pckm.st) : 0;
        if (l) {
            char *ts = xmalloc(l + 1);
            bool prior_bs = false;
            char *d = ts;
            for (char *c = pckm.st; c < p; ++c) {
                if (!prior_bs) {
                    switch (*c) {
                    case 0: abort(); // should never happen by construction
                    case '\\': prior_bs = true; break;
                    default: *d++ = *c; break;
                    }
                } else {
                    if (!*c) abort(); // should never happen by construction
                    *d++ = *c;
                    prior_bs = false;
                }
            }
            if (prior_bs) *d++ = '\\';
            *d++ = 0;
            self->ce->command_ = ts;
        }
    }
    action ArgEn {
        size_t l = p > pckm.st ? (size_t)(p - pckm.st) : 0;
        if (l) {
            char *ts = xmalloc(l + 1);
            memcpy(ts, pckm.st, l);
            ts[l] = 0;
            self->ce->args_ = ts;
        }
    }

    sptab = [ \t];
    cmdstr = ([^\0 \t] | '\\ ' | '\\\t')+;
    cmd = sptab* (cmdstr > St % CmdEn);
    args = sptab+ ([^\0])* > St % ArgEn;
    main := cmd args?;
}%%

%% write data;

static void ParseCfgState_parse_command_key(struct ParseCfgState *self)
{
    char *p = self->v_str;
    const char *pe = self->v_str + self->v_strlen;
    const char *eof = pe;

    struct Pckm pckm = {0};

    if (self->have_command)
        suicide("Duplicate 'command' value at line %zu\n", self->linenum);

    %% write init;
    %% write exec;

    if (pckm.cs == parse_cmd_key_m_error) {
        suicide("Malformed 'command' value at line %zu\n", self->linenum);
    } else if (pckm.cs >= parse_cmd_key_m_first_final) {
        self->have_command = true;
    } else {
        suicide("Incomplete 'command' value at line %zu\n", self->linenum);
    }
}

static void ParseCfgState_parse_time_unit(const struct ParseCfgState *self, const char *p, unsigned unit, unsigned *dest)
{
    unsigned t;
    if (!strconv_to_u32(self->time_st, p - 1, &t))
        suicide("Invalid time unit at line %zu\n", self->linenum);
    *dest += unit * t;
}

static void parse_int_value(const char *p, const char *start, size_t linenum, int *dest)
{
    if (!strconv_to_i32(start, p, dest))
        suicide("Invalid integer value at line %zu\n", linenum);
}

static void swap_int_pair(int *a, int *b) { int t = *a; *a = *b; *b = t; }

%%{
    machine ncrontab;
    access ncs->;

    spc = [ \t];
    eqsep = spc* '=' spc*;

    action TUnitSt { ncs->time_st = p; ncs->v_time = 0; }
    action TSecEn  { ParseCfgState_parse_time_unit(ncs, p, 1, &ncs->v_time); }
    action TMinEn  { ParseCfgState_parse_time_unit(ncs, p, 60, &ncs->v_time); }
    action THrEn   { ParseCfgState_parse_time_unit(ncs, p, 3600, &ncs->v_time); }
    action TDayEn  { ParseCfgState_parse_time_unit(ncs, p, 86400, &ncs->v_time); }
    action TWeekEn { ParseCfgState_parse_time_unit(ncs, p, 604800, &ncs->v_time); }

    action IntValSt {
        ncs->intv_st = p;
        ncs->v_int1 = ncs->v_int2 = 0;
        ncs->intv2_exist = false;
    }
    action IntValEn { parse_int_value(p, ncs->intv_st, ncs->linenum, &ncs->v_int1); }
    action IntVal2St { ncs->intv2_st = p; }
    action IntVal2En { parse_int_value(p, ncs->intv2_st, ncs->linenum, &ncs->v_int2); ncs->intv2_exist = true; }
    action IntValSwap {
        swap_int_pair(&ncs->v_int1, &ncs->v_int3);
        swap_int_pair(&ncs->v_int2, &ncs->v_int4);
    }
    action IntVal34Clear {
        ncs->v_int3 = -1;
        ncs->v_int4 = -1;
    }

    action StrValSt { ncs->strv_st = p; ncs->v_strlen = 0; }
    action StrValEn {
        ncs->v_strlen = p > ncs->strv_st ? (size_t)(p - ncs->strv_st) : 0;
        if (ncs->v_strlen >= sizeof ncs->v_str)
            suicide("error parsing line %zu in crontab: too long\n", ncs->linenum);
        memcpy(ncs->v_str, ncs->strv_st, ncs->v_strlen);
        ncs->v_str[ncs->v_strlen] = 0;
    }

    t_sec  = (digit+ > TUnitSt) 's' % TSecEn;
    t_min  = (digit+ > TUnitSt) 'm' % TMinEn;
    t_hr   = (digit+ > TUnitSt) 'h' % THrEn;
    t_day  = (digit+ > TUnitSt) 'd' % TDayEn;
    t_week = (digit+ > TUnitSt) 'w' % TWeekEn;
    t_any = (t_sec | t_min | t_hr | t_day | t_week);

    intval = (digit+ > IntValSt % IntValEn);
    timeval = t_any (spc* t_any)*;
    intrangeval = (digit+ > IntValSt % IntValEn)
                  ('-' (digit+ > IntVal2St % IntVal2En))?;
    stringval = ([^\0\n]+ > StrValSt % StrValEn);

    action JournalEn { ncs->ce->journal_ = true; }
    journal = 'journal'i % JournalEn;

    action RunAtEn {
        ncs->ce->runat_ = true;
        ncs->ce->exectime_ = ncs->v_int1;
        ncs->ce->maxruns_ = 1;
        ncs->ce->journal_ = true;
    }
    action MaxRunsEn {
        if (!ncs->ce->runat_)
            ncs->ce->maxruns_ = ncs->v_int1 > 0 ? (unsigned)ncs->v_int1 : 0;
    }

    runat = 'runat'i eqsep intval % RunAtEn;
    maxruns = 'maxruns'i eqsep intval % MaxRunsEn;

    action IntervalEn { ncs->ce->interval_ = ncs->v_time; }

    interval = 'interval'i eqsep timeval % IntervalEn;

    xhour = digit | ('0' digit) | ('1' digit) | '20' | '21' | '22' | '23';
    xminute = ('0' | '1' | '2' | '3' | '4' | '5') digit;
    hhmm = xhour > IntValSt % IntValEn ':' xminute > IntVal2St % IntVal2En;
    hhmm_range = hhmm > IntVal34Clear (spc* '-' spc* hhmm)? > IntValSwap % IntValSwap;

    action MonthEn { ParseCfgState_add_cst_mon(ncs); }
    action DayEn { ParseCfgState_add_cst_mday(ncs); }
    action WeekdayEn { ParseCfgState_add_cst_wday(ncs); }
    action TimeEn { ParseCfgState_add_cst_time(ncs); }

    month = 'month'i eqsep intrangeval % MonthEn;
    day = 'day'i eqsep intrangeval % DayEn;
    weekday = 'weekday'i eqsep intrangeval % WeekdayEn;
    time = 'time'i eqsep hhmm_range % TimeEn;

    action CommandEn { ParseCfgState_parse_command_key(ncs); }

    command = 'command'i eqsep stringval % CommandEn;

    cmds = command | time | weekday | day |
           month | interval | maxruns | runat | journal;

    action JobIdSt { ncs->jobid_st = p; }
    action JobIdEn { parse_int_value(p, ncs->jobid_st, ncs->linenum, &ncs->ce->id_); }
    action CreateCe { ParseCfgState_finish_ce(ncs); ParseCfgState_create_ce(ncs); }

    jobid = ('!' > CreateCe) (digit+ > JobIdSt) % JobIdEn;
    comment = (';'|'#') any*;

    main := jobid | cmds | comment;
}%%

%% write data;

static int do_parse_config(struct ParseCfgState *ncs, const char *p, size_t plen)
{
    const char *pe = p + plen;
    const char *eof = pe;

    %% write init;
    %% write exec;

    if (ncs->cs == ncrontab_error)
        return -1;
    if (ncs->cs >= ncrontab_first_final)
        return 1;
    return 0;
}

// Seeks back to start of file when done.
static size_t count_config_jobs(FILE *f)
{
    size_t r = 0;
    int lc = '\n', llc = 0;
    while (!feof(f)) {
        int c = fgetc(f);
        if (!c) {
            if (!feof(f))
                log_line("IO error reading config file\n");
            break;
        }
        if ((c >= '0' && c <= '9') && lc == '!' && llc == '\n') ++r;
        llc = lc;
        lc = c;
    }
    rewind(f);
    return r;
}

void parse_config(char const *path, char const *execfile,
                  struct Job **stk, struct Job **deadstk)
{
    struct ParseCfgState ncs;
    ParseCfgState_init(&ncs, stk, deadstk);

    char buf[MAX_LINE];
    FILE *f = fopen(path, "r");
    if (!f)
        suicide("Failed to open config file '%s': %s\n", path, strerror(errno));
    g_njobs = count_config_jobs(f);
    if (!g_njobs) {
        log_line("No jobs found in config file.  Exiting.\n");
        exit(EXIT_SUCCESS);
    }
    g_jobs = xmalloc(g_njobs * sizeof(struct Job));
    ncs.ce = g_jobs;
    while (!feof(f)) {
        if (!fgets(buf, sizeof buf, f)) {
            if (!feof(f))
                log_line("IO error reading config file '%s'\n", path);
            break;
        }
        size_t llen = strlen(buf);
        if (llen == 0)
            continue;
        if (buf[llen-1] == '\n')
            buf[--llen] = 0;
        ++ncs.linenum;
        if (do_parse_config(&ncs, buf, llen) < 0)
            suicide("Config file '%s' is malformed at line %zu\n", path, ncs.linenum);
    }
    ParseCfgState_finish_ce(&ncs);
    parse_history(execfile);

    for (struct Job *j = g_jobs, *jend = g_jobs + g_njobs; j != jend; ++j) {
        bool alive = !j->runat_?
                     ((j->maxruns_ == 0 || j->numruns_ < j->maxruns_) && j->exectime_ != 0)
                   : (j->numruns_ == 0);
        job_insert(alive ? stk : deadstk, j);
    }
    fclose(f);
}
