#include "Server/name_check.h"
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace names = dingosdk::server::names;
namespace {
void check(bool ok, const std::string &message) {
    if (ok) return;
    std::cerr << message << '\n';
    std::exit(1);
}
}

int main() {
    const std::vector<std::string> tags{"OWNER", "Mod"};
    check(names::strip_invisible("Pin\xE2\x80\x8Bgu") == "Pingu", "A zero-width space was kept");
    check(names::strip_invisible("Bob\xE2\x80\xAE") == "Bob", "A right-to-left override was kept");
    check(names::hides("Pin\xE2\x80\x8Bgu") && !names::hides("Heart \xE2\x9D\xA4\xEF\xB8\x8F"), "An emoji counted as hiding");
    check(names::fold("P\xD1\x96ngu") == names::fold("Pingu"), "A Cyrillic i did not fold to i");
    check(names::fold("\xD0\xA0ingu") == names::fold("pingu"), "A Cyrillic P did not fold to p");
    check(names::fold("P1ngu") == names::fold("PINGU"), "1 and I did not fold together");
    check(names::fold("\xEF\xBC\xB0ingu") == names::fold("Pingu"), "A fullwidth P did not fold to p");
    check(names::fold("Pi ng_u") == names::fold("Pingu"), "Spacing changed the folded name");
    check(names::fold("Pingu") != names::fold("Pongo"), "Different names folded together");
    check(names::strip_fake_tags("[Admin] Bob", tags) == "Bob", "A fake admin tag was kept");
    check(names::strip_fake_tags("[ADM1N] | Bob", tags) == "Bob", "A disguised fake admin tag was kept");
    check(names::strip_fake_tags("(owner)Bob", tags) == "Bob", "A role's own tag was kept");
    check(names::strip_fake_tags("[ABC] Bob", tags) == "[ABC] Bob", "A clan tag was removed");
    check(names::strip_fake_tags("Bob [Admin]", tags) == "Bob [Admin]", "Text after the name was treated as a tag");
    check(names::staff_word(names::fold("m0derator"), {}), "A disguised staff word was missed");
    check(names::strip_fake_tags("[GHOST] Bob", tags) == "[GHOST] Bob", "A clan tag containing a staff word was removed");
    check(names::strip_fake_tags("[Devils] Bob", tags) == "[Devils] Bob", "A clan tag starting with a staff word was removed");
    check(names::strip_fake_tags("[Admins] Bob", tags) == "Bob", "A plural staff tag was kept");
    check(names::staff_name(names::fold("Adm1n")) && names::staff_name(names::fold("Moderators")), "A staff title name was allowed");
    check(!names::staff_name(names::fold("Dev")) && !names::staff_name(names::fold("Host")) && !names::staff_name(names::fold("Administrative")),
          "An ordinary name was taken for a staff title");
    check(names::strip_invisible(std::string("A\xFF" "B")) == "AB", "Broken UTF-8 was kept");
    return 0;
}
