local mode, count, tasks = ...
local fixture = require("compat_fixture")
local name = "compat_fixture.finalizer"
local registrations, calls, seen, resurrected, problem
local weak = setmetatable({}, {__mode = "v"})

local function finalize(object)
  if collectgarbage("collect") ~= nil then problem = "GC reentered a finalizer" end
  local id = type(object) == "table" and object.id or fixture.finalizer_id(object, name)
  if mode == "native_finalizer_order" then
    calls[#calls + 1] = id
    if id == count + 2 then
      local payload = debug.getuservalue(object)
      if type(payload) ~= "table" or payload.id ~= id or weak[1] ~= nil then
        problem = "finalizer lost a user value or retained a weak reference"
      end
      if resurrected == nil then
        resurrected = object
        fixture.bind_finalizer(object, name)
      end
    end
  else
    if mode == "native_finalizer_blocked" and id == 1 then
      fixture.block_finalizer()
      if fixture.finalizer_id(object, name) ~= id then problem = "lost blocked finalizer payload" end
    end
    seen[id] = (seen[id] or 0) + 1
  end
end

local mt = {__gc = finalize}
debug.getregistry()[name] = mt

local function create_and_release()
  local records = {}
  for id = 1, count do records[id] = fixture.finalizer_payload(id) end
  for id = count, 1, -1 do
    fixture.bind_finalizer(records[id], name)
    registrations[#registrations + 1] = id
  end
  records[count + 1] = setmetatable({id = count + 1}, mt)
  registrations[#registrations + 1] = count + 1
  local with_reference = fixture.finalizer_payload(count + 2)
  fixture.bind_finalizer(with_reference, name)
  debug.setuservalue(with_reference, {id = count + 2})
  weak[1] = with_reference
  registrations[#registrations + 1] = count + 2
end

local function order()
  create_and_release()
  collectgarbage("collect")
  assert(not problem, problem)
  assert(#calls == #registrations and resurrected ~= nil and weak[1] == nil)
  for i = 1, #calls do
    assert(calls[i] == registrations[#registrations + 1 - i],
           "finalizers ran out of registration order at " .. i)
  end
  resurrected = false
  collectgarbage("collect")
  assert(not problem, problem)
  assert(#calls == #registrations + 1 and calls[#calls] == count + 2)
  resurrected = nil
  return count
end

local function churn(task)
  if mode == "native_finalizer_blocked" and task == 2 then
    while not fixture.finalizer_waiting() do end
    collectgarbage("collect")
    fixture.finalizer_waiting(true)
    return count
  end
  for i = 1, count do
    local record = fixture.finalizer_payload((task - 1) * count + i)
    fixture.bind_finalizer(record, name)
  end
  return count
end

local function reset()
  registrations, calls, seen, resurrected, problem = {}, {}, {}, nil, nil
end

local function validate()
  assert(not problem, problem)
  if mode ~= "native_finalizer_order" then
    local total = mode == "native_finalizer_blocked" and count or count * tasks
    for id = 1, total do assert(seen[id] == 1, "bad finalizer count for " .. id) end
  end
end

assert(mode == "native_finalizer_order" or mode == "native_task_finalizers" or
       mode == "native_finalizer_blocked")
return mode == "native_finalizer_order" and order or churn, reset, validate, count
