/*
 * defines.h - general #defines for ncron
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

/* Files where we store cronjob information. */
#define CONFIG_FILE_DEFAULT "/var/lib/ncron/crontab"
#define EXEC_FILE_DEFAULT "/var/lib/ncron/exectimes"
#define PID_FILE_DEFAULT "/var/run/ncron.pid"
#define LOG_FILE_DEFAULT "/var/log/ncron.log"

#define DEFAULT_PATH "/bin:/usr/bin:/usr/local/bin"

#define NCRON_VERSION "0.99"
#define MAX_PATH_LENGTH 512

#define MAXLINE 512
#define MAX_LOG_LINE 256
#define MAX_ARGS 30

/* 32-bit signed */
#define MAXINT 0x7fffffff

#define HAVE_STRL 1

