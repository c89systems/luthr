assert(_VERSION == "Lua 5.5")
assert(math.maxinteger == 9223372036854775807)
local shared = { count = 0 }
local task = coroutine.create(function()
  shared.count = shared.count + 1
  coroutine.yield("waiting")
  shared.count = shared.count + 1
  return 42
end)
local ok, value = coroutine.resume(task)
assert(ok and value == "waiting" and shared.count == 1)
ok, value = coroutine.resume(task)
assert(ok and value == 42 and shared.count == 2)
assert(coroutine.status(task) == "dead")
local closed = 0
ok = pcall(function()
  local resource <close> = setmetatable({}, {
    __close = function() closed = closed + 1 end,
  })
  error("expected")
end)
assert(not ok and closed == 1)
local dumped = string.dump(function(a, b) return a + b end)
assert(assert(load(dumped))(20, 22) == 42)
collectgarbage("collect")
print("interpreter behavior: passed")
