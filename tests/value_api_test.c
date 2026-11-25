#include <laccess.h>
#include <lauxlib.h>
#include <ldo.h>
#include <lstate.h>
#include <lualib.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define require(c)                                                             \
  do {                                                                         \
    if (!(c)) {                                                                \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c);                  \
      abort();                                                                 \
    }                                                                          \
  } while (0)

enum { VALUES = 15 };
static const int types[VALUES] = {
    LUA_TNIL,           LUA_TBOOLEAN,  LUA_TBOOLEAN, LUA_TNUMBER,
    LUA_TNUMBER,        LUA_TNUMBER,   LUA_TSTRING,  LUA_TSTRING,
    LUA_TLIGHTUSERDATA, LUA_TUSERDATA, LUA_TTHREAD,  LUA_TTABLE,
    LUA_TFUNCTION,      LUA_TFUNCTION, LUA_TFUNCTION};
static struct {
  lua_State *main, *worker, *thread;
  const void *identity[VALUES];
  unsigned allocations, denied, moved, entered, done, closed;
  int reject, deny_mode, relocate, parallel;
} probe;

static void *allocate(void *unused, void *memory, size_t old, size_t size) {
  void *replacement;
  (void)unused;
  if (size == 0) {
    free(memory);
    return NULL;
  }
  if (__atomic_load_n(&probe.reject, __ATOMIC_RELAXED)) {
    __atomic_fetch_add(&probe.denied, 1, __ATOMIC_RELAXED);
    return NULL;
  }
  replacement = malloc(size);
  if (replacement == NULL)
    return NULL;
  __atomic_fetch_add(&probe.allocations, 1, __ATOMIC_RELAXED);
  if (memory != NULL) {
    memcpy(replacement, memory, old < size ? old : size);
    free(memory);
  }
  return replacement;
}

static void relocate_after_snapshot(lua_State *L) {
  if (L == probe.worker && probe.relocate) {
    uintptr_t before = (uintptr_t)L->stack.p;
    probe.relocate = 0;
    require(luaD_reallocstack(L, stacksize(L) + 128, 1));
    require((uintptr_t)L->stack.p != before);
    probe.moved++;
  }
}

void __real_luaA_end(lua_State *, LuaObjectScope *);
void __wrap_luaA_end(lua_State *L, LuaObjectScope *scope) {
  __real_luaA_end(L, scope);
  relocate_after_snapshot(L);
}

void __real_luaA_getregistry(lua_State *, LuaNativeRoot *);
void __wrap_luaA_getregistry(lua_State *L, LuaNativeRoot *result) {
  __real_luaA_getregistry(L, result);
  relocate_after_snapshot(L);
}

static int noop(lua_State *L) {
  (void)L;
  return 0;
}

static void verify(lua_State *L, int index, unsigned item) {
  int numeric = item >= 3 && item <= 6;
  int integer = item == 3 || item == 4;
  int convertible_integer = integer || item == 6;
  int ok;
  char buffer[128];
  lua_Integer expected = item == 3   ? LUA_MININTEGER
                         : item == 4 ? LUA_MAXINTEGER
                                     : 123;
  require(lua_type(L, index) == types[item]);
  require(lua_isinteger(L, index) == integer);
  require(lua_isnumber(L, index) == numeric);
  require(lua_isstring(L, index) == (numeric || item == 7));
  require(lua_isuserdata(L, index) == (item == 8 || item == 9));
  require(lua_iscfunction(L, index) == (item == 12 || item == 13));
  require(lua_toboolean(L, index) == (item >= 2));
  require(lua_tocfunction(L, index) ==
          (item == 12 || item == 13 ? noop : NULL));
  require(lua_tothread(L, index) == (item == 10 ? probe.thread : NULL));
  require(lua_topointer(L, index) == probe.identity[item]);
  require(lua_touserdata(L, index) ==
          (item == 8 || item == 9 ? probe.identity[item] : NULL));
  if (item == 9)
    require(*(unsigned *)lua_touserdata(L, index) == 0x1234abcdU);
  if (item == 6 || item == 7) {
    const char *expected_string = item == 6 ? "123" : "hello";
    size_t length;
    const char *bytes = lua_tolstring(L, index, &length);
    require(bytes != NULL && strcmp(bytes, expected_string) == 0);
    require(length == strlen(expected_string));
    require(lua_rawlen(L, index) == length);
  }
  if (item == 9)
    require(lua_rawlen(L, index) == sizeof(unsigned));
  if (item == 11)
    require(lua_rawlen(L, index) == 0);
  if (item != 3 && item != 4 && item != 5 && item != 6 && item != 7) {
    size_t length = 17;
    require(lua_tolstring(L, index, &length) == NULL && length == 0);
  }
  if (numeric) {
    lua_Number number = lua_tonumberx(L, index, &ok);
    require(ok && number == (item == 5 ? 1.5 : (lua_Number)expected));
  } else {
    require(lua_tonumberx(L, index, &ok) == 0 && !ok);
  }
  if (convertible_integer) {
    require(lua_tointegerx(L, index, &ok) == expected && ok);
  } else {
    require(lua_tointegerx(L, index, &ok) == 0 && !ok);
  }
  if (integer || item == 5) {
    unsigned length = lua_numbertocstring(L, index, buffer);
    require(length > 1 && length < sizeof(buffer) && buffer[length - 1] == 0);
  } else
    require(lua_numbertocstring(L, index, buffer) == 0);
}

static int exercise(lua_State *L) {
  unsigned iteration = 0;
  unsigned before = __atomic_load_n(&probe.allocations, __ATOMIC_RELAXED);
  int reject = probe.deny_mode;
  __atomic_store_n(&probe.reject, reject, __ATOMIC_RELAXED);
  __atomic_store_n(&probe.entered, 1, __ATOMIC_RELEASE);
  do {
    unsigned i;
    require(lua_gettop(L) == VALUES);
    require(lua_status(L) == LUA_OK);
    require(lua_isyieldable(L));
    for (i = 0; i < VALUES; i++) {
      verify(L, (int)i + 1, i);
      verify(L, lua_upvalueindex((int)i + 1), i);
      lua_pushvalue(L, (int)i + 1);
      verify(L, -1, i);
      lua_pushnil(L);
      lua_copy(L, -2, -1);
      require(lua_rawequal(L, -1, -2));
      require(lua_rawequal(L, -1, (int)i + 1));
      lua_rotate(L, -2, 1);
      verify(L, -1, i);
      lua_copy(L, (int)i + 1, lua_upvalueindex((int)i + 1));
      verify(L, lua_upvalueindex((int)i + 1), i);
      lua_pop(L, 2);
    }
    lua_pushinteger(L, LUA_MININTEGER);
    require(lua_tointeger(L, -1) == LUA_MININTEGER);
    require(lua_absindex(L, -1) == VALUES + 1);
    lua_pop(L, 1);
    lua_pushnumber(L, 1.5);
    require(lua_tonumber(L, -1) == 1.5);
    lua_pop(L, 1);
    lua_pushboolean(L, 0);
    require(!lua_toboolean(L, -1));
    lua_pop(L, 1);
    lua_pushnil(L);
    require(lua_isnil(L, -1));
    lua_pop(L, 1);
    lua_pushlightuserdata(L, &probe);
    require(lua_touserdata(L, -1) == &probe);
    lua_pop(L, 1);
    require(lua_pushthread(L) == 0 && lua_tothread(L, -1) == L);
    lua_pop(L, 1);
    lua_pushcfunction(L, noop);
    require(lua_tocfunction(L, -1) == noop);
    lua_pop(L, 1);
    lua_settop(L, VALUES + 4);
    for (i = 1; i <= 4; i++)
      require(lua_isnil(L, VALUES + (int)i));
    lua_settop(L, VALUES);
    require(lua_type(L, VALUES + 1) == LUA_TNONE);
    require(lua_type(L, lua_upvalueindex(VALUES + 1)) == LUA_TNONE);
    require(!lua_rawequal(L, VALUES + 1, VALUES + 1));
    require(!lua_rawequal(L, 1, VALUES + 1));
    lua_pushvalue(L, LUA_REGISTRYINDEX);
    require(lua_istable(L, -1));
    lua_pop(L, 1);
    iteration++;
  } while (iteration < 8 ||
           (probe.parallel && !__atomic_load_n(&probe.done, __ATOMIC_ACQUIRE)));
  if (reject)
    require(__atomic_load_n(&probe.allocations, __ATOMIC_RELAXED) == before &&
            __atomic_load_n(&probe.denied, __ATOMIC_RELAXED) == 0);
  __atomic_store_n(&probe.reject, 0, __ATOMIC_RELAXED);
  lua_pushboolean(L, 1);
  return 1;
}

static int relocating(lua_State *L) {
  unsigned i;
  for (i = 0; i < VALUES; i++) {
    probe.relocate = 1;
    lua_pushvalue(L, lua_upvalueindex((int)i + 1));
    require(!probe.relocate && probe.moved == i + 1);
    verify(L, -1, i);
    lua_pop(L, 1);
  }
  probe.relocate = 1;
  lua_pushvalue(L, LUA_REGISTRYINDEX);
  require(!probe.relocate && probe.moved == VALUES + 1 && lua_istable(L, -1));
  lua_pop(L, 1);
  lua_pushboolean(L, 1);
  return 1;
}

static int close_value(lua_State *L) {
  (void)L;
  probe.closed++;
  require(luaL_dostring(
              L, "local t={42}; collectgarbage(); assert(t[1]==42)") == LUA_OK);
  return 0;
}
static int close_error(lua_State *L) {
  probe.closed++;
  return luaL_error(L, "expected closing error");
}
static int closing(lua_State *L) {
  lua_toclose(L, 1);
  lua_settop(L, 0);
  require(probe.closed == 1);
  lua_pushboolean(L, 1);
  return 1;
}

static int number_string(lua_State *L) {
  const char *minimum = "-9223372036854775808";
  size_t length;
  const char *bytes = lua_tolstring(L, lua_upvalueindex(1), &length);
  require(bytes != NULL && length == strlen(minimum));
  require(strcmp(bytes, minimum) == 0);
  require(lua_type(L, lua_upvalueindex(1)) == LUA_TSTRING);
  lua_pushnumber(L, 1.5);
  bytes = lua_tolstring(L, -1, NULL);
  require(bytes != NULL && strcmp(bytes, "1.5") == 0);
  lua_gc(L, LUA_GCCOLLECT);
  require(strcmp(bytes, "1.5") == 0);
  lua_pop(L, 1);
  lua_pushboolean(L, 1);
  return 1;
}

static void prepare(lua_State *L, lua_CFunction function) {
  unsigned i;
  require(lua_checkstack(L, VALUES * 3));
  lua_pushnil(L);
  lua_pushboolean(L, 0);
  lua_pushboolean(L, 1);
  lua_pushinteger(L, LUA_MININTEGER);
  lua_pushinteger(L, LUA_MAXINTEGER);
  lua_pushnumber(L, 1.5);
  lua_pushliteral(L, "123");
  lua_pushliteral(L, "hello");
  lua_pushlightuserdata(L, &probe);
  *(unsigned *)lua_newuserdatauv(L, sizeof(unsigned), 0) = 0x1234abcdU;
  probe.thread = lua_newthread(L);
  lua_newtable(L);
  lua_pushcfunction(L, noop);
  lua_pushinteger(L, 7);
  lua_pushcclosure(L, noop, 1);
  require(luaL_loadstring(L, "return 42") == LUA_OK);
  require(lua_gettop(L) == VALUES);
  for (i = 0; i < VALUES; i++)
    probe.identity[i] = lua_topointer(L, (int)i + 1);
  probe.worker = lua_newthread(L);
  for (i = 0; i < VALUES; i++)
    lua_pushvalue(L, (int)i + 1);
  lua_pushcclosure(L, function, VALUES);
  lua_xmove(L, probe.worker, 1);
  for (i = 0; i < VALUES; i++) {
    lua_pushvalue(L, (int)i + 1);
    lua_xmove(L, probe.worker, 1);
  }
}

static void *invoke(void *unused) {
  int results;
  (void)unused;
  require(lua_resume(probe.worker, NULL, VALUES, &results) == LUA_OK);
  require(results == 1 && lua_toboolean(probe.worker, -1));
  return NULL;
}

int main(int argc, char **argv) {
  lua_State *L = lua_newstate(allocate, NULL, 0);
  int mode, result;
  unsigned i;
  pthread_t worker;
  require(argc == 3 && L != NULL);
  mode = strcmp(argv[2], "denied") == 0          ? 1
         : strcmp(argv[2], "relocate") == 0      ? 2
         : strcmp(argv[2], "parallel") == 0      ? 3
         : strcmp(argv[2], "close") == 0         ? 4
         : strcmp(argv[2], "close_error") == 0   ? 5
         : strcmp(argv[2], "number_string") == 0 ? 6
                                                 : 0;
  probe.main = L;
  luaL_openlibs(L);
  lua_gc(L, strcmp(argv[1], "generational") == 0 ? LUA_GCGEN : LUA_GCINC);
  if (mode == 6) {
    lua_pushinteger(L, LUA_MININTEGER);
    lua_pushcclosure(L, number_string, 1);
    require(lua_pcall(L, 0, 1, 0) == LUA_OK && lua_toboolean(L, -1));
  } else if (mode >= 4) {
    lua_pushcfunction(L, closing);
    lua_newuserdatauv(L, 1, 0);
    lua_newtable(L);
    lua_pushcfunction(L, mode == 5 ? close_error : close_value);
    lua_setfield(L, -2, "__close");
    lua_setmetatable(L, -2);
    result = lua_pcall(L, 1, 1, 0);
    require(probe.closed == 1);
    if (mode == 5)
      require(result == LUA_ERRRUN &&
              strstr(lua_tostring(L, -1), "expected closing error") != NULL);
    else
      require(result == LUA_OK && lua_toboolean(L, -1));
  } else {
    prepare(L, mode == 2 ? relocating : exercise);
    probe.parallel = mode == 3;
    if (mode == 1)
      probe.deny_mode = 1;
    if (mode == 3) {
      require(pthread_create(&worker, NULL, invoke, NULL) == 0);
      while (!__atomic_load_n(&probe.entered, __ATOMIC_ACQUIRE))
        sched_yield();
      for (i = 0; i < 16; i++)
        lua_gc(L, LUA_GCCOLLECT);
      __atomic_store_n(&probe.done, 1, __ATOMIC_RELEASE);
      require(pthread_join(worker, NULL) == 0);
    } else
      invoke(NULL);
    __atomic_store_n(&probe.reject, 0, __ATOMIC_RELAXED);
  }
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCCOLLECT);
  lua_close(L);
  return 0;
}
