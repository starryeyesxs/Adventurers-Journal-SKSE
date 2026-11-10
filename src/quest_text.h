#pragma once

#include <cctype>
#include <optional>
#include <string>
#include <string_view>

namespace quest_text
{
    inline bool EqualIgnoreCase(std::string_view left, std::string_view right)
    {
        if (left.size() != right.size()) {
            return false;
        }
        for (std::size_t i = 0; i < left.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(left[i])) !=
                std::tolower(static_cast<unsigned char>(right[i]))) {
                return false;
            }
        }
        return true;
    }

    template <class Resolver>
    std::string Resolve(std::string_view text, Resolver&& resolve)
    {
        std::string result;
        std::size_t cursor = 0;
        while (cursor < text.size()) {
            const auto start = text.find('<', cursor);
            if (start == std::string_view::npos) {
                result.append(text.substr(cursor));
                break;
            }
            result.append(text.substr(cursor, start - cursor));

            const auto nextTag = text.find('<', start + 1);
            auto end = text.find('>', start + 1);
            const auto malformed = text.substr(start, 13);
            bool malformedTerminator = false;
            if (EqualIgnoreCase(malformed, "<Global=MGR1)") &&
                start + 13 < text.size() && text[start + 13] == '1' &&
                (start + 14 == text.size() || !std::isalnum(static_cast<unsigned char>(text[start + 14])))) {
                end = start + 12;
                malformedTerminator = true;
            } else if (end == std::string_view::npos ||
                       (nextTag != std::string_view::npos && nextTag < end)) {
                if (EqualIgnoreCase(text.substr(start, 8), "<Global=")) {
                    end = start + 8;
                    while (end < text.size()) {
                        const auto character = static_cast<unsigned char>(text[end]);
                        if (std::isalnum(character) || character == '_' ||
                            (text[end] == ')' && end == start + 11 &&
                             end + 1 < text.size() && text[end + 1] == '1')) {
                            ++end;
                        } else {
                            break;
                        }
                    }
                } else {
                    end = nextTag == std::string_view::npos ? text.size() : nextTag;
                }
            }

            auto token = text.substr(start + 1, end - start - 1);
            const auto separator = token.find('=');
            const auto type = token.substr(0, separator);
            const auto name = separator == std::string_view::npos
                                  ? std::string_view{}
                                  : token.substr(separator + 1);
            const auto replacement = resolve(type, name);
            result += replacement.value_or("?");

            cursor = end;
            if (cursor < text.size() && (text[cursor] == '>' || malformedTerminator)) {
                ++cursor;
                if (EqualIgnoreCase(type, "Global") && EqualIgnoreCase(name, "MGR1") &&
                    cursor < text.size() && text[cursor] == '1') {
                    ++cursor;
                    if (malformedTerminator && cursor < text.size() && text[cursor] == '>') {
                        ++cursor;
                    }
                }
            }
        }
        return result;
    }
}
