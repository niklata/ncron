/*
 * nstrl.c - strlcpy/strlcat implementation
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

#ifndef HAVE_STRLCPY
#define HAVE_STRLCPY 1

/*
 * I've written both array and pointer approaches.  Look at the generated asm
 * to pick which one best suits your compiler.  If you're using x86, however, I
 * highly suggest using the optimized forms.
 */

size_t strlcpy (char *dest, char *src, size_t size)
{
#if 1
    /*	register unsigned int i;

        for (i=0; size > 0 && src[i] != '\0'; ++i, size--)
        dest[i] = src[i];

        dest[i] = '\0';

        return i;*/
    register unsigned int i;
    char lar;

    lar = src[size];
    src[size] = '\0';
    for (i=0; src[i] != '\0'; ++i)
        dest[i] = src[i];

    src[size] = lar;
    dest[i] = '\0';

    return i;

#else
    register char *d = dest, *s = src;

    for (; size > 0 && *s != '\0'; size--, d++, s++)
        *d = *s;

    *d = '\0';
    return (d - dest) + (s - src);
#endif
}

size_t strlcat (char *dest, char *src, size_t size)
{
#if 0
    register unsigned int i, j;

    for(i=0; size > 0 && dest[i] != '\0'; size--, i++);
    for(j=0; size > 0 && src[i] != '\0'; size--, i++, j++)
        dest[i] = src[j];

    dest[i] = '\0';
    return i;
#else
    register char *d = dest, *s = src;

    for (; size > 0 && *d != '\0'; size--, d++);
    for (; size > 0 && *s != '\0'; size--, d++, s++)
        *d = *s;

    *d = '\0';
    return (d - dest) + (s - src);
#endif
}

#endif
