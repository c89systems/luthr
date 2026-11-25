local function pipeline(depth)
  if depth==0 then return coroutine.wrap(function(value) return value end) end
  local previous=pipeline(depth-1)
  return coroutine.wrap(function(value) return previous(value)+1 end)
end
assert(pipeline(30)(12)==42)

local function create_pair()
  local state = {value = 12}
  local function advance(delta)
    state.value = state.value + delta
    return state
  end
  return coroutine.create(advance), coroutine.wrap(advance)
end
local first, second = create_pair()
collectgarbage('collect')
local ok, state = coroutine.resume(first, 10)
assert(ok and state.value == 22)
assert(second(20) == state and state.value == 42)

local yield = coroutine.yield
local protected
protected = coroutine.create(function()
  return pcall(function()
    assert(coroutine.running() == protected)
    local first, second, third = yield("first", nil, 42)
    assert(first == 12 and second == nil and third == 30)
    local result = yield(first + third)
    return result, "finished"
  end)
end)
local ok, first, second, third = coroutine.resume(protected)
assert(ok and first == "first" and second == nil and third == 42)
assert(debug.getinfo(protected, 0, "f").func == yield)
assert(coroutine.status(protected) == "suspended")
ok, first = coroutine.resume(protected, 12, nil, 30)
assert(ok and first == 42)
ok, first, second, third = coroutine.resume(protected, "result")
assert(ok and first == true and second == "result" and third == "finished")
assert(coroutine.status(protected) == "dead")

print('coroutine pipeline: thirty nested wrapped coroutines passed')
