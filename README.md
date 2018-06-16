# cfunc

# build

clang cfunc.c -shared -fPIC -o cfunc.so

# use

~~~~
local cf = require'cfunc'
local chead = [[
#include "lua.h"
#include <stdio.h>
]]
local myUpvalue = 'an upvalue!'
local fun = cf.cfunc(function(x,y)
  return chead, [[
    get_x;
    get_myUpvalue;
    printf("%s %s %s\n", lua_tostring(L, -1), lua_tostring(L, -2), lua_tostring(L, I_y));
    lua_pop(L, 2);
    lua_pushstring(L, "another upvalue");
    set_myUpvalue;
    return 0;
  ]], myUpvalue
end)
fun('argument x', 'argument y')
print(myUpvalue)
~~~~
