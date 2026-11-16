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
