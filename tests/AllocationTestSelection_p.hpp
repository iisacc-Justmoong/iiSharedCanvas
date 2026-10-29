#pragma once

#include <algorithm>
#include <cstddef>
#include <optional>
#include <span>
#include <string_view>

namespace allocation_test {
struct Selection {
    bool memory = true;
    bool file = true;
    std::optional<std::size_t> operation;
};

inline std::optional<Selection> parse(int argc, char **argv, std::span<const std::string_view> names)
{
    Selection result;
    bool modeSelected = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "--memory-only" || argument == "--file-only") {
            if (modeSelected) return {};
            modeSelected = true;
            result.memory = argument == "--memory-only";
            result.file = argument == "--file-only";
        } else if (argument.starts_with("--operation=")) {
            if (result.operation) return {};
            const auto name = argument.substr(std::string_view("--operation=").size());
            const auto found = std::find(names.begin(), names.end(), name);
            if (found == names.end()) return {};
            result.operation = static_cast<std::size_t>(found - names.begin());
        } else return {};
    }
    return result;
}
} // namespace allocation_test
