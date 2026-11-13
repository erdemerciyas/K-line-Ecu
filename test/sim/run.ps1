# Ekran simülatörü: LVGL + ui_*.c sayfaları PC'de çizilir, sahneler test\sim\out\*.png olur.
# Derleyici ESP-IDF ile gelen esp-clang (x86-64 hedefi); C kütüphanesi Windows'un msvcrt.dll'i (host testleriyle aynı yol).
# LVGL ayarları cihazın build\config\sdkconfig.h dosyasından gelir: önce bir kez idf.py build gerekir.
#   pwsh -File test\sim\run.ps1              tüm sahneler
#   pwsh -File test\sim\run.ps1 -Scene drive adında "drive" geçen sahneler
#   pwsh -File test\sim\run.ps1 -Verbose     + uygulama logları
[CmdletBinding()]
param([string]$Scene = '', [string]$EspressifRoot = $(if ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH } else { 'C:\Espressif' }))
$ErrorActionPreference = 'Stop'

$clangDir = Get-ChildItem (Join-Path $EspressifRoot 'tools\esp-clang') -Directory | Sort-Object Name -Descending |
    Select-Object -First 1
if (-not $clangDir) { throw "esp-clang bulunamadı: $EspressifRoot\tools\esp-clang" }
$bin = Join-Path $clangDir.FullName 'esp-clang\bin'
$here = $PSScriptRoot
$root = (Resolve-Path (Join-Path $here '..\..')).Path
$main = Join-Path $root 'main'
$lvgl = Join-Path $root 'components\lvgl'
$out = Join-Path $here 'build'
$png = Join-Path $here 'out'
if (-not (Test-Path (Join-Path $root 'build\config\sdkconfig.h'))) { throw 'build\config\sdkconfig.h yok: önce idf.py build' }
New-Item -ItemType Directory -Force $out, "$out\lvgl", $png | Out-Null

foreach ($dll in 'msvcrt', 'kernel32') {
    & "$bin\llvm-dlltool.exe" -m i386:x86-64 -d "$here\$dll.def" -l "$out\$dll.lib"
    if ($LASTEXITCODE) { throw "llvm-dlltool $dll" }
}

$base = @('--target=x86_64-pc-windows-msvc', '-std=gnu17', '-O2', '-g0', '-ffreestanding', '-nostdlibinc',
    '-mno-stack-arg-probe', '-fno-stack-protector', '-fno-math-errno', '-DESP_PLATFORM', "-I$here\stubs", "-I$main", "-I$lvgl")

# LVGL: değişmeyen kaynak, nesneler saklanır (sadece kaynak daha yeniyse derlenir)
$skip = '[\\/]extra[\\/]libs[\\/]|[\\/]draw[\\/](nxp|sdl|stm32_dma2d|swm341_dma2d|arm2d)[\\/]|lv_tlsf\.c$|[\\/]gpu[\\/]'
$lvSrc = Get-ChildItem "$lvgl\src" -Recurse -Filter *.c | Where-Object { $_.FullName -notmatch $skip }
$lvObjs = [System.Collections.Concurrent.ConcurrentBag[string]]::new()
$fail = [System.Collections.Concurrent.ConcurrentBag[string]]::new()
$lvSrc | ForEach-Object -ThrottleLimit 12 -Parallel {
    $lv = $using:lvgl; $o = $using:out; $b = $using:bin
    $rel = $_.FullName.Substring($lv.Length + 1) -replace '[\\/]', '_'
    $obj = Join-Path $o 'lvgl' ($rel -replace '\.c$', '.obj')
    if (-not (Test-Path $obj) -or (Get-Item $obj).LastWriteTime -lt $_.LastWriteTime) {
        $flags = $using:base
        $msg = & (Join-Path $b "clang.exe") @flags -w -c $_.FullName -o $obj 2>&1 | Out-String
        if ($LASTEXITCODE) { ($using:fail).Add($msg) }
    }
    ($using:lvObjs).Add($obj)
}
if ($fail.Count) { $fail | Write-Host; throw "LVGL derleme hatası" }

# uygulama: firmware ile aynı dosya listesi (main\CMakeLists.txt), her seferinde derlenir
$cm = Get-Content "$main\CMakeLists.txt" -Raw
$appSrc = [regex]::Matches($cm, '"((ui_|settings)[a-z_]*\.c)"') | ForEach-Object { "$main\$($_.Groups[1].Value)" } | Sort-Object -Unique
$appSrc = @($appSrc) + "$here\sim_main.c"
$appObjs = [System.Collections.Concurrent.ConcurrentBag[string]]::new()
$warn = [System.Collections.Concurrent.ConcurrentBag[string]]::new()
$appSrc | ForEach-Object -ThrottleLimit 12 -Parallel {
    $o = $using:out; $b = $using:bin
    $obj = Join-Path $o ([IO.Path]::GetFileNameWithoutExtension($_) + '.obj')
    $flags = $using:base
    $msg = & (Join-Path $b "clang.exe") @flags -Wall -Wno-unused-parameter -Wno-unused-function -c $_ -o $obj 2>&1 | Out-String
    if ($LASTEXITCODE) { ($using:fail).Add($msg) } elseif ($msg.Trim()) { ($using:warn).Add($msg) }
    ($using:appObjs).Add($obj)
}
$warn | Write-Host
if ($fail.Count) { $fail | Write-Host; throw "derleme hatası" }

$rsp = Join-Path $out 'link.rsp'
(@($lvObjs) + @($appObjs) | ForEach-Object { "`"$_`"" }) -join "`n" | Set-Content $rsp -Encoding ascii
& "$bin\lld-link.exe" -nologo -entry:sim_entry -subsystem:console -nodefaultlib -stack:0x800000,0x800000 "@$rsp" `
    "$out\msvcrt.lib" "$out\kernel32.lib" "-out:$out\sim.exe"
if ($LASTEXITCODE) { throw 'bağlama hatası' }

[Console]::OutputEncoding = [Text.Encoding]::UTF8
$simArgs = @($png)
if ($Scene) { $simArgs += $Scene } elseif ($PSBoundParameters['Verbose']) { $simArgs += '*' }
if ($PSBoundParameters['Verbose']) { $simArgs += '-v' }
& "$out\sim.exe" @simArgs
exit $LASTEXITCODE
