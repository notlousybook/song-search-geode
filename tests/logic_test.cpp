// Standalone logic test for the song-search port.
// Mirrors the fuzzy search + sort + scroll-window logic in src/main.cpp
// without Geode headers, so it can run on plain GCC.
#define FTS_FUZZY_MATCH_IMPLEMENTATION
#include "fts_fuzzy_match.h"

#include <algorithm>
#include <cassert>
#include <cmath>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

struct FakeSong { int id; std::string name; std::string artist; };

static std::vector<std::pair<int, FakeSong>> do_search(
    const std::vector<std::pair<std::string, FakeSong>>& downloaded,
    const std::string& input) {
    std::vector<std::pair<int, FakeSong>> songs;
    songs.reserve(downloaded.size());
    const bool empty_input = input.empty();
    for (const auto& [index_str, song] : downloaded) {
        int score = 0;
        if (!empty_input && !fts::fuzzy_match(input.c_str(), index_str.c_str(), score)) continue;
        songs.push_back({score, song});
    }
    if (!empty_input && songs.size() > 1) {
        std::sort(songs.begin(), songs.end(), [&](const auto& a, const auto& b) {
            return a.first > b.first;
        });
    }
    return songs;
}

int main() {
    std::vector<std::pair<std::string, FakeSong>> downloaded = {
        {"Stereo Madness Forever", {1, "Stereo Madness", "Forever"}},
        {"Back on Track DJVI", {2, "Back on Track", "DJVI"}},
        {"Polargeist Step", {3, "Polargeist", "Step"}},
        {"Dry Out DJVI", {4, "Dry Out", "DJVI"}},
        {"Base After Base DJVI", {5, "Base After Base", "DJVI"}},
    };

    // 1. empty input returns everything, unsorted
    auto all = do_search(downloaded, "");
    assert(all.size() == 5);

    // 2. exact-ish query ranks the right song first
    auto djvi = do_search(downloaded, "djvi");
    assert(djvi.size() == 3);
    for (auto& [score, s] : djvi) (void)score, assert(s.artist == "DJVI");

    // 3. fuzzy query matches across name+artist blob
    auto polar = do_search(downloaded, "polr");
    assert(polar.size() == 1 && polar[0].second.id == 3);

    // 4. no match -> empty
    assert(do_search(downloaded, "zzzqqq").empty());

    // 5. scroll clamp: mirrors update_scroll_elements clamping
    constexpr float height = 55.f; // widget_height + spacing
    constexpr int n_widgets = 6;
    auto clamp_scroll = [&](size_t count, float y) {
        const auto m = height * (static_cast<int>(count) - n_widgets + 3);
        if (m < 0) return 0.f;
        return std::clamp(y, 0.f, m);
    };
    assert(clamp_scroll(0, 999.f) == 0.f);   // empty list pins to 0
    assert(clamp_scroll(3, 999.f) == 0.f);   // short list pins to 0
    assert(clamp_scroll(20, -50.f) == 0.f);  // negative pins to 0
    assert(clamp_scroll(20, 1e9f) == height * (20 - n_widgets + 3)); // caps at max
    assert(clamp_scroll(20, 100.f) == 100.f); // in-range untouched

    // 6. result label pluralization
    auto label = [](size_t n) {
        std::stringstream s; s << n << " result"; if (n != 1) s << "s"; return s.str();
    };
    assert(label(0) == "0 results" && label(1) == "1 result" && label(5) == "5 results");

    std::cout << "ALL LOGIC TESTS PASSED" << std::endl;
    return 0;
}
