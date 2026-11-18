#include <lapi.h>
#include <lauxlib.h>
#include <ldo.h>
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
static unsigned unavailable_batches, scan_batches, interrupt_assist;
void __real_luaQ_poll(lua_State *);
void __wrap_luaQ_poll(lua_State *L) {
  if (interrupt_assist && luaC_ingc(L)) {
    interrupt_assist = 0;
    luaD_throw(L, LUA_ERRRUN);
  }
  __real_luaQ_poll(L);
}
lu_mem __real_luaC_tracebatch(lua_State *, unsigned);
lu_mem __wrap_luaC_tracebatch(lua_State *L, unsigned limit) {
  if (unavailable_batches != 0) {
    unavailable_batches--;
    return 0; /* The shared work may already be claimed by another scanner. */
  }
  scan_batches++;
  return __real_luaC_tracebatch(L, limit);
}
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
  /* This fixture measures sweep pacing independently of marking slices. */
  luaC_waittrace(L);
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

static void interrupted_assist(lua_State *L, void *unused) {
  (void)unused;
  L->assist_epoch = G(L)->world.work_epoch;
  L->trace_debt = MAX_LMEM;
  interrupt_assist = 1;
  luaC_assisttrace(L);
  require(0);
}

static void bounded_marking(const char *kind) {
  lua_State *L = luaL_newstate();
  unsigned length = 0, steps = 0;
  int generational = strstr(kind, "generational") != NULL;
  int idle = strncmp(kind, "idle_", 5) == 0;
  int pressure = strncmp(kind, "pressure_", 9) == 0;
  int unavailable = strncmp(kind, "unavailable_", 12) == 0;
  int stopped = strncmp(kind, "stopped_", 8) == 0;
  int unwind = strncmp(kind, "unwind_", 7) == 0;
  require(L != NULL);
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  require(luaL_loadstring(L, "local head; for i=1,50000 do "
                             "head={next=head} end; return head") == LUA_OK);
  require(lua_pcall(L, 0, 1, 0) == LUA_OK);
  require(luaL_loadstring(L, "local sum=0; for i=1,2000000 do "
                             "sum=sum+i end; return sum") == LUA_OK);
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPMUL, 100);
  if (!stopped)
    lua_gc(L, LUA_GCRESTART);
  lua_lock(L);
  G(L)->GCgoal = MAX_LMEM; /* Isolate slice budgeting from heap pressure. */
  if (stopped) {
    lua_unlock(L);
    require(lua_gc(L, LUA_GCSTEP, (size_t)0) >= 0);
    lua_lock(L);
  } else {
    luaE_setdebt(G(L), 0);
    luaC_step(L); /* A normal request returns with its job unfinished. */
  }
  require(luaC_tracing(L));
  require(!G(L)->world.stopped && L->world_depth == 0);
  require(G(L)->trace_busy == 0);
  if (unwind) {
    unsigned before;
    require(luaD_rawrunprotected(L, interrupted_assist, NULL) == LUA_ERRRUN);
    require(!luaC_ingc(L) && L->native_depth == 0 && luaC_tracing(L));
    before = scan_batches;
    L->trace_debt = LUA_GC_ASSIST_BYTES;
    luaC_assisttrace(L);
    require(scan_batches > before);
  }
  lua_unlock(L);
  if (pressure) {
    lua_lock(L);
    /* Even a worker which has not allocated a full debt batch must help when
     * the shared allocation runway is spent. No stale epoch may bypass it. */
    __atomic_store_n(&G(L)->GCmarkdebt, 0, __ATOMIC_RELAXED);
    L->assist_epoch = G(L)->world.work_epoch - 1;
    L->trace_debt = 0;
    luaC_assisttrace(L);
    require(!luaC_tracing(L));
    lua_unlock(L);
  } else if (!idle && !stopped) {
    /* Subsequent allocation assistance also relinquishes application capacity.
     * No coroutine owns the continuation of the original request. */
    while (luaC_tracing(L)) {
      lua_lock(L);
      L->assist_epoch = G(L)->world.work_epoch;
      L->assist_debt = LUA_GC_ASSIST_BYTES;
      L->trace_debt = LUA_GC_ASSIST_BYTES;
      if (unavailable && steps == 0)
        unavailable_batches = 1;
      luaC_assisttrace(L);
      if (unavailable && steps == 0)
        require(L->trace_debt == LUA_GC_ASSIST_BYTES);
      require(!G(L)->world.stopped && G(L)->trace_busy == 0);
      lua_unlock(L);
      require(++steps < 10000);
    }
    require(steps > 1);
  }
  /* This loop neither allocates nor calls C. Existing VM safepoints must
   * finish marking instead of leaving a dormant collection behind. */
  scan_batches = 0;
  require(lua_pcall(L, 0, 1, 0) == LUA_OK);
  require(lua_tointeger(L, -1) == (lua_Integer)2000001000000LL);
  if (stopped) {
    /* A step requested with automatic GC stopped must not silently restart
     * collection through either VM polling or an allocation assist. */
    require(luaC_tracing(L) && scan_batches == 0 && !gcrunning(G(L)));
    lua_gc(L, LUA_GCCOLLECT);
  }
  require(!luaC_tracing(L));
  lua_pop(L, 1);
  lua_gc(L, LUA_GCCOLLECT);
  while (lua_istable(L, -1)) {
    lua_getfield(L, -1, "next");
    lua_remove(L, -2);
    length++;
  }
  require(length == 50000 && lua_isnil(L, -1));
  lua_close(L);
}

static void sweep_growth(void) {
  enum { CONTROL_BYTES = 65536 };
  lua_State *L = luaL_newstate();
  LuaGCFrame frame;
  void *during_mark, *after_mark;
  require(L != NULL);
  lua_gc(L, LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  require(luaL_loadstring(L, "local head; for i=1,10000 do "
                             "head={next=head} end; "
                             "for i=1,1000 do local t={i} end; "
                             "return head,{}") == LUA_OK);
  require(lua_pcall(L, 0, 2, 0) == LUA_OK);
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
  lua_gc(L, LUA_GCRESTART);
  lua_lock(L);
  G(L)->GCgoal = MAX_LMEM;
  luaE_setdebt(G(L), 0);
  luaC_step(L);
  require(luaC_tracing(L));
  during_mark = luaM_alloccontrol(L, CONTROL_BYTES);
  require(during_mark != NULL);
  luaC_waittrace(L);
  require(G(L)->gcstate == GCSswpallgc);
  after_mark = luaM_alloccontrol(L, CONTROL_BYTES);
  require(after_mark != NULL);
  luaW_stop(L);
  luaC_entergc(L, &frame);
  luaC_runtilstate(L, GCSpause, 1);
  /* Mark-end storage counts the earlier persistent allocation. The later
   * allocation consumes runway instead of granting another pause allowance. */
  require(gettotalbytes(G(L)) - G(L)->GCretired ==
          G(L)->GCcyclelive + CONTROL_BYTES);
  require(G(L)->GCgoal == applygcparam(G(L), PAUSE, G(L)->GCcyclelive));
  luaC_leavegc(L, &frame);
  luaW_start(L);
  luaC_reclaim(L);
  luaM_free_(L, during_mark, CONTROL_BYTES);
  luaM_free_(L, after_mark, CONTROL_BYTES);
  lua_unlock(L);
  lua_close(L);
}

struct closing_heap {
  size_t live;
  unsigned deny, failures, finalized, completed;
};

static void *closing_allocate(void *context, void *pointer, size_t old,
                              size_t size) {
  struct closing_heap *heap = context;
  void *replacement;
  if (pointer == NULL)
    old = 0;
  require(heap->live >= old);
  if (size >= 65536 && heap->deny) {
    heap->deny--;
    heap->failures++;
    return NULL;
  }
  if (size == 0) {
    free(pointer);
    heap->live -= old;
    return NULL;
  }
  replacement = realloc(pointer, size);
  if (replacement != NULL)
    heap->live = heap->live - old + size;
  return replacement;
}

static int closing_finalize(lua_State *L) {
  void *context;
  struct closing_heap *heap;
  lua_getallocf(L, &context);
  heap = context;
  require(heap->finalized++ == 0 && !luaC_tracing(L));
  /* Closing still owes an allocation retry when a finalizer needs memory.
   * This bypasses pooled free slots and deterministically fails once. */
  require(lua_newuserdatauv(L, 65536, 0) != NULL);
  heap->completed++;
  lua_pop(L, 1);
  return 0;
}

static void closing_pressure(const char *kind) {
  struct closing_heap heap = {0};
  lua_State *L = lua_newstate(closing_allocate, &heap, 0);
  int pending = strstr(kind, "pending") != NULL;
  require(L != NULL);
  lua_gc(L, strstr(kind, "generational") != NULL ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  lua_newuserdatauv(L, 0, 0);
  lua_newtable(L);
  lua_pushcfunction(L, closing_finalize);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  if (pending) {
    require(luaL_loadstring(L, "local head; for i=1,10000 do "
                               "head={head} end; return head") == LUA_OK);
    require(lua_pcall(L, 0, 1, 0) == LUA_OK);
    lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
    lua_gc(L, LUA_GCRESTART);
    lua_lock(L);
    G(L)->GCgoal = MAX_LMEM;
    luaE_setdebt(G(L), 0);
    luaC_step(L);
    require(luaC_tracing(L));
    lua_unlock(L);
  }
  heap.deny = 1;
  lua_close(L);
  require(heap.failures == 1 && heap.finalized == 1 && heap.completed == 1 &&
          heap.live == 0);
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
    luaM_flushdebt(L, L);
    before = G(L)->GCdebt;
    extra = luaM_alloccontrol(L, 65536);
    require(extra != NULL);
    luaM_flushdebt(L, L);
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
  if (strncmp(argv[1], "closing_", 8) == 0) {
    closing_pressure(argv[1]);
    return 0;
  }
  if (strcmp(argv[1], "sweep_growth") == 0) {
    sweep_growth();
    return 0;
  }
  if (strncmp(argv[1], "bounded_", 8) == 0 ||
      strncmp(argv[1], "idle_", 5) == 0 ||
      strncmp(argv[1], "pressure_", 9) == 0 ||
      strncmp(argv[1], "unavailable_", 12) == 0 ||
      strncmp(argv[1], "stopped_", 8) == 0 ||
      strncmp(argv[1], "unwind_", 7) == 0) {
    bounded_marking(argv[1]);
    return 0;
  }
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
