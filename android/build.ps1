# K-Line OBD Android uygulamasi: Gradle'siz derleme (Android SDK build-tools + JDK).
# Gerekenler: JDK 17+, Android SDK (platforms;android-34, build-tools;35.0.0).
# Kullanim:  pwsh -File android\build.ps1   (ya da: powershell -ExecutionPolicy Bypass -File android\build.ps1)
# Cikti:     android\build\K-Line-OBD.apk
param(
    [string]$Sdk = $(if ($env:ANDROID_HOME) { $env:ANDROID_HOME } else { "$env:LOCALAPPDATA\Android\Sdk" }),
    [int]$VersionCode = 1,
    [string]$VersionName = "1.0"
)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot
$bt = "$Sdk\build-tools\35.0.0"
$jar = "$Sdk\platforms\android-34\android.jar"
$out = "$here\build"
foreach ($p in @("$bt\aapt2.exe", "$bt\aapt.exe", "$bt\zipalign.exe", "$bt\d8.bat", "$bt\apksigner.bat", $jar)) {
    if (-not (Test-Path $p)) { throw "Bulunamadi: $p (sdkmanager ile platforms;android-34 ve build-tools;35.0.0 kur)" }
}
# yerel komutlar dogrudan cagrilir, her birinden sonra cikis kodu kontrol edilir
function Check($what) {
    if ($LASTEXITCODE -ne 0) { throw "$what basarisiz ($LASTEXITCODE)" }
}

if (Test-Path $out) { Remove-Item -Recurse -Force $out }
New-Item -ItemType Directory -Force "$out\assets", "$out\classes", "$out\dex" | Out-Null

# panel sayfasi tek kaynaktan: docs/panel.html
Copy-Item "$here\..\docs\panel.html" "$out\assets\panel.html"

# 1) kaynaklar + manifest
& "$bt\aapt2.exe" compile --dir "$here\res" -o "$out\res.zip"; Check "aapt2 compile"
& "$bt\aapt2.exe" link -o "$out\base.apk" -I $jar --manifest "$here\AndroidManifest.xml" -A "$out\assets" `
    --min-sdk-version 23 --target-sdk-version 34 --version-code $VersionCode --version-name $VersionName "$out\res.zip"
Check "aapt2 link"

# 2) Java -> dex
$src = Get-ChildItem "$here\src" -Recurse -Filter *.java | ForEach-Object { $_.FullName }
& javac -nowarn --release 8 -encoding UTF-8 -classpath $jar -d "$out\classes" $src; Check "javac"
# d8 tek tek .class dosyalarında iç sınıflarda takılıyor: önce jar'a topla
& jar cf "$out\classes.jar" -C "$out\classes" .; Check "jar"
& "$bt\d8.bat" --release --min-api 23 --lib $jar --output "$out\dex" "$out\classes.jar"; Check "d8"

# 3) dex'i ekle, hizala, imzala
Copy-Item "$out\base.apk" "$out\unsigned.apk"
Push-Location "$out\dex"
& "$bt\aapt.exe" add "$out\unsigned.apk" classes.dex | Out-Null; Check "aapt add"
Pop-Location
& "$bt\zipalign.exe" -f -p 4 "$out\unsigned.apk" "$out\aligned.apk"; Check "zipalign"

# imza anahtari: yoksa uretilir. Guncellemeler ayni anahtarla imzalanmali, bu dosyayi yedekle (git'e girmez).
$ks = "$here\kline-release.jks"
if (-not (Test-Path $ks)) {
    & keytool -genkeypair -keystore $ks -alias kline -keyalg RSA -keysize 2048 -validity 10000 `
        -storepass klineobd -keypass klineobd -dname "CN=K-Line OBD"
    Check "keytool"
}
& "$bt\apksigner.bat" sign --ks $ks --ks-pass pass:klineobd --key-pass pass:klineobd --out "$out\K-Line-OBD.apk" "$out\aligned.apk"
Check "apksigner sign"
& "$bt\apksigner.bat" verify "$out\K-Line-OBD.apk"; Check "apksigner verify"
Write-Host "APK hazir: $out\K-Line-OBD.apk"
