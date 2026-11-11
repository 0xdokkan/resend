# Downloads and verifies the official Slint C++ SDK for Windows (x64, MSVC)
# into .deps\, installs it there without asking, then prints the directory
# to pass to CMake as CMAKE_PREFIX_PATH. Messages go to stderr.
#
#   cmake -S . -B build -DCMAKE_PREFIX_PATH="$(powershell -File scripts/fetch-slint.ps1)"
$ErrorActionPreference = 'Stop'

$version = '1.18.1'
$installer = "Slint-cpp-$version-win64-MSVC-AMD64.exe"
# Digest published by GitHub for the release asset above.
$sha256 = 'eea71789a54982bc1c164d9e1d2c7f00fead1415e9ac396ccb48676f2f66a7a9'

$root = Split-Path -Parent $PSScriptRoot
$deps = Join-Path $root '.deps'
$sdk = Join-Path $deps "slint-$version-win64"

function Find-Config {
    if (-not (Test-Path $sdk)) { return $null }
    Get-ChildItem -Path $sdk -Recurse -Filter 'SlintConfig.cmake' -ErrorAction SilentlyContinue | Select-Object -First 1
}

if (-not (Find-Config)) {
    New-Item -ItemType Directory -Force -Path $deps | Out-Null
    $download = Join-Path $deps $installer
    if (-not (Test-Path $download) -or (Get-FileHash -Algorithm SHA256 $download).Hash -ne $sha256.ToUpper()) {
        [Console]::Error.WriteLine("Downloading official Slint C++ SDK $version into .deps\")
        Invoke-WebRequest -UseBasicParsing -OutFile $download `
            -Uri "https://github.com/slint-ui/slint/releases/download/v$version/$installer"
    }
    if ((Get-FileHash -Algorithm SHA256 $download).Hash -ne $sha256.ToUpper()) {
        Remove-Item -Force $download
        throw "Checksum mismatch for ${installer}: the download was discarded."
    }
    # An NSIS installer: /S installs silently, /D (last, unquoted) chooses the directory.
    $process = Start-Process -FilePath $download -ArgumentList '/S', "/D=$sdk" -Wait -PassThru
    if ($process.ExitCode -ne 0) { throw "The Slint installer failed with exit code $($process.ExitCode)." }
    if (-not (Find-Config)) { throw "The Slint installer did not install SlintConfig.cmake into $sdk." }
}

Write-Output $sdk
