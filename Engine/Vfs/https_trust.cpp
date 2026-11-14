#include "https_trust.h"
#include "isrg_roots.h"
#include <Windows.h>
#include <wincrypt.h>
#include <string>
#include <vector>

namespace dingosdk::https {
namespace {
// The DER bytes of a one-line PEM certificate (isrg_roots.h).
std::vector<BYTE> certificate_bytes(std::string_view pem) {
    constexpr std::string_view begin = "-----BEGIN CERTIFICATE-----", end = "-----END CERTIFICATE-----";
    if (!pem.starts_with(begin) || !pem.ends_with(end)) return {};
    const auto base64 = pem.substr(begin.size(), pem.size() - begin.size() - end.size());
    DWORD size{};
    if (!CryptStringToBinaryA(base64.data(), static_cast<DWORD>(base64.size()), CRYPT_STRING_BASE64, nullptr, &size, nullptr, nullptr))
        return {};
    std::vector<BYTE> bytes(size);
    if (!CryptStringToBinaryA(base64.data(), static_cast<DWORD>(base64.size()), CRYPT_STRING_BASE64, bytes.data(), &size, nullptr, nullptr))
        return {};
    bytes.resize(size);
    return bytes;
}
// A chain engine whose only trusted roots are ReSkate's (hExclusiveRoot): the Windows store, and
// whatever is or is not in it, plays no part. Made once and kept for the process.
HCERTCHAINENGINE builtin_engine() {
    static const HCERTCHAINENGINE engine = [] {
        HCERTSTORE roots = CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0, 0, nullptr);
        if (!roots) return HCERTCHAINENGINE{};
        int added{};
        for (const auto pem : {isrg_root_x1, isrg_root_x2}) {
            const auto bytes = certificate_bytes(pem);
            if (!bytes.empty() && CertAddEncodedCertificateToStore(roots, X509_ASN_ENCODING, bytes.data(),
                                                                   static_cast<DWORD>(bytes.size()), CERT_STORE_ADD_ALWAYS, nullptr))
                ++added;
        }
        HCERTCHAINENGINE made{};
        CERT_CHAIN_ENGINE_CONFIG config{};
        config.cbSize = sizeof(config);
        config.hExclusiveRoot = roots;
        if (!added || !CertCreateCertificateChainEngine(&config, &made)) made = {};
        CertCloseStore(roots, 0); // (the engine keeps its own reference)
        return made;
    }();
    return engine;
}
}

bool chains_to_builtin_root(const void* certificate, std::wstring_view host) {
    const auto server = static_cast<PCCERT_CONTEXT>(certificate);
    const auto engine = builtin_engine();
    if (!server || !engine || host.empty()) return false;
    LPSTR usages[] = {const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH)};
    CERT_CHAIN_PARA para{};
    para.cbSize = sizeof(para);
    para.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    para.RequestedUsage.Usage.cUsageIdentifier = 1;
    para.RequestedUsage.Usage.rgpszUsageIdentifier = usages;
    PCCERT_CHAIN_CONTEXT chain{};
    // The intermediates come from what the server sent (its store); nothing is fetched.
    if (!CertGetCertificateChain(engine, server, nullptr, server->hCertStore, &para, CERT_CHAIN_CACHE_ONLY_URL_RETRIEVAL, nullptr,
                                 &chain))
        return false;
    std::wstring name(host);
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA ssl{};
    ssl.cbSize = sizeof(ssl);
    ssl.dwAuthType = AUTHTYPE_SERVER;
    ssl.pwszServerName = name.data();
    CERT_CHAIN_POLICY_PARA policy{};
    policy.cbSize = sizeof(policy);
    policy.pvExtraPolicyPara = &ssl;
    CERT_CHAIN_POLICY_STATUS status{};
    status.cbSize = sizeof(status);
    const bool trusted = chain->TrustStatus.dwErrorStatus == CERT_TRUST_NO_ERROR &&
                         CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, chain, &policy, &status) && status.dwError == 0;
    CertFreeCertificateChain(chain);
    return trusted;
}
}
