-- A tip in chat every few minutes, in turn. Edit the list and the interval.
local tips = {
  "Tip: /goto <player> takes you to a friend.",
  "Tip: /rules shows this server's rules.",
  "Tip: /help lists every command you can use.",
}
local minutes = 5

local next_tip = 1
server.every(minutes * 60, function()
  if #server.players() == 0 then return end -- nobody to tell
  server.say(tips[next_tip])
  next_tip = next_tip % #tips + 1
end)
