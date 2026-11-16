local lj = require('lonejson').new({max_alloc_bytes=65536})
local checkpoint = json_checkpoint or function() end
local id = json_task_id and json_task_id() or 2
local mode = json_fault and json_fault() or 'normal'
local Row = lj.schema('Row', {
  lj.field('id', lj.i64 {required=true}),
  lj.field('name', lj.string {required=true, fixed_capacity=64}),
})
local record = Row:new_record()
Row:decode_into(record, '{"id":17,"name":"first"}')
assert(record.id == 17 and record.name == 'first')
Row:decode_into(record, '{"id":18,"name":"second"}')
assert(record.id == 18 and record.name == 'second')
local cursor = Row:array_stream_string('items',
  '{"items":[{"id":1,"name":"one"},{"id":2,"name":"two"}]}')
for n=1,2 do
  local value, err, status = cursor:next(record)
  assert(status == 'item' and value == record and err == nil)
  assert(record.id == n)
end
local value, err, status = cursor:next(record)
assert(value == nil and err == nil and status == 'eof')
cursor:close()
local chunks = {}
local ok, message = pcall(function()
  lj.encode_json_to_sink({id=id, name='callback'}, function(chunk)
    checkpoint(id)
    if mode == 'error' then error('LoneJSON sink expected') end
    if mode == 'oom' then json_oom() end
    chunks[#chunks+1] = chunk
  end)
end)
if mode == 'error' then
  assert(not ok and tostring(message):find('LoneJSON sink expected',1,true))
elseif mode == 'oom' then
  assert(not ok and tostring(message):find('not enough memory',1,true))
else
  assert(ok, message)
  local decoded = lj.decode_json(table.concat(chunks))
  assert(decoded.id == id and decoded.name == 'callback')
end
assert(lj.decode_json('{"usable":true}').usable)
local malformed, failure = Row:decode('{"id":')
-- The released binding returns the original input with its parse-error table.
assert(malformed == '{"id":' and type(failure.message) == 'string')
Row:decode_into(record, '{"id":42,"name":"recovered"}')
assert(record.id == 42 and record.name == 'recovered')
local sum=0
for n=1,4000 do sum=sum+n end
assert(sum==8002000)
return 42
