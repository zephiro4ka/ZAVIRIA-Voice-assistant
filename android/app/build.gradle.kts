plugins {
    id("com.android.application")
    id("org.jetbrains.kotlin.android")
}

android {
    namespace = "com.astrix.phone"
    compileSdk = 35
    buildToolsVersion = "35.0.0"

    defaultConfig {
        applicationId = "com.astrix.phone"
        minSdk = 23
        targetSdk = 35
        // номер сборки — минуты от 1970 года: каждая новая сборка «новее», телефон сам видит обновление на ПК
        val build = (System.currentTimeMillis() / 60000).toInt()
        versionCode = build
        versionName = "1.5"
        layout.buildDirectory.get().asFile.apply { mkdirs(); resolve("zaviria-version.txt").writeText(build.toString()) }
    }
    compileOptions {
        sourceCompatibility = JavaVersion.VERSION_17
        targetCompatibility = JavaVersion.VERSION_17
    }
    kotlinOptions { jvmTarget = "17" }
}

dependencies {
    implementation("com.squareup.okhttp3:okhttp:4.12.0")
}
