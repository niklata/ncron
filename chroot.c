/*
 * chroot.c - chroots ncron jobs
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
#include <stdlib.h>

#include "log.h"

int imprison(char *path)
{
    int ret;

    if (path == NULL)
        return -1;

    ret = chdir(path);
    if (ret) {
        log_line("Failed to chdir(%s).  Not invoking job.", path);
        exit(EXIT_FAILURE);
    }

    ret = chroot(path);
    if (ret) {
        log_line("Failed to chroot(%s).  Not invoking job.", path);
        exit(EXIT_FAILURE);
    }
    chdir("/");
    return 0;
}

