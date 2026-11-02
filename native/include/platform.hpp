// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace wizard::platform {
void initialize_process();
std::filesystem::path default_datadir();
struct FileInfo { std::uint64_t device, inode, size; };
class CaptureFile {
    std::intptr_t handle_ = -1;
public:
    explicit CaptureFile(const std::filesystem::path& path);
    ~CaptureFile();
    CaptureFile(const CaptureFile&) = delete;
    CaptureFile& operator=(const CaptureFile&) = delete;
    bool valid() const { return handle_ != -1; }
    FileInfo info() const;
    std::string read_at(std::uint64_t offset, std::size_t count) const;
};
struct OutputClosed : std::runtime_error { OutputClosed() : std::runtime_error("Output closed") {} };
class Output {
    struct Impl;
    std::unique_ptr<Impl> impl_;
public:
    explicit Output(std::function<bool()> stopped);
    ~Output();
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    void write(const std::string& line);
};
} // namespace wizard::platform
