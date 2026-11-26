assert(_VERSION=='Lua 5.5')
local kind,n,tasks=...
assert(type(kind)=='string' and n>=1 and tasks>=1)
local total=n*(n+1)//2
local shared={}
local initial_timestamp=1700000000000000000
local array_growth=kind=='shared_array_growth'
local sparse_growth=kind=='shared_sparse_growth'
local grows=array_growth or sparse_growth or kind=='shared_growth'
local direction=(array_growth or sparse_growth) and 1 or -1
local growth_offset=sparse_growth and (1<<40) or tasks
local function grow(id)
  local head=direction*id
  local key=direction*(growth_offset+(id-1)*n)
  local sum=0
  for i=1,n do
    key=key+direction
    shared[head]=i
    shared[key]=i
    sum=sum+shared[head]
  end
  return sum
end
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
  mixed_records=function(id)
    local row=shared[id]
    local sum=0
    for i=1,n do
      local units=(i%16)*0.25
      row.balance=row.balance+units
      row.timestamp=initial_timestamp+i*1000
      row.last={sequence=i,units=units}
      sum=sum+(row.timestamp-initial_timestamp)//1000+row.last.sequence
    end
    return sum
  end,
  array_records=function()
    local rows={}
    for i=1,n do rows[i]={i,{i*2}} end
    local sum=0
    for i=1,n do sum=sum+rows[i][1]+rows[i][2][1] end
    return sum
  end,
  retained_chain=function()
    local link
    for i=1,n do link={previous=link,value=i} end
    local sum=0
    for i=n,1,-1 do
      assert(link and link.value==i)
      sum=sum+link.value
      link=link.previous
    end
    assert(link==nil)
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
  shared_growth=grow,
  shared_array_growth=grow,
  shared_sparse_growth=grow,
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
local local_native_add=kind=='native_callbacks_local' or kind=='native_direct_local'
local native_add=kind=='native_callbacks' or kind=='native_direct' or local_native_add
local native_identity=kind=='native_identity'
local native_scalar=kind=='native_scalar' or kind=='native_scalar_direct' or kind=='native_protected'
if kind=='pattern_iterator' then
  local first, second = 'Zoë,Elsa,50', 'Elsa,Chloé,25'
  local input = first..'\n'..second..'\n'
  total = n * (#first + #second)
  workloads[kind] = function()
    local sum = 0
    for _ = 1, n do
      for row in input:gmatch('[^\n]+') do
        sum = sum + #row
      end
    end
    return sum
  end
end
if kind=='native_records' then
  local callback=require('compat_fixture').callback
  local schema={prefix='record=',weight=3}
  local tail=n%16
  total=schema.weight*total+(#schema.prefix+64)*n+120*(n//16)+tail*(tail+1)//2
  local function consume(row)
    return row.sequence*schema.weight+#row.payload
  end
  workloads[kind]=function()
    local recent={}
    local sum=0
    for i=1,n do
      local row={sequence=i,payload=schema.prefix..string.rep('x',64+i%16)}
      recent[(i-1)%64+1]=row
      sum=sum+callback(consume,row)
    end
    for i=math.max(1,n-63),n do
      local row=recent[(i-1)%64+1]
      assert(row.sequence==i and #row.payload==#schema.prefix+64+i%16)
    end
    return sum
  end
end
if kind=='native_streaming' then
  local native=require('compat_fixture')
  local chunk=string.rep('x',128)
  total=n*#chunk
  workloads[kind]=function()
    local sent,received=0,0
    local bytes=native.pump(function()
      assert(sent==received)
      if sent==n then return nil end
      sent=sent+1
      return chunk
    end,function(value)
      assert(value==chunk and sent==received+1)
      received=received+1
    end,#chunk)
    assert(received==n)
    return bytes
  end
end
if kind=='native_leaf' or kind=='native_stack' then
  local callback=require('compat_fixture').callback
  workloads[kind]=function()
    local sum=0
    for i=1,n do sum=sum+callback(i) end
    return sum
  end
end
if kind=='lua_callback' then
  total=total+n
  workloads[kind]=function()
    local function payload(value) return value+1 end
    local sum=0
    for i=1,n do sum=sum+payload(i) end
    return sum
  end
elseif native_add or native_identity or native_scalar then
  local native=require('compat_fixture')
  local payload
  if native_identity then
    payload=function(value) return value end
  elseif native_scalar then
    payload=function(value) return (value+3)*7-value+1 end
    total=6*total+22*n
  else
    payload=function(value) return value+1 end
    total=total+n
  end
  if local_native_add then
    local callback=native.callback
    workloads[kind]=function()
      local sum=0
      for i=1,n do sum=sum+callback(payload,i) end
      return sum
    end
  else
    workloads[kind]=function()
      local sum=0
      for i=1,n do sum=sum+native.callback(payload,i) end
      return sum
    end
  end
end
local work=assert(workloads[kind],'unknown workload')
if kind=='independent_tables' or kind=='array_records' then total=total*3 end
if kind=='mixed_records' then total=total*2 end
if kind=='shared_reads' then total=n end
if kind=='allocation_gc' or kind=='allocation_only' then total=total+n end
local function prepare()
  if kind=='shared_reads' then
    shared.value=1
  elseif kind=='mixed_records' then
    shared={}
    for id=1,tasks do
      shared[id]={balance=0.0,timestamp=0,last=false}
    end
  elseif kind=='shared_table' then
    for i=1,n*tasks do shared[i]=0 end
  elseif kind=='shared_hash' then
    for i=1,n*tasks do shared[-i]=0 end
  elseif grows then
    shared={}
    for id=1,tasks do shared[direction*id]=0 end
  end
end
local function verify()
  if kind=='shared_table' then
    for id=1,tasks do
      for i=1,n do assert(shared[(id-1)*n+i]==i) end
    end
  elseif kind=='mixed_records' then
    local remainder=n%16
    local balance=(n//16)*30+remainder*(remainder+1)/8
    for id=1,tasks do
      local row=shared[id]
      assert(row.balance==balance and row.timestamp==initial_timestamp+n*1000)
      assert(row.last.sequence==n and row.last.units==remainder*0.25)
    end
  elseif kind=='shared_hash' or grows then
    local offset=grows and growth_offset or 0
    for id=1,tasks do
      if grows then assert(shared[direction*id]==n) end
      for i=1,n do assert(shared[direction*(offset+(id-1)*n+i)]==i) end
    end
    if array_growth then assert(rawlen(shared)==tasks*(n+1)) end
  end
end
return function(id)
  assert(id>=1 and id<=tasks)
  local result=work(id)
  assert(result==total)
  return result
end,prepare,verify,total
