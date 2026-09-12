#pragma once

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace usdgeo::detail {

// The directory name under which one owner's payload generations live.
std::string PayloadOwnerKey(const std::string& owner);

// The generation name for payloads copied out of a generated-cache entry,
// which the entry's key already identifies.
std::string CachedGenerationName(const std::string& entryKey);

// One set of payload files written into a payload directory.
//
// Without an owner the directory is exclusive: payloads are written straight
// into it, and any payload name that already exists refuses the generation.
//
// With an owner, payloads live in `<directory>/<owner key>/<generation>/`.
// They are written into a private staging directory and published at Commit
// by renaming it into place, so an existing generation is never modified: a
// failed or cancelled generation leaves the published one untouched, and a
// generation with identical content reuses the published directory instead
// of replacing it. Publishing removes the owner's superseded generations.
// Nothing outside the owner's directory is ever written or removed.
class PayloadGeneration final {
public:
    PayloadGeneration(std::filesystem::path directory, std::string owner);
    ~PayloadGeneration();

    PayloadGeneration(const PayloadGeneration&) = delete;
    PayloadGeneration& operator=(const PayloadGeneration&) = delete;

    // Declares the payload names this generation writes, relative to where it
    // publishes them, and prepares the directory they are written into.
    bool Begin(const std::vector<std::string>& names, std::string& error);

    // Where a declared payload is written before Commit. Empty for a name
    // that was not declared.
    std::filesystem::path PathFor(const std::string& name) const;

    // Publishes the generation under a name derived from its content.
    bool Commit(std::string& error);

    // Publishes the generation under `name`, for content whose identity is
    // already known.
    bool CommitAs(const std::string& name, std::string& error);

    // Removes what this generation wrote, and the directories it created when
    // they are left empty. Does nothing after a commit.
    void Rollback();

    // The staging directory name that payload paths contain until Commit, and
    // the generation name that replaces it. Both are empty without an owner.
    const std::string& StagingName() const noexcept { return stagingName_; }
    const std::string& PublishedName() const noexcept { return publishedName_; }

    // Where a declared payload is after Commit.
    std::filesystem::path PublishedPathFor(const std::string& name) const;

    // The directory holding the owner's generations.
    static std::filesystem::path OwnerDirectory(
        const std::filesystem::path& directory, const std::string& owner);

private:
    bool Publish(const std::string& name, std::string& error);
    bool IsComplete(const std::filesystem::path& generation) const;

    std::filesystem::path directory_;
    std::string owner_;
    std::filesystem::path ownerDirectory_;
    std::filesystem::path stagingDirectory_;
    std::string stagingName_;
    std::string publishedName_;
    std::set<std::string> names_;
    bool directoryCreated_ = false;
    bool ownerDirectoryCreated_ = false;
    bool active_ = false;
};

} // namespace usdgeo::detail
