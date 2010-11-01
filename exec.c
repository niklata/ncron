/*
 * exec.c - functions to exec a job for ncron
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

