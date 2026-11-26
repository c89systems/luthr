local mode, iterations = ...
local lonejson = require("lonejson")
local message = ("Telemetry from métro station. "):rep(16)
local input = ('{"id":42,"service":"métro","bytes":4032,"active":true,"message":"%s"}'):format(message)
local decode_table = mode == "lonejson_table"
local decode_only = mode == "lonejson_decode"
local read_fields = mode == "lonejson_fields"
local create_records = mode == "lonejson_create"
local reuse_record = mode == "lonejson_reuse" or decode_only or read_fields
assert(decode_table or reuse_record or create_records or mode == "lonejson_fresh",
       "unknown LoneJSON workload")
local work = iterations * (create_records and 1 or read_fields and 2 or #input)

local function run()
  local json = lonejson.new()
  local schema = json.schema("Telemetry", {
    json.field("id", json.i64 { required = true }),
    json.field("service", json.string { fixed_capacity = 32, required = true }),
    json.field("bytes", json.i64 { required = true }),
    json.field("active", json.boolean { required = true }),
    json.field("message", json.string { fixed_capacity = 1024, required = true }),
  })
  local record = reuse_record and schema:new_record()
  local decode_into, decode, new_record = schema.decode_into, schema.decode, schema.new_record
  local result, sum = nil, 0
  if create_records then
    for _ = 1, iterations do result = new_record(schema) end
    assert(type(result) == "userdata" and type(result.clear) == "function")
    return work
  elseif decode_only then
    result = record
    for _ = 1, iterations do
      assert(decode_into(schema, result, input))
    end
    sum = (result.id + result.bytes) * iterations
  elseif read_fields then
    result = record
    assert(decode_into(schema, result, input))
    for _ = 1, iterations do
      sum = sum + result.id + result.bytes
    end
  else
    for _ = 1, iterations do
      if decode_table then
        result = assert(decode(schema, input))
      else
        result = record or new_record(schema)
        assert(decode_into(schema, result, input))
      end
      sum = sum + result.id + result.bytes
    end
  end
  assert(sum == (42 + 4032) * iterations)
  assert(result.service == "métro" and result.active and result.message == message)
  return work
end

local function noop() end
return run, noop, noop, work
