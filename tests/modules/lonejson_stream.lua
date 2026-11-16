local lj = require('lonejson').new({max_alloc_bytes=65536})
local Row = lj.schema('Row', {
  lj.field('id', lj.i64 {required=true}),
  lj.field('value', lj.string {required=true, fixed_capacity=256}),
})
local record = Row:new_record()
local cursor = Row:array_stream_fd('', json_input_fd)
assert(cursor)
q:send('[')
local count=0
while true do
  local value, err, status = cursor:next(record)
  if status == 'eof' then break end
  assert(status == 'item' and value == record, err and err.message)
  count=count+1
  assert(record.id == count and #record.value == json_payload_size)
  if count>1 then q:send(',') end
  lj.encode_json_to_sink({id=record.id*2, value=record.value}, function(chunk)
    json_stream_callback()
    q:send(chunk)
  end)
end
cursor:close()
q:send(']')
q:close()
return count
