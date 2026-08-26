#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace datum {

/// Whole-file byte I/O. Goes through std::fstream with the path object, so wide
/// paths on Windows keep working — the C stdio calls inside stb would not.
/// Both throw std::runtime_error on failure.
std::vector<uint8_t> read_bytes(const std::filesystem::path& file);
void write_bytes(const std::filesystem::path& file, std::span<const uint8_t> bytes);

}  // namespace datum
