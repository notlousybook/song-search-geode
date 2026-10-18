// Standalone logic test for the song-search port.
// Mirrors the fuzzy search + label-fit + scroll-window logic in src/main.cpp
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

    // 7. expo-out easing: renders ease toward a target the same way the mod
    //    does, must be monotonic, framerate-correct, and land exactly on target
    auto easeStep = [](float current, float target, float dt) -> float {
        const float factor = 1.f - std::exp(-18.f * dt);
        return current + (target - current) * factor;
    };
    {
        float pos = 0.f, target = 300.f;
        float prev = pos;
        int steps = 0;
        while (std::abs(target - pos) > 0.01f && steps < 100000) {
            pos = easeStep(pos, target, 1.f / 60.f);
            assert(pos >= prev - 1e-6f);   // never moves backwards
            assert(pos <= target + 1e-6f); // never overshoots (expo out)
            prev = pos;
            ++steps;
        }
        assert(std::abs(pos - target) < 0.01f);
        // a 30fps run should need fewer steps than the same distance at 60fps
        float pos30 = 0.f;
        int steps30 = 0;
        while (std::abs(target - pos30) > 0.01f && steps30 < 100000) {
            pos30 = easeStep(pos30, target, 1.f / 30.f);
            ++steps30;
        }
        assert(steps30 < steps);
        assert(std::abs(pos30 - target) < 0.01f);
        std::cout << "  easing: 60fps " << steps << " steps, 30fps " << steps30 << " steps\n";
    }

    // 8. label fit: mirrors SimpleSongWidget::fitLabel - shrink from the base
    //    scale down toward minScale, then truncate with "..." so text can
    //    never run under the row's buttons
    {
        auto width = [](const std::string& s) { return 10.f * s.size(); }; // fake font: 10px/char
        auto fit = [&](const std::string& text, float base, float maxW, float minS) -> std::string {
            const float scale = std::min(base, maxW / width(text));
            if (scale >= minS) return text; // shown whole (possibly shrunk)
            size_t lo = 0, hi = text.size();
            std::string best = "...";
            while (lo <= hi) {
                const size_t mid = lo + (hi - lo) / 2;
                std::string candidate = text.substr(0, mid) + "...";
                if (width(candidate) <= maxW / minS) { best = candidate; lo = mid + 1; }
                else { if (mid == 0) break; hi = mid - 1; }
            }
            return best;
        };
        constexpr float kMax = 240.f, kMin = 0.5f;
        // short text untouched
        assert(fit("Short", 0.8f, kMax, kMin) == "Short");
        // 280px fits the full budget at base scale, so it stays whole
        std::string medium(28, 'x');
        assert(fit(medium, 0.8f, kMax, kMin) == medium);
        // 400px needs a shrink to 0.6, but still fits whole above minScale
        std::string shrink(40, 'y');
        assert(fit(shrink, 0.8f, kMax, kMin) == shrink);
        // 600px cannot fit even at minScale -> cut with ellipsis
        std::string huge(60, 'z');
        std::string cut = fit(huge, 0.8f, kMax, kMin);
        assert(cut.size() < huge.size());
        assert(cut.size() >= 3 && cut.substr(cut.size() - 3) == "...");
        assert(width(cut) <= kMax / kMin + 1e-3f); // fits the budget at minScale
        std::cout << "  label fit: 60-char name -> " << cut.size() << " chars with ellipsis\n";
    }

    std::cout << "ALL LOGIC TESTS PASSED" << std::endl;
    return 0;
}
