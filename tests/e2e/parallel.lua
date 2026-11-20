assert(_VERSION=='Lua 5.5')
local kind,n,tasks=...
assert(type(kind)=='string' and n>=1 and tasks>=1)
local total=n*(n+1)//2
local shared={}
local workloads={
  integers=function()
    local sum=0
    for i=1,n do sum=sum+i end
    return sum
  end,
  independent_tables=function()
    local rows={}
    for i=1,n do rows[i]={key=i,value=i*2} end
    local sum=0
    for i=1,n do sum=sum+rows[i].key+rows[i].value end
    return sum
  end,
  shared_table=function(id)
    local base=(id-1)*n
    for i=1,n do shared[base+i]=i end
    local sum=0
    for i=1,n do sum=sum+shared[base+i] end
    return sum
  end,
  shared_hash=function(id)
    local base=(id-1)*n
    for i=1,n do shared[-(base+i)]=i end
    local sum=0
    for i=1,n do sum=sum+shared[-(base+i)] end
    return sum
  end,
  shared_growth=function(id)
    local base=tasks+(id-1)*n
    local sum=0
    for i=1,n do
      shared[-id]=i
      shared[-(base+i)]=i
      sum=sum+shared[-id]
    end
    return sum
  end,
  shared_reads=function()
    local sum=0
    for i=1,n do sum=sum+shared.value end
    return sum
  end,
  closures=function()
    local sum=0
    local function add(value) sum=sum+value end
    for i=1,n do add(i) end
    return sum
  end,
  allocation_gc=function()
    local sum=0
    for i=1,n do
      local row={i,i+1,i+2,tag='row'}
      sum=sum+row[2]
      if i%64==0 then collectgarbage('step',0) end
    end
    return sum
  end,
  allocation_only=function()
    local sum=0
    for i=1,n do
      local row={i,i+1,i+2,tag='row'}
      sum=sum+row[2]
    end
    return sum
  end,
}
local work=assert(workloads[kind],'unknown workload')
if kind=='independent_tables' then total=total*3 end
if kind=='shared_reads' then total=n end
if kind=='allocation_gc' or kind=='allocation_only' then total=total+n end
local function prepare()
  if kind=='shared_reads' then
    shared.value=1
  elseif kind=='shared_table' then
    for i=1,n*tasks do shared[i]=0 end
  elseif kind=='shared_hash' then
    for i=1,n*tasks do shared[-i]=0 end
  elseif kind=='shared_growth' then
    shared={}
    for id=1,tasks do shared[-id]=0 end
  end
end
local function verify()
  if kind=='shared_table' then
    for id=1,tasks do
      for i=1,n do assert(shared[(id-1)*n+i]==i) end
    end
  elseif kind=='shared_hash' or kind=='shared_growth' then
    local offset=kind=='shared_growth' and tasks or 0
    for id=1,tasks do
      if kind=='shared_growth' then assert(shared[-id]==n) end
      for i=1,n do assert(shared[-(offset+(id-1)*n+i)]==i) end
    end
  end
end
return function(id)
  assert(id>=1 and id<=tasks)
  local result=work(id)
  assert(result==total)
  return result
end,prepare,verify,total
