#pragma once

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#elif defined(__linux__) && defined(__aarch64__)
#include <sys/auxv.h>
#endif

namespace openscp::libssh2detail {

// AES-GCM runs through the crypto backend, which uses the CPU's AES and
// carry-less multiplication instructions when present, while libssh2
// implements chacha20-poly1305 in portable C. With those instructions AES-GCM
// is the cheaper cipher; without them software AES-GCM costs about twice the
// CPU of chacha20-poly1305. Unknown CPUs are treated as lacking them.
inline bool cpuHasAesGcmInstructions() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int eax = 0;
    unsigned int ebx = 0;
    unsigned int ecx = 0;
    unsigned int edx = 0;
    if (__get_cpuid(1, &eax, &ebx, &ecx, &edx) == 0)
        return false;
    constexpr unsigned int kPclmulqdq = 1U << 1;
    constexpr unsigned int kAesNi = 1U << 25;
    return (ecx & kAesNi) != 0 && (ecx & kPclmulqdq) != 0;
#elif defined(__APPLE__) && defined(__aarch64__)
    // Every Apple silicon CPU has the ARMv8 cryptography extensions.
    return true;
#elif defined(__linux__) && defined(__aarch64__)
    // Bit positions from the Linux arm64 ABI (asm/hwcap.h).
    constexpr unsigned long kHwcapAes = 1UL << 3;
    constexpr unsigned long kHwcapPmull = 1UL << 4;
    const unsigned long hwcap = getauxval(AT_HWCAP);
    return (hwcap & kHwcapAes) != 0 && (hwcap & kHwcapPmull) != 0;
#else
    return false;
#endif
}

// Both orders offer the same ciphers; only the preferred AEAD changes.
inline const char *sshCipherPreference(bool aesGcmInstructions) {
    return aesGcmInstructions
               ? "aes256-gcm@openssh.com,aes128-gcm@openssh.com,"
                 "chacha20-poly1305@openssh.com,aes256-ctr,aes128-ctr"
               : "chacha20-poly1305@openssh.com,aes256-gcm@openssh.com,"
                 "aes128-gcm@openssh.com,aes256-ctr,aes128-ctr";
}

} // namespace openscp::libssh2detail
