local fallback={answer=42}
local writes={}
local target=setmetatable({present=7,flag=false},{
  __index=fallback,
  __newindex=function(self,key,value)
    writes[#writes+1]=key
    rawset(self,key,value)
  end,
})
assert(target.present==7 and target.flag==false and target.answer==42)
target.present=8
target.flag=true
assert(#writes==0 and target.present==8 and target.flag==true)
target.new=17
assert(#writes==1 and writes[1]=='new' and target.new==17)
target.present=nil
assert(#writes==1 and target.present==nil)
target.present=21
assert(#writes==2 and target.present==21)
local proxy={}
setmetatable(target,{__index=fallback,__newindex=proxy})
target.routed=42
assert(rawget(target,'routed')==nil and proxy.routed==42)
assert(target.unknown==nil)

local calls=0
local replacement={__index=function() return 'new' end}
local original={__index=function(self,key)
  assert(key=='missing')
  calls=calls+1
  setmetatable(self,replacement)
  collectgarbage('collect')
  return 'old'
end}
local changed=setmetatable({},original)
original=nil
assert(changed.missing=='old' and calls==1)
assert(changed.missing=='new' and calls==1)

-- Self-metatables and both directions of a mutually referenced pair.
local self={}
rawset(self,'__index',function(_,key)
  collectgarbage('collect')
  return key..'!'
end)
rawset(self,'__len',function() return 42 end)
setmetatable(self,self)
assert(self.absent=='absent!' and #self==42)
local a,b={id=17},{id=42}
local function index(object,key)
  collectgarbage('collect')
  return rawget(object,'id')+key
end
local function assign(object,key,value)
  rawset(object,key,value)
end
rawset(a,'__index',index)
rawset(b,'__index',index)
rawset(a,'__newindex',assign)
rawset(b,'__newindex',assign)
setmetatable(a,b)
setmetatable(b,a)
for n=1,20 do
  assert(a[n]==17+n and b[n]==42+n)
  a['value'..n]=n
  b['value'..n]=n*2
  assert(a['value'..n]==n and b['value'..n]==n*2)
end
local cyclic={}
setmetatable(cyclic,{__index=cyclic,__newindex=cyclic})
assert(not pcall(function() return cyclic.missing end))
assert(not pcall(function() cyclic.missing=42 end))
local protected=setmetatable({}, {__metatable='protected'})
assert(getmetatable(protected)=='protected')
assert(not pcall(setmetatable,protected,{}))
assert(not pcall(rawset,{},nil,42))
assert(not pcall(rawset,{},0/0,42))

-- Mutating a metatable must invalidate cached absent methods.
local mt={}
local cached=setmetatable({},mt)
assert(cached.missing==nil)
cached.first=1
mt.__index=function(_,key) return 'read:'..key end
mt.__newindex=function(object,key,value) rawset(object,key,value*2) end
assert(cached.missing=='read:missing')
cached.second=2
assert(cached.first==1 and cached.second==4)
mt.__index=nil
mt.__newindex=nil
assert(cached.missing==nil)
cached.third=3
assert(cached.third==3)

-- Misses insert keys through the allocating path; hits copy whole values.
local keys={false,true,0,1,1.5,-7,math.maxinteger,
  'short',string.rep('long key',20),{},function() end,coroutine.create(function() end)}
local mixed=setmetatable({},{})
collectgarbage('collect')
for i,key in ipairs(keys) do
  local value={id=i,payload=string.rep(tostring(i),100)}
  mixed[key]=value
  if i%2==0 then collectgarbage('collect') end
end
collectgarbage('collect')
for i,key in ipairs(keys) do
  assert(mixed[key].id==i and mixed[key].payload==string.rep(tostring(i),100))
  mixed[key]=false
  assert(mixed[key]==false)
  mixed[key]=nil
  assert(mixed[key]==nil)
end
mixed[1.0]={answer=42}
assert(mixed[1].answer==42)
assert(not pcall(function() mixed[nil]=42 end))
assert(not pcall(function() mixed[0/0]=42 end))
-- Invalid raw keys can still be consumed by a __newindex function.
local intercepted={}
local invalid=setmetatable({}, {__newindex=function(_,key,value)
  collectgarbage('collect')
  intercepted[#intercepted+1]={key=key,value=value}
end})
invalid[nil]=17
invalid[0/0]=23
assert(#intercepted==2 and intercepted[1].key==nil and intercepted[1].value==17)
assert(intercepted[2].key~=intercepted[2].key and intercepted[2].value==23)

-- Each indexing form must resume with the correct register and operands.
local co=coroutine.create(function()
  local key={}
  local object=setmetatable({}, {
    __index=function(self,k)
      coroutine.yield('read',k)
      if k=='method' then return function(receiver) return receiver==self end end
      return k
    end,
    __newindex=function(self,k,v)
      coroutine.yield('write',k)
      rawset(self,k,v)
    end,
  })
  local function readcaptured() return object.field end
  local field=object.field
  local index=object[2]
  local value=object[key]
  assert(readcaptured()==field and field=='field' and index==2 and value==key)
  assert(object:method())
  object.field=field
  object[2]=index
  object[key]=value
  assert(object.field==field and object[2]==index and object[key]==value)
  return true
end)
local function yielded(operation,expected)
  local ok,action,key=coroutine.resume(co)
  assert(ok and action==operation and (expected==nil or key==expected))
  collectgarbage('collect')
  return key
end
yielded('read','field')
yielded('read',2)
local key=yielded('read')
assert(type(key)=='table')
yielded('read','field')
yielded('read','method')
yielded('write','field')
yielded('write',2)
yielded('write',key)
local ok,result=coroutine.resume(co)
assert(ok and result==true and coroutine.status(co)=='dead')

-- A primitive lookup tracks both edits to its method table and replacement
-- of that table. Turning the metatable weak also releases its index table.
local original_string_mt=debug.getmetatable('')
local function string_label() return ('subject').label end
local methods={label=41}
local string_mt={__index=methods}
debug.setmetatable('',string_mt)
assert(string_label()==41)
methods.label=42
assert(string_label()==42)
rawset(string_mt,'__index',{label=43})
assert(string_label()==43)
string_mt.__index=function(_,key) return key..'-callback' end
assert(string_label()=='label-callback')

local function weak_string_index()
  local owner={__index={label=44}}
  debug.setmetatable('',owner)
  assert(string_label()==44)
  return setmetatable(owner,{__mode='v'})
end
local weak_string_mt=weak_string_index()
collectgarbage('collect')
collectgarbage('collect')
assert(rawget(weak_string_mt,'__index')==nil)
assert(not pcall(string_label))
debug.setmetatable('',original_string_mt)
assert(('abc'):sub(2)=='bc')
print('metatables: hits, misses, writes, replacement, cache, GC, mixed keys, self/pair ownership, errors passed')
