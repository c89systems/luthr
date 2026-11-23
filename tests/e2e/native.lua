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

do
  local closed=0
  local scope=setmetatable({}, {__close=function(_,err)
    assert(err==nil or err==marker)
    closed=closed+1
  end})
  local function tail(fn,growth) return native.frame(scope,fn,growth) end
  local function payload()
    collectgarbage('collect')
    return 42,nil,scope
  end
  local function check(...)
    local values=table.pack(...)
    assert(values.n==3 and values[1]==42 and values[2]==nil and values[3]==scope)
  end
  check(tail(payload,32))
  check(tail(payload,8192))
  local value=native.frame(scope,payload,0)
  assert(value==42 and closed==3)
  local success,err=pcall(tail,function() error(marker) end,0)
  assert(not success and err==marker and closed==4)
  check(tail(payload,0))
  assert(closed==5)
end

do
  local call = native.call
  local function identity(x) return x end
  local function add(x,y) return x+y end
  local function increment(x) return x+1 end
  local function affine(x) return 1000000000-((x+3)*7-1) end
  local function nonlinear(x) return (x+3)*x-1 end
  local function decrease(x) return x-1000 end
  local function multiply(x,y) return x*y end
  local function constant() return 'literal result' end
  local function empty() end
  local token = {}
  local dumped = assert(load(string.dump(affine,true),'=dumped','b'))
  for i=1,1100 do
    assert(call(1,identity,token,i)==token)
    local values = table.pack(call(4,identity,i))
    assert(values.n==4 and values[1]==i and values[2]==nil and
           values[3]==nil and values[4]==nil)
    assert(select('#',call(0,constant))==0)
    assert(select('#',call(-1,empty))==0)
    assert(select('#',call(-1,identity,token))==1)
    assert(call(1,dumped,i)==999999980-7*i)
    assert(call(1,decrease,i)==i-1000)
  end
  assert(call(1,identity)==nil)
  assert(call(1,constant)=='literal result')
  assert(call(1,increment,math.maxinteger)==math.mininteger)
  for _,x in ipairs({math.mininteger,math.maxinteger,-1,0,19,2.0^53}) do
    local y = call(1,affine,x)
    assert(y==affine(x) and math.type(y)==math.type(x))
    assert(call(1,nonlinear,x)==nonlinear(x))
  end
  assert(call(1,add,1.5,2)==3.5)
  assert(call(1,multiply,6,7)==42)
  local value = setmetatable({}, {__add=function(_,b) return b+41 end})
  assert(call(1,increment,value)==42)
  assert(call(1,affine,value)==1000000000-((3+41)*7-1))
  assert(call(1,add,value,1)==42)
  assert(call(1,function(...) return select('#',...) end,1,2,3)==3)

  local calls, returns = 0,0
  debug.sethook(function(event)
    if debug.getinfo(2,'f').func==increment then
      if event=='call' then calls=calls+1
      elseif event=='return' then returns=returns+1 end
    end
  end,'cr')
  assert(call(1,increment,41)==42)
  debug.sethook()
  assert(calls==1 and returns==1)
end

print('native: nested callbacks, references, original errors, bounded 1 MiB stream passed')
