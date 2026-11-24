/*
** $Id: lstrlib.c $
** Standard library for string operations and pattern-matching
** See Copyright Notice in lua.h
*/

#define lstrlib_c
#define LUA_LIB

#include "lprefix.h"


#include <ctype.h>
#include <float.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lua.h"

#include "lauxlib.h"
#include "lualib.h"
#include "llimits.h"
#include "lapi.h"
#include "laccess.h"
#include "lauxowned.h"
#include "lbuiltin.h"
#include "ldo.h"
#include "lexec.h"
#include "lfunc.h"
#include "lgc.h"
#include "lstring.h"
#include "lundump.h"
#include "lvm.h"


/*
** maximum number of captures that a pattern can do during
** pattern-matching. This limit is arbitrary, but must fit in
** an unsigned char.
*/
#if !defined(LUA_MAXCAPTURES)
#define LUA_MAXCAPTURES		32
#endif

/* Byte work returns from libc before parking can move this activation. */
static void copybytes (lua_State *L, unsigned *work, char *dest,
                       const char *src, size_t len) {
  while (len != 0) {
    size_t chunk = len < 1024u ? len : 1024u;
    memcpy(dest, src, chunk);
    luaQ_workn(L, work, chunk);
    dest += chunk; src += chunk; len -= chunk;
  }
}

static void addstring (lua_State *L, unsigned *work, luaL_Buffer *b,
                       const char *src, size_t len) {
  while (len != 0) {
    size_t chunk = len < 1024u ? len : 1024u;
    luaL_addlstring(b, src, chunk);
    luaQ_workn(L, work, chunk);
    src += chunk; len -= chunk;
  }
}

/* Caller owns the stack and roots src. Long copies may park and move it. */
static void pushownedstring (lua_State *L, unsigned *work,
                             const char *src, size_t len) {
  if (len <= LUAI_MAXSHORTLEN) {
    TString *ts = luaS_newlstr(L, src, len);
    setsvalue2s(L, L->top.p, ts);
    api_incr_top(L);
    luaC_checkGC(L);
  }
  else {
    LuaNativeRoot result;
    TString *ts = luaS_createlngstrobj(L, len, 1);
    luaR_pushroot(L, &result);
    setsvalue(L, &result.value, ts);
    lua_unlock(L);
    luaS_buildlongstr(L, ts, src, work);
    lua_lock(L);
    setobj2s(L, L->top.p, &result.value);
    api_incr_top(L);
    luaC_checkGC(L);
    luaR_poproot(L, &result);
  }
}

static void pushstring (lua_State *L, unsigned *work,
                        const char *src, size_t len) {
  if (len <= LUAI_MAXSHORTLEN) luaQ_workn(L, work, len);
  lua_lock(L);
  pushownedstring(L, work, src, len);
  lua_unlock(L);
}

static void addpadding (lua_State *L, unsigned *work, luaL_Buffer *b,
                        size_t len, int byte) {
  while (len != 0) {
    size_t chunk = len < 1024u ? len : 1024u;
    char *dest = luaL_prepbuffsize(b, chunk);
    memset(dest, byte, chunk);
    luaL_addsize(b, chunk);
    luaQ_workn(L, work, chunk);
    len -= chunk;
  }
}

/* Returns len when no zero occurs in the retained byte range. */
static size_t findzero (lua_State *L, unsigned *work,
                        const char *src, size_t len) {
  size_t offset = 0;
  while (offset < len) {
    size_t chunk = len - offset < 1024u ? len - offset : 1024u;
    const char *end = (const char *)memchr(src + offset, 0, chunk);
    size_t used = end != NULL ? ct_diff2sz(end - src) - offset + 1 : chunk;
    luaQ_workn(L, work, used);
    if (end != NULL) return ct_diff2sz(end - src);
    offset += chunk;
  }
  return len;
}


static int str_len (lua_State *L) {
  size_t l;
  luaL_checklstring(L, 1, &l);
  lua_pushinteger(L, (lua_Integer)l);
  return 1;
}


/* Read a position from the owned frame. Use the public checker to report
** coercion errors after releasing the gate, allowing traceback callbacks. */
static lua_Integer readposition (lua_State *L, int arg, int optional,
                                 lua_Integer def) {
  if (arg < L->top.p - L->ci->func.p) {
    const TValue *value = s2v(L->ci->func.p + arg);
    lua_Integer position;
    if (optional && ttisnil(value)) return def;
    luaC_tracevalue(L, value);
    if (tointeger(value, &position)) return position;
  }
  else if (optional)
    return def;
  lua_unlock(L);
  def = luaL_checkinteger(L, arg);
  lua_lock(L);
  return def;
}

/* Return the rooted source and decoded range with the stack still owned. */
static const char *readrange (lua_State *L, LuaNativeRoot *root, int bytes,
                              size_t *start, size_t *end) {
  const char *source;
  size_t len;
  lua_Integer first, last;
  lua_lock(L);
  source = luaL_ownedrootstring(L, 1, root, &len);
  first = readposition(L, 2, bytes, 1);
  last = readposition(L, 3, 1, bytes ? first : -1);
  *start = luaB_firstpos(first, len);
  *end = luaB_lastpos(last, len);
  return source;
}

int luaB_strsub (lua_State *L) {
  LuaNativeRoot root;
  unsigned work = 0;
  size_t start, end;
  const char *source = readrange(L, &root, 0, &start, &end);
  if (start <= end)
    pushownedstring(L, &work, source + start - 1, (end - start) + 1);
  else
    pushownedstring(L, &work, "", 0);
  luaR_poproot(L, &root);
  lua_unlock(L);
  return 1;
}


typedef struct StringResult {
  LuaNativeRoot root;
  char small[LUAI_MAXSHORTLEN];
} StringResult;

/* Known-size transforms write long results in their final allocation.
** Only the collector can see the unfinished result through this root. */
static char *startresult (lua_State *L, StringResult *result, size_t len) {
  TString *string;
  if (len <= LUAI_MAXSHORTLEN) return result->small;
  lua_lock(L);
  string = luaS_createlngstrobj(L, len, 1);
  luaR_pushroot(L, &result->root);
  setsvalue(L, &result->root.value, string);
  lua_unlock(L);
  return getlngstr(string);
}

static void pushresult (lua_State *L, StringResult *result, size_t len,
                         unsigned *work) {
  TString *string = NULL;
  if (len > LUAI_MAXSHORTLEN) {
    string = tsvalue(&result->root.value);
    luaS_buildlongstr(L, string, NULL, work);
  }
  lua_lock(L);
  if (string == NULL) string = luaS_newlstr(L, result->small, len);
  setsvalue2s(L, L->top.p, string);
  api_incr_top(L);
  luaC_checkGC(L);
  if (len > LUAI_MAXSHORTLEN) luaR_poproot(L, &result->root);
  lua_unlock(L);
}

static int str_reverse (lua_State *L) {
  LuaNativeRoot root;
  StringResult result;
  unsigned work = 0;
  size_t l, i;
  const char *s = luaL_rootstring(L, 1, &root, &l);
  char *p = startresult(L, &result, l);
  for (i = 0; i < l; i++) {
    p[i] = s[l - i - 1];
    luaQ_work(L, &work);
  }
  pushresult(L, &result, l, &work);
  return luaL_rootresult(L, &root, 1, 1);
}

/* Ctype may retain carrier-local locale tables. Never park inside this leaf. */
static void __attribute__((noinline)) changecase (char *dest, const char *src,
                                                size_t len, int upper) {
  size_t i;
  for (i = 0; i < len; ++i)
    dest[i] = cast_char(upper ? toupper(cast_uchar(src[i]))
                             : tolower(cast_uchar(src[i])));
}

static int str_case (lua_State *L, int upper) {
  LuaNativeRoot root;
  StringResult result;
  unsigned work = 0;
  size_t l;
  size_t i;
  const char *s = luaL_rootstring(L, 1, &root, &l);
  char *p = startresult(L, &result, l);
  for (i = 0; i < l;) {
    size_t chunk = l - i < 1024u ? l - i : 1024u;
    changecase(p + i, s + i, chunk, upper);
    luaQ_workn(L, &work, chunk);
    i += chunk;
  }
  pushresult(L, &result, l, &work);
  return luaL_rootresult(L, &root, 1, 1);
}

static int str_lower (lua_State *L) { return str_case(L, 0); }

static int str_upper (lua_State *L) { return str_case(L, 1); }


/*
** MAX_SIZE is limited both by size_t and lua_Integer.
** When x <= MAX_SIZE, x can be safely cast to size_t or lua_Integer.
*/
static int str_rep (lua_State *L) {
  LuaNativeRoot roots[2];
  unsigned nroots = 1;
  unsigned work = 0;
  size_t len, lsep;
  const char *s = luaL_rootstring(L, 1, &roots[0], &len);
  lua_Integer n = luaL_checkinteger(L, 2);
  const char *sep;
  if (lua_isnoneornil(L, 3)) { sep = ""; lsep = 0; }
  else {
    sep = luaL_rootstring(L, 3, &roots[1], &lsep);
    nroots = 2;
  }
  if (n <= 0 || (len | lsep) == 0)
    lua_pushliteral(L, "");  /* no repetitions or both strings empty */
  else if (l_unlikely(len > MAX_SIZE - lsep ||
               cast_st2S(len + lsep) > cast_st2S(MAX_SIZE) / n))
    return luaL_error(L, "resulting string too large");
  else {
    size_t totallen = (cast_sizet(n) * (len + lsep)) - lsep;
    size_t written = len;
    StringResult result;
    char *p = startresult(L, &result, totallen);
    copybytes(L, &work, p, s, len);
    if (n > 1 && lsep != 0) {
      copybytes(L, &work, p + len, sep, lsep);
      written += lsep;
    }
    /* Duplicate the initialized prefix. The final copy omits the trailing
    ** separator by stopping at the exact result length. */
    while (written < totallen) {
      size_t remaining = totallen - written;
      size_t chunk = written < remaining ? written : remaining;
      copybytes(L, &work, p + written, p, chunk);
      written += chunk;
    }
    pushresult(L, &result, totallen, &work);
  }
  return luaL_rootresult(L, roots, nroots, 1);
}


int luaB_strbyte (lua_State *L) {
  LuaNativeRoot root;
  unsigned work = 0;
  size_t start, end;
  const char *source = readrange(L, &root, 1, &start, &end);
  int n = 0, i = 0;
  if (start <= end) {
    if (l_unlikely(end - start >= (size_t)INT_MAX)) {
      lua_unlock(L);
      return luaL_error(L, "string slice too long");
    }
    n = cast_int(end - start) + 1;
  }
  if (n > L->ci->top.p - L->top.p) {
    lua_unlock(L);
    luaL_checkstack(L, n, "string slice too long");
    lua_lock(L);
  }
  while (i < n) {
    int chunk = n - i < 1024 ? n - i : 1024;
    int limit = i + chunk;
    for (; i < limit; i++) {
      setivalue(s2v(L->top.p), cast_uchar(source[start - 1 + cast_uint(i)]));
      api_incr_top(L);
    }
    if (chunk == 1024) {
      lua_unlock(L);
      luaQ_workn(L, &work, 1024);
      lua_lock(L);
    }
  }
  luaR_poproot(L, &root);
  lua_unlock(L);
  return n;
}


static int str_char (lua_State *L) {
  unsigned work = 0;
  int n = lua_gettop(L);  /* number of arguments */
  int i;
  luaL_Buffer b;
  char *p = luaL_buffinitsize(L, &b, cast_uint(n));
  for (i=1; i<=n; i++) {
    lua_Unsigned c = (lua_Unsigned)luaL_checkinteger(L, i);
    luaL_argcheck(L, c <= (lua_Unsigned)UCHAR_MAX, i, "value out of range");
    p[i - 1] = cast_char(cast_uchar(c));
    luaQ_work(L, &work);
  }
  luaL_pushresultsize(&b, cast_uint(n));
  return 1;
}


/*
** Buffer to store the result of 'string.dump'. Initialized by the first
** trusted writer call after selecting and rooting the dumped function.
*/
struct str_Writer {
  int init;  /* true iff buffer has been initialized */
  unsigned work;
  luaL_Buffer B;
};


static int writer (lua_State *L, const void *b, size_t size, void *ud) {
  struct str_Writer *state = (struct str_Writer *)ud;
  if (!state->init) {
    state->init = 1;
    luaL_buffinit(L, &state->B);
  }
  if (b == NULL) {  /* finishing dump? */
    luaL_pushresult(&state->B);  /* push result */
    lua_replace(L, 1);  /* move it to reserved slot */
  }
  else
    addstring(L, &state->work, &state->B, (const char *)b, size);
  return 0;
}


static int str_dump (lua_State *L) {
  LuaNativeRoot function;
  struct str_Writer state;
  int strip = lua_toboolean(L, 2);
  luaL_rootarg(L, 1, &function);
  luaL_argcheck(L, ttisLclosure(&function.value), 1, "Lua function expected");
  state.init = 0;
  state.work = 0;
  lua_lock(L);
  luaU_dump(L, clLvalue(&function.value)->p, writer, &state, strip, 1);
  lua_unlock(L);
  lua_settop(L, 1);  /* leave final result on top */
  return luaL_rootresult(L, &function, 1, 1);
}



/*
** {======================================================
** METAMETHODS
** =======================================================
*/

#if defined(LUA_NOCVTS2N)	/* { */

/* no coercion from strings to numbers */

static const luaL_Reg stringmetamethods[] = {
  {"__index", NULL},
  {NULL, NULL}
};

#else		/* }{ */

static int tonum (lua_State *L, const LuaNativeRoot *value) {
  if (ttisnumber(&value->value)) {  /* already a number? */
    luaL_pushrooted(L, value);
    return 1;
  }
  else if (ttisstring(&value->value)) {
    size_t len;
    const char *s = getlstr(tsvalue(&value->value), len);
    return lua_stringtonumber(L, s) == len + 1;
  }
  return 0;
}


/*
** To be here, either the first operand was a string or the first
** operand didn't have a corresponding metamethod. (Otherwise, that
** other metamethod would have been called.) So, if this metamethod
** doesn't work, the only other option would be for the second
** operand to have a different metamethod.
*/
static void trymt (lua_State *L, const char *mtkey, const char *opname) {
  lua_settop(L, 2);  /* back to the original arguments */
  if (l_unlikely(lua_type(L, 2) == LUA_TSTRING ||
                 !luaL_getmetafield(L, 2, mtkey)))
    luaL_error(L, "attempt to %s a '%s' with a '%s'", opname,
                  luaL_typename(L, -2), luaL_typename(L, -1));
  lua_insert(L, -3);  /* put metamethod before arguments */
  lua_call(L, 2, 1);  /* call metamethod */
}


static int arith (lua_State *L, int op, const char *mtname) {
  LuaNativeRoot roots[2];
  luaL_rootarg(L, 1, &roots[0]);
  luaL_rootarg(L, 2, &roots[1]);
  if (tonum(L, &roots[0]) && tonum(L, &roots[1]))
    lua_arith(L, op);  /* result will be on the top */
  else {
    lua_settop(L, 0);
    luaL_pushrooted(L, &roots[0]);
    luaL_pushrooted(L, &roots[1]);
    trymt(L, mtname, mtname + 2);
  }
  return luaL_rootresult(L, roots, 2, 1);
}


static int arith_add (lua_State *L) {
  return arith(L, LUA_OPADD, "__add");
}

static int arith_sub (lua_State *L) {
  return arith(L, LUA_OPSUB, "__sub");
}

static int arith_mul (lua_State *L) {
  return arith(L, LUA_OPMUL, "__mul");
}

static int arith_mod (lua_State *L) {
  return arith(L, LUA_OPMOD, "__mod");
}

static int arith_pow (lua_State *L) {
  return arith(L, LUA_OPPOW, "__pow");
}

static int arith_div (lua_State *L) {
  return arith(L, LUA_OPDIV, "__div");
}

static int arith_idiv (lua_State *L) {
  return arith(L, LUA_OPIDIV, "__idiv");
}

static int arith_unm (lua_State *L) {
  return arith(L, LUA_OPUNM, "__unm");
}


static const luaL_Reg stringmetamethods[] = {
  /* Insert the method lookup first so it occupies its main hash position. */
  {"__index", NULL},
  {"__add", arith_add},
  {"__sub", arith_sub},
  {"__mul", arith_mul},
  {"__mod", arith_mod},
  {"__pow", arith_pow},
  {"__div", arith_div},
  {"__idiv", arith_idiv},
  {"__unm", arith_unm},
  {NULL, NULL}
};

#endif		/* } */

/* }====================================================== */

/*
** {======================================================
** PATTERN MATCHING
** =======================================================
*/


#define CAP_UNFINISHED	(-1)
#define CAP_POSITION	(-2)


typedef struct MatchState {
  const char *src_init;  /* init of source string */
  const char *src_end;  /* end ('\0') of source string */
  const char *p_end;  /* end ('\0') of pattern */
  lua_State *L;
  uintptr_t stacklimit;  /* native bound of this matching invocation */
  int matchdepth;  /* control for recursive depth (to avoid C stack overflow) */
  int level;  /* total number of captures (finished or unfinished) */
  unsigned work;  /* invocation-local CPU work across retries/backtracking */
  struct {
    const char *init;
    ptrdiff_t len;  /* length or special value (CAP_*) */
  } capture[LUA_MAXCAPTURES];
} MatchState;


/* recursive function */
static const char *match (MatchState *ms, const char *s, const char *p);


/* maximum recursion depth for 'match' */
#if !defined(MAXCCALLS)
#define MAXCCALLS	200
#endif


#define L_ESC		'%'
#define SPECIALS	"^$*+?.([%-"


static int check_capture (MatchState *ms, int l) {
  l -= '1';
  if (l_unlikely(l < 0 || l >= ms->level ||
                 ms->capture[l].len == CAP_UNFINISHED))
    return luaL_error(ms->L, "invalid capture index %%%d", l + 1);
  return l;
}


static int capture_to_close (MatchState *ms) {
  int level = ms->level;
  for (level--; level>=0; level--)
    if (ms->capture[level].len == CAP_UNFINISHED) return level;
  return luaL_error(ms->L, "invalid pattern capture");
}


static const char *classend (MatchState *ms, const char *p) {
  switch (*p++) {
    case L_ESC: {
      if (l_unlikely(p == ms->p_end))
        luaL_error(ms->L, "malformed pattern (ends with '%%')");
      return p+1;
    }
    case '[': {
      if (*p == '^') p++;
      do {  /* look for a ']' */
        luaQ_work(ms->L, &ms->work);
        if (l_unlikely(p == ms->p_end))
          luaL_error(ms->L, "malformed pattern (missing ']')");
        if (*(p++) == L_ESC && p < ms->p_end)
          p++;  /* skip escapes (e.g. '%]') */
      } while (*p != ']');
      return p+1;
    }
    default: {
      return p;
    }
  }
}


/* Keep libc's ctype TLS inside a leaf that returns before any checkpoint. */
static int __attribute__((noinline)) match_class (int c, int cl) {
  int res;
  switch (tolower(cl)) {
    case 'a' : res = isalpha(c); break;
    case 'c' : res = iscntrl(c); break;
    case 'd' : res = isdigit(c); break;
    case 'g' : res = isgraph(c); break;
    case 'l' : res = islower(c); break;
    case 'p' : res = ispunct(c); break;
    case 's' : res = isspace(c); break;
    case 'u' : res = isupper(c); break;
    case 'w' : res = isalnum(c); break;
    case 'x' : res = isxdigit(c); break;
    case 'z' : res = (c == 0); break;  /* deprecated option */
    default: return (cl == c);
  }
  return (islower(cl) ? res : !res);
}


static int matchbracketclass (MatchState *ms, int c, const char *p,
                              const char *ec) {
  int sig = 1;
  if (*(p+1) == '^') {
    sig = 0;
    p++;  /* skip the '^' */
  }
  while (++p < ec) {
    luaQ_work(ms->L, &ms->work);
    if (*p == L_ESC) {
      p++;
      if (match_class(c, cast_uchar(*p)))
        return sig;
    }
    else if ((*(p+1) == '-') && (p+2 < ec)) {
      p+=2;
      if (cast_uchar(*(p-2)) <= c && c <= cast_uchar(*p))
        return sig;
    }
    else if (cast_uchar(*p) == c) return sig;
  }
  return !sig;
}


static int singlematch (MatchState *ms, const char *s, const char *p,
                        const char *ep) {
  if (s >= ms->src_end)
    return 0;
  else {
    int c = cast_uchar(*s);
    switch (*p) {
      case '.': return 1;  /* matches any char */
      case L_ESC: return match_class(c, cast_uchar(*(p+1)));
      case '[': return matchbracketclass(ms, c, p, ep-1);
      default:  return (cast_uchar(*p) == c);
    }
  }
}


static const char *matchbalance (MatchState *ms, const char *s,
                                   const char *p) {
  if (l_unlikely(p >= ms->p_end - 1))
    luaL_error(ms->L, "malformed pattern (missing arguments to '%%b')");
  if (*s != *p) return NULL;
  else {
    int b = *p;
    int e = *(p+1);
    int cont = 1;
    while (++s < ms->src_end) {
      luaQ_work(ms->L, &ms->work);
      if (*s == e) {
        if (--cont == 0) return s+1;
      }
      else if (*s == b) cont++;
    }
  }
  return NULL;  /* string ends out of balance */
}


static const char *max_expand (MatchState *ms, const char *s,
                                 const char *p, const char *ep) {
  ptrdiff_t i = 0;  /* counts maximum expand for item */
  while (singlematch(ms, s + i, p, ep)) {
    i++;
    luaQ_work(ms->L, &ms->work);
  }
  /* keeps trying to match with the maximum repetitions */
  while (i>=0) {
    const char *res = match(ms, (s+i), ep+1);
    if (res) return res;
    i--;  /* else didn't match; reduce 1 repetition to try again */
  }
  return NULL;
}


static const char *min_expand (MatchState *ms, const char *s,
                                 const char *p, const char *ep) {
  for (;;) {
    const char *res = match(ms, s, ep+1);
    if (res != NULL)
      return res;
    else if (singlematch(ms, s, p, ep))
      s++;  /* try with one more repetition */
    else return NULL;
  }
}


static const char *start_capture (MatchState *ms, const char *s,
                                    const char *p, int what) {
  const char *res;
  int level = ms->level;
  if (level >= LUA_MAXCAPTURES) luaL_error(ms->L, "too many captures");
  ms->capture[level].init = s;
  ms->capture[level].len = what;
  ms->level = level+1;
  if ((res=match(ms, s, p)) == NULL)  /* match failed? */
    ms->level--;  /* undo capture */
  return res;
}


static const char *end_capture (MatchState *ms, const char *s,
                                  const char *p) {
  int l = capture_to_close(ms);
  const char *res;
  ms->capture[l].len = s - ms->capture[l].init;  /* close capture */
  if ((res = match(ms, s, p)) == NULL)  /* match failed? */
    ms->capture[l].len = CAP_UNFINISHED;  /* undo capture */
  return res;
}


static int matchbytes (MatchState *ms, const char *a, const char *b, size_t len) {
  while (len != 0) {
    size_t chunk = len < 1024u ? len : 1024u;
    int different = memcmp(a, b, chunk);
    luaQ_workn(ms->L, &ms->work, chunk);
    if (different) return 0;
    a += chunk; b += chunk; len -= chunk;
  }
  return 1;
}

static const char *match_capture (MatchState *ms, const char *s, int l) {
  size_t len;
  l = check_capture(ms, l);
  len = cast_sizet(ms->capture[l].len);
  if ((size_t)(ms->src_end-s) >= len &&
      matchbytes(ms, ms->capture[l].init, s, len))
    return s+len;
  else return NULL;
}


static const char *match (MatchState *ms, const char *s, const char *p) {
  /* Pattern recursion also runs outside interpreter C-call accounting. */
  if (l_unlikely(luaD_nativeposition() <= ms->stacklimit)) {
    lua_lock(ms->L);
    luaD_checknativestack(ms->L);
    lua_unlock(ms->L);
  }
  if (l_unlikely(ms->matchdepth-- == 0))
    luaL_error(ms->L, "pattern too complex");
  init: /* using goto to optimize tail recursion */
  luaQ_work(ms->L, &ms->work);
  if (p != ms->p_end) {  /* end of pattern? */
    switch (*p) {
      case '(': {  /* start capture */
        if (*(p + 1) == ')')  /* position capture? */
          s = start_capture(ms, s, p + 2, CAP_POSITION);
        else
          s = start_capture(ms, s, p + 1, CAP_UNFINISHED);
        break;
      }
      case ')': {  /* end capture */
        s = end_capture(ms, s, p + 1);
        break;
      }
      case '$': {
        if ((p + 1) != ms->p_end)  /* is the '$' the last char in pattern? */
          goto dflt;  /* no; go to default */
        s = (s == ms->src_end) ? s : NULL;  /* check end of string */
        break;
      }
      case L_ESC: {  /* escaped sequences not in the format class[*+?-]? */
        switch (*(p + 1)) {
          case 'b': {  /* balanced string? */
            s = matchbalance(ms, s, p + 2);
            if (s != NULL) {
              p += 4; goto init;  /* return match(ms, s, p + 4); */
            }  /* else fail (s == NULL) */
            break;
          }
          case 'f': {  /* frontier? */
            const char *ep; char previous;
            p += 2;
            if (l_unlikely(*p != '['))
              luaL_error(ms->L, "missing '[' after '%%f' in pattern");
            ep = classend(ms, p);  /* points to what is next */
            previous = (s == ms->src_init) ? '\0' : *(s - 1);
            if (!matchbracketclass(ms, cast_uchar(previous), p, ep - 1) &&
               matchbracketclass(ms, cast_uchar(*s), p, ep - 1)) {
              p = ep; goto init;  /* return match(ms, s, ep); */
            }
            s = NULL;  /* match failed */
            break;
          }
          case '0': case '1': case '2': case '3':
          case '4': case '5': case '6': case '7':
          case '8': case '9': {  /* capture results (%0-%9)? */
            s = match_capture(ms, s, cast_uchar(*(p + 1)));
            if (s != NULL) {
              p += 2; goto init;  /* return match(ms, s, p + 2) */
            }
            break;
          }
          default: goto dflt;
        }
        break;
      }
      default: dflt: {  /* pattern class plus optional suffix */
        const char *ep = classend(ms, p);  /* points to optional suffix */
        /* does not match at least once? */
        if (!singlematch(ms, s, p, ep)) {
          if (*ep == '*' || *ep == '?' || *ep == '-') {  /* accept empty? */
            p = ep + 1; goto init;  /* return match(ms, s, ep + 1); */
          }
          else  /* '+' or no suffix */
            s = NULL;  /* fail */
        }
        else {  /* matched once */
          switch (*ep) {  /* handle optional suffix */
            case '?': {  /* optional */
              const char *res;
              if ((res = match(ms, s + 1, ep + 1)) != NULL)
                s = res;
              else {
                p = ep + 1; goto init;  /* else return match(ms, s, ep + 1); */
              }
              break;
            }
            case '+':  /* 1 or more repetitions */
              s++;  /* 1 match already done */
              /* FALLTHROUGH */
            case '*':  /* 0 or more repetitions */
              s = max_expand(ms, s, p, ep);
              break;
            case '-':  /* 0 or more repetitions (minimum) */
              s = min_expand(ms, s, p, ep);
              break;
            default:  /* no suffix */
              s++; p = ep; goto init;  /* return match(ms, s + 1, ep); */
          }
        }
        break;
      }
    }
  }
  ms->matchdepth++;
  return s;
}



static const char *lmemfind (MatchState *ms, const char *s1, size_t l1,
                               const char *s2, size_t l2) {
  if (l2 == 0) return s1;  /* empty strings are everywhere */
  else if (l2 > l1) return NULL;  /* avoids a negative 'l1' */
  else {
    const char *init;  /* to search for a '*s2' inside 's1' */
    l2--;  /* 1st char will be checked by 'memchr' */
    l1 = l1-l2;  /* 's2' cannot be found after that */
    while (l1 > 0) {
      size_t chunk = l1 < 1024u ? l1 : 1024u;
      size_t used;
      init = (const char *)memchr(s1, *s2, chunk);
      used = init != NULL ? ct_diff2sz(init - s1) + 1 : chunk;
      luaQ_workn(ms->L, &ms->work, used);
      s1 += used; l1 -= used;
      if (init != NULL && matchbytes(ms, init + 1, s2 + 1, l2))
        return init;
    }
    return NULL;  /* not found */
  }
}


/*
** get information about the i-th capture. If there are no captures
** and 'i==0', return information about the whole match, which
** is the range 's'..'e'. If the capture is a string, return
** its length and put its address in '*cap'. If it is a position,
** put its address in '*cap' and return CAP_POSITION.
*/
static ptrdiff_t get_onecapture (MatchState *ms, int i, const char *s,
                                const char *e, const char **cap, int owned) {
  if (i >= ms->level) {
    if (l_unlikely(i != 0)) {
      if (owned) lua_unlock(ms->L);
      luaL_error(ms->L, "invalid capture index %%%d", i + 1);
    }
    *cap = s;
    return (e - s);
  }
  else {
    ptrdiff_t capl = ms->capture[i].len;
    *cap = ms->capture[i].init;
    if (l_unlikely(capl == CAP_UNFINISHED)) {
      if (owned) lua_unlock(ms->L);
      luaL_error(ms->L, "unfinished capture");
    }
    return capl;
  }
}

static void pushmatchposition (lua_State *L, lua_Integer position) {
  setivalue(s2v(L->top.p), position);
  api_incr_top(L);
}

static void pushownedcapture (MatchState *ms, int i, const char *s,
                              const char *e) {
  const char *cap;
  ptrdiff_t l = get_onecapture(ms, i, s, e, &cap, 1);
  if (l == CAP_POSITION)
    pushmatchposition(ms->L, ct_diff2S(cap - ms->src_init) + 1);
  else pushownedstring(ms->L, &ms->work, cap, cast_sizet(l));
}

static void push_onecapture (MatchState *ms, int i, const char *s,
                                                    const char *e) {
  lua_lock(ms->L);
  pushownedcapture(ms, i, s, e);
  lua_unlock(ms->L);
}

static int countcaptures (const MatchState *ms, const char *s) {
  return (ms->level == 0 && s != NULL) ? 1 : ms->level;
}

static void pushownedcaptures (MatchState *ms, const char *s,
                               const char *e, int count) {
  int i;
  for (i = 0; i < count; i++)
    pushownedcapture(ms, i, s, e);
}

static int push_captures (MatchState *ms, const char *s, const char *e) {
  int nlevels = countcaptures(ms, s);
  luaL_checkstack(ms->L, nlevels, "too many captures");
  lua_lock(ms->L);
  pushownedcaptures(ms, s, e, nlevels);
  lua_unlock(ms->L);
  return nlevels;  /* number of strings pushed */
}


/* check whether pattern has no special characters */
static int nospecials (MatchState *ms, const char *p, size_t l) {
  size_t upto;
  for (upto = 0; upto < l; ++upto) {
    luaQ_work(ms->L, &ms->work);
    if (p[upto] != '\0' && strchr(SPECIALS, p[upto]) != NULL)
      return 0;  /* pattern has a special character */
  }
  return 1;  /* no special chars found */
}


/*
** Prepare state for matches. These fields are not affected by each match.
*/
static void prepstate (MatchState *ms, lua_State *L,
                       const char *s, size_t ls, const char *p, size_t lp) {
  ms->L = L;
  ms->stacklimit = luaD_nativelimit(luaE_getnativestackcontext());
  ms->src_init = s;
  ms->src_end = s + ls;
  ms->p_end = p + lp;
  ms->work = 0;
}


/*
** (Re)prepare state for a match, setting fields that change during
** each match.
*/
static void reprepstate (MatchState *ms) {
  ms->matchdepth = MAXCCALLS;
  ms->level = 0;
}


static int findresult (MatchState *ms, LuaNativeRoot *roots,
                        const char *start, const char *end, int find) {
  lua_State *L = ms->L;
  int count = end != NULL ? countcaptures(ms, find ? NULL : start) : 0;
  int results = end != NULL ? count + 2 * find : 1;
  if (count != 0) luaL_checkstack(L, results, "too many captures");
  lua_lock(L);
  if (end == NULL) {
#if defined(LUA_FAILISFALSE)
    setbfvalue(s2v(L->top.p));
#else
    setnilvalue2s(L->top.p);
#endif
    api_incr_top(L);
  }
  else {
    if (find) {
      pushmatchposition(L, ct_diff2S(start - ms->src_init) + 1);
      pushmatchposition(L, ct_diff2S(end - ms->src_init));
    }
    pushownedcaptures(ms, start, end, count);
  }
  luaR_poproot(L, &roots[1]);
  luaR_poproot(L, &roots[0]);
  lua_unlock(L);
  return results;
}

static int str_find_aux (lua_State *L, int find) {
  LuaNativeRoot roots[2];
  MatchState ms;
  size_t ls, lp, init;
  const char *s, *p;
  int plain;
  lua_lock(L);
  s = luaL_ownedrootstring(L, 1, &roots[0], &ls);
  p = luaL_ownedrootstring(L, 2, &roots[1], &lp);
  init = luaB_firstpos(readposition(L, 3, 1, 1), ls) - 1;
  plain = find && L->top.p - L->ci->func.p > 4 &&
          !l_isfalse(s2v(L->ci->func.p + 4));
  lua_unlock(L);
  prepstate(&ms, L, s, ls, p, lp);
  if (init > ls)  /* start after string's end? */
    return findresult(&ms, roots, NULL, NULL, find);
  /* explicit request or no special characters? */
  if (find && (plain || nospecials(&ms, p, lp))) {
    /* do a plain search */
    const char *s2 = lmemfind(&ms, s + init, ls - init, p, lp);
    ms.level = 0;
    if (s2 != NULL) return findresult(&ms, roots, s2, s2 + lp, find);
  }
  else {
    const char *s1 = s + init;
    int anchor = (*p == '^');
    if (anchor) {
      p++; lp--;  /* skip anchor character */
    }
    do {
      const char *res;
      reprepstate(&ms);
      if ((res=match(&ms, s1, p)) != NULL)
        return findresult(&ms, roots, s1, res, find);
    } while (s1++ < ms.src_end && !anchor);
  }
  return findresult(&ms, roots, NULL, NULL, find);
}


static int str_find (lua_State *L) {
  return str_find_aux(L, 1);
}


static int str_match (lua_State *L) {
  return str_find_aux(L, 0);
}


/* state for 'gmatch' */
typedef struct GMatchState {
  const void *identity;
  TString *subject, *pattern;  /* identities only; user values hold the roots */
  size_t source_len, pattern_len;
  size_t next, last;
  lua_Unsigned version;
  int matched;
} GMatchState;

static const char gmatch_identity;

/* Payload ownership is held. Stored identities are never dereferenced: debug
** code can replace user values, but cannot make a stale input pointer safe. */
static GMatchState *gmatch_state (Udata *owner) {
  GMatchState *gm;
  if (owner->len != sizeof(GMatchState) || owner->nuvalue != 2)
    return NULL;
  gm = cast(GMatchState *, getudatamem(owner));
  if (gm->identity != &gmatch_identity ||
      !ttisstring(&owner->uv[0].uv) || !ttisstring(&owner->uv[1].uv) ||
      tsvalue(&owner->uv[0].uv) != gm->subject ||
      tsvalue(&owner->uv[1].uv) != gm->pattern ||
      tsslen(tsvalue(&owner->uv[0].uv)) != gm->source_len ||
      tsslen(tsvalue(&owner->uv[1].uv)) != gm->pattern_len ||
      gm->next > gm->source_len + 1 ||
      (gm->matched && gm->last > gm->source_len))
    return NULL;
  return gm;
}

static int badgmatch (lua_State *L, LuaObjectScope *scope) {
  if (scope != NULL) luaA_end(L, scope);
  lua_unlock(L);
  return luaL_error(L, "invalid gmatch iterator state");
}


static int gmatch_aux (lua_State *L) {
  LuaNativeRoot roots[3];  /* selected userdata, subject and pattern */
  LuaObjectScope scope;
  CClosure *function;
  Udata *owner;
  unsigned index, work = 0;
  MatchState ms;
  lua_lock(L);
  if (!ttisCclosure(s2v(L->ci->func.p)) ||
      clCvalue(s2v(L->ci->func.p))->nupvalues < 3)
    return badgmatch(L, NULL);
  for (index = 0; index < 3; ++index)
    luaR_pushroot(L, &roots[index]);
  function = clCvalue(s2v(L->ci->func.p));
  luaA_begin(L, obj2gco(function), &scope);
  luaR_setroot(L, &roots[0], &function->upvalue[2]);
  luaA_end(L, &scope);
  if (!ttisfulluserdata(&roots[0].value))
    return badgmatch(L, NULL);
  owner = uvalue(&roots[0].value);
  lua_unlock(L);
  for (;;) {
    GMatchState *gm;
    lua_Unsigned version;
    size_t ls, lp, next, last;
    int matched, committed;
    const char *s, *p, *src, *end = NULL;
    lua_lock(L);
    luaA_begin(L, obj2gco(owner), &scope);
    gm = gmatch_state(owner);
    if (gm == NULL) return badgmatch(L, &scope);
    luaR_setroot(L, &roots[1], &owner->uv[0].uv);
    luaR_setroot(L, &roots[2], &owner->uv[1].uv);
    s = getlstr(tsvalue(&roots[1].value), ls);
    p = getlstr(tsvalue(&roots[2].value), lp);
    next = gm->next; last = gm->last; matched = gm->matched;
    version = gm->version;
    luaA_end(L, &scope);
    lua_unlock(L);
    prepstate(&ms, L, s, ls, p, lp);
    ms.work = work;
    for (src = s + next; src <= ms.src_end; src++) {
      reprepstate(&ms);
      end = match(&ms, src, p);
      if (end != NULL && (!matched || ct_diff2sz(end - s) != last))
        break;
    }
    if (src > ms.src_end)
      return luaL_rootresult(L, roots, 3, 0);
    work = ms.work;
    lua_lock(L);
    luaA_begin(L, obj2gco(owner), &scope);
    gm = gmatch_state(owner);
    if (gm == NULL) return badgmatch(L, &scope);
    committed = gm->version == version;
    if (committed) {
      gm->next = gm->last = ct_diff2sz(end - s);
      gm->matched = 1;
      gm->version++;
    }
    luaA_end(L, &scope);
    lua_unlock(L);
    if (committed)
      return luaL_rootresult(L, roots, 3, push_captures(&ms, src, end));
    /* A peer consumed this position while matching ran outside ownership. */
  }
}


static int gmatch (lua_State *L) {
  LuaNativeRoot roots[3];
  size_t ls, lp;
  Udata *owner;
  CClosure *function;
  unsigned index;
  luaL_rootstring(L, 1, &roots[0], &ls);
  luaL_rootstring(L, 2, &roots[1], &lp);
  size_t init = luaB_firstpos(luaL_optinteger(L, 3, 1), ls) - 1;
  GMatchState *gm;
  if (init > ls)  /* start after string's end? */
    init = ls + 1;
  lua_lock(L);
  luaR_pushroot(L, &roots[2]);
  owner = luaS_newudata(L, sizeof(GMatchState), 2);
  setuvalue(L, &roots[2].value, owner);
  gm = cast(GMatchState *, getudatamem(owner));
  gm->identity = &gmatch_identity;
  gm->subject = tsvalue(&roots[0].value);
  gm->pattern = tsvalue(&roots[1].value);
  gm->source_len = ls; gm->pattern_len = lp;
  gm->next = init; gm->last = 0; gm->matched = 0; gm->version = 0;
  luaA_store(L, &owner->uv[0].uv, &roots[0].value);
  luaA_store(L, &owner->uv[1].uv, &roots[1].value);
  function = luaF_newCclosure(L, 3);
  function->f = gmatch_aux;
  function->managed = 1;
  for (index = 0; index < 3; ++index)
    luaA_store(L, &function->upvalue[index], &roots[index].value);
  L->top.p = L->ci->func.p + 1;
  setclCvalue(L, s2v(L->top.p), function);
  api_incr_top(L);
  for (index = 3; index != 0;)
    luaR_poproot(L, &roots[--index]);
  luaC_checkGC(L);
  lua_unlock(L);
  return 1;
}


static void addbytes (MatchState *ms, luaL_Buffer *b, const char *s, size_t len) {
  addstring(ms->L, &ms->work, b, s, len);
}

static void add_s (MatchState *ms, luaL_Buffer *b, const char *s,
                   const char *e, LuaNativeRoot *replacement) {
  size_t l;
  lua_State *L = ms->L;
  const char *news = luaL_roottostring(L, replacement, &l);
  const char *p;
  while (l != 0) {
    size_t chunk = l < 1024u ? l : 1024u;
    size_t prefix;
    p = (const char *)memchr(news, L_ESC, chunk);
    prefix = p != NULL ? ct_diff2sz(p - news) : chunk;
    luaQ_workn(L, &ms->work, p != NULL ? prefix + 1 : chunk);
    addbytes(ms, b, news, prefix);
    if (p == NULL) {
      news += chunk; l -= chunk;
      continue;
    }
    p++;  /* skip ESC */
    if (*p == L_ESC)  /* '%%' */
      luaL_addchar(b, *p);
    else if (*p == '0')  /* '%0' */
        addbytes(ms, b, s, ct_diff2sz(e - s));
    else if (*p >= '1' && *p <= '9') {  /* '%n', independent of libc TLS */
      const char *cap;
      ptrdiff_t resl = get_onecapture(ms, *p - '1', s, e, &cap, 0);
      if (resl == CAP_POSITION) {
        LuaNativeRoot position;
        size_t len;
        const char *value;
        lua_pushinteger(L, ct_diff2S(cap - ms->src_init) + 1);
        luaL_rootarg(L, lua_gettop(L), &position);
        lua_pop(L, 1);  /* Restore the buffer box before a possible wait. */
        value = luaL_roottostring(L, &position, &len);
        addbytes(ms, b, value, len);
        luaL_rootresult(L, &position, 1, 0);
      }
      else
        addbytes(ms, b, cap, cast_sizet(resl));
    }
    else
      luaL_error(L, "invalid use of '%c' in replacement string", L_ESC);
    l -= ct_diff2sz(p + 1 - news);
    news = p + 1;
  }
}


/*
** Add the replacement value to the string buffer 'b'.
** Return true if the original string was changed. (Function calls and
** table indexing resulting in nil or false do not change the subject.)
*/
static int add_value (MatchState *ms, luaL_Buffer *b, const char *s,
                      const char *e, int tr, LuaNativeRoot *replacement) {
  lua_State *L = ms->L;
  LuaNativeRoot result;
  int changed;
  switch (tr) {
    case LUA_TFUNCTION: {  /* call the function */
      int n;
      luaL_pushrooted(L, replacement);
      n = push_captures(ms, s, e);  /* all captures as arguments */
      lua_call(L, n, 1);  /* call it */
      break;
    }
    case LUA_TTABLE: {  /* index the table */
      luaL_pushrooted(L, replacement);
      push_onecapture(ms, 0, s, e);  /* first capture is the index */
      lua_gettable(L, -2);
      lua_remove(L, -2);
      break;
    }
    default: {  /* LUA_TNUMBER or LUA_TSTRING */
      add_s(ms, b, s, e, replacement);
      return 1;  /* something changed */
    }
  }
  luaL_rootarg(L, lua_gettop(L), &result);
  lua_pop(L, 1);  /* The buffer's box must again be the top stack value. */
  if (l_isfalse(&result.value)) {  /* nil or false? */
    addbytes(ms, b, s, ct_diff2sz(e - s));
    changed = 0;
  }
  else if (l_unlikely(!ttisstring(&result.value) &&
                     !cvt2str(&result.value)))
    return luaL_error(L, "invalid replacement value (a %s)",
                         lua_typename(L, ttype(&result.value)));
  else {
    size_t len;
    const char *value = luaL_roottostring(L, &result, &len);
    addbytes(ms, b, value, len);
    changed = 1;
  }
  luaL_rootresult(L, &result, 1, 0);
  return changed;
}


static int str_gsub (lua_State *L) {
  LuaNativeRoot roots[3];
  size_t srcl, lp;
  const char *src = luaL_rootstring(L, 1, &roots[0], &srcl);
  const char *p = luaL_rootstring(L, 2, &roots[1], &lp);
  const char *lastmatch = NULL;  /* end of last match */
  int tr;
  /* max replacements */
  lua_Integer max_s = luaL_optinteger(L, 4, cast_st2S(srcl) + 1);
  int anchor = (*p == '^');
  lua_Integer n = 0;  /* replacement count */
  int changed = 0;  /* change flag */
  MatchState ms;
  luaL_Buffer b;
  luaL_rootarg(L, 3, &roots[2]);
  tr = ttype(&roots[2].value);
  luaL_argexpected(L, tr == LUA_TNUMBER || tr == LUA_TSTRING ||
                   tr == LUA_TFUNCTION || tr == LUA_TTABLE, 3,
                      "string/function/table");
  luaL_buffinit(L, &b);
  if (anchor) {
    p++; lp--;  /* skip anchor character */
  }
  prepstate(&ms, L, src, srcl, p, lp);
  while (n < max_s) {
    const char *e;
    reprepstate(&ms);  /* (re)prepare state for new match */
    if ((e = match(&ms, src, p)) != NULL && e != lastmatch) {  /* match? */
      n++;
      changed = add_value(&ms, &b, src, e, tr, &roots[2]) || changed;
      src = lastmatch = e;
    }
    else if (src < ms.src_end)  /* otherwise, skip one character */
      luaL_addchar(&b, *src++);
    else break;  /* end of subject */
    if (anchor) break;
  }
  if (!changed)  /* no changes? */
    luaL_pushrooted(L, &roots[0]);  /* return the original subject snapshot */
  else {  /* something changed */
    addbytes(&ms, &b, src, ct_diff2sz(ms.src_end - src));
    luaL_pushresult(&b);  /* create and return new string */
  }
  lua_pushinteger(L, n);  /* number of substitutions */
  return luaL_rootresult(L, roots, 3, 2);
}

/* }====================================================== */



/*
** {======================================================
** STRING FORMAT
** =======================================================
*/

#if !defined(lua_number2strx)	/* { */

/*
** Hexadecimal floating-point formatter
*/

#define SIZELENMOD	(sizeof(LUA_NUMBER_FRMLEN)/sizeof(char))


/*
** Number of bits that goes into the first digit. It can be any value
** between 1 and 4; the following definition tries to align the number
** to nibble boundaries by making what is left after that first digit a
** multiple of 4.
*/
#define L_NBFD		((l_floatatt(MANT_DIG) - 1)%4 + 1)


/*
** Add integer part of 'x' to buffer and return new 'x'
*/
static lua_Number adddigit (char *buff, unsigned n, lua_Number x) {
  lua_Number dd = l_mathop(floor)(x);  /* get integer part from 'x' */
  int d = (int)dd;
  buff[n] = cast_char(d < 10 ? d + '0' : d - 10 + 'a');  /* add to buffer */
  return x - dd;  /* return what is left */
}


static int num2straux (char *buff, unsigned sz, lua_Number x) {
  /* if 'inf' or 'NaN', format it like '%g' */
  if (x != x || x == (lua_Number)HUGE_VAL || x == -(lua_Number)HUGE_VAL)
    return l_sprintf(buff, sz, LUA_NUMBER_FMT, (LUAI_UACNUMBER)x);
  else if (x == 0) {  /* can be -0... */
    /* create "0" or "-0" followed by exponent */
    return l_sprintf(buff, sz, LUA_NUMBER_FMT "x0p+0", (LUAI_UACNUMBER)x);
  }
  else {
    int e;
    lua_Number m = l_mathop(frexp)(x, &e);  /* 'x' fraction and exponent */
    unsigned n = 0;  /* character count */
    if (m < 0) {  /* is number negative? */
      buff[n++] = '-';  /* add sign */
      m = -m;  /* make it positive */
    }
    buff[n++] = '0'; buff[n++] = 'x';  /* add "0x" */
    m = adddigit(buff, n++, m * (1 << L_NBFD));  /* add first digit */
    e -= L_NBFD;  /* this digit goes before the radix point */
    if (m > 0) {  /* more digits? */
      buff[n++] = lua_getlocaledecpoint();  /* add radix point */
      do {  /* add as many digits as needed */
        m = adddigit(buff, n++, m * 16);
      } while (m > 0);
    }
    n += cast_uint(l_sprintf(buff + n, sz - n, "p%+d", e));  /* add exponent */
    lua_assert(n < sz);
    return cast_int(n);
  }
}


static int lua_number2strx (lua_State *L, char *buff, unsigned sz,
                            const char *fmt, lua_Number x) {
  int n = num2straux(buff, sz, x);
  if (fmt[SIZELENMOD] == 'A') {
    int i;
    for (i = 0; i < n; i++)
      buff[i] = cast_char(toupper(cast_uchar(buff[i])));
  }
  else if (l_unlikely(fmt[SIZELENMOD] != 'a'))
    return luaL_error(L, "modifiers for format '%%a'/'%%A' not implemented");
  return n;
}

#endif				/* } */


/*
** Maximum size for items formatted with '%f'. This size is produced
** by format('%.99f', -maxfloat), and is equal to 99 + 3 ('-', '.',
** and '\0') + number of decimal digits to represent maxfloat (which
** is maximum exponent + 1). (99+3+1, adding some extra, 110)
*/
#define MAX_ITEMF	(110 + l_floatatt(MAX_10_EXP))


/*
** All formats except '%f' do not need that large limit.  The other
** float formats use exponents, so that they fit in the 99 limit for
** significant digits; 's' for large strings and 'q' add items directly
** to the buffer; all integer formats also fit in the 99 limit.  The
** worst case are floats: they may need 99 significant digits, plus
** '0x', '-', '.', 'e+XXXX', and '\0'. Adding some extra, 120.
*/
#define MAX_ITEM	120


/* valid flags in a format specification */
#if !defined(L_FMTFLAGSF)

/* valid flags for a, A, e, E, f, F, g, and G conversions */
#define L_FMTFLAGSF	"-+#0 "

/* valid flags for o, x, and X conversions */
#define L_FMTFLAGSX	"-#0"

/* valid flags for d and i conversions */
#define L_FMTFLAGSI	"-+0 "

/* valid flags for u conversions */
#define L_FMTFLAGSU	"-0"

/* valid flags for c, p, and s conversions */
#define L_FMTFLAGSC	"-"

#endif


/*
** Maximum size of each format specification (such as "%-099.99d"):
** Initial '%', flags (up to 5), width (2), period, precision (2),
** length modifier (8), conversion specifier, and final '\0', plus some
** extra.
*/
#define MAX_FORMAT	32


static unsigned __attribute__((noinline)) quotedbyte (char *out,
                                                     int c, int next) {
  if (c == '"' || c == '\\' || c == '\n') {
    out[0] = '\\'; out[1] = cast_char(c);
    return 2;
  }
  if (iscntrl(c))
    return cast_uint(l_sprintf(out, 10, isdigit(next) ? "\\%03d" : "\\%d", c));
  out[0] = cast_char(c);
  return 1;
}

static void addquoted (lua_State *L, unsigned *work, luaL_Buffer *b,
                       const char *s, size_t len) {
  luaL_addchar(b, '"');
  while (len--) {
    char buff[10];
    unsigned size = quotedbyte(buff, cast_uchar(*s), cast_uchar(*(s+1)));
    unsigned i;
    for (i = 0; i < size; ++i)
      luaL_addchar(b, buff[i]);
    luaQ_workn(L, work, size);
    s++;
  }
  luaL_addchar(b, '"');
}


/*
** Serialize a floating-point number in such a way that it can be
** scanned back by Lua. Use hexadecimal format for "common" numbers
** (to preserve precision); inf, -inf, and NaN are handled separately.
** (NaN cannot be expressed as a numeral, so we write '(0/0)' for it.)
*/
static int quotefloat (lua_State *L, char *buff, lua_Number n) {
  const char *s;  /* for the fixed representations */
  if (n == (lua_Number)HUGE_VAL)  /* inf? */
    s = "1e9999";
  else if (n == -(lua_Number)HUGE_VAL)  /* -inf? */
    s = "-1e9999";
  else if (n != n)  /* NaN? */
    s = "(0/0)";
  else {  /* format number as hexadecimal */
    int  nb = lua_number2strx(L, buff, MAX_ITEM,
                                 "%" LUA_NUMBER_FRMLEN "a", n);
    /* ensures that 'buff' string uses a dot as the radix character */
    if (memchr(buff, '.', cast_uint(nb)) == NULL) {  /* no dot? */
      char point = lua_getlocaledecpoint();  /* try locale point */
      char *ppoint = (char *)memchr(buff, point, cast_uint(nb));
      if (ppoint) *ppoint = '.';  /* change it to a dot */
    }
    return nb;
  }
  /* for the fixed representations */
  return l_sprintf(buff, MAX_ITEM, "%s", s);
}


/* Snapshot the string produced by __tostring before restoring the buffer box.
** Neither the original argument nor a replaceable temporary holds its lifetime. */
static const char *formatstring (lua_State *L, const LuaNativeRoot *value,
                                 LuaNativeRoot *result, size_t *len) {
  luaL_pushrooted(L, value);
  luaL_tolstring(L, lua_gettop(L), NULL);
  luaL_rootarg(L, lua_gettop(L), result);
  lua_pop(L, 2);
  return luaL_roottostring(L, result, len);
}

static void addliteral (lua_State *L, unsigned *work, luaL_Buffer *b, int arg,
                        LuaNativeRoot *value) {
  switch (ttype(&value->value)) {
    case LUA_TSTRING: {
      size_t len;
      const char *s = getlstr(tsvalue(&value->value), len);
      addquoted(L, work, b, s, len);
      break;
    }
    case LUA_TNUMBER: {
      char *buff = luaL_prepbuffsize(b, MAX_ITEM);
      int nb;
      if (!ttisinteger(&value->value))  /* float? */
        nb = quotefloat(L, buff, fltvalue(&value->value));
      else {  /* integers */
        lua_Integer n = ivalue(&value->value);
        if (n == LUA_MININTEGER)  /* its decimal literal would parse as float */
          nb = l_sprintf(buff, MAX_ITEM, "0x%" LUA_INTEGER_FRMLEN "x",
                         (LUA_UNSIGNED)n);
        else
          nb = cast_int(luaO_todecimal(buff, l_castS2U(n), n < 0));
      }
      luaL_addsize(b, cast_uint(nb));
      break;
    }
    case LUA_TNIL: case LUA_TBOOLEAN: {
      LuaNativeRoot result;
      size_t len;
      const char *s = formatstring(L, value, &result, &len);
      addstring(L, work, b, s, len);
      luaL_rootresult(L, &result, 1, 0);
      break;
    }
    default: {
      luaL_argerror(L, arg, "value has no literal form");
    }
  }
}


static const char *get2digits (const char *s) {
  if (isdigit(cast_uchar(*s))) {
    s++;
    if (isdigit(cast_uchar(*s))) s++;  /* (2 digits at most) */
  }
  return s;
}


/*
** Check whether a conversion specification is valid. When called,
** first character in 'form' must be '%' and last character must
** be a valid conversion specifier. 'flags' are the accepted flags;
** 'precision' signals whether to accept a precision.
*/
static void __attribute__((noinline)) checkformat (lua_State *L,
                                       const char *form, const char *flags,
                                       int precision) {
  const char *spec = form + 1;  /* skip '%' */
  spec += strspn(spec, flags);  /* skip flags */
  if (*spec != '0') {  /* a width cannot start with '0' */
    spec = get2digits(spec);  /* skip width */
    if (*spec == '.' && precision) {
      spec++;
      spec = get2digits(spec);  /* skip precision */
    }
  }
  if (!isalpha(cast_uchar(*spec)))  /* did not go to the end? */
    luaL_error(L, "invalid conversion specification: '%s'", form);
}


/*
** Get a conversion specification and copy it to 'form'.
** Return the address of its last character.
*/
static const char *getformat (lua_State *L, const char *strfrmt,
                                            char *form) {
  /* spans flags, width, and precision ('0' is included as a flag) */
  size_t len = 0;
  while (len < MAX_FORMAT && strfrmt[len] != '\0' &&
         strchr(L_FMTFLAGSF "123456789.", strfrmt[len]) != NULL)
    len++;
  len++;  /* adds following character (should be the specifier) */
  /* still needs space for '%', '\0', plus a length modifier */
  if (len >= MAX_FORMAT - 10)
    luaL_error(L, "invalid format (too long)");
  *(form++) = '%';
  memcpy(form, strfrmt, len * sizeof(char));
  *(form + len) = '\0';
  return strfrmt + len - 1;
}


/*
** add length modifier into formats
*/
static void addlenmod (char *form, const char *lenmod) {
  size_t l = strlen(form);
  size_t lm = strlen(lenmod);
  char spec = form[l - 1];
  strcpy(form + l - 1, lenmod);
  form[l + lm - 1] = spec;
  form[l + lm] = '\0';
}


static lua_Integer formatinteger (lua_State *L, int arg, const TValue *value) {
  return ttisinteger(value) ? ivalue(value) : luaL_checkinteger(L, arg);
}

static int str_format (lua_State *L) {
  LuaNativeRoot format;
  unsigned work = 0;
  int top = lua_gettop(L);
  int arg = 1;
  size_t sfl;
  const char *strfrmt = luaL_rootstring(L, arg, &format, &sfl);
  const char *strfrmt_end = strfrmt+sfl;
  const char *flags;
  luaL_Buffer b;
  luaL_buffinit(L, &b);
  while (strfrmt < strfrmt_end) {
    if (*strfrmt != L_ESC)
      luaL_addchar(&b, *strfrmt++);
    else if (*++strfrmt == L_ESC)
      luaL_addchar(&b, *strfrmt++);  /* %% */
    else { /* format item */
      LuaNativeRoot value;
      char form[MAX_FORMAT];  /* to store the format ('%...') */
      unsigned maxitem = MAX_ITEM;  /* maximum length for the result */
      char *buff = luaL_prepbuffsize(&b, maxitem);  /* to put result */
      int nb = 0;  /* number of bytes in result */
      if (++arg > top)
        return luaL_argerror(L, arg, "no value");
      luaL_rootarg(L, arg, &value);
      strfrmt = getformat(L, strfrmt, form);
      switch (*strfrmt++) {
        case 'c': {
          checkformat(L, form, L_FMTFLAGSC, 0);
          nb = l_sprintf(buff, maxitem, form,
                          (int)formatinteger(L, arg, &value.value));
          break;
        }
        case 'd': case 'i':
          flags = L_FMTFLAGSI;
          goto intcase;
        case 'u':
          flags = L_FMTFLAGSU;
          goto intcase;
        case 'o': case 'x': case 'X':
          flags = L_FMTFLAGSX;
         intcase: {
          lua_Integer n = formatinteger(L, arg, &value.value);
          if (form[2] == '\0' &&
              (form[1] == 'd' || form[1] == 'i' || form[1] == 'u'))
            nb = cast_int(luaO_todecimal(buff, l_castS2U(n),
                                       n < 0 && form[1] != 'u'));
          else {
            checkformat(L, form, flags, 1);
            addlenmod(form, LUA_INTEGER_FRMLEN);
            nb = l_sprintf(buff, maxitem, form, (LUAI_UACINT)n);
          }
          break;
        }
        case 'a': case 'A':
          checkformat(L, form, L_FMTFLAGSF, 1);
          addlenmod(form, LUA_NUMBER_FRMLEN);
          nb = lua_number2strx(L, buff, maxitem, form,
                                  luaL_checknumber(L, arg));
          break;
        case 'f':
          maxitem = MAX_ITEMF;  /* extra space for '%f' */
          buff = luaL_prepbuffsize(&b, maxitem);
          /* FALLTHROUGH */
        case 'e': case 'E': case 'g': case 'G': {
          lua_Number n = luaL_checknumber(L, arg);
          checkformat(L, form, L_FMTFLAGSF, 1);
          addlenmod(form, LUA_NUMBER_FRMLEN);
          nb = l_sprintf(buff, maxitem, form, (LUAI_UACNUMBER)n);
          break;
        }
        case 'p': {
          const void *p;
          luaL_pushrooted(L, &value);
          p = lua_topointer(L, -1);
          lua_pop(L, 1);
          checkformat(L, form, L_FMTFLAGSC, 0);
          if (p == NULL) {  /* avoid calling 'printf' with argument NULL */
            p = "(null)";  /* result */
            form[strlen(form) - 1] = 's';  /* format it as a string */
          }
          nb = l_sprintf(buff, maxitem, form, p);
          break;
        }
        case 'q': {
          if (form[2] != '\0')  /* modifiers? */
            return luaL_error(L, "specifier '%%q' cannot have modifiers");
          addliteral(L, &work, &b, arg, &value);
          break;
        }
        case 's': {
          LuaNativeRoot result;
          size_t l;
          const char *s = formatstring(L, &value, &result, &l);
          if (form[2] == '\0')  /* no modifiers? */
            addstring(L, &work, &b, s, l);
          else {
            luaL_argcheck(L, findzero(L, &work, s, l) == l,
                             arg, "string contains zeros");
            checkformat(L, form, L_FMTFLAGSC, 1);
            if (strchr(form, '.') == NULL && l >= 100) {
              /* no precision and string is too long to be formatted */
              addstring(L, &work, &b, s, l);
            }
            else {  /* format the string into 'buff' */
              nb = l_sprintf(buff, maxitem, form, s);
            }
          }
          luaL_rootresult(L, &result, 1, 0);
          break;
        }
        default: {  /* also treat cases 'pnLlh' */
          return luaL_error(L, "invalid conversion '%s' to 'format'", form);
        }
      }
      lua_assert(cast_uint(nb) < maxitem);
      luaL_addsize(&b, cast_uint(nb));
      luaL_rootresult(L, &value, 1, 0);
    }
    luaQ_work(L, &work);
  }
  luaL_pushresult(&b);
  return luaL_rootresult(L, &format, 1, 1);
}

/* }====================================================== */


/*
** {======================================================
** PACK/UNPACK
** =======================================================
*/


/* value used for padding */
#if !defined(LUAL_PACKPADBYTE)
#define LUAL_PACKPADBYTE		0x00
#endif

/* maximum size for the binary representation of an integer */
#define MAXINTSIZE	16

/* number of bits in a character */
#define NB	CHAR_BIT

/* mask for one character (NB 1's) */
#define MC	((1 << NB) - 1)

/* size of a lua_Integer */
#define SZINT	((int)sizeof(lua_Integer))


/* dummy union to get native endianness */
static const union {
  int dummy;
  char little;  /* true iff machine is little endian */
} nativeendian = {1};


/*
** information to pack/unpack stuff
*/
typedef struct Header {
  lua_State *L;
  int islittle;
  unsigned maxalign;
  unsigned work;
} Header;


/*
** options for pack/unpack
*/
typedef enum KOption {
  Kint,		/* signed integers */
  Kuint,	/* unsigned integers */
  Kfloat,	/* single-precision floating-point numbers */
  Knumber,	/* Lua "native" floating-point numbers */
  Kdouble,	/* double-precision floating-point numbers */
  Kchar,	/* fixed-length strings */
  Kstring,	/* strings with prefixed length */
  Kzstr,	/* zero-terminated strings */
  Kpadding,	/* padding */
  Kpaddalign,	/* padding for alignment */
  Knop		/* no-op (configuration or spaces) */
} KOption;


/*
** Read an integer numeral from string 'fmt' or return 'df' if
** there is no numeral
*/
static int digit (int c) { return '0' <= c && c <= '9'; }

static size_t getnum (const char **fmt, size_t df) {
  if (!digit(**fmt))  /* no number? */
    return df;  /* return default value */
  else {
    size_t a = 0;
    do {
      a = a*10 + cast_uint(*((*fmt)++) - '0');
    } while (digit(**fmt) && a <= (MAX_SIZE - 9)/10);
    return a;
  }
}


/*
** Read an integer numeral and raises an error if it is larger
** than the maximum size of integers.
*/
static unsigned getnumlimit (Header *h, const char **fmt, size_t df) {
  size_t sz = getnum(fmt, df);
  if (l_unlikely((sz - 1u) >= MAXINTSIZE))
    return cast_uint(luaL_error(h->L,
               "integral size (%d) out of limits [1,%d]", sz, MAXINTSIZE));
  return cast_uint(sz);
}


/*
** Initialize Header
*/
static void initheader (lua_State *L, Header *h) {
  h->L = L;
  h->islittle = nativeendian.little;
  h->maxalign = 1;
  h->work = 0;
}


/*
** Read and classify next option. 'size' is filled with option's size.
*/
static KOption getoption (Header *h, const char **fmt, size_t *size) {
  /* dummy structure to get native alignment requirements */
  struct cD { char c; union { LUAI_MAXALIGN; } u; };
  int opt = *((*fmt)++);
  *size = 0;  /* default */
  switch (opt) {
    case 'b': *size = sizeof(char); return Kint;
    case 'B': *size = sizeof(char); return Kuint;
    case 'h': *size = sizeof(short); return Kint;
    case 'H': *size = sizeof(short); return Kuint;
    case 'l': *size = sizeof(long); return Kint;
    case 'L': *size = sizeof(long); return Kuint;
    case 'j': *size = sizeof(lua_Integer); return Kint;
    case 'J': *size = sizeof(lua_Integer); return Kuint;
    case 'T': *size = sizeof(size_t); return Kuint;
    case 'f': *size = sizeof(float); return Kfloat;
    case 'n': *size = sizeof(lua_Number); return Knumber;
    case 'd': *size = sizeof(double); return Kdouble;
    case 'i': *size = getnumlimit(h, fmt, sizeof(int)); return Kint;
    case 'I': *size = getnumlimit(h, fmt, sizeof(int)); return Kuint;
    case 's': *size = getnumlimit(h, fmt, sizeof(size_t)); return Kstring;
    case 'c':
      *size = getnum(fmt, cast_sizet(-1));
      if (l_unlikely(*size == cast_sizet(-1)))
        luaL_error(h->L, "missing size for format option 'c'");
      return Kchar;
    case 'z': return Kzstr;
    case 'x': *size = 1; return Kpadding;
    case 'X': return Kpaddalign;
    case ' ': break;
    case '<': h->islittle = 1; break;
    case '>': h->islittle = 0; break;
    case '=': h->islittle = nativeendian.little; break;
    case '!': {
      const size_t maxalign = offsetof(struct cD, u);
      h->maxalign = getnumlimit(h, fmt, maxalign);
      break;
    }
    default: luaL_error(h->L, "invalid format option '%c'", opt);
  }
  return Knop;
}


/*
** Read, classify, and fill other details about the next option.
** 'psize' is filled with option's size, 'notoalign' with its
** alignment requirements.
** Local variable 'size' gets the size to be aligned. (Kpadal option
** always gets its full alignment, other options are limited by
** the maximum alignment ('maxalign'). Kchar option needs no alignment
** despite its size.
*/
static KOption getdetails (Header *h, size_t totalsize, const char **fmt,
                           size_t *psize, unsigned *ntoalign) {
  KOption opt = getoption(h, fmt, psize);
  size_t align = *psize;  /* usually, alignment follows size */
  if (opt == Kpaddalign) {  /* 'X' gets alignment from following option */
    if (**fmt == '\0' || getoption(h, fmt, &align) == Kchar || align == 0)
      luaL_argerror(h->L, 1, "invalid next option for option 'X'");
  }
  if (align <= 1 || opt == Kchar)  /* need no alignment? */
    *ntoalign = 0;
  else {
    if (align > h->maxalign)  /* enforce maximum alignment */
      align = h->maxalign;
    if (l_unlikely(!ispow2(align))) {  /* not a power of 2? */
      *ntoalign = 0;  /* to avoid warnings */
      luaL_argerror(h->L, 1, "format asks for alignment not power of 2");
    }
    else {
      /* 'szmoda' = totalsize % align */
      unsigned szmoda = cast_uint(totalsize & (align - 1));
      *ntoalign = cast_uint((align - szmoda) & (align - 1));
    }
  }
  return opt;
}


/*
** Pack integer 'n' with 'size' bytes and 'islittle' endianness.
** The final 'if' handles the case when 'size' is larger than
** the size of a Lua integer, correcting the extra sign-extension
** bytes if necessary (by default they would be zeros).
*/
static void packint (luaL_Buffer *b, lua_Unsigned n,
                     int islittle, unsigned size, int neg) {
  char *buff = luaL_prepbuffsize(b, size);
  unsigned i;
  buff[islittle ? 0 : size - 1] = (char)(n & MC);  /* first byte */
  for (i = 1; i < size; i++) {
    n >>= NB;
    buff[islittle ? i : size - 1 - i] = (char)(n & MC);
  }
  if (neg && size > SZINT) {  /* negative number need sign extension? */
    for (i = SZINT; i < size; i++)  /* correct extra bytes */
      buff[islittle ? i : size - 1 - i] = (char)MC;
  }
  luaL_addsize(b, size);  /* add result to buffer */
}


/*
** Copy 'size' bytes from 'src' to 'dest', correcting endianness if
** given 'islittle' is different from native endianness.
*/
static void copywithendian (char *dest, const char *src,
                            unsigned size, int islittle) {
  if (islittle == nativeendian.little)
    memcpy(dest, src, size);
  else {
    dest += size - 1;
    while (size-- != 0)
      *(dest--) = *(src++);
  }
}


static int str_pack (lua_State *L) {
  LuaNativeRoot format;
  size_t formatlen;
  luaL_Buffer b;
  Header h;
  const char *fmt = luaL_rootstring(L, 1, &format, &formatlen);
  int arg = 1;  /* current argument to pack */
  size_t totalsize = 0;  /* accumulate total size of result */
  initheader(L, &h);
  lua_pushnil(L);  /* mark to separate arguments from string buffer */
  luaL_buffinit(L, &b);
  while (*fmt != '\0') {
    unsigned ntoalign;
    size_t size;
    KOption opt = getdetails(&h, totalsize, &fmt, &size, &ntoalign);
    luaL_argcheck(L, size + ntoalign <= MAX_SIZE - totalsize, arg,
                     "result too long");
    totalsize += ntoalign + size;
    while (ntoalign-- > 0)
     luaL_addchar(&b, LUAL_PACKPADBYTE);  /* fill alignment */
    arg++;
    switch (opt) {
      case Kint: {  /* signed integers */
        lua_Integer n = luaL_checkinteger(L, arg);
        if (size < SZINT) {  /* need overflow check? */
          lua_Integer lim = (lua_Integer)1 << ((size * NB) - 1);
          luaL_argcheck(L, -lim <= n && n < lim, arg, "integer overflow");
        }
        packint(&b, (lua_Unsigned)n, h.islittle, cast_uint(size), (n < 0));
        break;
      }
      case Kuint: {  /* unsigned integers */
        lua_Integer n = luaL_checkinteger(L, arg);
        if (size < SZINT)  /* need overflow check? */
          luaL_argcheck(L, (lua_Unsigned)n < ((lua_Unsigned)1 << (size * NB)),
                           arg, "unsigned overflow");
        packint(&b, (lua_Unsigned)n, h.islittle, cast_uint(size), 0);
        break;
      }
      case Kfloat: {  /* C float */
        float f = (float)luaL_checknumber(L, arg);  /* get argument */
        char *buff = luaL_prepbuffsize(&b, sizeof(f));
        /* move 'f' to final result, correcting endianness if needed */
        copywithendian(buff, (char *)&f, sizeof(f), h.islittle);
        luaL_addsize(&b, size);
        break;
      }
      case Knumber: {  /* Lua float */
        lua_Number f = luaL_checknumber(L, arg);  /* get argument */
        char *buff = luaL_prepbuffsize(&b, sizeof(f));
        /* move 'f' to final result, correcting endianness if needed */
        copywithendian(buff, (char *)&f, sizeof(f), h.islittle);
        luaL_addsize(&b, size);
        break;
      }
      case Kdouble: {  /* C double */
        double f = (double)luaL_checknumber(L, arg);  /* get argument */
        char *buff = luaL_prepbuffsize(&b, sizeof(f));
        /* move 'f' to final result, correcting endianness if needed */
        copywithendian(buff, (char *)&f, sizeof(f), h.islittle);
        luaL_addsize(&b, size);
        break;
      }
      case Kchar: {  /* fixed-size string */
        LuaNativeRoot field;
        size_t len;
        const char *s = luaL_rootstring(L, arg, &field, &len);
        luaL_argcheck(L, len <= size, arg, "string longer than given size");
        addstring(L, &h.work, &b, s, len);
        if (len < size) {  /* does it need padding? */
          addpadding(L, &h.work, &b, size - len, LUAL_PACKPADBYTE);
        }
        luaL_rootresult(L, &field, 1, 0);
        break;
      }
      case Kstring: {  /* strings with length count */
        LuaNativeRoot field;
        size_t len;
        const char *s = luaL_rootstring(L, arg, &field, &len);
        luaL_argcheck(L, size >= sizeof(lua_Unsigned) ||
                         len < ((lua_Unsigned)1 << (size * NB)),
                         arg, "string length does not fit in given size");
        /* pack length */
        packint(&b, (lua_Unsigned)len, h.islittle, cast_uint(size), 0);
        addstring(L, &h.work, &b, s, len);
        totalsize += len;
        luaL_rootresult(L, &field, 1, 0);
        break;
      }
      case Kzstr: {  /* zero-terminated string */
        LuaNativeRoot field;
        size_t len;
        const char *s = luaL_rootstring(L, arg, &field, &len);
        luaL_argcheck(L, findzero(L, &h.work, s, len) == len,
                         arg, "string contains zeros");
        addstring(L, &h.work, &b, s, len);
        luaL_addchar(&b, '\0');  /* add zero at the end */
        totalsize += len + 1;
        luaL_rootresult(L, &field, 1, 0);
        break;
      }
      case Kpadding: luaL_addchar(&b, LUAL_PACKPADBYTE);  /* FALLTHROUGH */
      case Kpaddalign: case Knop:
        arg--;  /* undo increment */
        break;
    }
    luaQ_work(L, &h.work);
  }
  luaL_pushresult(&b);
  return luaL_rootresult(L, &format, 1, 1);
}


static int str_packsize (lua_State *L) {
  LuaNativeRoot format;
  size_t formatlen;
  Header h;
  const char *fmt = luaL_rootstring(L, 1, &format, &formatlen);
  size_t totalsize = 0;  /* accumulate total size of result */
  initheader(L, &h);
  while (*fmt != '\0') {
    unsigned ntoalign;
    size_t size;
    KOption opt = getdetails(&h, totalsize, &fmt, &size, &ntoalign);
    luaL_argcheck(L, opt != Kstring && opt != Kzstr, 1,
                     "variable-length format");
    size += ntoalign;  /* total space used by option */
    luaL_argcheck(L, totalsize <= MAX_SIZE - size,
                     1, "format result too large");
    totalsize += size;
    luaQ_work(L, &h.work);
  }
  lua_pushinteger(L, cast_st2S(totalsize));
  return luaL_rootresult(L, &format, 1, 1);
}


/*
** Unpack an integer with 'size' bytes and 'islittle' endianness.
** If size is smaller than the size of a Lua integer and integer
** is signed, must do sign extension (propagating the sign to the
** higher bits); if size is larger than the size of a Lua integer,
** it must check the unread bytes to see whether they do not cause an
** overflow.
*/
static lua_Integer unpackint (lua_State *L, const char *str,
                              int islittle, int size, int issigned) {
  lua_Unsigned res = 0;
  int i;
  int limit = (size  <= SZINT) ? size : SZINT;
  for (i = limit - 1; i >= 0; i--) {
    res <<= NB;
    res |= (lua_Unsigned)(unsigned char)str[islittle ? i : size - 1 - i];
  }
  if (size < SZINT) {  /* real size smaller than lua_Integer? */
    if (issigned) {  /* needs sign extension? */
      lua_Unsigned mask = (lua_Unsigned)1 << (size*NB - 1);
      res = ((res ^ mask) - mask);  /* do sign extension */
    }
  }
  else if (size > SZINT) {  /* must check unread bytes */
    int mask = (!issigned || (lua_Integer)res >= 0) ? 0 : MC;
    for (i = limit; i < size; i++) {
      if (l_unlikely((unsigned char)str[islittle ? i : size - 1 - i] != mask))
        luaL_error(L, "%d-byte integer does not fit into Lua Integer", size);
    }
  }
  return (lua_Integer)res;
}


static int str_unpack (lua_State *L) {
  LuaNativeRoot roots[2];
  size_t formatlen;
  Header h;
  const char *fmt = luaL_rootstring(L, 1, &roots[0], &formatlen);
  size_t ld;
  const char *data = luaL_rootstring(L, 2, &roots[1], &ld);
  size_t pos = luaB_firstpos(luaL_optinteger(L, 3, 1), ld) - 1;
  int n = 0;  /* number of results */
  luaL_argcheck(L, pos <= ld, 3, "initial position out of string");
  initheader(L, &h);
  while (*fmt != '\0') {
    unsigned ntoalign;
    size_t size;
    KOption opt = getdetails(&h, pos, &fmt, &size, &ntoalign);
    luaL_argcheck(L, ntoalign + size <= ld - pos, 2,
                    "data string too short");
    pos += ntoalign;  /* skip alignment */
    /* stack space for item + next position */
    luaL_checkstack(L, 2, "too many results");
    n++;
    switch (opt) {
      case Kint:
      case Kuint: {
        lua_Integer res = unpackint(L, data + pos, h.islittle,
                                       cast_int(size), (opt == Kint));
        lua_pushinteger(L, res);
        break;
      }
      case Kfloat: {
        float f;
        copywithendian((char *)&f, data + pos, sizeof(f), h.islittle);
        lua_pushnumber(L, (lua_Number)f);
        break;
      }
      case Knumber: {
        lua_Number f;
        copywithendian((char *)&f, data + pos, sizeof(f), h.islittle);
        lua_pushnumber(L, f);
        break;
      }
      case Kdouble: {
        double f;
        copywithendian((char *)&f, data + pos, sizeof(f), h.islittle);
        lua_pushnumber(L, (lua_Number)f);
        break;
      }
      case Kchar: {
        pushstring(L, &h.work, data + pos, size);
        break;
      }
      case Kstring: {
        lua_Unsigned len = (lua_Unsigned)unpackint(L, data + pos,
                                          h.islittle, cast_int(size), 0);
        luaL_argcheck(L, len <= ld - pos - size, 2, "data string too short");
        pushstring(L, &h.work, data + pos + size, cast_sizet(len));
        pos += cast_sizet(len);  /* skip string */
        break;
      }
      case Kzstr: {
        size_t len = findzero(L, &h.work, data + pos, ld - pos);
        luaL_argcheck(L, pos + len < ld, 2,
                         "unfinished string for format 'z'");
        pushstring(L, &h.work, data + pos, len);
        pos += len + 1;  /* skip string plus final '\0' */
        break;
      }
      case Kpaddalign: case Kpadding: case Knop:
        n--;  /* undo increment */
        break;
    }
    pos += size;
    luaQ_work(L, &h.work);
  }
  lua_pushinteger(L, cast_st2S(pos) + 1);  /* next position */
  return luaL_rootresult(L, roots, 2, n + 1);
}

/* }====================================================== */


static const luaL_Reg strlib[] = {
  {"byte", luaB_strbyte},
  {"char", str_char},
  {"dump", str_dump},
  {"find", str_find},
  {"format", str_format},
  {"gmatch", gmatch},
  {"gsub", str_gsub},
  {"len", str_len},
  {"lower", str_lower},
  {"match", str_match},
  {"rep", str_rep},
  {"reverse", str_reverse},
  {"sub", luaB_strsub},
  {"upper", str_upper},
  {"pack", str_pack},
  {"packsize", str_packsize},
  {"unpack", str_unpack},
  {NULL, NULL}
};


static void createmetatable (lua_State *L) {
  const luaL_Reg *entry;
  /* table to be metatable for strings */
  luaL_newlibtable(L, stringmetamethods);
  for (entry = stringmetamethods; entry->name != NULL; ++entry) {
    if (entry->func != NULL) luaQ_pushmanaged(L, entry->func, 0);
    else lua_pushvalue(L, -2);  /* __index is the string library */
    lua_setfield(L, -2, entry->name);
  }
  lua_pushliteral(L, "");  /* dummy string */
  lua_pushvalue(L, -2);  /* copy table */
  lua_setmetatable(L, -2);  /* set table as metatable for strings */
  lua_pop(L, 2);  /* pop dummy string and metatable */
}


/*
** Open string library
*/
LUAMOD_API int luaopen_string (lua_State *L) {
  const luaL_Reg *entry;
  luaL_newlibtable(L, strlib);
  for (entry = strlib; entry->name != NULL; ++entry) {
    luaQ_pushmanaged(L, entry->func, 0);
    lua_setfield(L, -2, entry->name);
  }
  createmetatable(L);
  return 1;
}

