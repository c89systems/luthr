local native = require('compat_fixture')
local before=native.monotonic()
assert(math.type(before)=='integer' and native.monotonic()>=before)
assert(native.callback(function(value) return value+1 end,41)==42)
assert(native.callback(function(value)
  return native.callback(function(inner) return inner*2 end,value)
end,21)==42)
local marker={}
local ok, failure=pcall(native.callback,function() error(marker) end,0)
assert(not ok and failure==marker)
assert(not pcall(native.callback,17,0))
for _,value in ipairs({true,42,'binary\0value',{},function() return 42 end}) do
  assert(rawequal(native.reference(value),value))
end
assert(native.reference(nil)==nil)
local reads,writes,bytes=0,0,0
local payload=string.rep('x',127)..'\0'
local total=native.pump(function(capacity)
  assert(capacity==128 and writes==reads)
  if reads==8192 then return nil end
  reads=reads+1
  return payload
end,function(chunk)
  assert(reads==writes+1 and chunk==payload)
  writes=writes+1
  bytes=bytes+#chunk
  if writes%256==0 then collectgarbage('collect') end
end,128)
assert(total==1048576 and total==bytes and writes==8192)
assert(native.pump(function() return nil end,function() error('unexpected output') end,1)==0)
assert(native.pump(function() return '' end,function() error('unexpected output') end,1)==0)
assert(not pcall(native.pump,function() return 'too long' end,function() end,1))
assert(not pcall(native.pump,function() return {} end,function() end,1))
assert(not pcall(native.pump,function() end,function() end,0))
for _,site in ipairs({'reader','writer'}) do
  ok,failure=pcall(native.pump,function()
    if site=='reader' then error(marker) end
    return 'x'
  end,function() error(marker) end,1)
  assert(not ok and failure==marker)
end
assert(native.callback(function() return 42 end)==42)
print('native: nested callbacks, references, original errors, bounded 1 MiB stream passed')
