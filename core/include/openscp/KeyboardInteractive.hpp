// Keyboard-interactive prompts and the one classifier that decides what each
// asks for.
#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace openscp {

enum class KbdIntPromptResult { Handled, Unhandled, Cancelled };

// A prompt and whether the server lets the answer show while it is typed.
struct KbdIntPrompt {
    std::string text;
    bool echo = false;
};

enum class KbdIntPromptKind { Username, Password, OneTimeCode, Other };

// Words are matched whole and ignoring case. A code word ("code", "passcode",
// "token", "OTP", "one-time", "verification") makes a one-time code, even
// next to "password". A hidden prompt with "password" or "passphrase" asks
// for the account password, and a visible one with "username", "user" or
// "login" for the account name. Anything else is Other: a hidden one is a
// secret, and only the user can answer it.
KbdIntPromptKind classifyKbdIntPrompt(const KbdIntPrompt &prompt);

// Answers the prompts that the saved credentials cannot: all but the account
// name, and the account password in the first round. `responses` gets one
// answer per prompt, in order.
using KbdIntPromptsCB = std::function<KbdIntPromptResult(
    const std::string &name, const std::string &instruction,
    const std::vector<KbdIntPrompt> &prompts,
    std::vector<std::string> &responses)>;

} // namespace openscp
