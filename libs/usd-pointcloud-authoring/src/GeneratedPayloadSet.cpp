#include "GeneratedPayloadSet.h"

#include "usdgeo/CacheKey.h"

#include <charconv>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

namespace usdgeo::detail {
namespace {

constexpr const char* kRecordFormat = "usd-pointcloud-payload-owner-v1";

// A recorded name is only ever joined onto the payload directory, and a
// commit removes names the owner no longer generates, so a name that could
// escape the directory is rejected rather than trusted.
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

std::string RelativeName(const std::filesystem::path& directory,
                         const std::filesystem::path& payload) {
    const auto name = payload.lexically_normal()
                          .lexically_relative(directory.lexically_normal())
                          .generic_string();
    return IsSafeRelativeName(name) ? name : std::string();
}

bool ReadRecord(const std::filesystem::path& path,
                std::set<std::string>& names,
                std::string& error) {
    names.clear();
    const auto invalid = [&]() {
        names.clear();
        error = "payload ownership record " + path.filename().string() +
                " is invalid";
        return false;
    };
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        error = "unable to read payload ownership record " +
                path.filename().string();
        return false;
    }

    std::optional<std::uint64_t> count;
    std::uint64_t lineIndex = 0;
    std::string line;
    while (std::getline(input, line)) {
        const auto separator = line.find('=');
        if (separator == std::string::npos) {
            return invalid();
        }
        const auto key = line.substr(0, separator);
        const auto value = line.substr(separator + 1);
        if (lineIndex == 0) {
            if (key != "format" || value != kRecordFormat) {
                return invalid();
            }
        } else if (lineIndex == 1) {
            std::uint64_t parsed = 0;
            const auto* first = value.data();
            const auto* last = value.data() + value.size();
            const auto result = std::from_chars(first, last, parsed);
            if (key != "payload.count" || value.empty() ||
                result.ec != std::errc() || result.ptr != last) {
                return invalid();
            }
            count = parsed;
        } else if (key != "payload." + std::to_string(lineIndex - 2) ||
                   !IsSafeRelativeName(value) ||
                   !names.insert(value).second) {
            return invalid();
        }
        ++lineIndex;
    }
    if (input.bad() || !count || names.size() != *count) {
        return invalid();
    }
    return true;
}

} // namespace

GeneratedPayloadSet::GeneratedPayloadSet(std::filesystem::path directory,
                                         std::string owner)
    : directory_(directory.lexically_normal()), owner_(std::move(owner)) {
    // A trailing separator would leave an empty final element that makes
    // payload names relative to it ambiguous.
    if (!directory_.has_filename() && directory_.has_relative_path()) {
        directory_ = directory_.parent_path();
    }
    if (!owner_.empty()) {
        record_ = RecordPath(directory_, owner_);
    }
}

GeneratedPayloadSet::~GeneratedPayloadSet() {
    Rollback();
}

std::filesystem::path GeneratedPayloadSet::RecordPath(
    const std::filesystem::path& directory, const std::string& owner) {
    return directory / ("payload-owner-" +
                        StableCacheKey({{"payloadOwner", owner}}) +
                        ".manifest");
}

bool GeneratedPayloadSet::Claim(
    const std::vector<std::filesystem::path>& payloads, std::string& error,
    const AdoptPredicate& adopt) {
    if (active_) {
        error = "payload set is already claimed";
        return false;
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

    std::set<std::string> names;
    for (const auto& payload : payloads) {
        const auto name = RelativeName(directory_, payload);
        if (name.empty() || !names.insert(name).second) {
            error = "payload paths must be unique and inside the payload "
                    "directory";
            return false;
        }
    }

    previous_.clear();
    if (directoryExists) {
        if (!record_.empty()) {
            const auto recordExists = std::filesystem::exists(record_, status);
            if (status) {
                error = "unable to inspect the payload ownership record";
                return false;
            }
            if (recordExists && !ReadRecord(record_, previous_, error)) {
                return false;
            }
        }
        for (const auto& name : names) {
            const auto path = directory_ / std::filesystem::path(name);
            const auto exists = std::filesystem::exists(path, status);
            if (status) {
                error = "unable to inspect payload " + name;
                return false;
            }
            if (!exists) {
                continue;
            }
            if (!std::filesystem::is_regular_file(path, status) || status) {
                error = "payload directory entry " + name +
                        " is not a regular file";
                return false;
            }
            if (previous_.count(name) == 0 && !(adopt && adopt(path))) {
                error = "payload directory already contains " + name +
                        ", which this layer did not generate";
                return false;
            }
        }
    } else {
        std::filesystem::create_directories(directory_, status);
        if (status) {
            error = "unable to create payload directory";
            return false;
        }
        directoryCreated_ = true;
    }

    claimed_ = std::move(names);
    written_.clear();
    active_ = true;
    if (!record_.empty()) {
        auto claim = previous_;
        claim.insert(claimed_.begin(), claimed_.end());
        if (!WriteRecord(claim, error)) {
            Rollback();
            return false;
        }
        recordWritten_ = true;
    }
    return true;
}

void GeneratedPayloadSet::MarkWritten(const std::filesystem::path& payload) {
    written_.push_back(payload);
}

bool GeneratedPayloadSet::Commit(std::string& error) {
    if (!active_) {
        error = "payload set was not claimed";
        return false;
    }
    if (!record_.empty()) {
        // Superseded files go before the record shrinks, so a crash in
        // between leaves them listed and still replaceable.
        auto record = claimed_;
        for (const auto& name : previous_) {
            if (claimed_.count(name) != 0) {
                continue;
            }
            std::error_code removal;
            std::filesystem::remove(directory_ / std::filesystem::path(name),
                                    removal);
            if (removal) {
                record.insert(name);
            }
        }
        if (!WriteRecord(record, error)) {
            return false;
        }
    }
    active_ = false;
    recordWritten_ = false;
    directoryCreated_ = false;
    written_.clear();
    return true;
}

void GeneratedPayloadSet::Rollback() {
    if (!active_) {
        return;
    }
    active_ = false;

    std::set<std::string> retained;
    for (const auto& payload : written_) {
        std::error_code removal;
        std::filesystem::remove(payload, removal);
        if (removal) {
            retained.insert(RelativeName(directory_, payload));
        }
    }
    written_.clear();

    if (recordWritten_) {
        auto restored = previous_;
        restored.insert(retained.begin(), retained.end());
        std::error_code removal;
        std::string ignored;
        if (restored.empty()) {
            std::filesystem::remove(record_, removal);
        } else {
            WriteRecord(restored, ignored);
        }
        recordWritten_ = false;
    }

    if (directoryCreated_) {
        std::error_code status;
        if (std::filesystem::is_empty(directory_, status) && !status) {
            std::filesystem::remove(directory_, status);
        }
        directoryCreated_ = false;
    }
}

bool GeneratedPayloadSet::WriteRecord(const std::set<std::string>& names,
                                      std::string& error) const {
    std::ostringstream text;
    text << "format=" << kRecordFormat << '\n'
         << "payload.count=" << names.size() << '\n';
    std::size_t index = 0;
    for (const auto& name : names) {
        text << "payload." << index++ << '=' << name << '\n';
    }

    const std::filesystem::path temporary(record_.string() + ".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output << text.str();
        output.close();
        if (!output) {
            std::error_code removal;
            std::filesystem::remove(temporary, removal);
            error = "unable to write payload ownership record";
            return false;
        }
    }
    std::error_code rename;
    std::filesystem::rename(temporary, record_, rename);
    if (rename) {
        std::error_code removal;
        std::filesystem::remove(temporary, removal);
        error = "unable to publish payload ownership record";
        return false;
    }
    return true;
}

} // namespace usdgeo::detail
