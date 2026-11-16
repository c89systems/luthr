local function hex(value)
  return (value:gsub('.',function(byte) return string.format('%02x',byte:byte()) end))
end
local names={'C','ptb','pt_BR.iso88591','ISO-8859-1','pt_BR'}
local lower=string.char(224,225,226,227,228)
local mixed=string.char(224,193,227,195)
for _,name in ipairs(names) do
  assert(os.setlocale('C','all'))
  local selected=os.setlocale(name,'collate')
  local middle=string.char(225)..'lo'
  print('collate',name,selected or false,'alo'<middle,middle<'amo')

  assert(os.setlocale('C','all'))
  selected=os.setlocale(name,'ctype')
  print('ctype',name,selected or false,hex(lower:gsub('%a','x')),
    hex(mixed:gsub('%l','x')),hex(mixed:gsub('%u','x')),hex(string.upper(lower)))

  assert(os.setlocale('C','all'))
  selected=os.setlocale(name,'numeric')
  print('numeric',name,selected or false,tonumber('3,4'),tonumber('3.4'))
  assert(tonumber('  -.4  ')==-0.4 and tonumber('  +0x.41  ')==0x0.41)
  assert(not load('a=(3,4)'))
  assert(assert(load('return 3.4'))()==3.4)
  assert(assert(load('return .4,3'))()==.4)
  assert(assert(load('return 4.'))()==4)
  assert(assert(load('return 4.+.5'))()==4.5)
  assert(not tonumber('inf') and not tonumber('NAN'))
  assert(assert(load(string.format('return %q',4.51)))()==4.51)
  local ok,value=pcall(function() return '0x.1'+'0x,1'+'-0X.1' end)
  print('hexadecimal arithmetic',ok,ok and value or 'not convertible')
end
assert(os.setlocale('C','all'))
assert(os.setlocale()=='C' and os.setlocale(nil,'numeric')=='C')
print('locale: actual libc collation, character classes, numeric parsing and C reset passed')
