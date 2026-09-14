#pragma once
#include <string>
#include <vector>

class RTSSManager
{
public:
    static RTSSManager& Get();

    // Returns a list of all .cfg files in the RTSS Profiles directory
    std::vector<std::wstring> GetProfiles();

    // Sets the framerate limit for a specific executable (e.g. "Game.exe")
    bool SetFramerateLimit(const std::wstring& exeName, int limit);

    // Forces RTSS to reload
    void NotifyRTSS();

    std::wstring GetProfilesDir() const;

private:
    RTSSManager() = default;
};
