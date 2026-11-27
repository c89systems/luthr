/*
** $Id: lzio.c $
** Buffered streams
** See Copyright Notice in lua.h
*/

#define lzio_c
#define LUA_CORE

#include "lprefix.h"


#include <string.h>

#include "lua.h"

#include "lapi.h"
#include "lcall.h"
#include "llimits.h"
#include "lmem.h"
#include "lstate.h"
#include "lzio.h"


enum { INPUT_POLL_BYTES = 1024 };

static int readchunk (ZIO *z) {
  size_t size;
  lua_State *L = z->L;
  const char *buff;
  LuaCallScope call;
  luaQ_begincall(L, z->managed, &call);
  luaQ_entercall(L, &call);
  buff = z->reader(L, z->data, &size);
  luaQ_leavecall(L, &call);
  if (buff == NULL || size == 0) return 0;
  z->remaining = size;
  z->p = buff;
  return 1;
}


void luaZ_init (lua_State *L, ZIO *z, lua_Reader reader, void *data, int managed) {
  z->L = L;
  z->reader = reader;
  z->data = data;
  z->n = 0;
  z->remaining = 0;
  z->p = NULL;
  z->managed = managed;
  z->budget = INPUT_POLL_BYTES;
}

/* Expose only bytes before the next checkpoint. The checkpoint byte uses
** the refill path, even when it is the last byte a parser consumes. */
int luaZ_fill (ZIO *z) {
  size_t count;
  int c;
  lua_assert(z->n == 0);
  if (z->remaining == 0 && !readchunk(z)) return EOZ;
  c = cast_uchar(*z->p++);
  if (z->budget == 1) {
    --z->remaining;
    z->budget = INPUT_POLL_BYTES;
    luaX_poll(z->L);
    return c;
  }
  count = z->budget - 1;
  if (count > z->remaining) count = z->remaining;
  z->n = count - 1;  /* first byte is the return value */
  z->remaining -= count;
  z->budget -= cast_uint(count);
  return c;
}

/* --------------------------------------------------------------- read --- */

size_t luaZ_read (ZIO *z, void *b, size_t n) {
  while (n) {
    size_t m;
    if (z->n == 0) {
      int c = luaZ_fill(z);
      if (c == EOZ) return n;
      *(unsigned char *)b = cast_uchar(c);
      b = (char *)b + 1;
      --n;
      continue;
    }
    m = (n <= z->n) ? n : z->n;  /* min. between n and z->n */
    memcpy(b, z->p, m);
    z->n -= m;
    z->p += m;
    b = (char *)b + m;
    n -= m;
  }
  return 0;
}


const void *luaZ_getaddr (ZIO* z, size_t n) {
  const void *res;
  size_t window;
  if (z->n == 0 && z->remaining == 0 && !readchunk(z))
    return NULL;  /* no more input */
  if (z->n + z->remaining < n)
    return NULL;  /* block not whole; cannot give an address */
  res = z->p;  /* get block address */
  /* Fixed buffer slices do not contribute parser work. Refund any bytes
  ** that had already been issued to the current read window. */
  window = n < z->n ? n : z->n;
  z->n -= window;
  z->remaining -= n - window;
  z->budget += cast_uint(window);
  z->p += n;
  return res;
}
