#pragma once

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

inline bool promptRequestsUsername(std::string_view prompt) {
    auto containsAsciiCaseInsensitive =
        [prompt](std::string_view needle) -> bool {
        if (needle.empty() || needle.size() > prompt.size())
            return false;
        for (std::size_t offset = 0; offset <= prompt.size() - needle.size();
             ++offset) {
            bool matches = true;
            for (std::size_t i = 0; i < needle.size(); ++i) {
                unsigned char ch =
                    static_cast<unsigned char>(prompt[offset + i]);
                if (ch >= 'A' && ch <= 'Z')
                    ch = static_cast<unsigned char>(ch - 'A' + 'a');
                if (ch != static_cast<unsigned char>(needle[i])) {
                    matches = false;
                    break;
                }
            }
            if (matches)
                return true;
        }
        return false;
    };
    return containsAsciiCaseInsensitive("user") ||
           containsAsciiCaseInsensitive("name");
}

} // namespace openscp::libssh2detail
