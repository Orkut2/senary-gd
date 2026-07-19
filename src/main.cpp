// Senary Numbers — v0.2
//
// Changes from v0.1:
// - Coverage: global hook moved to the two-arg setString(char const*, bool)
//   funnel (single override — Geode $modify cannot hold two overloads of one
//   name) plus initWithString, so creation-time text (settings menu, pause
//   menu, popups) is now intercepted. Geode hooks at the function address,
//   so internal calls from the one-arg setString/setCString route through.
// - Pernif sign: horizontal flip (setFlipX) — vertical flip inverted the
//   glyph's baked drop shadow.
// - New Best rework: fires only when a full new pernif is completed; best
//   saves as floor(whole_pernif * 100/36) percent; popups suppressed for
//   intra-pernif gains; popup percent semantically rescaled, not
//   digit-converted.
// - PlayLayer::resetLevel and PauseLayer::setupProgressBars hooks so the
//   attempt-start label and pause-menu progress lines show pernifage.

#include <Geode/Geode.hpp>
#include <Geode/modify/CCLabelBMFont.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>

#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

using namespace geode::prelude;

namespace senary {

// Set while a targeted hook writes an already-senary string, so the global
// hook passes it through untouched.
static bool s_bypass = false;

// Set while showNewBest runs: digit runs immediately followed by '%' are
// semantically rescaled (percent -> whole pernif) instead of digit-converted,
// so the popup shows real pernifage. Other numbers (orbs etc.) still
// digit-convert.
static bool s_rescalePercentTokens = false;

// Last string this mod wrote to each label; identical re-sets pass through
// instead of being converted twice (senary output would be re-read as
// decimal). Stale entries on address reuse cost at most one skipped frame.
static std::unordered_map<CCLabelBMFont*, std::string> s_lastOutput;

static bool enabled() {
    return Mod::get()->getSettingValue<bool>("enabled");
}
static bool debugLog() {
    return Mod::get()->getSettingValue<bool>("debug-logging");
}

static std::string toSenary(uint64_t v) {
    if (v == 0) return "0";
    std::string out;
    while (v > 0) {
        out.insert(out.begin(), static_cast<char>('0' + (v % 6)));
        v /= 6;
    }
    return out;
}

// --- pernif math ----------------------------------------------------------

// Recover whole pernif from a stored best percent. Ceiling, not floor:
// saved values are floor(n * 100/36); 5% * 36/100 = 1.8 means 2 pernif.
static int pernifFromSavedPercent(int percent) {
    if (percent >= 100) return 36;
    if (percent <= 0) return 0;
    return static_cast<int>(std::ceil(percent * 36.0 / 100.0 - 1e-9));
}

// The percent value this mod saves for n whole pernif.
static int savedPercentForPernif(int wholePernif) {
    if (wholePernif >= 36) return 100;
    if (wholePernif <= 0) return 0;
    return static_cast<int>(std::floor(wholePernif * 100.0 / 36.0 + 1e-9));
}

static bool isLegalSavedPercent(int p) {
    return savedPercentForPernif(pernifFromSavedPercent(p)) == p;
}

// Map an arbitrary (vanilla-written) percent onto the legal floored set.
// Identity on already-legal values; raw values floor down (18 -> 6 whole
// pernif -> 16).
static int normalizeSavedPercent(int p) {
    if (isLegalSavedPercent(p)) return p;
    int whole = static_cast<int>(std::floor(p * 36.0 / 100.0 + 1e-9));
    return savedPercentForPernif(whole);
}

// percent (0..100) -> pernif display string, optionally with two floored
// senary radix digits, computed from the raw fraction.
static std::string formatPernif(double percent, bool radixDigits) {
    double f = percent / 100.0;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    int whole = static_cast<int>(std::floor(f * 36.0 + 1e-9));
    std::string s = toSenary(static_cast<uint64_t>(whole));
    if (radixDigits) {
        int frac = static_cast<int>(std::floor(f * 36.0 * 36.0 + 1e-9)) - whole * 36;
        if (frac < 0) frac = 0;
        if (frac > 35) frac = 35;
        s += '.';
        s += static_cast<char>('0' + frac / 6);
        s += static_cast<char>('0' + frac % 6);
    }
    s += '%';
    return s;
}

// --- text conversion ------------------------------------------------------

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
            if (leftOk && (rightOk || s.compare(end, 3, "age") == 0)) {
                s.replace(pos, from.size(), TO[i]);
                pos += std::string(TO[i]).size();
            } else {
                pos = end;
            }
        }
    }
}

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
                if (s_rescalePercentTokens && i < n && in[i] == '%' && v <= 100) {
                    // Semantic rescale inside New Best scope: percent -> pernif.
                    out += toSenary(static_cast<uint64_t>(
                        pernifFromSavedPercent(static_cast<int>(v))));
                } else {
                    out += toSenary(v);
                }
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

// Horizontal flip for every % glyph; explicit un-flip elsewhere since glyph
// sprites are reused across setString calls. cocos2d tags each glyph sprite
// with its character index.
static void flipPernifGlyphs(CCLabelBMFont* label, std::string const& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        auto* glyph = typeinfo_cast<CCSprite*>(label->getChildByTag(static_cast<int>(i)));
        if (glyph) glyph->setFlipX(text[i] == '%');
    }
}

// Shared core for the setString-family hooks; returns the string to pass on.
static std::string process(CCLabelBMFont* label, char const* raw) {
    std::string in = raw ? raw : "";
    if (!enabled()) return in;
    if (s_bypass) {
        s_lastOutput[label] = in;
        return in;
    }
    auto it = s_lastOutput.find(label);
    if (it != s_lastOutput.end() && it->second == in) return in;
    std::string out = convertText(in);
    s_lastOutput[label] = out;
    if (debugLog() && out != in) log::debug("senary: \"{}\" -> \"{}\"", in, out);
    return out;
}

// Write a pre-formatted senary string, bypassing conversion.
static void setSenaryString(CCLabelBMFont* label, std::string const& s) {
    s_bypass = true;
    label->setString(s.c_str());
    s_bypass = false;
    flipPernifGlyphs(label, s);
}

// Rewrite a label of the form "<prefix><digits[.digits]>%" keeping the
// prefix, replacing the numeric tail with a given pernif string. Used where
// the numeric part may already be digit-converted, so it is discarded rather
// than parsed.
static void rewritePercentTail(CCLabelBMFont* label, std::string const& pernif) {
    std::string cur = label->getString() ? label->getString() : "";
    size_t pct = cur.rfind('%');
    if (pct == std::string::npos) return;
    size_t start = pct;
    while (start > 0) {
        unsigned char prev = static_cast<unsigned char>(cur[start - 1]);
        if (std::isdigit(prev) || prev == '.') --start; else break;
    }
    if (start == pct) return; // no numeric tail found
    setSenaryString(label, cur.substr(0, start) + pernif);
}

} // namespace senary

// --- global backbone ------------------------------------------------------

// One override per function name only: Geode's $modify resolves the hook
// target through &Derived::name, which is ambiguous with two overloads.
// The two-arg setString is the funnel; the one-arg overload and setCString
// call into it, and Geode hooks the function address, so those calls are
// intercepted too. initWithString covers creation-time text in case it does
// not route through setString.
class $modify(SenaryLabel, CCLabelBMFont) {
    void setString(char const* str, bool needUpdateLabel) {
        std::string out = senary::process(this, str);
        CCLabelBMFont::setString(out.c_str(), needUpdateLabel);
        if (senary::enabled()) senary::flipPernifGlyphs(this, out);
    }
    bool initWithString(char const* str, char const* fnt, float width,
                        cocos2d::CCTextAlignment alignment, cocos2d::CCPoint imageOffset) {
        std::string out = senary::process(this, str);
        if (!CCLabelBMFont::initWithString(out.c_str(), fnt, width, alignment, imageOffset))
            return false;
        if (senary::enabled()) senary::flipPernifGlyphs(this, out);
        return true;
    }
};

// --- targeted semantic hooks ----------------------------------------------

class $modify(SenaryPlayLayer, PlayLayer) {
    struct Fields {
        int bestWholePernif = -1;
    };

    void updatePernifLabel() {
        if (!m_percentageLabel) return;
        std::string current = m_percentageLabel->getString() ? m_percentageLabel->getString() : "";
        bool radix = current.find('.') != std::string::npos;
        double percent = static_cast<double>(this->getCurrentPercent());
        senary::setSenaryString(m_percentageLabel, senary::formatPernif(percent, radix));
    }

    // Keep the stored bests on the legal floored set, and remember the
    // whole-pernif best for New Best gating.
    void normalizeStoredBests() {
        if (!m_level) return;
        int normal = senary::normalizeSavedPercent(m_level->m_normalPercent.value());
        if (normal != m_level->m_normalPercent.value()) m_level->m_normalPercent = normal;
        int practice = senary::normalizeSavedPercent(m_level->m_practicePercent);
        if (practice != m_level->m_practicePercent) m_level->m_practicePercent = practice;
        int stored = m_isPracticeMode ? practice : normal;
        int whole = senary::pernifFromSavedPercent(stored);
        if (whole > m_fields->bestWholePernif) m_fields->bestWholePernif = whole;
    }

    void updateProgressbar() {
        PlayLayer::updateProgressbar();
        if (!senary::enabled()) return;
        updatePernifLabel();
        normalizeStoredBests();
    }

    void resetLevel() {
        PlayLayer::resetLevel();
        if (!senary::enabled()) return;
        updatePernifLabel();
        normalizeStoredBests();
    }

    void showNewBest(bool newReward, int orbs, int diamonds,
                     bool demonKey, bool noRetry, bool noTitle) {
        if (!senary::enabled()) {
            PlayLayer::showNewBest(newReward, orbs, diamonds, demonKey, noRetry, noTitle);
            return;
        }
        int wholeNow = static_cast<int>(std::floor(
            static_cast<double>(this->getCurrentPercent()) * 36.0 / 100.0 + 1e-9));
        if (wholeNow <= m_fields->bestWholePernif) {
            // Intra-pernif gain: no popup, and undo vanilla's raw save.
            normalizeStoredBests();
            return;
        }
        m_fields->bestWholePernif = wholeNow;
        normalizeStoredBests(); // floors the freshly written raw percent
        senary::s_rescalePercentTokens = true;
        PlayLayer::showNewBest(newReward, orbs, diamonds, demonKey, noRetry, noTitle);
        senary::s_rescalePercentTokens = false;
    }
};

class $modify(SenaryLevelInfoLayer, LevelInfoLayer) {
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

class $modify(SenaryPauseLayer, PauseLayer) {
    void setupProgressBars() {
        PauseLayer::setupProgressBars();
        if (!senary::enabled()) return;
        auto* play = PlayLayer::get();
        if (!play || !play->m_level) return;
        struct Target { char const* id; int percent; };
        Target targets[] = {
            { "normal-progress-label",   play->m_level->m_normalPercent.value() },
            { "practice-progress-label", play->m_level->m_practicePercent },
        };
        for (auto const& t : targets) {
            auto* label = typeinfo_cast<CCLabelBMFont*>(this->getChildByIDRecursive(t.id));
            if (!label) continue;
            int pernif = senary::pernifFromSavedPercent(t.percent);
            senary::rewritePercentTail(label, senary::toSenary(pernif) + "%");
        }
    }
};
