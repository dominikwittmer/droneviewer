[CmdletBinding()]
param(
    [string]$OutputDirectory = ".\swisstopo-offline",
    [string]$TileUrl = "http://127.0.0.1:8080/tiles/{z}/{x}/{y}.pbf",
    [string]$AssetBaseUrl = "http://127.0.0.1:8080/map-assets",
    [switch]$KeepOnlineRelief,
    [switch]$FullUnicode
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$StyleUrl = 'https://vectortiles.geo.admin.ch/styles/ch.swisstopo.lightbasemap.vt/style.json'
$Out = [IO.Path]::GetFullPath($OutputDirectory)
$SpriteDir = Join-Path $Out 'sprites'
$GlyphDir = Join-Path $Out 'glyphs'

New-Item -ItemType Directory -Force -Path $Out, $SpriteDir, $GlyphDir | Out-Null

function Download-RequiredFile {
    param([string]$Uri, [string]$Destination)
    Write-Host "Lade $Uri"
    Invoke-WebRequest -Uri $Uri -OutFile $Destination -UseBasicParsing
}

function Download-OptionalFile {
    param([string]$Uri, [string]$Destination)
    try {
        Invoke-WebRequest -Uri $Uri -OutFile $Destination -UseBasicParsing
        return $true
    }
    catch {
        if (Test-Path $Destination) { Remove-Item $Destination -Force }
        return $false
    }
}

# 1. Offizielles Style laden
$OriginalStyleFile = Join-Path $Out 'style.original.json'
Download-RequiredFile $StyleUrl $OriginalStyleFile
$style = Get-Content $OriginalStyleFile -Raw | ConvertFrom-Json -Depth 100

# 2. Sprite-Dateien laden
if (-not $style.sprite) {
    throw 'Das offizielle Style enthält keine Sprite-URL.'
}
$spriteBase = [string]$style.sprite
foreach ($suffix in @('.json', '.png', '@2x.json', '@2x.png')) {
    Download-RequiredFile ($spriteBase + $suffix) (Join-Path $SpriteDir ("sprite" + $suffix))
}
$style.sprite = "$AssetBaseUrl/sprites/sprite"

# 3. Fontstacks aus allen Symbol-Layern ermitteln
$fontStacks = [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal)
foreach ($layer in $style.layers) {
    if ($null -ne $layer.layout -and $null -ne $layer.layout.'text-font') {
        $fontValue = $layer.layout.'text-font'
        # MapLibre bildet ein Array zu einem komma-separierten Fontstack zusammen.
        if ($fontValue -is [Array]) {
            [void]$fontStacks.Add(($fontValue -join ','))
        }
        elseif ($fontValue -is [string]) {
            [void]$fontStacks.Add($fontValue)
        }
    }
}

if ($fontStacks.Count -eq 0) {
    Write-Warning 'Keine text-font-Einträge gefunden. Es werden keine Glyphen heruntergeladen.'
}

# Standard: Lateinisch + erweiterte lateinische Zeichen + Satzzeichen/Symbole.
# FullUnicode: testet alle 256er-Blöcke; nicht vorhandene Blöcke werden übersprungen.
if ($FullUnicode) {
    $rangeStarts = 0..255 | ForEach-Object { $_ * 256 }
}
else {
    $rangeStarts = @(0, 256, 512, 7680, 8192, 8448, 9728, 9984, 64256)
}

$glyphTemplate = [string]$style.glyphs
if (-not $glyphTemplate) {
    throw 'Das offizielle Style enthält keine Glyph-URL.'
}

foreach ($fontStack in $fontStacks) {
    $encodedStack = [Uri]::EscapeDataString($fontStack)
    $fontFolder = Join-Path $GlyphDir $encodedStack
    New-Item -ItemType Directory -Force -Path $fontFolder | Out-Null

    Write-Host "`nGlyphen für: $fontStack"
    foreach ($start in $rangeStarts) {
        $end = $start + 255
        $range = "$start-$end"
        $uri = $glyphTemplate.Replace('{fontstack}', $encodedStack).Replace('{range}', $range)
        $dest = Join-Path $fontFolder "$range.pbf"
        if (Download-OptionalFile $uri $dest) {
            Write-Host "  OK $range"
        }
    }
}
$style.glyphs = "$AssetBaseUrl/glyphs/{fontstack}/{range}.pbf"

# 4. Lokale Base-Quelle einsetzen
$baseSourceName = $null
foreach ($candidate in @('base_v1.0.0', 'base')) {
    if ($style.sources.PSObject.Properties.Name -contains $candidate) {
        $baseSourceName = $candidate
        break
    }
}
if (-not $baseSourceName) {
    $baseSourceName = ($style.sources.PSObject.Properties | Where-Object {
        $_.Value.type -eq 'vector' -and (([string]$_.Value.url) -match 'ch\.swisstopo\.base\.vt')
    } | Select-Object -First 1).Name
}
if (-not $baseSourceName) {
    throw 'Die Quelle ch.swisstopo.base.vt wurde im Style nicht gefunden.'
}

$localSource = [ordered]@{
    type = 'vector'
    tiles = @($TileUrl)
    minzoom = 0
    maxzoom = 18
}
$style.sources.$baseSourceName = $localSource

# 5. Relief wahlweise online behalten oder vollständig entfernen
if (-not $KeepOnlineRelief) {
    $reliefNames = @($style.sources.PSObject.Properties | Where-Object {
        $_.Name -match 'relief' -or ([string]$_.Value.url) -match 'ch\.swisstopo\.relief\.vt'
    } | ForEach-Object Name)

    if ($reliefNames.Count -gt 0) {
        $style.layers = @($style.layers | Where-Object { $reliefNames -notcontains $_.source })
        foreach ($name in $reliefNames) {
            $style.sources.PSObject.Properties.Remove($name)
        }
    }
}

# 6. Vollständig lokales Style speichern
$OfflineStyleFile = Join-Path $Out 'style.json'
$style | ConvertTo-Json -Depth 100 | Set-Content -Path $OfflineStyleFile -Encoding UTF8

# Kleine Manifest-Datei für Kontrolle/Deployment
$manifest = [ordered]@{
    generatedAt = (Get-Date).ToString('o')
    sourceStyle = $StyleUrl
    tileUrl = $TileUrl
    assetBaseUrl = $AssetBaseUrl
    baseSource = $baseSourceName
    fontStacks = @($fontStacks)
    reliefOnline = [bool]$KeepOnlineRelief
    fullUnicode = [bool]$FullUnicode
}
$manifest | ConvertTo-Json -Depth 10 | Set-Content (Join-Path $Out 'offline-manifest.json') -Encoding UTF8

Write-Host "`nFertig: $Out" -ForegroundColor Green
Write-Host "Style: $OfflineStyleFile"
Write-Host 'Kopiere den gesamten Ordner in deine MAUI-App und stelle ihn unter /map-assets bereit.'
