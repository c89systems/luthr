local mode, iterations = ...
local lonejson = require("lonejson")
local message = ("Telemetry from métro station. "):rep(16)
local input = ('{"id":42,"service":"métro","bytes":4032,"active":true,"message":"%s"}'):format(message)
local decode_table = mode == "lonejson_table"
local reuse_record = mode == "lonejson_reuse"
assert(decode_table or reuse_record or mode == "lonejson_fresh", "unknown LoneJSON workload")

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
  for _ = 1, iterations do
    if decode_table then
      result = assert(decode(schema, input))
    else
      result = record or new_record(schema)
      assert(decode_into(schema, result, input))
    end
    sum = sum + result.id + result.bytes
  end
  assert(sum == (42 + 4032) * iterations)
  assert(result.service == "métro" and result.active and result.message == message)
  return #input * iterations
end

local function noop() end
return run, noop, noop, #input * iterations
