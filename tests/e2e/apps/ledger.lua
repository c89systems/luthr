local _, transactions, tasks = ...
local reports = {}
local accounts = { "Chloé", "Elsa", "Zoë" }
local Account = {}
Account.__index = Account

function Account:post(amount)
  self.balance = self.balance + amount
  self.entries = self.entries + 1
end

local function open_book()
  local book = {}
  for _, name in ipairs(accounts) do
    book[name] = setmetatable({ balance = 1000, entries = 0 }, Account)
  end
  return book
end

local function post_transfer(book, line)
  local from, to, cents = line:match("^([^,]+),([^,]+),(%d+)$")
  local amount = cents and math.tointeger(tonumber(cents))
  assert(amount and amount > 0, "invalid transfer")
  local debit = assert(book[from], "unknown source")
  local credit = assert(book[to], "unknown destination")
  assert(debit.balance >= amount, "insufficient funds")
  debit:post(-amount)
  credit:post(amount)
end

local function run(id)
  local book = open_book()
  for _ = 1, transactions do
    for line in ("Zoë,Elsa,50\nElsa,Chloé,25\nChloé,Zoë,50\nElsa,Chloé,25\n"):gmatch("[^\n]+") do
      post_transfer(book, line)
    end
  end
  local ok, message = pcall(post_transfer, book, "Zoë,missing,1")
  assert(not ok and message:find("unknown destination", 1, true))
  ok, message = pcall(post_transfer, book, "Zoë,Elsa,1001")
  assert(not ok and message:find("insufficient funds", 1, true))

  local balances, total, entries = {}, 0, 0
  for name, account in pairs(book) do
    balances[name] = account.balance
    total = total + account.balance
    entries = entries + account.entries
  end
  reports[id] = { balances = balances, entries = entries }
  return total * entries
end

local function reset()
  for id = 1, tasks do reports[id] = nil end
end

local function verify()
  for id = 1, tasks do
    local report = assert(reports[id], "missing ledger report")
    assert(report.entries == transactions * 8)
    for _, name in ipairs(accounts) do
      assert(report.balances[name] == 1000, "incorrect balance for " .. name)
    end
  end
end

return run, reset, verify, 3000 * transactions * 8
