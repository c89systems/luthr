local function pipeline(depth)
  if depth==0 then return coroutine.wrap(function(value) return value end) end
  local previous=pipeline(depth-1)
  return coroutine.wrap(function(value) return previous(value)+1 end)
end
assert(pipeline(30)(12)==42)
print('coroutine pipeline: thirty nested wrapped coroutines passed')
