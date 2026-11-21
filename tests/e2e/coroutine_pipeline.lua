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

print('coroutine pipeline: thirty nested wrapped coroutines passed')
