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
  local elapsed_wall=wall()-wall_start
  local elapsed_cpu=math.floor((clock()-cpu_start)*1000000000+0.5)
  if sample>warmups then
    print(string.format('%d,%d,%d,%d',sample-warmups,checksum,elapsed_cpu,elapsed_wall))
  end
end
