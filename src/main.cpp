// Senary Numbers — v0.4.2
//
// Changes from v0.4.1:
// - Time fractions now carry FOUR senary radix digits (untisecond
//   precision, 1/1296 T6 s); the endscreen uses the tick-precise level
//   time double (GD runs 240x ticks per SI second — well within a
//   double), showing fractions when vanilla did.
// - Daily/weekly/event countdown refreshes on its own fast schedule with
//   interpolated remaining time, so the T6 seconds tick at T6 cadence
//   instead of freezing ~1.85 SI s per value on vanilla's SI-second
//   updates.
// - Comment metadata (likes, date, percentage) re-converts inside the
//   otherwise-exempt CommentCell; username and comment text stay raw.
// - In-level pernifage moves to the LEFT of the progress bar, right-
//   aligned (anchor at the % sign), so the pernif sign stays fixed while
//   the number's width fluctuates leftward. Applies only when the
//   progress bar is shown.
//
// Changes from v0.4:
// - Generic time tokens ("12:34:56", "1:23", "0:58.123") convert to T6
//   everywhere: covers daily/weekly/event countdowns and platformer best
//   times on cells and leaderboards. Fractions render as two senary radix
//   digits (snaps, blips).
// - User-content exemption: labels inside CommentCell subtrees, and
//   TextArea content under LevelInfoLayer (the level description), are no
//   longer converted.
// - Editor senary input (setting, default on): typing in any
//   SetupTriggerPopup-derived trigger field is interpreted as senary —
//   including senary radix fractions ("1.3" = 1.5x) — while the display
//   keeps the typed digits. Known caveat: ID fields inside trigger popups
//   (e.g. song/SFX IDs) are indistinguishable from value fields and get
//   reinterpreted too; turn the setting off if that bites, and report
//   which fields need exemption.
// - Top-100 tab rename retries against an unconverted "100" label as well;
//   if the tab is a baked texture (v0.3 evidence suggests so), renaming
//   needs a replacement texture resource — see notes.
//
// Changes from v0.3 (compiled) / v0.3.2:
// - Quartet separator is a comma, and ALL converted numbers of five or more
//   senary digits are comma-grouped (except protected IDs).
// - Global pernif rule: any "N%" or "N.NN%" token anywhere in the game is
//   semantically rescaled to pernifage, not digit-converted. This replaces
//   the New Best-scoped rescale flag and fixes every remaining "244%" site
//   blind. Integer percents that are legal floored saves recover via
//   ceiling; anything else (e.g. the current-attempt percent vanilla feeds
//   the New Best popup) floors — fixing the 25.15-shows-30 off-by-one.
// - "ID:"-prefixed digit runs are left decimal (Level ID, Song ID).
// - Top 100 leaderboard: the server caps getGJScores at 100 entries
//   (documented), so it is clamped to two-nif — entries beyond rank 72x are
//   dropped, and the tab label's "244" is renamed to "200".
// - Orb icons hidden alongside orb counts (level page + browser cells).
// - Stat rows repack after rewriting: each icon/label to the right of a
//   label that changed width shifts by the accumulated delta, so download
//   counts no longer overlap the like icon.
// - User-content restores: level names and creator names on cells and the
//   level page are rewritten back from the source strings (m_levelName /
//   m_creatorName), undoing digit conversion of user-generated names.

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
#include <Geode/modify/LeaderboardsLayer.hpp>
#include <Geode/modify/SetupTriggerPopup.hpp>
#include <Geode/modify/CommentCell.hpp>
#include <Geode/modify/DailyLevelPage.hpp>
#include <Geode/binding/CommentCell.hpp>
#include <Geode/binding/TextArea.hpp>
#include <Geode/binding/GJComment.hpp>
#include <Geode/modify/CommentCell.hpp>
#include <Geode/modify/DailyLevelPage.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <string>
#include <chrono>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;

namespace senary {

static bool s_bypass = false;
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

// Comma-separated quartets from the right: "10344213" -> "1034,4213".
// Applied to every converted number of 5+ senary digits.
static std::string toSenaryGrouped(uint64_t v) {
    std::string s = toSenary(v);
    if (s.size() <= 4) return s;
    std::string out;
    size_t lead = s.size() % 4;
    if (lead) out = s.substr(0, lead);
    for (size_t i = lead; i < s.size(); i += 4) {
        if (!out.empty()) out += ',';
        out += s.substr(i, 4);
    }
    return out;
}

static constexpr uint64_t UNEXIAN = 1296;      // 6^4
static constexpr uint64_t BIEXIAN = 1679616;   // 6^8

// Below unexian: full grouped senary, even where vanilla abbreviated.
// At unexian and above: "X.dU" / "X.dB", one senary radix digit, rounded.
static std::string senaryAbbrev(double v) {
    if (v < 0) v = 0;
    if (v < static_cast<double>(UNEXIAN))
        return toSenaryGrouped(static_cast<uint64_t>(std::llround(v)));
    uint64_t base = (v >= static_cast<double>(BIEXIAN)) ? BIEXIAN : UNEXIAN;
    char suffix = (base == BIEXIAN) ? 'B' : 'U';
    uint64_t sixths = static_cast<uint64_t>(std::llround(v * 6.0 / static_cast<double>(base)));
    if (suffix == 'U' && sixths >= 6 * UNEXIAN) {
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

// Ceiling recovery for values saved by this mod's floored scheme.
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

// Whole-pernif display for an integer percent of unknown provenance:
// legal floored-save values round-trip via ceiling; anything else is a raw
// percent and floors (48% is 17 whole pernif, not 18).
static int displayPernifForPercent(int p) {
    if (p >= 100) return 36;
    if (p <= 0) return 0;
    if (isLegalSavedPercent(p)) return pernifFromSavedPercent(p);
    return static_cast<int>(std::floor(p * 36.0 / 100.0 + 1e-9));
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

// T6 seconds = SI seconds * 27/50, exactly. withFraction appends four
// senary radix digits (untisecond precision, 1/1296 T6 second).
static std::string formatT6Time(double siSeconds, bool withFraction = false) {
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
    if (withFraction) {
        int frac = static_cast<int>(std::floor((t6 - std::floor(t6)) * 1296.0 + 1e-6));
        if (frac < 0) frac = 0;
        if (frac > 1295) frac = 1295;
        std::string f = toSenary(static_cast<uint64_t>(frac));
        while (f.size() < 4) f.insert(f.begin(), '0');
        out += '.';
        out += f;
    }
    return out;
}

// Senary float from typed text ("1.3" = 1.5x, "-20" = -12x). False if the
// text contains digits 6-9 or isn't a plain number.
static bool parseSenaryFloat(std::string const& s, double& out) {
    if (s.empty()) return false;
    size_t i = 0;
    bool neg = false;
    if (s[0] == '-') { neg = true; i = 1; }
    if (i >= s.size()) return false;
    double value = 0;
    bool anyDigit = false, seenDot = false;
    double scale = 1.0 / 6.0;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c == '.') {
            if (seenDot) return false;
            seenDot = true;
        } else if (c >= '0' && c <= '5') {
            anyDigit = true;
            if (!seenDot) value = value * 6 + (c - '0');
            else { value += (c - '0') * scale; scale /= 6.0; }
        } else {
            return false; // includes 6-9: not senary input
        }
    }
    if (!anyDigit) return false;
    out = neg ? -value : value;
    return true;
}

static std::string formatDecimalForGame(double v) {
    if (std::fabs(v - std::llround(v)) < 1e-9)
        return std::to_string(static_cast<long long>(std::llround(v)));
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.6f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
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

// Runs preceded by "ID:" (any case, optional spaces) stay decimal.
static bool isIdProtected(std::string const& in, size_t runStart) {
    size_t j = runStart;
    while (j > 0 && in[j - 1] == ' ') --j;
    if (j < 3) return false;
    char a = in[j - 3], b = in[j - 2], c = in[j - 1];
    return (a == 'I' || a == 'i') && (b == 'D' || b == 'd') && c == ':';
}

// Already-converted senary quartets ("254,1442,3111") pass through — every
// digit <= 5 and comma groups of exactly four.
static bool matchSenaryQuartets(std::string const& in, size_t i, size_t& end) {
    size_t const n = in.size();
    size_t p = i;
    size_t firstLen = 0;
    bool allSenary = true;
    while (p < n && std::isdigit(static_cast<unsigned char>(in[p]))) {
        if (in[p] > '5') allSenary = false;
        ++p; ++firstLen;
    }
    if (!allSenary || firstLen == 0 || firstLen > 4) return false;
    size_t q = p;
    bool any = false;
    while (q + 4 < n + 1 && q + 4 <= n && in[q] == ',') {
        bool group = true;
        for (int k = 1; k <= 4; ++k) {
            char ch = q + k < n ? in[q + k] : 'x';
            if (!(ch >= '0' && ch <= '5')) { group = false; break; }
        }
        if (!group) break;
        q += 5;
        any = true;
    }
    if (!any) return false;
    if (q < n && (std::isalnum(static_cast<unsigned char>(in[q])) || in[q] == ',')) return false;
    if (i > 0 && std::isalnum(static_cast<unsigned char>(in[i - 1]))) return false;
    end = q;
    return true;
}

// Decimal comma-grouped number: \d{1,3}(,\d{3})+ with standalone boundaries.
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
        // group must be exactly 3 digits (a 4th digit means it isn't decimal grouping)
        if (q + 4 < n && std::isdigit(static_cast<unsigned char>(in[q + 4]))) return false;
        digits += in.substr(q + 1, 3);
        q += 4;
    }
    if (q == p) return false;
    if (q < n && (std::isdigit(static_cast<unsigned char>(in[q])) || std::isalpha(static_cast<unsigned char>(in[q]))))
        return false;
    if (i > 0 && std::isalnum(static_cast<unsigned char>(in[i - 1]))) return false;
    if (digits.size() > 15) return false;
    value = std::stoull(digits);
    end = q;
    return true;
}

// Vanilla abbreviation: \d+(\.\d+)?[KMB] with standalone boundaries.
static bool matchAbbrevNumber(std::string const& in, size_t i, double& value, size_t& end) {
    size_t const n = in.size();
    size_t p = i;
    while (p < n && std::isdigit(static_cast<unsigned char>(in[p]))) ++p;
    if (p == i || p - i > 12) return false;
    if (p < n && in[p] == '.') {
        size_t q = p + 1;
        while (q < n && std::isdigit(static_cast<unsigned char>(in[q]))) ++q;
        if (q == p + 1) return false;
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

// Time token: D+:DD[:DD][.D+] with sane sexagesimal parts -> T6 time.
// Covers countdowns (h:mm:ss) and platformer times (m:ss.mmm).
static bool matchTimeToken(std::string const& in, size_t i, std::string& out, size_t& end) {
    size_t const n = in.size();
    auto readDigits = [&](size_t& p, size_t maxLen) -> int {
        size_t start = p;
        while (p < n && p - start < maxLen && std::isdigit(static_cast<unsigned char>(in[p]))) ++p;
        if (p == start) return -1;
        return std::stoi(in.substr(start, p - start));
    };
    size_t p = i;
    int a = readDigits(p, 6);
    if (a < 0 || p >= n || in[p] != ':') return false;
    ++p;
    size_t bStart = p;
    int b = readDigits(p, 2);
    if (b < 0 || p - bStart != 2) return false;
    int c = -1;
    if (p < n && in[p] == ':') {
        size_t save = p;
        ++p;
        size_t cStart = p;
        c = readDigits(p, 2);
        if (c < 0 || p - cStart != 2) { p = save; c = -1; }
    }
    double frac = 0;
    bool hasFrac = false;
    if (p < n && in[p] == '.') {
        size_t save = p;
        ++p;
        size_t fStart = p;
        while (p < n && p - fStart < 3 && std::isdigit(static_cast<unsigned char>(in[p]))) ++p;
        if (p > fStart) {
            hasFrac = true;
            frac = std::stod("0." + in.substr(fStart, p - fStart));
        } else {
            p = save;
        }
    }
    // boundaries: not embedded in a longer token
    if (i > 0 && (std::isalnum(static_cast<unsigned char>(in[i - 1])) || in[i - 1] == ':' || in[i - 1] == '.'))
        return false;
    if (p < n && (std::isalnum(static_cast<unsigned char>(in[p])) || in[p] == ':')) return false;
    // sexagesimal sanity
    if (c >= 0) { if (b > 59 || c > 59) return false; }
    else        { if (b > 59) return false; }
    double si = (c >= 0) ? (a * 3600.0 + b * 60.0 + c) : (a * 60.0 + b);
    si += frac;
    out = formatT6Time(si, hasFrac);
    end = p;
    return true;
}

// Percentage token: \d+(\.\d+)?% — semantically rescaled to pernifage.
static bool matchPercentToken(std::string const& in, size_t i, std::string& out, size_t& end) {
    size_t const n = in.size();
    size_t p = i;
    while (p < n && std::isdigit(static_cast<unsigned char>(in[p]))) ++p;
    if (p == i || p - i > 6) return false;
    size_t intEnd = p;
    bool frac = false;
    if (p < n && in[p] == '.') {
        size_t q = p + 1;
        while (q < n && std::isdigit(static_cast<unsigned char>(in[q]))) ++q;
        if (q > p + 1) { frac = true; p = q; }
    }
    if (p >= n || in[p] != '%') return false;
    if (i > 0 && std::isalnum(static_cast<unsigned char>(in[i - 1]))) return false;
    double value = std::stod(in.substr(i, p - i));
    if (value > 100.0) return false; // not a progress percentage
    if (frac) {
        out = formatPernif(value, true);
    } else {
        int whole = displayPernifForPercent(static_cast<int>(std::llround(value)));
        out = toSenary(static_cast<uint64_t>(whole)) + "%";
    }
    (void)intEnd;
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
            size_t end;
            if (isIdProtected(in, i)) {
                while (i < n && std::isdigit(static_cast<unsigned char>(in[i]))) out += in[i++];
                continue;
            }
            if (matchSenaryQuartets(in, i, end)) {
                out += in.substr(i, end - i);
                i = end;
                continue;
            }
            std::string t6;
            if (matchTimeToken(in, i, t6, end)) {
                out += t6;
                i = end;
                continue;
            }
            std::string pernif;
            if (matchPercentToken(in, i, pernif, end)) {
                out += pernif;
                i = end;
                continue;
            }
            uint64_t commaVal;
            if (matchCommaNumber(in, i, commaVal, end)) {
                out += toSenaryGrouped(commaVal);
                i = end;
                continue;
            }
            double abbrevVal;
            if (matchAbbrevNumber(in, i, abbrevVal, end)) {
                out += senaryAbbrev(abbrevVal);
                i = end;
                continue;
            }
            size_t start = i;
            while (i < n && std::isdigit(static_cast<unsigned char>(in[i]))) ++i;
            size_t len = i - start;
            bool leftOk  = start == 0 || !std::isalnum(static_cast<unsigned char>(in[start - 1]));
            bool rightOk = i == n     || !std::isalnum(static_cast<unsigned char>(in[i]));
            if (leftOk && rightOk && len <= 15) {
                out += toSenaryGrouped(std::stoull(in.substr(start, len)));
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

static bool insideTextInput(CCLabelBMFont* label) {
    CCNode* node = label;
    for (int depth = 0; node && depth < 5; ++depth) {
        if (typeinfo_cast<CCTextInputNode*>(node)) return true;
        node = node->getParent();
    }
    return false;
}

// User-generated text stays untouched: anything inside a CommentCell, and
// TextArea content under LevelInfoLayer (the level description). Other
// TextAreas (info dialogs, end text) still convert.
static bool isUserContent(CCLabelBMFont* label) {
    bool sawTextArea = false;
    CCNode* node = label;
    for (int depth = 0; node && depth < 10; ++depth) {
        if (typeinfo_cast<CommentCell*>(node)) return true;
        if (typeinfo_cast<TextArea*>(node)) sawTextArea = true;
        if (sawTextArea && typeinfo_cast<LevelInfoLayer*>(node)) return true;
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
    if (isUserContent(label)) return in;
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

static void rewriteFromFirstDigit(CCLabelBMFont* label, std::string const& replacement) {
    std::string cur = label->getString() ? label->getString() : "";
    size_t first = cur.find_first_of("0123456789");
    if (first == std::string::npos) first = cur.size();
    setSenaryString(label, cur.substr(0, first) + replacement);
}

// Vanilla full stays full; vanilla abbreviated re-abbreviates.
static bool labelLooksAbbreviated(CCLabelBMFont* label) {
    std::string cur = label->getString() ? label->getString() : "";
    for (size_t i = 1; i < cur.size(); ++i) {
        if ((cur[i] == 'K' || cur[i] == 'M' || cur[i] == 'B') &&
            std::isdigit(static_cast<unsigned char>(cur[i - 1]))) return true;
    }
    return false;
}

static std::string formatCount(CCLabelBMFont* label, int count) {
    uint64_t v = static_cast<uint64_t>(std::max(0, count));
    return labelLooksAbbreviated(label) ? senaryAbbrev(static_cast<double>(v))
                                        : toSenaryGrouped(v);
}

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

// --- stat-row repacking -----------------------------------------------------

// Snapshot a horizontal row of icons/labels (nodes within 15px of the
// reference node's y), then after rewrites shift everything right of a
// grown label by the accumulated width delta. Label left edges stay put.
struct RowSnapshot {
    std::vector<std::pair<CCNode*, float>> nodes; // node, original x
    std::unordered_map<CCLabelBMFont*, float> oldWidths;
};

static RowSnapshot snapshotRow(std::vector<CCNode*> const& candidates, CCNode* reference) {
    RowSnapshot snap;
    if (!reference) return snap;
    float y0 = reference->getPositionY();
    for (auto* n : candidates) {
        if (!n || !n->isVisible()) continue;
        if (std::fabs(n->getPositionY() - y0) > 15.f) continue;
        snap.nodes.push_back({ n, n->getPositionX() });
        if (auto* l = typeinfo_cast<CCLabelBMFont*>(n))
            snap.oldWidths[l] = l->getScaledContentSize().width;
    }
    std::sort(snap.nodes.begin(), snap.nodes.end(),
              [](auto const& a, auto const& b) { return a.second < b.second; });
    return snap;
}

static void repackRow(RowSnapshot const& snap) {
    float shift = 0.f;
    for (auto const& [node, x] : snap.nodes) {
        float nx = x + shift;
        if (auto* l = typeinfo_cast<CCLabelBMFont*>(node)) {
            auto it = snap.oldWidths.find(l);
            if (it != snap.oldWidths.end()) {
                float delta = l->getScaledContentSize().width - it->second;
                nx += delta * l->getAnchorPoint().x; // keep left edge fixed
                shift += delta;
            }
        }
        node->setPositionX(nx);
    }
}

// Restore a user-content label to its source string, bypassing conversion.
static void restoreLabel(CCLabelBMFont* label, std::string const& original) {
    if (!label || original.empty()) return;
    setSenaryString(label, original);
}

// Find the first CCLabelBMFont in a subtree (for labels inside buttons).
static CCLabelBMFont* findLabelIn(CCNode* root) {
    if (!root) return nullptr;
    if (auto* l = typeinfo_cast<CCLabelBMFont*>(root)) return l;
    auto* children = root->getChildren();
    if (!children) return nullptr;
    for (int i = 0; i < static_cast<int>(root->getChildrenCount()); ++i) {
        if (auto* found = findLabelIn(static_cast<CCNode*>(children->objectAtIndex(i))))
            return found;
    }
    return nullptr;
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
        // With the progress bar shown, pin the pernifage to its LEFT,
        // right-aligned: the pernif sign stays fixed while the number's
        // width fluctuates leftward.
        if (m_progressBar && m_progressBar->isVisible()) {
            m_percentageLabel->setAnchorPoint({ 1.f, 0.5f });
            float left = m_progressBar->getPositionX()
                - m_progressBar->getScaledContentSize().width * m_progressBar->getAnchorPoint().x;
            m_percentageLabel->setPosition({ left - 5.f, m_progressBar->getPositionY() });
        }
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
        // The popup's percent tokens are rescaled by the global pernif rule.
        PlayLayer::showNewBest(newReward, orbs, diamonds, demonKey, noRetry, noTitle);
    }
};

// Level page: pernifage bars, exact counts, hidden orbs, name restores,
// row repack.
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

            // Hide orb count and icon before snapshotting the row.
            if (self->m_orbsLabel) self->m_orbsLabel->setVisible(false);
            if (self->m_orbsIcon)  self->m_orbsIcon->setVisible(false);

            std::vector<CCNode*> row = {
                self->getChildByIDRecursive("downloads-icon"),
                self->m_downloadsLabel,
                self->m_likesIcon,
                self->m_likesLabel,
                self->getChildByIDRecursive("length-icon"),
                self->m_lengthLabel,
                self->m_exactLengthLabel,
                self->m_orbsIcon,
                self->m_orbsLabel,
            };
            auto snap = senary::snapshotRow(row, self->m_downloadsLabel);

            if (self->m_downloadsLabel)
                senary::setSenaryString(self->m_downloadsLabel,
                    senary::formatCount(self->m_downloadsLabel, self->m_level->m_downloads));
            if (self->m_likesLabel)
                senary::setSenaryString(self->m_likesLabel,
                    senary::formatCount(self->m_likesLabel, self->m_level->m_likes));

            senary::repackRow(snap);

            // User-content restores.
            senary::restoreLabel(
                typeinfo_cast<CCLabelBMFont*>(self->getChildByIDRecursive("title-label")),
                self->m_level->m_levelName);
            if (auto* creatorBtn = self->getChildByIDRecursive("creator-name")) {
                std::string name = self->m_level->m_creatorName;
                if (!name.empty())
                    senary::restoreLabel(senary::findLabelIn(creatorBtn), "By " + name);
            }
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
            std::string cur = label->getString() ? label->getString() : "";
            bool frac = cur.find('.') != std::string::npos;
            senary::rewriteFromFirstDigit(label,
                senary::formatT6Time(play->m_gameState.m_levelTime, frac));
        });
    }
};

// Browser cells: exact counts, hidden orbs, name restores, row repack.
class $modify(SenaryLevelCell, LevelCell) {
    void loadCustomLevelCell() {
        LevelCell::loadCustomLevelCell();
        if (!senary::enabled() || !m_level) return;
        Ref<LevelCell> self(this);
        Loader::get()->queueInMainThread([self] {
            if (!self->m_level) return;

            auto findLabel = [&](char const* id) {
                return typeinfo_cast<CCLabelBMFont*>(self->getChildByIDRecursive(id));
            };

            auto* orbsLabel = findLabel("orbs-label");
            auto* orbsIcon  = typeinfo_cast<CCNode*>(self->getChildByIDRecursive("orbs-icon"));
            if (orbsLabel) orbsLabel->setVisible(false);
            if (orbsIcon)  orbsIcon->setVisible(false);

            auto* downloads = findLabel("downloads-label");
            auto* likes     = findLabel("likes-label");

            std::vector<CCNode*> row = {
                self->getChildByIDRecursive("length-icon"),
                findLabel("length-label"),
                self->getChildByIDRecursive("downloads-icon"),
                downloads,
                self->getChildByIDRecursive("likes-icon"),
                likes,
                orbsIcon,
                orbsLabel,
                findLabel("percentage-label"),
            };
            auto snap = senary::snapshotRow(row, downloads);

            if (downloads)
                senary::setSenaryString(downloads,
                    senary::formatCount(downloads, self->m_level->m_downloads));
            if (likes)
                senary::setSenaryString(likes,
                    senary::formatCount(likes, self->m_level->m_likes));

            senary::repackRow(snap);

            // User-content restores.
            senary::restoreLabel(findLabel("level-name"), self->m_level->m_levelName);
            if (auto* creatorBtn = self->getChildByIDRecursive("creator-name")) {
                std::string name = self->m_level->m_creatorName;
                if (!name.empty())
                    senary::restoreLabel(senary::findLabelIn(creatorBtn), "By " + name);
            }
        });
    }
};

// Daily/weekly/event countdown at T6 cadence. Vanilla's updateTimers
// refreshes the label once per SI second; a T6 second is ~1.85 SI s, so
// naive conversion freezes on each value and crosses boundaries late.
// Here the raw SI seconds are read (conversion bypassed around the
// original call), interpolated between vanilla updates with a steady
// clock, and the label is re-rendered on a fast schedule so T6 boundaries
// land on time.
class $modify(SenaryDailyLevelPage, DailyLevelPage) {
    struct Fields {
        int lastSi = -1;
        std::chrono::steady_clock::time_point lastChange{};
        std::string prefix;
        std::string suffix;
        bool scheduled = false;
    };

    static double nowSeconds() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    // Parse "h:mm:ss" / "m:ss" out of the raw label, remembering any
    // surrounding text.
    bool parseRawTimer(std::string const& raw, int& siSeconds) {
        size_t first = raw.find_first_of("0123456789");
        if (first == std::string::npos) return false;
        size_t last = raw.find_last_of("0123456789");
        std::string token = raw.substr(first, last - first + 1);
        int parts[3] = { 0, 0, 0 };
        int count = 0;
        size_t pos = 0;
        while (count < 3) {
            size_t colon = token.find(':', pos);
            std::string piece = colon == std::string::npos
                ? token.substr(pos) : token.substr(pos, colon - pos);
            if (piece.empty() || piece.find_first_not_of("0123456789") != std::string::npos)
                return false;
            parts[count++] = std::stoi(piece);
            if (colon == std::string::npos) break;
            pos = colon + 1;
        }
        if (count < 2) return false;
        siSeconds = count == 3
            ? parts[0] * 3600 + parts[1] * 60 + parts[2]
            : parts[0] * 60 + parts[1];
        m_fields->prefix = raw.substr(0, first);
        m_fields->suffix = raw.substr(last + 1);
        return true;
    }

    void renderT6Timer() {
        if (!m_timeLabel) return;
        if (m_fields->lastSi < 0) return;
        double elapsed = nowSeconds()
            - std::chrono::duration<double>(m_fields->lastChange.time_since_epoch()).count();
        double remaining = static_cast<double>(m_fields->lastSi) - elapsed;
        if (remaining < 0) remaining = 0;
        senary::setSenaryString(m_timeLabel,
            m_fields->prefix + senary::formatT6Time(remaining) + m_fields->suffix);
    }

    void senaryTimerTick(float) {
        if (!senary::enabled()) return;
        renderT6Timer();
    }

    void updateTimers(float dt) {
        if (!senary::enabled()) {
            DailyLevelPage::updateTimers(dt);
            return;
        }
        // Read the raw SI countdown: bypass conversion around the original.
        senary::s_bypass = true;
        DailyLevelPage::updateTimers(dt);
        senary::s_bypass = false;
        if (m_timeLabel && m_timeLabel->getString()) {
            int si;
            std::string raw = m_timeLabel->getString();
            if (parseRawTimer(raw, si)) {
                if (si != m_fields->lastSi) {
                    m_fields->lastSi = si;
                    m_fields->lastChange = std::chrono::steady_clock::now();
                }
                renderT6Timer();
            }
        }
        if (!m_fields->scheduled) {
            m_fields->scheduled = true;
            this->schedule(schedule_selector(SenaryDailyLevelPage::senaryTimerTick), 0.1f);
        }
    }
};

// Comment metadata converts inside the otherwise-exempt CommentCell:
// likes, date, and the commenter's percentage. Username and comment text
// stay raw user content.
class $modify(SenaryCommentCell, CommentCell) {
    void convertMetadataLabels() {
        Ref<CommentCell> self(this);
        Loader::get()->queueInMainThread([self] {
            static constexpr char const* IDS[] = {
                "likes-label", "date-label", "percentage-label"
            };
            for (auto* id : IDS) {
                auto* label = typeinfo_cast<CCLabelBMFont*>(self->getChildByIDRecursive(id));
                if (!label || !label->getString()) continue;
                std::string cur = label->getString();
                std::string conv = senary::convertText(cur);
                if (conv != cur) senary::setSenaryString(label, conv);
            }
        });
    }

    void loadFromComment(GJComment* comment) {
        CommentCell::loadFromComment(comment);
        if (senary::enabled()) convertMetadataLabels();
    }

    void updateLabelValues() {
        CommentCell::updateLabelValues();
        if (!senary::enabled()) return;
        if (m_likeLabel && m_likeLabel->getString()) {
            std::string cur = m_likeLabel->getString();
            std::string conv = senary::convertText(cur);
            if (conv != cur) senary::setSenaryString(m_likeLabel, conv);
        }
    }
};

// Comment metadata inside the CommentCell exemption: percentage badge,
// like count, and date digits re-convert; comment text and username stay.
class $modify(SenaryCommentCell, CommentCell) {
    void loadFromComment(GJComment* comment) {
        CommentCell::loadFromComment(comment);
        if (!senary::enabled() || !comment) return;
        int percent = comment->m_percentage;
        int likes = comment->m_likeCount;
        std::string date = comment->m_uploadDate;
        Ref<CommentCell> self(this);
        Loader::get()->queueInMainThread([self, percent, likes, date] {
            auto find = [&](char const* id) {
                return typeinfo_cast<CCLabelBMFont*>(self->getChildByIDRecursive(id));
            };
            if (auto* pct = find("percentage-label")) {
                if (percent > 0)
                    senary::setSenaryString(pct,
                        senary::toSenary(static_cast<uint64_t>(
                            senary::displayPernifForPercent(percent))) + "%");
            }
            if (auto* likeLabel = find("likes-label")) {
                std::string sign = likes < 0 ? "-" : "";
                senary::setSenaryString(likeLabel,
                    sign + senary::toSenaryGrouped(static_cast<uint64_t>(std::abs(likes))));
            }
            if (auto* dateLabel = find("date-label")) {
                if (!date.empty())
                    senary::setSenaryString(dateLabel, senary::convertText(date));
            }
        });
    }
};

// Daily/weekly countdown at true T6-second cadence. getDailyTimeString
// reports the integer seconds vanilla is displaying; updateTimers (bumped
// to a 0.25s interval) accumulates fractional time since that report and
// rewrites the label whenever the floored T6 second changes.
class $modify(SenaryDailyLevelPage, DailyLevelPage) {
    struct Fields {
        int lastReportedSeconds = -1;
        double sinceReport = 0.0;
        std::string lastShown;
    };

    bool init(GJTimedLevelType type) {
        if (!DailyLevelPage::init(type)) return false;
        if (senary::enabled())
            this->schedule(schedule_selector(DailyLevelPage::updateTimers), 0.25f);
        return true;
    }

    gd::string getDailyTimeString(int timeLeft) {
        if (senary::enabled()) {
            m_fields->lastReportedSeconds = timeLeft;
            m_fields->sinceReport = 0.0;
        }
        return DailyLevelPage::getDailyTimeString(timeLeft);
    }

    void updateTimers(float dt) {
        DailyLevelPage::updateTimers(dt);
        if (!senary::enabled()) return;
        if (m_fields->lastReportedSeconds < 0 || !m_timeLabel) return;
        m_fields->sinceReport += dt;
        double remaining = static_cast<double>(m_fields->lastReportedSeconds) - m_fields->sinceReport;
        if (remaining < 0) remaining = 0;
        std::string t6 = senary::formatT6Time(remaining, false);
        if (t6 == m_fields->lastShown) return;
        m_fields->lastShown = t6;
        senary::setSenaryString(m_timeLabel, t6);
    }
};

class $modify(SenaryLevelBrowserLayer, LevelBrowserLayer) {
    void setIDPopupClosed(SetIDPopup* popup, int value) {
        if (senary::enabled()) value = senary::reinterpretTypedAsSenary(value);
        LevelBrowserLayer::setIDPopupClosed(popup, value);
    }
};

// Editor trigger popups: typed digits are senary. The typed string is
// swapped to its decimal equivalent on the underlying text field while the
// vanilla parser runs, then restored, so the display keeps the senary
// digits. Covers every SetupTriggerPopup subclass. Known caveat: ID fields
// inside trigger popups (song/SFX IDs) are indistinguishable from value
// fields and get reinterpreted too — the "editor-senary-input" setting
// turns this off wholesale.
class $modify(SenarySetupTriggerPopup, SetupTriggerPopup) {
    void textChanged(CCTextInputNode* node) {
        static bool s_reentry = false;
        bool active = senary::enabled() &&
                      Mod::get()->getSettingValue<bool>("editor-senary-input") &&
                      !s_reentry && node && node->m_textField;
        if (!active) {
            SetupTriggerPopup::textChanged(node);
            return;
        }
        char const* raw = node->m_textField->getString();
        std::string typed = raw ? raw : "";
        double value;
        if (!senary::parseSenaryFloat(typed, value)) {
            SetupTriggerPopup::textChanged(node); // not senary (6-9, partial, empty)
            return;
        }
        std::string decimal = senary::formatDecimalForGame(value);
        if (decimal == typed) { // same either way (single digits etc.)
            SetupTriggerPopup::textChanged(node);
            return;
        }
        s_reentry = true;
        node->m_textField->setString(decimal.c_str());
        SetupTriggerPopup::textChanged(node);
        node->m_textField->setString(typed.c_str());
        node->refreshLabel();
        s_reentry = false;
    }
};

// Top 100 leaderboard -> Top two-nif. The server caps getGJScores at 100
// entries, so extension is impossible; entries beyond rank 72x are dropped
// and the tab's "244" (converted "100") is renamed to "200".
class $modify(SenaryLeaderboardsLayer, LeaderboardsLayer) {
    void setupLevelBrowser(cocos2d::CCArray* scores) {
        if (senary::enabled() && m_type == LeaderboardType::Top100 &&
            scores && scores->count() > 72) {
            auto* trimmed = cocos2d::CCArray::create();
            for (unsigned int i = 0; i < 72; ++i)
                trimmed->addObject(scores->objectAtIndex(i));
            LeaderboardsLayer::setupLevelBrowser(trimmed);
            return;
        }
        LeaderboardsLayer::setupLevelBrowser(scores);
    }

    bool init(LeaderboardType type, LeaderboardStat stat) {
        if (!LeaderboardsLayer::init(type, stat)) return false;
        if (!senary::enabled()) return true;
        Ref<LeaderboardsLayer> self(this);
        Loader::get()->queueInMainThread([self] {
            auto* menu = self->getChildByIDRecursive("top-100-menu");
            auto* label = senary::findLabelIn(menu);
            if (!label || !label->getString()) return;
            std::string cur = label->getString();
            size_t pos = cur.find("244");
            if (pos == std::string::npos) pos = cur.find("100");
            if (pos == std::string::npos) return;
            cur.replace(pos, 3, "200");
            senary::setSenaryString(label, cur);
        });
        return true;
    }
};
