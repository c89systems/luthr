#!/usr/bin/env python3
"""Generate the strict-C89, 64-bit Lua 5.5 public API boundary.

Lua 5.5's configured headers expose ``long long`` as lua_Integer.  This
generator preserves the complete callable Lua, lauxlib, and lualib surface
without requiring that non-C89 type in downstream headers.  The facade uses
two 32-bit words for Lua integer values and forwards to the pinned native Lua
library inside the generated implementation.
"""

import argparse
import pathlib
import re
import sys
from typing import Iterable, List, Sequence, Tuple


DECLARATION = re.compile(
    r"\b(?:LUA_API|LUALIB_API|LUAMOD_API)\s+"
    r"(?P<result>.*?)\(\s*(?P<name>lua(?:L|open)?_[A-Za-z0-9_]+)\s*\)"
    r"\s*\((?P<parameters>.*?)\)\s*;", re.DOTALL)

TYPE_REPLACEMENTS: Tuple[Tuple[str, str], ...] = (
    ("luaL_Buffer", "luthrum_lua_buffer"),
    ("luaL_Stream", "luthrum_lua_stream"),
    ("luaL_Reg", "luthrum_lua_reg"),
    ("lua_State", "luthrum_lua_state"),
    ("lua_Debug", "luthrum_lua_debug"),
    ("lua_Number", "luthrum_lua_number"),
    ("lua_Integer", "luthrum_lua_integer"),
    ("lua_Unsigned", "luthrum_lua_unsigned"),
    ("lua_KContext", "luthrum_lua_kcontext"),
    ("lua_CFunction", "luthrum_lua_c_function"),
    ("lua_KFunction", "luthrum_lua_k_function"),
    ("lua_Reader", "luthrum_lua_reader"),
    ("lua_Writer", "luthrum_lua_writer"),
    ("lua_Alloc", "luthrum_lua_alloc"),
    ("lua_WarnFunction", "luthrum_lua_warn_function"),
    ("lua_Hook", "luthrum_lua_hook"),
)


from generated_output_paths import validate_output_paths, write_generated_text


def strip_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)


def normalize(text: str) -> str:
    return " ".join(text.split())


def public_name(native_name: str) -> str:
    if native_name.startswith("luaL_"):
        return "luthrum_lua_l_" + native_name[len("luaL_"):]
    if native_name.startswith("luaopen_"):
        return "luthrum_lua_open_" + native_name[len("luaopen_"):]
    if native_name.startswith("lua_"):
        return "luthrum_lua_" + native_name[len("lua_"):]
    raise ValueError("unexpected Lua API name: " + native_name)


def transform(text: str) -> str:
    for native, public in TYPE_REPLACEMENTS:
        text = re.sub(r"\b" + re.escape(native) + r"\b", public, text)
    return text


def split_parameters(parameters: str) -> Iterable[str]:
    if normalize(parameters) == "void":
        return []
    depth = 0
    result: List[str] = []
    current: List[str] = []
    for character in parameters:
        if character == "(":
            depth += 1
        elif character == ")":
            depth -= 1
        if character == "," and depth == 0:
            result.append("".join(current).strip())
            current = []
        else:
            current.append(character)
    tail = "".join(current).strip()
    if tail:
        result.append(tail)
    return result


def parameter_type_and_name(parameter: str) -> Tuple[str, str]:
    callback = re.search(
        r"\(\s*\*\s*(?P<name>[A-Za-z_][A-Za-z0-9_]*)\s*\)", parameter)
    if callback:
        name = callback.group("name")
        return parameter.replace("*" + name, "*", 1), name
    match = re.match(r"(?P<type>.+?)(?P<name>[A-Za-z_][A-Za-z0-9_]*)"
                     r"\s*(?P<array>\[\s*\])?\s*$",
                     parameter.strip(), re.DOTALL)
    if not match:
        raise ValueError("cannot determine Lua parameter name: " + parameter)
    array = match.group("array") or ""
    return match.group("type").rstrip() + array, match.group("name")


def declarations(paths: Sequence[pathlib.Path]) -> List[Tuple[str, str, str]]:
    values: List[Tuple[str, str, str]] = []
    for path in paths:
        text = strip_comments(path.read_text(encoding="utf-8"))
        for match in DECLARATION.finditer(text):
            values.append((match.group("result"), match.group("name"),
                           match.group("parameters")))
    names = [item[1] for item in values]
    if len(values) != 156 or len(set(names)) != 156:
        raise ValueError("expected 156 unique Lua public functions, found {}".
                         format(len(set(names))))
    return values


def native_argument(native_type: str, name: str) -> str:
    compact = normalize(native_type)
    if compact == "va_list" or compact.endswith("[]"):
        return name
    if compact == "lua_Integer":
        return "luthrum_lua_native_integer(" + name + ")"
    if compact == "lua_Unsigned":
        return "luthrum_lua_native_unsigned(" + name + ")"
    return "({}){}".format(compact, name)


def definitions(items: Sequence[Tuple[str, str, str]]) -> str:
    special = {"lua_pushfstring", "lua_pushvfstring", "lua_gc", "luaL_error"}
    result: List[str] = []
    for native_result_text, native_name, native_parameters_text in items:
        if native_name in special:
            continue
        native_result = normalize(native_result_text)
        native_parameters = native_parameters_text.strip()
        public_result = transform(native_result_text).strip()
        public_parameters = transform(native_parameters).strip()
        arguments: List[str] = []
        for parameter in split_parameters(native_parameters):
            if parameter == "...":
                raise ValueError("unexpected variadic Lua API: " + native_name)
            native_type, name = parameter_type_and_name(parameter)
            arguments.append(native_argument(native_type, name))
        call = native_name + "(" + ", ".join(arguments) + ")"
        definition = "LUTHRUM_LUA_API {} {}({}) {{\n".format(
            public_result, public_name(native_name), public_parameters)
        if native_result == "void":
            definition += "  {};\n".format(call)
        elif native_result == "lua_Integer":
            definition += "  return luthrum_lua_integer_from_native({});\n".format(call)
        elif native_result == "lua_Unsigned":
            definition += "  return luthrum_lua_unsigned_from_native({});\n".format(call)
        else:
            definition += "  return ({}){};\n".format(public_result, call)
        definition += "}\n"
        result.append(definition)
    return "\n".join(result)


def convenience_header() -> str:
    return r"""/* C89 equivalents of every documented Lua convenience macro. */
#define LUTHRUM_LUA_SIGNATURE "\x1bLua"
#define LUTHRUM_LUA_VERSION_MAJOR "5"
#define LUTHRUM_LUA_VERSION_MINOR "5"
#define LUTHRUM_LUA_VERSION_RELEASE "1"
#define LUTHRUM_LUA_VERSION "Lua 5.5"
#define LUTHRUM_LUA_RELEASE "Lua 5.5.1"
#define LUTHRUM_LUA_COPYRIGHT LUTHRUM_LUA_RELEASE "  Copyright (C) 1994-2026 Lua.org, PUC-Rio"
#define LUTHRUM_LUA_AUTHORS "R. Ierusalimschy, L. H. de Figueiredo, W. Celes"
#define LUTHRUM_LUA_OPADD 0
#define LUTHRUM_LUA_OPSUB 1
#define LUTHRUM_LUA_OPMUL 2
#define LUTHRUM_LUA_OPMOD 3
#define LUTHRUM_LUA_OPPOW 4
#define LUTHRUM_LUA_OPDIV 5
#define LUTHRUM_LUA_OPIDIV 6
#define LUTHRUM_LUA_OPBAND 7
#define LUTHRUM_LUA_OPBOR 8
#define LUTHRUM_LUA_OPBXOR 9
#define LUTHRUM_LUA_OPSHL 10
#define LUTHRUM_LUA_OPSHR 11
#define LUTHRUM_LUA_OPUNM 12
#define LUTHRUM_LUA_OPBNOT 13
#define LUTHRUM_LUA_OPEQ 0
#define LUTHRUM_LUA_OPLT 1
#define LUTHRUM_LUA_OPLE 2
#define LUTHRUM_LUA_GCPMINORMUL 0
#define LUTHRUM_LUA_GCPMAJORMINOR 1
#define LUTHRUM_LUA_GCPMINORMAJOR 2
#define LUTHRUM_LUA_GCPPAUSE 3
#define LUTHRUM_LUA_GCPSTEPMUL 4
#define LUTHRUM_LUA_GCPSTEPSIZE 5
#define LUTHRUM_LUA_GCPN 6
#define LUTHRUM_LUA_N2SBUFFSZ 64
#define LUTHRUM_LUA_HOOKCALL 0
#define LUTHRUM_LUA_HOOKRET 1
#define LUTHRUM_LUA_HOOKLINE 2
#define LUTHRUM_LUA_HOOKCOUNT 3
#define LUTHRUM_LUA_HOOKTAILCALL 4
#define LUTHRUM_LUA_MASKCALL (1 << LUTHRUM_LUA_HOOKCALL)
#define LUTHRUM_LUA_MASKRET (1 << LUTHRUM_LUA_HOOKRET)
#define LUTHRUM_LUA_MASKLINE (1 << LUTHRUM_LUA_HOOKLINE)
#define LUTHRUM_LUA_MASKCOUNT (1 << LUTHRUM_LUA_HOOKCOUNT)
#define LUTHRUM_LUA_GNAME "_G"
#define LUTHRUM_LUA_ERRFILE (LUTHRUM_LUA_ERRERR + 1)
#define LUTHRUM_LUA_LOADED_TABLE "_LOADED"
#define LUTHRUM_LUA_PRELOAD_TABLE "_PRELOAD"
#define LUTHRUM_LUA_NOREF (-2)
#define LUTHRUM_LUA_REFNIL (-1)
#define LUTHRUM_LUA_FILEHANDLE "FILE*"
#define LUTHRUM_LUA_GLIBK 1
#define LUTHRUM_LUA_LOADLIBNAME "package"
#define LUTHRUM_LUA_LOADLIBK (LUTHRUM_LUA_GLIBK << 1)
#define LUTHRUM_LUA_COLIBNAME "coroutine"
#define LUTHRUM_LUA_COLIBK (LUTHRUM_LUA_LOADLIBK << 1)
#define LUTHRUM_LUA_DBLIBNAME "debug"
#define LUTHRUM_LUA_DBLIBK (LUTHRUM_LUA_COLIBK << 1)
#define LUTHRUM_LUA_IOLIBNAME "io"
#define LUTHRUM_LUA_IOLIBK (LUTHRUM_LUA_DBLIBK << 1)
#define LUTHRUM_LUA_MATHLIBNAME "math"
#define LUTHRUM_LUA_MATHLIBK (LUTHRUM_LUA_IOLIBK << 1)
#define LUTHRUM_LUA_OSLIBNAME "os"
#define LUTHRUM_LUA_OSLIBK (LUTHRUM_LUA_MATHLIBK << 1)
#define LUTHRUM_LUA_STRLIBNAME "string"
#define LUTHRUM_LUA_STRLIBK (LUTHRUM_LUA_OSLIBK << 1)
#define LUTHRUM_LUA_TABLIBNAME "table"
#define LUTHRUM_LUA_TABLIBK (LUTHRUM_LUA_STRLIBK << 1)
#define LUTHRUM_LUA_UTF8LIBNAME "utf8"
#define LUTHRUM_LUA_UTF8LIBK (LUTHRUM_LUA_TABLIBK << 1)
#define LUTHRUM_LUA_VERSUFFIX "_5_5"
#define LUTHRUM_LUA_L_NUMSIZES (sizeof(luthrum_lua_integer) * 16 + sizeof(luthrum_lua_number))
#define LUTHRUM_LUA_MAX_INTEGER luthrum_lua_integer_make(0x7fffffffU, 0xffffffffU)
#define LUTHRUM_LUA_MIN_INTEGER luthrum_lua_integer_make(0x80000000U, 0U)
#define LUTHRUM_LUA_MAX_UNSIGNED luthrum_lua_unsigned_make(0xffffffffU, 0xffffffffU)

/** Raises a Lua error if the loaded Lua ABI does not match this facade. */
LUTHRUM_LUA_API void luthrum_lua_l_checkversion(luthrum_lua_state *state);
/** Appends one character to an auxiliary string buffer. */
LUTHRUM_LUA_API void luthrum_lua_l_addchar(luthrum_lua_buffer *buffer_ptr,
                                     char character);
/** Adds two exact two-word Lua integers. */
LUTHRUM_LUA_API luthrum_lua_integer luthrum_lua_l_integer_add(luthrum_lua_integer left,
                                                       luthrum_lua_integer right);
/** Subtracts two exact two-word Lua integers. */
LUTHRUM_LUA_API luthrum_lua_integer luthrum_lua_l_integer_subtract(luthrum_lua_integer left,
                                                            luthrum_lua_integer right);
/** Multiplies two exact two-word Lua integers. */
LUTHRUM_LUA_API luthrum_lua_integer luthrum_lua_l_integer_multiply(luthrum_lua_integer left,
                                                            luthrum_lua_integer right);
/** Computes bitwise AND of two Lua integers. */
LUTHRUM_LUA_API luthrum_lua_integer luthrum_lua_l_integer_bit_and(luthrum_lua_integer left,
                                                           luthrum_lua_integer right);
/** Computes bitwise OR of two Lua integers. */
LUTHRUM_LUA_API luthrum_lua_integer luthrum_lua_l_integer_bit_or(luthrum_lua_integer left,
                                                          luthrum_lua_integer right);
/** Computes bitwise XOR of two Lua integers. */
LUTHRUM_LUA_API luthrum_lua_integer luthrum_lua_l_integer_bit_xor(luthrum_lua_integer left,
                                                           luthrum_lua_integer right);
/** Shifts a Lua integer left by count bits. */
LUTHRUM_LUA_API luthrum_lua_integer luthrum_lua_l_integer_shift_left(luthrum_lua_integer value,
                                                              unsigned int count);
/** Shifts a Lua integer right by count bits. */
LUTHRUM_LUA_API luthrum_lua_integer luthrum_lua_l_integer_shift_right(luthrum_lua_integer value,
                                                               unsigned int count);

#define luthrum_lua_upvalueindex(index) (LUTHRUM_LUA_REGISTRYINDEX - (index))
#define luthrum_lua_call(state, arguments, results) \
  luthrum_lua_callk((state), (arguments), (results), (luthrum_lua_kcontext)0, 0)
#define luthrum_lua_pcall(state, arguments, results, error_function) \
  luthrum_lua_pcallk((state), (arguments), (results), (error_function), \
                  (luthrum_lua_kcontext)0, 0)
#define luthrum_lua_yield(state, results) \
  luthrum_lua_yieldk((state), (results), (luthrum_lua_kcontext)0, 0)
#define luthrum_lua_getextraspace(state) ((void *)((char *)(state) - sizeof(void *)))
#define luthrum_lua_tonumber(state, index) luthrum_lua_tonumberx((state), (index), 0)
#define luthrum_lua_tointeger(state, index) luthrum_lua_tointegerx((state), (index), 0)
#define luthrum_lua_pop(state, count) luthrum_lua_settop((state), -(count) - 1)
#define luthrum_lua_newtable(state) luthrum_lua_createtable((state), 0, 0)
#define luthrum_lua_pushcfunction(state, function) \
  luthrum_lua_pushcclosure((state), (function), 0)
#define luthrum_lua_register(state, name, function) \
  (luthrum_lua_pushcfunction((state), (function)), luthrum_lua_setglobal((state), (name)))
#define luthrum_lua_isfunction(state, index) \
  (luthrum_lua_type((state), (index)) == LUTHRUM_LUA_TFUNCTION)
#define luthrum_lua_istable(state, index) \
  (luthrum_lua_type((state), (index)) == LUTHRUM_LUA_TTABLE)
#define luthrum_lua_islightuserdata(state, index) \
  (luthrum_lua_type((state), (index)) == LUTHRUM_LUA_TLIGHTUSERDATA)
#define luthrum_lua_isnil(state, index) \
  (luthrum_lua_type((state), (index)) == LUTHRUM_LUA_TNIL)
#define luthrum_lua_isboolean(state, index) \
  (luthrum_lua_type((state), (index)) == LUTHRUM_LUA_TBOOLEAN)
#define luthrum_lua_isthread(state, index) \
  (luthrum_lua_type((state), (index)) == LUTHRUM_LUA_TTHREAD)
#define luthrum_lua_isnone(state, index) \
  (luthrum_lua_type((state), (index)) == LUTHRUM_LUA_TNONE)
#define luthrum_lua_isnoneornil(state, index) (luthrum_lua_type((state), (index)) <= 0)
#define luthrum_lua_pushliteral(state, string) luthrum_lua_pushstring((state), "" string)
#define luthrum_lua_pushglobaltable(state) \
  ((void)luthrum_lua_rawgeti((state), LUTHRUM_LUA_REGISTRYINDEX, \
                           luthrum_lua_integer_make(0U, LUTHRUM_LUA_RIDX_GLOBALS)))
#define luthrum_lua_tostring(state, index) luthrum_lua_tolstring((state), (index), 0)
#define luthrum_lua_insert(state, index) luthrum_lua_rotate((state), (index), 1)
#define luthrum_lua_remove(state, index) \
  (luthrum_lua_rotate((state), (index), -1), luthrum_lua_pop((state), 1))
#define luthrum_lua_replace(state, index) \
  (luthrum_lua_copy((state), -1, (index)), luthrum_lua_pop((state), 1))
#define luthrum_lua_newuserdata(state, size) luthrum_lua_newuserdatauv((state), (size), 1)
#define luthrum_lua_getuservalue(state, index) luthrum_lua_getiuservalue((state), (index), 1)
#define luthrum_lua_setuservalue(state, index) luthrum_lua_setiuservalue((state), (index), 1)
#define luthrum_lua_resetthread(state) luthrum_lua_closethread((state), 0)
#define luthrum_lua_l_loadfile(state, filename) \
  luthrum_lua_l_loadfilex((state), (filename), 0)
#define luthrum_lua_l_loadbuffer(state, buffer, size, name) \
  luthrum_lua_l_loadbufferx((state), (buffer), (size), (name), 0)
#define luthrum_lua_l_checkstring(state, argument) \
  luthrum_lua_l_checklstring((state), (argument), 0)
#define luthrum_lua_l_optstring(state, argument, default_value) \
  luthrum_lua_l_optlstring((state), (argument), (default_value), 0)
#define luthrum_lua_l_typename(state, index) \
  luthrum_lua_typename((state), luthrum_lua_type((state), (index)))
#define luthrum_lua_l_dofile(state, filename) \
  (luthrum_lua_l_loadfile((state), (filename)) || \
   luthrum_lua_pcall((state), 0, LUTHRUM_LUA_MULTRET, 0))
#define luthrum_lua_l_dostring(state, string) \
  (luthrum_lua_l_loadstring((state), (string)) || \
   luthrum_lua_pcall((state), 0, LUTHRUM_LUA_MULTRET, 0))
#define luthrum_lua_l_getmetatable(state, name) \
  luthrum_lua_getfield((state), LUTHRUM_LUA_REGISTRYINDEX, (name))
#define luthrum_lua_l_opt(state, function, argument, default_value) \
  (luthrum_lua_isnoneornil((state), (argument)) ? (default_value) : \
   (function)((state), (argument)))
#define luthrum_lua_l_newlibtable(state, registration) \
  luthrum_lua_createtable((state), 0, \
                       (int)(sizeof(registration) / sizeof((registration)[0]) - 1))
#define luthrum_lua_l_newlib(state, registration) \
  (luthrum_lua_l_checkversion((state)), luthrum_lua_l_newlibtable((state), (registration)), \
   luthrum_lua_l_setfuncs((state), (registration), 0))
#define luthrum_lua_l_argcheck(state, condition, argument, message) \
  ((void)((condition) || luthrum_lua_l_argerror((state), (argument), (message))))
#define luthrum_lua_l_argexpected(state, condition, argument, type_name) \
  ((void)((condition) || luthrum_lua_l_typeerror((state), (argument), (type_name))))
#define luthrum_lua_l_pushfail(state) luthrum_lua_pushnil((state))
#define luthrum_lua_l_bufflen(buffer_ptr) ((buffer_ptr)->n)
#define luthrum_lua_l_buffaddr(buffer_ptr) ((buffer_ptr)->b)
#define luthrum_lua_l_addsize(buffer_ptr, size) ((buffer_ptr)->n += (size))
#define luthrum_lua_l_buffsub(buffer_ptr, size) ((buffer_ptr)->n -= (size))
#define luthrum_lua_l_prepbuffer(buffer_ptr) \
  luthrum_lua_l_prepbuffsize((buffer_ptr), LUTHRUM_LUA_BUFFER_SIZE)
#define luthrum_lua_l_openlibs(state) luthrum_lua_l_openselectedlibs((state), ~0, 0)
"""


def header(items: Sequence[Tuple[str, str, str]]) -> str:
    prototypes: List[str] = []
    contracts = {
        "lua_newstate": "Creates an owned Lua state with the supplied allocator; close it with luthrum_lua_close().",
        "lua_close": "Closes a Lua state and releases its Lua-owned objects; the pointer becomes invalid.",
        "lua_newthread": "Pushes a new coroutine on the parent stack; it shares the parent's global state.",
        "lua_gettop": "Returns the number of values on the Lua stack.",
        "lua_settop": "Sets stack height; a negative index is relative to the stack top.",
        "lua_tointegerx": "Converts a stack value to an exact two-word integer; isnum reports conversion success.",
        "lua_tolstring": "Returns a Lua-owned string view and optionally its byte length; stack changes can invalidate it.",
        "lua_pushlstring": "Copies len bytes onto the Lua stack as a string; the returned view is Lua-owned.",
        "lua_pushexternalstring": "Pushes an external string; keep its NUL-terminated buffer unchanged while Lua uses it. Lua calls a supplied finalizer after use.",
        "lua_callk": "Calls a Lua function on the stack; errors may long-jump through the caller.",
        "lua_pcallk": "Calls a Lua function with protected error handling and a status result.",
        "lua_load": "Loads a chunk from a reader callback and leaves the compiled function or error on the stack.",
        "lua_dump": "Writes a compiled function through the writer callback; it does not pop the function.",
        "lua_gc": "Controls Lua garbage collection; option-specific arguments follow the Lua 5.5 API.",
        "luaL_newstate": "Creates an owned Lua state with the standard allocator; close it with luthrum_lua_close().",
        "luaL_loadfilex": "Loads a file as a chunk without running it; returns a Lua status code.",
        "luaL_loadbufferx": "Loads a byte buffer as a chunk without running it; returns a Lua status code.",
        "luaL_ref": "Stores the stack-top value in a table and returns a registry reference.",
        "luaL_unref": "Releases a registry reference in the given table.",
        "luaL_openselectedlibs": "Loads or preloads the selected standard libraries according to the bitmasks.",
    }
    for result, name, parameters in items:
        public_result = transform(result).strip()
        public_parameters = transform(parameters).strip()
        description = contracts.get(name, "Uses the Lua 5.5 C API contract for {}.".format(name))
        anchor = name
        if name.startswith("luaopen_"):
            description = "Opens the {} standard library in the given state.".format(
                name[len("luaopen_"):])
            anchor = "6.1"
        elif name == "luaL_checkversion_":
            description = "Checks the Lua ABI version and numeric type sizes; raises a Lua error on mismatch."
            anchor = "luaL_checkversion"
        prototypes.append("/** {}\n * @see https://www.lua.org/manual/5.5/manual.html#{}\n */".format(
            description, anchor))
        prototypes.append("LUTHRUM_LUA_API {} {}({});".format(
            public_result, public_name(name), public_parameters))
    return """/* Generated by tools/generate_lua_c89_facade.py; do not edit. */
#ifndef LUTHRUM_LUA_H
#define LUTHRUM_LUA_H

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#if defined(_WIN32) && defined(LUTHRUM_LUA_BUILDING_SHARED)
#define LUTHRUM_LUA_API __declspec(dllexport)
#elif defined(_WIN32) && !defined(LUTHRUM_LUA_STATIC)
#define LUTHRUM_LUA_API __declspec(dllimport)
#elif defined(__GNUC__) || defined(__clang__)
#define LUTHRUM_LUA_API __attribute__((visibility("default")))
#else
#define LUTHRUM_LUA_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LUTHRUM_LUA_VERSION_MAJOR_N 5
#define LUTHRUM_LUA_VERSION_MINOR_N 5
#define LUTHRUM_LUA_VERSION_RELEASE_N 1
#define LUTHRUM_LUA_VERSION_NUM 505
#define LUTHRUM_LUA_VERSION_RELEASE_NUM 50501
#define LUTHRUM_LUA_ABI_VERSION 0
#define LUTHRUM_LUA_MULTRET (-1)
#define LUTHRUM_LUA_REGISTRYINDEX (-(INT_MAX / 2 + 1000))
#define LUTHRUM_LUA_UPVALUEINDEX(index) (LUTHRUM_LUA_REGISTRYINDEX - (index))
#define LUTHRUM_LUA_OK 0
#define LUTHRUM_LUA_YIELD 1
#define LUTHRUM_LUA_ERRRUN 2
#define LUTHRUM_LUA_ERRSYNTAX 3
#define LUTHRUM_LUA_ERRMEM 4
#define LUTHRUM_LUA_ERRERR 5
#define LUTHRUM_LUA_TNONE (-1)
#define LUTHRUM_LUA_TNIL 0
#define LUTHRUM_LUA_TBOOLEAN 1
#define LUTHRUM_LUA_TLIGHTUSERDATA 2
#define LUTHRUM_LUA_TNUMBER 3
#define LUTHRUM_LUA_TSTRING 4
#define LUTHRUM_LUA_TTABLE 5
#define LUTHRUM_LUA_TFUNCTION 6
#define LUTHRUM_LUA_TUSERDATA 7
#define LUTHRUM_LUA_TTHREAD 8
#define LUTHRUM_LUA_NUMTYPES 9
#define LUTHRUM_LUA_MINSTACK 20
#define LUTHRUM_LUA_RIDX_GLOBALS 2
#define LUTHRUM_LUA_RIDX_MAINTHREAD 3
#define LUTHRUM_LUA_RIDX_LAST 3
#define LUTHRUM_LUA_GCSTOP 0
#define LUTHRUM_LUA_GCRESTART 1
#define LUTHRUM_LUA_GCCOLLECT 2
#define LUTHRUM_LUA_GCCOUNT 3
#define LUTHRUM_LUA_GCCOUNTB 4
#define LUTHRUM_LUA_GCSTEP 5
#define LUTHRUM_LUA_GCISRUNNING 6
#define LUTHRUM_LUA_GCGEN 7
#define LUTHRUM_LUA_GCINC 8
#define LUTHRUM_LUA_GCPARAM 9
#define LUTHRUM_LUA_IDSIZE 60
#define LUTHRUM_LUA_BUFFER_SIZE (16 * sizeof(void *) * sizeof(double))

/** Opaque Lua VM state; close the owner with luthrum_lua_close(). */
typedef struct luthrum_lua_state luthrum_lua_state;
/** Lua numeric value; this bundle configures it as double. */
typedef double luthrum_lua_number;
/** Opaque C continuation context passed unchanged to its callback. */
typedef ptrdiff_t luthrum_lua_kcontext;
/** Exact 64-bit Lua integer bits as high and low 32-bit words. */
typedef struct luthrum_lua_integer {
  /** Most significant 32 bits, including the signed value's sign bit. */
  unsigned int high;
  /** Least significant 32 bits. */
  unsigned int low;
} luthrum_lua_integer;
/** Unsigned 64-bit Lua integer bits in the same two-word layout. */
typedef luthrum_lua_integer luthrum_lua_unsigned;
/** Lua C function; return the number of results left on the stack. */
typedef int (*luthrum_lua_c_function)(luthrum_lua_state *state);
/** Continuation callback for a yielded protected call or call. */
typedef int (*luthrum_lua_k_function)(luthrum_lua_state *state, int status,
                                   luthrum_lua_kcontext context);
/** Return a borrowed chunk valid until the next reader call; NULL or zero
 * size signals end of input. */
typedef const char *(*luthrum_lua_reader)(luthrum_lua_state *state, void *user,
                                       size_t *size);
/** Return zero on successful consumption of a chunk, nonzero on failure. */
typedef int (*luthrum_lua_writer)(luthrum_lua_state *state, const void *data,
                               size_t size, void *user);
/** Lua allocator: a zero new_size frees pointer; NULL signals failure. */
typedef void *(*luthrum_lua_alloc)(void *user, void *pointer, size_t old_size,
                                size_t new_size);
/** Receives warning fragments; to_continue marks an unfinished message. */
typedef void (*luthrum_lua_warn_function)(void *user, const char *message,
                                       int to_continue);

/** Hook/debug record; string fields are Lua-owned views. */
typedef struct luthrum_lua_debug {
  int event;
  const char *name;
  const char *namewhat;
  const char *what;
  const char *source;
  size_t srclen;
  int currentline;
  int linedefined;
  int lastlinedefined;
  unsigned char nups;
  unsigned char nparams;
  char isvararg;
  unsigned char extraargs;
  char istailcall;
  int ftransfer;
  int ntransfer;
  char short_src[LUTHRUM_LUA_IDSIZE];
  void *i_ci;
} luthrum_lua_debug;
/** Lua debug hook; record is borrowed for this callback invocation. */
typedef void (*luthrum_lua_hook)(luthrum_lua_state *state, luthrum_lua_debug *record);

/** Name/function record; registration arrays end with a NULL name. */
typedef struct luthrum_lua_reg {
  const char *name;
  luthrum_lua_c_function func;
} luthrum_lua_reg;
/** Auxiliary string builder; finish with a luthrum_lua_l_pushresult* operation. */
typedef struct luthrum_lua_buffer {
  char *b;
  size_t size;
  size_t n;
  luthrum_lua_state *L;
  union {
    long double alignment;
    void *pointer;
    char bytes[LUTHRUM_LUA_BUFFER_SIZE];
  } init;
} luthrum_lua_buffer;
/** File stream record used by Lua's standard I/O library. */
typedef struct luthrum_lua_stream {
  FILE *f;
  luthrum_lua_c_function closef;
} luthrum_lua_stream;

/** Packs exact 64-bit two's-complement bits from high and low words. */
LUTHRUM_LUA_API luthrum_lua_integer luthrum_lua_integer_make(unsigned int high,
                                                     unsigned int low);
/** Returns the high 32 bits of a Lua integer. */
LUTHRUM_LUA_API unsigned int luthrum_lua_integer_high(luthrum_lua_integer value);
/** Returns the low 32 bits of a Lua integer. */
LUTHRUM_LUA_API unsigned int luthrum_lua_integer_low(luthrum_lua_integer value);
/** Packs the high and low words of an unsigned Lua integer. */
LUTHRUM_LUA_API luthrum_lua_unsigned luthrum_lua_unsigned_make(unsigned int high,
                                                       unsigned int low);
/** Returns a static build-identification string; do not free it. */
LUTHRUM_LUA_API const char *luthrum_lua_ident(void);
/** Formats a string and pushes it on the Lua stack. */
LUTHRUM_LUA_API const char *luthrum_lua_pushfstring(luthrum_lua_state *state,
                                              const char *format, ...);
/** Controls Lua garbage collection; option-specific arguments follow Lua 5.5. */
LUTHRUM_LUA_API int luthrum_lua_gc(luthrum_lua_state *state, int option, ...);
/** Raises a formatted Lua error; does not return normally. */
LUTHRUM_LUA_API int luthrum_lua_l_error(luthrum_lua_state *state,
                                  const char *format, ...);

__LUTHRUM_LUA_PROTOTYPES__

__LUTHRUM_LUA_CONVENIENCE__

#ifdef __cplusplus
}
#endif

#endif /* LUTHRUM_LUA_H */
""".replace("__LUTHRUM_LUA_PROTOTYPES__", "\n".join(prototypes)).replace(
        "__LUTHRUM_LUA_CONVENIENCE__", convenience_header())


def special_definitions() -> str:
    return """LUTHRUM_LUA_API luthrum_lua_integer
luthrum_lua_integer_make(unsigned int high, unsigned int low)
{
  luthrum_lua_integer value;

  value.high = high;
  value.low = low;
  return value;
}

LUTHRUM_LUA_API unsigned int luthrum_lua_integer_high(luthrum_lua_integer value)
{
  return value.high;
}

LUTHRUM_LUA_API unsigned int luthrum_lua_integer_low(luthrum_lua_integer value)
{
  return value.low;
}

LUTHRUM_LUA_API luthrum_lua_unsigned
luthrum_lua_unsigned_make(unsigned int high, unsigned int low)
{
  return luthrum_lua_integer_make(high, low);
}

LUTHRUM_LUA_API const char *luthrum_lua_ident(void)
{
  return lua_ident;
}

static uint64_t luthrum_lua_integer_bits(luthrum_lua_integer value);
static lua_Integer luthrum_lua_native_integer(luthrum_lua_integer value);
typedef struct luthrum_lua_format_call {
  const char *format;
  va_list *arguments;
} luthrum_lua_format_call;
static const char *luthrum_lua_push_format_converted(lua_State *native_state,
                                                  const char *format,
                                                  va_list arguments);
static int luthrum_lua_push_format_protected(lua_State *native_state);
static const char *luthrum_lua_push_format(luthrum_lua_state *state,
                                         const char *format,
                                         va_list arguments);
static const char *luthrum_lua_push_memory_message(lua_State *native_state,
                                                 const char *format, ...);

LUTHRUM_LUA_API const char *
luthrum_lua_pushfstring(luthrum_lua_state *state, const char *format, ...)
{
  va_list arguments;
  const char *result;

  va_start(arguments, format);
  result = luthrum_lua_push_format(state, format, arguments);
  va_end(arguments);
  if (result == NULL) {
    lua_pushstring((lua_State *)state, "not enough memory");
    lua_error((lua_State *)state);
  }
  return result;
}

LUTHRUM_LUA_API const char *
luthrum_lua_pushvfstring(luthrum_lua_state *state, const char *format,
                      va_list arguments)
{
  return luthrum_lua_push_format(state, format, arguments);
}

static const char *
luthrum_lua_push_format(luthrum_lua_state *state, const char *format,
                     va_list arguments)
{
  lua_State *native_state;
  luthrum_lua_format_call call;
  va_list copied_arguments;
  int status;

  native_state = (lua_State *)state;
  if (strstr(format, "%I") == NULL && strstr(format, "%U") == NULL)
    return lua_pushvfstring(native_state, format, arguments);
  if (!lua_checkstack(native_state, 2)) {
    (void)luthrum_lua_push_memory_message(native_state, "%s", "not enough memory");
    return NULL;
  }
  call.format = format;
  __builtin_va_copy(copied_arguments, arguments);
  call.arguments = &copied_arguments;
  lua_pushcfunction(native_state, luthrum_lua_push_format_protected);
  lua_pushlightuserdata(native_state, &call);
  status = lua_pcall(native_state, 1, 1, 0);
  va_end(copied_arguments);
  if (status != LUA_OK)
    return NULL;
  return lua_tolstring(native_state, -1, NULL);
}

static const char *luthrum_lua_push_memory_message(lua_State *native_state,
                                                 const char *format, ...)
{
  va_list arguments;
  const char *message;

  va_start(arguments, format);
  message = lua_pushvfstring(native_state, format, arguments);
  va_end(arguments);
  return message;
}

static int luthrum_lua_push_format_protected(lua_State *native_state)
{
  luthrum_lua_format_call *call;

  call = (luthrum_lua_format_call *)lua_touserdata(native_state, 1);
  (void)luthrum_lua_push_format_converted(native_state, call->format,
                                       *call->arguments);
  return 1;
}

static const char *
luthrum_lua_push_format_converted(lua_State *native_state, const char *format,
                               va_list arguments)
{
  luaL_Buffer buffer;
  const char *cursor;

  luaL_buffinit(native_state, &buffer);
  cursor = format;
  while (*cursor != '\\0') {
    const char *percent = strchr(cursor, '%');
    if (percent == NULL) {
      luaL_addlstring(&buffer, cursor, strlen(cursor));
      break;
    }
    luaL_addlstring(&buffer, cursor, (size_t)(percent - cursor));
    cursor = percent + 1;
    if (*cursor == '\\0') {
      luaL_addchar(&buffer, '%');
      break;
    }
    switch (*cursor) {
      case '%': luaL_addchar(&buffer, '%'); break;
      case 's': lua_pushfstring(native_state, "%s", va_arg(arguments, char *)); break;
      case 'c': lua_pushfstring(native_state, "%c", va_arg(arguments, int)); break;
      case 'd': lua_pushfstring(native_state, "%d", va_arg(arguments, int)); break;
      case 'f': lua_pushfstring(native_state, "%f", va_arg(arguments, double)); break;
      case 'p': lua_pushfstring(native_state, "%p", va_arg(arguments, void *)); break;
      case 'I': lua_pushfstring(native_state, "%I", luthrum_lua_native_integer(va_arg(arguments, luthrum_lua_integer))); break;
      case 'U': lua_pushfstring(native_state, "%U",
                                va_arg(arguments, unsigned long)); break;
      default:
        luaL_addchar(&buffer, '%');
        luaL_addchar(&buffer, *cursor);
        ++cursor;
        continue;
    }
    if (*cursor != '%')
      luaL_addvalue(&buffer);
    ++cursor;
  }
  luaL_pushresult(&buffer);
  return lua_tolstring(native_state, -1, NULL);
}

LUTHRUM_LUA_API int luthrum_lua_l_error(luthrum_lua_state *state,
                                  const char *format, ...)
{
  va_list arguments;

  luaL_where((lua_State *)state, 1);
  va_start(arguments, format);
  (void)luthrum_lua_push_format(state, format, arguments);
  va_end(arguments);
  lua_concat((lua_State *)state, 2);
  return lua_error((lua_State *)state);
}

LUTHRUM_LUA_API int luthrum_lua_gc(luthrum_lua_state *state, int option, ...)
{
  va_list arguments;
  int first;
  int second;
  size_t step_size;

  va_start(arguments, option);
  if (option == LUA_GCSTEP) {
    step_size = va_arg(arguments, size_t);
    va_end(arguments);
    return lua_gc((lua_State *)state, option, step_size);
  }
  if (option == LUA_GCGEN) {
    va_end(arguments);
    return lua_gc((lua_State *)state, option);
  }
  if (option == LUA_GCINC) {
    va_end(arguments);
    return lua_gc((lua_State *)state, option);
  }
  if (option == LUA_GCPARAM) {
    first = va_arg(arguments, int);
    second = va_arg(arguments, int);
    va_end(arguments);
    return lua_gc((lua_State *)state, option, first, second);
  }
  va_end(arguments);
  return lua_gc((lua_State *)state, option);
}

LUTHRUM_LUA_API void luthrum_lua_l_checkversion(luthrum_lua_state *state)
{
  luaL_checkversion_((lua_State *)state, LUA_VERSION_NUM, LUAL_NUMSIZES);
}

LUTHRUM_LUA_API void
luthrum_lua_l_addchar(luthrum_lua_buffer *buffer_ptr, char character)
{
  if (buffer_ptr->n >= buffer_ptr->size) {
    (void)luthrum_lua_l_prepbuffsize(buffer_ptr, 1);
  }
  buffer_ptr->b[buffer_ptr->n++] = character;
}

LUTHRUM_LUA_API luthrum_lua_integer
luthrum_lua_l_integer_add(luthrum_lua_integer left, luthrum_lua_integer right)
{
  return luthrum_lua_integer_from_bits(luthrum_lua_integer_bits(left) +
                                    luthrum_lua_integer_bits(right));
}

LUTHRUM_LUA_API luthrum_lua_integer
luthrum_lua_l_integer_subtract(luthrum_lua_integer left, luthrum_lua_integer right)
{
  return luthrum_lua_integer_from_bits(luthrum_lua_integer_bits(left) -
                                    luthrum_lua_integer_bits(right));
}

LUTHRUM_LUA_API luthrum_lua_integer
luthrum_lua_l_integer_multiply(luthrum_lua_integer left, luthrum_lua_integer right)
{
  return luthrum_lua_integer_from_bits(luthrum_lua_integer_bits(left) *
                                    luthrum_lua_integer_bits(right));
}

LUTHRUM_LUA_API luthrum_lua_integer
luthrum_lua_l_integer_bit_and(luthrum_lua_integer left, luthrum_lua_integer right)
{
  return luthrum_lua_integer_from_bits(luthrum_lua_integer_bits(left) &
                                    luthrum_lua_integer_bits(right));
}

LUTHRUM_LUA_API luthrum_lua_integer
luthrum_lua_l_integer_bit_or(luthrum_lua_integer left, luthrum_lua_integer right)
{
  return luthrum_lua_integer_from_bits(luthrum_lua_integer_bits(left) |
                                    luthrum_lua_integer_bits(right));
}

LUTHRUM_LUA_API luthrum_lua_integer
luthrum_lua_l_integer_bit_xor(luthrum_lua_integer left, luthrum_lua_integer right)
{
  return luthrum_lua_integer_from_bits(luthrum_lua_integer_bits(left) ^
                                    luthrum_lua_integer_bits(right));
}

LUTHRUM_LUA_API luthrum_lua_integer
luthrum_lua_l_integer_shift_left(luthrum_lua_integer value, unsigned int count)
{
  if (count >= 64U) {
    return luthrum_lua_integer_make(0U, 0U);
  }
  return luthrum_lua_integer_from_bits(luthrum_lua_integer_bits(value) << count);
}

LUTHRUM_LUA_API luthrum_lua_integer
luthrum_lua_l_integer_shift_right(luthrum_lua_integer value, unsigned int count)
{
  if (count >= 64U) {
    return luthrum_lua_integer_make(0U, 0U);
  }
  return luthrum_lua_integer_from_bits(luthrum_lua_integer_bits(value) >> count);
}
"""


def source(items: Sequence[Tuple[str, str, str]]) -> str:
    return """/* Generated by tools/generate_lua_c89_facade.py; do not edit. */
#include <limits.h>
#include <stdint.h>
#include <stdarg.h>
#include <stddef.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <luthrum/lua.h>

typedef char luthrum_lua_unsigned_int_is_32_bits[
    (sizeof(unsigned int) * CHAR_BIT == 32) ? 1 : -1];
typedef char luthrum_lua_native_integer_is_64_bits[
    (sizeof(lua_Integer) * CHAR_BIT == 64) ? 1 : -1];
typedef char luthrum_lua_debug_layout_matches_native[
    (sizeof(luthrum_lua_debug) == sizeof(lua_Debug)) ? 1 : -1];
typedef char luthrum_lua_buffer_layout_matches_native[
    (sizeof(luthrum_lua_buffer) == sizeof(luaL_Buffer)) ? 1 : -1];
typedef char luthrum_lua_stream_layout_matches_native[
    (sizeof(luthrum_lua_stream) == sizeof(luaL_Stream)) ? 1 : -1];
typedef char luthrum_lua_reg_layout_matches_native[
    (sizeof(luthrum_lua_reg) == sizeof(luaL_Reg)) ? 1 : -1];

#define LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(public_type, native_type, field) \\
  typedef char luthrum_lua_layout_##public_type##_##field[ \\
      (offsetof(public_type, field) == offsetof(native_type, field)) ? 1 : -1]

LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_debug, lua_Debug, event);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_debug, lua_Debug, name);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_debug, lua_Debug, namewhat);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_debug, lua_Debug, what);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_debug, lua_Debug, source);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_debug, lua_Debug, srclen);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_debug, lua_Debug, currentline);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_debug, lua_Debug, linedefined);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_debug, lua_Debug, lastlinedefined);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_debug, lua_Debug, nups);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_debug, lua_Debug, nparams);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_debug, lua_Debug, isvararg);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_debug, lua_Debug, extraargs);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_debug, lua_Debug, istailcall);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_debug, lua_Debug, ftransfer);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_debug, lua_Debug, ntransfer);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_debug, lua_Debug, short_src);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_debug, lua_Debug, i_ci);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_buffer, luaL_Buffer, b);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_buffer, luaL_Buffer, size);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_buffer, luaL_Buffer, n);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_buffer, luaL_Buffer, L);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_buffer, luaL_Buffer, init);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_stream, luaL_Stream, f);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_stream, luaL_Stream, closef);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_reg, luaL_Reg, name);
LUTHRUM_LUA_ASSERT_LAYOUT_FIELD(luthrum_lua_reg, luaL_Reg, func);

#undef LUTHRUM_LUA_ASSERT_LAYOUT_FIELD

static uint64_t luthrum_lua_integer_bits(luthrum_lua_integer value)
{
  return ((uint64_t)value.high << 32) | (uint64_t)value.low;
}

static luthrum_lua_integer luthrum_lua_integer_from_bits(uint64_t bits)
{
  return luthrum_lua_integer_make((unsigned int)(bits >> 32),
                               (unsigned int)bits);
}

static lua_Integer luthrum_lua_native_integer(luthrum_lua_integer value)
{
  return (lua_Integer)luthrum_lua_integer_bits(value);
}

static luthrum_lua_integer luthrum_lua_integer_from_native(lua_Integer value)
{
  return luthrum_lua_integer_from_bits((uint64_t)value);
}

static luthrum_lua_unsigned luthrum_lua_unsigned_from_native(lua_Unsigned value)
{
  uint64_t bits;

  bits = (uint64_t)value;
  return luthrum_lua_unsigned_make((unsigned int)(bits >> 32),
                                (unsigned int)bits);
}

__LUTHRUM_LUA_SPECIAL_DEFINITIONS__

__LUTHRUM_LUA_DEFINITIONS__
""".replace("__LUTHRUM_LUA_SPECIAL_DEFINITIONS__", special_definitions()).replace(
        "__LUTHRUM_LUA_DEFINITIONS__", definitions(items))


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--include-dir", required=True, type=pathlib.Path)
    parser.add_argument("--header", required=True, type=pathlib.Path)
    parser.add_argument("--source", required=True, type=pathlib.Path)
    parser.add_argument("--runtime-header", required=True, type=pathlib.Path)
    parser.add_argument("--exports", required=True, type=pathlib.Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    validate_output_paths(args.header, args.source, args.exports)
    inputs = [args.include_dir / name for name in
              ("lua.h", "lauxlib.h", "lualib.h")]
    for path in inputs:
        if not path.is_file():
            raise ValueError("required input is missing: " + str(path))
    items = declarations(inputs)
    args.header.parent.mkdir(parents=True, exist_ok=True)
    args.source.parent.mkdir(parents=True, exist_ok=True)
    facade = header(items)
    write_generated_text(args.header, facade, encoding="utf-8")
    write_generated_text(args.source, source(items), encoding="utf-8")
    symbols = {item[1] for item in items}
    symbols.add("lua_ident")
    symbols.update(re.findall(
        r"^LUTHRUM_LUA_API\b[^;{}]*?\b(luthrum_lua_[A-Za-z0-9_]+)\s*\(",
        strip_comments(facade), flags=re.MULTILINE))
    symbols.update(re.findall(
        r"\b(luthrum_lua_runtime_[A-Za-z0-9_]+)\s*\(",
        strip_comments(args.runtime_header.read_text(encoding="utf-8"))))
    args.exports.parent.mkdir(parents=True, exist_ok=True)
    export_map = "{\n  global:\n" + "".join(
        "    " + name + ";\n" for name in sorted(symbols)) + "  local: *;\n};\n"
    write_generated_text(args.exports, export_map, encoding="utf-8")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError) as error:
        print("generate_lua_c89_facade.py: " + str(error), file=sys.stderr)
        raise SystemExit(1)
