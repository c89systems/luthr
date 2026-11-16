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
    ("luaL_Buffer", "luthr_lua_buffer"),
    ("luaL_Stream", "luthr_lua_stream"),
    ("luaL_Reg", "luthr_lua_reg"),
    ("lua_State", "luthr_lua_state"),
    ("lua_Debug", "luthr_lua_debug"),
    ("lua_Number", "luthr_lua_number"),
    ("lua_Integer", "luthr_lua_integer"),
    ("lua_Unsigned", "luthr_lua_unsigned"),
    ("lua_KContext", "luthr_lua_kcontext"),
    ("lua_CFunction", "luthr_lua_c_function"),
    ("lua_KFunction", "luthr_lua_k_function"),
    ("lua_Reader", "luthr_lua_reader"),
    ("lua_Writer", "luthr_lua_writer"),
    ("lua_Alloc", "luthr_lua_alloc"),
    ("lua_WarnFunction", "luthr_lua_warn_function"),
    ("lua_Hook", "luthr_lua_hook"),
)


from generated_output_paths import validate_output_paths, write_generated_text


def strip_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)


def normalize(text: str) -> str:
    return " ".join(text.split())


def public_name(native_name: str) -> str:
    if native_name.startswith("luaL_"):
        return "luthr_lua_l_" + native_name[len("luaL_"):]
    if native_name.startswith("luaopen_"):
        return "luthr_lua_open_" + native_name[len("luaopen_"):]
    if native_name.startswith("lua_"):
        return "luthr_lua_" + native_name[len("lua_"):]
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
        return "luthr_lua_native_integer(" + name + ")"
    if compact == "lua_Unsigned":
        return "luthr_lua_native_unsigned(" + name + ")"
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
        definition = "LUTHR_LUA_API {} {}({}) {{\n".format(
            public_result, public_name(native_name), public_parameters)
        if native_result == "void":
            definition += "  {};\n".format(call)
        elif native_result == "lua_Integer":
            definition += "  return luthr_lua_integer_from_native({});\n".format(call)
        elif native_result == "lua_Unsigned":
            definition += "  return luthr_lua_unsigned_from_native({});\n".format(call)
        else:
            definition += "  return ({}){};\n".format(public_result, call)
        definition += "}\n"
        result.append(definition)
    return "\n".join(result)


def convenience_header() -> str:
    return r"""/* C89 equivalents of every documented Lua convenience macro. */
#define LUTHR_LUA_SIGNATURE "\x1bLua"
#define LUTHR_LUA_VERSION_MAJOR "5"
#define LUTHR_LUA_VERSION_MINOR "5"
#define LUTHR_LUA_VERSION_RELEASE "1"
#define LUTHR_LUA_VERSION "Lua 5.5"
#define LUTHR_LUA_RELEASE "Lua 5.5.1"
#define LUTHR_LUA_COPYRIGHT LUTHR_LUA_RELEASE "  Copyright (C) 1994-2026 Lua.org, PUC-Rio"
#define LUTHR_LUA_AUTHORS "R. Ierusalimschy, L. H. de Figueiredo, W. Celes"
#define LUTHR_LUA_OPADD 0
#define LUTHR_LUA_OPSUB 1
#define LUTHR_LUA_OPMUL 2
#define LUTHR_LUA_OPMOD 3
#define LUTHR_LUA_OPPOW 4
#define LUTHR_LUA_OPDIV 5
#define LUTHR_LUA_OPIDIV 6
#define LUTHR_LUA_OPBAND 7
#define LUTHR_LUA_OPBOR 8
#define LUTHR_LUA_OPBXOR 9
#define LUTHR_LUA_OPSHL 10
#define LUTHR_LUA_OPSHR 11
#define LUTHR_LUA_OPUNM 12
#define LUTHR_LUA_OPBNOT 13
#define LUTHR_LUA_OPEQ 0
#define LUTHR_LUA_OPLT 1
#define LUTHR_LUA_OPLE 2
#define LUTHR_LUA_GCPMINORMUL 0
#define LUTHR_LUA_GCPMAJORMINOR 1
#define LUTHR_LUA_GCPMINORMAJOR 2
#define LUTHR_LUA_GCPPAUSE 3
#define LUTHR_LUA_GCPSTEPMUL 4
#define LUTHR_LUA_GCPSTEPSIZE 5
#define LUTHR_LUA_GCPN 6
#define LUTHR_LUA_N2SBUFFSZ 64
#define LUTHR_LUA_HOOKCALL 0
#define LUTHR_LUA_HOOKRET 1
#define LUTHR_LUA_HOOKLINE 2
#define LUTHR_LUA_HOOKCOUNT 3
#define LUTHR_LUA_HOOKTAILCALL 4
#define LUTHR_LUA_MASKCALL (1 << LUTHR_LUA_HOOKCALL)
#define LUTHR_LUA_MASKRET (1 << LUTHR_LUA_HOOKRET)
#define LUTHR_LUA_MASKLINE (1 << LUTHR_LUA_HOOKLINE)
#define LUTHR_LUA_MASKCOUNT (1 << LUTHR_LUA_HOOKCOUNT)
#define LUTHR_LUA_GNAME "_G"
#define LUTHR_LUA_ERRFILE (LUTHR_LUA_ERRERR + 1)
#define LUTHR_LUA_LOADED_TABLE "_LOADED"
#define LUTHR_LUA_PRELOAD_TABLE "_PRELOAD"
#define LUTHR_LUA_NOREF (-2)
#define LUTHR_LUA_REFNIL (-1)
#define LUTHR_LUA_FILEHANDLE "FILE*"
#define LUTHR_LUA_GLIBK 1
#define LUTHR_LUA_LOADLIBNAME "package"
#define LUTHR_LUA_LOADLIBK (LUTHR_LUA_GLIBK << 1)
#define LUTHR_LUA_COLIBNAME "coroutine"
#define LUTHR_LUA_COLIBK (LUTHR_LUA_LOADLIBK << 1)
#define LUTHR_LUA_DBLIBNAME "debug"
#define LUTHR_LUA_DBLIBK (LUTHR_LUA_COLIBK << 1)
#define LUTHR_LUA_IOLIBNAME "io"
#define LUTHR_LUA_IOLIBK (LUTHR_LUA_DBLIBK << 1)
#define LUTHR_LUA_MATHLIBNAME "math"
#define LUTHR_LUA_MATHLIBK (LUTHR_LUA_IOLIBK << 1)
#define LUTHR_LUA_OSLIBNAME "os"
#define LUTHR_LUA_OSLIBK (LUTHR_LUA_MATHLIBK << 1)
#define LUTHR_LUA_STRLIBNAME "string"
#define LUTHR_LUA_STRLIBK (LUTHR_LUA_OSLIBK << 1)
#define LUTHR_LUA_TABLIBNAME "table"
#define LUTHR_LUA_TABLIBK (LUTHR_LUA_STRLIBK << 1)
#define LUTHR_LUA_UTF8LIBNAME "utf8"
#define LUTHR_LUA_UTF8LIBK (LUTHR_LUA_TABLIBK << 1)
#define LUTHR_LUA_VERSUFFIX "_5_5"
#define LUTHR_LUA_L_NUMSIZES (sizeof(luthr_lua_integer) * 16 + sizeof(luthr_lua_number))
#define LUTHR_LUA_MAX_INTEGER luthr_lua_integer_make(0x7fffffffU, 0xffffffffU)
#define LUTHR_LUA_MIN_INTEGER luthr_lua_integer_make(0x80000000U, 0U)
#define LUTHR_LUA_MAX_UNSIGNED luthr_lua_unsigned_make(0xffffffffU, 0xffffffffU)

/** Raises a Lua error if the loaded Lua ABI does not match this facade. */
LUTHR_LUA_API void luthr_lua_l_checkversion(luthr_lua_state *state);
/** Appends one character to an auxiliary string buffer. */
LUTHR_LUA_API void luthr_lua_l_addchar(luthr_lua_buffer *buffer_ptr,
                                     char character);
/** Adds two exact two-word Lua integers. */
LUTHR_LUA_API luthr_lua_integer luthr_lua_l_integer_add(luthr_lua_integer left,
                                                       luthr_lua_integer right);
/** Subtracts two exact two-word Lua integers. */
LUTHR_LUA_API luthr_lua_integer luthr_lua_l_integer_subtract(luthr_lua_integer left,
                                                            luthr_lua_integer right);
/** Multiplies two exact two-word Lua integers. */
LUTHR_LUA_API luthr_lua_integer luthr_lua_l_integer_multiply(luthr_lua_integer left,
                                                            luthr_lua_integer right);
/** Computes bitwise AND of two Lua integers. */
LUTHR_LUA_API luthr_lua_integer luthr_lua_l_integer_bit_and(luthr_lua_integer left,
                                                           luthr_lua_integer right);
/** Computes bitwise OR of two Lua integers. */
LUTHR_LUA_API luthr_lua_integer luthr_lua_l_integer_bit_or(luthr_lua_integer left,
                                                          luthr_lua_integer right);
/** Computes bitwise XOR of two Lua integers. */
LUTHR_LUA_API luthr_lua_integer luthr_lua_l_integer_bit_xor(luthr_lua_integer left,
                                                           luthr_lua_integer right);
/** Shifts a Lua integer left by count bits. */
LUTHR_LUA_API luthr_lua_integer luthr_lua_l_integer_shift_left(luthr_lua_integer value,
                                                              unsigned int count);
/** Shifts a Lua integer right by count bits. */
LUTHR_LUA_API luthr_lua_integer luthr_lua_l_integer_shift_right(luthr_lua_integer value,
                                                               unsigned int count);

#define luthr_lua_upvalueindex(index) (LUTHR_LUA_REGISTRYINDEX - (index))
#define luthr_lua_call(state, arguments, results) \
  luthr_lua_callk((state), (arguments), (results), (luthr_lua_kcontext)0, 0)
#define luthr_lua_pcall(state, arguments, results, error_function) \
  luthr_lua_pcallk((state), (arguments), (results), (error_function), \
                  (luthr_lua_kcontext)0, 0)
#define luthr_lua_yield(state, results) \
  luthr_lua_yieldk((state), (results), (luthr_lua_kcontext)0, 0)
#define luthr_lua_getextraspace(state) ((void *)((char *)(state) - sizeof(void *)))
#define luthr_lua_tonumber(state, index) luthr_lua_tonumberx((state), (index), 0)
#define luthr_lua_tointeger(state, index) luthr_lua_tointegerx((state), (index), 0)
#define luthr_lua_pop(state, count) luthr_lua_settop((state), -(count) - 1)
#define luthr_lua_newtable(state) luthr_lua_createtable((state), 0, 0)
#define luthr_lua_pushcfunction(state, function) \
  luthr_lua_pushcclosure((state), (function), 0)
#define luthr_lua_register(state, name, function) \
  (luthr_lua_pushcfunction((state), (function)), luthr_lua_setglobal((state), (name)))
#define luthr_lua_isfunction(state, index) \
  (luthr_lua_type((state), (index)) == LUTHR_LUA_TFUNCTION)
#define luthr_lua_istable(state, index) \
  (luthr_lua_type((state), (index)) == LUTHR_LUA_TTABLE)
#define luthr_lua_islightuserdata(state, index) \
  (luthr_lua_type((state), (index)) == LUTHR_LUA_TLIGHTUSERDATA)
#define luthr_lua_isnil(state, index) \
  (luthr_lua_type((state), (index)) == LUTHR_LUA_TNIL)
#define luthr_lua_isboolean(state, index) \
  (luthr_lua_type((state), (index)) == LUTHR_LUA_TBOOLEAN)
#define luthr_lua_isthread(state, index) \
  (luthr_lua_type((state), (index)) == LUTHR_LUA_TTHREAD)
#define luthr_lua_isnone(state, index) \
  (luthr_lua_type((state), (index)) == LUTHR_LUA_TNONE)
#define luthr_lua_isnoneornil(state, index) (luthr_lua_type((state), (index)) <= 0)
#define luthr_lua_pushliteral(state, string) luthr_lua_pushstring((state), "" string)
#define luthr_lua_pushglobaltable(state) \
  ((void)luthr_lua_rawgeti((state), LUTHR_LUA_REGISTRYINDEX, \
                           luthr_lua_integer_make(0U, LUTHR_LUA_RIDX_GLOBALS)))
#define luthr_lua_tostring(state, index) luthr_lua_tolstring((state), (index), 0)
#define luthr_lua_insert(state, index) luthr_lua_rotate((state), (index), 1)
#define luthr_lua_remove(state, index) \
  (luthr_lua_rotate((state), (index), -1), luthr_lua_pop((state), 1))
#define luthr_lua_replace(state, index) \
  (luthr_lua_copy((state), -1, (index)), luthr_lua_pop((state), 1))
#define luthr_lua_newuserdata(state, size) luthr_lua_newuserdatauv((state), (size), 1)
#define luthr_lua_getuservalue(state, index) luthr_lua_getiuservalue((state), (index), 1)
#define luthr_lua_setuservalue(state, index) luthr_lua_setiuservalue((state), (index), 1)
#define luthr_lua_resetthread(state) luthr_lua_closethread((state), 0)
#define luthr_lua_l_loadfile(state, filename) \
  luthr_lua_l_loadfilex((state), (filename), 0)
#define luthr_lua_l_loadbuffer(state, buffer, size, name) \
  luthr_lua_l_loadbufferx((state), (buffer), (size), (name), 0)
#define luthr_lua_l_checkstring(state, argument) \
  luthr_lua_l_checklstring((state), (argument), 0)
#define luthr_lua_l_optstring(state, argument, default_value) \
  luthr_lua_l_optlstring((state), (argument), (default_value), 0)
#define luthr_lua_l_typename(state, index) \
  luthr_lua_typename((state), luthr_lua_type((state), (index)))
#define luthr_lua_l_dofile(state, filename) \
  (luthr_lua_l_loadfile((state), (filename)) || \
   luthr_lua_pcall((state), 0, LUTHR_LUA_MULTRET, 0))
#define luthr_lua_l_dostring(state, string) \
  (luthr_lua_l_loadstring((state), (string)) || \
   luthr_lua_pcall((state), 0, LUTHR_LUA_MULTRET, 0))
#define luthr_lua_l_getmetatable(state, name) \
  luthr_lua_getfield((state), LUTHR_LUA_REGISTRYINDEX, (name))
#define luthr_lua_l_opt(state, function, argument, default_value) \
  (luthr_lua_isnoneornil((state), (argument)) ? (default_value) : \
   (function)((state), (argument)))
#define luthr_lua_l_newlibtable(state, registration) \
  luthr_lua_createtable((state), 0, \
                       (int)(sizeof(registration) / sizeof((registration)[0]) - 1))
#define luthr_lua_l_newlib(state, registration) \
  (luthr_lua_l_checkversion((state)), luthr_lua_l_newlibtable((state), (registration)), \
   luthr_lua_l_setfuncs((state), (registration), 0))
#define luthr_lua_l_argcheck(state, condition, argument, message) \
  ((void)((condition) || luthr_lua_l_argerror((state), (argument), (message))))
#define luthr_lua_l_argexpected(state, condition, argument, type_name) \
  ((void)((condition) || luthr_lua_l_typeerror((state), (argument), (type_name))))
#define luthr_lua_l_pushfail(state) luthr_lua_pushnil((state))
#define luthr_lua_l_bufflen(buffer_ptr) ((buffer_ptr)->n)
#define luthr_lua_l_buffaddr(buffer_ptr) ((buffer_ptr)->b)
#define luthr_lua_l_addsize(buffer_ptr, size) ((buffer_ptr)->n += (size))
#define luthr_lua_l_buffsub(buffer_ptr, size) ((buffer_ptr)->n -= (size))
#define luthr_lua_l_prepbuffer(buffer_ptr) \
  luthr_lua_l_prepbuffsize((buffer_ptr), LUTHR_LUA_BUFFER_SIZE)
#define luthr_lua_l_openlibs(state) luthr_lua_l_openselectedlibs((state), ~0, 0)
"""


def header(items: Sequence[Tuple[str, str, str]]) -> str:
    prototypes: List[str] = []
    contracts = {
        "lua_newstate": "Creates an owned Lua state with the supplied allocator; close it with luthr_lua_close().",
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
        "luaL_newstate": "Creates an owned Lua state with the standard allocator; close it with luthr_lua_close().",
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
        prototypes.append("LUTHR_LUA_API {} {}({});".format(
            public_result, public_name(name), public_parameters))
    return """/* Generated by tools/generate_lua_c89_facade.py; do not edit. */
#ifndef LUTHR_LUA_H
#define LUTHR_LUA_H

#include <limits.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#if defined(_WIN32) && defined(LUTHR_LUA_BUILDING_SHARED)
#define LUTHR_LUA_API __declspec(dllexport)
#elif defined(_WIN32) && !defined(LUTHR_LUA_STATIC)
#define LUTHR_LUA_API __declspec(dllimport)
#elif defined(__GNUC__) || defined(__clang__)
#define LUTHR_LUA_API __attribute__((visibility("default")))
#else
#define LUTHR_LUA_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define LUTHR_LUA_VERSION_MAJOR_N 5
#define LUTHR_LUA_VERSION_MINOR_N 5
#define LUTHR_LUA_VERSION_RELEASE_N 1
#define LUTHR_LUA_VERSION_NUM 505
#define LUTHR_LUA_VERSION_RELEASE_NUM 50501
#define LUTHR_LUA_ABI_VERSION 0
#define LUTHR_LUA_MULTRET (-1)
#define LUTHR_LUA_REGISTRYINDEX (-(INT_MAX / 2 + 1000))
#define LUTHR_LUA_UPVALUEINDEX(index) (LUTHR_LUA_REGISTRYINDEX - (index))
#define LUTHR_LUA_OK 0
#define LUTHR_LUA_YIELD 1
#define LUTHR_LUA_ERRRUN 2
#define LUTHR_LUA_ERRSYNTAX 3
#define LUTHR_LUA_ERRMEM 4
#define LUTHR_LUA_ERRERR 5
#define LUTHR_LUA_TNONE (-1)
#define LUTHR_LUA_TNIL 0
#define LUTHR_LUA_TBOOLEAN 1
#define LUTHR_LUA_TLIGHTUSERDATA 2
#define LUTHR_LUA_TNUMBER 3
#define LUTHR_LUA_TSTRING 4
#define LUTHR_LUA_TTABLE 5
#define LUTHR_LUA_TFUNCTION 6
#define LUTHR_LUA_TUSERDATA 7
#define LUTHR_LUA_TTHREAD 8
#define LUTHR_LUA_NUMTYPES 9
#define LUTHR_LUA_MINSTACK 20
#define LUTHR_LUA_RIDX_GLOBALS 2
#define LUTHR_LUA_RIDX_MAINTHREAD 3
#define LUTHR_LUA_RIDX_LAST 3
#define LUTHR_LUA_GCSTOP 0
#define LUTHR_LUA_GCRESTART 1
#define LUTHR_LUA_GCCOLLECT 2
#define LUTHR_LUA_GCCOUNT 3
#define LUTHR_LUA_GCCOUNTB 4
#define LUTHR_LUA_GCSTEP 5
#define LUTHR_LUA_GCISRUNNING 6
#define LUTHR_LUA_GCGEN 7
#define LUTHR_LUA_GCINC 8
#define LUTHR_LUA_GCPARAM 9
#define LUTHR_LUA_IDSIZE 60
#define LUTHR_LUA_BUFFER_SIZE (16 * sizeof(void *) * sizeof(double))

/** Opaque Lua VM state; close the owner with luthr_lua_close(). */
typedef struct luthr_lua_state luthr_lua_state;
/** Lua numeric value; this bundle configures it as double. */
typedef double luthr_lua_number;
/** Opaque C continuation context passed unchanged to its callback. */
typedef ptrdiff_t luthr_lua_kcontext;
/** Exact 64-bit Lua integer bits as high and low 32-bit words. */
typedef struct luthr_lua_integer {
  /** Most significant 32 bits, including the signed value's sign bit. */
  unsigned int high;
  /** Least significant 32 bits. */
  unsigned int low;
} luthr_lua_integer;
/** Unsigned 64-bit Lua integer bits in the same two-word layout. */
typedef luthr_lua_integer luthr_lua_unsigned;
/** Lua C function; return the number of results left on the stack. */
typedef int (*luthr_lua_c_function)(luthr_lua_state *state);
/** Continuation callback for a yielded protected call or call. */
typedef int (*luthr_lua_k_function)(luthr_lua_state *state, int status,
                                   luthr_lua_kcontext context);
/** Return a borrowed chunk valid until the next reader call; NULL or zero
 * size signals end of input. */
typedef const char *(*luthr_lua_reader)(luthr_lua_state *state, void *user,
                                       size_t *size);
/** Return zero on successful consumption of a chunk, nonzero on failure. */
typedef int (*luthr_lua_writer)(luthr_lua_state *state, const void *data,
                               size_t size, void *user);
/** Lua allocator: a zero new_size frees pointer; NULL signals failure. */
typedef void *(*luthr_lua_alloc)(void *user, void *pointer, size_t old_size,
                                size_t new_size);
/** Receives warning fragments; to_continue marks an unfinished message. */
typedef void (*luthr_lua_warn_function)(void *user, const char *message,
                                       int to_continue);

/** Hook/debug record; string fields are Lua-owned views. */
typedef struct luthr_lua_debug {
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
  char short_src[LUTHR_LUA_IDSIZE];
  void *i_ci;
} luthr_lua_debug;
/** Lua debug hook; record is borrowed for this callback invocation. */
typedef void (*luthr_lua_hook)(luthr_lua_state *state, luthr_lua_debug *record);

/** Name/function record; registration arrays end with a NULL name. */
typedef struct luthr_lua_reg {
  const char *name;
  luthr_lua_c_function func;
} luthr_lua_reg;
/** Auxiliary string builder; finish with a luthr_lua_l_pushresult* operation. */
typedef struct luthr_lua_buffer {
  char *b;
  size_t size;
  size_t n;
  luthr_lua_state *L;
  union {
    long double alignment;
    void *pointer;
    char bytes[LUTHR_LUA_BUFFER_SIZE];
  } init;
} luthr_lua_buffer;
/** File stream record used by Lua's standard I/O library. */
typedef struct luthr_lua_stream {
  FILE *f;
  luthr_lua_c_function closef;
} luthr_lua_stream;

/** Packs exact 64-bit two's-complement bits from high and low words. */
LUTHR_LUA_API luthr_lua_integer luthr_lua_integer_make(unsigned int high,
                                                     unsigned int low);
/** Returns the high 32 bits of a Lua integer. */
LUTHR_LUA_API unsigned int luthr_lua_integer_high(luthr_lua_integer value);
/** Returns the low 32 bits of a Lua integer. */
LUTHR_LUA_API unsigned int luthr_lua_integer_low(luthr_lua_integer value);
/** Packs the high and low words of an unsigned Lua integer. */
LUTHR_LUA_API luthr_lua_unsigned luthr_lua_unsigned_make(unsigned int high,
                                                       unsigned int low);
/** Returns a static build-identification string; do not free it. */
LUTHR_LUA_API const char *luthr_lua_ident(void);
/** Formats a string and pushes it on the Lua stack. */
LUTHR_LUA_API const char *luthr_lua_pushfstring(luthr_lua_state *state,
                                              const char *format, ...);
/** Controls Lua garbage collection; option-specific arguments follow Lua 5.5. */
LUTHR_LUA_API int luthr_lua_gc(luthr_lua_state *state, int option, ...);
/** Raises a formatted Lua error; does not return normally. */
LUTHR_LUA_API int luthr_lua_l_error(luthr_lua_state *state,
                                  const char *format, ...);

__LUTHR_LUA_PROTOTYPES__

__LUTHR_LUA_CONVENIENCE__

#ifdef __cplusplus
}
#endif

#endif /* LUTHR_LUA_H */
""".replace("__LUTHR_LUA_PROTOTYPES__", "\n".join(prototypes)).replace(
        "__LUTHR_LUA_CONVENIENCE__", convenience_header())


def special_definitions() -> str:
    return """LUTHR_LUA_API luthr_lua_integer
luthr_lua_integer_make(unsigned int high, unsigned int low)
{
  luthr_lua_integer value;

  value.high = high;
  value.low = low;
  return value;
}

LUTHR_LUA_API unsigned int luthr_lua_integer_high(luthr_lua_integer value)
{
  return value.high;
}

LUTHR_LUA_API unsigned int luthr_lua_integer_low(luthr_lua_integer value)
{
  return value.low;
}

LUTHR_LUA_API luthr_lua_unsigned
luthr_lua_unsigned_make(unsigned int high, unsigned int low)
{
  return luthr_lua_integer_make(high, low);
}

LUTHR_LUA_API const char *luthr_lua_ident(void)
{
  return lua_ident;
}

static uint64_t luthr_lua_integer_bits(luthr_lua_integer value);
static lua_Integer luthr_lua_native_integer(luthr_lua_integer value);
typedef struct luthr_lua_format_call {
  const char *format;
  va_list *arguments;
} luthr_lua_format_call;
static const char *luthr_lua_push_format_converted(lua_State *native_state,
                                                  const char *format,
                                                  va_list arguments);
static int luthr_lua_push_format_protected(lua_State *native_state);
static const char *luthr_lua_push_format(luthr_lua_state *state,
                                         const char *format,
                                         va_list arguments);
static const char *luthr_lua_push_memory_message(lua_State *native_state,
                                                 const char *format, ...);

LUTHR_LUA_API const char *
luthr_lua_pushfstring(luthr_lua_state *state, const char *format, ...)
{
  va_list arguments;
  const char *result;

  va_start(arguments, format);
  result = luthr_lua_push_format(state, format, arguments);
  va_end(arguments);
  if (result == NULL) {
    lua_pushstring((lua_State *)state, "not enough memory");
    lua_error((lua_State *)state);
  }
  return result;
}

LUTHR_LUA_API const char *
luthr_lua_pushvfstring(luthr_lua_state *state, const char *format,
                      va_list arguments)
{
  return luthr_lua_push_format(state, format, arguments);
}

static const char *
luthr_lua_push_format(luthr_lua_state *state, const char *format,
                     va_list arguments)
{
  lua_State *native_state;
  luthr_lua_format_call call;
  va_list copied_arguments;
  int status;

  native_state = (lua_State *)state;
  if (strstr(format, "%I") == NULL && strstr(format, "%U") == NULL)
    return lua_pushvfstring(native_state, format, arguments);
  if (!lua_checkstack(native_state, 2)) {
    (void)luthr_lua_push_memory_message(native_state, "%s", "not enough memory");
    return NULL;
  }
  call.format = format;
  __builtin_va_copy(copied_arguments, arguments);
  call.arguments = &copied_arguments;
  lua_pushcfunction(native_state, luthr_lua_push_format_protected);
  lua_pushlightuserdata(native_state, &call);
  status = lua_pcall(native_state, 1, 1, 0);
  va_end(copied_arguments);
  if (status != LUA_OK)
    return NULL;
  return lua_tolstring(native_state, -1, NULL);
}

static const char *luthr_lua_push_memory_message(lua_State *native_state,
                                                 const char *format, ...)
{
  va_list arguments;
  const char *message;

  va_start(arguments, format);
  message = lua_pushvfstring(native_state, format, arguments);
  va_end(arguments);
  return message;
}

static int luthr_lua_push_format_protected(lua_State *native_state)
{
  luthr_lua_format_call *call;

  call = (luthr_lua_format_call *)lua_touserdata(native_state, 1);
  (void)luthr_lua_push_format_converted(native_state, call->format,
                                       *call->arguments);
  return 1;
}

static const char *
luthr_lua_push_format_converted(lua_State *native_state, const char *format,
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
      case 'I': lua_pushfstring(native_state, "%I", luthr_lua_native_integer(va_arg(arguments, luthr_lua_integer))); break;
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

LUTHR_LUA_API int luthr_lua_l_error(luthr_lua_state *state,
                                  const char *format, ...)
{
  va_list arguments;

  luaL_where((lua_State *)state, 1);
  va_start(arguments, format);
  (void)luthr_lua_push_format(state, format, arguments);
  va_end(arguments);
  lua_concat((lua_State *)state, 2);
  return lua_error((lua_State *)state);
}

LUTHR_LUA_API int luthr_lua_gc(luthr_lua_state *state, int option, ...)
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

LUTHR_LUA_API void luthr_lua_l_checkversion(luthr_lua_state *state)
{
  luaL_checkversion_((lua_State *)state, LUA_VERSION_NUM, LUAL_NUMSIZES);
}

LUTHR_LUA_API void
luthr_lua_l_addchar(luthr_lua_buffer *buffer_ptr, char character)
{
  if (buffer_ptr->n >= buffer_ptr->size) {
    (void)luthr_lua_l_prepbuffsize(buffer_ptr, 1);
  }
  buffer_ptr->b[buffer_ptr->n++] = character;
}

LUTHR_LUA_API luthr_lua_integer
luthr_lua_l_integer_add(luthr_lua_integer left, luthr_lua_integer right)
{
  return luthr_lua_integer_from_bits(luthr_lua_integer_bits(left) +
                                    luthr_lua_integer_bits(right));
}

LUTHR_LUA_API luthr_lua_integer
luthr_lua_l_integer_subtract(luthr_lua_integer left, luthr_lua_integer right)
{
  return luthr_lua_integer_from_bits(luthr_lua_integer_bits(left) -
                                    luthr_lua_integer_bits(right));
}

LUTHR_LUA_API luthr_lua_integer
luthr_lua_l_integer_multiply(luthr_lua_integer left, luthr_lua_integer right)
{
  return luthr_lua_integer_from_bits(luthr_lua_integer_bits(left) *
                                    luthr_lua_integer_bits(right));
}

LUTHR_LUA_API luthr_lua_integer
luthr_lua_l_integer_bit_and(luthr_lua_integer left, luthr_lua_integer right)
{
  return luthr_lua_integer_from_bits(luthr_lua_integer_bits(left) &
                                    luthr_lua_integer_bits(right));
}

LUTHR_LUA_API luthr_lua_integer
luthr_lua_l_integer_bit_or(luthr_lua_integer left, luthr_lua_integer right)
{
  return luthr_lua_integer_from_bits(luthr_lua_integer_bits(left) |
                                    luthr_lua_integer_bits(right));
}

LUTHR_LUA_API luthr_lua_integer
luthr_lua_l_integer_bit_xor(luthr_lua_integer left, luthr_lua_integer right)
{
  return luthr_lua_integer_from_bits(luthr_lua_integer_bits(left) ^
                                    luthr_lua_integer_bits(right));
}

LUTHR_LUA_API luthr_lua_integer
luthr_lua_l_integer_shift_left(luthr_lua_integer value, unsigned int count)
{
  if (count >= 64U) {
    return luthr_lua_integer_make(0U, 0U);
  }
  return luthr_lua_integer_from_bits(luthr_lua_integer_bits(value) << count);
}

LUTHR_LUA_API luthr_lua_integer
luthr_lua_l_integer_shift_right(luthr_lua_integer value, unsigned int count)
{
  if (count >= 64U) {
    return luthr_lua_integer_make(0U, 0U);
  }
  return luthr_lua_integer_from_bits(luthr_lua_integer_bits(value) >> count);
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

#include <luthr/lua.h>

typedef char luthr_lua_unsigned_int_is_32_bits[
    (sizeof(unsigned int) * CHAR_BIT == 32) ? 1 : -1];
typedef char luthr_lua_native_integer_is_64_bits[
    (sizeof(lua_Integer) * CHAR_BIT == 64) ? 1 : -1];
typedef char luthr_lua_debug_layout_matches_native[
    (sizeof(luthr_lua_debug) == sizeof(lua_Debug)) ? 1 : -1];
typedef char luthr_lua_buffer_layout_matches_native[
    (sizeof(luthr_lua_buffer) == sizeof(luaL_Buffer)) ? 1 : -1];
typedef char luthr_lua_stream_layout_matches_native[
    (sizeof(luthr_lua_stream) == sizeof(luaL_Stream)) ? 1 : -1];
typedef char luthr_lua_reg_layout_matches_native[
    (sizeof(luthr_lua_reg) == sizeof(luaL_Reg)) ? 1 : -1];

#define LUTHR_LUA_ASSERT_LAYOUT_FIELD(public_type, native_type, field) \\
  typedef char luthr_lua_layout_##public_type##_##field[ \\
      (offsetof(public_type, field) == offsetof(native_type, field)) ? 1 : -1]

LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_debug, lua_Debug, event);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_debug, lua_Debug, name);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_debug, lua_Debug, namewhat);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_debug, lua_Debug, what);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_debug, lua_Debug, source);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_debug, lua_Debug, srclen);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_debug, lua_Debug, currentline);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_debug, lua_Debug, linedefined);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_debug, lua_Debug, lastlinedefined);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_debug, lua_Debug, nups);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_debug, lua_Debug, nparams);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_debug, lua_Debug, isvararg);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_debug, lua_Debug, extraargs);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_debug, lua_Debug, istailcall);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_debug, lua_Debug, ftransfer);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_debug, lua_Debug, ntransfer);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_debug, lua_Debug, short_src);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_debug, lua_Debug, i_ci);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_buffer, luaL_Buffer, b);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_buffer, luaL_Buffer, size);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_buffer, luaL_Buffer, n);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_buffer, luaL_Buffer, L);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_buffer, luaL_Buffer, init);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_stream, luaL_Stream, f);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_stream, luaL_Stream, closef);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_reg, luaL_Reg, name);
LUTHR_LUA_ASSERT_LAYOUT_FIELD(luthr_lua_reg, luaL_Reg, func);

#undef LUTHR_LUA_ASSERT_LAYOUT_FIELD

static uint64_t luthr_lua_integer_bits(luthr_lua_integer value)
{
  return ((uint64_t)value.high << 32) | (uint64_t)value.low;
}

static luthr_lua_integer luthr_lua_integer_from_bits(uint64_t bits)
{
  return luthr_lua_integer_make((unsigned int)(bits >> 32),
                               (unsigned int)bits);
}

static lua_Integer luthr_lua_native_integer(luthr_lua_integer value)
{
  return (lua_Integer)luthr_lua_integer_bits(value);
}

static luthr_lua_integer luthr_lua_integer_from_native(lua_Integer value)
{
  return luthr_lua_integer_from_bits((uint64_t)value);
}

static luthr_lua_unsigned luthr_lua_unsigned_from_native(lua_Unsigned value)
{
  uint64_t bits;

  bits = (uint64_t)value;
  return luthr_lua_unsigned_make((unsigned int)(bits >> 32),
                                (unsigned int)bits);
}

__LUTHR_LUA_SPECIAL_DEFINITIONS__

__LUTHR_LUA_DEFINITIONS__
""".replace("__LUTHR_LUA_SPECIAL_DEFINITIONS__", special_definitions()).replace(
        "__LUTHR_LUA_DEFINITIONS__", definitions(items))


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
        r"^LUTHR_LUA_API\b[^;{}]*?\b(luthr_lua_[A-Za-z0-9_]+)\s*\(",
        strip_comments(facade), flags=re.MULTILINE))
    symbols.update(re.findall(
        r"\b(luthr_lua_runtime_[A-Za-z0-9_]+)\s*\(",
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
