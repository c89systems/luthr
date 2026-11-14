/*
** Temporary native roots for shared-value snapshots.
** See Copyright Notice in lua.h
*/

#ifndef lroots_h
#define lroots_h

#include "lstate.h"

/* These private operations do not allocate. The caller owns the Lua state and
** supplies stable native storage in LIFO order. The source of a snapshot must
** be protected while copied. A root does not synchronize its mutable referent.
** Protected unwinding removes scopes abandoned by errors, yields or self-close;
** normal return preserves roots backed by the caller's still-live storage.
** An unprotected throw abandons this state's scopes before forwarding/panic.
** A continuation needing longer ownership requires separate task/operation
** roots, not nodes added to this temporary chain during its invocation. */
LUAI_FUNC void luaR_pushroot (lua_State *L, LuaNativeRoot *root);
LUAI_FUNC void luaR_setroot (lua_State *L, LuaNativeRoot *root,
                          const TValue *value);
LUAI_FUNC void luaR_poproot (lua_State *L, LuaNativeRoot *root);

#endif
