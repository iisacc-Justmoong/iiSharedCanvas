#pragma once
#include <iiFileProviderDatabase.h>
#include <array>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

struct LegacyWorkingRecord {
    int kind;
    std::string id;
    int position;
    std::string data, digest;
};

// The version-1 bytes are a separately installed SDK test resource, produced
// by the frozen 0.34.1 public writer. No build-directory fixture is required.
inline std::vector<LegacyWorkingRecord> loadLegacyWorkingRecords(const std::filesystem::path &file)
{
    std::ifstream input(file);
    if (!input) throw std::runtime_error("cannot read legacy working-file fixture");
    std::vector<LegacyWorkingRecord> records;
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::array<std::string, 5> fields;
        std::size_t start = 0;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            const auto end = line.find('\t', start);
            if ((i + 1 < fields.size() && end == std::string::npos) ||
                (i + 1 == fields.size() && end != std::string::npos))
                throw std::runtime_error("malformed legacy working-file fixture");
            fields[i] = line.substr(start, end == std::string::npos ? end : end - start);
            start = end == std::string::npos ? line.size() : end + 1;
        }
        const auto number = [](const std::string &text) {
            int value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
                throw std::runtime_error("malformed legacy fixture number");
            return value;
        };
        if ((fields[3].size() % 2) || fields[4].size() != 64)
            throw std::runtime_error("malformed legacy fixture hex");
        records.push_back({number(fields[0]), std::move(fields[1]), number(fields[2]),
                           std::move(fields[3]), std::move(fields[4])});
    }
    if (!input.eof() || records.empty()) throw std::runtime_error("empty or incomplete legacy fixture");
    return records;
}

inline bool writeLegacyWorkingFile(const std::filesystem::path &path,
                                   const std::filesystem::path &fixture)
{
    namespace sql = iiFileProvider::native::sql;
    auto database = sql::Database::open(path, {.mode = sql::OpenMode::create});
    if (!database) return false;
    for (const auto *statement : {
        "PRAGMA application_id=1229542211", "PRAGMA user_version=1",
        "CREATE TABLE canvas_state (singleton INTEGER PRIMARY KEY CHECK(singleton=1), revision INTEGER NOT NULL CHECK(revision>=0))",
        "CREATE TABLE canvas_records (kind INTEGER NOT NULL, id TEXT NOT NULL, position INTEGER NOT NULL CHECK(position>=0), data BLOB NOT NULL, digest BLOB NOT NULL, UNIQUE(kind,id))",
        "INSERT INTO canvas_state VALUES(1,0)"})
        if (!database->execute(statement)) return false;
    const auto unhex = [](std::string_view text) {
        const auto digit = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            throw std::runtime_error("malformed legacy fixture hex digit");
        };
        sql::Blob bytes;
        bytes.reserve(text.size() / 2);
        for (std::size_t i = 0; i < text.size(); i += 2)
            bytes.push_back(std::uint8_t((digit(text[i]) << 4) | digit(text[i + 1])));
        return bytes;
    };
    for (const auto &record : loadLegacyWorkingRecords(fixture)) {
        auto insert = database->prepare("INSERT INTO canvas_records VALUES(?,?,?,?,?)");
        if (!insert || !insert->bind(1, std::int64_t(record.kind)) || !insert->bind(2, record.id) ||
            !insert->bind(3, std::int64_t(record.position)) || !insert->bind(4, unhex(record.data)) ||
            !insert->bind(5, unhex(record.digest)) || !insert->step()) return false;
    }
    return true;
}
