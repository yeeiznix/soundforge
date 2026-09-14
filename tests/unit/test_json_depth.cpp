// G3 P7 — JSON depth pre-parse gate (PLAN_G3 §6 P7; SEC-G3-7/-G3-8).
// Tests the shared checked_parse() depth gate on ALL 4 raw-JSON entry points
// (sf_project_from_json, sf_project_open_from_path, sf_validate_project_json,
// sf_migrate_json), the scanner contract of scan_json_depth() (distinct
// depth/unterminated pre-reject texts, string/escape awareness, no parser
// re-implementation), the 8 MiB byte cap, and a differential fuzz asserting
// scanner verdict ⊆ nlohmann outcome over mutated 255/256/257-depth docs.
#include <gtest/gtest.h>

#include "sf_internal.hpp"  // scan_json_depth / checked_parse / kMaxJsonDepth (internal)
#include "soundforge/sf_diagnostics.h"
#include "soundforge/sf_migration.h"
#include "soundforge/sf_project.h"
#include "soundforge/sf_schema.h"
#include "nlohmann/json.hpp"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::string read_file(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::ostringstream oss;
    oss << f.rdbuf();
    return oss.str();
}

// Canonical minimal v2 fixture — a fully schema-valid base document.
nlohmann::json base_doc() {
    return nlohmann::json::parse(read_file(FIXTURES_DIR "/project_minimal_v2.json"));
}

// Nest an object chain of `n` levels inside dspPresets[0].data. Document depth
// = 4 + n: root object(1) + dspPresets array(2) + envelope object(3) +
// data object(4) + n nested objects. data is an opaque envelope body, so any
// nesting depth keeps the document schema-valid.
nlohmann::json doc_with_data_depth(int n) {
    nlohmann::json doc = base_doc();
    nlohmann::json env = {
        {"id", "550e8400-e29b-41d4-a716-446655440055"},
        {"type", "dspPreset"},
        {"version", 1},
        {"createdAt", "2026-09-11T00:00:00.000Z"},
        {"modifiedAt", "2026-09-11T00:00:00.000Z"},
        {"provenance", "created"},
        {"data", nlohmann::json::object()},
    };
    nlohmann::json* cur = &env["data"];
    for (int i = 0; i < n; ++i) {
        (*cur)["k"] = nlohmann::json::object();
        cur = &(*cur)["k"];
    }
    doc["dspPresets"] = nlohmann::json::array({env});
    return doc;
}

// n = 252 -> depth 256, n = 253 -> depth 257.
std::string doc_str(int n) { return doc_with_data_depth(n).dump(); }

// Reference max depth over an already-parsed tree (nlohmann): iterative DFS.
// A bare object/array root has depth 1 (nlohmann json::depth() convention);
// a bare scalar root has depth 0 — same convention as scan_json_depth().
int tree_depth(const nlohmann::json& j) {
    struct Frame {
        const nlohmann::json* node;
        int d;
    };
    std::vector<Frame> st;
    st.push_back({&j, (j.is_object() || j.is_array()) ? 1 : 0});
    int best = 0;
    while (!st.empty()) {
        Frame f = st.back();
        st.pop_back();
        if (f.d > best) best = f.d;
        if (f.node->is_object()) {
            for (auto it = f.node->begin(); it != f.node->end(); ++it)
                st.push_back({&it.value(), f.d + 1});
        } else if (f.node->is_array()) {
            for (auto& v : *f.node) st.push_back({&v, f.d + 1});
        }
    }
    return best;
}

std::filesystem::path tmp_file(const char* name) {
    return std::filesystem::temp_directory_path() / name;
}

void write_file(const std::filesystem::path& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary);
    f << data;
}

// nlohmann verdict probe (silences warn_unused_result on parse()).
bool parseable(const std::string& s) {
    try {
        const nlohmann::json parsed = nlohmann::json::parse(s);
        static_cast<void>(parsed);
        return true;
    } catch (const nlohmann::json::exception&) {
        return false;
    }
}

// ---------------------------------------------------------------------------
// Differential fuzz helpers
// ---------------------------------------------------------------------------
std::string mutate(const std::string& base, int cls, std::mt19937& rng) {
    std::uniform_int_distribution<size_t> any(0, base.size());
    std::uniform_int_distribution<size_t> at(0, base.size() - 1);
    static const char pool[] = "{}\"\\[]:,\n 123abc";
    std::string m = base;
    switch (cls) {
        case 0:  // flip a byte (may become a structural or quote byte)
            m[at(rng)] = static_cast<char>(rng() & 0xFF);
            break;
        case 1:  // delete a byte
            m.erase(at(rng), 1);
            break;
        case 2:  // insert a structural/numeric byte
            m.insert(any(rng), 1, pool[rng() % (sizeof(pool) - 1)]);
            break;
        case 3:  // duplicate a byte
            m.insert(any(rng), 1, m[at(rng)]);
            break;
        case 4:  // truncate
            m.resize(any(rng));
            break;
        case 5:  // swap two bytes
            if (m.size() >= 2) {
                size_t a = at(rng);
                size_t b = at(rng);
                std::swap(m[a], m[b]);
            }
            break;
        case 6:  // inject a quote
            m.insert(any(rng), 1, '"');
            break;
        default:  // 7: append a structural tail
            static const char* tails[] = {"{", "}", "[", "\"", "\\", "]"};
            m += tails[rng() % 6];
            break;
    }
    return m;
}

}  // namespace

// ---------------------------------------------------------------------------
// Boundary: 256 accepted / 257 rejected on ALL FOUR parse sites
// ---------------------------------------------------------------------------
TEST(JsonDepth, Boundary256Accepted257RejectedAllSites) {
    const std::string d256 = doc_str(252);  // depth 256
    const std::string d257 = doc_str(253);  // depth 257
    ASSERT_EQ(tree_depth(nlohmann::json::parse(d256)), 256);
    ASSERT_EQ(tree_depth(nlohmann::json::parse(d257)), 257);

    // Scanner-level boundary.
    int md = -1;
    EXPECT_EQ(sfcore::scan_json_depth(d256.data(), d256.size(), &md),
              sfcore::JsonScanStatus::kOk);
    EXPECT_EQ(md, 256);
    EXPECT_EQ(sfcore::scan_json_depth(d257.data(), d257.size(), &md),
              sfcore::JsonScanStatus::kDepthExceeded);
    EXPECT_EQ(md, 257);

    // checked_parse-level: same verdicts, exact text.
    sfcore::json j;
    std::string perr;
    EXPECT_EQ(sfcore::checked_parse(d256.data(), d256.size(), &j, &perr), SF_OK);
    EXPECT_EQ(sfcore::checked_parse(d257.data(), d257.size(), &j, &perr), SF_E_SCHEMA);
    EXPECT_EQ(perr, "schema: json depth exceeds 256");

    // Site 1: sf_validate_project_json (err_buf carries the text).
    char err[512] = {0};
    EXPECT_EQ(sf_validate_project_json(d256.data(), d256.size(), err, sizeof(err)), SF_OK);
    EXPECT_EQ(sf_validate_project_json(d257.data(), d257.size(), err, sizeof(err)), SF_E_SCHEMA);
    EXPECT_EQ(std::string(err), "schema: json depth exceeds 256");

    // Site 2: sf_project_from_json.
    sf_project_t* p = nullptr;
    EXPECT_EQ(sf_project_from_json(d256.data(), d256.size(), &p), SF_OK);
    ASSERT_NE(p, nullptr);
    sf_project_destroy(p);
    p = nullptr;
    EXPECT_EQ(sf_project_from_json(d257.data(), d257.size(), &p), SF_E_SCHEMA);
    EXPECT_EQ(p, nullptr);
    EXPECT_EQ(std::string(sf_last_error(nullptr)), "schema: json depth exceeds 256");

    // Site 3: sf_project_open_from_path.
    const std::filesystem::path ok_path = tmp_file("sf_p7_depth256_ok.json");
    const std::filesystem::path bad_path = tmp_file("sf_p7_depth257_bad.json");
    write_file(ok_path, d256);
    write_file(bad_path, d257);
    sf_project_t* p2 = nullptr;
    EXPECT_EQ(sf_project_open_from_path(ok_path.c_str(), &p2), SF_OK);
    ASSERT_NE(p2, nullptr);
    sf_project_destroy(p2);
    p2 = nullptr;
    EXPECT_EQ(sf_project_open_from_path(bad_path.c_str(), &p2), SF_E_SCHEMA);
    EXPECT_EQ(p2, nullptr);
    EXPECT_EQ(std::string(sf_last_error(nullptr)), "schema: json depth exceeds 256");
    std::filesystem::remove(ok_path);
    std::filesystem::remove(bad_path);

    // Site 4: sf_migrate_json (1->1 no-op: only the parse is gated). The cap
    // must absorb j.dump(2) pretty-printing, which expands a 252-deep chain by
    // ~O(depth^2) indentation (~64 KiB here) — so size the buffer generously.
    const size_t cap = d256.size() + 512 * 1024;
    std::vector<char> buf(cap);
    std::memcpy(buf.data(), d256.data(), d256.size());
    size_t len = d256.size();
    EXPECT_EQ(sf_migrate_json(buf.data(), &len, cap, 1, 1), SF_OK);
    std::memcpy(buf.data(), d257.data(), d257.size());
    len = d257.size();
    EXPECT_EQ(sf_migrate_json(buf.data(), &len, cap, 1, 1), SF_E_SCHEMA);
    EXPECT_EQ(std::string(sf_last_error(nullptr)), "schema: json depth exceeds 256");
}

// ---------------------------------------------------------------------------
// Unterminated string: DISTINCT pre-reject text, never a false depth trip
// ---------------------------------------------------------------------------
TEST(JsonDepth, UnterminatedStringDistinctTextAllSites) {
    // Shallow (depth 1) unterminated string.
    const std::string doc = R"({"a": "oops)";
    int md = -1;
    EXPECT_EQ(sfcore::scan_json_depth(doc.data(), doc.size(), &md),
              sfcore::JsonScanStatus::kUnterminated);
    EXPECT_EQ(md, 1);
    char err[512] = {0};
    EXPECT_EQ(sf_validate_project_json(doc.data(), doc.size(), err, sizeof(err)), SF_E_SCHEMA);
    EXPECT_EQ(std::string(err), "schema: unterminated string in json");

    // Deep doc (exactly 256) + a trailing quote opening an unterminated
    // string: still the STRING text. An unterminated string never becomes a
    // false depth trip, even sitting ON the depth boundary.
    std::string deep = doc_str(252);
    deep.push_back('"');
    EXPECT_EQ(sfcore::scan_json_depth(deep.data(), deep.size(), nullptr),
              sfcore::JsonScanStatus::kUnterminated);
    EXPECT_EQ(sf_validate_project_json(deep.data(), deep.size(), err, sizeof(err)), SF_E_SCHEMA);
    EXPECT_EQ(std::string(err), "schema: unterminated string in json");

    // Same pre-reject text at the other three sites.
    sf_project_t* p = nullptr;
    EXPECT_EQ(sf_project_from_json(doc.data(), doc.size(), &p), SF_E_SCHEMA);
    EXPECT_EQ(p, nullptr);
    EXPECT_EQ(std::string(sf_last_error(nullptr)), "schema: unterminated string in json");

    const std::filesystem::path bad_path = tmp_file("sf_p7_unterminated.json");
    write_file(bad_path, doc);
    EXPECT_EQ(sf_project_open_from_path(bad_path.c_str(), &p), SF_E_SCHEMA);
    EXPECT_EQ(p, nullptr);
    EXPECT_EQ(std::string(sf_last_error(nullptr)), "schema: unterminated string in json");
    std::filesystem::remove(bad_path);

    std::vector<char> buf(doc.size() + 1024);
    std::memcpy(buf.data(), doc.data(), doc.size());
    size_t len = doc.size();
    EXPECT_EQ(sf_migrate_json(buf.data(), &len, buf.size(), 1, 1), SF_E_SCHEMA);
    EXPECT_EQ(std::string(sf_last_error(nullptr)), "schema: unterminated string in json");
}

// ---------------------------------------------------------------------------
// Scanner contract (SEC-G3-8)
// ---------------------------------------------------------------------------
TEST(JsonDepth, UnknownEscapeIsTwoCharUnit) {
    // "\x" is a 2-char unit: the '"' right after it closes the string, so the
    // scan stays clean. A scanner that swallowed TWO chars after the backslash
    // would leave the string open and trip "unterminated" — the discriminator.
    const std::string doc = R"({"a":"\x"})";
    int md = -1;
    EXPECT_EQ(sfcore::scan_json_depth(doc.data(), doc.size(), &md),
              sfcore::JsonScanStatus::kOk);
    EXPECT_EQ(md, 1);
    // nlohmann rejects the invalid escape — ITS verdict, unchanged by the
    // scanner (malformed JSON still yields the nlohmann result).
    char err[512] = {0};
    EXPECT_EQ(sf_validate_project_json(doc.data(), doc.size(), err, sizeof(err)), SF_E_SCHEMA);
    EXPECT_NE(std::string(err).find("JSON parse error"), std::string::npos) << err;
    EXPECT_EQ(std::string(err).find("schema:"), std::string::npos) << err;
}

TEST(JsonDepth, OddEvenBackslashRunsBeforeQuote) {
    // Even run: "\\" is an escaped backslash; the following '"' CLOSES the
    // string. Genuinely valid JSON — accepted by both.
    const std::string even = R"({"a":"x\\y"})";
    EXPECT_EQ(sfcore::scan_json_depth(even.data(), even.size(), nullptr),
              sfcore::JsonScanStatus::kOk);
    EXPECT_TRUE(parseable(even));

    // Odd run: the quote is escaped ("\\\"") -> string continues to EOF.
    const std::string odd = R"({"a":"x\\\"})";
    EXPECT_EQ(sfcore::scan_json_depth(odd.data(), odd.size(), nullptr),
              sfcore::JsonScanStatus::kUnterminated);
    EXPECT_FALSE(parseable(odd));

    // Escaped quote mid-string: '\"' eats the quote, the NEXT '"' closes.
    const std::string esc = R"({"a":"x\""})";
    EXPECT_EQ(sfcore::scan_json_depth(esc.data(), esc.size(), nullptr),
              sfcore::JsonScanStatus::kOk);
    EXPECT_TRUE(parseable(esc));
}

TEST(JsonDepth, MalformedUnicodeEscapeNeverDesyncsStringTracking) {
    // \u + 4 chars (non-hex included) is consumed as ONE unit: braces
    // swallowed inside the unit never count toward depth, and the string ends
    // at the real quote. nlohmann rejects the bad escape — its verdict.
    const std::string nonhex = R"({"a":"\u12x4"})";
    int md = -1;
    EXPECT_EQ(sfcore::scan_json_depth(nonhex.data(), nonhex.size(), &md),
              sfcore::JsonScanStatus::kOk);
    EXPECT_EQ(md, 1);
    EXPECT_FALSE(parseable(nonhex));

    // \u + 4 chars where two are '{' '}': the structurals stay inside the
    // escape unit — depth stays 1, no trip, no desync.
    const std::string braces = R"({"a":"\u12{}"})";
    md = -1;
    EXPECT_EQ(sfcore::scan_json_depth(braces.data(), braces.size(), &md),
              sfcore::JsonScanStatus::kOk);
    EXPECT_EQ(md, 1);
    EXPECT_FALSE(parseable(braces));

    // \u with <4 chars: the closing '"' is swallowed by the unit -> the
    // scanner answers "unterminated" (consistent, not desynced); nlohmann
    // rejects the malformed escape as well.
    const std::string short_esc = R"({"a":"\u12"})";
    EXPECT_EQ(sfcore::scan_json_depth(short_esc.data(), short_esc.size(), nullptr),
              sfcore::JsonScanStatus::kUnterminated);
    EXPECT_FALSE(parseable(short_esc));

    // Sanity: a VALID \uXXXX escapes exactly 4 chars and both agree.
    const std::string good = R"({"a":"\u0041"})";
    EXPECT_EQ(sfcore::scan_json_depth(good.data(), good.size(), nullptr),
              sfcore::JsonScanStatus::kOk);
    EXPECT_TRUE(parseable(good));
}

TEST(JsonDepth, RawControlBytesInStrings) {
    // Raw 0x01 inside a string: the scanner treats it as an ordinary in-string
    // byte — no trip, and structurals after it still don't count. nlohmann
    // rejects the unescaped control char (its verdict, unchanged).
    std::string doc = "{\"a\":\"x\x01y{[\"}";
    int md = -1;
    EXPECT_EQ(sfcore::scan_json_depth(doc.data(), doc.size(), &md),
              sfcore::JsonScanStatus::kOk);
    EXPECT_EQ(md, 1);
    char err[512] = {0};
    EXPECT_EQ(sf_validate_project_json(doc.data(), doc.size(), err, sizeof(err)), SF_E_SCHEMA);
    EXPECT_NE(std::string(err).find("JSON parse error"), std::string::npos) << err;
    EXPECT_EQ(std::string(err).find("schema:"), std::string::npos) << err;
}

TEST(JsonDepth, BracesAndEscapesInsideStringsDoNotAffectDepth) {
    // 4 "structural" characters and 2 escapes live INSIDE strings; the true
    // depth is [ (1) + { (2) = 2 and nothing else.
    const std::string doc = R"([{"a":"{[]}{","b":"x\"y\\z"}])";
    int md = -1;
    EXPECT_EQ(sfcore::scan_json_depth(doc.data(), doc.size(), &md),
              sfcore::JsonScanStatus::kOk);
    EXPECT_EQ(md, 2);
    EXPECT_TRUE(parseable(doc));
}

TEST(JsonDepth, MixedNestingAndStrayClosers) {
    // Mixed {/[ nesting to the boundary verbatim: accept at 256, trip at 257.
    std::string mixed;
    for (int i = 0; i < 128; ++i) mixed += "{[";
    EXPECT_EQ(sfcore::scan_json_depth(mixed.data(), mixed.size(), nullptr),
              sfcore::JsonScanStatus::kOk);
    std::string over = mixed + "{";
    int md = -1;
    EXPECT_EQ(sfcore::scan_json_depth(over.data(), over.size(), &md),
              sfcore::JsonScanStatus::kDepthExceeded);
    EXPECT_EQ(md, 257);

    // Stray closers clamp at depth 0 — never a false depth trip (nlohmann
    // judges the malformed doc).
    const std::string stray = "]]]}}";
    md = -1;
    EXPECT_EQ(sfcore::scan_json_depth(stray.data(), stray.size(), &md),
              sfcore::JsonScanStatus::kOk);
    EXPECT_EQ(md, 0);
    EXPECT_FALSE(parseable(stray));
}

TEST(JsonDepth, MalformedNonDepthJsonYieldsNlohmannResult) {
    const char* docs[] = {
        R"({"a": })",    // missing value
        R"([1, 2,])",    // trailing comma
        R"({,})",        // stray comma
        R"({"a" "b"})",  // missing colon
        R"(nul)",        // bad literal
        R"(})",          // stray closer
        R"(}, {)",       // garbage after a value (no quote: must stay scanner-silent)
    };
    for (const char* d : docs) {
        const std::string s(d);
        int md = -1;
        EXPECT_EQ(sfcore::scan_json_depth(s.data(), s.size(), &md),
                  sfcore::JsonScanStatus::kOk)
            << "scanner must stay silent on: " << s;
        char err[512] = {0};
        EXPECT_EQ(sf_validate_project_json(s.data(), s.size(), err, sizeof(err)), SF_E_SCHEMA)
            << s;
        EXPECT_NE(std::string(err).find("JSON parse error"), std::string::npos)
            << s << " -> " << err;
    }
}

// ---------------------------------------------------------------------------
// Pre-parse byte cap + depth (both fire before nlohmann)
// ---------------------------------------------------------------------------
TEST(JsonDepth, ByteCapAndDepthBothPreParse) {
    // >8 MiB and shallow: the byte cap fires on every in-RAM site.
    const std::string huge = std::string(sfcore::kMaxDocBytes + 1, ' ') + "{}";
    char err[512] = {0};
    EXPECT_EQ(sf_validate_project_json(huge.data(), huge.size(), err, sizeof(err)),
              SF_E_FILE_TOO_LARGE);
    EXPECT_EQ(std::string(err), "JSON input exceeds 8 MiB limit");
    sf_project_t* p = nullptr;
    EXPECT_EQ(sf_project_from_json(huge.data(), huge.size(), &p), SF_E_FILE_TOO_LARGE);
    EXPECT_EQ(p, nullptr);
    EXPECT_EQ(std::string(sf_last_error(nullptr)), "JSON input exceeds 8 MiB limit");

    // migrate: SEC-G3-7 closes the last ungated site — byte cap applies here
    // too, before any parse.
    std::vector<char> buf(huge.size() + 1);
    std::memcpy(buf.data(), huge.data(), huge.size());
    size_t len = huge.size();
    EXPECT_EQ(sf_migrate_json(buf.data(), &len, huge.size() + 4, 0, 1), SF_E_FILE_TOO_LARGE);
    EXPECT_EQ(std::string(sf_last_error(nullptr)), "JSON input exceeds 8 MiB limit");

    // open_from_path: read_file_capped rejects before any parse (same cap).
    const std::filesystem::path big = tmp_file("sf_p7_huge.json");
    write_file(big, huge);
    p = nullptr;
    EXPECT_EQ(sf_project_open_from_path(big.c_str(), &p), SF_E_FILE_TOO_LARGE);
    EXPECT_EQ(p, nullptr);
    EXPECT_EQ(std::string(sf_last_error(nullptr)), "file exceeds 8 MiB limit");
    std::filesystem::remove(big);

    // >8 MiB AND 257 deep: the byte cap wins (checked before depth).
    const std::string huge_deep = std::string(sfcore::kMaxDocBytes + 1, '{');
    EXPECT_EQ(sf_validate_project_json(huge_deep.data(), huge_deep.size(), err, sizeof(err)),
              SF_E_FILE_TOO_LARGE);
    EXPECT_EQ(std::string(err), "JSON input exceeds 8 MiB limit");

    // Small and 257 deep: the depth gate fires. Both gates are live pre-parse.
    const std::string d257 = doc_str(253);
    EXPECT_EQ(sf_validate_project_json(d257.data(), d257.size(), err, sizeof(err)), SF_E_SCHEMA);
    EXPECT_EQ(std::string(err), "schema: json depth exceeds 256");
}

// ---------------------------------------------------------------------------
// Accept-set equality: the gate changes NO nlohmann verdict (SEC-G3-8)
// ---------------------------------------------------------------------------
TEST(JsonDepth, AcceptSetEqualsNlohmannOnCorpus) {
    // Shallow corpus: valid docs (accept) + malformed-but-shallow docs (reject)
    // + unterminated strings (pre-reject must coincide with nlohmann reject).
    const char* corpus[] = {
        R"({})",
        R"([])",
        R"({"a":1,"b":[true,null,"s"]})",
        R"(42)",
        R"("just a string")",
        R"(null)",
        R"({"a":"\u0041"})",
        R"({"a":"x\\y"})",
        R"({"a": })",
        R"([1,2,])",
        R"(nul)",
        R"(})",
        R"({"a":"oops)",   // unterminated string
        R"({"a":"\\\")",   // odd backslash run -> unterminated
    };
    for (const char* d : corpus) {
        const std::string s(d);
        SCOPED_TRACE(s);
        int md = -1;
        const sfcore::JsonScanStatus st = sfcore::scan_json_depth(s.data(), s.size(), &md);
        const bool n_ok = parseable(s);

        sfcore::json j;
        std::string perr;
        bool rc_ok = false;
        sf_result_t rc = SF_OK;
        try {
            rc = sfcore::checked_parse(s.data(), s.size(), &j, &perr);
            rc_ok = (rc == SF_OK);
        } catch (const nlohmann::json::exception&) {
            rc_ok = false;  // nlohmann threw: checked_parse propagates, by design
        }

        if (st == sfcore::JsonScanStatus::kOk) {
            // Scanner stays silent: checked_parse's verdict MUST equal nlohmann.
            EXPECT_EQ(rc_ok, n_ok);
        } else {
            // Every scanner pre-reject must also be an nlohmann reject
            // (pre-rejects ⊆ nlohmann rejects) — no bypass, no accept-set loss.
            EXPECT_EQ(st, sfcore::JsonScanStatus::kUnterminated);
            EXPECT_FALSE(n_ok) << "scanner pre-rejected what nlohmann accepts";
            EXPECT_EQ(rc, SF_E_SCHEMA);
            EXPECT_EQ(perr, "schema: unterminated string in json");
        }
    }
}

TEST(JsonDepth, DepthTripPrecedesUnterminatedWhenBothApply) {
    // A genuinely >256-deep doc that ALSO ends in an unterminated string: the
    // depth gate fires at the 257th opener, long before the tail is reached.
    // The depth verdict is real (not a "false depth trip"); "unterminated" is
    // reserved for depth-safe documents. No nlohmann parse is attempted here —
    // the whole point of the gate is to not recurse on such input.
    std::string doc;
    for (int i = 0; i < 257; ++i) doc += "[";
    doc += "\"oops";
    int md = -1;
    EXPECT_EQ(sfcore::scan_json_depth(doc.data(), doc.size(), &md),
              sfcore::JsonScanStatus::kDepthExceeded);
    EXPECT_EQ(md, 257);
    sfcore::json j;
    std::string perr;
    EXPECT_EQ(sfcore::checked_parse(doc.data(), doc.size(), &j, &perr), SF_E_SCHEMA);
    EXPECT_EQ(perr, "schema: json depth exceeds 256");
}

// ---------------------------------------------------------------------------
// Differential fuzz: scanner verdict ⊆ nlohmann outcome
// ---------------------------------------------------------------------------
TEST(JsonDepth, DifferentialFuzzScannerSubsetOfNlohmann) {
    // Corpus: 255 / 256 / 257-depth schema-valid docs mutated across 8
    // mutation classes (flip / delete / insert / duplicate / truncate / swap /
    // inject-quote / append-structural).
    const std::vector<std::string> bases = {doc_str(251), doc_str(252), doc_str(253)};
    const std::vector<int> base_depths = {255, 256, 257};
    std::mt19937 rng(0xC0FFEEu);

    size_t mutated = 0;         // mutated docs actually scanned+parsed
    size_t nlohmann_accepted = 0;
    size_t depth_trips = 0;
    size_t unterminated_trips = 0;
    const int kClasses = 8;
    const int kReps = 30;

    for (size_t bi = 0; bi < bases.size(); ++bi) {
        for (int cls = 0; cls < kClasses; ++cls) {
            for (int rep = 0; rep < kReps; ++rep) {
                const std::string m = mutate(bases[bi], cls, rng);
                ++mutated;

                // Scanner verdict.
                int smd = -1;
                const sfcore::JsonScanStatus s =
                    sfcore::scan_json_depth(m.data(), m.size(), &smd);

                // nlohmann outcome (never let a mutation crash the test: the
                // parse is only used to LEARN nlohmann's verdict).
                bool n_ok = true;
                nlohmann::json parsed;
                try {
                    parsed = nlohmann::json::parse(m);
                } catch (const nlohmann::json::exception&) {
                    n_ok = false;
                }

                SCOPED_TRACE("base_depth=" + std::to_string(base_depths[bi]) +
                             " class=" + std::to_string(cls) +
                             " rep=" + std::to_string(rep));

                if (s == sfcore::JsonScanStatus::kOk) {
                    // "Never accept depth >256."
                    EXPECT_LE(smd, 256);
                } else if (s == sfcore::JsonScanStatus::kDepthExceeded) {
                    EXPECT_GT(smd, 256);
                    ++depth_trips;
                } else {
                    ++unterminated_trips;
                }

                if (n_ok) {
                    ++nlohmann_accepted;
                    // A scanner "unterminated" pre-reject on an nlohmann-ACCEPT
                    // document would mean string-tracking desync — forbidden.
                    EXPECT_NE(s, sfcore::JsonScanStatus::kUnterminated)
                        << "scanner pre-rejected (unterminated) what nlohmann accepts";
                    // "Depth equal on every nlohmann-accept."
                    EXPECT_EQ(smd, tree_depth(parsed))
                        << "scanner depth != nlohmann tree depth";
                    // Scanner depth trip can only accompany an nlohmann-accept
                    // when the TRUE depth exceeds 256 (the intended gate).
                    if (s == sfcore::JsonScanStatus::kDepthExceeded) {
                        EXPECT_GT(tree_depth(parsed), 256);
                    }
                }
                // nlohmann-rejected docs: scanner silent or tripping is fine —
                // pre-rejects ⊆ nlohmann rejects; malformed JSON yields the
                // nlohmann result.
            }
        }
    }

    // Stats for the gate report: how many mutated docs actually ran.
    EXPECT_GT(mutated, 0u);
    EXPECT_GT(nlohmann_accepted, 0u);  // fuzz must exercise both accept paths
    EXPECT_GT(depth_trips, 0u);        // and both pre-reject paths
    EXPECT_GT(unterminated_trips, 0u);
    ::testing::Test::RecordProperty("fuzz_mutated", static_cast<int>(mutated));
    ::testing::Test::RecordProperty("fuzz_nlohmann_accepted",
                                    static_cast<int>(nlohmann_accepted));
    ::testing::Test::RecordProperty("fuzz_depth_trips", static_cast<int>(depth_trips));
    ::testing::Test::RecordProperty("fuzz_unterminated_trips",
                                    static_cast<int>(unterminated_trips));
}