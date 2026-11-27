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
  local floor, ceil, sqrt = math.floor, math.ceil, math.sqrt
  assert(floor(math.maxinteger) == math.maxinteger)
  assert(ceil(math.mininteger) == math.mininteger)
  assert(floor(-3.25) == -4 and ceil(-3.25) == -3)
  assert(math.type(floor(2^80)) == 'float' and floor(2^80) == 2^80)
  assert(floor(math.huge) == math.huge and floor('42.5') == 42)
  assert(math.type(sqrt(1)) == 'float' and sqrt('4') == 2.0)
  assert(math.abs(math.mininteger) == math.mininteger)
  assert(1 / math.sin(-0.0) == -math.huge and sqrt(-1) ~= sqrt(-1))
  local whole, fraction = math.modf(-1.25)
  assert(whole == -1 and math.type(whole) == 'integer' and fraction == -0.25)
  whole, fraction = math.modf(math.huge)
  assert(whole == math.huge and math.type(fraction) == 'float' and fraction == 0)
  local fraction, exponent = math.frexp(40)
  assert(fraction == 0.625 and exponent == 6 and math.type(exponent) == 'integer')
  local ok, err = pcall(sqrt, {})
  assert(not ok and err:find('number expected', 1, true))

  local calls = 0
  debug.sethook(function(event)
    if event == 'call' and debug.getinfo(2, 'f').func == floor then
      calls = calls + 1
      assert(debug.setlocal(2, 1, 41.5))
    end
  end, 'c')
  local observed = floor(0)
  debug.sethook()
  assert(observed == 41 and calls == 1)
end

do
  local expected = {
    year = 1970, month = 1, day = 1, hour = 0, min = 0, sec = 0,
    yday = 1, wday = 5, isdst = false,
  }
  local first, second = os.date('!*t', 0), os.date('!*t', 0)
  assert(first ~= second and getmetatable(first) == nil)
  first.year = 2000
  collectgarbage('collect')
  for key, value in pairs(expected) do
    assert(rawget(second, key) == value)
  end
  for key in pairs(second) do assert(expected[key] ~= nil) end
  assert(first.year == 2000 and second.year == 1970)
end

do
  local token = {}
  local callable = setmetatable({}, {__call=function(_, ...) return ... end})
  local many = {[1]=token,[96]=42}
  local results = table.pack(pcall(callable, table.unpack(many, 1, 96)))
  assert(results.n == 97 and results[1] == true and results[2] == token and
         results[3] == nil and results[97] == 42)

  local thread = coroutine.create(function()
    return xpcall(function(...)
      coroutine.yield(...)
      error(token, 0)
    end, function(err) return {err} end, 41, nil, 42)
  end)
  local yielded = table.pack(coroutine.resume(thread))
  assert(yielded.n == 4 and yielded[1] and yielded[2] == 41 and
         yielded[3] == nil and yielded[4] == 42)
  local resumed, ok, err = coroutine.resume(thread)
  assert(resumed and ok == false and err[1] == token)

  ok, err = pcall(pcall)
  assert(not ok and err:find('value expected', 1, true))
  ok, err = pcall(xpcall, callable)
  assert(not ok and err:find('function expected', 1, true))
end

do
  local function fixed(first, last)
    return {first, nil, last, name='list'}
  end
  local left, right = {}, {}
  local list = fixed(left, right)
  assert(list[1]==left and list[2]==nil and list[3]==right and list.name=='list')

  local thread = coroutine.create(function()
    local function tail()
      coroutine.yield('constructing')
      return {41}, nil, {42}
    end
    return {'first', tail()}
  end)
  local ok, phase = coroutine.resume(thread)
  assert(ok and phase=='constructing')
  local partial
  for i=1,8 do
    local _, value = debug.getlocal(thread,2,i)
    if type(value)=='table' then
      partial=value
      break
    end
  end
  assert(partial)
  partial[2] = {'displaced'}
  collectgarbage('collect')
  local completed, result = coroutine.resume(thread)
  assert(completed and rawequal(result,partial))
  collectgarbage('collect')
  assert(result[1]=='first' and result[2][1]==41 and
         result[3]==nil and result[4][1]==42)
end

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

do
  local record = {left=20,right=22}
  local sum = 0
  for key, value in pairs(record) do
    assert(record[key] == value)
    sum = sum + value
    record[key] = nil
  end
  assert(sum == 42 and next(record) == nil)
  local mixed = {11,22,letter=9}
  sum = 0
  for key in next,mixed do sum = sum + mixed[key] end
  assert(sum == 42)
  local ok, err = pcall(function()
    for key in next,mixed,'unknown' do error(key) end
  end)
  assert(not ok and err:find("invalid key to 'next'", 1, true))

  local ordinary = {}
  ordinary.word, ordinary[{}], ordinary[-12] = 10, 11, 21
  ordinary[string.rep('k',96)] = 1
  sum = 0
  for key, value in next,ordinary do
    sum = sum + value
    ordinary[key] = nil
  end
  assert(sum == 43 and next(ordinary) == nil)
  local sparse = {}
  for i=1,400 do sparse[i]=i end
  for i=1,399 do sparse[i]=nil end
  local position, value = next(sparse)
  assert(position == 400 and value == 400 and next(sparse,position) == nil)

  local result = table.pack(next({answer=42}))
  assert(result.n == 2 and result[1] == 'answer' and result[2] == 42)
  local exhausted = table.pack(next({answer=42},'answer','ignored'))
  assert(exhausted.n == 1 and exhausted[1] == nil)
  local k, v, extra = next({answer=42})
  assert(k == 'answer' and v == 42 and extra == nil)
  local function tail_next(t)
    return next(t)
  end
  k, v = tail_next({answer=42})
  assert(k == 'answer' and v == 42)
  local function wrong_state()
    local ignored = next(false)
    return ignored
  end
  ok, err = pcall(wrong_state)
  assert(not ok and err:find("table expected", 1, true))
  local captured = {}
  for _, item, third, fourth in next,{10,20} do
    assert(third == nil and fourth == nil)
    captured[#captured+1] = function() return item end
  end
  assert(captured[1]() == 10 and captured[2]() == 20)
  local raw_results = 0
  for item, extra in rawget,{42},1 do
    assert(item == 42 and extra == nil)
    raw_results = raw_results + 1
  end
  assert(raw_results == 1)

  local iterator = next
  local calls, returns = 0, 0
  debug.sethook(function(event)
    if debug.getinfo(2,'f').func == iterator then
      if event == 'call' then calls = calls + 1 end
      if event == 'return' then returns = returns + 1 end
    end
  end,'cr')
  for key, value in iterator,{answer=42} do
    assert(key == 'answer' and value == 42)
  end
  debug.sethook()
  assert(calls == 2 and returns == 2)

  local weak = setmetatable({stable=42,dead={}}, {__mode='v'})
  collectgarbage('collect')
  local key, value = next(weak)
  assert(key == 'stable' and value == 42 and next(weak,key) == nil)
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
do
  local span = string.rep('a',3073)
  local input = span..'x'..span..'y'..span..'z'..span..'x'
  local result, count = input:gsub('[x-z]',{x=false,z=42},3)
  equal(result,span..'x'..span..'y'..span..'42'..span..'x')
  equal(count,3)
  result, count = input:gsub('[x-z]',function() return false end)
  equal(result,input)
  equal(count,4)
  result, count = span:gsub('[x-z]','%')
  equal(result,span)
  equal(count,0)

  local high = string.char(255)
  result, count = ('a\0'..high..'z'):gsub('[^a-z%z]','<%0>')
  equal(result,'a\0<'..high..'>z')
  equal(count,1)
  result, count = ('a\0'..high..'z'):gsub('[%z\254-\255]','!')
  equal(result,'a!!z')
  equal(count,2)
  result, count = ('a9]z'):gsub('[%d%]]','!')
  equal(result,'a!!z')
  equal(count,2)
  result, count = ('a]z'):gsub('[]z-a]','!')
  equal(result,'a!z')
  equal(count,1)
  result, count = ('axxxb'):gsub('[x]+','!')
  equal(result,'a!b')
  equal(count,1)
  result, count = ('xx'):gsub('^[x]','!')
  equal(result,'!x')
  equal(count,1)
  result, count = ('ab'):gsub('()',function(i) if i == 2 then return '!' end end)
  equal(result,'a!b')
  equal(count,3)
  result, count = span:gsub('[','!',0)
  equal(result,span)
  equal(count,0)
  assert(not pcall(string.gsub,'','[','!'))
end
equal(string.match('(a(b)c)','%b()'),'(a(b)c)')
do
  local left, right = ('aaaa'):match('^(a*)(a+)$')
  equal(left,'aaa')
  equal(right,'a')
  local first, last, before, token, after = ('ab'):find('^()(a+)b()$')
  equal(first,1)
  equal(last,2)
  equal(before,1)
  equal(token,'a')
  equal(after,3)
  equal(('x'):match('^y['),nil)
  assert(not pcall(string.match,'y','^y['))

  local pieces = {}
  for piece in ('\0x||y|\0'):gmatch('[^|]+') do
    pieces[#pieces+1] = piece
  end
  equal(#pieces,3)
  equal(pieces[1],'\0x')
  equal(pieces[2],'y')
  equal(pieces[3],'\0')
  local next_run = ('^^one'):gmatch('^+')
  equal(next_run(),'^^')
  equal(next_run(),nil)
end
do
  for label in ('acdglpsuwxzACDGLPSUWXZ%'):gmatch('.') do
    local atom = '%'..label
    local members = {}
    for i=0,255 do
      local byte = string.char(i)
      if byte:match('^'..atom..'$') then
        members[#members+1] = byte
      end
    end
    if #members > 0 then
      local subject = table.concat(members):rep(128)
      local capture, stop = subject:match('^('..atom..'+)()')
      equal(capture, subject)
      equal(stop, #subject + 1)
    end
  end
  equal(('12345X'):match('^(%d*)45X$'), '123')
  equal(('X'):match('^(%d*)X$'), '')
  local mixed = string.rep('a\0b', 4096)
  equal((mixed..'!'):match('^(.+)!$'), mixed)
  local fields = {}
  for i=1,32 do fields[i] = string.format('%04d', i) end
  local captures = table.pack(table.concat(fields):match(('(....)'):rep(#fields)))
  equal(captures.n, #fields)
  for i=1,#fields do equal(captures[i], fields[i]) end
end
do
  local function readbyte(subject, index)
    local first = subject:byte(index)
    local second = subject:byte(index, index)
    return first, second
  end
  local subject = string.char(255)..'A'
  local decoded = assert(load(string.dump(readbyte)))
  local first, second = decoded(subject,1)
  equal(first,255)
  equal(second,255)
  first, second = decoded(subject,-1)
  equal(first,65)
  equal(second,65)
  first, second = decoded(subject,3)
  equal(first,nil)
  equal(second,nil)

  local byte = string.byte
  string.byte = function(source, index, last)
    equal(source,subject)
    return last and index + last or index
  end
  first, second = decoded(subject,2)
  string.byte = byte
  equal(first,2)
  equal(second,4)

  local calls = 0
  debug.sethook(function(event)
    if event == 'call' and debug.getinfo(2,'f').func == byte then
      calls = calls + 1
    end
  end,'c')
  first, second = decoded(subject,2)
  debug.sethook()
  equal(first,65)
  equal(second,65)
  equal(calls,2)
end
do
  local capture = string.rep('x',8192)
  local subject = '['..capture..']'
  local pattern = '%[(x+)()%]'
  local first, last, text, position = string.find(subject,pattern)
  equal(first,1)
  equal(last,#subject)
  equal(text,capture)
  equal(position,#subject)
  text, position = string.match(subject,pattern)
  equal(text,capture)
  equal(position,#subject)
end
do
  local word, digits = 'ab', '12'
  local input = word..' '..digits
  local a, b, pos = string.match(input,'^(%a+) (%d+)()$')
  assert(a==word and b==digits and pos==6)
  local first, last, text, start = string.find(input,'^(%a+) ()%d+$')
  assert(first==1 and last==5 and text==word and start==4)
  equal(string.match('aaa','^(a+)a$'),'aa')
  equal(string.match(input,'^z$'),nil)
  assert(not pcall(string.match,'a','^(a'))

  local n, invalid = utf8.len('a\255b')
  assert(n==nil and invalid==2)
  equal(utf8.len('å中🙂',3,3),1)
  equal(utf8.len('',1,-1),0)
  equal(utf8.len('å','1'),1)
  assert(not pcall(utf8.len,'abc',5))
  equal(string.len(12),2)
  assert(math.tointeger(42.0)==42 and math.tointeger('42')==42)
  assert(math.tointeger(42.5)==nil and math.tointeger(false)==nil)
end
do
  local format = string.format
  equal(format('%d,%i,%u',0,-42,42),'0,-42,42')
  equal(format('%d/%d',math.mininteger,math.maxinteger),
    '-9223372036854775808/9223372036854775807')
  equal(format('%u',-1),'18446744073709551615')
  equal(format('%+06d/%#x/%.0d',42,42,0),'+00042/0x2a/')
  equal(format('%d/%i','-42',42.0),'-42/42')
  equal(tostring(math.mininteger),'-9223372036854775808')
  equal('value='..math.maxinteger,'value=9223372036854775807')
  local n = assert(load('return '..format('%q',math.mininteger)))()
  assert(math.type(n)=='integer' and n==math.mininteger)
  assert(not pcall(format,'%d',1.25))
  assert(not pcall(format,'%100d',1))
  local object = setmetatable({}, {__tostring=function()
    collectgarbage('collect')
    return 'rooted'
  end})
  equal(format('%d/%s/%q',42,object,'bytes'),'42/rooted/"bytes"')
end
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
