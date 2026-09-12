// SoundForge G0 — thin JNI passthrough between Kotlin (NativeBridge) and the
// sfcore C ABI (docs/PLAN_G0.md §4.2–4.3). No logic lives here:
//   jstring <-> const char*,  jlong <-> sf_project_t* (0 means null).
// JNI_OnLoad installs a native logcat sink via sf_set_log_sink, so Kotlin
// needs no Java-side callback; every sf_log line is forwarded under "SF/<tag>".
// The 12 exported functions (one per NativeBridge external fun) all use the
// full symbol prefix Java_id_soundforge_pastudio_platform_bridge_NativeBridge_.
// G0: synchronous calls; G2 introduces SfCommandQueue for audio-thread safety.
// G1: 16 exported functions (12 + renameProject, renameVenue,
// setVenueDimensions, setSceneGeometry) — one per NativeBridge external fun.
#include <jni.h>
#include <android/log.h>

#include <cstdint>
#include <cstdio>
#include <string>

#include "soundforge/sf_diagnostics.h"
#include "soundforge/sf_project.h"
#include "soundforge/sf_schema.h"  // sf_validate_project_json

namespace {

// sf_types.h order: DEBUG, INFO, WARN, ERROR -> logcat priorities.
android_LogPriority to_priority(int level) {
  switch (level) {
    case SF_LOG_DEBUG: return ANDROID_LOG_DEBUG;
    case SF_LOG_INFO: return ANDROID_LOG_INFO;
    case SF_LOG_WARN: return ANDROID_LOG_WARN;
    case SF_LOG_ERROR: return ANDROID_LOG_ERROR;
    default: return ANDROID_LOG_INFO;
  }
}

// Native sink installed in JNI_OnLoad. Plain C function pointer, no Java
// callback — the JVM keeps running even during flushes/crashes.
void sf_android_log_sink(sf_log_level_t level, const char* tag, const char* msg) {
  const std::string full_tag = std::string("SF/") + (tag ? tag : "core");
  __android_log_print(to_priority(static_cast<int>(level)), full_tag.c_str(), "%s",
                      msg ? msg : "");
}

sf_project_t* to_handle(jlong h) {
  return h == 0 ? nullptr : reinterpret_cast<sf_project_t*>(static_cast<uintptr_t>(h));
}

jlong to_jlong(const sf_project_t* p) {
  return p == nullptr ? 0 : static_cast<jlong>(reinterpret_cast<uintptr_t>(p));
}

// Copy a jstring into std::string (JNI modified UTF-8 — fine for G0 names and
// JSON). Returns "" for null or when allocation fails (Java OOM already
// pending in that case).
std::string to_std(JNIEnv* env, jstring s) {
  if (s == nullptr) return std::string();
  const char* c = env->GetStringUTFChars(s, nullptr);
  if (c == nullptr) return std::string();
  std::string out(c);
  env->ReleaseStringUTFChars(s, c);
  return out;
}

// Boundary guard: the C ABI never throws (its internal catches map failures to
// SF_E_* + last_error), but a contract violation must not crash the JVM.
void log_boundary_exception(JNIEnv* env, const char* where, const char* what) {
  if (env->ExceptionCheck()) return;  // a Java exception is already pending
  __android_log_print(ANDROID_LOG_ERROR, "SF/jni", "%s: %s", where,
                      what ? what : "unknown native error");
}

// Fallback health report: Kotlin's healthCheck() returns a non-null String,
// so the native side must never hand back a null jstring for it.
constexpr char kHealthErrorJson[] =
    "{\"status\":\"error\",\"warnings\":[],"
    "\"errors\":[\"health check failed (see last error)\"],\"stats\":{}}";

}  // namespace

extern "C" JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* /*vm*/, void* /*reserved*/) {
  sf_set_log_sink(sf_android_log_sink);
  sf_log(SF_LOG_INFO, "jni", "libsfcore loaded");
  return JNI_VERSION_1_6;
}

// --- Version ---------------------------------------------------------------

extern "C" JNIEXPORT jstring JNICALL
Java_id_soundforge_pastudio_platform_bridge_NativeBridge_engineVersion(JNIEnv* env, jobject /*thiz*/) {
  try {
    return env->NewStringUTF(sf_engine_version());
  } catch (const std::exception& e) {
    log_boundary_exception(env, "engineVersion", e.what());
    return env->NewStringUTF("unknown");  // Kotlin engineVersion() is non-null String
  } catch (...) {
    log_boundary_exception(env, "engineVersion", "unknown");
    return env->NewStringUTF("unknown");
  }
}

extern "C" JNIEXPORT jint JNICALL
Java_id_soundforge_pastudio_platform_bridge_NativeBridge_schemaVersion(JNIEnv* /*env*/, jobject /*thiz*/) {
  try {
    return sf_schema_version();
  } catch (...) {
    return -1;
  }
}

extern "C" JNIEXPORT jboolean JNICALL
Java_id_soundforge_pastudio_platform_bridge_NativeBridge_isCompatible(JNIEnv* /*env*/, jobject /*thiz*/,
                                                      jint schema_version) {
  try {
    return sf_is_compatible(schema_version) == 1 ? JNI_TRUE : JNI_FALSE;
  } catch (...) {
    return JNI_FALSE;
  }
}

// --- Editor mutators (G1) ---------------------------------------------------
// Thin passthroughs to the sf_project_* mutators (PLAN_G1 §4.2). Result is the
// SF_* code; callers map != SF_OK to lastError(handle) on the same thread.
// All three require a non-null handled project.

extern "C" JNIEXPORT jint JNICALL
Java_id_soundforge_pastudio_platform_bridge_NativeBridge_renameProject(JNIEnv* env, jobject /*thiz*/,
                                                                       jlong handle, jstring new_name) {
  try {
    return static_cast<jint>(sf_project_rename(to_handle(handle), to_std(env, new_name).c_str()));
  } catch (const std::exception& e) {
    log_boundary_exception(env, "renameProject", e.what());
    return SF_E_INVALID_ARG;
  } catch (...) {
    log_boundary_exception(env, "renameProject", "unknown");
    return SF_E_INVALID_ARG;
  }
}

extern "C" JNIEXPORT jint JNICALL
Java_id_soundforge_pastudio_platform_bridge_NativeBridge_renameVenue(JNIEnv* env, jobject /*thiz*/,
                                                                     jlong handle, jstring new_name) {
  try {
    return static_cast<jint>(sf_venue_rename(to_handle(handle), to_std(env, new_name).c_str()));
  } catch (const std::exception& e) {
    log_boundary_exception(env, "renameVenue", e.what());
    return SF_E_INVALID_ARG;
  } catch (...) {
    log_boundary_exception(env, "renameVenue", "unknown");
    return SF_E_INVALID_ARG;
  }
}

extern "C" JNIEXPORT jint JNICALL
Java_id_soundforge_pastudio_platform_bridge_NativeBridge_setVenueDimensions(JNIEnv* env, jobject /*thiz*/,
                                                                           jlong handle, jdouble w,
                                                                           jdouble d, jdouble h) {
  try {
    return static_cast<jint>(
        sf_venue_set_dimensions(to_handle(handle), static_cast<double>(w),
                                static_cast<double>(d), static_cast<double>(h)));
  } catch (const std::exception& e) {
    log_boundary_exception(env, "setVenueDimensions", e.what());
    return SF_E_INVALID_ARG;
  } catch (...) {
    log_boundary_exception(env, "setVenueDimensions", "unknown");
    return SF_E_INVALID_ARG;
  }
}

extern "C" JNIEXPORT jint JNICALL
Java_id_soundforge_pastudio_platform_bridge_NativeBridge_setSceneGeometry(JNIEnv* env, jobject /*thiz*/,
                                                                         jlong handle, jdouble cx,
                                                                         jdouble cy, jdouble cz,
                                                                         jdouble lx, jdouble ly,
                                                                         jdouble lz) {
  try {
    return static_cast<jint>(
        sf_scene_set_geometry(to_handle(handle), static_cast<double>(cx),
                              static_cast<double>(cy), static_cast<double>(cz),
                              static_cast<double>(lx), static_cast<double>(ly),
                              static_cast<double>(lz)));
  } catch (const std::exception& e) {
    log_boundary_exception(env, "setSceneGeometry", e.what());
    return SF_E_INVALID_ARG;
  } catch (...) {
    log_boundary_exception(env, "setSceneGeometry", "unknown");
    return SF_E_INVALID_ARG;
  }
}

// --- Lifecycle -------------------------------------------------------------

extern "C" JNIEXPORT jlong JNICALL
Java_id_soundforge_pastudio_platform_bridge_NativeBridge_projectCreate(JNIEnv* env, jobject /*thiz*/,
                                                       jstring name, jstring author) {
  try {
    const std::string n = to_std(env, name);
    std::string a;  // kept alive until the create call below
    if (author != nullptr) {
      a = to_std(env, author);
    }
    const char* author_c = author != nullptr ? a.c_str() : nullptr;  // NULL author allowed
    return to_jlong(sf_project_create(n.c_str(), author_c));
    // On 0: caller reads lastError(0) (thread-local) on the same thread.
  } catch (const std::exception& e) {
    log_boundary_exception(env, "projectCreate", e.what());
    return 0;
  } catch (...) {
    log_boundary_exception(env, "projectCreate", "unknown");
    return 0;
  }
}

extern "C" JNIEXPORT void JNICALL
Java_id_soundforge_pastudio_platform_bridge_NativeBridge_projectDestroy(JNIEnv* /*env*/, jobject /*thiz*/,
                                                        jlong handle) {
  if (handle != 0) {  // guard: 0L means "no project"
    sf_project_destroy(to_handle(handle));  // NULL handle is a no-op in the core
  }
}

// --- JSON codec ------------------------------------------------------------

extern "C" JNIEXPORT jstring JNICALL
Java_id_soundforge_pastudio_platform_bridge_NativeBridge_projectToJson(JNIEnv* env, jobject /*thiz*/,
                                                                       jlong handle) {
  char* json = nullptr;
  size_t len = 0;
  try {
    const sf_result_t rc = sf_project_to_json(to_handle(handle), &json, &len);
    if (rc != SF_OK || json == nullptr) {
      if (json != nullptr) sf_free_string(json);
      return env->NewStringUTF("");  // G0: empty string on error, no exception
    }
    jstring out = env->NewStringUTF(json);
    sf_free_string(json);
    return out;
  } catch (const std::exception& e) {
    if (json != nullptr) sf_free_string(json);
    log_boundary_exception(env, "projectToJson", e.what());
    return env->NewStringUTF("");
  } catch (...) {
    if (json != nullptr) sf_free_string(json);
    log_boundary_exception(env, "projectToJson", "unknown");
    return env->NewStringUTF("");
  }
}

extern "C" JNIEXPORT jlong JNICALL
Java_id_soundforge_pastudio_platform_bridge_NativeBridge_projectFromJson(JNIEnv* env, jobject /*thiz*/,
                                                         jstring json) {
  try {
    const std::string s = to_std(env, json);
    sf_project_t* out = nullptr;
    sf_project_from_json(s.c_str(), s.size(), &out);
    return to_jlong(out);  // 0 on error; caller reads lastError(0) on same thread
  } catch (const std::exception& e) {
    log_boundary_exception(env, "projectFromJson", e.what());
    return 0;
  } catch (...) {
    log_boundary_exception(env, "projectFromJson", "unknown");
    return 0;
  }
}

// --- Validation / health ---------------------------------------------------

// Null return = valid; otherwise the human-readable error string.
extern "C" JNIEXPORT jstring JNICALL
Java_id_soundforge_pastudio_platform_bridge_NativeBridge_validateJson(JNIEnv* env, jobject /*thiz*/,
                                                      jstring json) {
  try {
    const std::string s = to_std(env, json);
    char err[1024];
    err[0] = '\0';
    const sf_result_t rc = sf_validate_project_json(s.c_str(), s.size(), err, sizeof(err));
    if (rc == SF_OK) return nullptr;
    if (err[0] == '\0') {
      std::snprintf(err, sizeof(err), "invalid project JSON (code %d)", static_cast<int>(rc));
    }
    return env->NewStringUTF(err);
  } catch (const std::exception& e) {
    log_boundary_exception(env, "validateJson", e.what());
    return env->NewStringUTF("validateJson: native exception");
  } catch (...) {
    log_boundary_exception(env, "validateJson", "unknown");
    return env->NewStringUTF("validateJson: native exception");
  }
}

// Always returns the JSON report string; warnings/errors travel in-band
// ("status":"ok"|"warning"|"error").
extern "C" JNIEXPORT jstring JNICALL
Java_id_soundforge_pastudio_platform_bridge_NativeBridge_healthCheck(JNIEnv* env, jobject /*thiz*/,
                                                     jlong handle) {
  try {
    char report[8192];
    report[0] = '\0';
    const sf_result_t rc = sf_project_health_check(to_handle(handle), report, sizeof(report));
    if (rc != SF_OK && report[0] == '\0') {
      return env->NewStringUTF(kHealthErrorJson);
    }
    return env->NewStringUTF(report);
  } catch (const std::exception& e) {
    log_boundary_exception(env, "healthCheck", e.what());
    return env->NewStringUTF(kHealthErrorJson);
  } catch (...) {
    log_boundary_exception(env, "healthCheck", "unknown");
    return env->NewStringUTF(kHealthErrorJson);
  }
}

// --- Diagnostics -----------------------------------------------------------

// handle==0 reads the calling thread's last error (thread-local in sfcore).
extern "C" JNIEXPORT jstring JNICALL
Java_id_soundforge_pastudio_platform_bridge_NativeBridge_lastError(JNIEnv* env, jobject /*thiz*/, jlong handle) {
  try {
    const char* err = sf_last_error(to_handle(handle));
    return env->NewStringUTF(err != nullptr ? err : "no error");
  } catch (...) {
    return env->NewStringUTF("no error");
  }
}

extern "C" JNIEXPORT void JNICALL
Java_id_soundforge_pastudio_platform_bridge_NativeBridge_log(JNIEnv* env, jobject /*thiz*/, jint level,
                                             jstring tag, jstring msg) {
  try {
    const std::string t = to_std(env, tag);
    const std::string m = to_std(env, msg);
    int lvl = static_cast<int>(level);
    // Clamp: the core's level_name() table indexes 0..3; out-of-range input
    // from Kotlin would be an out-of-bounds read there.
    if (lvl < SF_LOG_DEBUG) lvl = SF_LOG_DEBUG;
    if (lvl > SF_LOG_ERROR) lvl = SF_LOG_ERROR;
    sf_log(static_cast<sf_log_level_t>(lvl), t.c_str(), m.c_str());
  } catch (const std::exception& e) {
    log_boundary_exception(env, "log", e.what());
  } catch (...) {
    log_boundary_exception(env, "log", "unknown");
  }
}

// sf_flush_logs(const char* path) — SfLogger passes filesDir/logs/sf.log.
extern "C" JNIEXPORT jint JNICALL
Java_id_soundforge_pastudio_platform_bridge_NativeBridge_flushLogs(JNIEnv* env, jobject /*thiz*/, jstring path) {
  try {
    const std::string p = to_std(env, path);
    return static_cast<jint>(sf_flush_logs(p.c_str()));
  } catch (const std::exception& e) {
    log_boundary_exception(env, "flushLogs", e.what());
    return SF_E_IO;
  } catch (...) {
    log_boundary_exception(env, "flushLogs", "unknown");
    return SF_E_IO;
  }
}
