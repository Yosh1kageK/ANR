#include "renderer/clips/process.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <mutex>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#include <mach-o/dyld.h>
#define NR_ENVIRON (*_NSGetEnviron())
#else
extern char** environ;
#define NR_ENVIRON environ
#endif
#endif

namespace renderer::clips {

#ifdef _WIN32

namespace {

std::wstring Wide(const std::string& s) {
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// One argument as the C runtime's command-line parsing reads it back.
std::wstring Quote(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos)
        return arg;
    std::wstring out = L"\"";
    for (size_t i = 0;; i++) {
        size_t slashes = 0;
        while (i < arg.size() && arg[i] == L'\\') {
            slashes++;
            i++;
        }
        if (i == arg.size()) {
            out.append(slashes * 2, L'\\');
            break;
        }
        if (arg[i] == L'"')
            out.append(slashes * 2 + 1, L'\\');
        else
            out.append(slashes, L'\\');
        out.push_back(arg[i]);
    }
    out.push_back(L'"');
    return out;
}

}  // namespace

struct Process::Impl {
    std::mutex mu;  // the process handle: Terminate may come from another thread
    HANDLE process = nullptr, out_read = nullptr, in_write = nullptr;
    bool exited = false;
    int code = -1;
};

Process::Process() : impl_(new Impl) {}
Process::~Process() {
    Kill();
}
bool Process::started() const { return impl_->process != nullptr; }

bool Process::Start(const std::vector<std::string>& args, const ProcessOptions& options, std::string* error) {
    Impl& p = *impl_;
    SECURITY_ATTRIBUTES inherit = {sizeof(inherit), nullptr, TRUE};
    HANDLE out_write = nullptr, in_read = nullptr, err_file = nullptr, null_out = nullptr, null_in = nullptr;
    auto fail = [&](const char* what) {
        if (error)
            *error = std::string(what) + " (error " + std::to_string(GetLastError()) + ")";
        for (HANDLE h : {out_write, in_read, err_file, null_out, null_in})
            if (h && h != INVALID_HANDLE_VALUE)
                CloseHandle(h);
        if (p.out_read) CloseHandle(p.out_read);
        if (p.in_write) CloseHandle(p.in_write);
        p.out_read = p.in_write = nullptr;
        return false;
    };
    if (options.pipe_stdout) {
        if (!CreatePipe(&p.out_read, &out_write, &inherit, (DWORD)options.stdout_pipe_bytes))
            return fail("CreatePipe");
        SetHandleInformation(p.out_read, HANDLE_FLAG_INHERIT, 0);
    } else {
        null_out = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
        out_write = null_out;
    }
    if (options.pipe_stdin) {
        if (!CreatePipe(&in_read, &p.in_write, &inherit, 1 << 20))
            return fail("CreatePipe");
        SetHandleInformation(p.in_write, HANDLE_FLAG_INHERIT, 0);
    } else {
        null_in = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ, &inherit, OPEN_EXISTING, 0, nullptr);
        in_read = null_in;
    }
    HANDLE err_handle = nullptr;
    if (options.stderr_path.empty()) {  // our stderr as an inheritable copy, else NUL (no console: a GUI or a task)
        HANDLE own = GetStdHandle(STD_ERROR_HANDLE);
        if (!own || own == INVALID_HANDLE_VALUE ||
            !DuplicateHandle(GetCurrentProcess(), own, GetCurrentProcess(), &err_file, 0, TRUE, DUPLICATE_SAME_ACCESS))
            err_file = CreateFileW(L"NUL", GENERIC_WRITE, FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr);
        err_handle = err_file;
    } else {
        err_file = CreateFileW(Wide(options.stderr_path).c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                               FILE_ATTRIBUTE_NORMAL, nullptr);
        if (err_file == INVALID_HANDLE_VALUE)
            return fail("cannot open the stderr file");
        err_handle = err_file;
    }
    std::wstring command;
    for (size_t i = 0; i < args.size(); i++)
        command += (i ? L" " : L"") + Quote(Wide(args[i]));
    // The child inherits its three standard handles and nothing else. With every inheritable handle (bInheritHandles
    // alone), a child started while another's pipes were open kept their write ends: the other's reader then never saw
    // the end of its pipe (a probe's output read to its end waited forever while a decoder ran).
    HANDLE handles[3];
    DWORD count = 0;
    for (HANDLE h : {in_read, out_write, err_handle})
        if (h && h != INVALID_HANDLE_VALUE && std::find(handles, handles + count, h) == handles + count)
            handles[count++] = h;
    SIZE_T list_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &list_bytes);
    std::vector<char> list_memory(list_bytes);
    auto list = (LPPROC_THREAD_ATTRIBUTE_LIST)list_memory.data();
    if (!InitializeProcThreadAttributeList(list, 1, 0, &list_bytes) ||
        !UpdateProcThreadAttribute(list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles, count * sizeof(HANDLE), nullptr,
                                   nullptr))
        return fail("the handle list");
    STARTUPINFOEXW si = {};
    si.StartupInfo.cb = sizeof(si);
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = in_read;
    si.StartupInfo.hStdOutput = out_write;
    si.StartupInfo.hStdError = err_handle;
    si.lpAttributeList = list;
    PROCESS_INFORMATION pi = {};
    const BOOL created = CreateProcessW(nullptr, command.data(), nullptr, nullptr, TRUE,
                                        CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr, nullptr,
                                        &si.StartupInfo, &pi);
    DeleteProcThreadAttributeList(list);
    if (!created)
        return fail("CreateProcess");
    CloseHandle(pi.hThread);
    {
        std::lock_guard<std::mutex> lock(p.mu);
        p.process = pi.hProcess;
        p.exited = false;
    }
    for (HANDLE h : {out_write, in_read, err_file})
        if (h && h != INVALID_HANDLE_VALUE)
            CloseHandle(h);
    return true;
}

size_t Process::Read(void* data, size_t bytes) {
    if (!impl_->out_read)
        return 0;
    DWORD got = 0;
    if (!ReadFile(impl_->out_read, data, (DWORD)std::min<size_t>(bytes, 1u << 24), &got, nullptr))
        return 0;
    return got;
}

bool Process::Write(const void* data, size_t bytes) {
    if (!impl_->in_write)
        return false;
    const char* p = (const char*)data;
    while (bytes > 0) {
        DWORD put = 0;
        if (!WriteFile(impl_->in_write, p, (DWORD)std::min<size_t>(bytes, 1u << 24), &put, nullptr) || put == 0)
            return false;
        p += put;
        bytes -= put;
    }
    return true;
}

void Process::CloseStdin() {
    if (impl_->in_write) {
        CloseHandle(impl_->in_write);
        impl_->in_write = nullptr;
    }
}

void Process::CloseStdout() {
    if (impl_->out_read) {
        CloseHandle(impl_->out_read);
        impl_->out_read = nullptr;
    }
}

void Process::Terminate() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->process && !impl_->exited)
        TerminateProcess(impl_->process, 1);
}

void Process::Kill() {
    Impl& p = *impl_;
    {
        std::lock_guard<std::mutex> lock(p.mu);
        if (p.process) {
            if (!p.exited)
                TerminateProcess(p.process, 1);
            WaitForSingleObject(p.process, 5000);
            CloseHandle(p.process);
            p.process = nullptr;
        }
    }
    CloseStdout();
    CloseStdin();
}

int Process::Wait() {
    Impl& p = *impl_;
    std::lock_guard<std::mutex> lock(p.mu);
    if (!p.process)
        return p.code;
    CloseStdin();
    WaitForSingleObject(p.process, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(p.process, &code);
    p.exited = true;
    p.code = (int)code;
    CloseHandle(p.process);
    p.process = nullptr;
    return p.code;
}

#else  // POSIX

struct Process::Impl {
    std::mutex mu;  // pid: Terminate may come from another thread
    pid_t pid = -1;
    int out_read = -1, in_write = -1;
    int code = -1;
};

Process::Process() : impl_(new Impl) {}
Process::~Process() {
    Kill();
}
bool Process::started() const { return impl_->pid > 0; }

namespace {
void CloseFd(int& fd) {
    if (fd >= 0) {
        close(fd);
        fd = -1;
    }
}

bool MakePipe(int fds[2]) {
    if (pipe(fds) != 0)
        return false;
    for (int i = 0; i < 2; i++)
        fcntl(fds[i], F_SETFD, FD_CLOEXEC);
    return true;
}
}  // namespace

bool Process::Start(const std::vector<std::string>& args, const ProcessOptions& options, std::string* error) {
    Impl& p = *impl_;
    static const bool ignore_sigpipe = [] {
        signal(SIGPIPE, SIG_IGN);  // a dead child's pipe fails the write instead of ending the program
        return true;
    }();
    (void)ignore_sigpipe;
    int out[2] = {-1, -1}, in[2] = {-1, -1};
    auto fail = [&](const char* what, int err) {
        if (error)
            *error = std::string(what) + ": " + std::strerror(err);
        for (int fd : {out[0], out[1], in[0], in[1]})
            if (fd >= 0)
                close(fd);
        return false;
    };
    if (options.pipe_stdout && !MakePipe(out))
        return fail("pipe", errno);
    if (options.pipe_stdin && !MakePipe(in))
        return fail("pipe", errno);
#ifdef F_SETPIPE_SZ
    if (options.pipe_stdout && options.stdout_pipe_bytes > 0)
        fcntl(out[0], F_SETPIPE_SZ, options.stdout_pipe_bytes);  // best effort: the system's maximum caps it
#endif
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (options.pipe_stdout)
        posix_spawn_file_actions_adddup2(&actions, out[1], STDOUT_FILENO);
    else
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    if (options.pipe_stdin)
        posix_spawn_file_actions_adddup2(&actions, in[0], STDIN_FILENO);
    else
        posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    if (!options.stderr_path.empty())
        posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, options.stderr_path.c_str(),
                                         O_WRONLY | O_CREAT | O_TRUNC, 0644);
    std::vector<char*> argv;
    for (const std::string& a : args)
        argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = -1;
    const int result = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), NR_ENVIRON);
    posix_spawn_file_actions_destroy(&actions);
    if (result != 0)
        return fail(("cannot start " + args[0]).c_str(), result);
    {
        std::lock_guard<std::mutex> lock(p.mu);
        p.pid = pid;
    }
    p.out_read = out[0];
    p.in_write = in[1];
    CloseFd(out[1]);
    CloseFd(in[0]);
    return true;
}

size_t Process::Read(void* data, size_t bytes) {
    if (impl_->out_read < 0)
        return 0;
    for (;;) {
        const ssize_t n = read(impl_->out_read, data, bytes);
        if (n >= 0)
            return (size_t)n;
        if (errno != EINTR)
            return 0;
    }
}

bool Process::Write(const void* data, size_t bytes) {
    if (impl_->in_write < 0)
        return false;
    const char* p = (const char*)data;
    while (bytes > 0) {
        const ssize_t n = write(impl_->in_write, p, bytes);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        p += n;
        bytes -= (size_t)n;
    }
    return true;
}

void Process::CloseStdin() { CloseFd(impl_->in_write); }
void Process::CloseStdout() { CloseFd(impl_->out_read); }

void Process::Terminate() {
    std::lock_guard<std::mutex> lock(impl_->mu);
    if (impl_->pid > 0)
        kill(impl_->pid, SIGKILL);
}

void Process::Kill() {
    Impl& p = *impl_;
    {
        std::lock_guard<std::mutex> lock(p.mu);
        if (p.pid > 0) {
            kill(p.pid, SIGKILL);  // before the pipes close: a closed pipe makes ffmpeg print a broken pipe error first
            int status = 0;
            while (waitpid(p.pid, &status, 0) < 0 && errno == EINTR) {
            }
            p.pid = -1;
        }
    }
    CloseFd(p.out_read);
    CloseFd(p.in_write);
}

int Process::Wait() {
    Impl& p = *impl_;
    std::lock_guard<std::mutex> lock(p.mu);
    if (p.pid <= 0)
        return p.code;
    CloseFd(p.in_write);
    int status = 0;
    while (waitpid(p.pid, &status, 0) < 0 && errno == EINTR) {
    }
    p.pid = -1;
    p.code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return p.code;
}

#endif

bool Process::ReadFull(void* data, size_t bytes) {
    char* p = (char*)data;
    while (bytes > 0) {
        const size_t n = Read(p, bytes);
        if (n == 0)
            return false;
        p += n;
        bytes -= n;
    }
    return true;
}

namespace {

bool IsFile(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

}  // namespace

std::string ExecutablePath() {
#ifdef _WIN32
    wchar_t buffer[4096];
    const DWORD n = GetModuleFileNameW(nullptr, buffer, 4096);
    return n > 0 ? std::filesystem::path(std::wstring(buffer, n)).string() : std::string();
#elif defined(__linux__)
    std::error_code ec;
    return std::filesystem::read_symlink("/proc/self/exe", ec).string();
#elif defined(__APPLE__)
    char buffer[4096];
    uint32_t size = sizeof(buffer);
    return _NSGetExecutablePath(buffer, &size) == 0 ? std::filesystem::path(buffer).lexically_normal().string() : std::string();
#else
    return {};
#endif
}

std::string FindTool(const std::string& name, const std::string& environment_variable) {
    if (const char* value = std::getenv(environment_variable.c_str()); value && *value)
        return value;
#ifdef _WIN32
    const std::string file = name + ".exe";
#else
    const std::string file = name;
#endif
    const std::string exe = ExecutablePath();
    if (!exe.empty()) {
        const std::filesystem::path beside = std::filesystem::path(exe).parent_path() / file;
        if (IsFile(beside))
            return beside.string();
    }
    return name;
}

std::string RunCapture(const std::vector<std::string>& args, bool* ok, size_t limit) {
    Process process;
    ProcessOptions options;
    options.pipe_stdout = true;
    std::string out;
    if (!process.Start(args, options)) {
        if (ok)
            *ok = false;
        return out;
    }
    char buffer[65536];
    for (size_t n; out.size() < limit && (n = process.Read(buffer, sizeof(buffer))) > 0;)
        out.append(buffer, n);
    process.CloseStdout();
    if (ok)
        *ok = process.Wait() == 0;
    return out;
}

}  // namespace renderer::clips
