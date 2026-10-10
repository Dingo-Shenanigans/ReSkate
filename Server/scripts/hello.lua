-- /hello <anything>: the smallest script command.
server.command("hello", function(player, args)
  return "hi " .. player.name .. ": " .. args
end)
