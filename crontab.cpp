#line 1 "crontab.rl"
// Copyright 2003-2024 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <memory>
extern "C" {
#include "nk/log.h"
#include "xmalloc.h"
#include "strconv.h"
}
#include "ncron.hpp"
#include "sched.hpp"

#define MAX_LINE 2048

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif

extern int gflags_debug;
extern size_t g_njobs;
extern Job *g_jobs;

struct item_history {
	time_t exectime = 0;
	time_t lasttime = 0;
	unsigned int numruns = 0;
};

struct ParseCfgState
{
	ParseCfgState(Job **stk, Job **dstk)
	: stackl(stk), deadstackl(dstk)
	{
		memset(v_str, 0, sizeof v_str);
	}
	char v_str[MAX_LINE];
	
	Job **stackl = nullptr;
	Job **deadstackl = nullptr;
	
	Job *ce = nullptr;
	
	const char *jobid_st = nullptr;
	const char *time_st = nullptr;
	const char *intv_st = nullptr;
	const char *intv2_st = nullptr;
	const char *strv_st = nullptr;
	
	size_t v_strlen = 0;
	size_t linenum = 0;
	
	unsigned int v_time;
	
	int v_int1 = 0;
	int v_int2 = 0;
	int v_int3 = -1;
	int v_int4 = -1;
	
	int cs = 0;
	bool have_command = false;
	
	bool intv2_exist = false;
	
	bool seen_cst_hhmm = false;
	bool seen_cst_wday = false;
	bool seen_cst_mday = false;
	bool seen_cst_mon = false;
	
	bool seen_job = false;
	
	void create_ce()
	{
		if (ce == g_jobs + g_njobs) {
			log_line("job count mismatch");
			exit(EXIT_FAILURE);
		}
		job_init(ce);
		seen_job = true;
		have_command = false;
		seen_cst_hhmm = false;
		seen_cst_wday = false;
		seen_cst_mday = false;
		seen_cst_mon = false;
	}
	
	inline void debug_print_ce() const
	{
		if (!gflags_debug) return;
			log_line("id=%d:\tcommand: %s", ce->id_, ce->command_ ? ce->command_ : "");
		log_line("\targs: %s", ce->args_ ? ce->args_ : "");
		log_line("\tnumruns: %u\n\tmaxruns: %u", ce->numruns_, ce->maxruns_);
		log_line("\tjournal: %s", ce->journal_ ? "true" : "false");
		log_line("\trunat: %s", ce->runat_ ? "true" : "false");
		log_line("\tinterval: %u\n\texectime: %lu\n\tlasttime: %lu", ce->interval_, ce->exectime_, ce->lasttime_);
	}
	
	void finish_ce()
	{
		if (!seen_job) return;
			
		debug_print_ce();
		
		if (ce->id_ < 0
			|| (ce->interval_ <= 0 && ce->exectime_ <= 0)
		|| !ce->command_ || !have_command) {
			log_line("ERROR IN CRONTAB: invalid id, command, or interval for job %d", ce->id_);
			exit(EXIT_FAILURE);
		}
		
		// XXX: O(n^2) might be nice to avoid.
		for (Job *i = g_jobs, *iend = ce; i != iend; ++i) {
			if (i->id_ == ce->id_) {
				log_line("ERROR IN CRONTAB: duplicate entry for job %d", ce->id_);
				exit(EXIT_FAILURE);
			}
		}
		
		// Preserve this job and work on the next one.
		++ce;
	}
};

struct hstm {
	const char *st = nullptr;
	int cs = 0;
	int id = -1;
	item_history h;
	bool parse_error = false;
	
	void print() const
	{
		if (!gflags_debug) return;
			log_line("id=%d:\tnumruns = %u\n\texectime = %lu\n\tlasttime = %lu",
		id, h.numruns, h.exectime, h.lasttime);
	}
};


#line 178 "crontab.rl"



#line 147 "crontab.cpp"
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


#line 180 "crontab.rl"


static int do_parse_history(hstm &hst, const char *p, size_t plen)
{
	const char *pe = p + plen;
	const char *eof = pe;
	

#line 218 "crontab.cpp"
	{
		hst.cs = (int)history_m_start;
	}
	
#line 187 "crontab.rl"


#line 223 "crontab.cpp"
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
			if ( _history_m_eof_trans[hst.cs] > 0 ) {
				_trans = (unsigned int)_history_m_eof_trans[hst.cs] - 1;
			}
		}
		else {
			_keys = ( _history_m_trans_keys + ((hst.cs<<1)));
			_inds = ( _history_m_indices + (_history_m_index_offsets[hst.cs]));
			
			if ( ( (*( p))) <= 124 && ( (*( p))) >= 48 ) {
				_ic = (int)_history_m_char_class[(int)( (*( p))) - 48];
				if ( _ic <= (int)(*( _keys+1)) && _ic >= (int)(*( _keys)) )
					_trans = (unsigned int)(*( _inds + (int)( _ic - (int)(*( _keys)) ) )); 
				else
					_trans = (unsigned int)_history_m_index_defaults[hst.cs];
			}
			else {
				_trans = (unsigned int)_history_m_index_defaults[hst.cs];
			}
			
		}
		hst.cs = (int)_history_m_cond_targs[_trans];
		
		if ( _history_m_cond_actions[_trans] != 0 ) {
			
			_acts = ( _history_m_actions + (_history_m_cond_actions[_trans]));
			_nacts = (unsigned int)(*( _acts));
			_acts += 1;
			while ( _nacts > 0 ) {
				switch ( (*( _acts)) )
				{
					case 0:  {
							{
#line 147 "crontab.rl"
							hst.st = p; }
						
#line 269 "crontab.cpp"

						break; 
					}
					case 1:  {
							{
#line 148 "crontab.rl"
							
							if (!strconv_to_i64(hst.st, p, &hst.h.lasttime)) {
								hst.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 282 "crontab.cpp"

						break; 
					}
					case 2:  {
							{
#line 154 "crontab.rl"
							
							if (!strconv_to_u32(hst.st, p, &hst.h.numruns)) {
								hst.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 295 "crontab.cpp"

						break; 
					}
					case 3:  {
							{
#line 160 "crontab.rl"
							
							if (!strconv_to_i64(hst.st, p, &hst.h.exectime)) {
								hst.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 308 "crontab.cpp"

						break; 
					}
					case 4:  {
							{
#line 166 "crontab.rl"
							
							if (!strconv_to_i32(hst.st, p, &hst.id)) {
								hst.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 321 "crontab.cpp"

						break; 
					}
				}
				_nacts -= 1;
				_acts += 1;
			}
			
		}
		
		if ( p == eof ) {
			if ( hst.cs >= 8 )
				goto _out;
		}
		else {
			if ( hst.cs != 0 ) {
				p += 1;
				goto _resume;
			}
		}
		_out: {}
	}
	
#line 188 "crontab.rl"

	
	if (hst.parse_error) return -1;
		if (hst.cs >= history_m_first_final)
		return 1;
	if (hst.cs == history_m_error)
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
		hstm hst;
		int r = do_parse_history(hst, buf, llen);
		if (r < 0) {
			log_line("%s history entry at line %zu; ignoring",
			r == -2 ? "Incomplete" : "Malformed", linenum);
			continue;
		}
		
		for (Job *j = g_jobs, *jend = g_jobs + g_njobs; j != jend; ++j) {
			if (j->id_ == hst.id) {
				hst.print();
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

static bool add_cst_mon(ParseCfgState &ncs)
{
	int min = ncs.v_int1;
	int max = ncs.intv2_exist ? ncs.v_int2 : -1;
	if (max < 0) max = min;
		assert(min > 0 && min <= 12);
	assert(max > 0 && max <= 12);
	if (max < min) return false;
		if (min <= 0 || min > 12) return false;
		if (max <= 0 || max > 12) return false;
		if (!ncs.seen_cst_mon) {
		memset(&ncs.ce->cst_mon_, 0, sizeof ncs.ce->cst_mon_);
		ncs.seen_cst_mon = true;
	}
	for (int i = min; i <= max; ++i)
	ncs.ce->cst_mon_[i - 1] = true;
	return true;
}

static bool add_cst_mday(ParseCfgState &ncs)
{
	int min = ncs.v_int1;
	int max = ncs.intv2_exist ? ncs.v_int2 : -1;
	if (max < 0) max = min;
		assert(min > 0 && min <= 31);
	assert(max > 0 && max <= 31);
	if (max < min) return false;
		if (min <= 0 || min > 31) return false;
		if (max <= 0 || max > 31) return false;
		if (!ncs.seen_cst_mday) {
		memset(&ncs.ce->cst_mday_, 0, sizeof ncs.ce->cst_mday_);
		ncs.seen_cst_mday = true;
	}
	for (int i = min; i <= max; ++i)
	ncs.ce->cst_mday_[i - 1] = true;
	return true;
}

static bool add_cst_wday(ParseCfgState &ncs)
{
	int min = ncs.v_int1;
	int max = ncs.intv2_exist ? ncs.v_int2 : -1;
	if (max < 0) max = min;
		assert(min > 0 && min <= 7);
	assert(max > 0 && max <= 7);
	if (max < min) return false;
		if (min <= 0 || min > 7) return false;
		if (max <= 0 || max > 7) return false;
		if (!ncs.seen_cst_wday) {
		memset(&ncs.ce->cst_wday_, 0, sizeof ncs.ce->cst_wday_);
		ncs.seen_cst_wday = true;
	}
	for (int i = min; i <= max; ++i)
	ncs.ce->cst_wday_[i - 1] = true;
	return true;
}

static bool add_cst_time(ParseCfgState &ncs)
{
	bool single_value = ncs.v_int3 == -1 && ncs.v_int4 == -1;
	// Enforce that range is low-high.
	if (!single_value) {
		if (ncs.v_int3 < ncs.v_int1) return false;
			if (ncs.v_int3 == ncs.v_int1) {
			if (ncs.v_int4 < ncs.v_int2) return false;
			}
	}
	if (!ncs.seen_cst_hhmm) {
		memset(&ncs.ce->cst_hhmm_, 0, sizeof ncs.ce->cst_hhmm_);
		ncs.seen_cst_hhmm = true;
	}
	int min = ncs.v_int1 * 60 + ncs.v_int2;
	int max = ncs.v_int3 * 60 + ncs.v_int4;
	assert(min >= 0 && min < 1440);
	assert(max >= 0 && max < 1440);
	for (int i = min; i <= max; ++i)
	ncs.ce->cst_hhmm_[i] = true;
	return true;
}

struct Pckm {
	Pckm() {}
	char *st = nullptr;
	int cs = 0;
};


#line 376 "crontab.rl"



#line 491 "crontab.cpp"
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


#line 378 "crontab.rl"


static void parse_command_key(ParseCfgState &ncs)
{
	char *p = ncs.v_str;
	const char *pe = ncs.v_str + ncs.v_strlen;
	const char *eof = pe;
	
	Pckm pckm;
	
	if (ncs.have_command) {
		log_line("Duplicate 'command' value at line %zu", ncs.linenum);
		exit(EXIT_FAILURE);
	}
	

#line 574 "crontab.cpp"
	{
		pckm.cs = (int)parse_cmd_key_m_start;
	}
	
#line 393 "crontab.rl"


#line 579 "crontab.cpp"
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
#line 336 "crontab.rl"
							pckm.st = p; }
						
#line 625 "crontab.cpp"

						break; 
					}
					case 1:  {
							{
#line 337 "crontab.rl"
							
							size_t l = p > pckm.st ? static_cast<size_t>(p - pckm.st) : 0;
							if (l) {
								char *ts = static_cast<char *>(xmalloc(l + 1));
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
								ncs.ce->command_ = ts;
							}
						}
						
#line 656 "crontab.cpp"

						break; 
					}
					case 2:  {
							{
#line 361 "crontab.rl"
							
							size_t l = p > pckm.st ? static_cast<size_t>(p - pckm.st) : 0;
							if (l) {
								char *ts = static_cast<char *>(xmalloc(l + 1));
								memcpy(ts, pckm.st, l);
								ts[l] = 0;
								ncs.ce->args_ = ts;
							}
						}
						
#line 672 "crontab.cpp"

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
	
#line 394 "crontab.rl"

	
	if (pckm.cs == parse_cmd_key_m_error) {
		log_line("Malformed 'command' value at line %zu", ncs.linenum);
		exit(EXIT_FAILURE);
	} else if (pckm.cs >= parse_cmd_key_m_first_final) {
		ncs.have_command = true;
	} else {
		log_line("Incomplete 'command' value at line %zu", ncs.linenum);
		exit(EXIT_FAILURE);
	}
}

static void parse_time_unit(const ParseCfgState &ncs, const char *p, unsigned unit, unsigned *dest)
{
	unsigned t;
	if (!strconv_to_u32(ncs.time_st, p - 1, &t)) {
		log_line("Invalid time unit at line %zu", ncs.linenum);
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


#line 532 "crontab.rl"



#line 729 "crontab.cpp"
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


#line 534 "crontab.rl"


static int do_parse_config(ParseCfgState &ncs, const char *p, size_t plen)
{
	const char *pe = p + plen;
	const char *eof = pe;
	

#line 973 "crontab.cpp"
	{
		ncs.cs = (int)ncrontab_start;
	}
	
#line 541 "crontab.rl"


#line 978 "crontab.cpp"
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
			if ( _ncrontab_eof_trans[ncs.cs] > 0 ) {
				_trans = (unsigned int)_ncrontab_eof_trans[ncs.cs] - 1;
			}
		}
		else {
			_keys = ( _ncrontab_trans_keys + ((ncs.cs<<1)));
			_inds = ( _ncrontab_indices + (_ncrontab_index_offsets[ncs.cs]));
			
			if ( ( (*( p))) <= 121 && ( (*( p))) >= 0 ) {
				_ic = (int)_ncrontab_char_class[(int)( (*( p))) - 0];
				if ( _ic <= (int)(*( _keys+1)) && _ic >= (int)(*( _keys)) )
					_trans = (unsigned int)(*( _inds + (int)( _ic - (int)(*( _keys)) ) )); 
				else
					_trans = (unsigned int)_ncrontab_index_defaults[ncs.cs];
			}
			else {
				_trans = (unsigned int)_ncrontab_index_defaults[ncs.cs];
			}
			
		}
		ncs.cs = (int)_ncrontab_cond_targs[_trans];
		
		if ( _ncrontab_cond_actions[_trans] != 0 ) {
			
			_acts = ( _ncrontab_actions + (_ncrontab_cond_actions[_trans]));
			_nacts = (unsigned int)(*( _acts));
			_acts += 1;
			while ( _nacts > 0 ) {
				switch ( (*( _acts)) )
				{
					case 0:  {
							{
#line 432 "crontab.rl"
							ncs.time_st = p; ncs.v_time = 0; }
						
#line 1024 "crontab.cpp"

						break; 
					}
					case 1:  {
							{
#line 433 "crontab.rl"
							parse_time_unit(ncs, p, 1, &ncs.v_time); }
						
#line 1032 "crontab.cpp"

						break; 
					}
					case 2:  {
							{
#line 434 "crontab.rl"
							parse_time_unit(ncs, p, 60, &ncs.v_time); }
						
#line 1040 "crontab.cpp"

						break; 
					}
					case 3:  {
							{
#line 435 "crontab.rl"
							parse_time_unit(ncs, p, 3600, &ncs.v_time); }
						
#line 1048 "crontab.cpp"

						break; 
					}
					case 4:  {
							{
#line 436 "crontab.rl"
							parse_time_unit(ncs, p, 86400, &ncs.v_time); }
						
#line 1056 "crontab.cpp"

						break; 
					}
					case 5:  {
							{
#line 437 "crontab.rl"
							parse_time_unit(ncs, p, 604800, &ncs.v_time); }
						
#line 1064 "crontab.cpp"

						break; 
					}
					case 6:  {
							{
#line 439 "crontab.rl"
							
							ncs.intv_st = p;
							ncs.v_int1 = ncs.v_int2 = 0;
							ncs.intv2_exist = false;
						}
						
#line 1076 "crontab.cpp"

						break; 
					}
					case 7:  {
							{
#line 444 "crontab.rl"
							parse_int_value(p, ncs.intv_st, ncs.linenum, &ncs.v_int1); }
						
#line 1084 "crontab.cpp"

						break; 
					}
					case 8:  {
							{
#line 445 "crontab.rl"
							ncs.intv2_st = p; }
						
#line 1092 "crontab.cpp"

						break; 
					}
					case 9:  {
							{
#line 446 "crontab.rl"
							parse_int_value(p, ncs.intv2_st, ncs.linenum, &ncs.v_int2); ncs.intv2_exist = true; }
						
#line 1100 "crontab.cpp"

						break; 
					}
					case 10:  {
							{
#line 447 "crontab.rl"
							
							using std::swap;
							swap(ncs.v_int1, ncs.v_int3);
							swap(ncs.v_int2, ncs.v_int4);
						}
						
#line 1112 "crontab.cpp"

						break; 
					}
					case 11:  {
							{
#line 452 "crontab.rl"
							
							ncs.v_int3 = -1;
							ncs.v_int4 = -1;
						}
						
#line 1123 "crontab.cpp"

						break; 
					}
					case 12:  {
							{
#line 457 "crontab.rl"
							ncs.strv_st = p; ncs.v_strlen = 0; }
						
#line 1131 "crontab.cpp"

						break; 
					}
					case 13:  {
							{
#line 458 "crontab.rl"
							
							ncs.v_strlen = p > ncs.strv_st ? static_cast<size_t>(p - ncs.strv_st) : 0;
							if (ncs.v_strlen >= sizeof ncs.v_str) {
								log_line("error parsing line %zu in crontab: too long", ncs.linenum);
								exit(EXIT_FAILURE);
							}
							memcpy(ncs.v_str, ncs.strv_st, ncs.v_strlen);
							ncs.v_str[ncs.v_strlen] = 0;
						}
						
#line 1147 "crontab.cpp"

						break; 
					}
					case 14:  {
							{
#line 481 "crontab.rl"
							ncs.ce->journal_ = true; }
						
#line 1155 "crontab.cpp"

						break; 
					}
					case 15:  {
							{
#line 484 "crontab.rl"
							
							ncs.ce->runat_ = true;
							ncs.ce->exectime_ = ncs.v_int1;
							ncs.ce->maxruns_ = 1;
							ncs.ce->journal_ = true;
						}
						
#line 1168 "crontab.cpp"

						break; 
					}
					case 16:  {
							{
#line 490 "crontab.rl"
							
							if (!ncs.ce->runat_)
							ncs.ce->maxruns_ = ncs.v_int1 > 0 ? static_cast<unsigned>(ncs.v_int1) : 0;
						}
						
#line 1179 "crontab.cpp"

						break; 
					}
					case 17:  {
							{
#line 498 "crontab.rl"
							ncs.ce->interval_ = ncs.v_time; }
						
#line 1187 "crontab.cpp"

						break; 
					}
					case 18:  {
							{
#line 507 "crontab.rl"
							add_cst_mon(ncs); }
						
#line 1195 "crontab.cpp"

						break; 
					}
					case 19:  {
							{
#line 508 "crontab.rl"
							add_cst_mday(ncs); }
						
#line 1203 "crontab.cpp"

						break; 
					}
					case 20:  {
							{
#line 509 "crontab.rl"
							add_cst_wday(ncs); }
						
#line 1211 "crontab.cpp"

						break; 
					}
					case 21:  {
							{
#line 510 "crontab.rl"
							add_cst_time(ncs); }
						
#line 1219 "crontab.cpp"

						break; 
					}
					case 22:  {
							{
#line 517 "crontab.rl"
							parse_command_key(ncs); }
						
#line 1227 "crontab.cpp"

						break; 
					}
					case 23:  {
							{
#line 524 "crontab.rl"
							ncs.jobid_st = p; }
						
#line 1235 "crontab.cpp"

						break; 
					}
					case 24:  {
							{
#line 525 "crontab.rl"
							parse_int_value(p, ncs.jobid_st, ncs.linenum, &ncs.ce->id_); }
						
#line 1243 "crontab.cpp"

						break; 
					}
					case 25:  {
							{
#line 526 "crontab.rl"
							ncs.finish_ce(); ncs.create_ce(); }
						
#line 1251 "crontab.cpp"

						break; 
					}
				}
				_nacts -= 1;
				_acts += 1;
			}
			
		}
		
		if ( p == eof ) {
			if ( ncs.cs >= 78 )
				goto _out;
		}
		else {
			if ( ncs.cs != 0 ) {
				p += 1;
				goto _resume;
			}
		}
		_out: {}
	}
	
#line 542 "crontab.rl"

	
	if (ncs.cs == ncrontab_error)
		return -1;
	if (ncs.cs >= ncrontab_first_final)
		return 1;
	return 0;
}

// Seeks back to start of file when done.
size_t count_config_jobs(FILE *f)
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
Job **stk, Job **deadstk)
{
	ParseCfgState ncs(stk, deadstk);
	
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
	g_jobs = static_cast<Job *>(xmalloc(g_njobs * sizeof(Job)));
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
		if (do_parse_config(ncs, buf, llen) < 0) {
			log_line("Config file '%s' is malformed at line %zu", path, ncs.linenum);
			exit(EXIT_FAILURE);
		}
	}
	ncs.finish_ce();
	parse_history(execfile);
	
	for (Job *j = g_jobs, *jend = g_jobs + g_njobs; j != jend; ++j) {
		bool alive = !j->runat_?
		((j->maxruns_ == 0 || j->numruns_ < j->maxruns_) && j->exectime_ != 0)
		: (j->numruns_ == 0);
		job_insert(alive ? stk : deadstk, j);
	}
	fclose(f);
}
