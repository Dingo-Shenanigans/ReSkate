-- /rules: the server's rules, one chat line each. Edit the list.
local rules = {
  "1. Be nice. No harassment, slurs or spam.",
  "2. Don't block spots with objects; clean up what you place.",
  "3. Admins have the last word.",
}

server.command("rules", function()
  return table.concat(rules, "\n")
end)
