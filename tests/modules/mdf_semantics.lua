local mdf = require('libmdf')
local checkpoint = binding_checkpoint or function() end
local id = binding_task_id and binding_task_id() or 2
local mode = binding_fault and binding_fault() or 'normal'
local options = {ansi_mode='off', boring=true, width=80}
local plain = mdf.render('hello world\n\n', options)
assert(plain == 'hello world\n', string.format('%q',plain))
local h = mdf.new(options)
local chunks, traces = {}, {}
h:set_sink(function(chunk)
  checkpoint(id)
  if mode == 'error' then error('MDF sink expected') end
  if mode == 'oom' then binding_oom() end
  chunks[#chunks+1] = chunk
end)
local pos = 1
local source = 'hello world\n\n'
local ok, message = pcall(function()
  h:render_stream(function(cap)
    if pos > #source then return nil end
    local chunk = source:sub(pos, pos+math.min(cap,3)-1)
    pos = pos+#chunk
    return chunk
  end)
end)
if mode == 'error' then
  assert(not ok and tostring(message):find('MDF sink expected',1,true))
elseif mode == 'oom' then
  assert(not ok and tostring(message):find('not enough memory',1,true))
else
  assert(ok, message)
  assert(table.concat(chunks) == 'hello world\n')
end
h:reset()
h:set_sink(function(chunk) chunks[#chunks+1] = chunk end)
assert(h:render('recovered\n\n') == 'recovered\n')
h:close()
h:close()
assert(not pcall(function() h:render('closed') end))

-- Callback failures preserve their original object after native cleanup.
local marker = {}
local nested = false
local bad_options = setmetatable({}, {__index=function(_, key)
  if not nested then
    nested=true
    local inner=mdf.new(options)
    inner:close()
  end
  if key == 'width' then error(marker) end
end})
local constructed, construction_error = pcall(mdf.new, bad_options)
assert(nested and not constructed and construction_error == marker)
if mode == 'oom' then
  local allocated, allocation_error = pcall(mdf.new, setmetatable({}, {
    __index=function() binding_oom() end,
  }))
  assert(not allocated and tostring(allocation_error):find('not enough memory',1,true))
end
for _, kind in ipairs({'source','sink','trace'}) do
  local once = false
  local opts = {ansi_mode='off', boring=true, width=80,
    write_trace=function(format, chunk)
      assert(format == 'ansi' and type(chunk) == 'string')
      if kind == 'trace' then error(marker) end
    end}
  local success, failure = pcall(mdf.render_stream, function(cap)
    assert(cap > 0)
    if kind == 'source' then error(marker) end
    if once then return nil end
    once=true
    return 'word boundary\n\n'
  end, function(chunk)
    assert(#chunk > 0)
    if kind == 'sink' then error(marker) end
  end, opts)
  assert(not success and failure == marker)
end

-- A stream can outlive its creating coroutine and uses the caller's state.
local output = {}
local stream = coroutine.wrap(function()
  return mdf.document_stream({ansi_mode='off', boring=true, width=80,
    write_trace=function(format, chunk) traces[#traces+1]=chunk end},
    function(chunk) output[#output+1]=chunk end)
end)()
collectgarbage('collect')
assert(stream:write('first '))
local before = #output
assert(stream:flush() and #output == before)
assert(stream:set_geometry(80,0,0))
assert(stream:write('document\n\n'))
assert(stream:finish_document())
assert(table.concat(output) == 'first document\n')
assert(table.concat(traces) == table.concat(output))
assert(stream:begin_document())
assert(stream:write('next\n\n') and stream:finish_document())
stream:close()
stream=nil
collectgarbage('collect')
return 42
