#pragma once

#include <filesystem>
#include <functional>
#include <set>
#include <string>
#include <vector>

namespace usdgeo::detail {

// The payload files one generation writes into a payload directory, and the
// rule that decides which existing files it may replace.
//
// Without an owner the directory is exclusive: any existing target path is a
// conflict. With an owner, the generation may replace exactly the files the
// same owner generated before, as listed in that owner's record inside the
// directory; any other existing file is still a conflict and is never
// touched. Ownership is recorded before the first write, so an interrupted
// generation leaves files the next generation for the same owner replaces.
//
// The record is named by a hash of the owner and lists relative payload
// names only, so neither the owner string nor a source identifier is
// persisted.
class GeneratedPayloadSet final {
public:
    GeneratedPayloadSet(std::filesystem::path directory, std::string owner);
    ~GeneratedPayloadSet();

    GeneratedPayloadSet(const GeneratedPayloadSet&) = delete;
    GeneratedPayloadSet& operator=(const GeneratedPayloadSet&) = delete;

    using AdoptPredicate = std::function<bool(const std::filesystem::path&)>;

    // Creates the directory when needed, refuses any planned path that
    // exists without belonging to this owner, and records the claim. An
    // existing file the owner does not hold is claimed anyway only when
    // `adopt` accepts it, which a caller does when it can prove the file
    // already holds exactly the bytes it would write.
    bool Claim(const std::vector<std::filesystem::path>& payloads,
               std::string& error, const AdoptPredicate& adopt = {});

    // Notes that this generation wrote one claimed payload.
    void MarkWritten(const std::filesystem::path& payload);

    // Removes files the owner generated before but this generation did not,
    // then records exactly the claimed payloads.
    bool Commit(std::string& error);

    // Removes what this generation wrote, restores the owner's previous
    // record, and removes the directory when this generation created it and
    // left it empty. Does nothing after Commit.
    void Rollback();

    static std::filesystem::path RecordPath(
        const std::filesystem::path& directory, const std::string& owner);

private:
    bool WriteRecord(const std::set<std::string>& names,
                     std::string& error) const;

    std::filesystem::path directory_;
    std::string owner_;
    std::filesystem::path record_;
    std::set<std::string> previous_;
    std::set<std::string> claimed_;
    std::vector<std::filesystem::path> written_;
    bool directoryCreated_ = false;
    bool recordWritten_ = false;
    bool active_ = false;
};

} // namespace usdgeo::detail
