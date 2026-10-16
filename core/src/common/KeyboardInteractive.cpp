#include "openscp/KeyboardInteractive.hpp"

#include <algorithm>
#include <cstddef>
#include <initializer_list>

namespace openscp {
namespace {

// The prompt's ASCII words in lower case; anything else separates them.
std::vector<std::string> promptWords(std::string_view text) {
    std::vector<std::string> words;
    std::string word;
    for (const char character : text) {
        const unsigned char ch = static_cast<unsigned char>(character);
        if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9')) {
            word.push_back(character);
        } else if (ch >= 'A' && ch <= 'Z') {
            word.push_back(static_cast<char>(ch - 'A' + 'a'));
        } else if (!word.empty()) {
            words.push_back(std::move(word));
            word.clear();
        }
    }
    if (!word.empty())
        words.push_back(std::move(word));
    return words;
}

bool hasAnyWord(const std::vector<std::string> &words,
                std::initializer_list<std::string_view> candidates) {
    return std::any_of(
        words.begin(), words.end(), [&](const std::string &word) {
            return std::find(candidates.begin(), candidates.end(), word) !=
                   candidates.end();
        });
}

bool hasWordPair(const std::vector<std::string> &words, std::string_view first,
                 std::string_view second) {
    for (std::size_t index = 0; index + 1 < words.size(); ++index) {
        if (words[index] == first && words[index + 1] == second)
            return true;
    }
    return false;
}

} // namespace

KbdIntPromptKind classifyKbdIntPrompt(const KbdIntPrompt &prompt) {
    const std::vector<std::string> words = promptWords(prompt.text);
    if (hasAnyWord(words, {"code", "passcode", "token", "otp", "totp",
                           "verification"}) ||
        hasWordPair(words, "one", "time")) {
        return KbdIntPromptKind::OneTimeCode;
    }
    if (!prompt.echo && hasAnyWord(words, {"password", "passphrase"}))
        return KbdIntPromptKind::Password;
    if (prompt.echo && hasAnyWord(words, {"username", "user", "login"}))
        return KbdIntPromptKind::Username;
    return KbdIntPromptKind::Other;
}

} // namespace openscp
