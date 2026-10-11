#include "Extension/Boot/ea_service_block.h"
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
using namespace dingosdk;
void check(bool value, const std::string& message) { if (!value) throw std::runtime_error(message); }
bool blocked(std::string_view host) { return ea_service_host(host); }
bool blocked(std::wstring_view host) { return ea_service_host(host); }
static_assert(ea_service_host(std::string_view("collector.errors.ea.com")), "The match is usable at compile time");
int main() {
    try {
        // Every EA service the game contacted in a recorded session, and a sample of the others it names.
        for (const auto* host : {"collector.errors.ea.com", "a-collector.errors.ea.com", "freeform-river.data.ea.com",
                "gcs.ea.com", "pin-river-grpc.data.ea.com", "pin-em.data.ea.com", "spring25.client.blazeredirector.ea.com",
                "spring18.gosredirector.ea.com", "accounts.ea.com", "gateway.grpc.ea.com", "experimentation-grpc.data.ea.com",
                "adtech-runtime-rest-internal.data.ea.com", "river-grpc-lt.eadpdata.ddns.ea.com", "pn.tnt-ea.com",
                "rtm.tnt-ea.com", "ea.com", "tnt-ea.com"})
            check(blocked(host), std::string("EA service must be blocked: ") + host);
        check(blocked("COLLECTOR.Errors.EA.com"), "Host names compare without case");
        check(blocked("gcs.ea.com."), "A trailing root dot is the same name");
        check(blocked(L"freeform-river.data.ea.com"), "Wide lookups are blocked too");
        check(blocked(L"GCS.EA.COM."), "Wide lookups compare without case and root dot");
        // What ReSkate and the game still need, and names that only look like EA's.
        for (const auto* host : {"dingo-dev-assets.akamaized.net", "api.reskate.dev", "api.github.com",
                "raw.githubusercontent.com", "thunderstore.io", "api.steampowered.com", "idea.com", "notea.com",
                "xtnt-ea.com", "ea.com.example.net", "ea.co", "eacom", "com", ".", "", "159.153.1.1"})
            check(!blocked(host), std::string("Host must stay reachable: ") + host);
        check(!blocked(L"dingo-dev-assets.akamaized.net"), "The fast-travel artwork CDN stays reachable");
        check(!blocked(L"gcs.ea.cöm"), "Characters outside ASCII never match a domain letter");
        check(!blocked("gcs.ea.com.."), "Only one root dot is stripped");
        std::cout << "EA service block checks passed.\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
