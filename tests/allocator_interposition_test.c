#include <lauxlib.h>
#include <lstate.h>
#include <stdlib.h>

static lua_State *probing;
static unsigned calls;

void *luaL_alloc(void *user, void *pointer, size_t old_size, size_t size) {
  (void)user;
  (void)old_size;
  if (probing != NULL) {
    if (__atomic_load_n(&G(probing)->allocator.owner, __ATOMIC_ACQUIRE) == NULL)
      abort();
    calls++;
  }
  if (size != 0)
    return realloc(pointer, size);
  free(pointer);
  return NULL;
}

int main(void) {
  lua_State *state = luaL_newstate();
  if (state == NULL || lua_getallocf(state, NULL) != luaL_alloc)
    abort();
  lua_gc(state, LUA_GCSTOP);
  probing = state;
  (void)lua_newuserdatauv(state, 4096, 0);
  probing = NULL;
  if (calls == 0)
    abort();
  lua_close(state);
  return 0;
}
