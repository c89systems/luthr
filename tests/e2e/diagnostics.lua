local function fails(fragment,operation)
  local ok,message=pcall(operation)
  assert(not ok and type(message)=='string')
  assert(message:find(fragment,1,true),message)
end
fails("nil value (local 'receiver')",function()
  local receiver
  return receiver.field
end)
fails("nil value (local 'receiver')",function()
  local receiver
  receiver.field=1
end)
local receiver
fails("nil value (upvalue 'receiver')",function() return receiver.field end)
fails("nil value (upvalue 'receiver')",function() receiver.field=1 end)
local env={}
fails("nil value (global 'missing')",assert(load('return missing.field','=diagnostic','t',env)))
fails("nil value (global 'missing')",assert(load('missing.field=1','=diagnostic','t',env)))
fails("nil value (field 'absent')",function()
  local object={}
  return object.absent.field
end)
fails("nil value (field 'absent')",function()
  local object={}
  object.absent.field=1
end)
fails('attempt to index a boolean value',function()
  return setmetatable({},{__index=false}).field
end)
fails('attempt to index a boolean value',function()
  setmetatable({},{__newindex=false}).field=1
end)
print('diagnostics: local, upvalue, global, field and metamethod-chain receivers passed')
