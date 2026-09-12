#include <gtest/gtest.h>

#include "soundforge/sf_project.h"
#include "soundforge/sf_diagnostics.h"

#include <cstring>
#include <thread>
#include <vector>

TEST(KotlinBridgeJni, LifecycleStress) {
    for (int i = 0; i < 100; ++i) {
        sf_project_t* p = sf_project_create("Stress", nullptr);
        ASSERT_NE(p, nullptr);
        char* json = nullptr;
        size_t len = 0;
        EXPECT_EQ(sf_project_to_json(p, &json, &len), SF_OK);
        sf_free_string(json);
        sf_project_destroy(p);
    }
}

TEST(KotlinBridgeJni, FromJsonVariations) {
    const char* inputs[] = {
        "not json",
        "{}",
        "{\"schemaVersion\":1,\"engineVersion\":\"0.1.0-g0\"}"
    };
    for (const char* s : inputs) {
        sf_project_t* p = nullptr;
        sf_project_from_json(s, std::strlen(s), &p);
        // Should not crash; some return null.
        if (p) sf_project_destroy(p);
    }
}

TEST(KotlinBridgeJni, ConcurrentCreateDestroy) {
    std::vector<std::thread> threads;
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([] {
            for (int j = 0; j < 50; ++j) {
                sf_project_t* p = sf_project_create("Concurrent", nullptr);
                ASSERT_NE(p, nullptr);
                sf_project_destroy(p);
            }
        });
    }
    for (auto& t : threads) t.join();
}
