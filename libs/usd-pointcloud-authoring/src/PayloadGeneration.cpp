#include "PayloadGeneration.h"

#include "usdgeo/CacheKey.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>
#include <system_error>

namespace usdgeo::detail {
namespace {

constexpr const char* kStagingPrefix = ".tmp-";
constexpr const char* kGeneratedPrefix = "g-";
constexpr const char* kCachedPrefix = "c-";

// A live generation touches its staging directory with every payload it
// writes, so one left untouched this long belongs to an interrupted process.
constexpr auto kStaleStagingAge = std::chrono::hours(1);

bool StartsWith(const std::string& value, const char* prefix) {
    return value.rfind(prefix, 0) == 0;
}

// Payload names are joined onto the directory the generation owns, so a name
// that could leave it is rejected rather than trusted.
bool IsSafeRelativeName(const std::string& value) {
    if (value.empty() || value.front() == '/' ||
        value.find_first_of("\\:\r\n") != std::string::npos) {
        return false;
    }
    std::size_t start = 0;
    for (;;) {
        const auto separator = value.find('/', start);
        const auto end =
            separator == std::string::npos ? value.size() : separator;
        const auto component = value.substr(start, end - start);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        if (separator == std::string::npos) {
            return true;
        }
        start = separator + 1;
    }
}

bool IsGenerationName(const std::string& value) {
    return (StartsWith(value, kGeneratedPrefix) ||
            StartsWith(value, kCachedPrefix)) &&
           value.find('/') == std::string::npos;
}

std::string Hex(std::uint64_t value) {
    std::ostringstream text;
    text << std::hex << std::setfill('0') << std::setw(16) << value;
    return text.str();
}

std::string NewStagingName() {
    static std::atomic_uint64_t sequence{0};
    std::random_device device;
    const auto clock = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    const auto value = (static_cast<std::uint64_t>(device()) << 32) ^
                       device() ^ clock ^
                       (++sequence * 0x9e3779b97f4a7c15ull);
    return kStagingPrefix + Hex(value);
}

void RemoveStaleStaging(const std::filesystem::path& ownerDirectory) {
    std::vector<std::filesystem::path> stale;
    const auto now = std::filesystem::file_time_type::clock::now();
    std::error_code error;
    for (std::filesystem::directory_iterator entry(ownerDirectory, error), end;
         !error && entry != end; entry.increment(error)) {
        std::error_code status;
        if (!StartsWith(entry->path().filename().string(), kStagingPrefix) ||
            !entry->is_directory(status) || status) {
            continue;
        }
        const auto modified =
            std::filesystem::last_write_time(entry->path(), status);
        if (!status && now - modified > kStaleStagingAge) {
            stale.push_back(entry->path());
        }
    }
    for (const auto& path : stale) {
        std::error_code removal;
        std::filesystem::remove_all(path, removal);
    }
}

// Names the published generation by what it contains, so regenerating the
// same payloads finds the directory already published.
bool HashPayloads(const std::filesystem::path& directory,
                  const std::set<std::string>& names,
                  std::string& hash,
                  std::string& error) {
    constexpr std::uint64_t offsetBasis = 14695981039346656037ull;
    constexpr std::uint64_t prime = 1099511628211ull;
    std::uint64_t value = offsetBasis;
    const auto mix = [&value](const char* data, std::size_t size) {
        for (std::size_t index = 0; index < size; ++index) {
            value ^= static_cast<unsigned char>(data[index]);
            value *= prime;
        }
    };
    std::array<char, 64 * 1024> buffer{};
    for (const auto& name : names) {
        mix(name.c_str(), name.size() + 1);
        std::ifstream input(directory / std::filesystem::path(name),
                            std::ios::binary);
        if (!input) {
            error = "unable to read generated payload " + name;
            return false;
        }
        std::uint64_t size = 0;
        while (input) {
            input.read(buffer.data(),
                       static_cast<std::streamsize>(buffer.size()));
            const auto count = static_cast<std::size_t>(input.gcount());
            mix(buffer.data(), count);
            size += count;
        }
        if (!input.eof()) {
            error = "unable to read generated payload " + name;
            return false;
        }
        const auto sizeText = std::to_string(size);
        mix(sizeText.c_str(), sizeText.size() + 1);
    }
    hash = Hex(value);
    return true;
}

} // namespace

std::string PayloadOwnerKey(const std::string& owner) {
    return StableCacheKey({{"payloadOwner", owner}});
}

std::string CachedGenerationName(const std::string& entryKey) {
    return kCachedPrefix + entryKey;
}

std::filesystem::path PayloadGeneration::OwnerDirectory(
    const std::filesystem::path& directory, const std::string& owner) {
    return directory / PayloadOwnerKey(owner);
}

PayloadGeneration::PayloadGeneration(std::filesystem::path directory,
                                     std::string owner)
    : directory_(directory.lexically_normal()), owner_(std::move(owner)) {
    // A trailing separator would leave an empty final element that makes the
    // payload paths below it ambiguous.
    if (!directory_.has_filename() && directory_.has_relative_path()) {
        directory_ = directory_.parent_path();
    }
    if (!owner_.empty()) {
        ownerDirectory_ = OwnerDirectory(directory_, owner_);
    }
}

PayloadGeneration::~PayloadGeneration() {
    Rollback();
}

bool PayloadGeneration::Begin(const std::vector<std::string>& names,
                              std::string& error) {
    if (active_) {
        error = "payload generation has already begun";
        return false;
    }
    std::set<std::string> declared;
    for (const auto& name : names) {
        if (!IsSafeRelativeName(name) || !declared.insert(name).second) {
            error = "payload names must be unique relative paths";
            return false;
        }
    }

    std::error_code status;
    const auto directoryExists = std::filesystem::exists(directory_, status);
    if (!status && directoryExists &&
        !std::filesystem::is_directory(directory_, status)) {
        status = std::make_error_code(std::errc::not_a_directory);
    }
    if (status) {
        error = "payload directory is not a usable directory";
        return false;
    }
    if (owner_.empty() && directoryExists) {
        for (const auto& name : declared) {
            const auto present = std::filesystem::exists(
                directory_ / std::filesystem::path(name), status);
            if (status) {
                error = "unable to inspect payload " + name;
                return false;
            }
            if (present) {
                error = "payload directory already contains " + name;
                return false;
            }
        }
    }

    names_ = std::move(declared);
    active_ = true;
    if (!directoryExists) {
        std::filesystem::create_directories(directory_, status);
        if (status) {
            error = "unable to create payload directory";
            Rollback();
            return false;
        }
        directoryCreated_ = true;
    }
    if (owner_.empty()) {
        return true;
    }

    const auto ownerExists = std::filesystem::exists(ownerDirectory_, status);
    if (status) {
        error = "unable to inspect the payload owner directory";
        Rollback();
        return false;
    }
    if (ownerExists) {
        RemoveStaleStaging(ownerDirectory_);
    } else {
        std::filesystem::create_directory(ownerDirectory_, status);
        if (status) {
            error = "unable to create the payload owner directory";
            Rollback();
            return false;
        }
        ownerDirectoryCreated_ = true;
    }
    for (int attempt = 0; attempt < 16 && stagingDirectory_.empty();
         ++attempt) {
        const auto name = NewStagingName();
        if (std::filesystem::create_directory(ownerDirectory_ / name, status)) {
            stagingName_ = name;
            stagingDirectory_ = ownerDirectory_ / name;
        }
    }
    if (stagingDirectory_.empty()) {
        error = "unable to create a payload staging directory";
        Rollback();
        return false;
    }
    return true;
}

std::filesystem::path PayloadGeneration::PathFor(const std::string& name) const {
    if (!active_ || names_.count(name) == 0) {
        return {};
    }
    const auto& base = owner_.empty() ? directory_ : stagingDirectory_;
    return base / std::filesystem::path(name);
}

std::filesystem::path PayloadGeneration::PublishedPathFor(
    const std::string& name) const {
    if (owner_.empty()) {
        return directory_ / std::filesystem::path(name);
    }
    if (publishedName_.empty()) {
        return {};
    }
    return ownerDirectory_ / publishedName_ / std::filesystem::path(name);
}

bool PayloadGeneration::Commit(std::string& error) {
    if (!active_) {
        error = "payload generation has not begun";
        return false;
    }
    if (owner_.empty()) {
        active_ = false;
        directoryCreated_ = false;
        return true;
    }
    for (const auto& name : names_) {
        std::error_code status;
        if (!std::filesystem::is_regular_file(
                stagingDirectory_ / std::filesystem::path(name), status)) {
            error = "generated payload " + name + " was not written";
            return false;
        }
    }
    std::string hash;
    if (!HashPayloads(stagingDirectory_, names_, hash, error)) {
        return false;
    }
    return Publish(kGeneratedPrefix + hash, error);
}

bool PayloadGeneration::CommitAs(const std::string& name, std::string& error) {
    if (!active_ || owner_.empty()) {
        error = "only an owned payload generation can be published by name";
        return false;
    }
    if (!IsGenerationName(name) || !IsSafeRelativeName(name)) {
        error = "invalid payload generation name";
        return false;
    }
    return Publish(name, error);
}

bool PayloadGeneration::IsComplete(
    const std::filesystem::path& generation) const {
    for (const auto& name : names_) {
        std::error_code status;
        const auto published = generation / std::filesystem::path(name);
        if (!std::filesystem::is_regular_file(published, status) || status) {
            return false;
        }
        const auto publishedSize = std::filesystem::file_size(published, status);
        if (status) {
            return false;
        }
        const auto stagedSize = std::filesystem::file_size(
            stagingDirectory_ / std::filesystem::path(name), status);
        if (status || stagedSize != publishedSize) {
            return false;
        }
    }
    return true;
}

bool PayloadGeneration::Publish(const std::string& name, std::string& error) {
    const auto published = ownerDirectory_ / name;
    std::error_code status;
    if (std::filesystem::exists(published, status) && !IsComplete(published)) {
        // A damaged generation is replaced rather than trusted.
        std::filesystem::remove_all(published, status);
        if (status) {
            error = "unable to replace incomplete payload generation " + name;
            return false;
        }
    }
    if (std::filesystem::exists(published, status)) {
        // The same payloads are already published; keep them in place so
        // nothing that has them open sees them replaced.
        std::filesystem::remove_all(stagingDirectory_, status);
    } else {
        std::filesystem::rename(stagingDirectory_, published, status);
        if (status) {
            // A concurrent generation of the same payloads may have
            // published first.
            std::error_code check;
            if (!std::filesystem::exists(published, check) ||
                !IsComplete(published)) {
                error = "unable to publish payload generation " + name;
                return false;
            }
            std::filesystem::remove_all(stagingDirectory_, check);
        }
    }
    publishedName_ = name;
    stagingDirectory_.clear();
    active_ = false;
    directoryCreated_ = false;
    ownerDirectoryCreated_ = false;

    std::vector<std::filesystem::path> superseded;
    std::error_code iteration;
    for (std::filesystem::directory_iterator entry(ownerDirectory_, iteration),
         end;
         !iteration && entry != end; entry.increment(iteration)) {
        const auto entryName = entry->path().filename().string();
        std::error_code entryStatus;
        if (entryName != name && IsGenerationName(entryName) &&
            entry->is_directory(entryStatus) && !entryStatus) {
            superseded.push_back(entry->path());
        }
    }
    for (const auto& path : superseded) {
        // Best effort: a file another process still holds open stays until a
        // later generation removes it.
        std::error_code removal;
        std::filesystem::remove_all(path, removal);
    }
    return true;
}

void PayloadGeneration::Rollback() {
    if (!active_) {
        return;
    }
    active_ = false;
    std::error_code status;
    if (owner_.empty()) {
        // Begin proved none of these existed, so any that do now were
        // written by this generation.
        for (const auto& name : names_) {
            std::filesystem::remove(directory_ / std::filesystem::path(name),
                                    status);
        }
    } else if (!stagingDirectory_.empty()) {
        std::filesystem::remove_all(stagingDirectory_, status);
        stagingDirectory_.clear();
    }
    // Removing a directory succeeds only while it is empty.
    if (ownerDirectoryCreated_) {
        std::filesystem::remove(ownerDirectory_, status);
        ownerDirectoryCreated_ = false;
    }
    if (directoryCreated_) {
        std::filesystem::remove(directory_, status);
        directoryCreated_ = false;
    }
}

} // namespace usdgeo::detail
