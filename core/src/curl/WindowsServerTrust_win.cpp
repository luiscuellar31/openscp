#include "curl/WindowsServerTrust_win.hpp"

// wincrypt.h comes before OpenSSL, whose headers undefine the names the two
// share, such as X509_NAME.
// clang-format off
#include <windows.h>
#include <wincrypt.h>
// clang-format on

#include <openssl/ssl.h>
#include <openssl/x509.h>

#include <cstddef>

namespace openscp::curlcommon {
namespace {

// Frees what one chain check opened.
struct ChainCheck {
    PCCERT_CONTEXT server = nullptr;
    HCERTSTORE sent = nullptr;
    PCCERT_CHAIN_CONTEXT chain = nullptr;

    ChainCheck() = default;
    ChainCheck(const ChainCheck &) = delete;
    ChainCheck &operator=(const ChainCheck &) = delete;
    ~ChainCheck() {
        if (chain)
            ::CertFreeCertificateChain(chain);
        if (sent)
            ::CertCloseStore(sent, 0);
        if (server)
            ::CertFreeCertificateContext(server);
    }
};

constexpr DWORD kCertificateEncoding = X509_ASN_ENCODING | PKCS_7_ASN_ENCODING;

std::vector<unsigned char> derOf(X509 *certificate) {
    std::vector<unsigned char> der;
    const int length = certificate ? i2d_X509(certificate, nullptr) : 0;
    if (length <= 0)
        return der;
    der.resize(static_cast<std::size_t>(length));
    unsigned char *end = der.data();
    if (i2d_X509(certificate, &end) != length)
        der.clear();
    return der;
}

// The OpenSSL reason that libcurl reports for a Windows one.
int opensslReason(unsigned long windowsError) {
    const auto is = [windowsError](HRESULT code) {
        return windowsError == static_cast<unsigned long>(code);
    };
    if (is(CERT_E_EXPIRED))
        return X509_V_ERR_CERT_HAS_EXPIRED;
    if (is(CERT_E_CHAINING))
        return X509_V_ERR_UNABLE_TO_GET_ISSUER_CERT_LOCALLY;
    // Such as a certificate in the Disallowed store.
    if (is(TRUST_E_EXPLICIT_DISTRUST))
        return X509_V_ERR_CERT_REJECTED;
    if (is(CERT_E_WRONG_USAGE))
        return X509_V_ERR_INVALID_PURPOSE;
    if (is(TRUST_E_CERT_SIGNATURE))
        return X509_V_ERR_CERT_SIGNATURE_FAILURE;
    return X509_V_ERR_CERT_UNTRUSTED;
}

// Replaces OpenSSL's check of the chain the server sent.
int verifyWithWindows(X509_STORE_CTX *context, void *) {
    std::vector<std::vector<unsigned char>> chain{
        derOf(X509_STORE_CTX_get0_cert(context))};
    if (STACK_OF(X509) *sent = X509_STORE_CTX_get0_untrusted(context)) {
        for (int index = 0; index < sk_X509_num(sent); ++index)
            chain.push_back(derOf(sk_X509_value(sent, index)));
    }
    const unsigned long error = windowsServerChainError(chain);
    X509_STORE_CTX_set_error(context,
                             error == 0 ? X509_V_OK : opensslReason(error));
    return error == 0 ? 1 : 0;
}

} // namespace

unsigned long
windowsServerChainError(const std::vector<std::vector<unsigned char>> &chain,
                        void *engine) {
    if (chain.empty() || chain.front().empty())
        return static_cast<unsigned long>(CERT_E_CHAINING);
    ChainCheck check;
    check.server = ::CertCreateCertificateContext(
        kCertificateEncoding, chain.front().data(),
        static_cast<DWORD>(chain.front().size()));
    check.sent = ::CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0,
                                 CERT_STORE_CREATE_NEW_FLAG, nullptr);
    if (!check.server || !check.sent)
        return ::GetLastError();
    for (std::size_t index = 1; index < chain.size(); ++index) {
        (void)::CertAddEncodedCertificateToStore(
            check.sent, kCertificateEncoding, chain[index].data(),
            static_cast<DWORD>(chain[index].size()), CERT_STORE_ADD_ALWAYS,
            nullptr);
    }

    // Windows fetches missing intermediates and roots as it builds the chain,
    // as for its own connections. Revocation stays unchecked, as before.
    LPSTR serverAuthentication = const_cast<LPSTR>(szOID_PKIX_KP_SERVER_AUTH);
    CERT_CHAIN_PARA parameters{};
    parameters.cbSize = sizeof(parameters);
    parameters.RequestedUsage.dwType = USAGE_MATCH_TYPE_AND;
    parameters.RequestedUsage.Usage.cUsageIdentifier = 1;
    parameters.RequestedUsage.Usage.rgpszUsageIdentifier =
        &serverAuthentication;
    if (!::CertGetCertificateChain(static_cast<HCERTCHAINENGINE>(engine),
                                   check.server, nullptr, check.sent,
                                   &parameters, 0, nullptr, &check.chain))
        return ::GetLastError();

    // Without a server name, the policy leaves the name to libcurl.
    SSL_EXTRA_CERT_CHAIN_POLICY_PARA server{};
    server.cbSize = sizeof(server);
    server.dwAuthType = AUTHTYPE_SERVER;
    CERT_CHAIN_POLICY_PARA policy{};
    policy.cbSize = sizeof(policy);
    policy.pvExtraPolicyPara = &server;
    CERT_CHAIN_POLICY_STATUS status{};
    status.cbSize = sizeof(status);
    if (!::CertVerifyCertificateChainPolicy(CERT_CHAIN_POLICY_SSL, check.chain,
                                            &policy, &status))
        return ::GetLastError();
    return status.dwError;
}

CURLcode useWindowsServerTrust(CURL *, void *sslContext, void *) {
    SSL_CTX_set_cert_verify_callback(static_cast<SSL_CTX *>(sslContext),
                                     verifyWithWindows, nullptr);
    return CURLE_OK;
}

} // namespace openscp::curlcommon
