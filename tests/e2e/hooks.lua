local debug=require('debug')
local chunk=assert(load('local b={10}\n_e2e_hook_value=b[1]\nb=4\n','=hook-lines'))
local expected={1,2,3}
local events=0
local function hook(event,line)
  events=events+1
  assert(event=='line' and line==expected[events],'unexpected line event')
end

-- Vary the entry phase across periodic VM polls, without GC perturbing it.
collectgarbage('stop')
for padding=0,2047 do
  for i=1,padding do end
  events=0
  debug.sethook(hook,'l'); chunk(); debug.sethook()
  assert(events==#expected)
end
collectgarbage('restart')

do
  local function target(a,b) return a,b end
  local function operands() return 'original',nil,'extra' end
  local first,second=target('first','second')
  assert(first=='first' and second=='second')

  local calls={}
  debug.sethook(function(event)
    if debug.getinfo(2,'f').func==target then
      calls[#calls+1]=event
      if event=='call' then assert(debug.setlocal(2,1,'rewritten')=='a') end
    end
  end,'cr')
  local a,b,c=target(operands())
  debug.sethook()
  assert(a=='rewritten' and b==nil and c==nil)
  assert(table.concat(calls,',')=='call,return')

  first,second=target('first')
  assert(first=='first' and second==nil)
end

do
  local target=assert(load([[return function(a, ...)
    coroutine.yield(a)
    return a
  end]], '=getinfo-source'))()
  local thread=coroutine.create(target)
  local ok, yielded=coroutine.resume(thread, 42)
  assert(ok and yielded==42)
  local info=debug.getinfo(thread,1,'SftuLLff')
  assert(info.func==target and info.source=='=getinfo-source' and info.what=='Lua')
  assert(info.nparams==1 and info.isvararg and not info.istailcall)
  assert(info.activelines[2] and info.activelines[3])
  local foreign=debug.getinfo(thread,target,'LSftu')
  assert(foreign.func==target and foreign.source==info.source)
  assert(foreign.nparams==info.nparams and foreign.isvararg==info.isvararg)
  assert(foreign.activelines[2] and foreign.activelines[3])
  assert(debug.getinfo(target,'f').func==target)
  assert(debug.getinfo(math.sin,'LS').activelines==nil)
  local current = coroutine.running()
  assert(debug.getinfo(current,0,'f').func==debug.getinfo)
  assert(debug.getinfo(current,0,nil).what=='C')
  assert(debug.getinfo('0','f').func==debug.getinfo)
  assert(debug.getinfo(0.0,'f').func==debug.getinfo)
  assert(next(debug.getinfo(target,''))==nil)
  assert(debug.getinfo(99999,'?')==nil)
  assert(debug.getinfo(-1)==nil)
  local accepted, message=pcall(debug.getinfo,target,'>f')
  assert(not accepted and message:find("invalid option '>'",1,true))
  assert(not pcall(debug.getinfo,thread,1,'f?'))
  local resumed, result=coroutine.resume(thread)
  assert(resumed and result==42)
end

do
  local marker = {}
  local function target() return 42 end
  local function native_error()
    -- Enter a native library on the hook stack before raising an error.
    assert(type(debug.getregistry())=='table')
    error(marker,0)
  end
  debug.sethook(function(event)
    if event=='call' and debug.getinfo(2,'f').func==target then
      native_error()
    end
  end,'c')
  local ok, raised = pcall(target)
  debug.sethook()
  assert(not ok and raised==marker)
  collectgarbage('collect')
  assert(target()==42 and debug.getinfo(target,'f').func==target)
end

do
  local registry, thread = debug.getregistry(), coroutine.running()
  local meta = debug.getmetatable(registry)
  local calls = {0,0,0}
  local function target() return 42 end
  local function observer(index)
    return function(event)
      if event=='call' and debug.getinfo(2,'f').func==target then
        calls[index]=calls[index]+1
      end
    end
  end
  local replacement = {[thread]=observer(2)}
  local inherited = {[thread]=observer(3)}
  debug.sethook(observer(1),'c')
  local hooks = registry._HOOKKEY
  assert(target()==42)
  registry._HOOKKEY=replacement
  assert(target()==42)
  debug.setmetatable(registry,{__index=function(_,key)
    if key=='_HOOKKEY' then return inherited end
  end})
  registry._HOOKKEY=nil
  assert(target()==42)
  registry._HOOKKEY=hooks
  debug.setmetatable(registry,meta)
  debug.sethook()
  assert(calls[1]==1 and calls[2]==1 and calls[3]==1)
end

local gaps={1,124,127,128,255,256,1000}
local active=true
local finalized=0
local mt={}
mt.__gc=function(object)
  finalized=finalized+1
  if active then debug.setmetatable(object,mt) end
end
debug.setmetatable({},mt)
for _,a in ipairs(gaps) do
  for _,b in ipairs(gaps) do
    collectgarbage('collect')
    local source='local b={10}\n_e2e_hook_value=b[1]'..string.rep('\n',a)..
      '+'..string.rep('\n',b)..'b[1]\nb=4\n'
    chunk=assert(load(source,'=hook-gaps'))
    expected={1,2+a,2+a+b,2+a,2+a+b,3+a+b}
    events=0
    debug.sethook(hook,'l'); chunk(); debug.sethook()
    assert(events==#expected)
  end
end
active=false
collectgarbage('collect')
assert(finalized>0)
_e2e_hook_value=nil
print('hooks: vararg entry, periodic poll phases, large line gaps and finalization passed')
