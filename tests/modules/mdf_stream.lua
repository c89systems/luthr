local mdf = require('libmdf')
mdf.render_stream(function(cap)
  return binding_read(cap)
end, function(chunk)
  binding_stream_callback()
  q:send(chunk)
end, {ansi_mode='off', boring=true, width=1024})
q:close()
return 42
