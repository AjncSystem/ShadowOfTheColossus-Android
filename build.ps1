# Builds the APK and, with -Install, installs and starts it on the connected device.
#   .\build.ps1                 debug APK
#   .\build.ps1 -Install        build + adb install + launch + follow logcat (tag SotC)
#   .\build.ps1 -Release        release APK (signed with the debug key for now)
param(
    [switch]$Install,
    [switch]$Release
)
# Native tools (gradle, git, adb) write progress to stderr; failures are caught by exit code.
$ErrorActionPreference = 'Continue'
$root = $PSScriptRoot

# Toolchain shared with the Hokkaido Fork setup on this PC; override with env vars if moved.
if (-not $env:ANDROID_HOME) { $env:ANDROID_HOME = 'E:\Hokkaido Fork\android-sdk' }
if (-not $env:GRADLE_USER_HOME) { $env:GRADLE_USER_HOME = 'E:\Hokkaido Fork\gradle-home' }
if (-not $env:JAVA_HOME) { $env:JAVA_HOME = 'C:\Program Files\Microsoft\jdk-21.0.11.10-hotspot' }
$adb = Join-Path $env:ANDROID_HOME 'platform-tools\adb.exe'

# The FEX build runs gzip (man page); Git for Windows ships one.
$gitUsr = 'C:\Program Files\Git\usr\bin'
if ((Test-Path $gitUsr) -and -not ($env:PATH -split ';' -contains $gitUsr)) { $env:PATH = "$env:PATH;$gitUsr" }
# The GPU library's host shaders are compiled by glslangValidator (MSYS2 CLANG64, needs its DLLs).
foreach ($msys in @($env:SOTC_GLSLANG_DIR, 'I:\msys64\clang64\bin', 'C:\msys64\clang64\bin')) {
    if ($msys -and (Test-Path "$msys\glslangValidator.exe")) {
        if (-not ($env:PATH -split ';' -contains $msys)) { $env:PATH = "$env:PATH;$msys" }
        break
    }
}

# Local patches on the pinned FEX checkout (never sent upstream, see docs/PLANO.md).
$fex = Join-Path $root 'externals\FEX'
foreach ($patch in Get-ChildItem (Join-Path $root 'patches\fex\*.patch') | Sort-Object Name) {
    git -C $fex apply --reverse --check $patch.FullName 2>$null
    if ($LASTEXITCODE -ne 0) {
        git -C $fex apply $patch.FullName
        if ($LASTEXITCODE -ne 0) { Write-Host "ERROR: cannot apply $($patch.Name)"; exit 1 }
        Write-Host "applied $($patch.Name)"
    }
}

$task = if ($Release) { 'assembleRelease' } else { 'assembleDebug' }
& "$root\gradlew.bat" -p $root $task
if ($LASTEXITCODE -ne 0) { Write-Host "ERROR: gradle $task failed"; exit 1 }

$variant = if ($Release) { 'release' } else { 'debug' }
$apk = Get-ChildItem "$root\app\build\outputs\apk\$variant\*.apk" | Select-Object -First 1
Write-Host "APK: $($apk.FullName) ($([math]::Round($apk.Length / 1MB, 1)) MB)"

if ($Install) {
    & $adb install -r $apk.FullName
    if ($LASTEXITCODE -ne 0) { throw 'adb install failed' }
    & $adb logcat -c
    & $adb shell am start -n io.github.ajncsystem.sotc/.SotcActivity
    & $adb logcat -s SotC:V SDL:V SDL/APP:V AndroidRuntime:E DEBUG:V
}
