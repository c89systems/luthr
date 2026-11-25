local _, iterations = ...
local json = require("json")
local decode = json.decode.getDecoder(true)
local encode = json.encode.getEncoder()
local fixture = assert(io.open("tests/data.txt", "rb"))
local metadata = assert(fixture:read("a"))
assert(fixture:close())

local function roundtrip(sequence)
  local input = ('{"sequence":%d,"metadata":%s}'):format(sequence, metadata)
  local record = decode(input)
  local restored = decode(encode(record))
  assert(record.sequence == sequence and restored.sequence == sequence)
  assert(record.metadata.a1 == json.util.null)
  assert(record.metadata.a2 == 2)
  assert(json.util.IsArray(record.metadata.a3))
  assert(record.metadata.a500 == true)

  local count, sum = 0, 0
  for key, value in pairs(record.metadata) do
    local other = restored.metadata[key]
    if type(value) == "table" then
      assert(json.util.IsArray(value) and json.util.IsArray(other))
      assert(next(value) == nil and next(other) == nil)
    else
      assert(other == value, key)
    end
    if type(value) == "number" then
      sum = sum + value
    end
    count = count + 1
  end
  assert(count == 500 and sum == 31250)
  for key in pairs(restored.metadata) do
    assert(record.metadata[key] ~= nil, key)
  end
  return sum
end

-- LPeg compiles the pattern on first use. Share the compiled decoder.
roundtrip(0)

local function run(task)
  local sum = 0
  for i = 1, iterations do
    sum = sum + roundtrip((task - 1) * iterations + i)
  end
  return sum
end

local function noop() end
return run, noop, noop, 31250 * iterations
