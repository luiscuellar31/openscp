#pragma once

#include "openscp/KeyboardInteractive.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace openscp::libssh2detail {

inline constexpr int kMaxKeyboardInteractivePrompts = 32;
inline constexpr std::size_t kMaxKeyboardInteractivePromptBytes = 16 * 1024;
inline constexpr std::size_t kMaxKeyboardInteractiveTotalPromptBytes =
    64 * 1024;
inline constexpr std::size_t kMaxKeyboardInteractiveAnswerBytes = 64 * 1024;

struct KeyboardInteractivePromptView {
    const unsigned char *text = nullptr;
    std::size_t length = 0;
};

inline bool validateEndpointHost(std::string_view host, const char *fieldLabel,
                                 std::string &error) {
    const std::string label =
        (fieldLabel && *fieldLabel) ? fieldLabel : "SSH host";
    if (host.empty()) {
        error = label + " is required.";
        return false;
    }
    const auto forbiddenHostCharacter = [](const unsigned char ch) {
        return ch < 0x20 || ch == 0x7f || std::isspace(ch) != 0 || ch == '/' ||
               ch == '\\' || ch == '@' || ch == '?' || ch == '#';
    };
    if (std::any_of(host.begin(), host.end(), forbiddenHostCharacter)) {
        error = label + " contains a forbidden host character.";
        return false;
    }

    const bool hasOpeningBracket = host.find('[') != std::string_view::npos;
    const bool hasClosingBracket = host.find(']') != std::string_view::npos;
    if (hasOpeningBracket || hasClosingBracket) {
        if (host.size() < 4 || host.front() != '[' || host.back() != ']' ||
            host.find('[', 1) != std::string_view::npos ||
            host.find(']') != host.size() - 1) {
            error = label + " contains malformed IPv6 brackets.";
            return false;
        }
    } else if (std::count(host.begin(), host.end(), ':') == 1) {
        error = label + " must not include a port; use the port field.";
        return false;
    }
    return true;
}

inline bool
copyKeyboardInteractivePrompts(const KeyboardInteractivePromptView *prompts,
                               int promptCount, std::vector<std::string> &out,
                               std::string &error) {
    out.clear();
    if (promptCount < 0 || promptCount > kMaxKeyboardInteractivePrompts) {
        error = "Keyboard-interactive prompt count exceeds the safety limit.";
        return false;
    }
    if (promptCount > 0 && !prompts) {
        error = "Keyboard-interactive prompt data is missing.";
        return false;
    }

    std::size_t totalBytes = 0;
    for (int i = 0; i < promptCount; ++i) {
        const std::size_t length = prompts[i].length;
        if (length > kMaxKeyboardInteractivePromptBytes ||
            totalBytes > kMaxKeyboardInteractiveTotalPromptBytes - length) {
            error =
                "Keyboard-interactive prompt data exceeds the safety limit.";
            return false;
        }
        if (length > 0 && !prompts[i].text) {
            error = "Keyboard-interactive prompt text is missing.";
            return false;
        }
        totalBytes += length;
    }
    if (totalBytes > kMaxKeyboardInteractiveTotalPromptBytes) {
        error = "Keyboard-interactive prompt data exceeds the safety limit.";
        return false;
    }

    try {
        out.reserve(static_cast<std::size_t>(promptCount));
        for (int i = 0; i < promptCount; ++i) {
            const char *text = reinterpret_cast<const char *>(prompts[i].text);
            out.emplace_back(text ? text : "", prompts[i].length);
        }
    } catch (...) {
        out.clear();
        error = "Could not allocate keyboard-interactive prompt data.";
        return false;
    }
    return true;
}

enum class KbdIntSavedAnswer { None, Username, Password };

// Which saved credential answers a prompt: the account name, or the account
// password in the first round only. A password prompt in a later round
// usually means the saved one was refused.
inline KbdIntSavedAnswer savedKbdIntAnswer(const KbdIntPrompt &prompt,
                                           int round) {
    switch (classifyKbdIntPrompt(prompt)) {
    case KbdIntPromptKind::Username:
        return KbdIntSavedAnswer::Username;
    case KbdIntPromptKind::Password:
        return round == 1 ? KbdIntSavedAnswer::Password
                          : KbdIntSavedAnswer::None;
    case KbdIntPromptKind::OneTimeCode:
    case KbdIntPromptKind::Other:
        break;
    }
    return KbdIntSavedAnswer::None;
}

} // namespace openscp::libssh2detail
