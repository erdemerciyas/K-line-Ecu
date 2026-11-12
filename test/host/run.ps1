# obd.c host testleri: ESP-IDF ile gelen esp-clang (x86-64 hedefi) ve lld-link ile PC'de derlenip çalışır.
# MSVC / MinGW / Windows SDK gerekmez; C kütüphanesi Windows'un kendi msvcrt.dll'idir.
#   pwsh -File test\host\run.ps1            testler
#   pwsh -File test\host\run.ps1 -Verbose   + simülasyon logları (K-Line baytları, init adımları)
[CmdletBinding()]
param([string]$EspressifRoot = $(if ($env:IDF_TOOLS_PATH) { $env:IDF_TOOLS_PATH } else { 'C:\Espressif' }))
$ErrorActionPreference = 'Stop'

$clangDir = Get-ChildItem (Join-Path $EspressifRoot 'tools\esp-clang') -Directory | Sort-Object Name -Descending |
    Select-Object -First 1
if (-not $clangDir) { throw "esp-clang bulunamadı: $EspressifRoot\tools\esp-clang" }
$bin = Join-Path $clangDir.FullName 'esp-clang\bin'
$here = $PSScriptRoot
$main = Join-Path $here '..\..\main'
$out = Join-Path $here 'build'
New-Item -ItemType Directory -Force $out | Out-Null

foreach ($dll in 'msvcrt', 'kernel32') {
    & "$bin\llvm-dlltool.exe" -m i386:x86-64 -d "$here\$dll.def" -l "$out\$dll.lib"
    if ($LASTEXITCODE) { throw "llvm-dlltool $dll" }
}

$cflags = @('--target=x86_64-pc-windows-msvc', '-std=gnu17', '-O1', '-g0', '-ffreestanding', '-nostdlibinc',
    '-mno-stack-arg-probe', '-fno-stack-protector', '-fno-math-errno', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
    "-I$here\stubs", "-I$main")
if ($PSBoundParameters['Verbose']) { $cflags += '-DSIM_VERBOSE' }

$objs = @()
foreach ($src in @("$here\test_obd.c", "$here\sim.c", "$main\obd_pid.c", "$main\imu_math.c")) {
    $obj = Join-Path $out ([IO.Path]::GetFileNameWithoutExtension($src) + '.obj')
    & "$bin\clang.exe" @cflags -c $src -o $obj
    if ($LASTEXITCODE) { throw "derleme hatası: $src" }
    $objs += $obj
}
# yığın baştan ayrılır: -mno-stack-arg-probe ile büyük çerçeveler koruma sayfasını atlamasın
& "$bin\lld-link.exe" -nologo -entry:test_entry -subsystem:console -nodefaultlib -stack:0x100000,0x100000 @objs `
    "$out\msvcrt.lib" "$out\kernel32.lib" "-out:$out\test_obd.exe"
if ($LASTEXITCODE) { throw 'bağlama hatası' }

[Console]::OutputEncoding = [Text.Encoding]::UTF8
& "$out\test_obd.exe"
exit $LASTEXITCODE
