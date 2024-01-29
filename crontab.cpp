#line 1 "crontab.rl"
// Copyright 2003-2024 Nicholas J. Kain <njkain at gmail dot com>
// SPDX-License-Identifier: MIT
#include <algorithm>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#include <nk/from_string.hpp>
#include <nk/defer.hpp>
extern "C" {
#include "nk/log.h"
}
#include "ncron.hpp"
#include "sched.hpp"
#include "crontab.hpp"

#define MAX_LINE 2048

#ifdef __GNUC__
#pragma GCC diagnostic ignored "-Wold-style-cast"
#endif

extern int gflags_debug;

std::vector<Job> g_jobs;

struct item_history {
	time_t exectime = 0;
	time_t lasttime = 0;
	unsigned int numruns = 0;
};

struct history_entry
{
	history_entry(int id_, item_history h_) : id(id_), history(h_) {}
	
	int id;
	item_history history;
};
static std::vector<history_entry> history_lut;

struct ParseCfgState
{
	ParseCfgState(std::vector<size_t> *stk, std::vector<size_t> *dstk)
	: stack(stk), deadstack(dstk)
	{
		memset(v_str, 0, sizeof v_str);
	}
	char v_str[MAX_LINE];
	
	std::vector<size_t> *stack;
	std::vector<size_t> *deadstack;
	
	Job ce;
	
	const char *jobid_st = nullptr;
	const char *time_st = nullptr;
	const char *intv_st = nullptr;
	const char *intv2_st = nullptr;
	const char *strv_st = nullptr;
	
	size_t v_strlen = 0;
	size_t linenum = 0;
	
	unsigned int v_time;
	
	int v_int = 0;
	int v_int2 = 0;
	
	int cs = 0;
	bool have_command = false;
	
	bool intv2_exist = false;
	bool runat = false;
	
	bool parse_error = false;
	
	void get_history()
	{
		for (const auto &i: history_lut) {
			if (i.id == ce.id) {
				ce.exectime = i.history.exectime;
				ce.lasttime = i.history.lasttime;
				ce.numruns = i.history.numruns;
				return;
			}
		}
	}
	
	void create_ce()
	{
		ce.clear();
		have_command = false;
		runat = false;
	}
	
	inline void debug_print_ce() const
	{
		if (!gflags_debug)
			return;
		log_line("-=- finish_ce -=-");
		log_line("id: %d", ce.id);
		log_line("command: %s", ce.command ? ce.command : "");
		log_line("args: %s", ce.args ? ce.args : "");
		log_line("numruns: %u", ce.numruns);
		log_line("maxruns: %u", ce.maxruns);
		log_line("journal: %s", ce.journal ? "true" : "false");
		for (const auto &i: ce.month)
		log_line("month: [%u,%u]", i.first, i.second);
		for (const auto &i: ce.day)
		log_line("day: [%u,%u]", i.first, i.second);
		for (const auto &i: ce.weekday)
		log_line("weekday: [%u,%u]", i.first, i.second);
		for (const auto &i: ce.hour)
		log_line("hour: [%u,%u]", i.first, i.second);
		for (const auto &i: ce.minute)
		log_line("minute: [%u,%u]", i.first, i.second);
		log_line("runat: %s", runat ? "true" : "false");
		log_line("interval: %u", ce.interval);
		log_line("exectime: %lu", ce.exectime);
		log_line("lasttime: %lu", ce.lasttime);
	}
	
	inline void debug_print_ce_history() const
	{
		if (!gflags_debug)
			return;
		log_line("[%d]->numruns = %u", ce.id, ce.numruns);
		log_line("[%d]->exectime = %lu", ce.id, ce.exectime);
		log_line("[%d]->lasttime = %lu", ce.id, ce.lasttime);
	}
	
	void finish_ce()
	{
		const auto append_stack = [this](bool is_alive) {
			assert(g_jobs.size() > 0);
			(is_alive ? stack : deadstack)->emplace_back(g_jobs.size() - 1);
		};
		
		defer [this]{ create_ce(); };
		debug_print_ce();
		
		if (ce.id < 0
			|| (ce.interval <= 0 && ce.exectime <= 0)
		|| !ce.command || !have_command) {
			if (gflags_debug)
				log_line("===> IGNORE");
			return;
		}
		// XXX: O(n^2) might be nice to avoid.
		for (auto &i: g_jobs) {
			if (i.id == ce.id) {
				log_line("ERROR IN CRONTAB: ignoring duplicate entry for job %d", ce.id);
				return;
			}
		}
		if (gflags_debug)
			log_line("===> ADD");
		
		/* we have a job to insert */
		if (!runat) {
			get_history();
			debug_print_ce_history();
			set_initial_exectime(ce);
			
			auto numruns = ce.numruns;
			auto maxruns = ce.maxruns;
			auto exectime = ce.exectime;
			g_jobs.emplace_back(std::move(ce));
			
			/* insert iif numruns < maxruns and no constr error */
			append_stack((maxruns == 0 || numruns < maxruns) && exectime != 0);
		} else {
			if (ce.interval > 0) {
				log_line("ERROR IN CRONTAB: interval is unused when runat is set: job %d", ce.id);
			}
			auto forced_exectime = ce.exectime;
			get_history();
			ce.exectime = forced_exectime;
			debug_print_ce_history();
			
			auto numruns = ce.numruns;
			g_jobs.emplace_back(std::move(ce));
			
			/* insert iif we haven't exceeded maxruns */
			append_stack(numruns == 0);
		}
	}
};

struct hstm {
	const char *st = nullptr;
	int cs = 0;
	int id = -1;
	item_history h;
	bool parse_error = false;
};

#define MARKED_HST() hst.st, (p > hst.st ? static_cast<size_t>(p - hst.st) : 0)


#line 238 "crontab.rl"



#line 207 "crontab.cpp"
static const signed char _history_m_actions[] = {
	0, 1, 0, 1, 1, 1, 2, 1,
	3, 1, 4, 0
};

static const signed char _history_m_key_offsets[] = {
	0, 0, 2, 5, 7, 10, 12, 15,
	17, 0
};

static const char _history_m_trans_keys[] = {
	48, 57, 61, 48, 57, 48, 57, 58,
	48, 57, 48, 57, 124, 48, 57, 48,
	57, 48, 57, 0
};

static const signed char _history_m_single_lengths[] = {
	0, 0, 1, 0, 1, 0, 1, 0,
	0, 0
};

static const signed char _history_m_range_lengths[] = {
	0, 1, 1, 1, 1, 1, 1, 1,
	1, 0
};

static const signed char _history_m_index_offsets[] = {
	0, 0, 2, 5, 7, 10, 12, 15,
	17, 0
};

static const signed char _history_m_cond_targs[] = {
	2, 0, 3, 2, 0, 4, 0, 5,
	4, 0, 6, 0, 7, 6, 0, 8,
	0, 8, 0, 0, 1, 2, 3, 4,
	5, 6, 7, 8, 0
};

static const signed char _history_m_cond_actions[] = {
	1, 0, 9, 0, 0, 1, 0, 7,
	0, 0, 1, 0, 5, 0, 0, 1,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 3, 0
};

static const signed char _history_m_eof_trans[] = {
	20, 21, 22, 23, 24, 25, 26, 27,
	28, 0
};

static const int history_m_start = 1;
static const int history_m_first_final = 8;
static const int history_m_error = 0;

static const int history_m_en_main = 1;


#line 240 "crontab.rl"


static int do_parse_history(hstm &hst, const char *p, size_t plen)
{
	const char *pe = p + plen;
	const char *eof = pe;
	

#line 271 "crontab.cpp"
	{
		hst.cs = (int)history_m_start;
	}
	
#line 247 "crontab.rl"


#line 276 "crontab.cpp"
	{
		int _klen;
		unsigned int _trans = 0;
		const char * _keys;
		const signed char * _acts;
		unsigned int _nacts;
		_resume: {}
		if ( p == pe && p != eof )
			goto _out;
		if ( p == eof ) {
			if ( _history_m_eof_trans[hst.cs] > 0 ) {
				_trans = (unsigned int)_history_m_eof_trans[hst.cs] - 1;
			}
		}
		else {
			_keys = ( _history_m_trans_keys + (_history_m_key_offsets[hst.cs]));
			_trans = (unsigned int)_history_m_index_offsets[hst.cs];
			
			_klen = (int)_history_m_single_lengths[hst.cs];
			if ( _klen > 0 ) {
				const char *_lower = _keys;
				const char *_upper = _keys + _klen - 1;
				const char *_mid;
				while ( 1 ) {
					if ( _upper < _lower ) {
						_keys += _klen;
						_trans += (unsigned int)_klen;
						break;
					}
					
					_mid = _lower + ((_upper-_lower) >> 1);
					if ( ( (*( p))) < (*( _mid)) )
						_upper = _mid - 1;
					else if ( ( (*( p))) > (*( _mid)) )
						_lower = _mid + 1;
					else {
						_trans += (unsigned int)(_mid - _keys);
						goto _match;
					}
				}
			}
			
			_klen = (int)_history_m_range_lengths[hst.cs];
			if ( _klen > 0 ) {
				const char *_lower = _keys;
				const char *_upper = _keys + (_klen<<1) - 2;
				const char *_mid;
				while ( 1 ) {
					if ( _upper < _lower ) {
						_trans += (unsigned int)_klen;
						break;
					}
					
					_mid = _lower + (((_upper-_lower) >> 1) & ~1);
					if ( ( (*( p))) < (*( _mid)) )
						_upper = _mid - 2;
					else if ( ( (*( p))) > (*( _mid + 1)) )
						_lower = _mid + 2;
					else {
						_trans += (unsigned int)((_mid - _keys)>>1);
						break;
					}
				}
			}
			
			_match: {}
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
#line 207 "crontab.rl"
							hst.st = p; }
						
#line 358 "crontab.cpp"

						break; 
					}
					case 1:  {
							{
#line 208 "crontab.rl"
							
							if (!nk::from_string<time_t>(MARKED_HST(), &hst.h.lasttime)) {
								hst.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 371 "crontab.cpp"

						break; 
					}
					case 2:  {
							{
#line 214 "crontab.rl"
							
							if (!nk::from_string<unsigned>(MARKED_HST(), &hst.h.numruns)) {
								hst.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 384 "crontab.cpp"

						break; 
					}
					case 3:  {
							{
#line 220 "crontab.rl"
							
							if (!nk::from_string<time_t>(MARKED_HST(), &hst.h.exectime)) {
								hst.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 397 "crontab.cpp"

						break; 
					}
					case 4:  {
							{
#line 226 "crontab.rl"
							
							if (!nk::from_string<int>(MARKED_HST(), &hst.id)) {
								hst.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 410 "crontab.cpp"

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
	
#line 248 "crontab.rl"

	
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
	auto f = fopen(path, "r");
	if (!f) {
		log_line("%s: failed to open history file \"%s\" for read: %s",
		__func__, path, strerror(errno));
		return;
	}
	defer [&f]{ fclose(f); };
	size_t linenum = 0;
	while (!feof(f)) {
		if (!fgets(buf, sizeof buf, f)) {
			if (!feof(f))
				log_line("%s: io error fetching line of '%s'", __func__, path);
			break;
		}
		auto llen = strlen(buf);
		if (llen == 0)
			continue;
		if (buf[llen-1] == '\n')
			buf[--llen] = 0;
		++linenum;
		hstm hst;
		const auto r = do_parse_history(hst, buf, llen);
		if (r < 0) {
			log_line("%s: %s configuration at line %zu; ignoring",
			__func__, r == -2 ? "Incomplete" : "Malformed", linenum);
			continue;
		}
		history_lut.emplace_back(hst.id, hst.h);
	}
}

static inline bool addcstlist(ParseCfgState &ncs, Job::cst_list &list,
int wildcard, int min, int max)
{
	int low = ncs.v_int;
	int high = ncs.intv2_exist ? ncs.v_int2 : wildcard;
	return ncs.ce.add_constraint(list, low, high, wildcard, min, max);
}

struct Pckm {
	Pckm() {}
	char *st = nullptr;
	int cs = 0;
};

#define MARKED_PCKM() pckm.st, (p > pckm.st ? static_cast<size_t>(p - pckm.st) : 0)


#line 352 "crontab.rl"



#line 496 "crontab.cpp"
static const signed char _parse_cmd_key_m_actions[] = {
	0, 1, 0, 1, 1, 1, 2, 2,
	0, 2, 2, 1, 0, 2, 1, 2,
	3, 1, 0, 2, 0
};

static const signed char _parse_cmd_key_m_key_offsets[] = {
	0, 0, 4, 8, 11, 12, 16, 20,
	24, 0
};

static const char _parse_cmd_key_m_trans_keys[] = {
	0, 9, 32, 92, 0, 9, 32, 92,
	0, 9, 32, 0, 0, 9, 32, 92,
	0, 9, 32, 92, 0, 9, 32, 92,
	0, 9, 32, 92, 0
};

static const signed char _parse_cmd_key_m_single_lengths[] = {
	0, 4, 4, 3, 1, 4, 4, 4,
	4, 0
};

static const signed char _parse_cmd_key_m_range_lengths[] = {
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0
};

static const signed char _parse_cmd_key_m_index_offsets[] = {
	0, 0, 5, 10, 14, 16, 21, 26,
	31, 0
};

static const signed char _parse_cmd_key_m_cond_targs[] = {
	0, 1, 1, 5, 2, 0, 3, 3,
	5, 2, 0, 3, 3, 4, 0, 4,
	0, 6, 6, 5, 2, 0, 3, 3,
	8, 7, 0, 3, 3, 8, 7, 0,
	6, 6, 8, 7, 0, 1, 2, 3,
	4, 5, 6, 7, 8, 0
};

static const signed char _parse_cmd_key_m_cond_actions[] = {
	0, 0, 0, 1, 1, 0, 3, 3,
	0, 0, 0, 1, 1, 1, 0, 0,
	0, 3, 3, 0, 0, 0, 10, 10,
	1, 1, 0, 3, 3, 0, 0, 0,
	3, 3, 0, 0, 0, 0, 3, 7,
	5, 3, 16, 13, 13, 0
};

static const signed char _parse_cmd_key_m_eof_trans[] = {
	37, 38, 39, 40, 41, 42, 43, 44,
	45, 0
};

static const int parse_cmd_key_m_start = 1;
static const int parse_cmd_key_m_first_final = 2;
static const int parse_cmd_key_m_error = 0;

static const int parse_cmd_key_m_en_main = 1;


#line 354 "crontab.rl"


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
	
#line 369 "crontab.rl"


#line 579 "crontab.cpp"
	{
		int _klen;
		unsigned int _trans = 0;
		const char * _keys;
		const signed char * _acts;
		unsigned int _nacts;
		_resume: {}
		if ( p == pe && p != eof )
			goto _out;
		if ( p == eof ) {
			if ( _parse_cmd_key_m_eof_trans[pckm.cs] > 0 ) {
				_trans = (unsigned int)_parse_cmd_key_m_eof_trans[pckm.cs] - 1;
			}
		}
		else {
			_keys = ( _parse_cmd_key_m_trans_keys + (_parse_cmd_key_m_key_offsets[pckm.cs]));
			_trans = (unsigned int)_parse_cmd_key_m_index_offsets[pckm.cs];
			
			_klen = (int)_parse_cmd_key_m_single_lengths[pckm.cs];
			if ( _klen > 0 ) {
				const char *_lower = _keys;
				const char *_upper = _keys + _klen - 1;
				const char *_mid;
				while ( 1 ) {
					if ( _upper < _lower ) {
						_keys += _klen;
						_trans += (unsigned int)_klen;
						break;
					}
					
					_mid = _lower + ((_upper-_lower) >> 1);
					if ( ( (*( p))) < (*( _mid)) )
						_upper = _mid - 1;
					else if ( ( (*( p))) > (*( _mid)) )
						_lower = _mid + 1;
					else {
						_trans += (unsigned int)(_mid - _keys);
						goto _match;
					}
				}
			}
			
			_klen = (int)_parse_cmd_key_m_range_lengths[pckm.cs];
			if ( _klen > 0 ) {
				const char *_lower = _keys;
				const char *_upper = _keys + (_klen<<1) - 2;
				const char *_mid;
				while ( 1 ) {
					if ( _upper < _lower ) {
						_trans += (unsigned int)_klen;
						break;
					}
					
					_mid = _lower + (((_upper-_lower) >> 1) & ~1);
					if ( ( (*( p))) < (*( _mid)) )
						_upper = _mid - 2;
					else if ( ( (*( p))) > (*( _mid + 1)) )
						_lower = _mid + 2;
					else {
						_trans += (unsigned int)((_mid - _keys)>>1);
						break;
					}
				}
			}
			
			_match: {}
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
#line 312 "crontab.rl"
							pckm.st = p; }
						
#line 661 "crontab.cpp"

						break; 
					}
					case 1:  {
							{
#line 313 "crontab.rl"
							
							size_t l = p > pckm.st ? static_cast<size_t>(p - pckm.st) : 0;
							if (l) {
								auto ts = static_cast<char *>(malloc(l + 1));
								bool prior_bs = false;
								auto d = ts;
								for (auto c = pckm.st; c < p; ++c) {
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
								ncs.ce.command = ts;
							}
						}
						
#line 692 "crontab.cpp"

						break; 
					}
					case 2:  {
							{
#line 337 "crontab.rl"
							
							size_t l = p > pckm.st ? static_cast<size_t>(p - pckm.st) : 0;
							if (l) {
								auto ts = static_cast<char *>(malloc(l + 1));
								memcpy(ts, pckm.st, l);
								ts[l] = 0;
								ncs.ce.args = ts;
							}
						}
						
#line 708 "crontab.cpp"

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
	
#line 370 "crontab.rl"

	
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

#define MARKED_TIME() ncs.time_st, (p > (ncs.time_st + 1) ? static_cast<size_t>(p - ncs.time_st - 1) : 0)
#define MARKED_INTV1() ncs.intv_st, (p > ncs.intv_st ? static_cast<size_t>(p - ncs.intv_st) : 0)
#define MARKED_INTV2() ncs.intv2_st, (p > ncs.intv2_st ? static_cast<size_t>(p - ncs.intv2_st) : 0)
#define MARKED_JOBID() ncs.jobid_st, (p > ncs.jobid_st ? static_cast<size_t>(p - ncs.jobid_st) : 0)


#line 534 "crontab.rl"



#line 752 "crontab.cpp"
static const signed char _ncrontab_actions[] = {
	0, 1, 0, 1, 1, 1, 2, 1,
	3, 1, 4, 1, 5, 1, 6, 1,
	7, 1, 8, 1, 10, 1, 12, 1,
	22, 1, 23, 1, 24, 2, 1, 0,
	2, 1, 15, 2, 2, 0, 2, 2,
	15, 2, 3, 0, 2, 3, 15, 2,
	4, 0, 2, 4, 15, 2, 5, 0,
	2, 5, 15, 2, 7, 13, 2, 7,
	14, 2, 7, 16, 2, 7, 17, 2,
	7, 18, 2, 7, 19, 2, 7, 20,
	2, 9, 16, 2, 9, 17, 2, 9,
	18, 2, 9, 19, 2, 9, 20, 2,
	11, 21, 0
};

static const short _ncrontab_key_offsets[] = {
	0, 0, 19, 21, 23, 25, 27, 29,
	31, 33, 36, 40, 42, 44, 47, 51,
	53, 55, 57, 59, 62, 66, 68, 70,
	72, 74, 76, 78, 80, 82, 85, 89,
	96, 98, 100, 102, 104, 106, 108, 114,
	116, 118, 120, 122, 124, 127, 131, 133,
	135, 137, 139, 142, 146, 148, 150, 152,
	154, 157, 161, 163, 165, 167, 169, 171,
	174, 178, 180, 182, 184, 186, 188, 190,
	193, 197, 199, 201, 201, 203, 207, 210,
	212, 215, 217, 221, 225, 229, 233, 237,
	237, 239, 242, 244, 247, 249, 251, 254,
	0
};

static const char _ncrontab_trans_keys[] = {
	33, 35, 59, 67, 68, 72, 73, 74,
	77, 82, 87, 99, 100, 104, 105, 106,
	109, 114, 119, 48, 57, 79, 111, 77,
	109, 77, 109, 65, 97, 78, 110, 68,
	100, 9, 32, 61, 0, 9, 10, 32,
	65, 97, 89, 121, 9, 32, 61, 9,
	32, 48, 57, 48, 57, 79, 111, 85,
	117, 82, 114, 9, 32, 61, 9, 32,
	48, 57, 48, 57, 78, 110, 84, 116,
	69, 101, 82, 114, 86, 118, 65, 97,
	76, 108, 9, 32, 61, 9, 32, 48,
	57, 100, 104, 109, 115, 119, 48, 57,
	79, 111, 85, 117, 82, 114, 78, 110,
	65, 97, 76, 108, 65, 73, 79, 97,
	105, 111, 88, 120, 82, 114, 85, 117,
	78, 110, 83, 115, 9, 32, 61, 9,
	32, 48, 57, 78, 110, 85, 117, 84,
	116, 69, 101, 9, 32, 61, 9, 32,
	48, 57, 48, 57, 78, 110, 84, 116,
	72, 104, 9, 32, 61, 9, 32, 48,
	57, 48, 57, 85, 117, 78, 110, 65,
	97, 84, 116, 9, 32, 61, 9, 32,
	48, 57, 69, 101, 69, 101, 75, 107,
	68, 100, 65, 97, 89, 121, 9, 32,
	61, 9, 32, 48, 57, 48, 57, 48,
	57, 0, 10, 0, 9, 10, 32, 44,
	48, 57, 48, 57, 44, 48, 57, 48,
	57, 9, 32, 48, 57, 9, 32, 48,
	57, 9, 32, 48, 57, 9, 32, 48,
	57, 9, 32, 48, 57, 48, 57, 44,
	48, 57, 48, 57, 44, 48, 57, 48,
	57, 48, 57, 44, 48, 57, 48, 57,
	0
};

static const signed char _ncrontab_single_lengths[] = {
	0, 19, 0, 2, 2, 2, 2, 2,
	2, 3, 4, 2, 2, 3, 2, 0,
	2, 2, 2, 3, 2, 0, 2, 2,
	2, 2, 2, 2, 2, 3, 2, 5,
	2, 2, 2, 2, 2, 2, 6, 2,
	2, 2, 2, 2, 3, 2, 2, 2,
	2, 2, 3, 2, 0, 2, 2, 2,
	3, 2, 0, 2, 2, 2, 2, 3,
	2, 2, 2, 2, 2, 2, 2, 3,
	2, 0, 0, 0, 2, 4, 1, 0,
	1, 0, 2, 2, 2, 2, 2, 0,
	0, 1, 0, 1, 0, 0, 1, 0,
	0
};

static const signed char _ncrontab_range_lengths[] = {
	0, 0, 1, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 1, 1,
	0, 0, 0, 0, 1, 1, 0, 0,
	0, 0, 0, 0, 0, 0, 1, 1,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 1, 0, 0,
	0, 0, 0, 1, 1, 0, 0, 0,
	0, 1, 1, 0, 0, 0, 0, 0,
	1, 0, 0, 0, 0, 0, 0, 0,
	1, 1, 1, 0, 0, 0, 1, 1,
	1, 1, 1, 1, 1, 1, 1, 0,
	1, 1, 1, 1, 1, 1, 1, 1,
	0
};

static const short _ncrontab_index_offsets[] = {
	0, 0, 20, 22, 25, 28, 31, 34,
	37, 40, 44, 49, 52, 55, 59, 63,
	65, 68, 71, 74, 78, 82, 84, 87,
	90, 93, 96, 99, 102, 105, 109, 113,
	120, 123, 126, 129, 132, 135, 138, 145,
	148, 151, 154, 157, 160, 164, 168, 171,
	174, 177, 180, 184, 188, 190, 193, 196,
	199, 203, 207, 209, 212, 215, 218, 221,
	225, 229, 232, 235, 238, 241, 244, 247,
	251, 255, 257, 259, 260, 263, 268, 271,
	273, 276, 278, 282, 286, 290, 294, 298,
	299, 301, 304, 306, 309, 311, 313, 316,
	0
};

static const signed char _ncrontab_cond_targs[] = {
	2, 75, 75, 3, 11, 16, 22, 32,
	38, 59, 65, 3, 11, 16, 22, 32,
	38, 59, 65, 0, 74, 0, 4, 4,
	0, 5, 5, 0, 6, 6, 0, 7,
	7, 0, 8, 8, 0, 9, 9, 0,
	9, 9, 10, 0, 0, 77, 0, 77,
	76, 12, 12, 0, 13, 13, 0, 13,
	13, 14, 0, 14, 14, 78, 0, 79,
	0, 17, 17, 0, 18, 18, 0, 19,
	19, 0, 19, 19, 20, 0, 20, 20,
	80, 0, 81, 0, 23, 23, 0, 24,
	24, 0, 25, 25, 0, 26, 26, 0,
	27, 27, 0, 28, 28, 0, 29, 29,
	0, 29, 29, 30, 0, 30, 30, 31,
	0, 82, 83, 84, 85, 86, 31, 0,
	33, 33, 0, 34, 34, 0, 35, 35,
	0, 36, 36, 0, 37, 37, 0, 87,
	87, 0, 39, 46, 53, 39, 46, 53,
	0, 40, 40, 0, 41, 41, 0, 42,
	42, 0, 43, 43, 0, 44, 44, 0,
	44, 44, 45, 0, 45, 45, 88, 0,
	47, 47, 0, 48, 48, 0, 49, 49,
	0, 50, 50, 0, 50, 50, 51, 0,
	51, 51, 89, 0, 90, 0, 54, 54,
	0, 55, 55, 0, 56, 56, 0, 56,
	56, 57, 0, 57, 57, 91, 0, 92,
	0, 60, 60, 0, 61, 61, 0, 62,
	62, 0, 63, 63, 0, 63, 63, 64,
	0, 64, 64, 93, 0, 66, 66, 0,
	67, 67, 0, 68, 68, 0, 69, 69,
	0, 70, 70, 0, 71, 71, 0, 71,
	71, 72, 0, 72, 72, 94, 0, 95,
	0, 74, 0, 75, 0, 0, 76, 0,
	77, 0, 77, 76, 15, 78, 0, 79,
	0, 21, 80, 0, 81, 0, 30, 30,
	31, 0, 30, 30, 31, 0, 30, 30,
	31, 0, 30, 30, 31, 0, 30, 30,
	31, 0, 0, 88, 0, 52, 89, 0,
	90, 0, 58, 91, 0, 92, 0, 93,
	0, 73, 94, 0, 95, 0, 0, 1,
	2, 3, 4, 5, 6, 7, 8, 9,
	10, 11, 12, 13, 14, 15, 16, 17,
	18, 19, 20, 21, 22, 23, 24, 25,
	26, 27, 28, 29, 30, 31, 32, 33,
	34, 35, 36, 37, 38, 39, 40, 41,
	42, 43, 44, 45, 46, 47, 48, 49,
	50, 51, 52, 53, 54, 55, 56, 57,
	58, 59, 60, 61, 62, 63, 64, 65,
	66, 67, 68, 69, 70, 71, 72, 73,
	74, 75, 76, 77, 78, 79, 80, 81,
	82, 83, 84, 85, 86, 87, 88, 89,
	90, 91, 92, 93, 94, 95, 0
};

static const signed char _ncrontab_cond_actions[] = {
	27, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 23, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 19, 0, 19,
	19, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 13, 0, 17,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	13, 0, 17, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 1,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 13, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 13, 0, 17, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 13, 0, 17,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 13, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 13, 0, 17,
	0, 0, 0, 0, 0, 0, 0, 0,
	19, 0, 19, 19, 15, 0, 0, 0,
	0, 15, 0, 0, 0, 0, 9, 9,
	47, 0, 7, 7, 41, 0, 5, 5,
	35, 0, 3, 3, 29, 0, 11, 11,
	53, 0, 0, 0, 0, 15, 0, 0,
	0, 0, 15, 0, 0, 0, 0, 0,
	0, 15, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0,
	25, 0, 95, 95, 68, 83, 74, 89,
	50, 44, 38, 32, 56, 21, 62, 77,
	92, 65, 80, 59, 71, 86, 0
};

static const short _ncrontab_eof_trans[] = {
	319, 320, 321, 322, 323, 324, 325, 326,
	327, 328, 329, 330, 331, 332, 333, 334,
	335, 336, 337, 338, 339, 340, 341, 342,
	343, 344, 345, 346, 347, 348, 349, 350,
	351, 352, 353, 354, 355, 356, 357, 358,
	359, 360, 361, 362, 363, 364, 365, 366,
	367, 368, 369, 370, 371, 372, 373, 374,
	375, 376, 377, 378, 379, 380, 381, 382,
	383, 384, 385, 386, 387, 388, 389, 390,
	391, 392, 393, 394, 395, 396, 397, 398,
	399, 400, 401, 402, 403, 404, 405, 406,
	407, 408, 409, 410, 411, 412, 413, 414,
	0
};

static const int ncrontab_start = 1;
static const int ncrontab_first_final = 74;
static const int ncrontab_error = 0;

static const int ncrontab_en_main = 1;


#line 536 "crontab.rl"


static int do_parse_config(ParseCfgState &ncs, const char *p, size_t plen)
{
	const char *pe = p + plen;
	const char *eof = pe;
	

#line 1008 "crontab.cpp"
	{
		ncs.cs = (int)ncrontab_start;
	}
	
#line 543 "crontab.rl"


#line 1013 "crontab.cpp"
	{
		int _klen;
		unsigned int _trans = 0;
		const char * _keys;
		const signed char * _acts;
		unsigned int _nacts;
		_resume: {}
		if ( p == pe && p != eof )
			goto _out;
		if ( p == eof ) {
			if ( _ncrontab_eof_trans[ncs.cs] > 0 ) {
				_trans = (unsigned int)_ncrontab_eof_trans[ncs.cs] - 1;
			}
		}
		else {
			_keys = ( _ncrontab_trans_keys + (_ncrontab_key_offsets[ncs.cs]));
			_trans = (unsigned int)_ncrontab_index_offsets[ncs.cs];
			
			_klen = (int)_ncrontab_single_lengths[ncs.cs];
			if ( _klen > 0 ) {
				const char *_lower = _keys;
				const char *_upper = _keys + _klen - 1;
				const char *_mid;
				while ( 1 ) {
					if ( _upper < _lower ) {
						_keys += _klen;
						_trans += (unsigned int)_klen;
						break;
					}
					
					_mid = _lower + ((_upper-_lower) >> 1);
					if ( ( (*( p))) < (*( _mid)) )
						_upper = _mid - 1;
					else if ( ( (*( p))) > (*( _mid)) )
						_lower = _mid + 1;
					else {
						_trans += (unsigned int)(_mid - _keys);
						goto _match;
					}
				}
			}
			
			_klen = (int)_ncrontab_range_lengths[ncs.cs];
			if ( _klen > 0 ) {
				const char *_lower = _keys;
				const char *_upper = _keys + (_klen<<1) - 2;
				const char *_mid;
				while ( 1 ) {
					if ( _upper < _lower ) {
						_trans += (unsigned int)_klen;
						break;
					}
					
					_mid = _lower + (((_upper-_lower) >> 1) & ~1);
					if ( ( (*( p))) < (*( _mid)) )
						_upper = _mid - 2;
					else if ( ( (*( p))) > (*( _mid + 1)) )
						_lower = _mid + 2;
					else {
						_trans += (unsigned int)((_mid - _keys)>>1);
						break;
					}
				}
			}
			
			_match: {}
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
#line 395 "crontab.rl"
							ncs.time_st = p; ncs.v_time = 0; }
						
#line 1095 "crontab.cpp"

						break; 
					}
					case 1:  {
							{
#line 396 "crontab.rl"
							
							unsigned t;
							if (!nk::from_string<unsigned>(MARKED_TIME(), &t)) {
								ncs.parse_error = true;
								{p += 1; goto _out; }
							}
							ncs.v_time += t;
						}
						
#line 1110 "crontab.cpp"

						break; 
					}
					case 2:  {
							{
#line 404 "crontab.rl"
							
							unsigned t;
							if (!nk::from_string<unsigned>(MARKED_TIME(), &t)) {
								ncs.parse_error = true;
								{p += 1; goto _out; }
							}
							ncs.v_time += 60 * t;
						}
						
#line 1125 "crontab.cpp"

						break; 
					}
					case 3:  {
							{
#line 412 "crontab.rl"
							
							unsigned t;
							if (!nk::from_string<unsigned>(MARKED_TIME(), &t)) {
								ncs.parse_error = true;
								{p += 1; goto _out; }
							}
							ncs.v_time += 3600 * t;
						}
						
#line 1140 "crontab.cpp"

						break; 
					}
					case 4:  {
							{
#line 420 "crontab.rl"
							
							unsigned t;
							if (!nk::from_string<unsigned>(MARKED_TIME(), &t)) {
								ncs.parse_error = true;
								{p += 1; goto _out; }
							}
							ncs.v_time += 86400 * t;
						}
						
#line 1155 "crontab.cpp"

						break; 
					}
					case 5:  {
							{
#line 428 "crontab.rl"
							
							unsigned t;
							if (!nk::from_string<unsigned>(MARKED_TIME(), &t)) {
								ncs.parse_error = true;
								{p += 1; goto _out; }
							}
							ncs.v_time += 604800 * t;
						}
						
#line 1170 "crontab.cpp"

						break; 
					}
					case 6:  {
							{
#line 437 "crontab.rl"
							
							ncs.intv_st = p;
							ncs.v_int = ncs.v_int2 = 0;
							ncs.intv2_exist = false;
						}
						
#line 1182 "crontab.cpp"

						break; 
					}
					case 7:  {
							{
#line 442 "crontab.rl"
							
							if (!nk::from_string<int>(MARKED_INTV1(), &ncs.v_int)) {
								ncs.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 1195 "crontab.cpp"

						break; 
					}
					case 8:  {
							{
#line 448 "crontab.rl"
							ncs.intv2_st = p; }
						
#line 1203 "crontab.cpp"

						break; 
					}
					case 9:  {
							{
#line 449 "crontab.rl"
							
							if (!nk::from_string<int>(MARKED_INTV2(), &ncs.v_int2)) {
								ncs.parse_error = true;
								{p += 1; goto _out; }
							}
							ncs.intv2_exist = true;
						}
						
#line 1217 "crontab.cpp"

						break; 
					}
					case 10:  {
							{
#line 457 "crontab.rl"
							ncs.strv_st = p; ncs.v_strlen = 0; }
						
#line 1225 "crontab.cpp"

						break; 
					}
					case 11:  {
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
						
#line 1241 "crontab.cpp"

						break; 
					}
					case 12:  {
							{
#line 481 "crontab.rl"
							ncs.ce.journal = true; }
						
#line 1249 "crontab.cpp"

						break; 
					}
					case 13:  {
							{
#line 484 "crontab.rl"
							
							ncs.runat = true;
							ncs.ce.exectime = ncs.v_int;
							ncs.ce.maxruns = 1;
							ncs.ce.journal = true;
						}
						
#line 1262 "crontab.cpp"

						break; 
					}
					case 14:  {
							{
#line 490 "crontab.rl"
							
							if (!ncs.runat)
							ncs.ce.maxruns = ncs.v_int > 0 ? static_cast<unsigned>(ncs.v_int) : 0;
						}
						
#line 1273 "crontab.cpp"

						break; 
					}
					case 15:  {
							{
#line 498 "crontab.rl"
							ncs.ce.interval = ncs.v_time; }
						
#line 1281 "crontab.cpp"

						break; 
					}
					case 16:  {
							{
#line 502 "crontab.rl"
							addcstlist(ncs, ncs.ce.month, 0, 1, 12); }
						
#line 1289 "crontab.cpp"

						break; 
					}
					case 17:  {
							{
#line 503 "crontab.rl"
							addcstlist(ncs, ncs.ce.day, 0, 1, 31); }
						
#line 1297 "crontab.cpp"

						break; 
					}
					case 18:  {
							{
#line 504 "crontab.rl"
							addcstlist(ncs, ncs.ce.weekday, 0, 1, 7); }
						
#line 1305 "crontab.cpp"

						break; 
					}
					case 19:  {
							{
#line 505 "crontab.rl"
							addcstlist(ncs, ncs.ce.hour, 24, 0, 23); }
						
#line 1313 "crontab.cpp"

						break; 
					}
					case 20:  {
							{
#line 506 "crontab.rl"
							addcstlist(ncs, ncs.ce.minute, 60, 0, 59); }
						
#line 1321 "crontab.cpp"

						break; 
					}
					case 21:  {
							{
#line 514 "crontab.rl"
							parse_command_key(ncs); }
						
#line 1329 "crontab.cpp"

						break; 
					}
					case 22:  {
							{
#line 521 "crontab.rl"
							ncs.jobid_st = p; }
						
#line 1337 "crontab.cpp"

						break; 
					}
					case 23:  {
							{
#line 522 "crontab.rl"
							
							if (!nk::from_string<int>(MARKED_JOBID(), &ncs.ce.id)) {
								ncs.parse_error = true;
								{p += 1; goto _out; }
							}
						}
						
#line 1350 "crontab.cpp"

						break; 
					}
					case 24:  {
							{
#line 528 "crontab.rl"
							ncs.finish_ce(); }
						
#line 1358 "crontab.cpp"

						break; 
					}
				}
				_nacts -= 1;
				_acts += 1;
			}
			
		}
		
		if ( p == eof ) {
			if ( ncs.cs >= 74 )
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
	
#line 544 "crontab.rl"

	
	if (ncs.parse_error) return -1;
		if (ncs.cs == ncrontab_error)
		return -1;
	if (ncs.cs >= ncrontab_first_final)
		return 1;
	return 0;
}

void parse_config(char const *path, char const *execfile,
std::vector<size_t> *stk,
std::vector<size_t> *deadstk)
{
	g_jobs.clear();
	ParseCfgState ncs(stk, deadstk);
	parse_history(execfile);
	
	char buf[MAX_LINE];
	auto f = fopen(path, "r");
	if (!f) {
		log_line("%s: failed to open file: '%s': %s", __func__, path, strerror(errno));
		exit(EXIT_FAILURE);
	}
	defer [&f]{ fclose(f); };
	while (!feof(f)) {
		if (!fgets(buf, sizeof buf, f)) {
			if (!feof(f))
				log_line("%s: io error fetching line of '%s'", __func__, path);
			break;
		}
		auto llen = strlen(buf);
		if (llen == 0)
			continue;
		if (buf[llen-1] == '\n')
			buf[--llen] = 0;
		++ncs.linenum;
		if (do_parse_config(ncs, buf, llen) < 0) {
			log_line("%s: do_parse_config(%s) failed at line %zu", __func__, path, ncs.linenum);
			exit(EXIT_FAILURE);
		}
	}
	ncs.finish_ce();
	for (auto &i: g_jobs) i.merge_constraints();
	std::sort(stk->begin(), stk->end(), LtCronEntry);
	history_lut.clear();
}

