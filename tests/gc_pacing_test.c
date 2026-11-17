#include <lapi.h>
#include <lauxlib.h>
#include <lgc.h>
#include <lmem.h>
#include <lstate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#define require(condition)                                                     \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);          \
      abort();                                                                 \
    }                                                                          \
  } while (0)

enum step_mode { AUTOMATIC, EXPLICIT, FULL };
static void *release_string(void *context, void *pointer, size_t old,
                            size_t size) {
  unsigned *freed = context;
  require(size == 0 && old == 65);
  (*freed)++;
  free(pointer);
  return NULL;
}

static l_mem progress(l_mem debt, enum step_mode mode) {
  lua_State *L = luaL_newstate();
  l_mem reclaimed;
  unsigned i, length = 0, freed = 0;
  require(L != NULL);
  lua_gc(L, LUA_GCINC);
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCSTOP);
  require(luaL_loadstring(L, "local head; for i=1,10000 do "
                             "head={next=head} end; return head") == LUA_OK);
  require(lua_pcall(L, 0, 1, 0) == LUA_OK);
  for (i = 0; i < 5000; i++) {
    char *text = malloc(65);
    require(text != NULL);
    memset(text, 'x', 64);
    text[64] = '\0';
    lua_pushexternalstring(L, text, 64, release_string, &freed);
    lua_pop(L, 1);
  }
  /* A newest live object lets sweeping reach the garbage through a stable
   * interior cursor, rather than retiring the whole dead head while entering
   * the sweep phase. The long chain remains a separate graph safety check. */
  lua_newtable(L);
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPMUL, 100);
  lua_gc(L, LUA_GCRESTART);
  lua_gc(L, LUA_GCSTEP, (size_t)0);
  require(freed == 0);
  if (mode == FULL)
    lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, 0);
  lua_lock(L);
  require(G(L)->gcstate == GCSswpallgc);
  luaE_setdebt(G(L), debt);
  if (mode == EXPLICIT) {
    lua_unlock(L);
    lua_gc(L, LUA_GCSTEP, (size_t)8192);
    lua_lock(L);
  } else
    luaC_step(L);
  if (mode == FULL)
    require(G(L)->gcstate == GCSpause);
  reclaimed = (l_mem)freed;
  lua_unlock(L);
  lua_gc(L, LUA_GCCOLLECT);
  lua_pop(L, 1); /* newest live anchor */
  while (lua_istable(L, -1)) {
    lua_getfield(L, -1, "next");
    lua_remove(L, -2);
    length++;
  }
  require(length == 10000 && lua_isnil(L, -1));
  lua_close(L);
  return reclaimed;
}

static void persistent_storage(int generational, int refresh) {
  enum { CONTROL_BYTES = 1024 * 1024 };
  lua_State *L = luaL_newstate();
  void *control, *extra = NULL;
  unsigned freed = 0;
  char *text;
  require(L != NULL);
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  lua_lock(L);
  control = luaM_alloccontrol(L, CONTROL_BYTES);
  require(control != NULL);
  memset(control, 0xa5, CONTROL_BYTES);
  lua_unlock(L);
  require(luaL_loadstring(L, "for i=1,500 do local t={i,i+1} end") == LUA_OK);
  lua_gc(L, LUA_GCRESTART);
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_gc(L, LUA_GCCOUNT) >= CONTROL_BYTES / 1024);
  if (refresh) {
    l_mem before, spent;
    lua_lock(L);
    luaM_flushdebt(L);
    before = G(L)->GCdebt;
    extra = luaM_alloccontrol(L, 65536);
    require(extra != NULL);
    luaM_flushdebt(L);
    spent = G(L)->GCdebt;
    require(spent == before - 65536 && spent > 0);
    /* A late physical completion must preserve the outstanding allocation
     * liability rather than treating these new bytes as a fresh baseline. */
    luaC_finishreclaim(L);
    require(G(L)->GCdebt == spent);
    lua_unlock(L);
  }
  text = malloc(65);
  require(text != NULL);
  memset(text, 'x', 64);
  text[64] = '\0';
  lua_pushexternalstring(L, text, 64, release_string, &freed);
  lua_pop(L, 1);
  require(lua_pcall(L, 0, 0, 0) == LUA_OK);
  /* This bounded amount of allocation fits the live heap's allowance, even
   * when most live storage belongs to runtime records rather than GC objects.
   */
  require(freed == 0);
  lua_gc(L, LUA_GCCOLLECT);
  require(freed == 1);
  lua_lock(L);
  require(((unsigned char *)control)[CONTROL_BYTES - 1] == 0xa5);
  luaM_free_(L, control, CONTROL_BYTES);
  if (extra != NULL)
    luaM_free_(L, extra, 65536);
  lua_unlock(L);
  lua_close(L);
}

struct chunk {
  char *bytes;
  size_t size;
  int read;
};

static int save_chunk(lua_State *L, const void *bytes, size_t size,
                      void *context) {
  struct chunk *chunk = context;
  char *grown = realloc(chunk->bytes, chunk->size + size);
  (void)L;
  require(grown != NULL);
  chunk->bytes = grown;
  memcpy(grown + chunk->size, bytes, size);
  chunk->size += size;
  return 0;
}

static const char *read_chunk(lua_State *L, void *context, size_t *size) {
  struct chunk *chunk = context;
  (void)L;
  *size = chunk->read ? 0 : chunk->size;
  chunk->read = 1;
  return *size != 0 ? chunk->bytes : NULL;
}

static void borrowed_storage(int generational) {
  struct chunk dumped = {0}, image = {0};
  lua_State *L = luaL_newstate();
  unsigned pass;
  require(L != NULL);
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  require(luaL_loadstring(L, "local function add(x) return x+40 end; "
                             "return add(2)") == LUA_OK);
  require(lua_dump(L, save_chunk, &dumped, 0) == 0 && dumped.size != 0);
  lua_pop(L, 1);
  image.size = dumped.size;
  image.bytes = mmap(NULL, image.size, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  require(image.bytes != MAP_FAILED);
  memcpy(image.bytes, dumped.bytes, image.size);
  require(mprotect(image.bytes, image.size, PROT_READ) == 0);
  for (pass = 0; pass < 2; pass++) {
    image.read = 0;
    require(lua_load(L, read_chunk, &image, "=borrowed", "B") == LUA_OK);
    lua_lock(L);
    require(clLvalue(s2v(L->top.p - 1))->p->flag & PF_FIXED);
    lua_unlock(L);
    require(lua_pcall(L, 0, 1, 0) == LUA_OK);
    require(lua_tointeger(L, -1) == 42);
    lua_pop(L, 1);
    lua_gc(L, LUA_GCCOLLECT);
    require(G(L)->GCretired == 0);
    /* Reloading the same read-only mapping also proves that retirement did
     * not treat its borrowed code/debug arrays as allocator-owned buffers. */
    require(memcmp(image.bytes, dumped.bytes, image.size) == 0);
  }
  lua_close(L);
  require(munmap(image.bytes, image.size) == 0);
  free(dumped.bytes);
}

int main(int argc, char **argv) {
  l_mem basic, overdue;
  require(argc == 2);
  if (strncmp(argv[1], "borrowed_", 9) == 0) {
    borrowed_storage(strstr(argv[1], "generational") != NULL);
    return 0;
  }
  if (strncmp(argv[1], "persistent_", 11) == 0 ||
      strncmp(argv[1], "refresh_", 8) == 0) {
    persistent_storage(strstr(argv[1], "generational") != NULL,
                       strncmp(argv[1], "refresh_", 8) == 0);
    return 0;
  }
  basic = progress(0, AUTOMATIC);
  require(basic > 0);
  if (strcmp(argv[1], "explicit") == 0) {
    overdue = progress(-8192, EXPLICIT);
    require(overdue == basic);
  } else if (strcmp(argv[1], "fast") == 0) {
    overdue = progress(-8192, FULL);
    require(overdue > basic * 8);
  } else {
    require(strcmp(argv[1], "overdue") == 0 ||
            strcmp(argv[1], "overflow") == 0);
    overdue = progress(strcmp(argv[1], "overflow") == 0 ? -MAX_LMEM : -8192,
                       AUTOMATIC);
    fprintf(stderr, "reclaimed strings: basic %lld, overdue %lld\n",
            (long long)basic, (long long)overdue);
    require(overdue > basic * 8);
  }
  return 0;
}
