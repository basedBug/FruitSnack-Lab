# Verifies the LittleFS web payload before it is flashed to the device.
#
#   * every JS module parses (node --check)
#   * data/js is flat: no .js modules in subdirectories
#   * every asset referenced by index.html exists in data/
#   * no external network resources (the device may be offline)
#   * every import is a static, relative ./name.js that resolves in data/js
#   * every getElementById('x') in the JS has an matching id="x" in index.html
#   * the payload fits comfortably in the LittleFS partition
#
# Usage:  tools\check_webui.ps1

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

$dataDir = Join-Path $root 'data'
$jsDir = Join-Path $dataDir 'js'
$failures = 0

function Fail([string]$message) {
  Write-Host "  [FAIL] $message" -ForegroundColor Red
  $script:failures++
}
function Pass([string]$message) {
  Write-Host "  [PASS] $message" -ForegroundColor Cyan
}

Write-Host "Web UI verification" -ForegroundColor Cyan

# ---------------------------------------------------------------- syntax
$allJsFiles = @(Get-ChildItem -Path $dataDir -Recurse -Filter '*.js' -File -ErrorAction SilentlyContinue)
if (-not $allJsFiles) { Fail 'no JavaScript files found in data/' } else {
  foreach ($file in $allJsFiles) {
    & node --check $file.FullName 2>&1 | Out-Null
    if ($LASTEXITCODE -eq 0) { Pass "parses: $($file.Name)" } else { Fail "syntax error in $($file.FullName)" }
  }
}

# ---------------------------------------------------------------- flat module directory
# The browser modules are loaded as siblings; a nested module means the layout
# drifted and the relative import contract below can no longer be reasoned about.
$jsFiles = @()
if (-not (Test-Path $jsDir)) {
  Fail 'data/js not found'
} else {
  $jsFiles = @(Get-ChildItem -Path $jsDir -Filter '*.js' -File)
  foreach ($file in $allJsFiles) {
    if (-not [string]::Equals($file.DirectoryName, $jsDir, [System.StringComparison]::OrdinalIgnoreCase)) {
      Fail "JS module must live directly in data/js (flat layout): $($file.FullName)"
    }
  }
  if ($jsFiles.Count -gt 0) { Pass 'data/js is flat' } else { Fail 'no JavaScript modules found directly in data/js' }
}

# ---------------------------------------------------------------- referenced assets
$indexPath = Join-Path $dataDir 'index.html'
$html = $null
if (Test-Path $indexPath) {
  $html = Get-Content $indexPath -Raw
  $refs = [regex]::Matches($html, '(?:src|href)="([^"]+)"') | ForEach-Object { $_.Groups[1].Value } |
    Where-Object { $_ -notmatch '^(https?:)?//' -and $_ -notmatch '^data:' -and $_ -notmatch '^#' }
  foreach ($ref in $refs) {
    $clean = ($ref -split '\?')[0].TrimStart('/')
    $target = Join-Path $dataDir ($clean -replace '/', '\')
    if (Test-Path $target) { Pass "asset present: $ref" } else { Fail "missing asset: $ref" }
  }
} else {
  Fail 'data/index.html not found'
}

# ---------------------------------------------------------------- no external resources
# The dashboard must not depend on the internet. Local device addresses
# (http://<hostname>.local) are fine - they point at the ESP32 itself.
$offenders = @()
Get-ChildItem -Path $dataDir -Recurse -File -Include '*.html', '*.css', '*.js' -ErrorAction SilentlyContinue | ForEach-Object {
  $file = $_
  $text = Get-Content $file.FullName -Raw
  foreach ($match in [regex]::Matches($text, '(?:https?:)?//[A-Za-z0-9._-]+\.[A-Za-z]{2,}[^\s"''`)]*')) {
    $url = $match.Value
    if ($url -match '\.local\b|localhost\b|127\.0\.0\.1') { continue }  # the device itself
    $offenders += "$($file.Name): $url"
  }
}
if ($offenders.Count -gt 0) {
  foreach ($o in ($offenders | Select-Object -Unique)) { Fail "external resource reference -> $o" }
} else {
  Pass 'no external resource references'
}

# ---------------------------------------------------------------- module graph
# Only static `import ... from './name.js'` (and `export ... from`) is allowed:
# bare specifiers, absolute paths, ../ traversal and dynamic import() cannot be
# served by LittleFS as a flat sibling set.
foreach ($file in $jsFiles) {
  $text = Get-Content $file.FullName -Raw

  if ($text -match '\bimport\s*\(') {
    Fail "dynamic import in $($file.Name); use a static ./name.js import"
  }

  $specifiers = @()
  foreach ($match in [regex]::Matches($text, '\bimport\s+[''"]([^''"]+)[''"]')) {
    $specifiers += $match.Groups[1].Value
  }
  foreach ($match in [regex]::Matches($text, '\bfrom\s+[''"]([^''"]+)[''"]')) {
    $specifiers += $match.Groups[1].Value
  }

  foreach ($spec in $specifiers) {
    if ($spec -notmatch '^\./[A-Za-z0-9_.-]+\.js$') {
      Fail "import in $($file.Name) is not a static relative ./name.js: '$spec'"
      continue
    }
    $target = Join-Path $jsDir ($spec -replace '^\./', '')
    if (Test-Path -LiteralPath $target -PathType Leaf) {
      Pass "import resolves: $spec"
    } else {
      Fail "unresolved import: $spec (from $($file.Name))"
    }
  }
}

# ---------------------------------------------------------------- DOM ids
# A typo'd or deleted id makes getElementById return null and kills the module
# at runtime, which the firmware flash cannot detect.
if ($html) {
  $htmlIds = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::Ordinal)
  foreach ($match in [regex]::Matches($html, '\bid\s*=\s*(?:"([^"]*)"|''([^'']*)'')')) {
    $id = if ($match.Groups[1].Success) { $match.Groups[1].Value } else { $match.Groups[2].Value }
    if ($id) { [void]$htmlIds.Add($id) }
  }

  $missingIds = @()
  foreach ($file in $jsFiles) {
    $text = Get-Content $file.FullName -Raw
    foreach ($match in [regex]::Matches($text, 'getElementById\(\s*[''"]([^''"]+)[''"]\s*\)')) {
      $id = $match.Groups[1].Value
      if (-not $htmlIds.Contains($id)) {
        $missingIds += "$($file.Name): getElementById('$id')"
      }
    }
  }
  $missingIds = @($missingIds | Select-Object -Unique)
  if ($missingIds.Count -gt 0) {
    foreach ($entry in $missingIds) { Fail "DOM id not defined in index.html -> $entry" }
  } else {
    Pass 'all getElementById ids are defined in index.html'
  }
} else {
  Fail 'cannot cross-check DOM ids: data/index.html not found'
}

# ---------------------------------------------------------------- size
$total = (Get-ChildItem -Path $dataDir -Recurse -File | Measure-Object -Property Length -Sum).Sum
$partitionBytes = 0x1F0000
Write-Host ("  payload size: {0:N0} bytes (partition {1:N0} bytes)" -f $total, $partitionBytes)
if ($total -lt ($partitionBytes * 0.8)) { Pass 'payload fits in the LittleFS partition' } else { Fail 'payload too large for the partition' }

Write-Host ""
if ($failures -eq 0) {
  Write-Host "Web UI verification passed." -ForegroundColor Green
  exit 0
}
Write-Host "$failures web UI check(s) failed." -ForegroundColor Red
exit 1
