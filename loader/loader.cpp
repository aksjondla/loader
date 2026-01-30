#include <windows.h>
#include <tlhelp32.h>
#include <iostream>
#include <fstream>
#include <string>
#include <cctype>

struct LoaderConfig {
    std::string dll_path = ".\\UniversalHookX-Coop.dll";
    std::string game_path = ".\\Game.exe";
    std::string process_name = "Game.exe";
    bool show_console = false;
};

static std::string ReadTextFile(const std::string& path) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) {
        return {};
    }
    std::string contents;
    in.seekg(0, std::ios::end);
    contents.resize(static_cast<size_t>(in.tellg()));
    in.seekg(0, std::ios::beg);
    in.read(&contents[0], contents.size());
    return contents;
}

static std::string GetExeDir() {
    char path[MAX_PATH] = {};
    GetModuleFileNameA(NULL, path, MAX_PATH);
    std::string full(path);
    size_t pos = full.find_last_of("\\/");
    if (pos == std::string::npos) {
        return ".";
    }
    return full.substr(0, pos);
}

static bool IsAbsolutePath(const std::string& path) {
    if (path.size() >= 2 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':') {
        return true;
    }
    if (path.size() >= 2 && path[0] == '\\' && path[1] == '\\') {
        return true;
    }
    return false;
}

static std::string JoinPath(const std::string& base, const std::string& rel) {
    if (base.empty()) {
        return rel;
    }
    if (rel.empty()) {
        return base;
    }
    std::string out = base;
    if (out.back() != '\\' && out.back() != '/') {
        out.push_back('\\');
    }
    if (rel.rfind(".\\", 0) == 0) {
        out += rel.substr(2);
    } else if (rel.rfind("./", 0) == 0) {
        out += rel.substr(2);
    } else {
        out += rel;
    }
    return out;
}

static std::string ResolvePath(const std::string& baseDir, const std::string& path) {
    if (IsAbsolutePath(path)) {
        return path;
    }
    return JoinPath(baseDir, path);
}

static bool FindJsonString(const std::string& json, const char* key, std::string& out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return false;
    }
    pos++;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if (pos >= json.size() || json[pos] != '"') {
        return false;
    }
    ++pos;
    std::string value;
    while (pos < json.size()) {
        char c = json[pos++];
        if (c == '\\' && pos < json.size()) {
            char esc = json[pos++];
            switch (esc) {
                case '"':
                case '\\':
                case '/':
                    value.push_back(esc);
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                default:
                    value.push_back(esc);
                    break;
            }
            continue;
        }
        if (c == '"') {
            break;
        }
        value.push_back(c);
    }
    out = value;
    return true;
}

static bool FindJsonBool(const std::string& json, const char* key, bool& out) {
    std::string needle = std::string("\"") + key + "\"";
    size_t pos = json.find(needle);
    if (pos == std::string::npos) {
        return false;
    }
    pos = json.find(':', pos + needle.size());
    if (pos == std::string::npos) {
        return false;
    }
    pos++;
    while (pos < json.size() && std::isspace(static_cast<unsigned char>(json[pos]))) {
        ++pos;
    }
    if (json.compare(pos, 4, "true") == 0) {
        out = true;
        return true;
    }
    if (json.compare(pos, 5, "false") == 0) {
        out = false;
        return true;
    }
    return false;
}

static std::wstring ToWide(const std::string& str) {
    if (str.empty()) {
        return std::wstring();
    }
    int size = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, NULL, 0);
    if (size <= 0) {
        return std::wstring();
    }
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), -1, &out[0], size);
    if (!out.empty() && out.back() == L'\0') {
        out.pop_back();
    }
    return out;
}

static LoaderConfig LoadConfig() {
    LoaderConfig cfg;
    const std::string exeDir = GetExeDir();
    const std::string cfgPath = JoinPath(exeDir, "loader_config.json");
    std::string json = ReadTextFile(cfgPath);
    if (json.empty()) {
        std::cout << "Config not found: " << cfgPath << " (using defaults)\n";
        return cfg;
    }

    std::string val;
    if (FindJsonString(json, "dll_path", val)) {
        cfg.dll_path = val;
    }
    if (FindJsonString(json, "game_path", val)) {
        cfg.game_path = val;
    }
    if (FindJsonString(json, "process_name", val)) {
        cfg.process_name = val;
    }
    bool show = cfg.show_console;
    if (FindJsonBool(json, "show_console", show)) {
        cfg.show_console = show;
    }
    return cfg;
}

DWORD GetProcessIDByName(const wchar_t* processName) {
    PROCESSENTRY32W entry;
    entry.dwSize = sizeof(PROCESSENTRY32W);

    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, NULL);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return 0;
    }

    if (Process32FirstW(snapshot, &entry) == TRUE) {
        while (Process32NextW(snapshot, &entry) == TRUE) {
            if (_wcsicmp(entry.szExeFile, processName) == 0) {
                CloseHandle(snapshot);
                return entry.th32ProcessID;
            }
        }
    }

    CloseHandle(snapshot);
    return 0;
}

bool InjectDLL(DWORD processID, const char* dllPath) {
    HANDLE processHandle = OpenProcess(PROCESS_ALL_ACCESS, FALSE, processID);
    if (!processHandle) {
        std::cerr << "Failed to open process." << std::endl;
        return false;
    }

    void* allocatedMemory = VirtualAllocEx(processHandle, nullptr, strlen(dllPath) + 1, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!allocatedMemory) {
        std::cerr << "Failed to allocate memory in target process." << std::endl;
        CloseHandle(processHandle);
        return false;
    }

    if (!WriteProcessMemory(processHandle, allocatedMemory, dllPath, strlen(dllPath) + 1, nullptr)) {
        std::cerr << "Failed to write DLL path to target process." << std::endl;
        VirtualFreeEx(processHandle, allocatedMemory, 0, MEM_RELEASE);
        CloseHandle(processHandle);
        return false;
    }

    HMODULE kernel32 = GetModuleHandle(L"kernel32.dll");
    FARPROC loadLibrary = GetProcAddress(kernel32, "LoadLibraryA");

    HANDLE threadHandle = CreateRemoteThread(processHandle, nullptr, 0, (LPTHREAD_START_ROUTINE)loadLibrary, allocatedMemory, 0, nullptr);
    if (!threadHandle) {
        std::cerr << "Failed to create remote thread." << std::endl;
        VirtualFreeEx(processHandle, allocatedMemory, 0, MEM_RELEASE);
        CloseHandle(processHandle);
        return false;
    }

    WaitForSingleObject(threadHandle, INFINITE);

    VirtualFreeEx(processHandle, allocatedMemory, 0, MEM_RELEASE);
    CloseHandle(threadHandle);
    CloseHandle(processHandle);
    return true;
}

static int RunInjection(const LoaderConfig& cfg, const std::string& exeDir) {
    const std::string dllPathResolved = ResolvePath(exeDir, cfg.dll_path);
    const std::string gamePathResolved = ResolvePath(exeDir, cfg.game_path);

    std::wstring gamePathW = ToWide(gamePathResolved);
    std::wstring processNameW = ToWide(cfg.process_name);
    if (gamePathW.empty() || processNameW.empty()) {
        std::cerr << "Config error: empty game_path or process_name." << std::endl;
        return 1;
    }

    STARTUPINFOW si = {};
    PROCESS_INFORMATION pi = {};
    si.cb = sizeof(si);

    DWORD createFlags = cfg.show_console ? CREATE_NEW_CONSOLE : 0;
    if (!CreateProcessW(gamePathW.c_str(), NULL, NULL, NULL, FALSE, createFlags, NULL, NULL, &si, &pi)) {
        std::cerr << "Failed to start game process." << std::endl;
        return 1;
    }

    DWORD processID = 0;
    while (processID == 0) {
        std::cout << "find ProcessIDByName" << std::endl;
        processID = GetProcessIDByName(processNameW.c_str());
        Sleep(100);
    }

    const bool injected = InjectDLL(processID, dllPathResolved.c_str());
    if (injected) {
        std::cout << "DLL injected successfully!" << std::endl;
    } else {
        std::cout << "Failed to inject DLL." << std::endl;
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    return injected ? 0 : 1;
}

int main() {
    LoaderConfig cfg = LoadConfig();
    if (!cfg.show_console) {
        FreeConsole();
    }

    const std::string exeDir = GetExeDir();
    return RunInjection(cfg, exeDir);
}
