-- /goto <player>: teleport yourself next to another player.
server.command("goto", function(player, args)
  if args == "" then return "/goto <player>" end
  local target = server.player(args)
  if not target then return "No single player matches \"" .. args .. "\"." end
  if target.id == player.id then return "That is you." end
  if not target.x then return target.name .. " has no position yet." end
  -- A step beside them and a little up, so nobody lands inside anyone.
  if not server.teleport(player, target.x + 2, target.y + 1, target.z) then
    return "You can't be teleported until your game has loaded the map."
  end
  return "Teleported to " .. target.name .. "."
end)
