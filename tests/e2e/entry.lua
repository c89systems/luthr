assert(_VERSION == 'Lua 5.5')
local directory, source, collector = assert(arg[1]), assert(arg[2]), assert(arg[3])
e2e_workdir = assert(arg[4])
package.cpath = directory .. '/?.so'
collectgarbage(collector)
assert(loadfile(source))()
