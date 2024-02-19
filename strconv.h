#ifndef NCRON_STRCONV_H_
#define NCRON_STRCONV_H_
#include <stdint.h>
#include <stdbool.h>

bool strconv_to_u32(const char *str, const char *strend, uint32_t *val);
bool strconv_to_u64(const char *str, const char *strend, uint64_t *val);
bool strconv_to_i32(const char *str, const char *strend, int32_t *val);
bool strconv_to_i64(const char *str, const char *strend, int64_t *val);

#endif
