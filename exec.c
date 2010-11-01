/*
 * exec.c - functions to exec a job
 * Time-stamp: <2010-11-01 17:07:21 nk>
 *
 * (c) 2003-2010 Nicholas J. Kain <njkain@gmail.com>
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

#include <sys/types.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <pwd.h>

#include "defines.h"
#include "log.h"
#include "nstrl.h"

#ifndef HAVE_CLEARENV
extern char **environ;
#endif

void ncm_fix_env(uid_t uid)
{
    struct passwd *pw;
    char uids[20];

#ifdef HAVE_CLEARENV
    clearenv();
#else
    environ = NULL; /* clearenv isn't portable */
#endif

    pw = getpwuid(uid);

    if (pw == NULL) {
        log_line("User uid %i does not exist.  Not exec'ing.", uid);
        exit(EXIT_FAILURE);
    }

    snprintf(uids, sizeof uids, "%i", uid);
    if (setenv("UID", uids, 1))
        goto fail_fix_env;

    if (setenv("USER", pw->pw_name, 1))
        goto fail_fix_env;
    if (setenv("USERNAME", pw->pw_name, 1))
        goto fail_fix_env;
    if (setenv("LOGNAME", pw->pw_name, 1))
        goto fail_fix_env;

    if (setenv("HOME", pw->pw_dir, 1))
        goto fail_fix_env;
    if (setenv("PWD", pw->pw_dir, 1))
        goto fail_fix_env;

    if (chdir(pw->pw_dir)) {
        log_line("Failed to chdir to uid %i's homedir.  Not exec'ing.", uid);
        exit(EXIT_FAILURE);
    }

    if (setenv("SHELL", pw->pw_shell, 1))
        goto fail_fix_env;
    if (setenv("PATH", DEFAULT_PATH, 1))
        goto fail_fix_env;

    return;

fail_fix_env:

    log_line("Failed to sanitize environment.  Not exec'ing.\n");
    exit(EXIT_FAILURE);
}

void ncm_execute(char *command, char *args)
{
    static char *argv[MAX_ARGS];
    static int n;
    int m;
    char *p, *q;
    size_t len;

    /* free memory used on previous execution */
    for (m = 1; m < n; m++)
        free(argv[m]);
    n = 0;

    if (command == NULL)
        return;

    /* strip the path from the command name and store in cmdname */
    p = strrchr(command, '/');
    if (p != NULL) {
        argv[0] = ++p;
    } else {
        argv[0] = command;
    }

    /* decompose args into argv */
    p = args;
    for (n = 1;; p = strchr(p, ' '), n++) {
        if (p == NULL || n > (MAX_ARGS - 2)) {
            argv[n] = NULL;
            break;
        }
        if (n != 1)
            p++; /* skip the space */
        q = strchr(p, ' ');
        if (q == NULL)
            q = strchr(p, '\0');
        len = q - p + 1;
        argv[n] = malloc(len);

        if (argv[n] == NULL) {
            log_line("FATAL - malloc() failed in execute().\n");
            exit(EXIT_FAILURE);
        }

        strlcpy(argv[n], p, len);
    }

    execv(command, argv);
}

