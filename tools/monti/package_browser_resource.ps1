param(
  [string]$BrowserDir = "out\Release",
  [string]$Version = "1.0.0",
  [string]$OutDir = "resource-release",
  [string]$ArchiveName = "Scout-Web-win-x64.zip"
)

$ErrorActionPreference = "Stop"

function Copy-IfExists($Source, $Destination) {
  if (Test-Path -LiteralPath $Source) {
    Copy-Item -LiteralPath $Source -Destination $Destination -Recurse -Force
  }
}

$browserRoot = Resolve-Path -LiteralPath $BrowserDir
$exe = @(
  Join-Path $browserRoot "Monti Browser.exe"
  Join-Path $browserRoot "Monti.exe"
  Join-Path $browserRoot "chrome.exe"
) | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1

if (-not $exe) {
  throw "No browser executable found in $browserRoot. Expected Monti Browser.exe, Monti.exe, or chrome.exe."
}

$outRoot = Join-Path (Get-Location) $OutDir
$stage = Join-Path $outRoot "Monti Browser"
$archivePath = Join-Path $outRoot $ArchiveName
$manifestPath = Join-Path $outRoot "latest-win-x64.json"

Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $stage | Out-Null
New-Item -ItemType Directory -Force -Path $outRoot | Out-Null

$rootFilePatterns = @("*.exe", "*.dll", "*.pak", "*.dat", "*.bin", "*.json", "*.manifest", "*.ico", "*.png")
foreach ($pattern in $rootFilePatterns) {
  Get-ChildItem -LiteralPath $browserRoot -Filter $pattern -File -ErrorAction SilentlyContinue |
    ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $stage -Force }
}

$runtimeDirs = @(
  "locales",
  "Locales",
  "swiftshader",
  "MEIPreload",
  "WidevineCdm",
  "Dictionaries",
  "resources"
)
foreach ($dir in $runtimeDirs) {
  Copy-IfExists (Join-Path $browserRoot $dir) (Join-Path $stage $dir)
}

Remove-Item -LiteralPath $archivePath -Force -ErrorAction SilentlyContinue
Compress-Archive -LiteralPath $stage -DestinationPath $archivePath -Force

$sha512 = [Convert]::ToBase64String(
  [System.Security.Cryptography.SHA512]::Create().ComputeHash([System.IO.File]::ReadAllBytes($archivePath))
)
$size = (Get-Item -LiteralPath $archivePath).Length
$releaseDate = (Get-Date).ToUniversalTime().ToString("o")

$manifest = [ordered]@{
  version = $Version
  url = $ArchiveName
  sha512 = $sha512
  size = $size
  releaseDate = $releaseDate
}
$manifestJson = $manifest | ConvertTo-Json
[System.IO.File]::WriteAllText(
  $manifestPath,
  $manifestJson,
  (New-Object System.Text.UTF8Encoding($false))
)

Write-Host "Created $archivePath"
Write-Host "Created $manifestPath"
Write-Host "Upload keys:"
Write-Host "  resources/browser/$ArchiveName"
Write-Host "  resources/browser/latest-win-x64.json"
