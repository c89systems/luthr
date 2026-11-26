local mode, iterations = ...
assert(mode == "pslog_callback" or mode == "pslog_file")
local pslog = require("pslog")
local line = '{"lvl":"info","msg":"checkout","id":42,"status":"ready","ok":true}\n'
local work = #line * iterations

local function run()
  local file, sample
  local produced, chunks = 0, 0
  local output
  if mode == "pslog_callback" then
    output = function(chunk)
      produced = produced + #chunk
      chunks = chunks + 1
      if sample == nil then sample = chunk end
    end
  else
    file = assert(io.tmpfile())
    output = file
  end

  local logger = pslog.new_json(output, {disable_timestamp = true})
  local write = logger.info
  for _ = 1, iterations do
    write(logger, "checkout", "id", 42, "status", "ready", "ok", true)
  end
  logger:close()

  if file then
    produced = assert(file:seek("end"))
    assert(file:seek("set"))
    sample = assert(file:read(#line))
    assert(file:close())
  else
    assert(chunks == iterations, "expected one chunk per line")
  end
  assert(sample == line, "unexpected libpslog record")
  assert(produced == work, "unexpected libpslog byte count")
  return produced
end

local function noop() end
return run, noop, noop, work
