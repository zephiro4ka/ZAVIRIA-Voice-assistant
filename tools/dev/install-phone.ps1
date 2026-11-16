# Собрать APK телефона (debug) и поставить на подключённый по USB телефон.
# Запуск: pwsh -File tools\dev\install-phone.ps1   (-NoInstall — только собрать)
# Телефон: Honor 400 Lite, Android 16. Отладка по USB должна быть включена; у Honor есть второй пользователь (100),
# поэтому ставим с --user 0. adb без телефона зависает — все вызовы с ограничением по времени.
param([switch]$NoInstall)
$root = Split-Path (Split-Path $PSScriptRoot)
$env:JAVA_HOME = "$env:LOCALAPPDATA\Android\jdk"
$env:ANDROID_HOME = "$env:LOCALAPPDATA\Android\Sdk"
$adb = "$env:ANDROID_HOME\platform-tools\adb.exe"
$apk = "$root\android\app\build\outputs\apk\debug\app-debug.apk"

Push-Location "$root\android"
$out = & "$env:LOCALAPPDATA\Android\gradle-8.9\bin\gradle.bat" assembleDebug --no-daemon -q 2>&1 # свой Gradle 8.9 — без скачивания обёртки
$code = $LASTEXITCODE
Pop-Location
if ($code -ne 0) { $out | Select-String '^e:|error:' | Select-Object -First 15 | ForEach-Object { $_.Line }; 'СБОРКА НЕ ПРОШЛА'; exit 1 }
"сборка: ок, $([int]((Get-Item $apk).Length / 1KB)) КБ"
# APK и номер версии — рядом с ZAVIRIA на ПК: телефон обновится оттуда сам (Настройки → Обновление приложения),
# а бот пришлёт его по команде /apk — USB и отладка не нужны
$version = Get-Content "$root\android\app\build\zaviria-version.txt" -ErrorAction SilentlyContinue
foreach ($dir in "$root\ZAVIRIA\x64\Release\phone", "$env:LOCALAPPDATA\ZAVIRIA\phone") {
    if (-not (Test-Path (Split-Path $dir))) { continue }
    New-Item -ItemType Directory -Force $dir | Out-Null
    Copy-Item -LiteralPath $apk "$dir\zaviria.apk" -Force
    if ($version) { Set-Content -LiteralPath "$dir\version.txt" $version -NoNewline -Encoding ascii }
}
"для обновления с ПК: версия $version"
if ($NoInstall) { exit 0 }

$devices = Start-Job { param($a) & $a devices 2>&1 } -ArgumentList $adb
if (-not (Wait-Job $devices -Timeout 20)) { Stop-Job $devices; 'adb не ответил — переподключи телефон'; exit 1 }
if (-not ((Receive-Job $devices) -match '\sdevice$')) { 'по USB телефона нет — обнови с телефона: Настройки → Обновление приложения (или /apk в боте)'; exit 0 }

$copy = "$env:TEMP\zaviria-phone.apk" # путь с кириллицей adb не любит
Copy-Item -LiteralPath $apk $copy -Force
$job = Start-Job { param($a, $c) & $a install -r -t --user 0 $c 2>&1 | Select-Object -Last 1 } -ArgumentList $adb, $copy
if (Wait-Job $job -Timeout 180) { 'установка: ' + (Receive-Job $job) } else { Stop-Job $job; 'установка не успела за 3 минуты' }
Remove-Item $copy -ErrorAction SilentlyContinue
$start = Start-Job { param($a) & $a shell am start -n com.astrix.phone/.MainActivity 2>&1 } -ArgumentList $adb # без тайм-аута мог висеть
if (-not (Wait-Job $start -Timeout 20)) { Stop-Job $start }
# падение приложения на Honor: журнал зашифрован, отчёт смотреть так:
#   adb shell dumpsys dropbox --print data_app_crash
