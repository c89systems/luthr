local _, count = ...
local names = {}
local function name(id, index)
  return string.pack(">I4I4", id, index)
end

local function reset()
  for i = 1, 128 do names[name(0, i)] = i end
end

local function check_captures(id)
  local body = ("bin\0"):rep(15)
  local iterator = coroutine.wrap(function()
    for i = 1, 64 do
      local subject = ("%d:"):format(id * 1000 + i) .. body
      local key, value, finish = subject:match("^(%d+):(.+)()$")
      coroutine.yield(key, value, finish)
    end
  end)
  local captured, seen = {}, 0
  for key, value, finish in iterator do
    seen = seen + 1
    assert(key == tostring(id * 1000 + seen) and #value == #body)
    assert(finish == #key + #body + 2)
    captured[key] = value
  end
  assert(seen == 64)
  collectgarbage("collect")
  for i = 1, seen do
    assert(rawequal(captured[tostring(id * 1000 + i)], body))
  end
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
  check_captures(id)
  return sum
end

local function verify()
  for i = 1, 128 do assert(names[name(0, i)] == i) end
end

return work, reset, verify, count * (count + 1) // 2
