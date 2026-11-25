/* Reachability-based sharing, before publication. See lua.h for license. */
#define LUA_CORE
#include "lprefix.h"
#include "laccess.h"
#include "lfunc.h"
#include "lgc.h"
#include "ltable.h"


static void enqueue (lua_State *L, GCObject *object, GCObject **pending) {
  uintptr_t *state;
  uintptr_t owner;
  if (object == NULL || (state = luaA_sharing(object)) == NULL) return;
  owner = __atomic_load_n(state, __ATOMIC_ACQUIRE);
  if (owner & LUA_OBJECT_SHARED) return;
  lua_assert(owner == cast(uintptr_t, L));
  if (object->tt == LUA_VTABLE && luaC_tasktable(gco2t(object)))
    luaC_sealtask(L);
  /* The queue is a temporary root even for a graph reached through weak edges.
  ** Table ownership resolves the current weak cutoff before enqueueing them. */
  luaC_tracepointer(L, object);
  if (object->tt == LUA_VUPVAL) luaF_publishupval(L, gco2upv(object));
  if (*pending != NULL) luaC_objbarrier(L, object, *pending);
  __atomic_store_n(state, cast(uintptr_t, *pending) | LUA_OBJECT_SHARED,
                    __ATOMIC_RELEASE);
  *pending = object;
}

static void value (lua_State *L, const TValue *edge, GCObject **pending) {
  if (iscollectable(edge)) enqueue(L, gcvalue(edge), pending);
}

void luaA_share (lua_State *L, GCObject *object) {
  LuaSharingRoots roots;
  uintptr_t *initial;
  if (object == NULL) return;
  initial = luaA_sharing(object);
  if (initial == NULL) {
    if (luaC_taskstring(object)) luaC_sealtask(L);
    return;
  }
  if (__atomic_load_n(initial, __ATOMIC_ACQUIRE) & LUA_OBJECT_SHARED) return;
  roots.previous = L->sharing_roots;
  roots.pending = roots.current = NULL;
  L->sharing_roots = &roots;
  enqueue(L, object, &roots.pending);
  while (roots.pending != NULL) {
    GCObject *current = roots.pending;
    uintptr_t *state = luaA_sharing(current);
    roots.current = current;
    roots.pending = cast(GCObject *, __atomic_load_n(state, __ATOMIC_RELAXED) &
                                 ~LUA_OBJECT_SHARED);
    /* The temporary heap link is a GC edge too. Preserve it before removal,
    ** including if a new epoch began while this promoter was parked. */
    if (roots.pending != NULL) luaC_tracepointer(L, roots.pending);
    __atomic_store_n(state, LUA_OBJECT_SHARED, __ATOMIC_RELEASE);
    if (current->tt == LUA_VUPVAL) {
      /* Enqueue materialized an open binding before switching its storage. */
      value(L, &gco2upv(current)->value, &roots.pending);
    }
    else {
      LuaObjectScope scope;
      unsigned i;
      luaA_begin(L, current, &scope);
      switch (current->tt) {
        case LUA_VTABLE: {
          Table *table = gco2t(current);
          Table *keys = luaH_recordkeys(table->node);
          unsigned p, parts = luaH_hashparts(table);
          enqueue(L, cast(GCObject *, table->metatable), &roots.pending);
          if (keys != NULL) {
            uintptr_t tags = table->arraydata;
            Value *values = luaH_recordvalues(table, tags);
            lua_assert(luaH_recordbacking(tags) == NULL);
            for (i = 0; i < keys->asize; i++) {
              lu_byte tag = luaH_recordtag(tags, i);
              if (tag & BIT_ISCOLLECTABLE)
                enqueue(L, values[i].gc, &roots.pending);
              else {
                TValue value = {0};
                uint64_t packed;
                settt_(&value, tag);
                if (tag == LUA_VNUMINT) val_(&value) = values[i];
                packed = luaH_packrecord(&value);
                if (packed != LUA_RECORD_FROZEN) {
                  __atomic_store_n(&values[i].bits, packed, __ATOMIC_RELEASE);
                  luaH_setrecordtag(&table->arraydata, i, LUA_RECORD_PACKED);
                }
              }
            }
            break;
          }
          for (i = 0; i < table->asize; i++)
            if (*getArrTag(table, i) & BIT_ISCOLLECTABLE)
              enqueue(L, getArrVal(table, i)->gc, &roots.pending);
          for (p = 0; p < parts; ++p) {
            Node *node = luaH_hashnodes(table, p), *end = node + luaH_nodesize(node);
            for (; node < end; ++node) {
              if (!isempty(gval(node))) {
                if (keyiscollectable(node)) enqueue(L, gckey(node), &roots.pending);
                value(L, gval(node), &roots.pending);
              }
            }
          }
          break;
        }
        case LUA_VLCL: {
          LClosure *closure = gco2lcl(current);
          for (i = 0; i < closure->nupvalues; i++)
            enqueue(L, cast(GCObject *, closure->upvals[i]), &roots.pending);
          break;
        }
        case LUA_VCCL: {
          CClosure *closure = gco2ccl(current);
          for (i = 0; i < closure->nupvalues; i++)
            value(L, &closure->upvalue[i], &roots.pending);
          break;
        }
        default: {
          Udata *userdata = gco2u(current);
          enqueue(L, cast(GCObject *, userdata->metatable), &roots.pending);
          for (i = 0; i < userdata->nuvalue; i++)
            value(L, &userdata->uv[i].uv, &roots.pending);
          break;
        }
      }
      luaA_end(L, &scope);
    }
    roots.current = NULL;
  }
  L->sharing_roots = roots.previous;
}

void luaA_sharevalue (lua_State *L, const TValue *edge) {
  if (iscollectable(edge)) luaA_share(L, gcvalue(edge));
}

void luaA_publish (lua_State *L, GCObject *owner, const TValue *edge) {
  uintptr_t *state = luaA_sharing(owner);
  if (__atomic_load_n(state, __ATOMIC_ACQUIRE) & LUA_OBJECT_SHARED)
    luaA_sharevalue(L, edge);
}
