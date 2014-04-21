#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "cfg.h"
#include "ncron.h"
#include "nk/log.h"
#include "nk/privilege.h"
#include "nk/copy_cmdarg.h"

struct cfgparse {
   char buf[MAX_BUF];
   size_t buflen;
   int ternary; // = 0 nothing, -1 = false, +1 = true
   int cs;
};

%%{
    machine cfg_actions;
    access ccfg.;

    action clear {
        memset(&ccfg.buf, 0, sizeof ccfg.buf);
        ccfg.buflen = 0;
        ccfg.ternary = 0;
    }
    action append {
        if (ccfg.buflen < sizeof ccfg.buf - 1)
            ccfg.buf[ccfg.buflen++] = *p;
        else
            suicide("line or option is too long");
    }
    action term {
        if (ccfg.buflen < sizeof ccfg.buf)
            ccfg.buf[ccfg.buflen] = 0;
    }
    action truval { ccfg.ternary = 1; }
    action falsval { ccfg.ternary = -1; }

    action background {
        switch (ccfg.ternary) {
        case 1: gflags_detach = 1; break;
        case -1: gflags_detach = 0; default: break;
        }
    }
    action sleep {
        g_initial_sleep = atoi(optarg);
        if (g_initial_sleep < 0)
            g_initial_sleep = 0;
        break;
    }
    action noexecsave {
        switch (ccfg.ternary) {
        case 1: g_ncron_execmode = 2; default: break;
        }
    }
    action journal {
        switch (ccfg.ternary) {
        case 1: g_ncron_execmode = 1; default: break;
        }
    }
    action crontabfile {
        copy_cmdarg(g_ncron_conf, ccfg.buf, sizeof g_ncron_conf, "conf");
    }
    action historyfile {
        copy_cmdarg(g_ncron_execfile, ccfg.buf, sizeof g_ncron_execfile,
                    "history");
    }
    action pidfile {
        copy_cmdarg(g_ncron_pidfile, ccfg.buf, sizeof g_ncron_pidfile,
                    "pidfile");
    }
    action quiet {
        switch (ccfg.ternary) {
        case 1: gflags_quiet = 1; default: break;
        }
    }
    action version { print_version(); exit(EXIT_SUCCESS); }
    action help { show_usage(); exit(EXIT_SUCCESS); }
}%%

%%{
    machine file_cfg;
    access ccfg.;
    include cfg_actions;

    spc = [ \t];
    delim = spc* '=' spc*;
    string = [^\n]+ >clear $append %term;
    term = '\n';
    value = delim string term;
    truval = ('true'|'1') % truval;
    falsval = ('false'|'0') % falsval;
    boolval = delim (truval|falsval) term;

    blankline = term;

    background = 'background' boolval @background;
    sleep = 'sleep' value @sleep;
    noexecsave = 'noexecsave' boolval @noexecsave;
    journal = 'journal' boolval @journal;
    crontabfile = 'crontab' value @crontabfile;
    historyfile = 'history' value @historyfile;
    pidfile = 'pidfile' value @pidfile;
    quiet = 'quiet' boolval @quiet;

    main := blankline |
        background | sleep | noexecsave | journal | crontabfile | historyfile |
        pidfile | quiet
    ;
}%%

%% write data;

static void parse_cfgfile(const char *fname)
{
    struct cfgparse ccfg;
    memset(&ccfg, 0, sizeof ccfg);
    FILE *f = fopen(fname, "r");
    if (!f)
        suicide("Unable to open config file '%s'.", fname);
    char l[MAX_BUF];
    size_t linenum = 0;
    while (linenum++, fgets(l, sizeof l, f)) {
        size_t llen = strlen(l);
        const char *p = l;
        const char *pe = l + llen;
        %% write init;
        %% write exec;

        if (ccfg.cs == file_cfg_error)
            suicide("error parsing config file line %zu: malformed", linenum);
        if (ccfg.cs < file_cfg_first_final)
            suicide("error parsing config file line %zu: incomplete", linenum);
    }
    fclose(f);
}

%%{
    machine cmd_cfg;
    access ccfg.;
    include cfg_actions;

    action cfgfile { parse_cfgfile(ccfg.buf); }
    action tbv { ccfg.ternary = 1; }

    string = [^\0]+ >clear $append %term;
    argval = 0 string 0;
    tbv = 0 % tbv;

    cfgfile = ('-c'|'--config') argval @cfgfile;
    background = ('-b'|'--background') tbv @background;
    sleep = ('-s'|'--sleep') argval @sleep;
    noexecsave = ('-0'|'--noexecsave') tbv @noexecsave;
    journal = ('-j'|'--journal') tbv @journal;
    crontabfile = ('-t'|'--crontab') argval @crontabfile;
    historyfile = ('-H'|'--history') argval @historyfile;
    pidfile = ('-p'|'--pidfile') argval @pidfile;
    quiet = ('-q'|'--quiet') tbv @quiet;
    version = ('-v'|'--version') 0 @version;
    help = ('-h'|'--help') 0 @help;

    main := (
        cfgfile | background | sleep | noexecsave | journal | crontabfile |
        historyfile | pidfile | quiet | version | help
    )*;
}%%

%% write data;

void parse_cmdline(int argc, char *argv[])
{
    char argb[8192];
    size_t argbl = 0;
    for (size_t i = 1; i < (size_t)argc; ++i) {
        ssize_t snl;
        if (i > 1)
            snl = snprintf(argb + argbl, sizeof argb - argbl, "%c%s",
                           0, argv[i]);
        else
            snl = snprintf(argb + argbl, sizeof argb - argbl, "%s", argv[i]);
        if (snl < 0 || (size_t)snl >= sizeof argb)
            suicide("error parsing command line option: option too long");
        argbl += snl;
    }
    if (argbl == 0)
        return;
    struct cfgparse ccfg;
    memset(&ccfg, 0, sizeof ccfg);
    const char *p = argb;
    const char *pe = argb + argbl + 1;
    const char *eof = pe;

    %% write init;
    %% write exec;

    if (ccfg.cs == cmd_cfg_error)
        suicide("error parsing command line option: malformed");
    if (ccfg.cs >= cmd_cfg_first_final)
        return;
    suicide("error parsing command line option: incomplete");
}

