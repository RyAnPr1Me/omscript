#include "codegen.h"
#include "compiler.h"
#include "lexer.h"
#include "parser.h"
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <llvm/ADT/SmallVector.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#include <llvm/Support/raw_ostream.h>
#include <optional>
#include <sstream>
#include <vector>

#ifdef _WIN32
#include <io.h>
#include <process.h>
#include <windows.h>
#else
#include <unistd.h>
#endif

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

namespace {

constexpr const char* kCompilerVersion = "OmScript Compiler v" OMSC_VERSION;
constexpr const char* kPathConfigMarker = "# omsc-path-auto";
constexpr const char* kGitHubReleasesApiUrl = "https://api.github.com/repos/RyAnPr1Me/omscript/releases/latest";
constexpr const char* kGitHubReleasesDownloadBase = "https://github.com/RyAnPr1Me/omscript/releases/download";
constexpr const char* kDefaultRegistryUrl = "https://raw.githubusercontent.com/RyAnPr1Me/omscript/main/user-packages";
constexpr const char* kLocalPackagesDir = "om_packages";
constexpr int kApiTimeoutSeconds = 10;
constexpr int kDownloadTimeoutSeconds = 120;

// ---------------------------------------------------------------------------
// Cross-platform helpers
// ---------------------------------------------------------------------------

/// Create a unique temporary file from a template ending in XXXXXX.
/// On success the file is created and its fd is returned; the template buffer
/// is modified in-place to contain the actual path.  Returns -1 on failure.
int portableMkstemp(std::vector<char>& templateBuf) {
#ifdef _WIN32
    // _mktemp_s modifies the template in place, then we open the file.
    if (_mktemp_s(templateBuf.data(), templateBuf.size()) != 0) {
        return -1;
    }
    int fd = -1;
    _sopen_s(&fd, templateBuf.data(), _O_CREAT | _O_EXCL | _O_RDWR, _SH_DENYNO, _S_IREAD | _S_IWRITE);
    return fd;
#else
    return mkstemp(templateBuf.data());
#endif
}

/// Close a file descriptor portably.
void portableClose(int fd) {
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
}

/// Create a unique temporary directory from a template ending in XXXXXX.
/// Returns true on success; the template buffer is modified in-place.
bool portableMkdtemp(std::vector<char>& templateBuf) {
#ifdef _WIN32
    if (_mktemp_s(templateBuf.data(), templateBuf.size()) != 0) {
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(templateBuf.data(), ec);
    return !ec;
#else
    return mkdtemp(templateBuf.data()) != nullptr;
#endif
}

/// Return the path to the currently running executable.
std::string getExecutablePath() {
    const char* envPath = std::getenv("OMSC_BINARY_PATH");
    if (envPath) {
        return envPath;
    }
#if defined(_WIN32)
    char buf[32768]; // Use large buffer to avoid truncation
    DWORD len = GetModuleFileNameA(nullptr, buf, sizeof(buf));
    if (len > 0 && len < sizeof(buf) && GetLastError() != ERROR_INSUFFICIENT_BUFFER) {
        return std::string(buf, len);
    }
#elif defined(__APPLE__)
    char buf[4096];
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0) {
        try {
            return std::filesystem::canonical(buf).string();
        } catch (...) {
            return buf;
        }
    }
#else
    // Linux: /proc/self/exe
    try {
        return std::filesystem::read_symlink("/proc/self/exe").string();
    } catch (...) {
    }
#endif
    return "";
}

/// Return the PATH separator character for the current platform.
constexpr char kPathSeparator =
#ifdef _WIN32
    ';';
#else
    ':';
#endif

/// Return the binary filename for the compiler on this platform.
constexpr const char* kBinaryName =
#ifdef _WIN32
    "omsc.exe";
#else
    "omsc";
#endif

/// Return true if the current process has root/administrator privileges.
bool isPrivileged() {
#ifdef _WIN32
    // On Windows, check if the process is elevated.
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        return false;
    }
    TOKEN_ELEVATION elevation;
    DWORD size = sizeof(elevation);
    bool elevated = false;
    if (GetTokenInformation(token, TokenElevation, &elevation, sizeof(elevation), &size)) {
        elevated = elevation.TokenIsElevated != 0;
    }
    CloseHandle(token);
    return elevated;
#else
    return geteuid() == 0;
#endif
}

/// Return the platform-architecture string used in release asset names.
std::string getPlatformArch() {
#if defined(_WIN32)
#if defined(_M_ARM64)
    return "windows-arm64";
#else
    return "windows-x86_64";
#endif
#elif defined(__APPLE__)
#if defined(__arm64__) || defined(__aarch64__)
    return "macos-arm64";
#else
    return "macos-x86_64";
#endif
#else
#if defined(__aarch64__) || defined(_M_ARM64)
    return "linux-aarch64";
#else
    return "linux-x86_64";
#endif
#endif
}

/// Return the archive extension for the current platform.
constexpr const char* kArchiveExtension =
#ifdef _WIN32
    ".zip";
#else
    ".tar.gz";
#endif

/// Return the user home directory.
std::string getHomeDir() {
#ifdef _WIN32
    const char* userProfile = std::getenv("USERPROFILE");
    if (userProfile) {
        return userProfile;
    }
    const char* homeDrive = std::getenv("HOMEDRIVE");
    const char* homePath = std::getenv("HOMEPATH");
    if (homeDrive && homePath) {
        return std::string(homeDrive) + homePath;
    }
    return "";
#else
    const char* home = std::getenv("HOME");
    return home ? home : "";
#endif
}

/// Return the base URL for the remote package registry.
/// Defaults to the GitHub raw URL for the main branch of omscript.
/// Can be overridden via the OMSC_REGISTRY_URL environment variable.
std::string getRegistryBaseUrl() {
    const char* env = std::getenv("OMSC_REGISTRY_URL");
    if (env && env[0] != '\0') {
        return env;
    }
    return kDefaultRegistryUrl;
}

struct Version {
    int major = 0;
    int minor = 0;
    int patch = 0;
    bool valid = false;
};

Version parseVersion(const std::string& versionStr) {
    Version v;
    std::string s = versionStr;
    if (!s.empty() && s[0] == 'v') {
        s = s.substr(1);
    }
    try {
        std::istringstream ss(s);
        std::string part;
        if (!std::getline(ss, part, '.') || part.empty()) {
            return v;
        }
        v.major = std::stoi(part);
        if (!std::getline(ss, part, '.') || part.empty()) {
            return v;
        }
        v.minor = std::stoi(part);
        if (std::getline(ss, part, '.') && !part.empty()) {
            v.patch = std::stoi(part);
        }
        v.valid = true;
    } catch (const std::exception&) {
        v.valid = false;
    }
    return v;
}

bool versionGreaterThan(const Version& a, const Version& b) {
    if (a.major != b.major) {
        return a.major > b.major;
    }
    if (a.minor != b.minor) {
        return a.minor > b.minor;
    }
    return a.patch > b.patch;
}

// Extract the value of a simple JSON string field (e.g. "tag_name":"v0.9.4").
std::string extractJsonStringField(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\":\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) {
        return "";
    }
    pos += searchKey.size();
    size_t end = json.find('"', pos);
    if (end == std::string::npos) {
        return "";
    }
    return json.substr(pos, end - pos);
}

// Use curl to fetch the latest release tag from the GitHub API.
// Returns an empty string on failure.
std::string fetchLatestReleaseTag() {
    auto curlPathOrErr = llvm::sys::findProgramByName("curl");
    if (!curlPathOrErr) {
        return "";
    }
    std::string curlBin = *curlPathOrErr;

    // Create a secure temporary file.
    std::string tmpTemplate = std::filesystem::temp_directory_path().string() + "/omsc_release_XXXXXX";
    std::vector<char> tmpBuf(tmpTemplate.begin(), tmpTemplate.end());
    tmpBuf.push_back('\0');
    int fd = portableMkstemp(tmpBuf);
    if (fd == -1) {
        return "";
    }
    portableClose(fd);
    std::string tmpFile(tmpBuf.data());

    std::string timeoutStr = std::to_string(kApiTimeoutSeconds);
    std::vector<std::string> args = {curlBin,
                                     "-s",
                                     "-L",
                                     "--max-time",
                                     timeoutStr,
                                     "-H",
                                     "Accept: application/vnd.github.v3+json",
                                     "-H",
                                     "User-Agent: omsc-updater",
                                     "-o",
                                     tmpFile,
                                     kGitHubReleasesApiUrl};
    llvm::SmallVector<llvm::StringRef, 14> argRefs;
    for (const auto& a : args) {
        argRefs.push_back(a);
    }

    int rc = llvm::sys::ExecuteAndWait(curlBin, argRefs);
    if (rc != 0) {
        std::error_code ec;
        std::filesystem::remove(tmpFile, ec);
        return "";
    }

    std::ifstream f(tmpFile);
    if (!f.is_open()) {
        std::error_code ec;
        std::filesystem::remove(tmpFile, ec);
        return "";
    }
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    std::error_code ec;
    std::filesystem::remove(tmpFile, ec);

    if (json.empty()) {
        return "";
    }
    return extractJsonStringField(json, "tag_name");
}

// Download the release tarball for `tagName` and install the binary to `installDir`.
bool downloadAndInstallRelease(const std::string& tagName, const std::string& installDir) {
    auto curlPathOrErr = llvm::sys::findProgramByName("curl");
    if (!curlPathOrErr) {
        std::cerr << "Error: curl is required to download updates but was not found\n";
        return false;
    }
    auto tarPathOrErr = llvm::sys::findProgramByName("tar");
    if (!tarPathOrErr) {
        std::cerr << "Error: tar is required to extract updates but was not found\n";
        return false;
    }
    std::string curlBin = *curlPathOrErr;
    std::string tarBin = *tarPathOrErr;

    // Detect platform/architecture for the asset name.
    std::string platformArch = getPlatformArch();

    // Build the download URL: e.g. .../download/v0.9.4/omsc-0.9.4-linux-x86_64.tar.gz
    std::string version = tagName;
    if (!version.empty() && version[0] == 'v') {
        version = version.substr(1);
    }
    std::string assetName = "omsc-" + version + "-" + platformArch + std::string(kArchiveExtension);
    std::string downloadUrl = std::string(kGitHubReleasesDownloadBase) + "/" + tagName + "/" + assetName;

    // Create a secure temporary file for the tarball.
    std::string tmpBase = std::filesystem::temp_directory_path().string();
    std::string tarTemplate = tmpBase + "/omsc_update_XXXXXX";
    std::vector<char> tarBuf(tarTemplate.begin(), tarTemplate.end());
    tarBuf.push_back('\0');
    int tarFd = portableMkstemp(tarBuf);
    if (tarFd == -1) {
        std::cerr << "Error: failed to create temporary file for download\n";
        return false;
    }
    portableClose(tarFd);
    std::string tmpTarball(tarBuf.data());

    // Create a secure temporary directory for extraction.
    std::string dirTemplate = tmpBase + "/omsc_extract_XXXXXX";
    std::vector<char> dirBuf(dirTemplate.begin(), dirTemplate.end());
    dirBuf.push_back('\0');
    if (!portableMkdtemp(dirBuf)) {
        std::cerr << "Error: failed to create temporary directory for extraction\n";
        std::error_code ec;
        std::filesystem::remove(tmpTarball, ec);
        return false;
    }
    std::string tmpDir(dirBuf.data());

    std::cout << "Downloading " << assetName << "...\n";

    // Download tarball
    std::string downloadTimeout = std::to_string(kDownloadTimeoutSeconds);
    std::vector<std::string> dlArgs = {
        curlBin, "-L", "--max-time", downloadTimeout, "-H", "User-Agent: omsc-updater", "-o", tmpTarball, downloadUrl};
    llvm::SmallVector<llvm::StringRef, 10> dlArgRefs;
    for (const auto& a : dlArgs) {
        dlArgRefs.push_back(a);
    }
    int rc = llvm::sys::ExecuteAndWait(curlBin, dlArgRefs);
    if (rc != 0) {
        std::cerr << "Error: failed to download " << assetName << "\n";
        std::error_code ec;
        std::filesystem::remove(tmpTarball, ec);
        std::filesystem::remove_all(tmpDir, ec);
        return false;
    }

    // Extract tarball
    std::vector<std::string> tarArgs = {tarBin, "-xzf", tmpTarball, "-C", tmpDir};
    llvm::SmallVector<llvm::StringRef, 6> tarArgRefs;
    for (const auto& a : tarArgs) {
        tarArgRefs.push_back(a);
    }
    rc = llvm::sys::ExecuteAndWait(tarBin, tarArgRefs);
    std::error_code ec;
    std::filesystem::remove(tmpTarball, ec);
    if (rc != 0) {
        std::cerr << "Error: failed to extract " << assetName << "\n";
        std::filesystem::remove_all(tmpDir, ec);
        return false;
    }

    // Locate the omsc binary inside the extracted directory
    std::string binaryPath;
    try {
        for (const auto& entry : std::filesystem::recursive_directory_iterator(tmpDir)) {
            std::string name = entry.path().filename().string();
            if (name == "omsc" || name == "omsc-" + platformArch) {
                binaryPath = entry.path().string();
                break;
            }
        }
    } catch (const std::exception& e) {
        std::cerr << "Error searching extracted archive: " << e.what() << "\n";
        std::filesystem::remove_all(tmpDir, ec);
        return false;
    }
    if (binaryPath.empty()) {
        std::cerr << "Error: could not find omsc binary in extracted archive\n";
        std::filesystem::remove_all(tmpDir, ec);
        return false;
    }

    // Install the binary atomically: copy to temp file then rename.
    std::string targetPath = installDir + "/omsc";
    std::string tmpTemplate = installDir + "/omsc_update_XXXXXX";
    std::vector<char> installTmpBuf(tmpTemplate.begin(), tmpTemplate.end());
    installTmpBuf.push_back('\0');
    int installTmpFd = portableMkstemp(installTmpBuf);
    if (installTmpFd == -1) {
        std::cerr << "Error: failed to create temporary file for installation in " << installDir << "\n";
        std::filesystem::remove_all(tmpDir, ec);
        return false;
    }
    portableClose(installTmpFd);
    std::string installTmpPath(installTmpBuf.data());
    try {
        std::filesystem::copy_file(binaryPath, installTmpPath, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::permissions(installTmpPath,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec |
                                         std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
                                         std::filesystem::perms::others_read | std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::replace);
        std::filesystem::rename(installTmpPath, targetPath);
    } catch (const std::exception& e) {
        std::cerr << "Error installing update: " << e.what() << "\n";
        std::filesystem::remove(installTmpPath, ec);
        std::filesystem::remove_all(tmpDir, ec);
        return false;
    }
    std::filesystem::remove_all(tmpDir, ec);
    std::cout << "OmScript updated to " << tagName << " at " << targetPath << "\n";
    return true;
}

bool isRoot() {
    return isPrivileged();
}

std::string detectDistro() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    std::ifstream osRelease("/etc/os-release");
    if (!osRelease.is_open()) {
        return "unknown";
    }
    std::string line;
    while (std::getline(osRelease, line)) {
        if (line.find("ID=") == 0) {
            if (line.find("arch") != std::string::npos || line.find("manjaro") != std::string::npos ||
                line.find("endeavouros") != std::string::npos) {
                return "arch";
            } else if (line.find("ubuntu") != std::string::npos || line.find("debian") != std::string::npos ||
                       line.find("linuxmint") != std::string::npos) {
                return "debian";
            }
        }
    }
    return "linux";
#endif
}

std::string getInstallPrefix(bool system) {
#ifdef _WIN32
    if (system) {
        const char* pf = std::getenv("ProgramFiles");
        return pf ? std::string(pf) + "\\OmScript" : "C:\\Program Files\\OmScript";
    }
    std::string home = getHomeDir();
    return home + "\\.omscript";
#else
    if (system) {
        return "/usr/local";
    }
    std::string home = getHomeDir();
    return home + "/.local";
#endif
}

std::string getInstallBinDir(bool system) {
#ifdef _WIN32
    return getInstallPrefix(system);
#else
    return getInstallPrefix(system) + "/bin";
#endif
}

bool fileExists(const std::string& path) {
    return std::filesystem::exists(path);
}

bool isInPath(const std::string& binDir) {
    const char* pathPtr = getenv("PATH");
    std::string pathEnv = pathPtr ? pathPtr : "";
    size_t pos = 0;
    std::string token;
    while ((pos = pathEnv.find(kPathSeparator)) != std::string::npos) {
        token = pathEnv.substr(0, pos);
        if (token == binDir) {
            return true;
        }
        pathEnv.erase(0, pos + 1);
    }
    return pathEnv == binDir;
}

bool isSymlinkOrCopy(const std::string& path, const std::string& target) {
    if (!fileExists(path)) {
        return false;
    }
    try {
        auto canonicalTarget = std::filesystem::canonical(target);
        if (std::filesystem::is_symlink(path)) {
            return std::filesystem::read_symlink(path) == canonicalTarget;
        }
        return std::filesystem::canonical(path) == canonicalTarget;
    } catch (...) {
        return false;
    }
}

bool installToSystem(const std::string& targetDir, bool force) {
    std::string exePath = getExecutablePath();

    if (exePath.empty() || !fileExists(exePath)) {
        std::cerr << "Error: Cannot determine own executable path\n";
        return false;
    }

    if (!fileExists(targetDir)) {
        std::cerr << "Error: Target directory does not exist: " << targetDir << "\n";
        return false;
    }

    std::string targetPath = targetDir + "/" + std::string(kBinaryName);

    if (!force && isSymlinkOrCopy(targetPath, exePath)) {
        std::cout << "OmScript is already installed at " << targetPath << "\n";
        return true;
    }

    // Write to a temp file in the target directory, then atomically rename so
    // the in-place replace is safe even if the copy fails partway through.
    std::string tmpTemplate = targetDir + "/omsc_install_XXXXXX";
    std::vector<char> tmpBuf(tmpTemplate.begin(), tmpTemplate.end());
    tmpBuf.push_back('\0');
    int tmpFd = portableMkstemp(tmpBuf);
    if (tmpFd == -1) {
        std::cerr << "Error: failed to create temporary file in " << targetDir << "\n";
        return false;
    }
    portableClose(tmpFd);
    std::string tmpPath(tmpBuf.data());

    try {
        std::filesystem::copy_file(exePath, tmpPath, std::filesystem::copy_options::overwrite_existing);
        std::filesystem::permissions(tmpPath,
                                     std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec |
                                         std::filesystem::perms::group_read | std::filesystem::perms::group_exec |
                                         std::filesystem::perms::others_read | std::filesystem::perms::others_exec,
                                     std::filesystem::perm_options::replace);
        std::filesystem::rename(tmpPath, targetPath);
    } catch (const std::exception& e) {
        std::cerr << "Error installing binary: " << e.what() << "\n";
        std::error_code ec;
        std::filesystem::remove(tmpPath, ec);
        return false;
    }

    std::cout << "Installed OmScript to " << targetPath << "\n";
    return true;
}

void doInstall() {
    std::string distro = detectDistro();
    std::cout << "Detected distribution: " << distro << "\n";

    // Determine install directory up front so the update path can use it.
    std::string installDir = isRoot() ? "/usr/local/bin" : getInstallBinDir(false);

    // Check GitHub releases for a newer version.
    std::cout << "Checking for updates...\n";
    std::string latestTag = fetchLatestReleaseTag();
    if (!latestTag.empty()) {
        // Extract the version tag from the compiled-in version string (e.g. "v0.9.3").
        std::string currentTag;
        std::string versionStr(kCompilerVersion);
        size_t vPos = versionStr.rfind('v');
        if (vPos != std::string::npos) {
            currentTag = versionStr.substr(vPos);
        }

        Version currentVer = parseVersion(currentTag);
        Version latestVer = parseVersion(latestTag);

        if (currentVer.valid && latestVer.valid && versionGreaterThan(latestVer, currentVer)) {
            std::cout << "New version available: " << latestTag << " (current: " << currentTag << ")\n";
            if (!fileExists(installDir)) {
                std::filesystem::create_directories(installDir);
            }
            if (downloadAndInstallRelease(latestTag, installDir)) {
                std::cout << "\nOmScript has been updated to " << latestTag << "!\n";
                if (!isRoot() && !isInPath(installDir)) {
                    std::cout << "\nIMPORTANT: Add " << installDir << " to your PATH:\n";
                    std::cout << "    echo 'export PATH=\"" << installDir << ":$PATH\"' >> ~/.bashrc\n";
                    std::cout << "    source ~/.bashrc\n";
                }
                return;
            }
            std::cout << "Update failed, falling back to installing current version...\n";
        } else if (currentVer.valid && latestVer.valid) {
            std::cout << "Already at the latest version (" << currentTag << ").\n";
        }
    } else {
        std::cout << "Could not check for updates (network unavailable or curl not found).\n";
    }

    std::string exePath = getExecutablePath();

    if (exePath.empty() || !fileExists(exePath)) {
        std::cerr << "Error: Cannot determine own executable path\n";
        return;
    }

    std::string binDir = getInstallBinDir(false);
    std::string userPath = binDir + "/" + std::string(kBinaryName);

    if (isRoot()) {
        std::string sysBinDir = getInstallBinDir(true);
        std::string sysPath = sysBinDir + "/" + std::string(kBinaryName);
        if (fileExists(sysPath)) {
            std::cout << "Updating system installation at " << sysPath << "...\n";
        } else {
            std::cout << "Installing to system location " << sysPath << "...\n";
        }
        installToSystem(sysBinDir, true);
        std::cout << "\nOmScript has been installed system-wide!\n";
        return;
    }

    std::cout << "Not running as root. Attempting user installation...\n";

    if (!fileExists(binDir)) {
        std::cout << "Creating " << binDir << "...\n";
        std::filesystem::create_directories(binDir);
    }

    if (isInPath(binDir)) {
        std::cout << binDir << " is already in PATH.\n";
    } else {
        std::cout << "\nWARNING: " << binDir << " is not in your PATH!\n";
        std::cout << "Add this to your ~/.bashrc or ~/.profile:\n";
        std::cout << "    export PATH=\"" << binDir << ":$PATH\"\n\n";
    }

    std::cout << "Installing to user location " << userPath << "...\n";
    installToSystem(binDir, true);

    std::cout << "\nInstallation complete!\n";
    std::cout << "Run 'omsc --version' to verify.\n";

    if (!isInPath(binDir)) {
        std::cout << "\nIMPORTANT: Add " << binDir << " to your PATH:\n";
        std::cout << "    echo 'export PATH=\"" << binDir << ":$PATH\"' >> ~/.bashrc\n";
        std::cout << "    source ~/.bashrc\n";
    }
}

void doUninstall() {
    // Candidate paths to check, in priority order.
    std::vector<std::string> candidates;
    std::string bn(kBinaryName);
    if (isRoot()) {
        candidates.push_back(getInstallBinDir(true) + "/" + bn);
#ifndef _WIN32
        candidates.push_back("/usr/bin/" + bn);
#endif
    } else {
        candidates.push_back(getInstallBinDir(false) + "/" + bn);
#ifndef _WIN32
        candidates.push_back("/usr/local/bin/" + bn);
        candidates.push_back("/usr/bin/" + bn);
#endif
    }

    bool removedAny = false;
    for (const auto& path : candidates) {
        std::error_code ec;
        if (!std::filesystem::exists(path, ec)) {
            continue;
        }
        if (std::filesystem::remove(path, ec)) {
            std::cout << "Removed " << path << "\n";
            removedAny = true;
        } else {
            std::cerr << "Error: failed to remove " << path << ": " << ec.message() << "\n";
        }
    }

    if (!removedAny) {
        std::cout << "OmScript is not installed in any known location.\n";
        return;
    }

    // Remove PATH entries added by omsc install from shell config files.
#ifndef _WIN32
    std::string home = getHomeDir();
    if (!home.empty()) {
    std::vector<std::string> shellConfigs = {home + "/.bashrc", home + "/.profile",
                                             home + "/.zshrc"};
    for (const auto& configPath : shellConfigs) {
        std::ifstream in(configPath);
        if (!in.is_open()) {
            continue;
        }
        std::vector<std::string> lines;
        std::string line;
        bool changed = false;
        while (std::getline(in, line)) {
            lines.push_back(line);
        }
        in.close();

        // Remove lines that contain the omsc PATH marker and the PATH export added by omsc.
        // The marker is a unique sentinel written exclusively by omsc install.
        std::vector<std::string> filtered;
        for (size_t i = 0; i < lines.size(); ++i) {
            if (lines[i].find(kPathConfigMarker) != std::string::npos) {
                // Skip the marker line and the following export line if it was written by omsc.
                changed = true;
                if (i + 1 < lines.size() && lines[i + 1].find("export PATH=") != std::string::npos) {
                    ++i; // skip the export line too
                }
                continue;
            }
            filtered.push_back(lines[i]);
        }

        if (!changed) {
            continue;
        }

        // Write atomically: write to a temp file then rename.
        std::string tmpTemplate = configPath + ".omsc_XXXXXX";
        std::vector<char> cfgTmpBuf(tmpTemplate.begin(), tmpTemplate.end());
        cfgTmpBuf.push_back('\0');
        int cfgTmpFd = portableMkstemp(cfgTmpBuf);
        if (cfgTmpFd == -1) {
            std::cerr << "Warning: could not create temp file to update " << configPath << "\n";
            continue;
        }
        portableClose(cfgTmpFd);
        std::string cfgTmpPath(cfgTmpBuf.data());
        {
            std::ofstream out(cfgTmpPath, std::ios::trunc);
            if (!out.is_open()) {
                std::cerr << "Warning: could not update " << configPath << "\n";
                std::error_code ec;
                std::filesystem::remove(cfgTmpPath, ec);
                continue;
            }
            for (const auto& l : filtered) {
                out << l << "\n";
            }
        }
        std::error_code ec;
        std::filesystem::rename(cfgTmpPath, configPath, ec);
        if (ec) {
            std::cerr << "Warning: could not update " << configPath << ": " << ec.message() << "\n";
            std::filesystem::remove(cfgTmpPath, ec);
            continue;
        }
        std::cout << "Removed PATH entry from " << configPath << "\n";
    }
    }
#endif

    std::cout << "\nOmScript has been uninstalled.\n";
}

void ensureInPath() {
#ifdef _WIN32
    // On Windows, PATH management is handled differently (e.g. via setx).
    // Skip automatic shell config modifications.
    return;
#else
    const char* binaryPath = getenv("OMSC_BINARY_PATH");
    if (!binaryPath) {
        return;
    }

    std::string binaryDir = std::filesystem::path(binaryPath).parent_path();
    std::string exePath;
    try {
        exePath = std::filesystem::canonical(binaryPath);
    } catch (const std::filesystem::filesystem_error&) {
        return;
    }

    std::string home = getHomeDir();
    if (home.empty()) {
        return;
    }
    std::string shellConfig = home + "/.bashrc";
    std::ifstream checkConfig(shellConfig);
    if (!checkConfig.is_open()) {
        shellConfig = home + "/.profile";
    }
    checkConfig.close();

    std::ifstream readConfig(shellConfig);
    if (!readConfig.is_open()) {
        return;
    }

    std::string line;
    bool found = false;
    while (std::getline(readConfig, line)) {
        if (line.find(kPathConfigMarker) != std::string::npos && line.find(binaryDir) != std::string::npos) {
            found = true;
            break;
        }
    }
    readConfig.close();

    if (!found) {
        std::ofstream writeConfig(shellConfig, std::ios::app);
        if (writeConfig.is_open()) {
            writeConfig << "\n"
                        << kPathConfigMarker << "\n"
                        << "export PATH=\"" << binaryDir << ":$PATH\"\n";
            writeConfig.close();
            std::cout << "Added " << binaryDir << " to PATH in " << shellConfig << "\n";
            std::cout << "Please run 'source " << shellConfig << "' or restart your terminal\n";
        }
    }

    if (exePath != binaryDir + "/omsc") {
        std::string linkPath = binaryDir + "/omsc";
        std::error_code ec;
        std::filesystem::create_symlink(exePath, linkPath, ec);
        if (ec) {
            std::cerr << "Warning: could not create symlink " << linkPath << ": " << ec.message() << "\n";
        }
    }
#endif
}

// Paths of temporary files to clean up on abnormal exit (signal).
// These are set by the 'run' command before executing the compiled program.
// Fixed-size C-style buffers for async-signal-safe access in signal handlers.
static constexpr size_t kMaxTempPathLen = 4096;
static char g_tempOutputFile[kMaxTempPathLen] = {};
static char g_tempObjectFile[kMaxTempPathLen] = {};

// Signal handler for SIGINT / SIGTERM — removes temporary files created
// during `omsc run` and re-raises so the default handler sets the exit status.
// Uses only async-signal-safe operations (unlink, _exit, signal, raise).
void signalHandler(int sig) {
    if (g_tempOutputFile[0] != '\0') {
#ifdef _WIN32
        _unlink(g_tempOutputFile);
#else
        unlink(g_tempOutputFile);
#endif
    }
    if (g_tempObjectFile[0] != '\0') {
#ifdef _WIN32
        _unlink(g_tempObjectFile);
#else
        unlink(g_tempObjectFile);
#endif
    }
    // Re-raise the signal with default action so the exit status reflects it.
    signal(sig, SIG_DFL);
    raise(sig);
}

// ---------------------------------------------------------------------------
// Package manager
// ---------------------------------------------------------------------------

/// Minimal JSON string field extractor (reuse the existing pattern).
/// Extracts a simple "key":"value" or "key": "value" pair.
std::string jsonField(const std::string& json, const std::string& key) {
    // Try with no space: "key":"value"
    std::string pattern1 = "\"" + key + "\":\"";
    size_t pos = json.find(pattern1);
    if (pos != std::string::npos) {
        pos += pattern1.size();
        size_t end = json.find('"', pos);
        if (end != std::string::npos) {
            return json.substr(pos, end - pos);
        }
    }
    // Try with space: "key": "value"
    std::string pattern2 = "\"" + key + "\": \"";
    pos = json.find(pattern2);
    if (pos != std::string::npos) {
        pos += pattern2.size();
        size_t end = json.find('"', pos);
        if (end != std::string::npos) {
            return json.substr(pos, end - pos);
        }
    }
    return "";
}

/// Extract a JSON array of strings, e.g. "files": ["a.om", "b.om"]
std::vector<std::string> jsonArrayField(const std::string& json, const std::string& key) {
    std::vector<std::string> result;
    std::string pattern = "\"" + key + "\"";
    size_t pos = json.find(pattern);
    if (pos == std::string::npos) {
        return result;
    }
    pos = json.find('[', pos);
    if (pos == std::string::npos) {
        return result;
    }
    size_t end = json.find(']', pos);
    if (end == std::string::npos) {
        return result;
    }
    std::string arr = json.substr(pos + 1, end - pos - 1);
    // Parse simple quoted strings from the array
    size_t i = 0;
    while (i < arr.size()) {
        size_t q1 = arr.find('"', i);
        if (q1 == std::string::npos)
            break;
        size_t q2 = arr.find('"', q1 + 1);
        if (q2 == std::string::npos)
            break;
        result.push_back(arr.substr(q1 + 1, q2 - q1 - 1));
        i = q2 + 1;
    }
    return result;
}

struct PackageInfo {
    std::string name;
    std::string version;
    std::string description;
    std::string entry;
    std::vector<std::string> files;
    bool valid = false;
};

/// Parse a PackageInfo from a JSON string (e.g. the contents of package.json).
PackageInfo parsePackageManifest(const std::string& json) {
    PackageInfo info;
    info.name = jsonField(json, "name");
    info.version = jsonField(json, "version");
    info.description = jsonField(json, "description");
    info.entry = jsonField(json, "entry");
    info.files = jsonArrayField(json, "files");
    info.valid = !info.name.empty();
    return info;
}

/// Read a PackageInfo from a local file on disk.
PackageInfo readPackageManifest(const std::string& manifestPath) {
    std::ifstream f(manifestPath);
    if (!f.is_open()) {
        return PackageInfo{};
    }
    std::string json((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    return parsePackageManifest(json);
}

/// Download a URL to a local file using curl.  Returns true on success.
bool downloadFile(const std::string& url, const std::string& destPath) {
    // Reject paths containing ".." to prevent path traversal
    if (destPath.find("..") != std::string::npos) {
        std::cerr << "Error: invalid destination path\n";
        return false;
    }
    auto curlPathOrErr = llvm::sys::findProgramByName("curl");
    if (!curlPathOrErr) {
        std::cerr << "Error: curl is required for package downloads but was not found\n";
        std::cerr << "  Install with: apt-get install curl (Debian/Ubuntu) or brew install curl (macOS)\n";
        return false;
    }
    std::string curlBin = *curlPathOrErr;
    std::string timeoutStr = std::to_string(kDownloadTimeoutSeconds);
    std::vector<std::string> args = {curlBin,    "-s", "-f",     "-L", "--max-redirs", "5", "--max-time",
                                     timeoutStr, "-o", destPath, url};
    llvm::SmallVector<llvm::StringRef, 12> argRefs;
    for (const auto& a : args) {
        argRefs.push_back(a);
    }
    int rc = llvm::sys::ExecuteAndWait(curlBin, argRefs);
    return rc == 0;
}

/// Download a URL and return its contents as a string.
/// Returns an empty string on failure.
std::string downloadString(const std::string& url) {
    auto curlPathOrErr = llvm::sys::findProgramByName("curl");
    if (!curlPathOrErr) {
        return "";
    }
    std::string curlBin = *curlPathOrErr;

    // Create a secure temporary file
    std::string tmpTemplate = std::filesystem::temp_directory_path().string() + "/omsc_pkg_XXXXXX";
    std::vector<char> tmpBuf(tmpTemplate.begin(), tmpTemplate.end());
    tmpBuf.push_back('\0');
    int fd = portableMkstemp(tmpBuf);
    if (fd == -1) {
        return "";
    }
    portableClose(fd);
    std::string tmpFile(tmpBuf.data());

    std::string timeoutStr = std::to_string(kApiTimeoutSeconds);
    std::vector<std::string> args = {curlBin,    "-s", "-f",    "-L", "--max-redirs", "5", "--max-time",
                                     timeoutStr, "-o", tmpFile, url};
    llvm::SmallVector<llvm::StringRef, 12> argRefs;
    for (const auto& a : args) {
        argRefs.push_back(a);
    }
    int rc = llvm::sys::ExecuteAndWait(curlBin, argRefs);
    if (rc != 0) {
        std::error_code ec;
        std::filesystem::remove(tmpFile, ec);
        return "";
    }
    std::ifstream f(tmpFile);
    if (!f.is_open()) {
        std::error_code ec;
        std::filesystem::remove(tmpFile, ec);
        return "";
    }
    std::string content((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    f.close();
    std::error_code ec;
    std::filesystem::remove(tmpFile, ec);
    return content;
}

/// Parse the remote index.json into a list of PackageInfo objects.
/// The index is a JSON file with a top-level "packages" array where each
/// element has the same fields as a package.json manifest.
std::vector<PackageInfo> parseRegistryIndex(const std::string& json) {
    std::vector<PackageInfo> packages;
    // Find the "packages" array
    std::string key = "\"packages\"";
    size_t pos = json.find(key);
    if (pos == std::string::npos) {
        return packages;
    }
    pos = json.find('[', pos);
    if (pos == std::string::npos) {
        return packages;
    }
    // Find each object {...} inside the array
    size_t depth = 0;
    size_t objStart = std::string::npos;
    for (size_t i = pos + 1; i < json.size(); ++i) {
        char c = json[i];
        if (c == '{') {
            if (depth == 0) {
                objStart = i;
            }
            depth++;
        } else if (c == '}') {
            depth--;
            if (depth == 0 && objStart != std::string::npos) {
                std::string obj = json.substr(objStart, i - objStart + 1);
                auto info = parsePackageManifest(obj);
                if (info.valid) {
                    packages.push_back(info);
                }
                objStart = std::string::npos;
            }
        } else if (c == ']' && depth == 0) {
            break;
        }
    }
    // Sort by name for consistent output
    std::sort(packages.begin(), packages.end(),
              [](const PackageInfo& a, const PackageInfo& b) { return a.name < b.name; });
    return packages;
}

int doPkgInstall(const std::string& pkgName, bool quiet) {
    if (!quiet) {
        std::cout << "Fetching package '" << pkgName << "'...\n";
    }

    // Download the package manifest from GitHub
    std::string registryBase = getRegistryBaseUrl();
    std::string manifestUrl = registryBase + "/" + pkgName + "/package.json";
    std::string manifestJson = downloadString(manifestUrl);
    if (manifestJson.empty()) {
        std::cerr << "Error: package '" << pkgName << "' not found in registry\n";
        return 1;
    }
    auto manifest = parsePackageManifest(manifestJson);
    if (!manifest.valid) {
        std::cerr << "Error: invalid package manifest for '" << pkgName << "'\n";
        return 1;
    }

    // Determine files to download
    std::vector<std::string> files = manifest.files;
    if (files.empty() && !manifest.entry.empty()) {
        files.push_back(manifest.entry);
    }
    if (files.empty()) {
        std::cerr << "Error: package '" << pkgName << "' has no files to install\n";
        return 1;
    }

    // Create local install directory
    std::string localDir = std::string(kLocalPackagesDir) + "/" + pkgName;
    std::filesystem::create_directories(localDir);

    // Download each file from GitHub
    for (const auto& file : files) {
        std::string fileUrl = registryBase + "/" + pkgName + "/" + file;
        std::string dst = localDir + "/" + file;
        if (!downloadFile(fileUrl, dst)) {
            std::cerr << "Error: failed to download '" << file << "'\n";
            // Clean up partial install
            std::error_code ec;
            std::filesystem::remove_all(localDir, ec);
            return 1;
        }
    }

    // Write the manifest locally
    {
        std::ofstream mf(localDir + "/package.json");
        if (mf.is_open()) {
            mf << manifestJson;
        }
    }

    if (!quiet) {
        std::cout << "Installed " << pkgName << "@" << manifest.version << " (" << files.size() << " file(s))\n";
    }
    return 0;
}

int doPkgRemove(const std::string& pkgName, bool quiet) {
    std::string localDir = std::string(kLocalPackagesDir) + "/" + pkgName;
    if (!std::filesystem::is_directory(localDir)) {
        std::cerr << "Error: package '" << pkgName << "' is not installed\n";
        return 1;
    }
    std::error_code ec;
    std::filesystem::remove_all(localDir, ec);
    if (ec) {
        std::cerr << "Error: failed to remove package '" << pkgName << "': " << ec.message() << "\n";
        return 1;
    }
    if (!quiet) {
        std::cout << "Removed " << pkgName << "\n";
    }
    return 0;
}

int doPkgList(bool quiet) {
    if (!std::filesystem::is_directory(kLocalPackagesDir)) {
        if (!quiet) {
            std::cout << "No packages installed.\n";
        }
        return 0;
    }
    bool found = false;
    for (const auto& entry : std::filesystem::directory_iterator(kLocalPackagesDir)) {
        if (!entry.is_directory()) {
            continue;
        }
        auto manifest = entry.path() / "package.json";
        if (std::filesystem::exists(manifest)) {
            auto info = readPackageManifest(manifest.string());
            if (info.valid) {
                std::cout << info.name << "@" << info.version;
                if (!info.description.empty()) {
                    std::cout << " - " << info.description;
                }
                std::cout << "\n";
                found = true;
            }
        }
    }
    if (!found && !quiet) {
        std::cout << "No packages installed.\n";
    }
    return 0;
}

int doPkgSearch(const std::string& query, bool quiet) {
    std::string indexJson = downloadString(getRegistryBaseUrl() + "/index.json");
    if (indexJson.empty()) {
        std::cerr << "Error: failed to fetch package registry (check your internet connection)\n";
        return 1;
    }
    auto packages = parseRegistryIndex(indexJson);
    if (packages.empty()) {
        if (!quiet) {
            std::cout << "No packages available.\n";
        }
        return 0;
    }
    bool found = false;
    for (const auto& pkg : packages) {
        // If query is empty, show all; otherwise filter by name or description
        if (!query.empty()) {
            if (pkg.name.find(query) == std::string::npos && pkg.description.find(query) == std::string::npos) {
                continue;
            }
        }
        std::cout << pkg.name << "@" << pkg.version;
        if (!pkg.description.empty()) {
            std::cout << " - " << pkg.description;
        }
        std::cout << "\n";
        found = true;
    }
    if (!found && !quiet) {
        std::cout << "No packages matching '" << query << "'.\n";
    }
    return 0;
}

int doPkgInfo(const std::string& pkgName) {
    // First check installed packages
    std::string localManifest = std::string(kLocalPackagesDir) + "/" + pkgName + "/package.json";
    bool installed = std::filesystem::exists(localManifest);

    PackageInfo info;
    if (installed) {
        info = readPackageManifest(localManifest);
    } else {
        // Download manifest from remote registry
        std::string manifestUrl = getRegistryBaseUrl() + "/" + pkgName + "/package.json";
        std::string manifestJson = downloadString(manifestUrl);
        if (!manifestJson.empty()) {
            info = parsePackageManifest(manifestJson);
        }
    }

    if (!info.valid) {
        std::cerr << "Error: package '" << pkgName << "' not found\n";
        return 1;
    }

    std::cout << "Name:        " << info.name << "\n";
    std::cout << "Version:     " << info.version << "\n";
    std::cout << "Description: " << info.description << "\n";
    std::cout << "Entry:       " << info.entry << "\n";
    if (!info.files.empty()) {
        std::cout << "Files:       ";
        for (size_t i = 0; i < info.files.size(); ++i) {
            if (i > 0)
                std::cout << ", ";
            std::cout << info.files[i];
        }
        std::cout << "\n";
    }
    std::cout << "Installed:   " << (installed ? "yes" : "no") << "\n";
    return 0;
}

/// Entry point for `omsc pkg <subcommand> [args...]`.
/// Returns the exit code.
int doPkg(int argc, char* argv[], int startIndex, bool quiet) {
    if (startIndex >= argc) {
        std::cerr << "Error: missing pkg subcommand (install, remove, list, search, info)\n";
        return 1;
    }
    std::string sub = argv[startIndex];
    if (sub == "install" || sub == "add") {
        if (startIndex + 1 >= argc) {
            std::cerr << "Error: pkg install requires a package name\n";
            return 1;
        }
        return doPkgInstall(argv[startIndex + 1], quiet);
    }
    if (sub == "remove" || sub == "uninstall" || sub == "rm") {
        if (startIndex + 1 >= argc) {
            std::cerr << "Error: pkg remove requires a package name\n";
            return 1;
        }
        return doPkgRemove(argv[startIndex + 1], quiet);
    }
    if (sub == "list" || sub == "ls") {
        return doPkgList(quiet);
    }
    if (sub == "search" || sub == "find") {
        std::string query;
        if (startIndex + 1 < argc) {
            query = argv[startIndex + 1];
        }
        return doPkgSearch(query, quiet);
    }
    if (sub == "info" || sub == "show") {
        if (startIndex + 1 >= argc) {
            std::cerr << "Error: pkg info requires a package name\n";
            return 1;
        }
        return doPkgInfo(argv[startIndex + 1]);
    }
    std::cerr << "Error: unknown pkg subcommand '" << sub << "'\n";
    std::cerr << "Available subcommands: install, remove, list, search, info\n";
    return 1;
}

void printUsage(const char* progName) {
    std::cout << kCompilerVersion << "\n";
    std::cout << "Usage:\n";
    std::cout << "  " << progName << " <source.om> [-o output]\n";
    std::cout << "  " << progName << " compile <source.om> [-o output]\n";
    std::cout << "  " << progName << " run <source.om> [-o output] [-- args...]\n";
    std::cout << "  " << progName << " check <source.om>\n";
    std::cout << "  " << progName << " lex <source.om>\n";
    std::cout << "  " << progName << " parse <source.om>\n";
    std::cout << "  " << progName << " emit-ast <source.om>\n";
    std::cout << "  " << progName << " emit-ir <source.om> [-o output.ll]\n";
    std::cout << "  " << progName << " clean [-o output]\n";
    std::cout << "  " << progName << " version\n";
    std::cout << "  " << progName << " install\n";
    std::cout << "  " << progName << " uninstall\n";
    std::cout << "  " << progName << " pkg <subcommand> [args]\n";
    std::cout << "  " << progName << " help\n";
    std::cout << "\nCommands:\n";
    std::cout << "  -b, -c, --build, --compile  Compile a source file (default)\n";
    std::cout << "  -r, --run            Compile and run a source file\n";
    std::cout << "  --check              Parse and validate a source file without generating code\n";
    std::cout << "  -l, --lex, --tokens  Print lexer tokens\n";
    std::cout << "  -a, -p, --ast, --parse, --emit-ast  Parse and summarize the AST\n";
    std::cout << "  -e, -i, --emit-ir, --ir     Emit LLVM IR\n";
    std::cout << "  -C, --clean          Remove outputs\n";
    std::cout << "  -h, --help           Show this help message\n";
    std::cout << "  -v, --version        Show compiler version\n";
    std::cout << "\nGeneral Options:\n";
    std::cout << "  -o, --output <file>  Output file name (default: a.out, stdout for emit-ir)\n";
    std::cout << "  -k, --keep-temps     Keep temporary outputs when running\n";
    std::cout << "  -V, --verbose        Show detailed compilation output (IR, progress)\n";
    std::cout << "  -q, --quiet          Suppress all non-error output\n";
    std::cout << "  --time               Show compilation timing breakdown\n";
    std::cout << "  --dump-ast           Print the full AST tree (with parse/emit-ast command)\n";
    std::cout << "  --dump-tokens        Alias for the lex command\n";
    std::cout << "  --emit-obj           Emit object file only (skip linking)\n";
    std::cout << "  --dry-run            Validate and compile without writing output files\n";
    std::cout << "\nOptimization Levels:\n";
    std::cout << "  -O0                  No optimization\n";
    std::cout << "  -O1                  Basic optimization\n";
    std::cout << "  -O2                  Moderate optimization (default)\n";
    std::cout << "  -O3                  Aggressive optimization\n";
    std::cout << "  -Ofast               Maximum runtime optimization (alias for -O3)\n";
    std::cout << "\nTarget Options:\n";
    std::cout << "  -march=<cpu>         Target CPU architecture (default: native)\n";
    std::cout << "                       Examples: native, x86-64, skylake, znver3, cortex-a72\n";
    std::cout << "  -mtune=<cpu>         CPU to optimize for (scheduling and tuning)\n";
    std::cout << "                       Default: same as -march\n";
    std::cout << "\nFeature Flags:\n";
    std::cout << "  -flto                Enable link-time optimization\n";
    std::cout << "  -fno-lto             Disable link-time optimization (default)\n";
    std::cout << "  -fpic                Generate position-independent code (default)\n";
    std::cout << "  -fno-pic             Disable position-independent code\n";
    std::cout << "  -ffast-math          Enable unsafe floating-point optimizations\n";
    std::cout << "  -fno-fast-math       Disable unsafe floating-point optimizations (default)\n";
    std::cout << "  -foptmax             Enable OPTMAX block optimization (default)\n";
    std::cout << "  -fno-optmax          Disable OPTMAX block optimization\n";
    std::cout << "  -fjit                Enable hybrid bytecode/JIT compilation (default)\n";
    std::cout << "  -fno-jit             Disable JIT; compile all functions to native code\n";
    std::cout << "  -fstack-protector    Enable stack protection\n";
    std::cout << "  -fno-stack-protector Disable stack protection (default)\n";
    std::cout << "\nLinker Options:\n";
    std::cout << "  -static              Use static linking\n";
    std::cout << "  -s, --strip          Strip symbols from output binary\n";
    std::cout << "\nInstallation:\n";
    std::cout << "  " << progName << " install        Add to PATH (first run or update)\n";
    std::cout << "  " << progName << " uninstall      Remove installed binary and PATH entry\n";
    std::cout << "\nPackage Manager:\n";
    std::cout << "  " << progName << " pkg install <name>    Download and install a package\n";
    std::cout << "  " << progName << " pkg remove <name>     Remove an installed package\n";
    std::cout << "  " << progName << " pkg list              List installed packages\n";
    std::cout << "  " << progName << " pkg search [query]    Search available packages online\n";
    std::cout << "  " << progName << " pkg info <name>       Show package details\n";
}

std::string readSourceFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        throw std::runtime_error("Could not open file: " + filename);
    }
    std::stringstream buffer;
    buffer << file.rdbuf();
    file.close();
    return buffer.str();
}

const char* tokenTypeToString(omscript::TokenType type) {
    switch (type) {
    case omscript::TokenType::INTEGER:
        return "INTEGER";
    case omscript::TokenType::FLOAT:
        return "FLOAT";
    case omscript::TokenType::STRING:
        return "STRING";
    case omscript::TokenType::IDENTIFIER:
        return "IDENTIFIER";
    case omscript::TokenType::FN:
        return "FN";
    case omscript::TokenType::RETURN:
        return "RETURN";
    case omscript::TokenType::IF:
        return "IF";
    case omscript::TokenType::ELSE:
        return "ELSE";
    case omscript::TokenType::WHILE:
        return "WHILE";
    case omscript::TokenType::DO:
        return "DO";
    case omscript::TokenType::FOR:
        return "FOR";
    case omscript::TokenType::VAR:
        return "VAR";
    case omscript::TokenType::CONST:
        return "CONST";
    case omscript::TokenType::BREAK:
        return "BREAK";
    case omscript::TokenType::CONTINUE:
        return "CONTINUE";
    case omscript::TokenType::IN:
        return "IN";
    case omscript::TokenType::TRUE:
        return "TRUE";
    case omscript::TokenType::FALSE:
        return "FALSE";
    case omscript::TokenType::NULL_LITERAL:
        return "NULL_LITERAL";
    case omscript::TokenType::OPTMAX_START:
        return "OPTMAX_START";
    case omscript::TokenType::OPTMAX_END:
        return "OPTMAX_END";
    case omscript::TokenType::SWITCH:
        return "SWITCH";
    case omscript::TokenType::CASE:
        return "CASE";
    case omscript::TokenType::DEFAULT:
        return "DEFAULT";
    case omscript::TokenType::PLUS:
        return "PLUS";
    case omscript::TokenType::MINUS:
        return "MINUS";
    case omscript::TokenType::STAR:
        return "STAR";
    case omscript::TokenType::STAR_STAR:
        return "STAR_STAR";
    case omscript::TokenType::SLASH:
        return "SLASH";
    case omscript::TokenType::PERCENT:
        return "PERCENT";
    case omscript::TokenType::ASSIGN:
        return "ASSIGN";
    case omscript::TokenType::EQ:
        return "EQ";
    case omscript::TokenType::NE:
        return "NE";
    case omscript::TokenType::LT:
        return "LT";
    case omscript::TokenType::LE:
        return "LE";
    case omscript::TokenType::GT:
        return "GT";
    case omscript::TokenType::GE:
        return "GE";
    case omscript::TokenType::AND:
        return "AND";
    case omscript::TokenType::OR:
        return "OR";
    case omscript::TokenType::NOT:
        return "NOT";
    case omscript::TokenType::PLUSPLUS:
        return "PLUSPLUS";
    case omscript::TokenType::MINUSMINUS:
        return "MINUSMINUS";
    case omscript::TokenType::PLUS_ASSIGN:
        return "PLUS_ASSIGN";
    case omscript::TokenType::MINUS_ASSIGN:
        return "MINUS_ASSIGN";
    case omscript::TokenType::STAR_ASSIGN:
        return "STAR_ASSIGN";
    case omscript::TokenType::SLASH_ASSIGN:
        return "SLASH_ASSIGN";
    case omscript::TokenType::PERCENT_ASSIGN:
        return "PERCENT_ASSIGN";
    case omscript::TokenType::AMPERSAND_ASSIGN:
        return "AMPERSAND_ASSIGN";
    case omscript::TokenType::PIPE_ASSIGN:
        return "PIPE_ASSIGN";
    case omscript::TokenType::CARET_ASSIGN:
        return "CARET_ASSIGN";
    case omscript::TokenType::LSHIFT_ASSIGN:
        return "LSHIFT_ASSIGN";
    case omscript::TokenType::RSHIFT_ASSIGN:
        return "RSHIFT_ASSIGN";
    case omscript::TokenType::QUESTION:
        return "QUESTION";
    case omscript::TokenType::AMPERSAND:
        return "AMPERSAND";
    case omscript::TokenType::PIPE:
        return "PIPE";
    case omscript::TokenType::CARET:
        return "CARET";
    case omscript::TokenType::TILDE:
        return "TILDE";
    case omscript::TokenType::LSHIFT:
        return "LSHIFT";
    case omscript::TokenType::RSHIFT:
        return "RSHIFT";
    case omscript::TokenType::RANGE:
        return "RANGE";
    case omscript::TokenType::ARROW:
        return "ARROW";
    case omscript::TokenType::LPAREN:
        return "LPAREN";
    case omscript::TokenType::RPAREN:
        return "RPAREN";
    case omscript::TokenType::LBRACE:
        return "LBRACE";
    case omscript::TokenType::RBRACE:
        return "RBRACE";
    case omscript::TokenType::LBRACKET:
        return "LBRACKET";
    case omscript::TokenType::RBRACKET:
        return "RBRACKET";
    case omscript::TokenType::SEMICOLON:
        return "SEMICOLON";
    case omscript::TokenType::COMMA:
        return "COMMA";
    case omscript::TokenType::COLON:
        return "COLON";
    case omscript::TokenType::DOT:
        return "DOT";
    case omscript::TokenType::END_OF_FILE:
        return "END_OF_FILE";
    case omscript::TokenType::INVALID:
        return "INVALID";
    }
    return "UNKNOWN";
}

void printTokens(const std::vector<omscript::Token>& tokens) {
    for (const auto& token : tokens) {
        std::cout << token.line << ":" << token.column << " " << tokenTypeToString(token.type);
        if (!token.lexeme.empty()) {
            std::cout << " '" << token.lexeme << "'";
        }
        std::cout << "\n";
    }
}

void printProgramSummary(const omscript::Program* program) {
    std::cout << "Parsed program with " << program->functions.size() << " function(s).\n";
    if (program->functions.empty()) {
        return;
    }
    std::cout << "Functions:\n";
    for (const auto& fn : program->functions) {
        std::cout << "  " << fn->name << "(";
        for (size_t i = 0; i < fn->parameters.size(); ++i) {
            const auto& param = fn->parameters[i];
            std::cout << param.name;
            if (!param.typeName.empty()) {
                std::cout << ": " << param.typeName;
            }
            if (i + 1 < fn->parameters.size()) {
                std::cout << ", ";
            }
        }
        std::cout << ")";
        if (fn->isOptMax) {
            std::cout << " [OPTMAX]";
        }
        std::cout << "\n";
    }
}

// ---------------------------------------------------------------------------
// AST dump helpers — recursive pretty-printer for --dump-ast
// ---------------------------------------------------------------------------

void dumpExpression(const omscript::Expression* expr, int indent);
void dumpStatement(const omscript::Statement* stmt, int indent);

void printIndent(int indent) {
    for (int i = 0; i < indent; ++i) {
        std::cout << "  ";
    }
}

void dumpExpression(const omscript::Expression* expr, int indent) {
    if (!expr) {
        printIndent(indent);
        std::cout << "(null)\n";
        return;
    }
    printIndent(indent);
    switch (expr->type) {
    case omscript::ASTNodeType::LITERAL_EXPR: {
        auto* lit = static_cast<const omscript::LiteralExpr*>(expr);
        std::cout << "LiteralExpr";
        if (lit->literalType == omscript::LiteralExpr::LiteralType::INTEGER) {
            std::cout << " int=" << lit->intValue;
        } else if (lit->literalType == omscript::LiteralExpr::LiteralType::FLOAT) {
            std::cout << " float=" << lit->floatValue;
        } else {
            std::cout << " str=\"" << lit->stringValue << "\"";
        }
        std::cout << "\n";
        break;
    }
    case omscript::ASTNodeType::IDENTIFIER_EXPR: {
        auto* id = static_cast<const omscript::IdentifierExpr*>(expr);
        std::cout << "IdentifierExpr '" << id->name << "'\n";
        break;
    }
    case omscript::ASTNodeType::BINARY_EXPR: {
        auto* bin = static_cast<const omscript::BinaryExpr*>(expr);
        std::cout << "BinaryExpr op='" << bin->op << "'\n";
        dumpExpression(bin->left.get(), indent + 1);
        dumpExpression(bin->right.get(), indent + 1);
        break;
    }
    case omscript::ASTNodeType::UNARY_EXPR: {
        auto* un = static_cast<const omscript::UnaryExpr*>(expr);
        std::cout << "UnaryExpr op='" << un->op << "'\n";
        dumpExpression(un->operand.get(), indent + 1);
        break;
    }
    case omscript::ASTNodeType::CALL_EXPR: {
        auto* call = static_cast<const omscript::CallExpr*>(expr);
        std::cout << "CallExpr '" << call->callee << "' args=" << call->arguments.size() << "\n";
        for (const auto& arg : call->arguments) {
            dumpExpression(arg.get(), indent + 1);
        }
        break;
    }
    case omscript::ASTNodeType::ASSIGN_EXPR: {
        auto* assign = static_cast<const omscript::AssignExpr*>(expr);
        std::cout << "AssignExpr '" << assign->name << "'\n";
        dumpExpression(assign->value.get(), indent + 1);
        break;
    }
    case omscript::ASTNodeType::POSTFIX_EXPR: {
        auto* pf = static_cast<const omscript::PostfixExpr*>(expr);
        std::cout << "PostfixExpr op='" << pf->op << "'\n";
        dumpExpression(pf->operand.get(), indent + 1);
        break;
    }
    case omscript::ASTNodeType::PREFIX_EXPR: {
        auto* pf = static_cast<const omscript::PrefixExpr*>(expr);
        std::cout << "PrefixExpr op='" << pf->op << "'\n";
        dumpExpression(pf->operand.get(), indent + 1);
        break;
    }
    case omscript::ASTNodeType::TERNARY_EXPR: {
        auto* tern = static_cast<const omscript::TernaryExpr*>(expr);
        std::cout << "TernaryExpr\n";
        printIndent(indent + 1);
        std::cout << "condition:\n";
        dumpExpression(tern->condition.get(), indent + 2);
        printIndent(indent + 1);
        std::cout << "then:\n";
        dumpExpression(tern->thenExpr.get(), indent + 2);
        printIndent(indent + 1);
        std::cout << "else:\n";
        dumpExpression(tern->elseExpr.get(), indent + 2);
        break;
    }
    case omscript::ASTNodeType::ARRAY_EXPR: {
        auto* arr = static_cast<const omscript::ArrayExpr*>(expr);
        std::cout << "ArrayExpr elements=" << arr->elements.size() << "\n";
        for (const auto& el : arr->elements) {
            dumpExpression(el.get(), indent + 1);
        }
        break;
    }
    case omscript::ASTNodeType::INDEX_EXPR: {
        auto* idx = static_cast<const omscript::IndexExpr*>(expr);
        std::cout << "IndexExpr\n";
        dumpExpression(idx->array.get(), indent + 1);
        dumpExpression(idx->index.get(), indent + 1);
        break;
    }
    case omscript::ASTNodeType::INDEX_ASSIGN_EXPR: {
        auto* ia = static_cast<const omscript::IndexAssignExpr*>(expr);
        std::cout << "IndexAssignExpr\n";
        dumpExpression(ia->array.get(), indent + 1);
        dumpExpression(ia->index.get(), indent + 1);
        dumpExpression(ia->value.get(), indent + 1);
        break;
    }
    default:
        std::cout << "Expression(unknown)\n";
        break;
    }
}

void dumpStatement(const omscript::Statement* stmt, int indent) {
    if (!stmt) {
        printIndent(indent);
        std::cout << "(null)\n";
        return;
    }
    printIndent(indent);
    switch (stmt->type) {
    case omscript::ASTNodeType::VAR_DECL: {
        auto* vd = static_cast<const omscript::VarDecl*>(stmt);
        std::cout << "VarDecl '" << vd->name << "'";
        if (vd->isConst)
            std::cout << " const";
        if (!vd->typeName.empty())
            std::cout << " type=" << vd->typeName;
        std::cout << "\n";
        if (vd->initializer) {
            dumpExpression(vd->initializer.get(), indent + 1);
        }
        break;
    }
    case omscript::ASTNodeType::RETURN_STMT: {
        auto* ret = static_cast<const omscript::ReturnStmt*>(stmt);
        std::cout << "ReturnStmt\n";
        if (ret->value) {
            dumpExpression(ret->value.get(), indent + 1);
        }
        break;
    }
    case omscript::ASTNodeType::IF_STMT: {
        auto* ifs = static_cast<const omscript::IfStmt*>(stmt);
        std::cout << "IfStmt\n";
        printIndent(indent + 1);
        std::cout << "condition:\n";
        dumpExpression(ifs->condition.get(), indent + 2);
        printIndent(indent + 1);
        std::cout << "then:\n";
        dumpStatement(ifs->thenBranch.get(), indent + 2);
        if (ifs->elseBranch) {
            printIndent(indent + 1);
            std::cout << "else:\n";
            dumpStatement(ifs->elseBranch.get(), indent + 2);
        }
        break;
    }
    case omscript::ASTNodeType::WHILE_STMT: {
        auto* ws = static_cast<const omscript::WhileStmt*>(stmt);
        std::cout << "WhileStmt\n";
        printIndent(indent + 1);
        std::cout << "condition:\n";
        dumpExpression(ws->condition.get(), indent + 2);
        printIndent(indent + 1);
        std::cout << "body:\n";
        dumpStatement(ws->body.get(), indent + 2);
        break;
    }
    case omscript::ASTNodeType::DO_WHILE_STMT: {
        auto* dw = static_cast<const omscript::DoWhileStmt*>(stmt);
        std::cout << "DoWhileStmt\n";
        printIndent(indent + 1);
        std::cout << "body:\n";
        dumpStatement(dw->body.get(), indent + 2);
        printIndent(indent + 1);
        std::cout << "condition:\n";
        dumpExpression(dw->condition.get(), indent + 2);
        break;
    }
    case omscript::ASTNodeType::FOR_STMT: {
        auto* fs = static_cast<const omscript::ForStmt*>(stmt);
        std::cout << "ForStmt iter='" << fs->iteratorVar << "'";
        if (!fs->iteratorType.empty())
            std::cout << " type=" << fs->iteratorType;
        std::cout << "\n";
        printIndent(indent + 1);
        std::cout << "start:\n";
        dumpExpression(fs->start.get(), indent + 2);
        printIndent(indent + 1);
        std::cout << "end:\n";
        dumpExpression(fs->end.get(), indent + 2);
        if (fs->step) {
            printIndent(indent + 1);
            std::cout << "step:\n";
            dumpExpression(fs->step.get(), indent + 2);
        }
        printIndent(indent + 1);
        std::cout << "body:\n";
        dumpStatement(fs->body.get(), indent + 2);
        break;
    }
    case omscript::ASTNodeType::FOR_EACH_STMT: {
        auto* fe = static_cast<const omscript::ForEachStmt*>(stmt);
        std::cout << "ForEachStmt iter='" << fe->iteratorVar << "'\n";
        printIndent(indent + 1);
        std::cout << "collection:\n";
        dumpExpression(fe->collection.get(), indent + 2);
        printIndent(indent + 1);
        std::cout << "body:\n";
        dumpStatement(fe->body.get(), indent + 2);
        break;
    }
    case omscript::ASTNodeType::BREAK_STMT:
        std::cout << "BreakStmt\n";
        break;
    case omscript::ASTNodeType::CONTINUE_STMT:
        std::cout << "ContinueStmt\n";
        break;
    case omscript::ASTNodeType::SWITCH_STMT: {
        auto* sw = static_cast<const omscript::SwitchStmt*>(stmt);
        std::cout << "SwitchStmt cases=" << sw->cases.size() << "\n";
        printIndent(indent + 1);
        std::cout << "condition:\n";
        dumpExpression(sw->condition.get(), indent + 2);
        for (size_t ci = 0; ci < sw->cases.size(); ++ci) {
            const auto& sc = sw->cases[ci];
            printIndent(indent + 1);
            if (sc.isDefault) {
                std::cout << "default:\n";
            } else {
                std::cout << "case:\n";
                dumpExpression(sc.value.get(), indent + 2);
            }
            for (const auto& s : sc.body) {
                dumpStatement(s.get(), indent + 2);
            }
        }
        break;
    }
    case omscript::ASTNodeType::BLOCK: {
        auto* block = static_cast<const omscript::BlockStmt*>(stmt);
        std::cout << "Block stmts=" << block->statements.size() << "\n";
        for (const auto& s : block->statements) {
            dumpStatement(s.get(), indent + 1);
        }
        break;
    }
    case omscript::ASTNodeType::EXPR_STMT: {
        auto* es = static_cast<const omscript::ExprStmt*>(stmt);
        std::cout << "ExprStmt\n";
        dumpExpression(es->expression.get(), indent + 1);
        break;
    }
    default:
        std::cout << "Statement(unknown)\n";
        break;
    }
}

void dumpAST(const omscript::Program* program) {
    std::cout << "Program functions=" << program->functions.size() << "\n";
    for (const auto& fn : program->functions) {
        std::cout << "  FunctionDecl '" << fn->name << "' params=" << fn->parameters.size();
        if (fn->isOptMax)
            std::cout << " [OPTMAX]";
        std::cout << "\n";
        for (const auto& p : fn->parameters) {
            std::cout << "    Param '" << p.name << "'";
            if (!p.typeName.empty())
                std::cout << " type=" << p.typeName;
            std::cout << "\n";
        }
        if (fn->body) {
            dumpStatement(fn->body.get(), 2);
        }
    }
}

} // namespace

int main(int argc, char* argv[]) {
    // Install signal handlers for graceful cleanup of temporary files.
    std::signal(SIGINT, signalHandler);
    std::signal(SIGTERM, signalHandler);

    // Add to PATH on first run if needed
    ensureInPath();

    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    enum class Command { Compile, Run, Lex, Parse, EmitIR, Clean, Help, Version, Install, Uninstall, Check, Pkg };

    int argIndex = 1;
    bool verbose = false;
    bool quiet = false;
    bool showTiming = false;
    bool dumpAstFull = false;
    bool emitObjOnly = false;
    bool dryRun = false;
    omscript::OptimizationLevel optLevel = omscript::OptimizationLevel::O2;
    std::string marchCpu;
    std::string mtuneCpu;
    bool flagLTO = false;
    bool flagPIC = true;
    bool flagFastMath = false;
    bool flagOptMax = true;
    bool flagJIT = true;
    bool flagStatic = false;
    bool flagStrip = false;
    bool flagStackProtector = false;
    const auto tryParseOptimizationFlag = [](const std::string& arg) -> std::optional<omscript::OptimizationLevel> {
        if (arg == "-Ofast") {
            return omscript::OptimizationLevel::O3;
        }
        if (arg.size() == 3 && arg[0] == '-' && arg[1] == 'O' && arg[2] >= '0' && arg[2] <= '3') {
            static constexpr omscript::OptimizationLevel levels[] = {
                omscript::OptimizationLevel::O0, omscript::OptimizationLevel::O1, omscript::OptimizationLevel::O2,
                omscript::OptimizationLevel::O3};
            return levels[arg[2] - '0'];
        }
        return std::nullopt;
    };

    // Try to parse -march=<cpu>, -mtune=<cpu>, and feature flags.
    // Returns true if the argument was consumed.
    const auto tryParseTargetOrFeatureFlag = [&](const std::string& arg) -> bool {
        if (arg.rfind("-march=", 0) == 0) {
            marchCpu = arg.substr(7);
            return true;
        }
        if (arg.rfind("-mtune=", 0) == 0) {
            mtuneCpu = arg.substr(7);
            return true;
        }
        if (arg == "-flto") {
            flagLTO = true;
            return true;
        }
        if (arg == "-fno-lto") {
            flagLTO = false;
            return true;
        }
        if (arg == "-fpic") {
            flagPIC = true;
            return true;
        }
        if (arg == "-fno-pic") {
            flagPIC = false;
            return true;
        }
        if (arg == "-ffast-math") {
            flagFastMath = true;
            return true;
        }
        if (arg == "-fno-fast-math") {
            flagFastMath = false;
            return true;
        }
        if (arg == "-foptmax") {
            flagOptMax = true;
            return true;
        }
        if (arg == "-fno-optmax") {
            flagOptMax = false;
            return true;
        }
        if (arg == "-fjit") {
            flagJIT = true;
            return true;
        }
        if (arg == "-fno-jit") {
            flagJIT = false;
            return true;
        }
        if (arg == "-fstack-protector") {
            flagStackProtector = true;
            return true;
        }
        if (arg == "-fno-stack-protector") {
            flagStackProtector = false;
            return true;
        }
        if (arg == "-static") {
            flagStatic = true;
            return true;
        }
        if (arg == "-s" || arg == "--strip") {
            flagStrip = true;
            return true;
        }
        return false;
    };

    // Allow global options before commands/input (e.g. `omsc -V parse file.om`).
    while (argIndex < argc) {
        std::string arg = argv[argIndex];
        if (arg == "-V" || arg == "--verbose") {
            verbose = true;
            argIndex++;
            continue;
        }
        if (arg == "-q" || arg == "--quiet") {
            quiet = true;
            argIndex++;
            continue;
        }
        if (arg == "--time") {
            showTiming = true;
            argIndex++;
            continue;
        }
        if (arg == "--dump-ast") {
            dumpAstFull = true;
            argIndex++;
            continue;
        }
        if (arg == "--emit-obj") {
            emitObjOnly = true;
            argIndex++;
            continue;
        }
        if (arg == "--dry-run") {
            dryRun = true;
            argIndex++;
            continue;
        }
        if (auto parsedOpt = tryParseOptimizationFlag(arg)) {
            optLevel = *parsedOpt;
            argIndex++;
            continue;
        }
        if (tryParseTargetOrFeatureFlag(arg)) {
            argIndex++;
            continue;
        }
        break;
    }

    std::string firstArg = argIndex < argc ? argv[argIndex] : "";
    if (firstArg.empty()) {
        std::cerr << "Error: no input file specified (run '" << argv[0] << " --help' for usage)\n";
        return 1;
    }
    Command command = Command::Compile;
    bool commandMatched = false;
    if (firstArg == "help" || firstArg == "-h" || firstArg == "--help") {
        command = Command::Help;
        commandMatched = true;
    } else if (firstArg == "version" || firstArg == "-v" || firstArg == "--version") {
        command = Command::Version;
        commandMatched = true;
    } else if (firstArg == "install" || firstArg == "update" || firstArg == "--install" || firstArg == "--update") {
        command = Command::Install;
        commandMatched = true;
    } else if (firstArg == "uninstall" || firstArg == "--uninstall") {
        command = Command::Uninstall;
        commandMatched = true;
    } else if (firstArg == "pkg" || firstArg == "--pkg" || firstArg == "package") {
        command = Command::Pkg;
        argIndex++;
        commandMatched = true;
    } else if (firstArg == "compile" || firstArg == "build" || firstArg == "-c" || firstArg == "-b" ||
               firstArg == "--compile" || firstArg == "--build") {
        command = Command::Compile;
        argIndex++;
        commandMatched = true;
    } else if (firstArg == "run" || firstArg == "-r" || firstArg == "--run") {
        command = Command::Run;
        argIndex++;
        commandMatched = true;
    } else if (firstArg == "check" || firstArg == "--check") {
        command = Command::Check;
        argIndex++;
        commandMatched = true;
    } else if (firstArg == "lex" || firstArg == "tokens" || firstArg == "-l" || firstArg == "--lex" ||
               firstArg == "--tokens" || firstArg == "--dump-tokens") {
        command = Command::Lex;
        argIndex++;
        commandMatched = true;
    } else if (firstArg == "parse" || firstArg == "emit-ast" || firstArg == "-p" || firstArg == "-a" ||
               firstArg == "--parse" || firstArg == "--ast" || firstArg == "--emit-ast") {
        command = Command::Parse;
        argIndex++;
        commandMatched = true;
    } else if (firstArg == "emit-ir" || firstArg == "-e" || firstArg == "-i" || firstArg == "--emit-ir" ||
               firstArg == "--ir") {
        command = Command::EmitIR;
        argIndex++;
        commandMatched = true;
    } else if (firstArg == "clean" || firstArg == "-C" || firstArg == "--clean") {
        command = Command::Clean;
        argIndex++;
        commandMatched = true;
    }

    if (!commandMatched && !firstArg.empty() && firstArg[0] != '-') {
        bool hasOmExtension = firstArg.size() >= 3 && firstArg.substr(firstArg.size() - 3) == ".om";
        if (!hasOmExtension && !std::filesystem::exists(firstArg)) {
            std::cerr << "Error: unknown command '" << firstArg << "'\n";
            printUsage(argv[0]);
            return 1;
        }
    }


    if (command == Command::Help) {
        printUsage(argv[0]);
        return 0;
    }
    if (command == Command::Version) {
        std::cout << kCompilerVersion << "\n";
        return 0;
    }
    if (command == Command::Install) {
        doInstall();
        return 0;
    }
    if (command == Command::Uninstall) {
        doUninstall();
        return 0;
    }
    if (command == Command::Pkg) {
        return doPkg(argc, argv, argIndex, quiet);
    }

    std::string sourceFile;
    std::string outputFile = command == Command::EmitIR ? "" : "a.out";
    bool outputSpecified = false;
    bool supportsOutputOption = command == Command::Compile || command == Command::Run || command == Command::EmitIR ||
                                command == Command::Clean;
    bool parsingRunArgs = false;
    bool keepTemps = false;
    std::vector<std::string> runArgs;

    // Parse command line arguments
    for (int i = argIndex; i < argc; i++) {
        std::string arg = argv[i];
        if (command == Command::Run && arg == "--") {
            parsingRunArgs = true;
            continue;
        }
        if (!parsingRunArgs && (arg == "-h" || arg == "--help")) {
            printUsage(argv[0]);
            return 0;
        }
        if (!parsingRunArgs && (arg == "-v" || arg == "--version")) {
            std::cout << kCompilerVersion << "\n";
            return 0;
        }
        if (!parsingRunArgs && (arg == "-k" || arg == "--keep-temps")) {
            if (command != Command::Run) {
                std::cerr << "Error: -k/--keep-temps is only supported for run commands\n";
                return 1;
            }
            keepTemps = true;
            continue;
        }
        if (!parsingRunArgs && (arg == "-V" || arg == "--verbose")) {
            verbose = true;
            continue;
        }
        if (!parsingRunArgs && (arg == "-q" || arg == "--quiet")) {
            quiet = true;
            continue;
        }
        if (!parsingRunArgs && arg == "--time") {
            showTiming = true;
            continue;
        }
        if (!parsingRunArgs && arg == "--dump-ast") {
            dumpAstFull = true;
            continue;
        }
        if (!parsingRunArgs && arg == "--emit-obj") {
            emitObjOnly = true;
            continue;
        }
        if (!parsingRunArgs && arg == "--dry-run") {
            dryRun = true;
            continue;
        }
        if (!parsingRunArgs) {
            if (auto parsedOpt = tryParseOptimizationFlag(arg)) {
                optLevel = *parsedOpt;
                continue;
            }
        }
        if (!parsingRunArgs && tryParseTargetOrFeatureFlag(arg)) {
            continue;
        }
        if (!parsingRunArgs && (arg == "-o" || arg == "--output")) {
            if (!supportsOutputOption) {
                std::cerr << "Error: -o/--output is only supported for compile/run/emit-ir/clean commands\n";
                return 1;
            }
            if (outputSpecified) {
                std::cerr << "Error: output file specified multiple times\n";
                return 1;
            }
            if (i + 1 < argc) {
                const char* nextArg = argv[i + 1];
                if (nextArg[0] == '\0' || nextArg[0] == '-') {
                    std::cerr << "Error: -o/--output requires a valid output file name\n";
                    return 1;
                }
                outputFile = argv[++i];
                outputSpecified = true;
            } else {
                std::cerr << "Error: -o/--output requires an argument\n";
                return 1;
            }
        } else if (!parsingRunArgs && !arg.empty() && arg[0] == '-') {
            std::cerr << "Error: unknown option '" << arg << "'\n";
            return 1;
        } else if (command == Command::Clean) {
            std::cerr << "Error: clean does not accept input files (got '" << arg << "')\n";
            return 1;
        } else if (sourceFile.empty()) {
            sourceFile = arg;
        } else if (command == Command::Run && parsingRunArgs) {
            runArgs.push_back(arg);
        } else {
            std::cerr << "Error: multiple input files specified ('" << sourceFile << "' and '" << arg << "')\n";
            return 1;
        }
    }

    if (command == Command::Clean) {
        bool removedAny = false;
        auto removeIfPresent = [&](const std::string& path) {
            std::error_code ec;
            if (!std::filesystem::exists(path, ec)) {
                return;
            }
            if (std::filesystem::remove(path, ec)) {
                removedAny = true;
                return;
            }
            if (ec) {
                std::cerr << "Warning: failed to remove '" << path << "': " << ec.message() << "\n";
            }
        };
        removeIfPresent(outputFile);
        removeIfPresent(outputFile + ".o");
        if (removedAny) {
            std::cout << "Cleaned outputs for " << outputFile << "\n";
        } else {
            std::cout << "Nothing to clean for " << outputFile << "\n";
        }
        return 0;
    }

    if (sourceFile.empty()) {
        std::cerr << "Error: no input file specified\n";
        printUsage(argv[0]);
        return 1;
    }

    try {
        auto totalStart = std::chrono::steady_clock::now();

        if (command == Command::Lex || command == Command::Parse || command == Command::EmitIR ||
            command == Command::Check) {
            auto lexStart = std::chrono::steady_clock::now();
            std::string source = readSourceFile(sourceFile);
            omscript::Lexer lexer(source);
            auto tokens = lexer.tokenize();
            auto lexEnd = std::chrono::steady_clock::now();

            if (command == Command::Lex) {
                printTokens(tokens);
                if (showTiming) {
                    auto lexMs =
                        std::chrono::duration_cast<std::chrono::microseconds>(lexEnd - lexStart).count() / 1000.0;
                    std::cerr << "Timing: lex " << lexMs << "ms\n";
                }
                return 0;
            }

            auto parseStart = std::chrono::steady_clock::now();
            omscript::Parser parser(tokens);
            auto program = parser.parse();
            auto parseEnd = std::chrono::steady_clock::now();

            if (command == Command::Check) {
                if (!quiet) {
                    std::cout << sourceFile << ": OK (" << program->functions.size() << " function(s))\n";
                }
                if (showTiming) {
                    auto lexMs =
                        std::chrono::duration_cast<std::chrono::microseconds>(lexEnd - lexStart).count() / 1000.0;
                    auto parseMs =
                        std::chrono::duration_cast<std::chrono::microseconds>(parseEnd - parseStart).count() / 1000.0;
                    auto totalMs = std::chrono::duration_cast<std::chrono::microseconds>(
                                       std::chrono::steady_clock::now() - totalStart)
                                       .count() /
                                   1000.0;
                    std::cerr << "Timing: lex " << lexMs << "ms, parse " << parseMs << "ms, total " << totalMs
                              << "ms\n";
                }
                return 0;
            }

            if (command == Command::Parse) {
                if (dumpAstFull) {
                    dumpAST(program.get());
                } else {
                    printProgramSummary(program.get());
                }
                if (showTiming) {
                    auto lexMs =
                        std::chrono::duration_cast<std::chrono::microseconds>(lexEnd - lexStart).count() / 1000.0;
                    auto parseMs =
                        std::chrono::duration_cast<std::chrono::microseconds>(parseEnd - parseStart).count() / 1000.0;
                    std::cerr << "Timing: lex " << lexMs << "ms, parse " << parseMs << "ms\n";
                }
                return 0;
            }

            // EmitIR
            auto codegenStart = std::chrono::steady_clock::now();
            omscript::CodeGenerator codegen(optLevel);
            codegen.generate(program.get());
            auto codegenEnd = std::chrono::steady_clock::now();

            if (!dryRun) {
                if (outputFile.empty()) {
                    codegen.getModule()->print(llvm::outs(), nullptr);
                    llvm::outs().flush();
                } else {
                    std::error_code ec;
                    llvm::raw_fd_ostream out(outputFile, ec, llvm::sys::fs::OF_None);
                    if (ec) {
                        throw std::runtime_error("Could not write IR to file: " + ec.message());
                    }
                    codegen.getModule()->print(out, nullptr);
                }
            } else if (!quiet) {
                std::cout << "Dry run: IR generation successful for " << sourceFile << "\n";
            }
            if (showTiming) {
                auto lexMs = std::chrono::duration_cast<std::chrono::microseconds>(lexEnd - lexStart).count() / 1000.0;
                auto parseMs =
                    std::chrono::duration_cast<std::chrono::microseconds>(parseEnd - parseStart).count() / 1000.0;
                auto codegenMs =
                    std::chrono::duration_cast<std::chrono::microseconds>(codegenEnd - codegenStart).count() / 1000.0;
                std::cerr << "Timing: lex " << lexMs << "ms, parse " << parseMs << "ms, codegen " << codegenMs
                          << "ms\n";
            }
            return 0;
        }

        if (dryRun) {
            // For compile/run with --dry-run: lex, parse, codegen but don't write files.
            auto lexStart = std::chrono::steady_clock::now();
            std::string source = readSourceFile(sourceFile);
            omscript::Lexer lexer(source);
            auto tokens = lexer.tokenize();
            auto lexEnd = std::chrono::steady_clock::now();

            auto parseStart = std::chrono::steady_clock::now();
            omscript::Parser parser(tokens);
            auto program = parser.parse();
            auto parseEnd = std::chrono::steady_clock::now();

            auto codegenStart = std::chrono::steady_clock::now();
            omscript::CodeGenerator codegen(optLevel);
            codegen.setMarch(marchCpu);
            codegen.setMtune(mtuneCpu);
            codegen.setPIC(flagPIC);
            codegen.setFastMath(flagFastMath);
            codegen.setOptMax(flagOptMax);
            if (flagJIT) {
                codegen.generateHybrid(program.get());
            } else {
                codegen.generate(program.get());
            }
            auto codegenEnd = std::chrono::steady_clock::now();

            if (!quiet) {
                std::cout << "Dry run: compilation successful for " << sourceFile << "\n";
            }
            if (showTiming) {
                auto lexMs = std::chrono::duration_cast<std::chrono::microseconds>(lexEnd - lexStart).count() / 1000.0;
                auto parseMs =
                    std::chrono::duration_cast<std::chrono::microseconds>(parseEnd - parseStart).count() / 1000.0;
                auto codegenMs =
                    std::chrono::duration_cast<std::chrono::microseconds>(codegenEnd - codegenStart).count() / 1000.0;
                auto totalMs =
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - totalStart)
                        .count() /
                    1000.0;
                std::cerr << "Timing: lex " << lexMs << "ms, parse " << parseMs << "ms, codegen " << codegenMs
                          << "ms, total " << totalMs << "ms\n";
            }
            return 0;
        }

        if (emitObjOnly) {
            // Compile to object file only (no linking).
            std::string source = readSourceFile(sourceFile);
            omscript::Lexer lexer(source);
            auto tokens = lexer.tokenize();
            omscript::Parser parser(tokens);
            auto program = parser.parse();

            omscript::CodeGenerator codegen(optLevel);
            codegen.setMarch(marchCpu);
            codegen.setMtune(mtuneCpu);
            codegen.setPIC(flagPIC);
            codegen.setFastMath(flagFastMath);
            codegen.setOptMax(flagOptMax);
            if (flagJIT) {
                codegen.generateHybrid(program.get());
            } else {
                codegen.generate(program.get());
            }

            std::string objFile =
                outputSpecified ? outputFile : (std::filesystem::path(sourceFile).stem().string() + ".o");
            codegen.writeObjectFile(objFile);
            if (!quiet) {
                std::cout << "Object file written: " << objFile << "\n";
            }
            if (showTiming) {
                auto totalMs =
                    std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - totalStart)
                        .count() /
                    1000.0;
                std::cerr << "Timing: total " << totalMs << "ms\n";
            }
            return 0;
        }

        omscript::Compiler compiler;
        compiler.setVerbose(verbose);
        compiler.setOptimizationLevel(optLevel);
        compiler.setMarch(marchCpu);
        compiler.setMtune(mtuneCpu);
        compiler.setLTO(flagLTO);
        compiler.setPIC(flagPIC);
        compiler.setFastMath(flagFastMath);
        compiler.setOptMax(flagOptMax);
        compiler.setJIT(flagJIT);
        compiler.setStaticLinking(flagStatic);
        compiler.setStrip(flagStrip);
        compiler.setStackProtector(flagStackProtector);
        if (quiet) {
            compiler.setVerbose(false);
        }
        compiler.compile(sourceFile, outputFile);

        if (showTiming) {
            auto totalMs =
                std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - totalStart)
                    .count() /
                1000.0;
            std::cerr << "Timing: total " << totalMs << "ms\n";
        }

        if (command == Command::Run) {
            // Register temp files for cleanup on signal (Ctrl+C during program run).
            if (!outputSpecified && !keepTemps) {
                std::string objPath = outputFile + ".o";
                std::strncpy(g_tempOutputFile, outputFile.c_str(), kMaxTempPathLen - 1);
                g_tempOutputFile[kMaxTempPathLen - 1] = '\0';
                std::strncpy(g_tempObjectFile, objPath.c_str(), kMaxTempPathLen - 1);
                g_tempObjectFile[kMaxTempPathLen - 1] = '\0';
            }
            std::filesystem::path runPath = std::filesystem::absolute(outputFile);
            std::string runProgram = runPath.string();
            llvm::SmallVector<llvm::StringRef, 8> argRefs;
            argRefs.push_back(runProgram);
            for (const auto& arg : runArgs) {
                argRefs.push_back(arg);
            }
            int result = llvm::sys::ExecuteAndWait(runProgram, argRefs);
            if (result < 0) {
                std::cerr << "Error: program terminated by signal " << (-result) << "\n";
                // Clean up temp files even on signal failure.
                if (!outputSpecified && !keepTemps) {
                    std::error_code ec;
                    std::filesystem::remove(outputFile, ec);
                    std::filesystem::remove(outputFile + ".o", ec);
                }
                return 128 + (-result); // Follow shell convention for signal exits
            }
            if (result != 0) {
                std::cout << "Program exited with code " << result << "\n";
            }
            if (!outputSpecified && !keepTemps) {
                std::error_code ec;
                std::filesystem::remove(outputFile, ec);
                if (ec) {
                    std::cerr << "Warning: failed to remove temporary output file '" << outputFile
                              << "': " << ec.message() << "\n";
                }
                std::filesystem::remove(outputFile + ".o", ec);
                if (ec) {
                    std::cerr << "Warning: failed to remove temporary object file '" << outputFile
                              << ".o': " << ec.message() << "\n";
                }
            }
            return result;
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }
}
