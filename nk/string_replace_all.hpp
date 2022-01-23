#ifndef __NKFT_STRING_REPLACE_ALL_
#define __NKFT_STRING_REPLACE_ALL_

#include <string>

static inline void string_replace_all(std::string &s, const char *from,
                                      size_t fromlen, const char *to)
{
    size_t pos{0};
    while ((pos = s.find(from, pos)) != std::string::npos) {
        s.replace(pos, fromlen, to);
        pos += fromlen;
    }
}

#endif

