#include <laccess.h>
#include <lapi.h>
#include <lauxlib.h>
#include <ldo.h>
#include <lgc.h>
#include <lmem.h>
#include <lstate.h>
#include <ltable.h>
#include <lualib.h>
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

enum step_mode {
  AUTOMATIC,
  EXPLICIT,
  FULL,
  SWEEP_PRESSURE,
  SWEEP_CREDIT,
  SWEEP_POLL_PRESSURE
};
static unsigned unavailable_batches, scan_batches, sweep_batches;
static unsigned interrupt_assist, stops;
static void *nursery_backing;
static LuaAllocBlock *nursery_slot;
static lua_State *nursery_peer;

void __real_luaM_free_(lua_State *, void *, size_t);
void __wrap_luaM_free_(lua_State *L, void *block, size_t size) {
  if (nursery_backing != NULL && block == nursery_backing) {
    LuaWorldScope scope;
    nursery_backing = NULL;
    require(!(luaMP_header(nursery_slot) & LUA_BLOCK_COLLECTABLE));
    /* Run a collector with the nursery owner withdrawn, as an allocator
     * wait can do while its dead tables still have backing to return. */
    scope = luaW_detach(L);
    require(lua_gc(nursery_peer, LUA_GCCOLLECT) == 0);
    luaW_attach(L, scope);
  }
  __real_luaM_free_(L, block, size);
}

lu_mem __real_luaC_sweepbatch(lua_State *, unsigned, int);
lu_mem __wrap_luaC_sweepbatch(lua_State *L, unsigned limit, int complete) {
  sweep_batches++;
  return __real_luaC_sweepbatch(L, limit, complete);
}
void __real_luaW_stop(lua_State *);
void __wrap_luaW_stop(lua_State *L) {
  stops += L->world_depth == 0;
  __real_luaW_stop(L);
}
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
  if (mode == SWEEP_CREDIT) {
    unsigned before = sweep_batches;
    /* Other helpers paid the proportional work at this allocation point.
     * Work-quota debt must not bypass the shared credit on the next check. */
    G(L)->pacer.goal = G(L)->pacer.start;
    G(L)->pacer.expected = G(L)->pacer.work;
    __atomic_store_n(&G(L)->pacer.hard, MAX_LMEM, __ATOMIC_RELAXED);
    L->trace_debt = LUA_GC_ASSIST_BYTES;
    luaC_checkGC(L);
    require(L->trace_debt == 0 && sweep_batches == before);
    luaC_checkGC(L);
    require(luaC_sweeping(L) && sweep_batches == before);
  } else if (mode == SWEEP_PRESSURE || mode == SWEEP_POLL_PRESSURE) {
    __atomic_store_n(&G(L)->pacer.hard, 0, __ATOMIC_RELAXED);
    L->trace_debt = 0;
    if (mode == SWEEP_POLL_PRESSURE) {
      luaC_pollsweep(L);
      require(luaC_sweeping(L)); /* nonallocating workers owe a bounded slice */
    } else {
      luaC_checkGC(L);
      require(!luaC_sweeping(L));
    }
  } else if (mode == EXPLICIT) {
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

/* These fixtures need an unfinished young cycle with no automatic helpers.
 * A public basic young step now completes the minor collection. */
static void start_stopped_trace(lua_State *L) {
  require(G(L)->gcstp == GCSTPUSR && !luaC_tracing(L) && !luaC_sweeping(L));
  G(L)->gcstp = 0; /* The fixture owns the only carrier. */
  luaE_setdebt(G(L), 0);
  luaC_step(L);
  G(L)->gcstp = GCSTPUSR;
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
  G(L)->pacer.goal = MAX_LMEM; /* Isolate slice budgeting from heap pressure. */
  if (stopped && generational)
    start_stopped_trace(L);
  else if (stopped) {
    lua_unlock(L);
    require(lua_gc(L, LUA_GCSTEP, (size_t)0) >= 0);
    lua_lock(L);
  } else {
    unsigned before = stops;
    luaE_setdebt(G(L), 0);
    luaC_step(L); /* A normal request returns with its job unfinished. */
    require(stops == before);
  }
  require(luaC_tracing(L));
  require(!luaW_stopped(&G(L)->world) && L->world_depth == 0);
  require(G(L)->trace_busy == 0);
  /* The synthetic debt below represents allocation through the soft target.
   * Keep the independent hard guard at MAX_LMEM to test bounded slices. */
  G(L)->pacer.goal = G(L)->pacer.start;
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
    __atomic_store_n(&G(L)->pacer.hard, 0, __ATOMIC_RELAXED);
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
      require(!luaW_stopped(&G(L)->world) && G(L)->trace_busy == 0);
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
  G(L)->pacer.goal = MAX_LMEM;
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
  luaC_waitsweep(L);
  require(G(L)->gcstate == GCSpause);
  /* Both allocations happened after this cohort began. They remain charged,
   * but neither grants the next pause an unverified survival allowance. */
  require(gettotalbytes(G(L)) - G(L)->GCretired ==
          G(L)->GCcyclelive + 2 * CONTROL_BYTES);
  require(G(L)->pacer.goal == applygcparam(G(L), PAUSE, G(L)->GCcyclelive));
  luaC_leavegc(L, &frame);
  luaW_start(L);
  luaC_reclaim(L);
  luaM_free_(L, during_mark, CONTROL_BYTES);
  luaM_free_(L, after_mark, CONTROL_BYTES);
  lua_unlock(L);
  lua_close(L);
}

static unsigned page_releases, watched_bin;

void __real_luaM_releasepage(lua_State *, LuaAllocPage *, l_mem);
void __wrap_luaM_releasepage(lua_State *L, LuaAllocPage *page, l_mem bytes) {
  if (page->pool == watched_bin)
    __atomic_fetch_add(&page_releases, 1, __ATOMIC_RELAXED);
  __real_luaM_releasepage(L, page, bytes);
}

static void sweep_pooled(int separate_parts) {
  enum { SMALL_TABLES = 512, LARGE_ARRAY = 2048 };
  lua_State *L = luaL_newstate();
  global_State *g;
  LuaGCFrame frame;
  l_mem before;
  require(L != NULL);
  lua_gc(L, LUA_GCINC);
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCSTOP);
  lua_createtable(L, 2, 0);
  lua_pushinteger(L, 47);
  lua_rawseti(L, 1, 2);
  for (unsigned i = 0; i < SMALL_TABLES; i++) {
    lua_createtable(L, separate_parts ? 0 : 2, 0);
    if (separate_parts) {
      Table *table = (Table *)lua_topointer(L, -1);
      lua_lock(L);
      luaH_resize(L, table, 2, 2);
      require(table->inline_bytes == 0 &&
              luaH_pooledsize(table) > sizeof(Table));
      lua_unlock(L);
    }
    if (i == 0) {
      const void *table = lua_topointer(L, -1);
      watched_bin = luaMP_page((const LuaAllocBlock *)table - 1)->pool;
    }
    lua_pop(L, 1);
  }
  lua_createtable(L, LARGE_ARRAY, 0);
  lua_pop(L, 1);
  lua_gc(L, LUA_GCRESTART);

  lua_lock(L);
  g = G(L);
  g->pacer.goal = MAX_LMEM;
  luaE_setdebt(g, 0);
  luaC_step(L);
  luaC_waittrace(L);
  require(luaC_sweeping(L));
  luaW_stop(L);
  luaC_entergc(L, &frame);
  before = gettotalbytes(g);
  luaC_waitsweep(L);
  require(__atomic_load_n(&page_releases, __ATOMIC_RELAXED) != 0);
  require(before - gettotalbytes(g) >= SMALL_TABLES * (l_mem)sizeof(Table));
  require(g->GCretired >= LARGE_ARRAY * (l_mem)sizeof(Value));
  require(gettotalbytes(g) - g->GCretired == g->GCcyclelive);
  luaC_leavegc(L, &frame);
  luaW_start(L);
  luaC_reclaim(L);
  lua_unlock(L);

  require(lua_rawgeti(L, 1, 2) == LUA_TNUMBER);
  require(lua_tointeger(L, -1) == 47);
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
    G(L)->pacer.goal = MAX_LMEM;
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

/* Black allocations made during marking have not survived a tracing cycle.
 * They spend its allowance; they cannot also enlarge the next live base. */
static void born_black(int generational) {
  lua_State *L = luaL_newstate();
  size_t birth;
  require(L != NULL);
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  require(luaL_loadstring(L, "local head; for i=1,10000 do "
                             "head={next=head} end; return head") == LUA_OK);
  require(lua_pcall(L, 0, 1, 0) == LUA_OK);
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPMUL, 100);
  lua_gc(L, LUA_GCRESTART);
  lua_lock(L);
  luaE_setdebt(G(L), 0);
  luaC_step(L);
  require(luaC_tracing(L));
  birth = (size_t)gettotalbytes(G(L)) * 4;
  require(birth < 32 * 1024 * 1024);
  /* The public GC-control API finishes marking before changing options. This
   * fixture keeps that same job open while temporarily suppressing assists. */
  luaW_stop(L);
  G(L)->gcstp = GCSTPUSR;
  luaW_start(L);
  lua_unlock(L);
  require(lua_newuserdatauv(L, birth, 0) != NULL);
  lua_pop(L, 1);
  lua_lock(L);
  require(luaC_tracing(L));
  luaW_stop(L);
  luaC_waittrace(L);
  luaC_waitsweep(L);
  if (G(L)->gckind != KGC_GENMINOR)
    luaC_runtilstate(L, GCSpause, 1);
  luaW_start(L);
  require(!luaC_tracing(L) && !luaC_sweeping(L));
  /* No new growth allowance remains while these untraced bytes are live. */
  require(G(L)->pacer.goal < (l_mem)gettotalbytes(G(L)) - G(L)->GCretired);
  lua_unlock(L);
  lua_close(L);
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
    before = luaE_getdebt(G(L));
    extra = luaM_alloccontrol(L, 65536);
    require(extra != NULL);
    luaM_flushdebt(L);
    spent = luaE_getdebt(G(L));
    require(spent == before - 65536 && spent > 0);
    /* A late physical completion must preserve the outstanding allocation
     * liability rather than treating these new bytes as a fresh baseline. */
    luaC_finishreclaim(L);
    require(luaE_getdebt(G(L)) == spent);
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

static void young_cycle(lua_State *L) {
  require(lua_gc(L, LUA_GCSTEP, (size_t)0) == 0);
  require(!luaC_tracing(L) && !luaC_sweeping(L));
  require(lua_gc(L, LUA_GCISRUNNING) == 0);
}

/* A concurrent sweep can promote a parent before a child allocated outside
 * its frozen cohort. Their ages must not imply that the edge is already old.
 */
static void cohort_promotion(void) {
  lua_State *L = luaL_newstate();
  GCObject *parent, *control, *child;
  require(L != NULL);
  lua_gc(L, LUA_GCGEN);
  lua_gc(L, LUA_GCPARAM, LUA_GCPMINORMAJOR, 0);
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
  lua_gc(L, LUA_GCSTOP);
  lua_newtable(L);
  parent = (GCObject *)lua_topointer(L, -1);
  lua_setglobal(L, "parent");
  lua_newtable(L);
  control = (GCObject *)lua_topointer(L, -1);
  lua_setglobal(L, "control");
  lua_newtable(L);
  lua_newtable(L);
  lua_pushliteral(L, "v");
  lua_setfield(L, -2, "__mode");
  lua_setmetatable(L, -2);
  lua_setglobal(L, "weak_parent");
  young_cycle(L);
  require(getage(parent) == G_SURVIVAL);
  lua_lock(L);
  start_stopped_trace(L);
  luaC_waittrace(L);
  require(luaC_sweeping(L) && getage(parent) == G_SURVIVAL_CLEAN);
  require(getage(control) == G_SURVIVAL_CLEAN);
  lua_unlock(L);
  lua_getglobal(L, "parent");
  lua_newtable(L);
  child = (GCObject *)lua_topointer(L, -1);
  lua_pushinteger(L, 42);
  lua_setfield(L, -2, "value");
  lua_setfield(L, -2, "child");
  lua_pop(L, 1);
  require(lua_getglobal(L, "weak_parent") == LUA_TTABLE);
  lua_newtable(L);
  lua_setfield(L, -2, "child");
  lua_pop(L, 1);
  lua_lock(L);
  luaC_waitsweep(L);
  require(getage(parent) == G_TOUCHED1 && isgray(parent));
  require(getage(control) == G_OLD && !isgray(control));
  require(getage(child) == G_NEW);
  lua_unlock(L);
  young_cycle(L);
  require(getage(child) == G_SURVIVAL);
  require(lua_getglobal(L, "weak_parent") == LUA_TTABLE);
  require(lua_getfield(L, -1, "child") == LUA_TNIL);
  lua_pop(L, 2);
  young_cycle(L);
  require(lua_getglobal(L, "parent") == LUA_TTABLE);
  require(lua_getfield(L, -1, "child") == LUA_TTABLE);
  require(lua_getfield(L, -1, "value") == LUA_TNUMBER);
  require(lua_tointeger(L, -1) == 42);
  require(getage(parent) == G_OLD && !isgray(parent));
  lua_close(L);
}

static void marking_edges(void) {
  lua_State *L = luaL_newstate();
  GCObject *parent;
  unsigned i;
  require(L != NULL);
  lua_gc(L, LUA_GCGEN);
  lua_gc(L, LUA_GCPARAM, LUA_GCPMINORMAJOR, 0);
  lua_gc(L, LUA_GCPARAM, LUA_GCPSTEPSIZE, 64);
  lua_gc(L, LUA_GCSTOP);
  lua_newtable(L);
  parent = (GCObject *)lua_topointer(L, -1);
  lua_setglobal(L, "parent");
  for (i = 0; i < 4; i++)
    young_cycle(L);
  require(getage(parent) == G_OLD && !isgray(parent));
  /* Leave enough young graph work to test writes both before and after this
   * owner is scanned, in the same collection. No automatic slices can race
   * the fixture's explicitly chosen publication points. */
  require(luaL_loadstring(L, "local h; for i=1,10000 do h={h} end; keep=h") ==
          LUA_OK);
  require(lua_pcall(L, 0, 0, 0) == LUA_OK);
  lua_lock(L);
  start_stopped_trace(L);
  require(luaC_tracing(L));
  lua_unlock(L);
  for (i = 0; i < 2; i++) {
    GCObject *child;
    lua_getglobal(L, "parent");
    lua_newtable(L);
    child = (GCObject *)lua_topointer(L, -1);
    lua_pushinteger(L, 42 + i);
    lua_setfield(L, -2, "value");
    lua_setfield(L, -2, i == 0 ? "first" : "second");
    lua_pop(L, 1);
    require(getage(child) == G_NEW && getage(parent) == G_TOUCHED0);
    if (i == 0) {
      unsigned batches = 0;
      lua_lock(L);
      while (getage(parent) == G_TOUCHED0) {
        luaC_tracebatch(L, 1);
        require(++batches < 1000);
      }
      require(getage(parent) == G_TOUCHED1 && luaC_tracing(L));
      lua_unlock(L);
    }
  }
  lua_lock(L);
  luaC_waittrace(L);
  luaC_waitsweep(L);
  lua_unlock(L);
  for (i = 0; i < 4; i++) {
    young_cycle(L);
    require(lua_getglobal(L, "parent") == LUA_TTABLE);
    require(lua_getfield(L, -1, "first") == LUA_TTABLE);
    lua_getfield(L, -1, "value");
    require(lua_tointeger(L, -1) == 42);
    lua_pop(L, 2);
    require(lua_getfield(L, -1, "second") == LUA_TTABLE);
    lua_getfield(L, -1, "value");
    require(lua_tointeger(L, -1) == 43);
    lua_pop(L, 3);
  }
  require(getage(parent) == G_OLD && !isgray(parent));
  lua_close(L);
}

static int identity(lua_State *L) { return lua_gettop(L); }

static int saved_table(lua_State *L) {
  lua_pushvalue(L, lua_upvalueindex(1));
  return 1;
}

static int retain_table(lua_State *L) {
  lua_pushcclosure(L, saved_table, 1);
  return 1;
}

static int move_table(lua_State *L) {
  lua_State *other = lua_tothread(L, lua_upvalueindex(1));
  require(other != NULL);
  lua_xmove(L, other, 1);
  return 0;
}

static void nursery(int generational) {
  lua_State *L = luaL_newstate();
  lua_State *other;
  unsigned before;
  require(L != NULL);
  luaL_openlibs(L);
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  lua_newtable(L);
  other = lua_newthread(L);
  require(
      luaL_loadstring(
          L,
          "local old, make, move = ...; "
          "old.edge={{23}}; old[{39}]=true; "
          "local c=make({71}); "
          "local function bind(x) return function() return x[1] end end; "
          "local read=bind({27}); local root={left={37}}; "
          "local obj=setmetatable({}, {__index={word=19}}); "
          "move({{89}}); "
          "for i=1,2048 do local discarded={i,tag=true} end; "
          "local seen; for k in pairs(old) do "
          "if type(k)=='table' then seen=k[1] end end; "
          "return old.edge[1][1]+seen+c()[1]+read()+root.left[1]+obj.word") ==
      LUA_OK);
  lua_pushvalue(L, 1);
  lua_pushcfunction(L, retain_table);
  lua_pushvalue(L, 2);
  lua_pushcclosure(L, move_table, 1);
  lua_gc(L, LUA_GCCOLLECT);
  lua_lock(L);
  luaE_setdebt(G(L), 32768);
  __atomic_store_n(&G(L)->pacer.hard, getgcbytes(G(L)) + 65536,
                   __ATOMIC_RELAXED);
  lua_unlock(L);
  before = stops;
  require(lua_pcall(L, 3, 1, 0) == LUA_OK);
  require(lua_tointeger(L, -1) == 216);
  require(stops == before);
  require(lua_rawgeti(other, 1, 1) == LUA_TTABLE);
  require(lua_rawgeti(other, -1, 1) == LUA_TNUMBER);
  require(lua_tointeger(other, -1) == 89);
  lua_close(L);
}

static unsigned nursery_finalizers;

static void nursery_backing_wait(void) {
  lua_State *L = luaL_newstate();
  Table *table;
  require(L != NULL);
  lua_gc(L, LUA_GCINC);
  nursery_peer = lua_newthread(L);
  lua_gc(L, LUA_GCCOLLECT);
  lua_lock(L);
  luaE_setdebt(G(L), MAX_LMEM);
  __atomic_store_n(&G(L)->pacer.hard, MAX_LMEM, __ATOMIC_RELAXED);
  lua_unlock(L);

  lua_createtable(L, 256, 0);
  table = (Table *)lua_topointer(L, -1);
  nursery_slot = (LuaAllocBlock *)table - 1;
  nursery_backing = luaH_array(table) - table->asize;
  lua_pop(L, 1);
  for (unsigned i = 0; i < 64; i++) {
    lua_newtable(L);
    lua_pop(L, 1);
  }
  lua_lock(L);
  luaE_setdebt(G(L), 0);
  luaC_step(L);
  lua_unlock(L);
  require(nursery_backing == NULL);
  lua_close(L);
}

static int nursery_finalize(lua_State *L) {
  require(lua_type(L, 1) == LUA_TUSERDATA);
  nursery_finalizers++;
  return 0;
}

static void nursery_progress(void) {
  lua_State *L = luaL_newstate();
  require(L != NULL);
  lua_gc(L, LUA_GCINC);
  lua_newuserdatauv(L, 1, 0);
  lua_newtable(L);
  lua_pushcfunction(L, nursery_finalize);
  lua_setfield(L, -2, "__gc");
  lua_setmetatable(L, -2);
  require(
      luaL_loadstring(L, "for i=1,25000 do local discarded={i,tag=true} end") ==
      LUA_OK);
  lua_gc(L, LUA_GCCOLLECT);
  lua_remove(L, 1);
  lua_lock(L);
  luaE_setdebt(G(L), 32768);
  __atomic_store_n(&G(L)->pacer.hard, getgcbytes(G(L)) + 65536,
                   __ATOMIC_RELAXED);
  lua_unlock(L);
  require(lua_pcall(L, 0, 0, 0) == LUA_OK);
  require(nursery_finalizers == 1);
  lua_close(L);
}

static void table_growth(int generational, int stopped) {
  lua_State *L = luaL_newstate();
  uint64_t epoch;
  require(L != NULL);
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCSTOP);
  require(luaL_loadstring(L, "local t = ...; for i=1,200000 do t[i]=i end; "
                             "return t[1], t[200000]") == LUA_OK);
  lua_newtable(L);
  lua_gc(L, LUA_GCCOLLECT);
  if (!stopped)
    lua_gc(L, LUA_GCRESTART);
  lua_lock(L);
  require(!luaC_tracing(L) && !luaC_sweeping(L));
  luaE_setdebt(G(L), LUA_GC_ASSIST_BYTES);
  require(!luaC_needstep(L) && !luaC_cyclepressure(L));
  epoch = luaC_traceepoch(G(L));
  lua_unlock(L);
  require(lua_pcall(L, 1, 2, 0) == LUA_OK);
  require((luaC_traceepoch(G(L)) != epoch) == !stopped);
  require(lua_tointeger(L, -2) == 1 && lua_tointeger(L, -1) == 200000);
  lua_close(L);
}

static void managed_function(int generational) {
  lua_State *L = luaL_newstate();
  unsigned before;
  require(L != NULL);
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  luaQ_pushmanaged(L, identity, 0);
  lua_gc(L, LUA_GCCOLLECT);
  lua_lock(L);
  luaE_setdebt(G(L), 0);
  lua_unlock(L);
  before = stops;
  luaQ_pushmanaged(L, identity, 0);
  require(stops == before);
  require(lua_rawequal(L, 1, 2));
  lua_pushinteger(L, 42);
  require(lua_pcall(L, 1, 1, 0) == LUA_OK);
  require(lua_tointeger(L, -1) == 42);
  lua_close(L);
}

static void leaf_publication(int generational) {
  lua_State *L = luaL_newstate();
  lua_State *peer;
  TValue index;
  Table *array, *hash, *first, *second, *graph;
  require(L != NULL);
  lua_gc(L, generational ? LUA_GCGEN : LUA_GCINC);
  lua_gc(L, LUA_GCCOLLECT);
  lua_gc(L, LUA_GCSTOP);
  require(luaL_dostring(
              L, "local keep; for i=1,6000 do keep={keep} end\n"
                 "return {false}, {last=false}, {number=42,ratio=1.25},\n"
                 "       {number=-7,ratio=2.5}, {child={},number=3}, keep") ==
          LUA_OK);
  lua_pushliteral(L, "last");
  lua_lock(L);
  luaA_share(L, gcvalue(s2v(L->stack.p + 1)));
  luaA_share(L, gcvalue(s2v(L->stack.p + 2)));
  start_stopped_trace(L);
  require(luaC_tracing(L) && !luaW_stopped(&G(L)->world));
  array = hvalue(s2v(L->stack.p + 1));
  hash = hvalue(s2v(L->stack.p + 2));
  first = hvalue(s2v(L->stack.p + 3));
  second = hvalue(s2v(L->stack.p + 4));
  graph = hvalue(s2v(L->stack.p + 5));
  setivalue(&index, 1);
  require(!luaH_tryset(L, array, &index, s2v(L->stack.p + 5), 1));
  require(luaA_localtable(L, graph));
  require(luaH_tryset(L, array, &index, s2v(L->stack.p + 3), 1));
  require(luaH_tryset(L, hash, s2v(L->stack.p + 7), s2v(L->stack.p + 4), 0));
  require(first->sharing == LUA_OBJECT_SHARED &&
          second->sharing == LUA_OBJECT_SHARED);
  lua_unlock(L);
  lua_settop(L, 2); /* Only the published slots retain these children. */
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_rawgeti(L, 1, 1) == LUA_TTABLE);
  require(lua_getfield(L, -1, "number") == LUA_TNUMBER);
  require(lua_tointeger(L, -1) == 42);
  lua_pop(L, 1);
  require(lua_getfield(L, -1, "ratio") == LUA_TNUMBER);
  require(lua_tonumber(L, -1) == 1.25);
  lua_pop(L, 2);
  require(lua_getfield(L, 2, "last") == LUA_TTABLE);
  require(lua_getfield(L, -1, "number") == LUA_TNUMBER);
  require(lua_tointeger(L, -1) == -7);
  lua_pop(L, 1);
  require(lua_getfield(L, -1, "ratio") == LUA_TNUMBER);
  require(lua_tonumber(L, -1) == 2.5);
  lua_pop(L, 1);
  peer = lua_newthread(L);
  lua_pushvalue(L, 3);
  lua_xmove(L, peer, 1);
  lua_pushliteral(peer, "changed");
  lua_setfield(peer, 1, "ratio");
  lua_gc(L, LUA_GCCOLLECT);
  require(lua_getfield(L, 3, "ratio") == LUA_TSTRING);
  require(strcmp(lua_tostring(L, -1), "changed") == 0);
  lua_close(L);
}

int main(int argc, char **argv) {
  l_mem basic, overdue;
  require(argc == 2);
  if (strncmp(argv[1], "table_growth_", 13) == 0) {
    table_growth(strstr(argv[1], "generational") != NULL,
                 strstr(argv[1], "stopped") != NULL);
    return 0;
  }
  if (strncmp(argv[1], "managed_function_", 17) == 0) {
    managed_function(strcmp(argv[1], "managed_function_generational") == 0);
    return 0;
  }
  if (strncmp(argv[1], "leaf_publication_", 17) == 0) {
    leaf_publication(strstr(argv[1], "generational") != NULL);
    return 0;
  }
  if (strcmp(argv[1], "marking_edges") == 0) {
    marking_edges();
    return 0;
  }
  if (strcmp(argv[1], "cohort_promotion") == 0) {
    cohort_promotion();
    return 0;
  }
  if (strcmp(argv[1], "sweep_pressure") == 0) {
    require(progress(0, SWEEP_PRESSURE) != 0);
    return 0;
  }
  if (strcmp(argv[1], "sweep_poll_pressure") == 0) {
    require(progress(0, SWEEP_POLL_PRESSURE) > 0);
    return 0;
  }
  if (strcmp(argv[1], "sweep_credit") == 0) {
    require(progress(0, SWEEP_CREDIT) == 0);
    return 0;
  }
  if (strncmp(argv[1], "closing_", 8) == 0) {
    closing_pressure(argv[1]);
    return 0;
  }
  if (strcmp(argv[1], "sweep_growth") == 0) {
    sweep_growth();
    return 0;
  }
  if (strcmp(argv[1], "sweep_inline") == 0 ||
      strcmp(argv[1], "sweep_pooled") == 0) {
    sweep_pooled(strcmp(argv[1], "sweep_pooled") == 0);
    return 0;
  }
  if (strncmp(argv[1], "nursery_", 8) == 0) {
    if (strcmp(argv[1], "nursery_backing") == 0)
      nursery_backing_wait();
    else if (strcmp(argv[1], "nursery_progress") == 0)
      nursery_progress();
    else
      nursery(strstr(argv[1], "generational") != NULL);
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
  if (strncmp(argv[1], "born_black_", 11) == 0) {
    born_black(strstr(argv[1], "generational") != NULL);
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
