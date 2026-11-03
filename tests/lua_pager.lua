local mdf = require("libmdf")
local path, mode = arg[1], arg[2]
local failure = {}
local calls = 0
local ok, result, reason = pcall(mdf.pager, path, {
  format = "text",
  should_cancel = function()
    calls = calls + 1
    if mode == "early" then return true end
    if calls < 12 then return false end
    if mode == "error" then error(failure) end
    return true
  end,
})
if mode == "error" then
  assert(not ok and result == failure, "callback error is rethrown unchanged after pager cleanup")
else
  assert(ok and result == false and reason == "cancelled", "cancellation has a distinct result")
end
assert(calls > 0)
io.write("LUA_PAGER_DONE\n")
