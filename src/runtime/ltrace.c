/*
** Execution-owned root publication for concurrent tracing.
** See Copyright Notice in lua.h
*/
#define ltrace_c
#define LUA_CORE
#include "lprefix.h"

#include "lfunc.h"
#include "ldo.h"
#include "lgc.h"
#include "lmem.h"
#include "lstate.h"
#include "ltable.h"
#include "lvm.h"

static __thread LuaGCFrame *host_gcframe;

LuaGCFrame *luaC_gcframe (void) {
  LuaExecution *execution = luaQ_current();
  return execution != NULL ? execution->gc_frame : host_gcframe;
}

void luaC_restoregcframe (LuaGCFrame *frame) {
  LuaExecution *execution = luaQ_current();
  if (execution != NULL) execution->gc_frame = frame;
  else host_gcframe = frame;
}

int luaC_ingc (lua_State *L) {
  LuaGCFrame *frame;
  for (frame = luaC_gcframe(); frame != NULL; frame = frame->previous)
    if (frame->global == G(L)) return 1;
  return 0;
}

void luaC_entergc (lua_State *L, LuaGCFrame *frame) {
  frame->previous = luaC_gcframe();
  frame->global = G(L);
  L->native_depth++;
  luaC_restoregcframe(frame);
}

void luaC_leavegc (lua_State *L, LuaGCFrame *frame) {
  lua_assert(luaC_gcframe() == frame && L->native_depth != 0);
  luaC_restoregcframe(frame->previous);
  L->native_depth--;
}

void luaC_traceview (lua_State *L, const Table *view) {
  unsigned i;
  if (!luaC_tracing(L)) return;
  for (i = 0; i < view->asize; i++) {
    lu_byte tag = *getArrTag(view, i);
    if (tag & BIT_ISCOLLECTABLE)
      luaC_traceobject(L, getArrVal(view, i)->gc);
  }
  if (view->node == NULL || isdummy(view)) return;
  for (i = 0; i < sizenode(view); i++) {
    Node *node = gnode(view, i);
    if (!isempty(gval(node))) {
      if (keyiscollectable(node)) luaC_traceobject(L, gckey(node));
      luaC_tracevalue(L, gval(node));
    }
  }
}

/* The caller owns the builder's stack gate, or the prototype is frozen.
** This publishes referents, not shared mark-list mutations. */
void luaC_snapshotproto (lua_State *L, Proto *proto) {
  uint64_t epoch = G(L)->trace_epoch;
  int i;
  if (__atomic_load_n(&proto->trace_snapshot, __ATOMIC_ACQUIRE) == epoch)
    return;
  if (proto->source != NULL) luaC_traceobject(L, obj2gco(proto->source));
  for (i = 0; i < proto->sizek; i++) luaC_tracevalue(L, &proto->k[i]);
  for (i = 0; i < proto->sizeupvalues; i++)
    if (proto->upvalues[i].name != NULL)
      luaC_traceobject(L, obj2gco(proto->upvalues[i].name));
  for (i = 0; i < proto->sizep; i++)
    if (proto->p[i] != NULL) luaC_traceobject(L, obj2gco(proto->p[i]));
  for (i = 0; i < proto->sizelocvars; i++)
    if (proto->locvars[i].varname != NULL)
      luaC_traceobject(L, obj2gco(proto->locvars[i].varname));
  proto->trace_bytes = luaF_protosize(proto);
  __atomic_store_n(&proto->trace_snapshot, epoch, __ATOMIC_RELEASE);
}

/* The thread's execution owns all stack/native-root descriptors. No mutable
** referent is inspected here: capture cells and objects are separate work.
** The epoch publication makes both the snapshot and its barrier queues visible
** before a scanner can finish this root. Stacks stay black with the hybrid
** heap barrier; they do not need a second, globally paused traversal. */
void luaC_snapshot (lua_State *L, lua_State *thread) {
  uint64_t epoch = G(L)->trace_epoch;
  StkId slot;
  LuaNativeRoot *root;
  UpVal *upvalue;
  Proto *proto;
  if (__atomic_load_n(&thread->trace_snapshot, __ATOMIC_ACQUIRE) == epoch)
    return;
  luaC_traceobject(L, obj2gco(thread));
  if (thread->stack.p != NULL) {
    /* Cells, not their register shadows, own captured bindings. A shadow is a
    ** temporary root only while an operation can still use it (including a
    ** partial result at an allocation boundary). Do not retain an obsolete
    ** referent merely because the owner has not read this local again. */
    upvalue = thread->openupval;
    for (slot = thread->top.p; slot > thread->stack.p;) {
      ptrdiff_t level = savestack(thread, --slot);
      while (upvalue != NULL && upvalue->level > level)
        upvalue = upvalue->open.next;
      if (upvalue == NULL || upvalue->level != level ||
          luaV_shadowroot(thread, slot))
        luaC_tracevalue(L, s2v(slot));
    }
  }
  for (root = thread->native_roots; root != NULL; root = root->previous) {
    luaC_tracevalue(L, &root->value);
    if (root->table_view != NULL) luaC_traceview(L, root->table_view);
  }
  for (upvalue = thread->openupval; upvalue != NULL; upvalue = upvalue->open.next)
    luaC_traceobject(L, obj2gco(upvalue));
  for (proto = thread->building_protos; proto != NULL; proto = proto->build_next) {
    luaC_traceobject(L, obj2gco(proto));
    luaC_snapshotproto(L, proto);
  }
  if (thread->stack.p != NULL) {
    /* Obsolete slots must not retain dangling payloads after this cycle.
    ** Registers live across a safepoint were published through top first. */
    for (slot = thread->top.p; slot < thread->stack_last.p + EXTRA_STACK; slot++)
      setnilvalue2s(slot);
  }
  thread->trace_bytes = luaE_threadsize(thread);
  __atomic_store_n(&thread->trace_snapshot, epoch, __ATOMIC_RELEASE);
}

void luaC_checksnapshot (lua_State *L) {
  if (luaC_tracing(L)) {
    /* Paired operations own two gates but share the first participation word.
    ** An allocation wait on either stack must publish both root sets. */
    if (L->stack_peer != NULL) luaC_snapshot(L, L->stack_peer);
    luaC_snapshot(L, L);
  }
}

int luaC_snapshotparked (lua_State *L, lua_State *thread) {
  LuaWorld *world = &G(L)->world;
  int parked;
  luaX_checksync(pthread_mutex_lock(&world->mutex));
  parked = thread->root_waiting != 0;
  if (parked) luaC_snapshot(L, thread);
  luaX_checksync(pthread_mutex_unlock(&world->mutex));
  return parked;
}

void luaC_trimstack (lua_State *L) {
  if (L->trace_trimmed != L->trace_snapshot && L->world_depth == 0 &&
      L->object_scopes == NULL && !L->reclaiming && luaW_scope(L).heap == 0) {
    L->trace_trimmed = L->trace_snapshot;
    L->reclaiming = 1;
    luaD_shrinkstack(L);
    luaM_flushdebt(L);
    L->reclaiming = 0;
  }
}

lu_mem luaC_tracebatch (lua_State *L, unsigned limit) {
  global_State *g = G(L);
  LuaTraceWork work = {0};
  if (!luaC_tracing(L) || limit == 0) return 0;
  if (limit > LUA_TRACE_BATCH) limit = LUA_TRACE_BATCH;
  luaX_checksync(pthread_mutex_lock(&g->world.mutex));
  luaC_claimtrace(L, &work, limit);
  luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
  if (work.count == 0) return work.scanned;
  luaC_scantrace(L, &work, limit);
  luaX_checksync(pthread_mutex_lock(&g->world.mutex));
  luaC_publishtrace(L, &work);
  luaQ_wake(&g->world.changed, &g->world.waiters);
  luaX_checksync(pthread_mutex_unlock(&g->world.mutex));
  return cast(lu_mem, work.marked) + work.scanned;
}

static int weakdead (lua_State *L, GCObject *object) {
  if (object == NULL) return 0;
  if (novariant(object->tt) == LUA_TSTRING) {
    /* Lua strings behave as values in weak tables, including external ones. */
    luaC_traceobject(L, object);
    return 0;
  }
  return iswhite(object);
}

/* Weak reads first resolve the same decision as the collector, while holding
** the table's ownership. This prevents promoting an entry which a concurrent
** clearer has already classified as absent. Resolve each cutoff only once:
** every later insertion shades both referents before publishing its entry. */
void luaC_prepareweak (lua_State *L, Table *table) {
  unsigned cutoff = __atomic_load_n(&G(L)->trace_weakcutoff, __ATOMIC_ACQUIRE);
  unsigned pending, i;
  if (cutoff == 0 || table->weak_epoch != G(L)->trace_epoch) return;
  /* Tables discovered through finalization have not participated in the first
  ** weak-value decision. Their values must wait for resurrection's strong
  ** closure, which precedes the later key/value cutoff. */
  pending = ((cutoff & 2) ? cutoff : cutoff & ~table->weak_initial_cutoff);
  pending = (pending & table->gcweak) & ~table->weak_cleared;
  if (pending == 0) return;
  if (pending & 1) {
    for (i = 0; i < table->asize; i++) {
      lu_byte *tag = getArrTag(table, i);
      if ((*tag & BIT_ISCOLLECTABLE) &&
          weakdead(L, getArrVal(table, i)->gc))
        *tag = LUA_VEMPTY;
    }
  }
  for (i = 0; i < sizenode(table); i++) {
    Node *node = gnode(table, i);
    if (isempty(gval(node))) continue;
    if (((pending & 1) && iscollectable(gval(node)) &&
         weakdead(L, gcvalue(gval(node)))) ||
        ((pending & 2) && keyiscollectable(node) &&
         weakdead(L, gckey(node)))) {
      setempty(gval(node));
      if (keyiscollectable(node)) setdeadkey(node);
    }
  }
  table->weak_cleared |= cast_byte(pending);
}
