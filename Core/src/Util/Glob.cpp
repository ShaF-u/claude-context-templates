#include "Core/Util/Glob.hpp"

#include <regex>

namespace aistudio::core {

namespace {

std::string GlobToRegexSource(const std::string& pattern) {
    std::string regex_source;
    regex_source.reserve(pattern.size() * 2);
    for (const char c : pattern) {
        switch (c) {
            case '*':
                regex_source += ".*";
                break;
            case '.':
            case '\\':
            case '^':
            case '$':
            case '+':
            case '?':
            case '(':
            case ')':
            case '[':
            case ']':
            case '{':
            case '}':
            case '|':
                regex_source += '\\';
                regex_source += c;
                break;
            default:
                regex_source += c;
        }
    }
    return regex_source;
}

} // namespace

bool GlobMatch(const std::string& text, const std::string& pattern) {
    try {
        const std::regex re("^" + GlobToRegexSource(pattern) + "$");
        return std::regex_match(text, re);
    } catch (const std::regex_error&) {
        return false;
    }
}

} // namespace aistudio::core
