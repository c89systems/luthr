/*
** Temporary native roots for shared-value snapshots.
** See Copyright Notice in lua.h
*/

#ifndef lroots_h
#define lroots_h

#include "lstate.h"
#include "lgc.h"

/* These private operations do not allocate. The caller owns the Lua state and
** supplies stable native storage in LIFO order. The source of a snapshot must
** be protected while copied. A root does not synchronize its mutable referent.
** Parallel callers hold managed participation and appropriate stack ownership;
** a detached native callback must reenter before modifying these private roots.
** Protected unwinding removes scopes abandoned by errors, yields or self-close;
** normal return preserves roots backed by the caller's still-live storage.
** An unprotected throw abandons this state's scopes before forwarding/panic.
** A continuation needing longer ownership requires separate task/operation
** roots, not nodes added to this temporary chain during its invocation. */
l_sinline void luaR_pushroot (lua_State *L, LuaNativeRoot *root) {
  setnilvalue(&root->value);
  root->table_view = NULL;
  root->previous = L->native_roots;
  L->native_roots = root;
}

/* Copy a value retained by an already-covered strong edge. */
l_sinline void luaR_copyroot (lua_State *L, LuaNativeRoot *root,
                             const TValue *value) {
  root->table_view = NULL;
  setobj(L, &root->value, value);
}

l_sinline void luaR_setroot (lua_State *L, LuaNativeRoot *root,
                            const TValue *value) {
  luaR_copyroot(L, root, value);
  luaC_tracevalue(L, &root->value);
}

l_sinline void luaR_poproot (lua_State *L, LuaNativeRoot *root) {
  lua_assert(L->native_roots == root);
  L->native_roots = root->previous;
}

#endif
