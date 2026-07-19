// Senary Numbers — v0.3.2 (second internal revision of the third compiled
// test version; senary versioning: v0.5 rolls over to v1.0)
//
// Changes from v0.3.0/.1:
// - Comma-grouped numbers ("178,540,243") parse as one value instead of
//   per-group digit conversion; output is quartet-grouped senary.
// - Vanilla K/M/B abbreviations re-abbreviate as unexians (U) / biexians
//   (B), one rounded senary radix digit. Where the true count is available
//   (LevelInfoLayer, LevelCell), labels are recomputed from the exact int;
//   the string-level fallback handles everything else. Below unexian the
//   full form always shows, even where vanilla abbreviated (1.1K -> 5032).
// - Endscreen time is T6: SI seconds * 27/50 = T6 seconds, shown as senary
//   h:mm:ss (ss always zero-padded, mm only when hours show), computed from the precise double
//   m_gameState.m_levelTime, not the rounded label.
// - Orb count labels hidden outside levels (LevelInfoLayer, LevelCell);
//   in-level collection animation untouched.
// - Labels inside CCTextInputNode are exempt from conversion — typed
//   digits display as typed.
// - LevelBrowserLayer's number popup (page jump etc.) reinterprets the
//   typed digits as senary on the backend: typing 100 jumps to page 36x.

#include <Geode/Geode.hpp>
#include <Geode/binding/CCTextInputNode.hpp>
#include <Geode/modify/CCLabelBMFont.hpp>
#include <Geode/modify/CCCounterLabel.hpp>
#include <Geode/modify/PlayLayer.hpp>
#include <Geode/modify/LevelInfoLayer.hpp>
#include <Geode/modify/PauseLayer.hpp>
#include <Geode/modify/EndLevelLayer.hpp>
#include <Geode/modify/LevelCell.hpp>
#include <Geode/modify/LevelBrowserLayer.hpp>

#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <unordered_map>

using namespace geode::prelude;

namespace senary {

static bool s_bypass = false;
static bool s_rescalePercentTokens = false;
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

// Quartet grouping from the right: "10344213" -> "1034 4213".
static std::string toSenaryGrouped(uint64_t v) {
    std::string s = toSenary(v);
    if (s.size() <= 4) return s;
    std::string out;
    size_t lead = s.size() % 4;
    if (lead) out = s.substr(0, lead);
    for (size_t i = lead; i < s.size(); i += 4) {
        if (!out.empty()) out += ' ';
        out += s.substr(i, 4);
    }
    return out;
}

static constexpr uint64_t UNEXIAN = 1296;      // 6^4
static constexpr uint64_t BIEXIAN = 1679616;   // 6^8

// Below unexian: full quartet-grouped senary, even where vanilla
// abbreviated (the 1,000-1,295 in-between zone: "1.1K" -> 5032).
// At unexian and above: "X.dU" / "X.dB", one senary radix digit, rounded.
static std::string senaryAbbrev(double v) {
    if (v < 0) v = 0;
    if (v < static_cast<double>(UNEXIAN))
        return toSenaryGrouped(static_cast<uint64_t>(std::llround(v)));
    uint64_t base = (v >= static_cast<double>(BIEXIAN)) ? BIEXIAN : UNEXIAN;
    char suffix = (base == BIEXIAN) ? 'B' : 'U';
    uint64_t sixths = static_cast<uint64_t>(std::llround(v * 6.0 / static_cast<double>(base)));
    if (suffix == 'U' && sixths >= 6 * UNEXIAN) { // rounded up across the boundary
        base = BIEXIAN;
        suffix = 'B';
        sixths = static_cast<uint64_t>(std::llround(v * 6.0 / static_cast<double>(base)));
    }
    std::string s = toSenary(sixths / 6);
    s += '.';
    s += static_cast<char>('0' + sixths % 6);
    s += suffix;
    return s;
}

// --- pernif math ----------------------------------------------------------

static int pernifFromSavedPercent(int percent) {
    if (percent >= 100) return 36;
    if (percent <= 0) return 0;
    return static_cast<int>(std::ceil(percent * 36.0 / 100.0 - 1e-9));
}

static int savedPercentForPernif(int wholePernif) {
    if (wholePernif >= 36) return 100;
    if (wholePernif <= 0) return 0;
    return static_cast<int>(std::floor(wholePernif * 100.0 / 36.0 + 1e-9));
}

static bool isLegalSavedPercent(int p) {
    return savedPercentForPernif(pernifFromSavedPercent(p)) == p;
}

static int normalizeSavedPercent(int p) {
    if (isLegalSavedPercent(p)) return p;
    int whole = static_cast<int>(std::floor(p * 36.0 / 100.0 + 1e-9));
    return savedPercentForPernif(whole);
}

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

// --- T6 time --------------------------------------------------------------

// SI day = 86400 s; T6 day = 36h * 36min * 36s = 46656 T6-seconds.
// T6 seconds = SI seconds * 46656/86400 = SI * 27/50, exactly.
static std::string formatT6Time(double siSeconds) {
    if (siSeconds < 0) siSeconds = 0;
    double t6 = siSeconds * 27.0 / 50.0;
    uint64_t total = static_cast<uint64_t>(std::floor(t6 + 1e-9));
    uint64_t h = total / 1296;
    uint64_t m = (total / 36) % 36;
    uint64_t s = total % 36;
    auto pad2 = [](uint64_t v) {
        std::string d = toSenary(v);
        return d.size() < 2 ? "0" + d : d;
    };
    std::string out;
    if (h > 0) out = toSenary(h) + ":" + pad2(m);
    else out = toSenary(m);
    out += ":" + pad2(s);
    return out;
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

// Try to match a comma-grouped number at position i: \d{1,3}(,\d{3})+ with
// standalone boundaries. On success sets value/end and returns true.
static bool matchCommaNumber(std::string const& in, size_t i, uint64_t& value, size_t& end) {
    size_t const n = in.size();
    size_t p = i;
    size_t firstLen = 0;
    while (p < n && std::isdigit(static_cast<unsigned char>(in[p]))) { ++p; ++firstLen; }
    if (firstLen == 0 || firstLen > 3) return false;
    if (p >= n || in[p] != ',') return false;
    std::string digits = in.substr(i, firstLen);
    size_t q = p;
    while (q + 3 < n && in[q] == ',' &&
           std::isdigit(static_cast<unsigned char>(in[q + 1])) &&
           std::isdigit(static_cast<unsigned char>(in[q + 2])) &&
           std::isdigit(static_cast<unsigned char>(in[q + 3]))) {
        digits += in.substr(q + 1, 3);
        q += 4;
    }
    if (q == p) return false; // no full ,ddd group followed
    if (q < n && (std::isdigit(static_cast<unsigned char>(in[q])) || std::isalpha(static_cast<unsigned char>(in[q]))))
        return false;
    if (i > 0 && std::isalnum(static_cast<unsigned char>(in[i - 1]))) return false;
    if (digits.size() > 15) return false;
    value = std::stoull(digits);
    end = q;
    return true;
}

// Try to match an abbreviated number at position i: \d+(\.\d+)?[KMB] with
// standalone boundaries. On success sets value/end and returns true.
static bool matchAbbrevNumber(std::string const& in, size_t i, double& value, size_t& end) {
    size_t const n = in.size();
    size_t p = i;
    while (p < n && std::isdigit(static_cast<unsigned char>(in[p]))) ++p;
    if (p == i || p - i > 12) return false;
    size_t fracStart = 0, fracLen = 0;
    if (p < n && in[p] == '.') {
        fracStart = p + 1;
        size_t q = fracStart;
        while (q < n && std::isdigit(static_cast<unsigned char>(in[q]))) ++q;
        if (q == fracStart) return false;
        fracLen = q - fracStart;
        p = q;
    }
    if (p >= n) return false;
    char suf = in[p];
    if (suf != 'K' && suf != 'M' && suf != 'B') return false;
    if (p + 1 < n && std::isalnum(static_cast<unsigned char>(in[p + 1]))) return false;
    if (i > 0 && std::isalnum(static_cast<unsigned char>(in[i - 1]))) return false;
    double v = std::stod(in.substr(i, p - i));
    v *= (suf == 'K') ? 1e3 : (suf == 'M') ? 1e6 : 1e9;
    value = v;
    end = p + 1;
    return true;
}

static std::string convertText(std::string const& in) {
    std::string out;
    out.reserve(in.size() + 8);
    size_t i = 0;
    size_t const n = in.size();
    while (i < n) {
        unsigned char c = static_cast<unsigned char>(in[i]);
        if (std::isdigit(c)) {
            uint64_t commaVal; size_t commaEnd;
            double abbrevVal; size_t abbrevEnd;
            if (matchCommaNumber(in, i, commaVal, commaEnd)) {
                out += toSenaryGrouped(commaVal);
                i = commaEnd;
                continue;
            }
            if (matchAbbrevNumber(in, i, abbrevVal, abbrevEnd)) {
                out += senaryAbbrev(abbrevVal);
                i = abbrevEnd;
                continue;
            }
            size_t start = i;
            while (i < n && std::isdigit(static_cast<unsigned char>(in[i]))) ++i;
            size_t len = i - start;
            bool leftOk  = start == 0 || !std::isalnum(static_cast<unsigned char>(in[start - 1]));
            bool rightOk = i == n     || !std::isalnum(static_cast<unsigned char>(in[i]));
            if (leftOk && rightOk && len <= 15) {
                uint64_t v = std::stoull(in.substr(start, len));
                if (s_rescalePercentTokens && i < n && in[i] == '%' && v <= 100) {
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

static void flipPernifGlyphs(CCLabelBMFont* label, std::string const& text) {
    for (size_t i = 0; i < text.size(); ++i) {
        auto* glyph = typeinfo_cast<CCSprite*>(label->getChildByTag(static_cast<int>(i)));
        if (glyph) glyph->setFlipX(text[i] == '%');
    }
}

// Labels inside a text input display what the user typed, untouched.
static bool insideTextInput(CCLabelBMFont* label) {
    CCNode* node = label;
    for (int depth = 0; node && depth < 5; ++depth) {
        if (typeinfo_cast<CCTextInputNode*>(node)) return true;
        node = node->getParent();
    }
    return false;
}

static std::string process(CCLabelBMFont* label, char const* raw) {
    std::string in = raw ? raw : "";
    if (!enabled()) return in;
    if (s_bypass) {
        s_lastOutput[label] = in;
        return in;
    }
    if (insideTextInput(label)) return in;
    auto it = s_lastOutput.find(label);
    if (it != s_lastOutput.end() && it->second == in) return in;
    std::string out = convertText(in);
    s_lastOutput[label] = out;
    if (debugLog() && out != in) log::debug("senary: \"{}\" -> \"{}\"", in, out);
    return out;
}

static void setSenaryString(CCLabelBMFont* label, std::string const& s) {
    s_bypass = true;
    label->setString(s.c_str());
    s_bypass = false;
    flipPernifGlyphs(label, s);
}

// Keep the non-numeric prefix, replace everything from the first digit on.
static void rewriteFromFirstDigit(CCLabelBMFont* label, std::string const& replacement) {
    std::string cur = label->getString() ? label->getString() : "";
    size_t first = cur.find_first_of("0123456789");
    if (first == std::string::npos) first = cur.size();
    setSenaryString(label, cur.substr(0, first) + replacement);
}

// Did vanilla abbreviate this label? (any K/M/B directly after a digit)
static bool labelLooksAbbreviated(CCLabelBMFont* label) {
    std::string cur = label->getString() ? label->getString() : "";
    for (size_t i = 1; i < cur.size(); ++i) {
        if ((cur[i] == 'K' || cur[i] == 'M' || cur[i] == 'B') &&
            std::isdigit(static_cast<unsigned char>(cur[i - 1]))) return true;
    }
    return false;
}

// Vanilla full stays full at any size; vanilla abbreviated re-abbreviates
// (falling back to full below unexian).
static std::string formatCount(CCLabelBMFont* label, int count) {
    uint64_t v = static_cast<uint64_t>(std::max(0, count));
    return labelLooksAbbreviated(label) ? senaryAbbrev(static_cast<double>(v))
                                        : toSenaryGrouped(v);
}

static void rewritePercentTail(CCLabelBMFont* label, std::string const& pernif) {
    std::string cur = label->getString() ? label->getString() : "";
    size_t pct = cur.rfind('%');
    if (pct == std::string::npos) return;
    size_t start = pct;
    while (start > 0) {
        unsigned char prev = static_cast<unsigned char>(cur[start - 1]);
        if (std::isdigit(prev) || prev == '.') --start; else break;
    }
    if (start == pct) return;
    setSenaryString(label, cur.substr(0, start) + pernif);
}

// Typed digits are senary. Reinterpret an int the game parsed as decimal:
// its decimal digit string, read base 6. Digits 6-9 mean the input wasn't
// valid senary; pass it through unchanged.
static int reinterpretTypedAsSenary(int value) {
    if (value < 0) return value;
    std::string d = std::to_string(value);
    int out = 0;
    for (char ch : d) {
        if (ch > '5') return value;
        out = out * 6 + (ch - '0');
    }
    return out;
}

} // namespace senary

// --- global backbone ------------------------------------------------------

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

class $modify(SenaryCounterLabel, CCCounterLabel) {
    void updateString() {
        CCCounterLabel::updateString();
        if (!senary::enabled()) return;
        int v = m_currentCount;
        if (v < 0) return;
        senary::setSenaryString(this, senary::toSenaryGrouped(static_cast<uint64_t>(v)));
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
            normalizeStoredBests();
            return;
        }
        m_fields->bestWholePernif = wholeNow;
        normalizeStoredBests();
        senary::s_rescalePercentTokens = true;
        PlayLayer::showNewBest(newReward, orbs, diamonds, demonKey, noRetry, noTitle);
        senary::s_rescalePercentTokens = false;
    }
};

// Level page: pernifage bars, exact download/like counts, hidden orb count.
class $modify(SenaryLevelInfoLayer, LevelInfoLayer) {
    void setupProgressBars() {
        LevelInfoLayer::setupProgressBars();
        if (!senary::enabled() || !m_level) return;
        Ref<LevelInfoLayer> self(this);
        Loader::get()->queueInMainThread([self] {
            if (!self->m_level) return;
            struct Target { char const* id; int percent; };
            Target targets[] = {
                { "normal-mode-percentage",   self->m_level->m_normalPercent.value() },
                { "practice-mode-percentage", self->m_level->m_practicePercent },
            };
            for (auto const& t : targets) {
                auto* label = typeinfo_cast<CCLabelBMFont*>(self->getChildByIDRecursive(t.id));
                if (!label) continue;
                int pernif = senary::pernifFromSavedPercent(t.percent);
                senary::setSenaryString(label, senary::toSenary(pernif) + "%");
            }
            if (self->m_downloadsLabel)
                senary::setSenaryString(self->m_downloadsLabel,
                    senary::formatCount(self->m_downloadsLabel, self->m_level->m_downloads));
            if (self->m_likesLabel)
                senary::setSenaryString(self->m_likesLabel,
                    senary::formatCount(self->m_likesLabel, self->m_level->m_likes));
            if (self->m_orbsLabel) self->m_orbsLabel->setVisible(false);
        });
    }
};

class $modify(SenaryPauseLayer, PauseLayer) {
    void setupProgressBars() {
        PauseLayer::setupProgressBars();
        if (!senary::enabled()) return;
        Ref<PauseLayer> self(this);
        Loader::get()->queueInMainThread([self] {
            auto* play = PlayLayer::get();
            if (!play || !play->m_level) return;
            struct Target { char const* id; int percent; };
            Target targets[] = {
                { "normal-progress-label",   play->m_level->m_normalPercent.value() },
                { "practice-progress-label", play->m_level->m_practicePercent },
            };
            for (auto const& t : targets) {
                auto* label = typeinfo_cast<CCLabelBMFont*>(self->getChildByIDRecursive(t.id));
                if (!label) continue;
                int pernif = senary::pernifFromSavedPercent(t.percent);
                senary::rewritePercentTail(label, senary::toSenary(pernif) + "%");
            }
        });
    }
};

// Endscreen: T6 time from the precise level-time double.
class $modify(SenaryEndLevelLayer, EndLevelLayer) {
    void customSetup() {
        EndLevelLayer::customSetup();
        if (!senary::enabled()) return;
        Ref<EndLevelLayer> self(this);
        Loader::get()->queueInMainThread([self] {
            auto* play = PlayLayer::get();
            if (!play) return;
            auto* label = typeinfo_cast<CCLabelBMFont*>(self->getChildByIDRecursive("time-label"));
            if (!label) return;
            senary::rewriteFromFirstDigit(label,
                senary::formatT6Time(play->m_gameState.m_levelTime));
        });
    }
};

// Browser cells: exact counts, hidden orb count.
class $modify(SenaryLevelCell, LevelCell) {
    void loadCustomLevelCell() {
        LevelCell::loadCustomLevelCell();
        if (!senary::enabled() || !m_level) return;
        Ref<LevelCell> self(this);
        Loader::get()->queueInMainThread([self] {
            if (!self->m_level) return;
            struct Target { char const* id; int count; };
            Target targets[] = {
                { "downloads-label", self->m_level->m_downloads },
                { "likes-label",     self->m_level->m_likes },
            };
            for (auto const& t : targets) {
                auto* label = typeinfo_cast<CCLabelBMFont*>(self->getChildByIDRecursive(t.id));
                if (!label) continue;
                senary::setSenaryString(label, senary::formatCount(label, t.count));
            }
            if (auto* orbs = typeinfo_cast<CCLabelBMFont*>(self->getChildByIDRecursive("orbs-label")))
                orbs->setVisible(false);
        });
    }
};

// Number-entry popups reaching LevelBrowserLayer (page jump, folders):
// typed digits are senary; the game gets the decimal value.
class $modify(SenaryLevelBrowserLayer, LevelBrowserLayer) {
    void setIDPopupClosed(SetIDPopup* popup, int value) {
        if (senary::enabled()) value = senary::reinterpretTypedAsSenary(value);
        LevelBrowserLayer::setIDPopupClosed(popup, value);
    }
};
