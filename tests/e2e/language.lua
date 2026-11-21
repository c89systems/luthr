local function equal(actual, expected)
  assert(actual == expected)
end
equal(math.maxinteger + 1, math.mininteger)
equal((0xff & 0x39) ~ 0x10, 0x29)
equal(7 // 3, 2)
equal(-7 // 3, -3)
local mt = {__add=function(a,b) return a.value+b.value end,
  __index=function(_,key) return key..'!' end,
  __len=function() return 42 end}
local object = setmetatable({value=20},mt)
equal(object+setmetatable({value=22},mt),42)
equal(object.missing,'missing!')
equal(#object,42)
local packed = table.pack(nil,42,nil)
equal(packed.n,3)
local a,b,c = table.unpack(packed,1,3)
assert(a==nil and b==42 and c==nil)

do
  local token = {}
  local function tailassert(...)
    return assert(...)
  end
  local results = table.pack(tailassert(token,nil,42,nil))
  assert(results.n==4 and results[1]==token and results[2]==nil and
         results[3]==42 and results[4]==nil)
  local x,y,z = assert(true,42)
  assert(x==true and y==42 and z==nil)
  local callable = setmetatable({}, {__call=assert})
  x,y = callable(42)
  assert(x==callable and y==42)
  local failed, err = pcall(function() assert(false,token) end)
  assert(not failed and rawequal(err,token))
  assert(not pcall(function() assert() end))
  assert(select('#suffix',token,nil,42)==3)
  results = table.pack(select(-2,token,nil,42))
  assert(results.n==2 and results[1]==nil and results[2]==42)
  assert(select('2',token,42)==42 and select('#',select(100,token))==0)
  assert(not pcall(function() select(0,token) end))
  local t = setmetatable({[1]=token},mt)
  assert(rawequal(rawget(t,1.0),token) and rawget(t,'missing')==nil)
  assert(rawequal(1,1.0) and not rawequal({},{}))
  x,y = rawequal(t,t)
  assert(x==true and y==nil)
  x,y = string.byte('')
  assert(x==nil and y==nil)
  assert(not pcall(function() rawget(false,1) end))
  assert(not pcall(function() rawequal(token) end))

  local target, events = assert, {}
  debug.sethook(function(event)
    if debug.getinfo(2,'f').func == target then
      events[#events+1] = event
    end
  end,'cr')
  local observed = target(token)
  debug.sethook()
  assert(observed==token and table.concat(events,',')=='call,return')
end

do
  local convert = tonumber
  local a,b = convert(' +0x2a ',nil,'ignored')
  assert(a==42 and b==nil and math.type(a)=='integer')
  assert(convert(1.5)==1.5 and math.type(convert('1.5'))=='float')
  assert(convert('2a',16)==42 and convert(string.rep(' ',128)..'42')==42)
  assert(select('#',convert('42\0suffix'))==1 and convert('42\0suffix')==nil)
  assert(convert('invalid')==nil and convert(false)==nil and not pcall(convert))
  local function tail(value) return convert(value) end
  assert(tail('42')==42)
  local events = {}
  debug.sethook(function(event)
    if debug.getinfo(2,'f').func == convert then
      events[#events+1] = event
      if event == 'call' then assert(debug.setlocal(2,1,'42')) end
    end
  end,'cr')
  local observed = convert('0')
  debug.sethook()
  assert(observed==42 and table.concat(events,',')=='call,return')
end

local closed, marker = {}, {}
local function resource(id)
  return setmetatable({}, {__close=function(_,err)
    closed[#closed+1]=id
    if id == 'outer' then assert(err == marker) end
  end})
end
local ok, failure = pcall(function()
  local outer <close> = resource('outer')
  do local inner <close> = resource('inner') end
  error(marker)
end)
assert(not ok and failure == marker)
equal(table.concat(closed,','),'inner,outer')
local co = coroutine.create(function(initial)
  local held <close> = resource('coroutine')
  local value = coroutine.yield(initial+1,nil,'paused')
  return value*2
end)
local resumed, first, second, third = coroutine.resume(co,20)
assert(resumed and first==21 and second==nil and third=='paused')
resumed, first = coroutine.resume(co,21)
assert(resumed and first==42 and coroutine.status(co)=='dead')
equal(closed[#closed],'coroutine')
local pending = coroutine.create(function()
  local held <close> = resource('cancelled')
  coroutine.yield()
end)
assert(coroutine.resume(pending))
assert(coroutine.close(pending))
equal(closed[#closed],'cancelled')

equal(string.gsub('alpha=12 beta=21','(%a+)=(%d+)',function(k,v)
  return k..'='..tonumber(v)*2
end),'alpha=24 beta=42')
equal(string.match('(a(b)c)','%b()'),'(a(b)c)')
equal(utf8.len('å中🙂'),3)
local bytes = string.pack('<i8I2',-42,65535)
local signed, unsigned, position = string.unpack('<i8I2',bytes)
assert(signed==-42 and unsigned==65535 and position==#bytes+1)
local fragments = {'local n=', '0; for i=1,100 do ', 'n=n+i end; return n'}
local reads=0
local loaded = assert(load(function()
  reads=reads+1
  return fragments[reads]
end,'fragmented-source','t',{}))
equal(loaded(),5050)
equal(assert(load(string.dump(loaded,true),'roundtrip','b',{}))(),5050)
local hooks=0
debug.sethook(function() hooks=hooks+1 end,'',7)
local sum=0
for n=1,100 do sum=sum+n end
debug.sethook()
assert(hooks>0 and sum==5050)
local finalized=0
do
  local cycle=setmetatable({}, {__gc=function() finalized=finalized+1 end})
  cycle.self=cycle
end
collectgarbage('collect')
collectgarbage('collect')
equal(finalized,1)
print('language: integers, metamethods, closures, coroutines, close, strings, load, hooks, GC passed')
