/*
MIT License

Copyright (c) 2018 Scott Petersen

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

// write c in your lua source

#define LUA_LIB
#include "lua.h"
#include "lauxlib.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <dlfcn.h>

static struct MemoEntry {
  char *chead;
  size_t cheadlen;
  char *cimpl;
  size_t cimpllen;
  int nlocals;
  char **locals;
  int nups;
  char **ups;
  lua_CFunction fun;
  struct MemoEntry *next;
} *gMemoHead;

// check for an existing c function
static struct MemoEntry *memoFind(lua_State *L, int idx, const char *chead, size_t cheadlen, const char *cimpl, size_t cimpllen) {
  lua_Debug ar;

  lua_pushvalue(L, idx);
  lua_getinfo(L, ">Su", &ar);

  struct MemoEntry **prevNext = &gMemoHead;
  struct MemoEntry *cur = gMemoHead;

  // we'll need this top of stack to check locals
  lua_pushvalue(L, idx);

  // go through list to find a match
  for(; cur; prevNext = &cur->next, cur = cur->next) {
    // check simple stuff up front
    if(cheadlen != cur->cheadlen || cimpllen != cur->cimpllen || ar.nups != cur->nups)
      continue;

    int nlocals = cur->nlocals;
    char **locals = cur->locals;
    int nups = cur->nups;
    char **ups = cur->ups;

    int n;

    // check locals are same name, same order
    for(n = 0; n < nlocals; n++) {
      const char *local = lua_getlocal(L, NULL, n+1);

      if(!local || strcmp(local, locals[n]))
        break;
    }
    if(n < nlocals || lua_getlocal(L, NULL, nlocals+1))
      continue;

    // check upvalues are same name, same order
    for(n = 0; n < nups; n++) {
      if(strcmp(lua_getupvalue(L, -1, n+1), ups[n]))
        break;
    }
    if(n < nups)
      continue;

    // check include string (if there is one)
    if(memcmp(chead, cur->chead, cheadlen))
      continue;
    // finally, check implementation string
    if(memcmp(cur->cimpl, cimpl, cimpllen))
      continue;

    // if we got here, we found it!
    break;
  }
  // no longer need arg on stack
  lua_pop(L, 1);
  if(cur) {
    // move to front
    *prevNext = cur->next;
    cur->next = gMemoHead;
    gMemoHead = cur;
  }
  return cur;
}

static struct MemoEntry *memoAdd(lua_State *L, int idx, const char *chead, size_t cheadlen, const char *cimpl, size_t cimpllen) {
  struct MemoEntry *newEntry = malloc(sizeof(struct MemoEntry));

  newEntry->chead = malloc(cheadlen);
  memcpy(newEntry->chead, chead, cheadlen);
  newEntry->cheadlen = cheadlen;

  newEntry->cimpl = malloc(cimpllen);
  memcpy(newEntry->cimpl, cimpl, cimpllen);
  newEntry->cimpllen = cimpllen;

  lua_Debug ar;

  lua_pushvalue(L, idx);
  lua_getinfo(L, ">Su", &ar);

  int n;

  int nlocals = 0;

  lua_pushvalue(L, idx);
  while(lua_getlocal(L, NULL, nlocals+1))
    nlocals++;

  char **locals = malloc(sizeof(char *) * nlocals);

  for(n = 0; n < nlocals; n++)
    locals[n] = strdup(lua_getlocal(L, NULL, n+1));

  lua_pop(L, 1);
  newEntry->locals = locals;
  newEntry->nlocals = nlocals;

  char **ups = malloc(sizeof(char *) * ar.nups);

  for(n = 0; n < ar.nups; n++)
    ups[n] = strdup(lua_getupvalue(L, idx, n+1));

  newEntry->nups = ar.nups;
  newEntry->ups = ups;

  newEntry->fun = NULL;

  newEntry->next = gMemoHead;
  gMemoHead = newEntry;

  return newEntry;
}

// TODO driverize
// TODO unsafe
// TODO lots of error checking
// compile the function, load it
static void compileFun(struct MemoEntry *entry) {
  char const *tmp = getenv("TMPDIR");

  if(!tmp)
    tmp = "/tmp";

  char *soPath;
  char *cmd;

  asprintf(&soPath, "%s/lua.cfunc.%d.so", tmp, (int)getpid());
  asprintf(&cmd, "clang -shared -undefined dynamic_lookup -x c -O2 - -o '%s'", soPath);

  FILE *p = popen(cmd, "w");

  free(cmd);

  if(p) {
    // "line" 100000+ represents include source
    fputs("#line 100000\n", p);
    fwrite(entry->chead, entry->cheadlen, 1, p);

    int n;

    int nlocals = entry->nlocals;
    char **locals = entry->locals;
    int nups = entry->nups;
    char **ups = entry->ups;

    // "line" 200000+ represents macros and function def
    fputs("\n#line 200000\n", p);
    for(n = 0; n < nlocals; n++) {
      fprintf(p, "#define I_%s %d\n", locals[n], n+1);
      fprintf(p, "#define get_%s do { lua_pushvalue(L, I_%s); } while(0)\n", locals[n], locals[n]);
    }
    for(n = 0; n < nups; n++) {
      fprintf(p, "#define get_%s do { lua_getupvalue(L, lua_upvalueindex(1), %d); } while(0)\n", ups[n], n+1);
      fprintf(p, "#define set_%s do { lua_setupvalue(L, lua_upvalueindex(1), %d); } while(0)\n", ups[n], n+1);
    }
    // "line" 1+ represents implementation
    fputs("LUALIB_API int cfunc(lua_State *L) {\n#line 1\n", p);
    fwrite(entry->cimpl, entry->cimpllen, 1, p);
    fputs("}", p);
    pclose(p);

    void *dl = dlopen(soPath, RTLD_LOCAL);

    unlink(soPath);
    free(soPath);

    entry->fun = dlsym(dl, "cfunc");
  }
  else
    free(soPath);
}

static int cfunc_cfunc (lua_State *L) {
  lua_pushvalue(L, 1);
  lua_call(L, 0, 2);

  luaL_checkstring(L, -2);
  luaL_checkstring(L, -1);

  size_t cheadlen;
  const char *chead = lua_tolstring(L, -2, &cheadlen);
  size_t cimpllen;
  const char *cimpl = lua_tolstring(L, -1, &cimpllen);

  lua_pop(L, 2);

  struct MemoEntry *entry = memoFind(L, 1, chead, cheadlen, cimpl, cimpllen);

  if(!entry) {
    entry = memoAdd(L, 1, chead, cheadlen, cimpl, cimpllen);

    compileFun(entry);
  }

  if(entry->nups) {
    lua_pushvalue(L, 1);
    lua_pushcclosure(L, entry->fun, 1);
  }
  else
    lua_pushcfunction(L, entry->fun);
  return 1;
}

static luaL_Reg cfunclib[] = {
  {"cfunc", cfunc_cfunc },
  {NULL, NULL}
};

LUALIB_API int luaopen_cfunc (lua_State *L) {
  luaL_newlib(L, cfunclib);

  return 1;
}
