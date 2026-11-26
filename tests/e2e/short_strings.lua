local _, count = ...
local names = {}
local function name(id, index)
  return string.pack(">I4I4", id, index)
end

local function reset()
  for i = 1, 128 do names[name(0, i)] = i end
end

local function work(id)
  local own = {}
  local sum = 0
  for i = 1, count do
    own[name(id, i)] = i
    local stable = i % 128 + 1
    assert(names[name(0, stable)] == stable)
    local result = name(id + 1024, i)
    assert(#result == 8 and rawlen(result) == 8)
    if i % 127 == 0 then collectgarbage("step", 0) end
  end
  collectgarbage("collect")
  for i = 1, count do
    local value = assert(own[name(id, i)])
    assert(value == i)
    sum = sum + value
  end
  local weak = setmetatable({}, {__mode = "kv"})
  do
    local value = name(id + 2048, count)
    weak[value] = value
  end
  collectgarbage("collect")
  local key = name(id + 2048, count)
  assert(rawequal(weak[key], key))
  return sum
end

local function verify()
  for i = 1, 128 do assert(names[name(0, i)] == i) end
end

return work, reset, verify, count * (count + 1) // 2
