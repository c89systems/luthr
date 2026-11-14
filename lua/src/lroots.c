/*
** Temporary native roots for shared-value snapshots.
** See Copyright Notice in lua.h
*/

#define lroots_c
#define LUA_CORE

#include "lprefix.h"
#include "lgc.h"
#include "lroots.h"

void luaR_pushroot (lua_State *L, LuaNativeRoot *root) {
  setnilvalue(&root->value);
  root->previous = L->native_roots;
  L->native_roots = root;
}

void luaR_setroot (lua_State *L, LuaNativeRoot *root, const TValue *value) {
  setobj(L, &root->value, value);
}

void luaR_poproot (lua_State *L, LuaNativeRoot *root) {
  lua_assert(L->native_roots == root);
  L->native_roots = root->previous;
}
