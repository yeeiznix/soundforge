// SoundForge G0 — :app module (docs/PLAN_G0.md §2.1–2.3).
// Compiles the Kotlin/Compose shell + the sfcore shared library via CMake.
// NOTE: no Android SDK in the G0 dev container; validated by review only.
plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "id.soundforge.pastudio"
    compileSdk = 34

    defaultConfig {
        applicationId = "id.soundforge.pastudio"
        minSdk = 26
        targetSdk = 34
        versionCode = 1
        versionName = "0.1.0-g0"

        testInstrumentationRunner = "androidx.test.runner.AndroidJUnitRunner"

        ndk {
            // G0: device = arm64-v8a, emulator = x86_64.
            abiFilters += listOf("arm64-v8a", "x86_64")
        }
    }

    // Kotlin sources live at module-root ui/ and platform/ (plan §2.2–2.3),
    // not under src/main/java. Manifest/res use the default src/main locations.
    sourceSets {
        getByName("main") {
            java.srcDirs("ui", "platform")
            kotlin.srcDirs("ui", "platform")
        }
    }

    externalNativeBuild {
        cmake {
            path = file("src/main/cpp/CMakeLists.txt")
            version = "3.22.1"
        }
    }

    buildFeatures {
        compose = true
    }
    composeOptions {
        kotlinCompilerExtensionVersion = "1.5.5"
    }

    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions {
        jvmTarget = "17"
    }

    // G0: default (uncompressed) native-lib packaging; extractNativeLibs=false.
    packaging {
        jniLibs {
            useLegacyPackaging = false
        }
    }
}

dependencies {
    implementation("androidx.activity:activity-compose:1.8.2")
    implementation(platform("androidx.compose:compose-bom:2024.02.01"))
    implementation("androidx.compose.ui:ui")
    implementation("androidx.compose.material3:material3")
    implementation("androidx.compose.ui:ui-tooling-preview")
    implementation("androidx.navigation:navigation-compose:2.7.7")
    implementation("androidx.lifecycle:lifecycle-runtime-compose:2.7.0")
    implementation("androidx.lifecycle:lifecycle-viewmodel-compose:2.7.0")
    implementation("org.jetbrains.kotlinx:kotlinx-coroutines-android:1.7.3")

    debugImplementation("androidx.compose.ui:ui-tooling")

    testImplementation("junit:junit:4.13.2")

    androidTestImplementation("androidx.test.ext:junit:1.1.5")
    androidTestImplementation("androidx.test:core:1.5.0")
    androidTestImplementation("androidx.test:runner:1.5.2")
    androidTestImplementation("androidx.test:rules:1.5.2")
    androidTestImplementation(platform("androidx.compose:compose-bom:2024.02.01"))
    androidTestImplementation("androidx.compose.ui:ui-test-junit4")
    // Test-manifest injection must be on the APP debug classpath to work.
    debugImplementation("androidx.compose.ui:ui-test-manifest")
}
