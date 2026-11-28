#pragma once

#include <cstddef>
#include <string_view>

namespace openscp::libssh2detail {

// The size in bits of the modulus of an "ssh-rsa" public key blob, as
// ssh-keygen -l shows it, or 0 when the blob is not one. RFC 4253 section 6.6
// lays the blob out as the type string, then the mpints e and n, each behind
// a 32-bit big-endian length.
inline int rsaModulusBits(const unsigned char *blob, std::size_t length) {
    std::size_t offset = 0;
    const auto nextField = [&](const unsigned char *&start, std::size_t &size) {
        if (!blob || length - offset < 4)
            return false;
        size = (static_cast<std::size_t>(blob[offset]) << 24) |
               (static_cast<std::size_t>(blob[offset + 1]) << 16) |
               (static_cast<std::size_t>(blob[offset + 2]) << 8) |
               static_cast<std::size_t>(blob[offset + 3]);
        offset += 4;
        if (length - offset < size)
            return false;
        start = blob + offset;
        offset += size;
        return true;
    };
    const unsigned char *type = nullptr;
    const unsigned char *exponent = nullptr;
    const unsigned char *modulus = nullptr;
    std::size_t typeSize = 0;
    std::size_t exponentSize = 0;
    std::size_t modulusSize = 0;
    if (!nextField(type, typeSize) ||
        std::string_view(reinterpret_cast<const char *>(type), typeSize) !=
            "ssh-rsa" ||
        !nextField(exponent, exponentSize) || !nextField(modulus, modulusSize))
        return 0;
    // A positive mpint whose top bit is set starts with a zero byte.
    while (modulusSize > 0 && *modulus == 0) {
        ++modulus;
        --modulusSize;
    }
    if (modulusSize == 0)
        return 0;
    int bits = static_cast<int>((modulusSize - 1) * 8);
    for (unsigned top = *modulus; top != 0; top >>= 1)
        ++bits;
    return bits;
}

} // namespace openscp::libssh2detail
