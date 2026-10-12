-- Greets each player as they join, and tells everyone when someone leaves.
server.on("join", function(player)
  server.tell(player, "Welcome, " .. player.name .. "! Type /help for this server's commands.")
  server.say(player.name .. " joined. " .. #server.players() .. " skating.")
end)

server.on("leave", function(player)
  server.say(player.name .. " left.")
end)
