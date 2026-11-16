local id = file_task_id()
file_start:recv()
if id == 0 then
  return await_checked(2)
end
local body = file_bodies[id]
file_bodies[id] = nil
return body()
