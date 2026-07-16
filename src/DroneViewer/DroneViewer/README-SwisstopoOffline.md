# swisstopo Light Base Map vollständig offline

Dieses Paket erzeugt aus dem offiziellen swisstopo-Style eine lokale Variante und lädt die benötigten Sprites und Glyphen herunter.

## 1. Paket erzeugen

```powershell
Set-ExecutionPolicy -Scope Process Bypass
.\Setup-SwisstopoOffline.ps1 `
  -OutputDirectory ".\swisstopo-offline" `
  -TileUrl "http://127.0.0.1:8080/tiles/{z}/{x}/{y}.pbf" `
  -AssetBaseUrl "http://127.0.0.1:8080/map-assets"
```

Danach enthält `swisstopo-offline`:

```text
style.json
style.original.json
offline-manifest.json
sprites/
  sprite.json
  sprite.png
  sprite@2x.json
  sprite@2x.png
glyphs/
  <URL-codierter Fontstack>/
    0-255.pbf
    256-511.pbf
    ...
```

Das erzeugte `style.json` verwendet nur noch:

```text
http://127.0.0.1:8080/tiles/{z}/{x}/{y}.pbf
http://127.0.0.1:8080/map-assets/sprites/sprite
http://127.0.0.1:8080/map-assets/glyphs/{fontstack}/{range}.pbf
```

## 2. In die MAUI-App übernehmen

Kopiere den Inhalt nach:

```text
Resources/Raw/map-assets/
```

und ergänze in der Projektdatei bei Bedarf:

```xml
<ItemGroup>
  <MauiAsset Include="Resources\Raw\map-assets\**" LogicalName="map-assets/%(RecursiveDir)%(Filename)%(Extension)" />
</ItemGroup>
```

Falls dein lokaler HTTP-Server direkt aus dem App-Paket liest, müssen diese Routen verfügbar sein:

```text
GET /map-assets/style.json
GET /map-assets/sprites/sprite.json
GET /map-assets/sprites/sprite.png
GET /map-assets/sprites/sprite@2x.json
GET /map-assets/sprites/sprite@2x.png
GET /map-assets/glyphs/{fontstack}/{range}.pbf
GET /tiles/{z}/{x}/{y}.pbf
```

Wichtig: `{fontstack}` kommt URL-codiert an. Der Server muss den Pfad entweder unverändert gegen den ebenfalls URL-codierten Ordnernamen auflösen oder vor der Dateisuche konsistent dekodieren.

## 3. MapLibre laden

```javascript
const map = new maplibregl.Map({
    container: 'map',
    style: 'http://127.0.0.1:8080/map-assets/style.json',
    center: [9.53, 46.85],
    zoom: 12
});
```

## Relief

Die Light Base Map besteht offiziell aus `Base` und `Relief`. Das Skript entfernt standardmässig die Relief-Layer, weil dein vorhandenes MBTiles nur `ch.swisstopo.base.vt` enthält. Damit ist die Karte vollständig offline, aber ohne Reliefdarstellung.

Mit `-KeepOnlineRelief` bleibt das Relief online. Dann ist die Karte nicht vollständig offline.

Für vollständig offline **mit Relief** brauchst du zusätzlich das MBTiles von `ch.swisstopo.relief.vt` und eine zweite lokale Tile-Route. Danach muss auch die Relief-Quelle in `style.json` auf diese Route gesetzt werden.

## Glyphenbereiche

Standardmässig werden die für die Schweizer Landessprachen und häufige Kartenzeichen relevanten Unicode-Blöcke geladen. Für alle auf dem Server verfügbaren Unicode-Blöcke:

```powershell
.\Setup-SwisstopoOffline.ps1 -FullUnicode
```

Das verursacht deutlich mehr HTTP-Anfragen und benötigt mehr Speicherplatz.
