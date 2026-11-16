#include <luthr/lua.h>

#include <limits.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

static const char *luthr_lua_facade_pushvfstring(luthr_lua_state *state,
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
  luthr_lua_state *state;
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
    state = luthr_lua_l_newstate();
    if (state == NULL) {
      free(context.pointer);
      return 1;
    }
    luthr_lua_newtable(state);
    (void)luthr_lua_pushexternalstring(state, samples[i], context.size - 1U,
                                       NULL, NULL);
    luthr_lua_pushinteger(state, luthr_lua_integer_make(0U, 42U));
    luthr_lua_rawset(state, 1);
    (void)luthr_lua_pushexternalstring(state, context.pointer,
                                       context.size - 1U,
                                       release_external_string, &context);
    (void)luthr_lua_rawget(state, 1);
    failed |= luthr_lua_integer_low(luthr_lua_tointeger(state, -1)) != 42U;
    luthr_lua_settop(state, 0);
    (void)luthr_lua_gc(state, LUTHR_LUA_GCCOLLECT);
    (void)luthr_lua_gc(state, LUTHR_LUA_GCCOLLECT);
    failed |= context.releases != 1 || context.failed;
    luthr_lua_close(state);
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

static int format_test_pushfstring(luthr_lua_state *state) {
  reject_allocations = 1;
  (void)luthr_lua_pushfstring(state, "value=%s", "allocation failure probe");
  return 1;
}

static int format_test_pushfstring_integer(luthr_lua_state *state) {
  reject_allocations = 1;
  (void)luthr_lua_pushfstring(state, "value=%I",
                              luthr_lua_integer_make(0U, 12345U));
  return 1;
}

static int format_test_error_contract(void) {
  luthr_lua_state *state;
  const char *result;
  int status;

  state = luthr_lua_newstate(format_test_allocator, NULL, 0U);
  if (state == NULL)
    return 1;
  reject_allocations = 1;
  result = luthr_lua_facade_pushvfstring(state, "value=%I",
                                         luthr_lua_integer_make(0U, 12345U));
  reject_allocations = 0;
  if (result != NULL || luthr_lua_gettop(state) != 1 ||
      strcmp(luthr_lua_tostring(state, -1), "not enough memory") != 0) {
    luthr_lua_close(state);
    return 2;
  }
  luthr_lua_pop(state, 1);

  reject_allocations = 1;
  result = luthr_lua_facade_pushvfstring(state, "value=%U", 65UL);
  reject_allocations = 0;
  if (result != NULL || luthr_lua_gettop(state) != 1 ||
      strcmp(luthr_lua_tostring(state, -1), "not enough memory") != 0) {
    luthr_lua_close(state);
    return 5;
  }
  luthr_lua_pop(state, 1);

  luthr_lua_pushcfunction(state, format_test_pushfstring);
  status = luthr_lua_pcall(state, 0, 1, 0);
  reject_allocations = 0;
  if (status != LUTHR_LUA_ERRMEM) {
    luthr_lua_close(state);
    return 3;
  }
  luthr_lua_pop(state, 1);

  luthr_lua_pushcfunction(state, format_test_pushfstring_integer);
  status = luthr_lua_pcall(state, 0, 1, 0);
  reject_allocations = 0;
  luthr_lua_close(state);
  return status == LUTHR_LUA_ERRMEM ? 0 : 4;
}

static int luthr_lua_facade_check_integer(luthr_lua_integer value,
                                          unsigned int high, unsigned int low) {
  return luthr_lua_integer_high(value) == high &&
         luthr_lua_integer_low(value) == low;
}

static const char *luthr_lua_facade_pushvfstring(luthr_lua_state *state,
                                                 const char *format, ...) {
  va_list arguments;
  const char *result;

  va_start(arguments, format);
  result = luthr_lua_pushvfstring(state, format, arguments);
  va_end(arguments);
  return result;
}

static int luthr_lua_facade_error_with_location(luthr_lua_state *state) {
  return luthr_lua_l_error(state, "failure %I", luthr_lua_integer_make(0U, 7U));
}

int main(void) {
  luthr_lua_buffer buffer;
  luthr_lua_integer value;
  luthr_lua_state *state;
  const char *string;

  state = luthr_lua_l_newstate();
  if (state == 0) {
    return 1;
  }
  luthr_lua_l_checkversion(state);
  if (luthr_lua_l_loadstring(state, "return 2 + 3") != LUTHR_LUA_OK ||
      luthr_lua_pcall(state, 0, 1, 0) != LUTHR_LUA_OK ||
      !luthr_lua_facade_check_integer(luthr_lua_tointeger(state, -1), 0U, 5U)) {
    luthr_lua_close(state);
    return 2;
  }
  luthr_lua_pop(state, 1);

  string = luthr_lua_facade_pushvfstring(state, "%I:%U",
                                         luthr_lua_integer_make(0U, 1U), 65UL);
  if (string == 0 || strcmp(string, "1:A") != 0) {
    luthr_lua_close(state);
    return 8;
  }
  luthr_lua_pop(state, 1);

  string = luthr_lua_pushfstring(state, "%I", luthr_lua_integer_make(0U, 1U));
  if (string == 0 || strcmp(string, "1") != 0) {
    luthr_lua_close(state);
    return 7;
  }
  luthr_lua_pop(state, 1);

  string = luthr_lua_pushfstring(state, "%U %q", 65UL);
  if (string == 0 || strcmp(string, "A %q") != 0 ||
      luthr_lua_gettop(state) != 1) {
    luthr_lua_close(state);
    return 9;
  }
  luthr_lua_pop(state, 1);

  string = luthr_lua_facade_pushvfstring(state, "%I %q",
                                         luthr_lua_integer_make(0U, 1U));
  if (string == 0 || strcmp(string, "1 %q") != 0 ||
      luthr_lua_gettop(state) != 1) {
    luthr_lua_close(state);
    return 10;
  }
  luthr_lua_pop(state, 1);

  luthr_lua_pushcfunction(state, luthr_lua_facade_error_with_location);
  luthr_lua_setglobal(state, "fail");
  if (luthr_lua_l_loadbuffer(state, "fail()", 6U, "@example.lua") !=
          LUTHR_LUA_OK ||
      luthr_lua_pcall(state, 0, 0, 0) != LUTHR_LUA_ERRRUN) {
    luthr_lua_close(state);
    return 11;
  }
  string = luthr_lua_tostring(state, -1);
  if (string == 0 || strcmp(string, "example.lua:1: failure 7") != 0 ||
      luthr_lua_gettop(state) != 1) {
    luthr_lua_close(state);
    return 12;
  }
  luthr_lua_pop(state, 1);

  luthr_lua_newtable(state);
  luthr_lua_pushinteger(state, luthr_lua_integer_make(0U, 9U));
  luthr_lua_rawseti(state, -2, luthr_lua_integer_make(0U, 1U));
  if (!luthr_lua_facade_check_integer(luthr_lua_rawlen(state, -1), 0U, 1U)) {
    luthr_lua_close(state);
    return 3;
  }
  luthr_lua_pop(state, 1);

  luthr_lua_l_buffinit(state, &buffer);
  luthr_lua_l_addchar(&buffer, 'a');
  luthr_lua_l_addstring(&buffer, "bc");
  luthr_lua_l_pushresult(&buffer);
  string = luthr_lua_tostring(state, -1);
  if (string == 0 || strcmp(string, "abc") != 0) {
    luthr_lua_close(state);
    return 4;
  }
  luthr_lua_pop(state, 1);

  value = luthr_lua_l_integer_add(luthr_lua_integer_make(0U, 1U),
                                  luthr_lua_integer_make(0U, 2U));
  if (!luthr_lua_facade_check_integer(value, 0U, 3U) ||
      !luthr_lua_facade_check_integer(
          luthr_lua_l_integer_shift_left(luthr_lua_integer_make(0U, 1U), 32U),
          1U, 0U) ||
      !luthr_lua_facade_check_integer(
          luthr_lua_l_integer_shift_right(luthr_lua_integer_make(1U, 0U), 32U),
          0U, 1U) ||
      luthr_lua_ident() == 0) {
    luthr_lua_close(state);
    return 5;
  }

  if (luthr_lua_gc(state, LUTHR_LUA_GCGEN) != LUTHR_LUA_GCINC ||
      luthr_lua_gc(state, LUTHR_LUA_GCINC) != LUTHR_LUA_GCGEN ||
      luthr_lua_gc(state, LUTHR_LUA_GCSTEP, (size_t)INT_MAX + 1U) < 0) {
    luthr_lua_close(state);
    return 6;
  }

  luthr_lua_l_openlibs(state);
  luthr_lua_close(state);
  if (external_string_contract() != 0)
    return 18;
  return format_test_error_contract();
}
