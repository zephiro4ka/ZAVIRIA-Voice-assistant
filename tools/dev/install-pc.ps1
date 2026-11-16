# Собрать ZAVIRIA (Release) и поставить в установленную копию: закрыть, заменить ZAVIRIA.exe, запустить снова.
# Запуск: pwsh -File tools\dev\install-pc.ps1   (-NoInstall — только собрать)
param([switch]$NoInstall)
$root = Split-Path (Split-Path $PSScriptRoot)
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe'
$release = "$root\ZAVIRIA\x64\Release"
$app = "$env:LOCALAPPDATA\ZAVIRIA"

$out = & $msbuild "$root\ZAVIRIA\ZAVIRIA.vcxproj" /p:Configuration=Release /p:Platform=x64 /m /v:m /nologo 2>&1
if ($LASTEXITCODE -ne 0) {
    $out | Select-String ' error ' | Select-Object -First 15 | ForEach-Object { $_.Line }
    'СБОРКА НЕ ПРОШЛА — установленная ZAVIRIA не тронута'
    exit 1
}
'сборка: ок'
if ($NoInstall) { exit 0 }

Get-Process ZAVIRIA -ErrorAction SilentlyContinue | Where-Object { $_.Path -like "$app*" } | Stop-Process -Force
# программа раньше называлась ASTRIX: один раз переносим её папку целиком (настройки, модели), старый exe убираем
$old = "$env:LOCALAPPDATA\ASTRIX"
if ((Test-Path $old) -and -not (Test-Path $app)) {
    Get-Process ASTRIX -ErrorAction SilentlyContinue | Stop-Process -Force
    Start-Sleep 2
    Rename-Item $old 'ZAVIRIA' -ErrorAction Stop
    Remove-Item "$app\ASTRIX.exe" -Force -ErrorAction SilentlyContinue
    'перенесла папку ASTRIX → ZAVIRIA'
}
Start-Sleep 2
try { Copy-Item "$release\ZAVIRIA.exe" "$app\ZAVIRIA.exe" -Force -ErrorAction Stop }
catch { 'НЕ ЗАМЕНИЛА ZAVIRIA.exe — файл занят (зависшая копия?). Старая версия осталась: ' + $_.Exception.Message; exit 2 }
if ((Get-Item "$app\ZAVIRIA.exe").Length -ne (Get-Item "$release\ZAVIRIA.exe").Length) { 'НЕ ЗАМЕНИЛА ZAVIRIA.exe — размер не совпал'; exit 2 }
$p = Start-Process "$app\ZAVIRIA.exe" -WorkingDirectory $app -PassThru
Start-Sleep 6
'установлено, ZAVIRIA запущена: ' + (-not $p.HasExited)
