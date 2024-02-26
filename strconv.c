#include <stdlib.h>
#include <limits.h>
#include <errno.h>
#include "strconv.h"

bool strconv_to_u32(const char *str, const char *strend, uint32_t *val)
{
  char *endptr;
  unsigned long r = strtoul(str, &endptr, 10);
  if (r == ULONG_MAX && errno == ERANGE)
    return false;
  if (*str != '\0' && *endptr == *strend) {
    *val = r;
    return true;
  }
  return false;
}

bool strconv_to_u64(const char *str, const char *strend, uint64_t *val)
{
  char *endptr;
  unsigned long long r = strtoull(str, &endptr, 10);
  if (r == ULLONG_MAX && errno == ERANGE)
    return false;
  if (*str != '\0' && *endptr == *strend) {
    *val = r;
    return true;
  }
  return false;
}

bool strconv_to_i32(const char *str, const char *strend, int32_t *val)
{
  char *endptr;
  long r = strtol(str, &endptr, 10);
  if ((r == LONG_MAX || r == LONG_MIN) && errno == ERANGE)
    return false;
  if (*str != '\0' && *endptr == *strend) {
    *val = r;
    return true;
  }
  return false;
}

bool strconv_to_i64(const char *str, const char *strend, int64_t *val)
{
  char *endptr;
  long long r = strtoll(str, &endptr, 10);
  if ((r == LLONG_MAX || r == LLONG_MIN) && errno == ERANGE)
    return false;
  if (*str != '\0' && *endptr == *strend) {
    *val = r;
    return true;
  }
  return false;
}
