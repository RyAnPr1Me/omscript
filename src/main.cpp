#include "codegen.h"
#include "compiler.h"
#include "lexer.h"
#include "parser.h"
#include <csignal>
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
#include <unistd.h>
#include <vector>

namespace {

constexpr const char* kCompilerVersion = "OmScript Compiler v" OMSC_VERSION;
constexpr const char* kPathConfigMarker = "# omsc-path-auto";
constexpr const char* kGitHubReleasesApiUrl = "https://api.github.com/repos/RyAnPr1Me/omscript/releases/latest";
constexpr const char* kGitHubReleasesDownloadBase = "https://github.com/RyAnPr1Me/omscript/releases/download";
constexpr int kApiTimeoutSeconds = 10;
constexpr int kDownloadTimeoutSeconds = 120;

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

    // Create a secure temporary file using mkstemp.
    std::string tmpTemplate = std::filesystem::temp_directory_path().string() + "/omsc_release_XXXXXX";
    std::vector<char> tmpBuf(tmpTemplate.begin(), tmpTemplate.end());
    tmpBuf.push_back('\0');
    int fd = mkstemp(tmpBuf.data());
    if (fd == -1) {
        return "";
    }
    close(fd);
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
#if defined(__aarch64__) || defined(_M_ARM64)
    std::string platformArch = "linux-aarch64";
#else
    std::string platformArch = "linux-x86_64";
#endif

    // Build the download URL: e.g. .../download/v0.9.4/omsc-0.9.4-linux-x86_64.tar.gz
    std::string version = tagName;
    if (!version.empty() && version[0] == 'v') {
        version = version.substr(1);
    }
    std::string assetName = "omsc-" + version + "-" + platformArch + ".tar.gz";
    std::string downloadUrl = std::string(kGitHubReleasesDownloadBase) + "/" + tagName + "/" + assetName;

    // Create a secure temporary file for the tarball using mkstemp.
    std::string tmpBase = std::filesystem::temp_directory_path().string();
    std::string tarTemplate = tmpBase + "/omsc_update_XXXXXX";
    std::vector<char> tarBuf(tarTemplate.begin(), tarTemplate.end());
    tarBuf.push_back('\0');
    int tarFd = mkstemp(tarBuf.data());
    if (tarFd == -1) {
        std::cerr << "Error: failed to create temporary file for download\n";
        return false;
    }
    close(tarFd);
    std::string tmpTarball(tarBuf.data());

    // Create a secure temporary directory for extraction using mkdtemp.
    std::string dirTemplate = tmpBase + "/omsc_extract_XXXXXX";
    std::vector<char> dirBuf(dirTemplate.begin(), dirTemplate.end());
    dirBuf.push_back('\0');
    if (mkdtemp(dirBuf.data()) == nullptr) {
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
    int installTmpFd = mkstemp(installTmpBuf.data());
    if (installTmpFd == -1) {
        std::cerr << "Error: failed to create temporary file for installation in " << installDir << "\n";
        std::filesystem::remove_all(tmpDir, ec);
        return false;
    }
    close(installTmpFd);
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
    return geteuid() == 0;
}

std::string detectDistro() {
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
}

std::string getInstallPrefix(bool system) {
    if (system) {
        return "/usr/local";
    }
    std::string home = getenv("HOME") ? getenv("HOME") : "";
    return home + "/.local";
}

std::string getInstallBinDir(bool system) {
    return getInstallPrefix(system) + "/bin";
}

bool fileExists(const std::string& path) {
    return std::filesystem::exists(path);
}

bool isInPath(const std::string& binDir) {
    std::string pathEnv = getenv("PATH") ? getenv("PATH") : "";
    size_t pos = 0;
    std::string token;
    while ((pos = pathEnv.find(':')) != std::string::npos) {
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
    std::string exePath;
    const char* envPath = getenv("OMSC_BINARY_PATH");
    if (envPath) {
        exePath = envPath;
    } else {
        exePath = std::filesystem::read_symlink("/proc/self/exe").string();
    }

    if (!fileExists(exePath)) {
        std::cerr << "Error: Cannot determine own executable path\n";
        return false;
    }

    if (!fileExists(targetDir)) {
        std::cerr << "Error: Target directory does not exist: " << targetDir << "\n";
        return false;
    }

    std::string targetPath = targetDir + "/omsc";

    if (!force && isSymlinkOrCopy(targetPath, exePath)) {
        std::cout << "OmScript is already installed at " << targetPath << "\n";
        return true;
    }

    // Write to a temp file in the target directory, then atomically rename so
    // the in-place replace is safe even if the copy fails partway through.
    std::string tmpTemplate = targetDir + "/omsc_install_XXXXXX";
    std::vector<char> tmpBuf(tmpTemplate.begin(), tmpTemplate.end());
    tmpBuf.push_back('\0');
    int tmpFd = mkstemp(tmpBuf.data());
    if (tmpFd == -1) {
        std::cerr << "Error: failed to create temporary file in " << targetDir << "\n";
        return false;
    }
    close(tmpFd);
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

    const char* envPath = getenv("OMSC_BINARY_PATH");
    std::string exePath = envPath ? envPath : std::filesystem::read_symlink("/proc/self/exe").string();

    if (!fileExists(exePath)) {
        std::cerr << "Error: Cannot determine own executable path\n";
        return;
    }

    std::string binDir = getInstallBinDir(false);
    std::string userPath = binDir + "/omsc";

    if (isRoot()) {
        std::string sysPath = "/usr/local/bin/omsc";
        if (fileExists(sysPath)) {
            std::cout << "Updating system installation at " << sysPath << "...\n";
        } else {
            std::cout << "Installing to system location " << sysPath << "...\n";
        }
        installToSystem("/usr/local/bin", true);
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
    if (isRoot()) {
        candidates.push_back("/usr/local/bin/omsc");
        candidates.push_back("/usr/bin/omsc");
    } else {
        candidates.push_back(getInstallBinDir(false) + "/omsc");
        candidates.push_back("/usr/local/bin/omsc");
        candidates.push_back("/usr/bin/omsc");
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
    const char* homeDir = getenv("HOME");
    if (!homeDir) {
        return;
    }
    std::vector<std::string> shellConfigs = {std::string(homeDir) + "/.bashrc",
                                             std::string(homeDir) + "/.profile",
                                             std::string(homeDir) + "/.zshrc"};
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
        int cfgTmpFd = mkstemp(cfgTmpBuf.data());
        if (cfgTmpFd == -1) {
            std::cerr << "Warning: could not create temp file to update " << configPath << "\n";
            continue;
        }
        close(cfgTmpFd);
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

    std::cout << "\nOmScript has been uninstalled.\n";
}

void ensureInPath() {
    const char* binaryPath = getenv("OMSC_BINARY_PATH");
    if (!binaryPath) {
        return;
    }

    std::string binaryDir = std::filesystem::path(binaryPath).parent_path();
    std::string exePath = std::filesystem::canonical(binaryPath);

    const char* homeDir = getenv("HOME");
    if (!homeDir) {
        return;
    }
    std::string shellConfig = std::string(homeDir) + "/.bashrc";
    std::ifstream checkConfig(shellConfig);
    if (!checkConfig.is_open()) {
        shellConfig = std::string(homeDir) + "/.profile";
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
        unlink(g_tempOutputFile);
    }
    if (g_tempObjectFile[0] != '\0') {
        unlink(g_tempObjectFile);
    }
    // Re-raise the signal with default action so the exit status reflects it.
    signal(sig, SIG_DFL);
    raise(sig);
}

void printUsage(const char* progName) {
    std::cout << kCompilerVersion << "\n";
    std::cout << "Usage:\n";
    std::cout << "  " << progName << " <source.om> [-o output]\n";
    std::cout << "  " << progName << " compile <source.om> [-o output]\n";
    std::cout << "  " << progName << " run <source.om> [-o output] [-- args...]\n";
    std::cout << "  " << progName << " lex <source.om>\n";
    std::cout << "  " << progName << " parse <source.om>\n";
    std::cout << "  " << progName << " emit-ast <source.om>\n";
    std::cout << "  " << progName << " emit-ir <source.om> [-o output.ll]\n";
    std::cout << "  " << progName << " clean [-o output]\n";
    std::cout << "  " << progName << " version\n";
    std::cout << "  " << progName << " install\n";
    std::cout << "  " << progName << " uninstall\n";
    std::cout << "  " << progName << " help\n";
    std::cout << "\nCommands:\n";
    std::cout << "  -b, -c, --build, --compile  Compile a source file (default)\n";
    std::cout << "  -r, --run            Compile and run a source file\n";
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

    enum class Command { Compile, Run, Lex, Parse, EmitIR, Clean, Help, Version, Install, Uninstall };

    int argIndex = 1;
    bool verbose = false;
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
        if (arg == "-flto") { flagLTO = true; return true; }
        if (arg == "-fno-lto") { flagLTO = false; return true; }
        if (arg == "-fpic") { flagPIC = true; return true; }
        if (arg == "-fno-pic") { flagPIC = false; return true; }
        if (arg == "-ffast-math") { flagFastMath = true; return true; }
        if (arg == "-fno-fast-math") { flagFastMath = false; return true; }
        if (arg == "-foptmax") { flagOptMax = true; return true; }
        if (arg == "-fno-optmax") { flagOptMax = false; return true; }
        if (arg == "-fjit") { flagJIT = true; return true; }
        if (arg == "-fno-jit") { flagJIT = false; return true; }
        if (arg == "-fstack-protector") { flagStackProtector = true; return true; }
        if (arg == "-fno-stack-protector") { flagStackProtector = false; return true; }
        if (arg == "-static") { flagStatic = true; return true; }
        if (arg == "-s" || arg == "--strip") { flagStrip = true; return true; }
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
    } else if (firstArg == "compile" || firstArg == "build" || firstArg == "-c" || firstArg == "-b" ||
               firstArg == "--compile" || firstArg == "--build") {
        command = Command::Compile;
        argIndex++;
        commandMatched = true;
    } else if (firstArg == "run" || firstArg == "-r" || firstArg == "--run") {
        command = Command::Run;
        argIndex++;
        commandMatched = true;
    } else if (firstArg == "lex" || firstArg == "tokens" || firstArg == "-l" || firstArg == "--lex" ||
               firstArg == "--tokens") {
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
        if (command == Command::Lex || command == Command::Parse || command == Command::EmitIR) {
            std::string source = readSourceFile(sourceFile);
            omscript::Lexer lexer(source);
            auto tokens = lexer.tokenize();
            if (command == Command::Lex) {
                printTokens(tokens);
                return 0;
            }
            omscript::Parser parser(tokens);
            auto program = parser.parse();
            if (command == Command::Parse) {
                printProgramSummary(program.get());
                return 0;
            }
            omscript::CodeGenerator codegen(optLevel);
            codegen.generate(program.get());
            if (outputFile.empty()) {
                codegen.getModule()->print(llvm::outs(), nullptr);
                llvm::outs().flush();
                return 0;
            }
            std::error_code ec;
            llvm::raw_fd_ostream out(outputFile, ec, llvm::sys::fs::OF_None);
            if (ec) {
                throw std::runtime_error("Could not write IR to file: " + ec.message());
            }
            codegen.getModule()->print(out, nullptr);
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
        compiler.compile(sourceFile, outputFile);
        if (command == Command::Run) {
            // Register temp files for cleanup on signal (Ctrl+C during program run).
            if (!outputSpecified && !keepTemps) {
                std::string objPath = outputFile + ".o";
                std::strncpy(g_tempOutputFile, outputFile.c_str(), kMaxTempPathLen - 1);
                std::strncpy(g_tempObjectFile, objPath.c_str(), kMaxTempPathLen - 1);
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
