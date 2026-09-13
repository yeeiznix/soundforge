// SoundForge G3 P1 — name model: sf_scene_rename + utf8_char_count + caps.
// PLAN_G3 §4.5 (D5) / §7.1 `test_name_model.cpp`.
#include <gtest/gtest.h>

#include "sf_internal.hpp"
#include "soundforge/sf_project.h"
#include "soundforge/sf_graph.h"

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace {

std::string json_of(sf_project_t* p) {
    char* out = nullptr;
    size_t len = 0;
    EXPECT_EQ(sf_project_to_json(p, &out, &len), SF_OK);
    std::string s(out ? out : "", len);
    if (out) sf_free_string(out);
    return s;
}

// Extract a JSON string value for `field` (naive but stable for these tests).
std::string string_field(const std::string& body, const char* field) {
    const std::string key = std::string("\"") + field + "\": \"";
    const size_t at = body.find(key);
    if (at == std::string::npos) return std::string();
    const size_t start = at + key.size();
    const size_t end = body.find('"', start);
    if (end == std::string::npos) return std::string();
    return body.substr(start, end - start);
}

// The scene object follows the project object; extract scene.id from the
// scene block specifically.
std::string scene_block_id(const std::string& s) {
    const size_t scene_at = s.find("\"scene\"");
    if (scene_at == std::string::npos) return std::string();
    const std::string sub = s.substr(scene_at);
    return string_field(sub, "id");
}

// Extract scene.name from the scene block (first "name" after the "scene" key).
std::string scene_block_name(const std::string& s) {
    const size_t scene_at = s.find("\"scene\"");
    if (scene_at == std::string::npos) return std::string();
    return string_field(s.substr(scene_at), "name");
}

// G3 ORC-P1-2: accepted names may hold malformed UTF-8 (fail-open counter).
// to_json must NEVER throw — the replace handler emits U+FFFD. Returns success.
bool serializes(sf_project_t* p) {
    char* out = nullptr;
    size_t len = 0;
    const sf_result_t rc = sf_project_to_json(p, &out, &len);
    if (out) sf_free_string(out);
    return rc == SF_OK;
}

// Count `"action": "scene.update"` entries and find the one carrying "rename".
int count_actions(const std::string& body, const char* action) {
    const std::string needle = std::string("\"action\": \"") + action + "\"";
    int n = 0;
    size_t at = 0;
    while ((at = body.find(needle, at)) != std::string::npos) {
        ++n;
        at += needle.size();
    }
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// utf8_char_count unit table (RFC 3629; fail-open on malformed bytes)
// ---------------------------------------------------------------------------

TEST(Utf8CharCount, AsciiAndMultibyte) {
    EXPECT_EQ(sfcore::utf8_char_count(""), 0u);
    EXPECT_EQ(sfcore::utf8_char_count(nullptr), 0u);
    EXPECT_EQ(sfcore::utf8_char_count("abc"), 3u);
    // U+00E9 é = C3 A9 (2 bytes, 1 cp).
    EXPECT_EQ(sfcore::utf8_char_count("\xC3\xA9"), 1u);
    // U+20AC € = E2 82 AC (3 bytes, 1 cp).
    EXPECT_EQ(sfcore::utf8_char_count("\xE2\x82\xAC"), 1u);
    // U+1F600 😀 = F0 9F 98 80 (4 bytes, 1 cp).
    EXPECT_EQ(sfcore::utf8_char_count("\xF0\x9F\x98\x80"), 1u);
    // Mixed: "aé€😀" -> 4 cps.
    EXPECT_EQ(sfcore::utf8_char_count("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"), 4u);
}

TEST(Utf8CharCount, FailOpenOnMalformedSequences) {
    // Lone continuation byte 0x80 -> 1 char (fail-open).
    EXPECT_EQ(sfcore::utf8_char_count("\x80"), 1u);
    // Truncated 2-byte lead (0xC3 followed by NUL) -> 1 char.
    EXPECT_EQ(sfcore::utf8_char_count("\xC3"), 1u);
    // Truncated 3-byte sequence (E2 82, missing 3rd) -> 2 chars fail-open.
    EXPECT_EQ(sfcore::utf8_char_count("\xE2\x82"), 2u);
    // Overlong 2-byte encoding of '/' (C0 AF) -> 2 malformed bytes -> 2 chars.
    EXPECT_EQ(sfcore::utf8_char_count("\xC0\xAF"), 2u);
    // Overlong 3-byte encoding of U+0000 (E0 80 80) -> 3 chars.
    EXPECT_EQ(sfcore::utf8_char_count("\xE0\x80\x80"), 3u);
    // Out-of-range 4-byte (F4 90 80 80 = U+110000) -> 4 chars.
    EXPECT_EQ(sfcore::utf8_char_count("\xF4\x90\x80\x80"), 4u);
    // UTF-16 surrogate half encoded as 3 bytes (ED A0 80) -> 3 chars.
    EXPECT_EQ(sfcore::utf8_char_count("\xED\xA0\x80"), 3u);
    // Invalid lead byte 0xFF -> 1 char.
    EXPECT_EQ(sfcore::utf8_char_count("\xFF"), 1u);
    // Conservative invariant: chars >= bytes for any byte string.
    const std::string broken = "\x80\xC3\xE2\x82\xF0\x9F";
    EXPECT_GE(sfcore::utf8_char_count(broken.c_str()), broken.size());
}

// ---------------------------------------------------------------------------
// sf_scene_rename happy path + audit + modifiedAt
// ---------------------------------------------------------------------------

TEST(SceneRename, HappyPathAuditAndTimestamp) {
    sf_project_t* p = sf_project_create("P", nullptr);
    ASSERT_NE(p, nullptr);
    const std::string before = json_of(p);
    const std::string scene_id = scene_block_id(before);
    ASSERT_FALSE(scene_id.empty());

    std::this_thread::sleep_for(std::chrono::milliseconds(2));  // avoid same-ms
    EXPECT_EQ(sf_scene_rename(p, "Main Stage"), SF_OK);

    const std::string after = json_of(p);
    EXPECT_NE(after.find("\"name\": \"Main Stage\""), std::string::npos);
    EXPECT_NE(after.find("\"project\": "), std::string::npos);
    // Audit action scene.update reused; detail distinguishes the rename.
    EXPECT_GE(count_actions(after, "scene.update"), 1);
    EXPECT_NE(after.find("rename name=Main Stage"), std::string::npos);
    EXPECT_NE(after.find("\"objectId\": \"" + scene_id + "\""), std::string::npos);
    // modifiedAt bumped.
    EXPECT_NE(string_field(before, "modifiedAt"), string_field(after, "modifiedAt"));

    // Freeze invariant: project.name (and everything else) untouched.
    EXPECT_STREQ(sf_project_get_name(p), "P");
    sf_project_destroy(p);
}

TEST(SceneRename, DoesNotTouchProjectNameOrVenue) {
    sf_project_t* p = sf_project_create("OriginalProject", nullptr);
    ASSERT_NE(p, nullptr);
    const std::string before = json_of(p);
    const std::string proj_id_before = string_field(before, "id");
    const std::string venue_before = string_field(before, "venueRef");

    ASSERT_EQ(sf_scene_rename(p, "SceneX"), SF_OK);
    const std::string after = json_of(p);

    EXPECT_EQ(string_field(after, "id"), proj_id_before);  // project.id stable
    EXPECT_EQ(string_field(after, "venueRef"), venue_before);
    EXPECT_STREQ(sf_project_get_name(p), "OriginalProject");
    sf_project_destroy(p);
}

TEST(SceneRename, SurvivesSaveReopen) {
    sf_project_t* p = sf_project_create("RT", nullptr);
    ASSERT_NE(p, nullptr);
    ASSERT_EQ(sf_scene_rename(p, "Survives"), SF_OK);
    char* json = nullptr;
    size_t len = 0;
    ASSERT_EQ(sf_project_to_json(p, &json, &len), SF_OK);
    sf_project_destroy(p);

    sf_project_t* p2 = nullptr;
    ASSERT_EQ(sf_project_from_json(json, len, &p2), SF_OK);
    ASSERT_NE(p2, nullptr);
    EXPECT_NE(json_of(p2).find("\"name\": \"Survives\""), std::string::npos);
    sf_project_destroy(p2);
    sf_free_string(json);
}

// ---------------------------------------------------------------------------
// Rejection cases
// ---------------------------------------------------------------------------

TEST(SceneRename, RejectsEmptyWhitespaceAndNull) {
    sf_project_t* p = sf_project_create("P", nullptr);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(sf_scene_rename(nullptr, "X"), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_scene_rename(p, nullptr), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_scene_rename(p, ""), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_scene_rename(p, "   "), SF_E_INVALID_ARG);
    EXPECT_EQ(sf_scene_rename(p, "\t\n"), SF_E_INVALID_ARG);
    EXPECT_NE(std::string(sf_last_error(p)).find("non-empty"), std::string::npos);
    sf_project_destroy(p);
}

TEST(SceneRename, CodePointBoundary200Accepted201Rejected) {
    sf_project_t* p = sf_project_create("P", nullptr);
    ASSERT_NE(p, nullptr);

    // 200 ASCII code points accepted.
    EXPECT_EQ(sf_scene_rename(p, std::string(200, 'a').c_str()), SF_OK);
    // 201 ASCII code points rejected (byte cap satisfied, cp cap not).
    EXPECT_EQ(sf_scene_rename(p, std::string(201, 'a').c_str()), SF_E_INVALID_ARG);
    EXPECT_NE(std::string(sf_last_error(p)).find("too long"), std::string::npos);
    // Multibyte: 200 2-byte code points = 400 bytes, accepted.
    std::string cp200;
    for (int i = 0; i < 200; ++i) cp200 += "\xC3\xA9";
    EXPECT_EQ(cp200.size(), 400u);
    EXPECT_EQ(sf_scene_rename(p, cp200.c_str()), SF_OK);
    // 201 2-byte code points = 402 bytes, rejected on code points.
    std::string cp201 = cp200 + "\xC3\xA9";
    EXPECT_EQ(sf_scene_rename(p, cp201.c_str()), SF_E_INVALID_ARG);
    sf_project_destroy(p);
}

TEST(SceneRename, ByteBoundary800Accepted801Rejected) {
    sf_project_t* p = sf_project_create("P", nullptr);
    ASSERT_NE(p, nullptr);

    // 4-byte code points: 200 cps = 800 bytes — both ceilings exactly met.
    std::string cp200x4;
    for (int i = 0; i < 200; ++i) cp200x4 += "\xF0\x9F\x98\x80";
    ASSERT_EQ(cp200x4.size(), 800u);
    EXPECT_EQ(sf_scene_rename(p, cp200x4.c_str()), SF_OK);
    // ORC-P1-2: accepted 600 B / 800 B names must round-trip through to_json.
    EXPECT_TRUE(serializes(p));

    // 201 4-byte cps = 804 bytes: rejected (cp cap; byte cap too).
    std::string cp201x4 = cp200x4 + "\xF0\x9F\x98\x80";
    EXPECT_EQ(sf_scene_rename(p, cp201x4.c_str()), SF_E_INVALID_ARG);

    // Byte ceiling can bite first with fail-open counting: 801 invalid bytes
    // count as 801 chars AND 801 bytes -> rejected.
    std::string bad(801, '\x80');
    EXPECT_EQ(sf_scene_rename(p, bad.c_str()), SF_E_INVALID_ARG);
    sf_project_destroy(p);
}

TEST(SceneRename, Utf8InvalidInputConservativelyRejected) {
    sf_project_t* p = sf_project_create("P", nullptr);
    ASSERT_NE(p, nullptr);
    // 100 malformed bytes -> 100 chars, 100 bytes: accepted (fail-open).
    EXPECT_EQ(sf_scene_rename(p, std::string(100, '\x80').c_str()), SF_OK);
    // ORC-P1-2: malformed bytes must still serialize (U+FFFD), never brick.
    EXPECT_TRUE(serializes(p));
    sf_project_destroy(p);
}

// ---------------------------------------------------------------------------
// ORC-P1-1/2: long / malformed names must serialize (audit detail truncation
// no longer splits UTF-8; dump uses error_handler_t::replace).
// ---------------------------------------------------------------------------

TEST(NameModelSerialization, LongMultibyteNameSerializes) {
    sf_project_t* p = sf_project_create("P", nullptr);
    ASSERT_NE(p, nullptr);
    // 200 3-byte code points = 600 bytes: the audit detail ("rename name="+600B)
    // exceeds the 512-byte ceiling and is truncated at a UTF-8 boundary.
    std::string cp200x3;
    for (int i = 0; i < 200; ++i) cp200x3 += "\xE2\x82\xAC";  // €
    ASSERT_EQ(cp200x3.size(), 600u);
    EXPECT_EQ(sf_scene_rename(p, cp200x3.c_str()), SF_OK);
    EXPECT_TRUE(serializes(p));

    // Save path must also succeed (same replace handler).
    const std::string path = std::string(FIXTURES_DIR) + "/orcp12_tmp.sfproj";
    EXPECT_EQ(sf_project_save_to_path(p, path.c_str()), SF_OK);
    std::remove(path.c_str());
    sf_project_destroy(p);
}

TEST(NameModelSerialization, MalformedNameSerializesWithoutBricking) {
    sf_project_t* p = sf_project_create("P", nullptr);
    ASSERT_NE(p, nullptr);
    // 100 lone continuation bytes: accepted by the fail-open counter, malformed
    // on the wire. to_json and save must both succeed (replace -> U+FFFD).
    const std::string bad(100, '\x80');
    EXPECT_EQ(sf_scene_rename(p, bad.c_str()), SF_OK);
    EXPECT_TRUE(serializes(p));
    const std::string path = std::string(FIXTURES_DIR) + "/orcp12_bad_tmp.sfproj";
    EXPECT_EQ(sf_project_save_to_path(p, path.c_str()), SF_OK);
    std::remove(path.c_str());
    sf_project_destroy(p);
}

// ---------------------------------------------------------------------------
// ORC-P1-3: true freeze invariant — no OTHER mutator writes doc.scene.name
// ---------------------------------------------------------------------------

TEST(SceneRename, FreezeInvariantNoOtherMutatorTouchesSceneName) {
    sf_project_t* p = sf_project_create("P", nullptr);
    ASSERT_NE(p, nullptr);
    ASSERT_EQ(sf_scene_rename(p, "FrozenScene"), SF_OK);
    const std::string frozen = scene_block_name(json_of(p));
    ASSERT_EQ(frozen, "FrozenScene");

    // Every non-scene mutator must leave scene.name byte-identical.
    EXPECT_EQ(sf_project_rename(p, "NewProjectName"), SF_OK);
    EXPECT_EQ(scene_block_name(json_of(p)), frozen);

    EXPECT_EQ(sf_venue_rename(p, "NewVenueName"), SF_OK);
    EXPECT_EQ(scene_block_name(json_of(p)), frozen);

    char id[37];
    EXPECT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, "NodeA", id), SF_OK);
    EXPECT_EQ(scene_block_name(json_of(p)), frozen);

    // And each still serializes cleanly.
    EXPECT_TRUE(serializes(p));
    sf_project_destroy(p);
}

// ---------------------------------------------------------------------------
// Relaxed project/venue caps (bytes -> code points)
// ---------------------------------------------------------------------------

TEST(NameCaps, ProjectRenameMultibyteUpTo200CodePoints) {
    sf_project_t* p = sf_project_create("R", nullptr);
    ASSERT_NE(p, nullptr);
    // 200 2-byte code points = 400 bytes: previously rejected on bytes, now ok.
    std::string cp200;
    for (int i = 0; i < 200; ++i) cp200 += "\xC3\xA9";
    EXPECT_EQ(sf_project_rename(p, cp200.c_str()), SF_OK);
    // ORC-P1-2: accepted long multibyte project name must serialize.
    EXPECT_TRUE(serializes(p));
    // Project 200 4-byte cps = 800 bytes (both ceilings exactly met): serialize.
    std::string cp200x4;
    for (int i = 0; i < 200; ++i) cp200x4 += "\xF0\x9F\x98\x80";
    ASSERT_EQ(cp200x4.size(), 800u);
    EXPECT_EQ(sf_project_rename(p, cp200x4.c_str()), SF_OK);
    EXPECT_TRUE(serializes(p));
    EXPECT_EQ(sf_project_rename(p, (cp200 + "\xC3\xA9").c_str()), SF_E_INVALID_ARG);
    sf_project_destroy(p);
}

TEST(NameCaps, VenueRenameMultibyteUpTo200CodePoints) {
    sf_project_t* p = sf_project_create("R", nullptr);
    ASSERT_NE(p, nullptr);
    std::string cp200;
    for (int i = 0; i < 200; ++i) cp200 += "\xE2\x82\xAC";  // 3-byte €
    EXPECT_EQ(cp200.size(), 600u);
    EXPECT_EQ(sf_venue_rename(p, cp200.c_str()), SF_OK);
    EXPECT_EQ(sf_venue_rename(p, (cp200 + "\xE2\x82\xAC").c_str()), SF_E_INVALID_ARG);
    sf_project_destroy(p);
}

TEST(NameCaps, VenueRenameByteCeiling800) {
    sf_project_t* p = sf_project_create("R", nullptr);
    ASSERT_NE(p, nullptr);
    // 801 invalid bytes -> fail-open 801 chars -> rejected (byte + cp cap).
    EXPECT_EQ(sf_venue_rename(p, std::string(801, '\x80').c_str()), SF_E_INVALID_ARG);
    sf_project_destroy(p);
}

// ---------------------------------------------------------------------------
// Relaxed node-label cap (64 bytes -> 64 code points, byte ceiling 256)
// ---------------------------------------------------------------------------

TEST(NameCaps, NodeLabelMultibyteUpTo64CodePoints) {
    sf_project_t* p = sf_project_create("R", nullptr);
    ASSERT_NE(p, nullptr);
    char id[37];
    // 64 3-byte code points = 192 bytes: >64 bytes but <=64 cps -> accepted.
    std::string label64;
    for (int i = 0; i < 64; ++i) label64 += "\xE2\x82\xAC";
    ASSERT_GT(label64.size(), 64u);
    EXPECT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, label64.c_str(), id), SF_OK);

    // 65 code points rejected with the updated (>64 chars) message.
    std::string label65 = label64 + "\xE2\x82\xAC";
    EXPECT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, label65.c_str(), id),
              SF_E_INVALID_ARG);
    EXPECT_NE(std::string(sf_last_error(p)).find(">64 chars"), std::string::npos);
    sf_project_destroy(p);
}

TEST(NameCaps, NodeLabelByteCeiling256) {
    sf_project_t* p = sf_project_create("R", nullptr);
    ASSERT_NE(p, nullptr);
    char id[37];
    // 4-byte cps: 64 cps = 256 bytes exactly -> accepted.
    std::string label64x4;
    for (int i = 0; i < 64; ++i) label64x4 += "\xF0\x9F\x98\x80";
    ASSERT_EQ(label64x4.size(), 256u);
    EXPECT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, label64x4.c_str(), id), SF_OK);
    // 65 cps = 260 bytes -> rejected.
    EXPECT_EQ(sf_graph_add_node(p, SF_NODE_SOURCE, (label64x4 + "\xF0\x9F\x98\x80").c_str(), id),
              SF_E_INVALID_ARG);
    sf_project_destroy(p);
}
