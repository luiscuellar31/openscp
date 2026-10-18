// TLS server verification with the trust of Windows itself, for the libcurl
// backends. OpenSSL has no trust store of its own on Windows, and the copy of
// the system's that libcurl can give it lacks the roots Windows downloads when
// it first needs them, the Disallowed store and the distrust Microsoft ships.
// Windows only.
#pragma once

#ifdef _WIN32

#include <curl/curl.h>

#include <vector>

namespace openscp::curlcommon {

// Why Windows does not trust, for TLS server authentication, the server whose
// DER certificate comes first in `chain`, followed by the intermediates it
// sent: a code such as CERT_E_UNTRUSTEDROOT or CERT_E_EXPIRED, or 0 when
// Windows trusts it. The host name is left to libcurl, which also checks IP
// addresses. `engine` is an HCERTCHAINENGINE, null for the current user's
// trust, which tests replace with roots of their own.
unsigned long
windowsServerChainError(const std::vector<std::vector<unsigned char>> &chain,
                        void *engine = nullptr);

// A CURLOPT_SSL_CTX_FUNCTION that has OpenSSL ask windowsServerChainError()
// instead of its own trust store.
CURLcode useWindowsServerTrust(CURL *curl, void *sslContext, void *data);

} // namespace openscp::curlcommon

#endif
