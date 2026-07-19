// Senary Numbers — v0.1
// Global backbone: every CCLabelBMFont setString is intercepted; standalone
// decimal digit runs are rewritten in senary, "percent" becomes "pernif",
// and every % glyph is flipped vertically (the pernif sign).
// Targeted hooks: PlayLayer progress label and LevelInfoLayer best-percent
// bars are semantically rescaled to pernifage (x * 36/100), not just
// digit-converted.
//
// Known v0.1 over-conversion (to be excluded in later rounds):
// user-generated names/descriptions containing digits, version strings,
// dates, IDs outside LevelInfoLayer. Under-conversion guard: none known.

#include <Geode/Geode.hpp>
#include <Geode/modify/CCLabelBMFont.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>

#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

using namespace geode::prelude;

namespace senary {

// Set while a targeted hook writes an already-senary string, so the global
// hook passes it through untouched (senary digits would otherwise be
// re-read as decimal and mangled).
static bool s_bypass = false;

// Last string this mod wrote to each label. If the same text comes back in
// (game re-setting a label from its own getString, or our layered hooks on
// setString overloads re-entering each other) it is passed through instead
// of being converted a second time. Entries go stale when a label is
// destroyed; worst case a recycled address skips one conversion frame.
static std::unordered_map<CCLabelBMFont*, std::string> s_lastOutput;

static bool enabled() {
    return Mod::get()->getSettingValue<bool>("enabled");
}
static bool debugLog() {
    return Mod::get()->getSettingValue<bool>("debug-logging");
}

// Pure base conversion of a non-negative integer.
static std::string toSenary(uint64_t v) {
    if (v == 0) return "0";
    std::string out;
    while (v > 0) {
        out.insert(out.begin(), static_cast<char>('0' + (v % 6)));
        v /= 6;
    }
    return out;
}

// Whole-word, case-pattern-preserving percent -> pernif.
static void replacePercentWords(std::string& s) {
    static constexpr const char* FROM[] = { "percent", "Percent", "PERCENT" };
    static constexpr const char* TO[]   = { "pernif",  "Pernif",  "PERNIF"  };
    for (int i = 0; i < 3; ++i) {
        std::string const from = FROM[i];
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            bool leftOk  = pos == 0 || !std::isalpha(static_cast<unsigned char>(s[pos - 1]));
            size_t end   = pos + from.size();
            bool rightOk = end >= s.size() || !std::isalpha(static_cast<unsigned char>(s[end]));
            // "percentage" -> allow: treat a trailing "age" as part of the same
            // word family ("percentage" -> "pernifage").
            if (leftOk && (rightOk || s.compare(end, 3, "age") == 0)) {
                s.replace(pos, from.size(), TO[i]);
                pos += std::string(TO[i]).size();
            } else {
                pos = end;
            }
        }
    }
}

// Convert standalone decimal digit runs in-place. A run is standalone when
// bounded by non-alphanumeric characters (so "Attempt 57" and "57%" convert,
// "x2f" and "M4" do not). Runs longer than 15 digits are left alone.
static std::string convertText(std::string const& in) {
    std::string out;
    out.reserve(in.size() + 8);
    size_t i = 0;
    size_t const n = in.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (std::isdigit(c)) {
            size_t start = i;
            while (i < n && std::isdigit(static_cast<unsigned char>(in[i]))) ++i;
            size_t len = i - start;
            bool leftOk  = start == 0 || !std::isalnum(static_cast<unsigned char>(in[start - 1]));
            bool rightOk = i == n     || !std::isalnum(static_cast<unsigned char>(in[i]));
            if (leftOk && rightOk && len <= 15) {
                uint64_t v = std::stoull(in.substr(start, len));
                out += toSenary(v);
            } else {
                out += in.substr(start, len);
            }
        } else {
            out += in[i];
            ++i;
        }
    }
    replacePercentWords(out);
    return out;
}

// Flip every % glyph vertically; un-flip everything else (glyph sprites are
// reused across setString calls, so a sprite that was % last frame may be a
// digit now). cocos2d tags each glyph sprite with its character index.
static void flipPernifGlyphs(CCLabelBMFont* label, std::string const& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        auto* glyph = typeinfo_cast<CCSprite*>(label->getChildByTag(static_cast<int>(i)));
        if (glyph) glyph->setFlipY(text[i] == '%');
    }
}

// Shared core for every setString-family hook. Returns the string that
// should actually be passed to the original function.
static std::string process(CCLabelBMFont* label, char const* raw) {
    std::string in = raw ? raw : "";
    if (!enabled()) return in;
    if (s_bypass) {
        s_lastOutput[label] = in;
        return in;
    }
    auto it = s_lastOutput.find(label);
    if (it != s_lastOutput.end() && it->second == in) return in; // already ours
    std::string out = convertText(in);
    s_lastOutput[label] = out;
    if (debugLog() && out != in) log::debug("senary: \"{}\" -> \"{}\"", in, out);
    return out;
}

// --- pernifage formatting -------------------------------------------------

// percent (0..100 float) -> pernif display string, optionally with two
// floored senary radix digits. Computed from the raw fraction, never from a
// rounded decimal string.
static std::string formatPernif(double percent, bool radixDigits) {
    double f = percent / 100.0;          // 0..1
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    int whole = static_cast<int>(std::floor(f * 36.0 + 1e-9));
    std::string s = toSenary(static_cast<uint64_t>(whole));
    if (radixDigits) {
        int frac = static_cast<int>(std::floor(f * 36.0 * 36.0 + 1e-9)) - whole * 36;
        if (frac < 0) frac = 0;
        if (frac > 35) frac = 35;
        char buf[4] = { static_cast<char>('0' + frac / 6),
                        static_cast<char>('0' + frac % 6), 0, 0 };
        s += ".";
        s += buf;
    }
    s += "%"; // rendered as the flipped pernif sign
    return s;
}

// Recover whole pernif from a stored best percent. Must be ceiling: saved
// values are floor(pernif * 100/36), and e.g. 5% * 36/100 = 1.8 represents
// 2 whole pernif, not 1.
static int pernifFromSavedPercent(int percent) {
    if (percent >= 100) return 36;
    if (percent <= 0) return 0;
    return static_cast<int>(std::ceil(percent * 36.0 / 100.0 - 1e-9));
}

// Write a pre-formatted senary string to a label, bypassing conversion.
static void setSenaryString(CCLabelBMFont* label, std::string const& s) {
    s_bypass = true;
    label->setString(s.c_str());
    s_bypass = false;
    flipPernifGlyphs(label, s);
}

} // namespace senary

// --- global backbone ------------------------------------------------------

class $modify(SenaryLabel, CCLabelBMFont) {
    void setString(char const* str) {
        std::string out = senary::process(this, str);
        CCLabelBMFont::setString(out.c_str());
        if (senary::enabled()) senary::flipPernifGlyphs(this, out);
    }
    void setString(char const* str, bool needUpdateLabel) {
        std::string out = senary::process(this, str);
        CCLabelBMFont::setString(out.c_str(), needUpdateLabel);
        if (senary::enabled() && needUpdateLabel) senary::flipPernifGlyphs(this, out);
    }
    void setCString(char const* str) {
        std::string out = senary::process(this, str);
        CCLabelBMFont::setCString(out.c_str());
        if (senary::enabled()) senary::flipPernifGlyphs(this, out);
    }
};

// --- targeted semantic hooks ----------------------------------------------

class $modify(PlayLayer) {
    void updateProgressbar() {
        PlayLayer::updateProgressbar();
        if (!senary::enabled() || !m_percentageLabel) return;
        // Vanilla wrote e.g. "47%" or "47.35%"; conversion preserves the
        // '.', so its presence tells us the decimals setting is on without
        // needing the game-variable ID.
        std::string current = m_percentageLabel->getString();
        bool radix = current.find('.') != std::string::npos;
        double percent = static_cast<double>(this->getCurrentPercent());
        senary::setSenaryString(m_percentageLabel, senary::formatPernif(percent, radix));
    }
};

class $modify(LevelInfoLayer) {
    void setupProgressBars() {
        LevelInfoLayer::setupProgressBars();
        if (!senary::enabled() || !m_level) return;
        struct Target { char const* id; int percent; };
        Target targets[] = {
            { "normal-mode-percentage",   m_level->m_normalPercent.value() },
            { "practice-mode-percentage", m_level->m_practicePercent },
        };
        for (auto const& t : targets) {
            auto* label = typeinfo_cast<CCLabelBMFont*>(this->getChildByIDRecursive(t.id));
            if (!label) continue;
            int pernif = senary::pernifFromSavedPercent(t.percent);
            senary::setSenaryString(label, senary::toSenary(pernif) + "%");
        }
    }
};
