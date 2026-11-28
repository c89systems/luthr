/* Stack operations under managed admission. See lua.h for license. */
#ifndef lapiowned_h
#define lapiowned_h

#include "laccess.h"
#include "lapi.h"
#include "lroots.h"
#include "lstring.h"
#include "ltable.h"
#include "lvm.h"

/* Push a root for the value at an API index. The caller pops the root. */
LUAI_FUNC TValue *luaA_indexvalue (lua_State *L, int idx, LuaNativeRoot *root);
LUAI_FUNC int luaA_testnamedudata (lua_State *L, int idx, const char *name,
                                  void **result);
LUAI_FUNC void luaA_setnamedmeta (lua_State *L, const char *name);
/* Replace a trusted library's lightuserdata buffer placeholder. */
LUAI_FUNC void luaA_finishbufferstring (lua_State *L, const char *s, size_t len);

l_sinline void *luaA_udatapointer (const TValue *o) {
  switch (ttype(o)) {
    case LUA_TUSERDATA: return getudatamem(uvalue(o));
    case LUA_TLIGHTUSERDATA: return pvalue(o);
    default: return NULL;
  }
}

l_sinline int luaA_pushmetatable (lua_State *L, const TValue *object) {
  LuaNativeRoot mt;
  int found;
  luaR_pushroot(L, &mt);
  luaA_getmetatable(L, object, &mt);
  found = !ttisnil(&mt.value);
  if (found) {
    setobj2s(L, L->top.p, &mt.value);
    api_incr_top(L);
  }
  luaR_poproot(L, &mt);
  return found;
}

l_sinline int luaA_pushstringfield (lua_State *L, const TValue *t,
                                    TString *str) {
  lu_byte tag;
  luaV_fastget(t, str, s2v(L->top.p), luaH_getstr, tag);
  if (!luaV_needsindex(tag))
    api_incr_top(L);
  else {
    setsvalue2s(L, L->top.p, str);
    api_incr_top(L);
    tag = luaV_finishget(L, t, s2v(L->top.p - 1), L->top.p - 1);
  }
  return novariant(tag);
}

l_sinline int luaA_pushfield (lua_State *L, const TValue *t, const char *k) {
  LuaNativeRoot object;
  int tag;
  luaR_pushroot(L, &object);
  luaR_setroot(L, &object, t);
  tag = luaA_pushstringfield(L, &object.value, luaS_new(L, k));
  luaR_poproot(L, &object);
  return tag;
}

#endif
