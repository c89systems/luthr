local _, events, tasks = ...
local reports = {}
local format = "<I4I8"

local function read_events(file)
  return coroutine.wrap(function()
    for line in file:lines() do
      local level, bytes, service = line:match("^(%u+) (%d+) (.+)$")
      assert(level and utf8.len(service), "malformed event")
      coroutine.yield(level, tonumber(bytes), service)
    end
  end)
end

local function run(id)
  local input <close> = assert(io.tmpfile())
  for number = 1, events do
    assert(input:write(("INFO %d métro\nWARN %d edge\n"):format(number, number * 2)))
  end
  assert(input:seek("set"))
  local services, warnings = {}, 0
  for level, bytes, name in read_events(input) do
    services[name] = (services[name] or 0) + bytes
    if level == "WARN" then warnings = warnings + 1 end
  end

  local names = {}
  for name in pairs(services) do names[#names + 1] = name end
  table.sort(names)
  local rows = {}
  for _, name in ipairs(names) do
    rows[#rows + 1] = string.pack(format, utf8.len(name), services[name])
  end
  reports[id] = { names = names, totals = table.concat(rows), warnings = warnings }
  return services["métro"] + services.edge + warnings
end

local function reset()
  for id = 1, tasks do reports[id] = nil end
end

local bytes = events * (events + 1) // 2
local function verify()
  for id = 1, tasks do
    local report = assert(reports[id], "missing event report")
    assert(#report.names == 2 and report.names[1] == "edge" and report.names[2] == "métro")
    assert(report.warnings == events, "incorrect warning count")
    local length, total, offset = string.unpack(format, report.totals)
    assert(length == 4 and total == bytes * 2, "incorrect edge report")
    length, total, offset = string.unpack(format, report.totals, offset)
    assert(length == 5 and total == bytes and offset == #report.totals + 1,
           "incorrect métro report")
  end
end

return run, reset, verify, bytes * 3 + events
