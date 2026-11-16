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
local ok,failure=xpcall(function() error({answer=42}) end,function(value)
  assert(value.answer==42)
  return 'handled'
end)
assert(not ok and failure=='handled')
print('files/modules: binary IO, EOF, close, preload cache, xpcall passed')
