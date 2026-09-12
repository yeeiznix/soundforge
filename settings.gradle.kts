// SoundForge G0 — Gradle settings (docs/PLAN_G0.md §2.1).
// Repositories are declared here only; modules must not add their own.
pluginManagement {
    repositories {
        google()
        mavenCentral()
        gradlePluginPortal()
    }
}

dependencyResolutionManagement {
    repositoriesMode.set(RepositoriesMode.FAIL_ON_PROJECT_REPOS)
    repositories {
        google()
        mavenCentral()
    }
}

rootProject.name = "SoundForge"
include(":app")
