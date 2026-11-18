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
#include "llimits.h"
#include "lmem.h"
#include "lstate.h"
#include "lzio.h"


int luaZ_fill (ZIO *z) {
  size_t size;
  lua_State *L = z->L;
  const char *buff;
  LuaWorldScope depth;
  LuaCallScope call;
  luaQ_begincall(L, z->managed, &call);
  depth = luaQ_entercall(L, &call);
  buff = z->reader(L, z->data, &size);
  luaQ_leavecall(L, &call, depth);
  luaQ_endcall(L, &call);
  if (buff == NULL || size == 0) {
    z->n = 0; z->p = NULL;
    return EOZ;
  }
  z->n = size - 1;  /* discount char being returned */
  z->p = buff;
  return cast_uchar(*(z->p++));
}


void luaZ_init (lua_State *L, ZIO *z, lua_Reader reader, void *data, int managed) {
  z->L = L;
  z->reader = reader;
  z->data = data;
  z->n = 0;
  z->p = NULL;
  z->managed = managed;
  z->work = 0;
}

/* Constructor state is visible to GC before a consumed-byte checkpoint parks. */
static void readwork (ZIO *z, size_t size) {
  while (size >= 1024u - z->work) {
    size -= 1024u - z->work;
    z->work = 0;
    luaX_poll(z->L);
  }
  z->work += cast_uint(size);
}

int luaZ_getc (ZIO *z) {
  int c;
  if (z->n != 0) {
    z->n--;
    c = cast_uchar(*(z->p++));
  }
  else c = luaZ_fill(z);
  if (c != EOZ) readwork(z, 1);
  return c;
}

/* --------------------------------------------------------------- read --- */

static int checkbuffer (ZIO *z) {
  if (z->n == 0) {  /* no bytes in buffer? */
    if (luaZ_fill(z) == EOZ)  /* try to read more */
      return 0;  /* no more input */
    else {
      z->n++;  /* luaZ_fill consumed first byte; put it back */
      z->p--;
    }
  }
  return 1;  /* now buffer has something */
}


size_t luaZ_read (ZIO *z, void *b, size_t n) {
  while (n) {
    size_t m;
    if (!checkbuffer(z))
      return n;  /* no more input; return number of missing bytes */
    m = (n <= z->n) ? n : z->n;  /* min. between n and z->n */
    if (m > 1024u) m = 1024u;
    memcpy(b, z->p, m);
    z->n -= m;
    z->p += m;
    b = (char *)b + m;
    n -= m;
    readwork(z, m);
  }
  return 0;
}


const void *luaZ_getaddr (ZIO* z, size_t n) {
  const void *res;
  if (!checkbuffer(z))
    return NULL;  /* no more input */
  if (z->n < n)  /* not enough bytes? */
    return NULL;  /* block not whole; cannot give an address */
  res = z->p;  /* get block address */
  z->n -= n;  /* consume these bytes */
  z->p += n;
  return res;
}
