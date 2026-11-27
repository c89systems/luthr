/*
** $Id: lundump.h $
** load precompiled Lua chunks
** See Copyright Notice in lua.h
*/

#ifndef lundump_h
#define lundump_h

#include <limits.h>

#include "llimits.h"
#include "lobject.h"
#include "lzio.h"


/* data to catch conversion errors */
#define LUAC_DATA	"\x19\x93\r\n\x1a\n"

#define LUAC_INT	-0x5678
#define LUAC_INST	0x12345678
#define LUAC_NUM	cast_num(-370.5)

/*
** Encode major-minor version in one byte, one nibble for each
*/
#define LUAC_VERSION	(LUA_VERSION_MAJOR_N*16+LUA_VERSION_MINOR_N)

#define LUAC_FORMAT	0	/* this is the official format */


/* load one chunk; from lundump.c */
struct LuaNativeRoot;
LUAI_FUNC LClosure* luaU_undump (lua_State* L, ZIO* Z,
                                 struct LuaNativeRoot *strings,
                                 struct LuaNativeRoot *function,
                                 const char* name, int fixed);

/* Dump one chunk. 'managed' is reserved for the trusted runtime buffer writer;
** public/foreign writers always pass zero and retain native admission. */
LUAI_FUNC int luaU_dump (lua_State* L, const Proto* f, lua_Writer w,
                         void* data, int strip, int managed);

#endif
