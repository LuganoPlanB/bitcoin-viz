// SPDX-License-Identifier: GPL-3.0-or-later
#include "wizard.hpp"
#include "platform.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <limits>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace wizard {
WriterLock::WriterLock(const std::filesystem::path& database) {
    if (database.has_parent_path()) std::filesystem::create_directories(database.parent_path());
    auto path=database; path+=".writer.lock";
#ifdef _WIN32
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open observer writer lock");
    OVERLAPPED overlap{};
    if (!LockFileEx(file,LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&overlap)) {
        CloseHandle(file); throw std::runtime_error("An observer/ingester already owns this database");
    }
    handle_=reinterpret_cast<std::intptr_t>(file);
#else
    const int file=::open(path.c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600);
    if (file<0) throw std::runtime_error("Cannot open observer writer lock");
    if (::flock(file,LOCK_EX|LOCK_NB)) { ::close(file); throw std::runtime_error("An observer/ingester already owns this database"); }
    handle_=file;
#endif
}
WriterLock::~WriterLock() {
    if (handle_==-1) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    ::close(static_cast<int>(handle_));
#endif
}
} // namespace wizard

namespace wizard::platform {
void initialize_process() {
#ifdef _WIN32
    _umask(0); // Windows access control is inherited from the containing directory.
    _setmode(_fileno(stdout),_O_BINARY);
#else
    ::umask(0077);
#endif
}
std::filesystem::path default_datadir() {
#ifdef _WIN32
    if (const auto* local=_wgetenv(L"LOCALAPPDATA")) return std::filesystem::path(local)/L"Bitcoin";
    if (const auto* roaming=_wgetenv(L"APPDATA")) return std::filesystem::path(roaming)/L"Bitcoin";
    return "Bitcoin";
#elif defined(__APPLE__)
    if (const char* home=std::getenv("HOME")) return std::filesystem::path(home)/"Library/Application Support/Bitcoin";
    return "Bitcoin";
#else
    if (const char* home=std::getenv("HOME")) return std::filesystem::path(home)/".bitcoin";
    return ".bitcoin";
#endif
}
CaptureFile::CaptureFile(const std::filesystem::path& path) {
#ifdef _WIN32
    HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if (file==INVALID_HANDLE_VALUE) {
        const auto error=GetLastError();
        if (error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND) return;
        throw std::runtime_error("Cannot open capture file");
    }
    handle_=reinterpret_cast<std::intptr_t>(file);
#else
    handle_=::open(path.c_str(),O_RDONLY|O_CLOEXEC);
    if (handle_<0 && errno!=ENOENT) throw std::runtime_error("Cannot open capture file");
#endif
}
CaptureFile::~CaptureFile() {
    if (!valid()) return;
#ifdef _WIN32
    CloseHandle(reinterpret_cast<HANDLE>(handle_));
#else
    ::close(static_cast<int>(handle_));
#endif
}
FileInfo CaptureFile::info() const {
#ifdef _WIN32
    BY_HANDLE_FILE_INFORMATION data{};
    if (!GetFileInformationByHandle(reinterpret_cast<HANDLE>(handle_),&data)) throw std::runtime_error("Cannot stat capture file");
    return {data.dwVolumeSerialNumber,(std::uint64_t(data.nFileIndexHigh)<<32U)|data.nFileIndexLow,
            (std::uint64_t(data.nFileSizeHigh)<<32U)|data.nFileSizeLow};
#else
    struct stat data{};
    if (::fstat(static_cast<int>(handle_),&data) || data.st_size<0) throw std::runtime_error("Cannot stat capture file");
    return {std::uint64_t(data.st_dev),std::uint64_t(data.st_ino),std::uint64_t(data.st_size)};
#endif
}
std::string CaptureFile::read_at(std::uint64_t offset,std::size_t count) const {
    if (offset>std::uint64_t(std::numeric_limits<std::int64_t>::max()) || count>4000000) throw std::runtime_error("Capture read exceeds resource limit");
    std::string bytes(count,'\0');
    std::size_t done=0;
#ifdef _WIN32
    LARGE_INTEGER position{}; position.QuadPart=static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(reinterpret_cast<HANDLE>(handle_),position,nullptr,FILE_BEGIN)) throw std::runtime_error("Capture seek failed");
#endif
    while (done<count) {
#ifdef _WIN32
        DWORD n=0;
        if (!ReadFile(reinterpret_cast<HANDLE>(handle_),bytes.data()+done,static_cast<DWORD>(count-done),&n,nullptr)) throw std::runtime_error("Capture read failed");
#else
        if (offset>std::uint64_t(std::numeric_limits<off_t>::max())-done) throw std::runtime_error("Capture offset overflow");
        const auto n=::pread(static_cast<int>(handle_),bytes.data()+done,count-done,static_cast<off_t>(offset+done));
        if (n<0) { if (errno==EINTR) continue; throw std::runtime_error("Capture read failed"); }
#endif
        if (n==0) break;
        done+=static_cast<std::size_t>(n);
    }
    bytes.resize(done); return bytes;
}

struct Output::Impl {
    std::function<bool()> stopped;
#ifdef _WIN32
    HANDLE output=INVALID_HANDLE_VALUE,writer=nullptr;
    std::atomic<bool> done{false};
    std::mutex mutex;
    std::thread cancel;
    explicit Impl(std::function<bool()> stop) : stopped(std::move(stop)) {
        output=GetStdHandle(STD_OUTPUT_HANDLE);
        if (!output || output==INVALID_HANDLE_VALUE) throw std::runtime_error("Cannot open Gource output");
        cancel=std::thread([this] {
            while (!done.load()) {
                if (stopped()) { std::lock_guard<std::mutex> lock(mutex); if (writer) CancelSynchronousIo(writer); }
                std::this_thread::sleep_for(std::chrono::milliseconds(25));
            }
        });
    }
    ~Impl() {
        done.store(true); cancel.join();
        if (writer) CloseHandle(writer);
    }
#else
    int old_flags;
    explicit Impl(std::function<bool()> stop) : stopped(std::move(stop)),old_flags(::fcntl(STDOUT_FILENO,F_GETFL)) {
        if (old_flags<0 || ::fcntl(STDOUT_FILENO,F_SETFL,old_flags|O_NONBLOCK)) throw std::runtime_error("Cannot configure Gource output");
    }
    ~Impl() { ::fcntl(STDOUT_FILENO,F_SETFL,old_flags); }
#endif
};
Output::Output(std::function<bool()> stopped) : impl_(std::make_unique<Impl>(std::move(stopped))) {}
Output::~Output()=default;
void Output::write(const std::string& line) {
    auto& p=*impl_;
#ifdef _WIN32
    {
        std::lock_guard<std::mutex> lock(p.mutex);
        if (!p.writer && !DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&p.writer,0,FALSE,DUPLICATE_SAME_ACCESS))
            throw std::runtime_error("Cannot create cancellable output handle");
    }
#endif
    std::size_t offset=0;
    while (offset<line.size()) {
        if (p.stopped()) throw OutputClosed();
#ifdef _WIN32
        DWORD n=0;
        const auto length=std::min<std::size_t>(line.size()-offset,65536);
        if (!WriteFile(p.output,line.data()+offset,static_cast<DWORD>(length),&n,nullptr)) {
            const auto error=GetLastError();
            if (error==ERROR_BROKEN_PIPE || (error==ERROR_OPERATION_ABORTED && p.stopped())) throw OutputClosed();
            throw std::runtime_error("Gource output write failed");
        }
#else
        const auto n=::write(STDOUT_FILENO,line.data()+offset,line.size()-offset);
        if (n<0 && errno==EINTR) continue;
        if (n<0 && (errno==EAGAIN || errno==EWOULDBLOCK)) {
            pollfd fd{STDOUT_FILENO,POLLOUT,0};
            if (::poll(&fd,1,100)<0 && errno!=EINTR) throw std::runtime_error("Gource output poll failed");
            continue;
        }
        if (n<0 && errno==EPIPE) throw OutputClosed();
        if (n<0) throw std::runtime_error("Gource output write failed");
#endif
        if (n==0) throw std::runtime_error("Gource output made no progress");
        offset+=static_cast<std::size_t>(n);
    }
}
} // namespace wizard::platform
