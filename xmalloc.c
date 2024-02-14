#include "xmalloc.h"
void *xmalloc(size_t s)
{
  void *r = malloc(s);
  if (!r) exit(EXIT_FAILURE);
  return r;
}
