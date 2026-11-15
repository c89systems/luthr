#include <luthrum/lua.h>

#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static const char *luthrum_lua_facade_pushvfstring(luthrum_lua_state *state,
                                                   const char *format, ...);

static int reject_allocations;

struct external_string_context {
  char *pointer;
  size_t size;
  int releases;
  int failed;
};

static void *release_external_string(void *user, void *pointer, size_t old_size,
                                     size_t new_size) {
  struct external_string_context *context;
  context = (struct external_string_context *)user;
  context->failed |= pointer != context->pointer || old_size != context->size ||
                     new_size != 0U;
  context->releases++;
  free(pointer);
  context->pointer = NULL;
  return NULL;
}

static int external_string_contract(void) {
  static const char *const samples[] = {
      "", "short external string",
      "a long external string used as a table key with ownership retained "
      "until its exactly-once release callback"};
  struct external_string_context context;
  luthrum_lua_state *state;
  unsigned int i;
  int failed;
  failed = 0;
  for (i = 0U; i < sizeof(samples) / sizeof(samples[0]); ++i) {
    memset(&context, 0, sizeof(context));
    context.size = strlen(samples[i]) + 1U;
    context.pointer = (char *)malloc(context.size);
    if (context.pointer == NULL)
      return 1;
    memcpy(context.pointer, samples[i], context.size);
    state = luthrum_lua_l_newstate();
    if (state == NULL) {
      free(context.pointer);
      return 1;
    }
    luthrum_lua_newtable(state);
    (void)luthrum_lua_pushexternalstring(state, samples[i], context.size - 1U,
                                         NULL, NULL);
    luthrum_lua_pushinteger(state, luthrum_lua_integer_make(0U, 42U));
    luthrum_lua_rawset(state, 1);
    (void)luthrum_lua_pushexternalstring(state, context.pointer,
                                         context.size - 1U,
                                         release_external_string, &context);
    (void)luthrum_lua_rawget(state, 1);
    failed |= luthrum_lua_integer_low(luthrum_lua_tointeger(state, -1)) != 42U;
    luthrum_lua_settop(state, 0);
    (void)luthrum_lua_gc(state, LUTHRUM_LUA_GCCOLLECT);
    (void)luthrum_lua_gc(state, LUTHRUM_LUA_GCCOLLECT);
    failed |= context.releases != 1 || context.failed;
    luthrum_lua_close(state);
    failed |= context.releases != 1 || context.failed;
  }
  return failed;
}

static void *format_test_allocator(void *user, void *pointer, size_t old_size,
                                   size_t new_size) {
  (void)user;
  if (new_size == 0U) {
    free(pointer);
    return NULL;
  }
  if (reject_allocations && new_size > old_size)
    return NULL;
  return realloc(pointer, new_size);
}

static int format_test_pushfstring(luthrum_lua_state *state) {
  reject_allocations = 1;
  (void)luthrum_lua_pushfstring(state, "value=%s", "allocation failure probe");
  return 1;
}

static int format_test_pushfstring_integer(luthrum_lua_state *state) {
  reject_allocations = 1;
  (void)luthrum_lua_pushfstring(state, "value=%I",
                                luthrum_lua_integer_make(0U, 12345U));
  return 1;
}

static int format_test_error_contract(void) {
  luthrum_lua_state *state;
  const char *result;
  int status;

  state = luthrum_lua_newstate(format_test_allocator, NULL, 0U);
  if (state == NULL)
    return 1;
  reject_allocations = 1;
  result = luthrum_lua_facade_pushvfstring(
      state, "value=%I", luthrum_lua_integer_make(0U, 12345U));
  reject_allocations = 0;
  if (result != NULL || luthrum_lua_gettop(state) != 1 ||
      strcmp(luthrum_lua_tostring(state, -1), "not enough memory") != 0) {
    luthrum_lua_close(state);
    return 2;
  }
  luthrum_lua_pop(state, 1);

  reject_allocations = 1;
  result = luthrum_lua_facade_pushvfstring(state, "value=%U", 65UL);
  reject_allocations = 0;
  if (result != NULL || luthrum_lua_gettop(state) != 1 ||
      strcmp(luthrum_lua_tostring(state, -1), "not enough memory") != 0) {
    luthrum_lua_close(state);
    return 5;
  }
  luthrum_lua_pop(state, 1);

  luthrum_lua_pushcfunction(state, format_test_pushfstring);
  status = luthrum_lua_pcall(state, 0, 1, 0);
  reject_allocations = 0;
  if (status != LUTHRUM_LUA_ERRMEM) {
    luthrum_lua_close(state);
    return 3;
  }
  luthrum_lua_pop(state, 1);

  luthrum_lua_pushcfunction(state, format_test_pushfstring_integer);
  status = luthrum_lua_pcall(state, 0, 1, 0);
  reject_allocations = 0;
  luthrum_lua_close(state);
  return status == LUTHRUM_LUA_ERRMEM ? 0 : 4;
}

static int luthrum_lua_facade_check_integer(luthrum_lua_integer value,
                                            unsigned int high,
                                            unsigned int low) {
  return luthrum_lua_integer_high(value) == high &&
         luthrum_lua_integer_low(value) == low;
}

static const char *luthrum_lua_facade_pushvfstring(luthrum_lua_state *state,
                                                   const char *format, ...) {
  va_list arguments;
  const char *result;

  va_start(arguments, format);
  result = luthrum_lua_pushvfstring(state, format, arguments);
  va_end(arguments);
  return result;
}

static int luthrum_lua_facade_error_with_location(luthrum_lua_state *state) {
  return luthrum_lua_l_error(state, "failure %I",
                             luthrum_lua_integer_make(0U, 7U));
}

int main(void) {
  luthrum_lua_buffer buffer;
  luthrum_lua_integer value;
  luthrum_lua_state *state;
  const char *string;

  state = luthrum_lua_l_newstate();
  if (state == 0) {
    return 1;
  }
  luthrum_lua_l_checkversion(state);
  if (luthrum_lua_l_loadstring(state, "return 2 + 3") != LUTHRUM_LUA_OK ||
      luthrum_lua_pcall(state, 0, 1, 0) != LUTHRUM_LUA_OK ||
      !luthrum_lua_facade_check_integer(luthrum_lua_tointeger(state, -1), 0U,
                                        5U)) {
    luthrum_lua_close(state);
    return 2;
  }
  luthrum_lua_pop(state, 1);

  string = luthrum_lua_facade_pushvfstring(
      state, "%I:%U", luthrum_lua_integer_make(0U, 1U), 65UL);
  if (string == 0 || strcmp(string, "1:A") != 0) {
    luthrum_lua_close(state);
    return 8;
  }
  luthrum_lua_pop(state, 1);

  string =
      luthrum_lua_pushfstring(state, "%I", luthrum_lua_integer_make(0U, 1U));
  if (string == 0 || strcmp(string, "1") != 0) {
    luthrum_lua_close(state);
    return 7;
  }
  luthrum_lua_pop(state, 1);

  string = luthrum_lua_pushfstring(state, "%U %q", 65UL);
  if (string == 0 || strcmp(string, "A %q") != 0 ||
      luthrum_lua_gettop(state) != 1) {
    luthrum_lua_close(state);
    return 9;
  }
  luthrum_lua_pop(state, 1);

  string = luthrum_lua_facade_pushvfstring(state, "%I %q",
                                           luthrum_lua_integer_make(0U, 1U));
  if (string == 0 || strcmp(string, "1 %q") != 0 ||
      luthrum_lua_gettop(state) != 1) {
    luthrum_lua_close(state);
    return 10;
  }
  luthrum_lua_pop(state, 1);

  luthrum_lua_pushcfunction(state, luthrum_lua_facade_error_with_location);
  luthrum_lua_setglobal(state, "fail");
  if (luthrum_lua_l_loadbuffer(state, "fail()", 6U, "@example.lua") !=
          LUTHRUM_LUA_OK ||
      luthrum_lua_pcall(state, 0, 0, 0) != LUTHRUM_LUA_ERRRUN) {
    luthrum_lua_close(state);
    return 11;
  }
  string = luthrum_lua_tostring(state, -1);
  if (string == 0 || strcmp(string, "example.lua:1: failure 7") != 0 ||
      luthrum_lua_gettop(state) != 1) {
    luthrum_lua_close(state);
    return 12;
  }
  luthrum_lua_pop(state, 1);

  luthrum_lua_newtable(state);
  luthrum_lua_pushinteger(state, luthrum_lua_integer_make(0U, 9U));
  luthrum_lua_rawseti(state, -2, luthrum_lua_integer_make(0U, 1U));
  if (!luthrum_lua_facade_check_integer(luthrum_lua_rawlen(state, -1), 0U,
                                        1U)) {
    luthrum_lua_close(state);
    return 3;
  }
  luthrum_lua_pop(state, 1);

  luthrum_lua_l_buffinit(state, &buffer);
  luthrum_lua_l_addchar(&buffer, 'a');
  luthrum_lua_l_addstring(&buffer, "bc");
  luthrum_lua_l_pushresult(&buffer);
  string = luthrum_lua_tostring(state, -1);
  if (string == 0 || strcmp(string, "abc") != 0) {
    luthrum_lua_close(state);
    return 4;
  }
  luthrum_lua_pop(state, 1);

  value = luthrum_lua_l_integer_add(luthrum_lua_integer_make(0U, 1U),
                                    luthrum_lua_integer_make(0U, 2U));
  if (!luthrum_lua_facade_check_integer(value, 0U, 3U) ||
      !luthrum_lua_facade_check_integer(
          luthrum_lua_l_integer_shift_left(luthrum_lua_integer_make(0U, 1U),
                                           32U),
          1U, 0U) ||
      !luthrum_lua_facade_check_integer(
          luthrum_lua_l_integer_shift_right(luthrum_lua_integer_make(1U, 0U),
                                            32U),
          0U, 1U) ||
      luthrum_lua_ident() == 0) {
    luthrum_lua_close(state);
    return 5;
  }

  if (luthrum_lua_gc(state, LUTHRUM_LUA_GCGEN) != LUTHRUM_LUA_GCINC ||
      luthrum_lua_gc(state, LUTHRUM_LUA_GCINC) != LUTHRUM_LUA_GCGEN ||
      luthrum_lua_gc(state, LUTHRUM_LUA_GCSTEP, (size_t)INT_MAX + 1U) < 0) {
    luthrum_lua_close(state);
    return 6;
  }

  luthrum_lua_l_openlibs(state);
  luthrum_lua_close(state);
  if (external_string_contract() != 0)
    return 18;
  return format_test_error_contract();
}
