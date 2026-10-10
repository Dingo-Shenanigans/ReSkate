-- /props (admins): who has placed the most objects. /props clear deletes them all.
server.command("props", function(player, args)
  if args == "clear" then return server.clear_objects() end
  local players = server.players()
  table.sort(players, function(a, b) return (a.objects or 0) > (b.objects or 0) end)
  local lines, total = {}, 0
  for _, p in ipairs(players) do
    total = total + (p.objects or 0)
    if #lines < 5 and (p.objects or 0) > 0 then lines[#lines + 1] = p.objects .. "  " .. p.name end
  end
  table.insert(lines, 1, total .. " objects placed" .. (total > 0 and "; most:" or "."))
  return table.concat(lines, "\n")
end, {admin = true})
