/* pidfile.c - process id file functions
 *
 * Copyright 2003-2015 Nicholas J. Kain <njkain at gmail dot com>
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

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include "nk/pidfile.h"
#include "nk/log.h"
#include "nk/io.h"

void write_pid(const char * const file) {
    int fd = open(file, O_WRONLY|O_CREAT|O_CLOEXEC, 00744);
    if (fd < 0)
        suicide("%s: open(%s) failed: %s", __func__, file, strerror(errno));
    pid_t pid = getpid();
    char wbuf[64];
    ssize_t r = snprintf(wbuf, sizeof wbuf, "%u", pid);
    if (r < 0 || (size_t)r >= sizeof wbuf)
        suicide("%s: snprintf(%s, %u) failed: %s", __func__, file, pid, strerror(errno));
    ssize_t written = safe_write(fd, wbuf, (size_t)r);
    if (written < 0 || written != r)
        suicide("%s: write(%s) failed: %s", __func__, file, strerror(errno));
    if (close(fd))
        suicide("%s: close(%s) failed: %s", __func__, file, strerror(errno));
}

