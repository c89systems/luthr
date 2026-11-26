local path=e2e_workdir..'/io.bin'
local file=assert(io.open(path,'w+b'))
local data='first\nsecond\0binary\nlast'
assert(file:write(data))
assert(file:seek('set',0)==0)
assert(file:read('l')=='first')
assert(file:read('l')=='second\0binary')
assert(file:read('a')=='last')
assert(file:read('a')=='')
assert(file:seek('set',0)==0)
local lines=0
for line in file:lines() do
  lines=lines+1
  assert(type(line)=='string')
end
assert(lines==3)
assert(file:close())
assert(io.type(file)=='closed file')
assert(not pcall(function() file:read('a') end))
assert(os.remove(path))
local loads=0
package.preload['portable_e2e_module']=function(name,where)
  assert(name=='portable_e2e_module' and where==':preload:')
  loads=loads+1
  return {answer=42}
end
local first,where=require('portable_e2e_module')
assert(first.answer==42 and where==':preload:')
assert(require('portable_e2e_module')==first and loads==1)
package.loaded['portable_e2e_module']=nil
assert(require('portable_e2e_module').answer==42 and loads==2)
assert(select('#',require('portable_e2e_module'))==1)

local registry=debug.getregistry()
local loaded=registry._LOADED
local misses=0
registry._LOADED=setmetatable({},{
  __index=function(_,name)
    assert(name=='portable_e2e_module')
    misses=misses+1
    return 'registry result'
  end
})
assert(require('portable_e2e_module')=='registry result' and misses==1)
registry._LOADED={portable_e2e_module=false}
local reloaded, origin=require('portable_e2e_module')
assert(reloaded.answer==42 and origin==':preload:' and loads==3)
assert(registry._LOADED.portable_e2e_module==reloaded)
registry._LOADED=loaded

local binary='portable_e2e_binary\0suffix'
package.preload.portable_e2e_binary=function(name,data)
  assert(name==binary and data==':preload:')
end
local implicit, origin=require(binary, 'discarded')
assert(implicit==true and origin==':preload:')
assert(package.loaded.portable_e2e_binary==true and package.loaded[binary]==nil)
assert(select('#',require(binary))==1)

local searchers=package.searchers
package.searchers={
  function() return 'first searcher declined' end,
  function(name)
    if name~='portable_e2e_custom' then return 'second searcher declined' end
    return function(loader_name,data)
      assert(loader_name==name and data==searchers)
      return false
    end, searchers
  end
}
local result, data=require('portable_e2e_custom')
assert(result==false and data==searchers)
local missing, message=pcall(require, 'portable_e2e_missing')
assert(not missing and message:find('first searcher declined',1,true)
                   and message:find('second searcher declined',1,true))
package.searchers=searchers

local ok,failure=xpcall(function() error({answer=42}) end,function(value)
  assert(value.answer==42)
  return 'handled'
end)
assert(not ok and failure=='handled')
print('files/modules: binary IO, EOF, close, preload cache, xpcall passed')
