assert(_VERSION=='Lua 5.5')
local directory,kind,collector=assert(arg[1]),assert(arg[2]),assert(arg[3])
local samples,warmups,divisor=assert(tonumber(arg[4])),assert(tonumber(arg[5])),assert(tonumber(arg[6]))
local repetitions=assert(tonumber(arg[7]))
assert(samples>=1 and warmups>=0 and divisor>=1 and repetitions>=1)
package.cpath=directory..'/?.so'
local native=require('compat_fixture')
local clock,wall=os.clock,native.monotonic
collectgarbage(collector)
local function count(n) return math.max(1,n//divisor) end
local workloads={
  integers=function()
    local n=count(1000000)
    local sum=0
    for i=1,n do sum=sum+i end
    assert(sum==n*(n+1)//2)
    return sum
  end,
  tables=function()
    local n=count(50000)
    local values={}
    for i=1,n do values[i]={key=i,value=i*2} end
    local sum=0
    for i=1,n do sum=sum+values[i].key+values[i].value end
    assert(sum==3*n*(n+1)//2)
    return sum
  end,
  closures=function()
    local n=count(100000)
    local sum=0
    local function add(value) sum=sum+value end
    for i=1,n do add(i) end
    assert(sum==n*(n+1)//2)
    return sum
  end,
  strings=function()
    local n=count(10000)
    local sum=0
    for i=1,n do
      local text=string.format('row=%d value=%d',i,i*2)
      local a,b=text:match('row=(%d+) value=(%d+)')
      sum=sum+tonumber(a)+tonumber(b)
    end
    assert(sum==3*n*(n+1)//2)
    return sum
  end,
  floating_point=function()
    local n=count(200000)
    local sum=0.0
    for i=1,n do
      local value=i*0.25
      sum=sum+value*value
    end
    local scaled=math.tointeger(sum*16)
    assert(scaled==n*(n+1)*(2*n+1)//6)
    return scaled
  end,
  metamethods=function()
    local n=count(40000)
    local methods={__index=function(self,key)
      assert(key=='total')
      return self.value*2
    end}
    local value=setmetatable({value=0},methods)
    local sum=0
    for i=1,n do
      value.value=i
      sum=sum+value.total
    end
    assert(sum==n*(n+1))
    return sum
  end,
  coroutines=function()
    local n=count(40000)
    local next_value=coroutine.wrap(function()
      for i=1,n do coroutine.yield(i) end
    end)
    local sum=0
    for i in next_value do sum=sum+i end
    assert(sum==n*(n+1)//2)
    return sum
  end,
  protected_calls=function()
    local n=count(20000)
    local function read(value)
      if value%8==0 then error(value,0) end
      return value
    end
    local sum,errors=0,0
    for i=1,n do
      local ok,value=pcall(read,i)
      sum=sum+value
      if not ok then errors=errors+1 end
    end
    assert(sum==n*(n+1)//2 and errors==n//8)
    return sum+errors
  end,
  load_chunks=function()
    local n=count(3000)
    local source='local value,scale=...; return value*scale+1'
    local bytecode=string.dump(assert(load(source,'=benchmark','t')),true)
    local sum=0
    for i=1,n do
      sum=sum+assert(load(source,'=benchmark','t'))(i,2)
      sum=sum+assert(load(bytecode,'=benchmark','b'))(i,2)
    end
    assert(sum==2*n*(n+2))
    return sum
  end,
  utf8_binary=function()
    local n=count(20000)
    local text='métro'
    local sum=0
    for i=1,n do
      local blob=string.pack('<I4s2',i,text)
      local id,name,next_byte=string.unpack('<I4s2',blob)
      assert(next_byte==#blob+1)
      sum=sum+id+utf8.len(name)
    end
    assert(sum==n*(n+1)//2+5*n)
    return sum
  end,
  buffered_io=function()
    local n=count(10000)
    local file <close> = assert(io.tmpfile())
    for i=1,n do assert(file:write(i,'\n')) end
    assert(file:seek('set'))
    local sum=0
    for line in file:lines() do sum=sum+tonumber(line) end
    assert(sum==n*(n+1)//2)
    return sum
  end,
  math_library=function()
    local n=count(60000)
    local floor,sqrt=math.floor,math.sqrt
    local sum=0
    for i=1,n do sum=sum+floor(sqrt(i*i)+0.5) end
    assert(sum==n*(n+1)//2)
    return sum
  end,
  module_loading=function()
    local n=count(5000)
    local name='luthr_benchmark_module'
    local loads=0
    package.preload[name]=function()
      loads=loads+1
      return {sequence=loads}
    end
    local sum=0
    for i=1,n do
      local module=require(name)
      assert(rawequal(module,require(name)))
      sum=sum+module.sequence
      package.loaded[name]=nil
    end
    package.preload[name]=nil
    assert(loads==n and sum==n*(n+1)//2)
    return sum
  end,
  calendar=function()
    local n=count(5000)
    local epoch=1700000000
    local sum=0
    for i=1,n do
      local time=epoch+i*60
      sum=sum+os.date('!*t',time).sec+math.tointeger(os.difftime(time,epoch))
    end
    assert(sum==20*n+30*n*(n+1))
    return sum
  end,
  debug_hooks=function()
    local n=count(16000)
    local function target(value) return value end
    local calls=0
    debug.sethook(function()
      if debug.getinfo(2,'f').func==target then calls=calls+1 end
    end,'c')
    local sum=0
    for i=1,n do sum=sum+target(i) end
    debug.sethook()
    assert(calls==n and sum==n*(n+1)//2)
    return sum
  end,
  allocation_gc=function()
    local n=count(50000)
    local sum=0
    for i=1,n do
      local value={i,i+1,i+2,tag='row'}
      sum=sum+value[2]
    end
    collectgarbage('collect')
    assert(sum==n*(n+1)//2+n)
    return sum
  end,
  native_callbacks=function()
    local n=count(10000)
    local sum=0
    for i=1,n do sum=sum+native.callback(function(value) return value+1 end,i) end
    assert(sum==n*(n+1)//2+n)
    return sum
  end,
  streaming=function()
    local n=count(8192)
    local sent,received=0,0
    local chunk=string.rep('x',128)
    local total=native.pump(function()
      assert(sent==received)
      if sent==n then return nil end
      sent=sent+1
      return chunk
    end,function(value)
      assert(value==chunk and sent==received+1)
      received=received+1
    end,128)
    assert(total==n*128 and received==n)
    return total
  end,
}
local work=assert(workloads[kind],'unknown workload')
for sample=1,warmups+samples do
  collectgarbage('collect')
  local cpu_start,wall_start=clock(),wall()
  local checksum=0
  for iteration=1,repetitions do checksum=checksum+work() end
  collectgarbage('collect')  -- charge final collector work to this sample
  local elapsed_wall=wall()-wall_start
  local elapsed_cpu=math.floor((clock()-cpu_start)*1000000000+0.5)
  if sample>warmups then
    print(string.format('%d,%d,%d,%d',sample-warmups,checksum,elapsed_cpu,elapsed_wall))
  end
end
