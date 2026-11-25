/* Native execution of bounded bytecode loops. See lua.h for license. */
#ifndef ljit_h
#define ljit_h

#include "lstate.h"

typedef struct LuaLoopFrame {
  lua_State *thread;
  CallInfo *call;
  StkId base;
  LClosure *closure;
  const Instruction *entry;
} LuaLoopFrame;

typedef unsigned (*LuaLoopCode)(LuaLoopFrame *frame);

enum { LUA_LOOP_COLD, LUA_LOOP_BUILDING, LUA_LOOP_UNAVAILABLE };

typedef struct LuaLoopSite {
  int pc;
  unsigned words;
  unsigned hits;
  uintptr_t entry;  /* atomic: cold, compiling, unavailable, or executable */
  void *allocation;  /* owns the executable bytes and their alignment pad */
  size_t allocsize;
  size_t codesize;  /* whole, protected pages within the allocation */
} LuaLoopSite;

LUAI_FUNC void luaJ_lower (lua_State *L, Proto *p);
LUAI_FUNC void luaJ_freeloops (lua_State *L, Proto *p, int retire);
LUAI_FUNC LuaLoopSite *luaJ_loopat (const Proto *p, int pc);
LUAI_FUNC LuaLoopCode luaJ_prepare (lua_State *L, Proto *p,
                                    LuaLoopSite *site);
LUAI_FUNC int luaV_loopnewtable (LuaLoopFrame *frame, unsigned offset);
LUAI_FUNC int luaJ_emit (const Proto *p, const LuaLoopSite *site,
                        unsigned char *code, size_t capacity);

#endif
