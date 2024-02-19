#line 1 "crontab.rl"
// Copyright 2003-2024 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
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
	self->stackl = stk;
	self->deadstackl = dstk;
	self->ce = NULL;
	self->jobid_st = NULL;
	self->time_st = NULL;
	self->intv_st = NULL;
	self->strv_st = NULL;
	self->v_strlen = 0;
	self->linenum = 0;
	self->v_time = 0;
	self->v_int1 = 0;
	self->v_int2 = 0;
	self->v_int3 = -1;
	self->v_int4 = -1;
	self->cs = 0;
	self->have_command = false;
	self->intv2_exist = false;
	self->seen_cst_hhmm = false;
	self->seen_cst_wday = false;
	self->seen_cst_mday = false;
	self->seen_cst_mon = false;
	self->seen_job = false;
	memset(self->v_str, 0, sizeof self->v_str);
}

static void ParseCfgState_create_ce(struct ParseCfgState *self)
{
	if (self->ce == g_jobs + g_njobs) {
		log_line("job count mismatch");
		exit(EXIT_FAILURE);
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
	log_line("id=%d:\tcommand: %s", j->id_, j->command_ ? j->command_ : "");
	log_line("\targs: %s", j->args_ ? j->args_ : "");
	log_line("\tnumruns: %u\n\tmaxruns: %u", j->numruns_, j->maxruns_);
	log_line("\tjournal: %s", j->journal_ ? "true" : "false");
	log_line("\trunat: %s", j->runat_ ? "true" : "false");
	log_line("\tinterval: %u\n\texectime: %lu\n\tlasttime: %lu", j->interval_, j->exectime_, j->lasttime_);
}

static void ParseCfgState_finish_ce(struct ParseCfgState *self)
{
	if (!self->seen_job) return;
		
	ParseCfgState_debug_print_ce(self);
	
	if (self->ce->id_ < 0
		|| (self->ce->interval_ <= 0 && self->ce->exectime_ <= 0)
	|| !self->ce->command_ || !self->have_command) {
		log_line("ERROR IN CRONTAB: invalid id, command, or interval for job %d", self->ce->id_);
		exit(EXIT_FAILURE);
	}
	
	// XXX: O(n^2) might be nice to avoid.
	for (struct Job *i = g_jobs, *iend = self->ce; i != iend; ++i) {
		if (i->id_ == self->ce->id_) {
			log_line("ERROR IN CRONTAB: duplicate entry for job %d", self->ce->id_);
			exit(EXIT_FAILURE);
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
		log_line("id=%d:\tnumruns = %u\n\texectime = %lu\n\tlasttime = %lu",
	self->id, self->h.numruns, self->h.exectime, self->h.lasttime);
}


#line 191 "crontab.rl"



#line 160 "crontab.c"
static const signed char _history_m_actions[] = {
	0, 1, 0, 1, 1, 1, 2, 1,
	3, 1, 4, 0
};

static const char _history_m_trans_keys[] = {
	1, 0, 0, 0, 0, 3, 0, 0,
	0, 1, 0, 0, 0, 4, 0, 0,
	0, 0, 0
};

static const signed char _history_m_char_class[] = {
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 1, 2, 2, 3, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 2, 2, 2, 2,
	2, 2, 2, 2, 4, 0
};

static const signed char _history_m_index_offsets[] = {
	0, 0, 1, 5, 6, 8, 9, 14,
	15, 0
};

static const signed char _history_m_indices[] = {
	2, 3, 0, 0, 4, 6, 7, 8,
	10, 11, 0, 0, 0, 12, 14, 16,
	0
};

static const signed char _history_m_index_defaults[] = {
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0
};

static const signed char _history_m_cond_targs[] = {
	0, 1, 2, 2, 3, 3, 4, 4,
	5, 5, 6, 6, 7, 7, 8, 8,
	8, 0
};

static const signed char _history_m_cond_actions[] = {
	0, 0, 1, 0, 9, 0, 1, 0,
	7, 0, 1, 0, 5, 0, 1, 3,
	0, 0
};

static const signed char _history_m_eof_trans[] = {
	1, 2, 4, 6, 8, 10, 12, 14,
	16, 0
};

static const int history_m_start = 1;
static const int history_m_first_final = 8;
static const int history_m_error = 0;

static const int history_m_en_main = 1;


#line 193 "crontab.rl"


static int do_parse_history(struct hstm *hst, const char *p, size_t plen)
{
	const char *pe = p + plen;
	const char *eof = pe;
	

#line 231 "crontab.c"
	{
		hst->cs = (int)history_m_start;
	}
	
#line 200 "crontab.rl"


#line 236 "crontab.c"
	{
		unsigned int _trans = 0;
		const char * _keys;
		const signed char * _acts;
		const signed char * _inds;
		unsigned int _nacts;
		int _ic;
		_resume: {}
		if ( p == pe && p != eof )
			goto _out;
		if ( p == eof ) {
			if ( _history_m_eof_trans[hst->cs] > 0 ) {
				_trans = (unsigned int)_history_m_eof_trans[hst->cs] - 1;
			}
		}
		else {
			_keys = ( _history_m_trans_keys + ((hst->cs<<1)));
			_inds = ( _history_m_indices + (_history_m_index_offsets[hst->cs]));
			
			if ( ( (*( p))) <= 124 && ( (*( p))) >= 48 ) {
				_ic = (int)_history_m_char_class[(int)( (*( p))) - 48];
				if ( _ic <= (int)(*( _keys+1)) && _ic >= (int)(*( _keys)) )
					_trans = (unsigned int)(*( _inds + (int)( _ic - (int)(*( _keys)) ) )); 
				else
					_trans = (unsigned int)_history_m_index_defaults[hst->cs];
			}
			else {
				_trans = (unsigned int)_history_m_index_defaults[hst->cs];
			}
			
		}
		hst->cs = (int)_history_m_cond_targs[_trans];
		
		if ( _history_m_cond_actions[_trans] != 0 ) {
			
			_acts = ( _history_m_actions + (_history_m_cond_actions[_trans]));
			_nacts = (unsigned int)(*( _acts));
			_acts += 1;
			while ( _nacts > 0 ) {
				switch ( (*( _acts)) )
				{
					case 0:  {
							{
#line 160 "crontab.rl"
							hst->st = p; }
						
#line 282 "crontab.c"

						break; 
					}
					case 1:  {
							{
#line 161 "crontab.rl"
							
							if (!strconv_to_i64(hst->st, p, &hst->h.lasttime)) {
								hst->parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 295 "crontab.c"

						break; 
					}
					case 2:  {
							{
#line 167 "crontab.rl"
							
							if (!strconv_to_u32(hst->st, p, &hst->h.numruns)) {
								hst->parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 308 "crontab.c"

						break; 
					}
					case 3:  {
							{
#line 173 "crontab.rl"
							
							if (!strconv_to_i64(hst->st, p, &hst->h.exectime)) {
								hst->parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 321 "crontab.c"

						break; 
					}
					case 4:  {
							{
#line 179 "crontab.rl"
							
							if (!strconv_to_i32(hst->st, p, &hst->id)) {
								hst->parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 334 "crontab.c"

						break; 
					}
				}
				_nacts -= 1;
				_acts += 1;
			}
			
		}
		
		if ( p == eof ) {
			if ( hst->cs >= 8 )
				goto _out;
		}
		else {
			if ( hst->cs != 0 ) {
				p += 1;
				goto _resume;
			}
		}
		_out: {}
	}
	
#line 201 "crontab.rl"

	
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
		log_line("Failed to open history file '%s' for read: %s", path, strerror(errno));
		return;
	}
	size_t linenum = 0;
	while (!feof(f)) {
		if (!fgets(buf, sizeof buf, f)) {
			if (!feof(f)) log_line("IO error reading history file '%s'", path);
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
			log_line("%s history entry at line %zu; ignoring",
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
						log_line("ERROR IN CRONTAB: interval is unused when runat is set: job %d", j->id_);
						exit(EXIT_FAILURE);
					}
				}
			}
		}
	}
	fclose(f);
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


#line 388 "crontab.rl"



#line 503 "crontab.c"
static const signed char _parse_cmd_key_m_actions[] = {
	0, 1, 0, 1, 1, 1, 2, 2,
	0, 2, 2, 1, 0, 2, 1, 2,
	3, 1, 0, 2, 0
};

static const char _parse_cmd_key_m_trans_keys[] = {
	1, 0, 0, 3, 0, 3, 0, 2,
	0, 0, 0, 3, 0, 3, 0, 3,
	0, 3, 0
};

static const signed char _parse_cmd_key_m_char_class[] = {
	0, 1, 1, 1, 1, 1, 1, 1,
	1, 2, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,
	2, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 3, 0
};

static const signed char _parse_cmd_key_m_index_offsets[] = {
	0, 0, 4, 8, 11, 12, 16, 20,
	24, 0
};

static const signed char _parse_cmd_key_m_indices[] = {
	0, 2, 1, 3, 0, 5, 6, 7,
	0, 9, 10, 0, 0, 5, 14, 7,
	0, 16, 17, 18, 0, 20, 6, 21,
	0, 20, 14, 21, 0
};

static const signed char _parse_cmd_key_m_index_defaults[] = {
	0, 2, 5, 9, 12, 5, 16, 20,
	20, 0
};

static const signed char _parse_cmd_key_m_cond_targs[] = {
	0, 1, 2, 5, 2, 2, 3, 5,
	3, 4, 3, 4, 4, 5, 6, 6,
	7, 3, 8, 7, 7, 8, 8, 0
};

static const signed char _parse_cmd_key_m_cond_actions[] = {
	0, 0, 1, 1, 3, 0, 3, 0,
	7, 1, 1, 5, 0, 3, 3, 16,
	1, 10, 1, 13, 0, 0, 13, 0
};

static const signed char _parse_cmd_key_m_eof_trans[] = {
	1, 2, 5, 9, 12, 14, 16, 20,
	23, 0
};

static const int parse_cmd_key_m_start = 1;
static const int parse_cmd_key_m_first_final = 2;
static const int parse_cmd_key_m_error = 0;

static const int parse_cmd_key_m_en_main = 1;


#line 390 "crontab.rl"


static void ParseCfgState_parse_command_key(struct ParseCfgState *self)
{
	char *p = self->v_str;
	const char *pe = self->v_str + self->v_strlen;
	const char *eof = pe;
	
	struct Pckm pckm = {0};
	
	if (self->have_command) {
		log_line("Duplicate 'command' value at line %zu", self->linenum);
		exit(EXIT_FAILURE);
	}
	

#line 586 "crontab.c"
	{
		pckm.cs = (int)parse_cmd_key_m_start;
	}
	
#line 405 "crontab.rl"


#line 591 "crontab.c"
	{
		unsigned int _trans = 0;
		const char * _keys;
		const signed char * _acts;
		const signed char * _inds;
		unsigned int _nacts;
		int _ic;
		_resume: {}
		if ( p == pe && p != eof )
			goto _out;
		if ( p == eof ) {
			if ( _parse_cmd_key_m_eof_trans[pckm.cs] > 0 ) {
				_trans = (unsigned int)_parse_cmd_key_m_eof_trans[pckm.cs] - 1;
			}
		}
		else {
			_keys = ( _parse_cmd_key_m_trans_keys + ((pckm.cs<<1)));
			_inds = ( _parse_cmd_key_m_indices + (_parse_cmd_key_m_index_offsets[pckm.cs]));
			
			if ( ( (*( p))) <= 92 && ( (*( p))) >= 0 ) {
				_ic = (int)_parse_cmd_key_m_char_class[(int)( (*( p))) - 0];
				if ( _ic <= (int)(*( _keys+1)) && _ic >= (int)(*( _keys)) )
					_trans = (unsigned int)(*( _inds + (int)( _ic - (int)(*( _keys)) ) )); 
				else
					_trans = (unsigned int)_parse_cmd_key_m_index_defaults[pckm.cs];
			}
			else {
				_trans = (unsigned int)_parse_cmd_key_m_index_defaults[pckm.cs];
			}
			
		}
		pckm.cs = (int)_parse_cmd_key_m_cond_targs[_trans];
		
		if ( _parse_cmd_key_m_cond_actions[_trans] != 0 ) {
			
			_acts = ( _parse_cmd_key_m_actions + (_parse_cmd_key_m_cond_actions[_trans]));
			_nacts = (unsigned int)(*( _acts));
			_acts += 1;
			while ( _nacts > 0 ) {
				switch ( (*( _acts)) )
				{
					case 0:  {
							{
#line 348 "crontab.rl"
							pckm.st = p; }
						
#line 637 "crontab.c"

						break; 
					}
					case 1:  {
							{
#line 349 "crontab.rl"
							
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
						
#line 668 "crontab.c"

						break; 
					}
					case 2:  {
							{
#line 373 "crontab.rl"
							
							size_t l = p > pckm.st ? (size_t)(p - pckm.st) : 0;
							if (l) {
								char *ts = xmalloc(l + 1);
								memcpy(ts, pckm.st, l);
								ts[l] = 0;
								self->ce->args_ = ts;
							}
						}
						
#line 684 "crontab.c"

						break; 
					}
				}
				_nacts -= 1;
				_acts += 1;
			}
			
		}
		
		if ( p == eof ) {
			if ( pckm.cs >= 2 )
				goto _out;
		}
		else {
			if ( pckm.cs != 0 ) {
				p += 1;
				goto _resume;
			}
		}
		_out: {}
	}
	
#line 406 "crontab.rl"

	
	if (pckm.cs == parse_cmd_key_m_error) {
		log_line("Malformed 'command' value at line %zu", self->linenum);
		exit(EXIT_FAILURE);
	} else if (pckm.cs >= parse_cmd_key_m_first_final) {
		self->have_command = true;
	} else {
		log_line("Incomplete 'command' value at line %zu", self->linenum);
		exit(EXIT_FAILURE);
	}
}

static void ParseCfgState_parse_time_unit(const struct ParseCfgState *self, const char *p, unsigned unit, unsigned *dest)
{
	unsigned t;
	if (!strconv_to_u32(self->time_st, p - 1, &t)) {
		log_line("Invalid time unit at line %zu", self->linenum);
		exit(EXIT_FAILURE);
	}
	*dest += unit * t;
}

static void parse_int_value(const char *p, const char *start, size_t linenum, int *dest)
{
	if (!strconv_to_i32(start, p, dest)) {
		log_line("Invalid integer value at line %zu", linenum);
		exit(EXIT_FAILURE);
	}
}

static void swap_int_pair(int *a, int *b) { int t = *a; *a = *b; *b = t; }


#line 545 "crontab.rl"



#line 743 "crontab.c"
static const signed char _ncrontab_actions[] = {
	0, 1, 0, 1, 1, 1, 2, 1,
	3, 1, 4, 1, 5, 1, 6, 1,
	7, 1, 8, 1, 12, 1, 14, 1,
	23, 1, 24, 1, 25, 2, 1, 0,
	2, 1, 17, 2, 2, 0, 2, 2,
	17, 2, 3, 0, 2, 3, 17, 2,
	4, 0, 2, 4, 17, 2, 5, 0,
	2, 5, 17, 2, 7, 15, 2, 7,
	16, 2, 7, 18, 2, 7, 19, 2,
	7, 20, 2, 9, 10, 2, 9, 18,
	2, 9, 19, 2, 9, 20, 2, 11,
	6, 2, 13, 22, 3, 9, 10, 21,
	0
};

static const char _ncrontab_trans_keys[] = {
	1, 0, 3, 37, 6, 10, 24, 24,
	22, 35, 22, 35, 13, 13, 23, 23,
	15, 33, 2, 12, 0, 2, 13, 13,
	32, 32, 2, 12, 2, 10, 6, 10,
	23, 23, 27, 27, 16, 16, 25, 25,
	29, 29, 13, 13, 21, 21, 2, 12,
	2, 10, 6, 37, 24, 24, 28, 28,
	25, 25, 23, 23, 13, 13, 21, 21,
	13, 24, 31, 31, 25, 25, 28, 28,
	23, 23, 26, 36, 2, 12, 2, 10,
	23, 23, 27, 27, 17, 34, 2, 12,
	2, 10, 6, 10, 28, 28, 23, 23,
	13, 13, 27, 27, 2, 12, 2, 10,
	18, 18, 22, 35, 16, 16, 2, 12,
	2, 10, 6, 11, 11, 11, 6, 9,
	6, 10, 2, 5, 2, 10, 6, 11,
	11, 11, 6, 9, 6, 10, 6, 11,
	6, 11, 16, 16, 16, 16, 20, 20,
	15, 33, 13, 13, 32, 32, 2, 12,
	2, 10, 6, 10, 6, 10, 1, 0,
	0, 0, 0, 2, 5, 10, 6, 10,
	2, 10, 2, 10, 2, 10, 2, 10,
	2, 10, 1, 0, 6, 10, 5, 10,
	6, 10, 6, 10, 2, 5, 1, 0,
	5, 10, 6, 10, 0
};

static const signed char _ncrontab_char_class[] = {
	0, 1, 1, 1, 1, 1, 1, 1,
	1, 2, 0, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 1,
	2, 3, 1, 4, 1, 1, 1, 1,
	1, 1, 1, 1, 1, 5, 1, 1,
	6, 6, 7, 8, 9, 9, 10, 10,
	10, 10, 11, 4, 1, 12, 1, 1,
	1, 13, 1, 14, 15, 16, 1, 1,
	17, 18, 19, 20, 21, 22, 23, 24,
	1, 1, 25, 26, 27, 28, 29, 30,
	31, 32, 1, 1, 1, 1, 1, 1,
	1, 13, 1, 14, 33, 16, 1, 1,
	34, 18, 19, 20, 21, 35, 23, 24,
	1, 1, 25, 36, 27, 28, 29, 37,
	31, 32, 0
};

static const short _ncrontab_index_offsets[] = {
	0, 0, 35, 40, 41, 55, 69, 70,
	71, 90, 101, 104, 105, 106, 117, 126,
	131, 132, 133, 134, 135, 136, 137, 138,
	149, 158, 190, 191, 192, 193, 194, 195,
	196, 208, 209, 210, 211, 212, 223, 234,
	243, 244, 245, 263, 274, 283, 288, 289,
	290, 291, 292, 303, 312, 313, 327, 328,
	339, 348, 354, 355, 359, 364, 368, 377,
	383, 384, 388, 393, 399, 405, 406, 407,
	408, 427, 428, 429, 440, 449, 454, 459,
	459, 460, 463, 469, 474, 483, 492, 501,
	510, 519, 519, 524, 530, 535, 540, 544,
	544, 550, 0
};

static const short _ncrontab_indices[] = {
	2, 3, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 4, 5, 0, 0, 6,
	7, 0, 0, 8, 0, 0, 9, 0,
	10, 0, 0, 11, 0, 0, 5, 0,
	8, 0, 11, 13, 13, 13, 13, 13,
	14, 15, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 15, 16,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 16, 17, 18, 19,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 19, 19, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 20, 0, 21, 22,
	23, 24, 24, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 25, 25, 0, 0,
	0, 26, 26, 26, 26, 26, 28, 28,
	28, 28, 28, 29, 30, 31, 32, 33,
	34, 35, 35, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 36, 36, 0, 0,
	0, 37, 37, 37, 37, 37, 38, 38,
	38, 38, 38, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 39, 40, 41, 42, 43, 44, 45,
	46, 47, 48, 49, 50, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 51,
	52, 53, 54, 55, 56, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 56, 56,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 57, 57, 0, 0, 0, 58, 58,
	58, 58, 58, 59, 60, 61, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 61, 61,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 62, 62, 0, 0, 0, 63, 63,
	63, 63, 63, 65, 65, 65, 65, 65,
	66, 67, 68, 69, 69, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 70, 70,
	0, 0, 0, 71, 71, 71, 71, 71,
	72, 73, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 73, 74,
	74, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 75, 75, 0, 0, 0, 76,
	77, 78, 78, 78, 80, 80, 80, 80,
	80, 81, 81, 83, 83, 83, 83, 85,
	85, 85, 85, 85, 86, 0, 0, 87,
	87, 0, 0, 0, 88, 89, 90, 90,
	90, 92, 92, 92, 92, 92, 93, 93,
	95, 95, 95, 95, 97, 97, 97, 97,
	97, 92, 92, 92, 0, 0, 93, 80,
	80, 80, 0, 0, 81, 100, 101, 102,
	103, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 103, 104, 105, 105, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 106,
	106, 0, 0, 0, 107, 107, 107, 107,
	107, 109, 109, 109, 109, 109, 111, 111,
	111, 111, 111, 0, 0, 21, 22, 116,
	117, 117, 117, 117, 117, 119, 119, 119,
	119, 119, 121, 0, 0, 0, 122, 122,
	122, 122, 122, 124, 0, 0, 0, 125,
	125, 125, 125, 125, 127, 0, 0, 0,
	128, 128, 128, 128, 128, 130, 0, 0,
	0, 131, 131, 131, 131, 131, 133, 0,
	0, 0, 134, 134, 134, 134, 134, 137,
	137, 137, 137, 137, 139, 140, 140, 140,
	140, 140, 142, 142, 142, 142, 142, 144,
	144, 144, 144, 144, 146, 0, 0, 147,
	150, 151, 151, 151, 151, 151, 153, 153,
	153, 153, 153, 0
};

static const signed char _ncrontab_index_defaults[] = {
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 21, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 3,
	113, 21, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0
};

static const signed char _ncrontab_cond_targs[] = {
	0, 1, 2, 79, 3, 11, 16, 26,
	32, 46, 52, 69, 2, 78, 4, 5,
	6, 7, 8, 9, 10, 80, 81, 12,
	13, 14, 82, 15, 83, 17, 18, 19,
	20, 21, 22, 23, 24, 25, 25, 84,
	85, 86, 87, 88, 27, 28, 29, 30,
	31, 89, 33, 40, 34, 35, 36, 37,
	38, 39, 90, 41, 42, 43, 44, 91,
	45, 92, 47, 48, 49, 50, 51, 93,
	53, 54, 55, 56, 57, 68, 58, 57,
	58, 59, 59, 60, 60, 94, 61, 62,
	63, 67, 64, 63, 64, 65, 65, 66,
	66, 95, 67, 68, 70, 71, 72, 73,
	74, 75, 76, 96, 77, 97, 78, 78,
	80, 80, 81, 82, 15, 82, 83, 83,
	84, 24, 25, 85, 24, 25, 86, 24,
	25, 87, 24, 25, 88, 24, 25, 89,
	90, 90, 91, 45, 91, 92, 92, 93,
	93, 94, 61, 62, 95, 96, 77, 96,
	97, 97, 0
};

static const signed char _ncrontab_cond_actions[] = {
	0, 0, 27, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 23, 0, 0,
	0, 0, 0, 0, 0, 19, 19, 0,
	0, 0, 13, 0, 17, 0, 0, 0,
	0, 0, 0, 0, 0, 1, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 13, 0, 0, 0, 0, 13,
	0, 17, 0, 0, 0, 0, 0, 13,
	0, 0, 0, 0, 86, 86, 86, 0,
	0, 15, 0, 17, 0, 0, 0, 0,
	13, 13, 13, 0, 0, 15, 0, 17,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 13, 0, 17, 25, 0,
	89, 0, 89, 68, 15, 0, 80, 0,
	50, 9, 47, 44, 7, 41, 38, 5,
	35, 32, 3, 29, 56, 11, 53, 21,
	62, 0, 65, 15, 0, 77, 0, 59,
	0, 92, 74, 74, 92, 71, 15, 0,
	83, 0, 0
};

static const short _ncrontab_eof_trans[] = {
	1, 2, 13, 5, 15, 16, 17, 18,
	19, 20, 21, 6, 24, 25, 26, 28,
	7, 30, 31, 32, 33, 34, 35, 36,
	37, 39, 8, 45, 46, 47, 48, 49,
	9, 51, 53, 54, 55, 56, 57, 58,
	52, 60, 61, 62, 63, 65, 10, 67,
	68, 69, 70, 71, 11, 73, 74, 75,
	76, 80, 81, 83, 85, 87, 88, 92,
	93, 95, 97, 99, 100, 12, 101, 102,
	103, 104, 105, 106, 107, 109, 111, 4,
	113, 115, 116, 119, 121, 124, 127, 130,
	133, 136, 137, 139, 142, 144, 146, 149,
	150, 153, 0
};

static const int ncrontab_start = 1;
static const int ncrontab_first_final = 78;
static const int ncrontab_error = 0;

static const int ncrontab_en_main = 1;


#line 547 "crontab.rl"


static int do_parse_config(struct ParseCfgState *ncs, const char *p, size_t plen)
{
	const char *pe = p + plen;
	const char *eof = pe;
	

#line 987 "crontab.c"
	{
		ncs->cs = (int)ncrontab_start;
	}
	
#line 554 "crontab.rl"


#line 992 "crontab.c"
	{
		unsigned int _trans = 0;
		const char * _keys;
		const signed char * _acts;
		const short * _inds;
		unsigned int _nacts;
		int _ic;
		_resume: {}
		if ( p == pe && p != eof )
			goto _out;
		if ( p == eof ) {
			if ( _ncrontab_eof_trans[ncs->cs] > 0 ) {
				_trans = (unsigned int)_ncrontab_eof_trans[ncs->cs] - 1;
			}
		}
		else {
			_keys = ( _ncrontab_trans_keys + ((ncs->cs<<1)));
			_inds = ( _ncrontab_indices + (_ncrontab_index_offsets[ncs->cs]));
			
			if ( ( (*( p))) <= 121 && ( (*( p))) >= 0 ) {
				_ic = (int)_ncrontab_char_class[(int)( (*( p))) - 0];
				if ( _ic <= (int)(*( _keys+1)) && _ic >= (int)(*( _keys)) )
					_trans = (unsigned int)(*( _inds + (int)( _ic - (int)(*( _keys)) ) )); 
				else
					_trans = (unsigned int)_ncrontab_index_defaults[ncs->cs];
			}
			else {
				_trans = (unsigned int)_ncrontab_index_defaults[ncs->cs];
			}
			
		}
		ncs->cs = (int)_ncrontab_cond_targs[_trans];
		
		if ( _ncrontab_cond_actions[_trans] != 0 ) {
			
			_acts = ( _ncrontab_actions + (_ncrontab_cond_actions[_trans]));
			_nacts = (unsigned int)(*( _acts));
			_acts += 1;
			while ( _nacts > 0 ) {
				switch ( (*( _acts)) )
				{
					case 0:  {
							{
#line 446 "crontab.rl"
							ncs->time_st = p; ncs->v_time = 0; }
						
#line 1038 "crontab.c"

						break; 
					}
					case 1:  {
							{
#line 447 "crontab.rl"
							ParseCfgState_parse_time_unit(ncs, p, 1, &ncs->v_time); }
						
#line 1046 "crontab.c"

						break; 
					}
					case 2:  {
							{
#line 448 "crontab.rl"
							ParseCfgState_parse_time_unit(ncs, p, 60, &ncs->v_time); }
						
#line 1054 "crontab.c"

						break; 
					}
					case 3:  {
							{
#line 449 "crontab.rl"
							ParseCfgState_parse_time_unit(ncs, p, 3600, &ncs->v_time); }
						
#line 1062 "crontab.c"

						break; 
					}
					case 4:  {
							{
#line 450 "crontab.rl"
							ParseCfgState_parse_time_unit(ncs, p, 86400, &ncs->v_time); }
						
#line 1070 "crontab.c"

						break; 
					}
					case 5:  {
							{
#line 451 "crontab.rl"
							ParseCfgState_parse_time_unit(ncs, p, 604800, &ncs->v_time); }
						
#line 1078 "crontab.c"

						break; 
					}
					case 6:  {
							{
#line 453 "crontab.rl"
							
							ncs->intv_st = p;
							ncs->v_int1 = ncs->v_int2 = 0;
							ncs->intv2_exist = false;
						}
						
#line 1090 "crontab.c"

						break; 
					}
					case 7:  {
							{
#line 458 "crontab.rl"
							parse_int_value(p, ncs->intv_st, ncs->linenum, &ncs->v_int1); }
						
#line 1098 "crontab.c"

						break; 
					}
					case 8:  {
							{
#line 459 "crontab.rl"
							ncs->intv2_st = p; }
						
#line 1106 "crontab.c"

						break; 
					}
					case 9:  {
							{
#line 460 "crontab.rl"
							parse_int_value(p, ncs->intv2_st, ncs->linenum, &ncs->v_int2); ncs->intv2_exist = true; }
						
#line 1114 "crontab.c"

						break; 
					}
					case 10:  {
							{
#line 461 "crontab.rl"
							
							swap_int_pair(&ncs->v_int1, &ncs->v_int3);
							swap_int_pair(&ncs->v_int2, &ncs->v_int4);
						}
						
#line 1125 "crontab.c"

						break; 
					}
					case 11:  {
							{
#line 465 "crontab.rl"
							
							ncs->v_int3 = -1;
							ncs->v_int4 = -1;
						}
						
#line 1136 "crontab.c"

						break; 
					}
					case 12:  {
							{
#line 470 "crontab.rl"
							ncs->strv_st = p; ncs->v_strlen = 0; }
						
#line 1144 "crontab.c"

						break; 
					}
					case 13:  {
							{
#line 471 "crontab.rl"
							
							ncs->v_strlen = p > ncs->strv_st ? (size_t)(p - ncs->strv_st) : 0;
							if (ncs->v_strlen >= sizeof ncs->v_str) {
								log_line("error parsing line %zu in crontab: too long", ncs->linenum);
								exit(EXIT_FAILURE);
							}
							memcpy(ncs->v_str, ncs->strv_st, ncs->v_strlen);
							ncs->v_str[ncs->v_strlen] = 0;
						}
						
#line 1160 "crontab.c"

						break; 
					}
					case 14:  {
							{
#line 494 "crontab.rl"
							ncs->ce->journal_ = true; }
						
#line 1168 "crontab.c"

						break; 
					}
					case 15:  {
							{
#line 497 "crontab.rl"
							
							ncs->ce->runat_ = true;
							ncs->ce->exectime_ = ncs->v_int1;
							ncs->ce->maxruns_ = 1;
							ncs->ce->journal_ = true;
						}
						
#line 1181 "crontab.c"

						break; 
					}
					case 16:  {
							{
#line 503 "crontab.rl"
							
							if (!ncs->ce->runat_)
							ncs->ce->maxruns_ = ncs->v_int1 > 0 ? (unsigned)ncs->v_int1 : 0;
						}
						
#line 1192 "crontab.c"

						break; 
					}
					case 17:  {
							{
#line 511 "crontab.rl"
							ncs->ce->interval_ = ncs->v_time; }
						
#line 1200 "crontab.c"

						break; 
					}
					case 18:  {
							{
#line 520 "crontab.rl"
							ParseCfgState_add_cst_mon(ncs); }
						
#line 1208 "crontab.c"

						break; 
					}
					case 19:  {
							{
#line 521 "crontab.rl"
							ParseCfgState_add_cst_mday(ncs); }
						
#line 1216 "crontab.c"

						break; 
					}
					case 20:  {
							{
#line 522 "crontab.rl"
							ParseCfgState_add_cst_wday(ncs); }
						
#line 1224 "crontab.c"

						break; 
					}
					case 21:  {
							{
#line 523 "crontab.rl"
							ParseCfgState_add_cst_time(ncs); }
						
#line 1232 "crontab.c"

						break; 
					}
					case 22:  {
							{
#line 530 "crontab.rl"
							ParseCfgState_parse_command_key(ncs); }
						
#line 1240 "crontab.c"

						break; 
					}
					case 23:  {
							{
#line 537 "crontab.rl"
							ncs->jobid_st = p; }
						
#line 1248 "crontab.c"

						break; 
					}
					case 24:  {
							{
#line 538 "crontab.rl"
							parse_int_value(p, ncs->jobid_st, ncs->linenum, &ncs->ce->id_); }
						
#line 1256 "crontab.c"

						break; 
					}
					case 25:  {
							{
#line 539 "crontab.rl"
							ParseCfgState_finish_ce(ncs); ParseCfgState_create_ce(ncs); }
						
#line 1264 "crontab.c"

						break; 
					}
				}
				_nacts -= 1;
				_acts += 1;
			}
			
		}
		
		if ( p == eof ) {
			if ( ncs->cs >= 78 )
				goto _out;
		}
		else {
			if ( ncs->cs != 0 ) {
				p += 1;
				goto _resume;
			}
		}
		_out: {}
	}
	
#line 555 "crontab.rl"

	
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
				log_line("IO error reading config file");
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
	if (!f) {
		log_line("Failed to open config file '%s': %s", path, strerror(errno));
		exit(EXIT_FAILURE);
	}
	g_njobs = count_config_jobs(f);
	if (!g_njobs) {
		log_line("No jobs found in config file.  Exiting.");
		exit(EXIT_SUCCESS);
	}
	g_jobs = xmalloc(g_njobs * sizeof(struct Job));
	ncs.ce = g_jobs;
	while (!feof(f)) {
		if (!fgets(buf, sizeof buf, f)) {
			if (!feof(f))
				log_line("IO error reading config file '%s'", path);
			break;
		}
		size_t llen = strlen(buf);
		if (llen == 0)
			continue;
		if (buf[llen-1] == '\n')
			buf[--llen] = 0;
		++ncs.linenum;
		if (do_parse_config(&ncs, buf, llen) < 0) {
			log_line("Config file '%s' is malformed at line %zu", path, ncs.linenum);
			exit(EXIT_FAILURE);
		}
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
