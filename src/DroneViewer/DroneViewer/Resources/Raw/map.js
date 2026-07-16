let map = null;
let mapTilerKey = 'KEY';
let isOfflineMode = false;
let mapReady = false;

function applyOfflineStyle(style) {
    if (!map) {
        console.error('applyOfflineStyle: map is null');
        return;
    }

    map.setStyle(style, { diff: false });

    map.once('style.load', () => {
        map.jumpTo({
            center: [7.444237, 46.946508],
            zoom: 8
        });
        restoreCustomLayers();
        console.log('Offline style applied');
    });
}

async function setOfflineMode(styleJsonString) {
    try {
        let style;

        if (styleJsonString && styleJsonString.trim() !== '') {
            style = JSON.parse(styleJsonString);
        } else {
            const response = await fetch(STYLES.offline.url);
            if (!response.ok) {
                throw new Error(`Failed to load offline style: ${response.status}`);
            }

            style = await response.json();
        }

        applyOfflineStyle(style);
    } catch (error) {
        console.error('Error in setOfflineMode:', error);
    }
}

function setOfflineModeWithStyle(styleJsonString) {
    setOfflineMode(styleJsonString);
}

const STYLES = {
    swiss: {
        url: 'https://vectortiles.geo.admin.ch/styles/ch.swisstopo.basemap.vt/style.json',
        online: true
    },
    osm: {
        get url() {
            return `https://api.maptiler.com/maps/streets-v2/style.json?key=${mapTilerKey}`;
        },
        online: true
    },
    offline: {
        url: './swisstopo-style-offline.json',
        online: false
    }
};

const DRONE_COLORS = [
    '#FF4444', // Rot
    '#44FF44', // Grün
    '#4444FF', // Blau
    '#FFFF44', // Gelb
    '#FF44FF', // Magenta
    '#44FFFF', // Cyan
    '#FF8844', // Orange
    '#8844FF', // Violett
    '#FF6666', // Helles Rot
    '#66FF66', // Helles Grün
    '#6666FF', // Helles Blau
    '#FFDD44', // Gold
    '#FF44AA', // Rosa
    '#44FFAA', // Mintgrün
    '#AA44FF', // Lila
    '#FFAA44', // Pfirsich
    '#44AAFF', // Himmelblau
    '#FF5555', // Koralle
    '#55FF55', // Lindgrün
    '#5555FF'  // Königsblau
];

// Map Drohnen-ID zu Farbindex
let droneColorMap = {};

function getDroneColor(nodeId) {
    if (!droneColorMap[nodeId]) {
        droneColorMap[nodeId] = DRONE_COLORS[Object.keys(droneColorMap).length % DRONE_COLORS.length];
    }
    return droneColorMap[nodeId];
}

function setMapTilerKey(key) {
    if (key && key.trim() !== '') {
        mapTilerKey = key;
        console.log('MapTiler key updated');
    }
}

// Globaler Error Handler
window.addEventListener('error', function (event) {
    console.error('Global error caught:', event.message);
    console.error('  at:', event.filename + ':' + event.lineno + ':' + event.colno);
    console.error('  stack:', event.error ? event.error.stack : 'no stack');
});

window.addEventListener('unhandledrejection', function (event) {
    console.error('Unhandled promise rejection:', event.reason);
    console.error('  Promise:', event.promise);
    if (event.reason && event.reason.stack) {
        console.error('  Stack:', event.reason.stack);
    }
});

async function switchStyle(name) {
    if (!mapReady || !map) {
        console.warn('switchStyle called before map is ready, ignoring.');
        return;
    }

    const center = map.getCenter();
    const zoom = map.getZoom();
    const bearing = map.getBearing();
    const pitch = map.getPitch();

    map.setStyle(STYLES[name].url, { diff: false });

    map.once('style.load', () => {
        map.jumpTo({ center, zoom, bearing, pitch });
        restoreCustomLayers();
    });
}

function restoreCustomLayers() {
    if (!map) return;

    if (!map.getSource('drone-path')) {
        map.addSource('drone-path', {
            type: 'geojson',
            data: { type: 'Feature', geometry: { type: 'LineString', coordinates: [] } }
        });
        map.addLayer({
            id: 'drone-line',
            type: 'line',
            source: 'drone-path',
            paint: { 'line-color': '#ff0000', 'line-width': 3 }
        });
        console.log('drone-path layer restored after style switch');
    }
}

// Lade MapLibre GL dynamisch
async function loadMapLibre(useOffline) {
    console.log('loadMapLibre called with useOffline=' + useOffline);
    isOfflineMode = useOffline;

    // CSS laden
    const cssLink = document.getElementById('maplibre-css');
    if (useOffline) {
        cssLink.href = 'maplibre-gl.css';
    } else {
        cssLink.href = 'https://unpkg.com/maplibre-gl@latest/dist/maplibre-gl.css';
    }

    // JavaScript laden
    const script = document.createElement('script');
    if (useOffline) {
        script.src = 'maplibre-gl.js';
    } else {
        script.src = 'https://unpkg.com/maplibre-gl@latest/dist/maplibre-gl.js';
    }

    script.onload = function () {
        console.log('MapLibre GL loaded successfully');
        document.getElementById('loading').style.display = 'none';
        try {
            initializeMap();
        } catch (error) {
            console.error('Error initializing map:', error);
            document.getElementById('loading').innerText = 'Fehler beim Initialisieren: ' + error.message;
        }
    };

    script.onerror = function (error) {
        console.error('Error loading MapLibre GL script:', error);
        document.getElementById('loading').innerText = 'Fehler beim Laden der Karte';
    };

    document.head.appendChild(script);
    console.log('MapLibre script tag added to head');
}

function initializeMap() {

    try {
        console.log('Initializing map...');

        const initialStyle = isOfflineMode
            ? {
                version: 8,
                sources: {},
                layers: [
                    {
                        id: 'background',
                        type: 'background',
                        paint: {
                            'background-color': '#f8f8f8'
                        }
                    }
                ]
            }
            : STYLES.swiss.url;

        map = new maplibregl.Map({
            container: 'map',
            style: initialStyle,
            zoom: 10,
            renderWorldCopies: false
        });

        console.log('Map object created');


        // Flugbahn hinzufügen
        map.on('load', () => {
            console.log('Map loaded event fired');
            mapReady = true;

            // Benachrichtige MAUI/C# dass MapLibre bereit ist (ersetzt Polling-Loop)
            try { window.location.href = 'maploaded://ready'; } catch (e) {
                console.warn('maploaded notification failed:', e);
            }

            // Prüfe ob Source bereits existiert (kann bei Reload passieren)
            if (!map.getSource('drone-path')) {
                map.addSource('drone-path', {
                    type: 'geojson',
                    data: {
                        type: 'Feature',
                        geometry: {
                            type: 'LineString',
                            coordinates: []
                        }
                    }
                });

                map.addLayer({
                    id: 'drone-line',
                    type: 'line',
                    source: 'drone-path',
                    paint: {
                        'line-color': '#ff0000',
                        'line-width': 3
                    }
                });
                console.log('Drone path layer added');
            }
        });

        map.on('error', (e) => {
            console.error('=== MAP ERROR EVENT ===');
            console.error('Error type:', e.error ? e.error.constructor.name : 'unknown');
            console.error('Error message:', e.error ? e.error.message : (e.message || 'no message'));
            console.error('Error stack:', e.error ? e.error.stack : 'no stack');
            if (e.sourceId) console.error('Source ID:', e.sourceId);
            if (e.tile) console.error('Tile coords:', e.tile.tileID ? e.tile.tileID.canonical : 'unknown');

            // Versuche wichtige Properties zu loggen ohne zirkuläre Referenzen
            try {
                const safeProps = {};
                for (const key in e) {
                    if (e.hasOwnProperty(key) && typeof e[key] !== 'object') {
                        safeProps[key] = e[key];
                    }
                }
                console.error('Event properties:', JSON.stringify(safeProps));
            } catch (err) {
                console.error('Could not stringify event properties');
            }
            console.error('======================');
        });

        // Stelle sicher, dass inaktive Marker nach dem Scrollen ihre Styles behalten
        map.on('moveend', () => {
            restoreInactiveStyles();
        });

        // Auch während der Bewegung und nach jedem Render
        // map.on('move', () => {
        //     restoreInactiveStyles();
        // });

        // map.on('render', () => {
        //     restoreInactiveStyles();
        // });

        console.log('Map initialization complete');
    } catch (error) {
        console.error('Error in initializeMap:', error);
        throw error;
    }

}

// Drohnen Marker
let droneMarkers = {};
let operatorMarkers = {};
let selectedDroneId = null;

function removeAllMarkers() {

    // Drohnen entfernen
    for (const nodeId in droneMarkers) {
        droneMarkers[nodeId].marker.remove();
    }
    droneMarkers = {};
    // Operatoren entfernen
    for (const nodeId in operatorMarkers) {
        operatorMarkers[nodeId].marker.remove();
    }
    operatorMarkers = {};
}

// Stelle inaktive Styles nach Scroll wieder her
function restoreInactiveStyles() {
    // Drohnen
    for (const nodeId in droneMarkers) {
        const marker = droneMarkers[nodeId];
        if (marker.inactive) {
            marker.element.style.setProperty('opacity', '0.4', 'important');
            marker.element.style.setProperty('filter', 'grayscale(100%)', 'important');
        }
    }

    // Operatoren
    for (const nodeId in operatorMarkers) {
        const marker = operatorMarkers[nodeId];
        if (marker.inactive) {
            marker.element.style.setProperty('opacity', '0.4', 'important');
            marker.element.style.setProperty('filter', 'grayscale(100%)', 'important');
        }
    }
}

function updateDroneMarkers(drones) {
    if (!Array.isArray(drones)) return;
    for (const d of drones) {
        if (!d) continue;
        addDroneMarker(
            d.id,
            d.lon,
            d.lat,
            d.name ?? 'Drohne',
            d.alt ?? -999,
            d.spd ?? 0,
            d.hdg ?? -1,
            d.opId ?? 0
        );
    }
}

function updateOperatorMarkers(operators) {
    if (!Array.isArray(operators)) return;
    for (const o of operators) {
        if (!o) continue;
        addOperatorMarker(o.id, o.lon, o.lat, o.name ?? 'Operator');
    }
}

// Erstelle dynamisches SVG für Drohne basierend auf Farbe
function createDroneMarkerElement(nodeId, color) {
    const el = document.createElement('div');
    el.className = 'drone-marker';

    const svg = `
        <svg width="40" height="40" viewBox="0 0 40 40" xmlns="http://www.w3.org/2000/svg">
            <circle cx="20" cy="20" r="14" fill="${color}" stroke="white" stroke-width="2"/>
            <circle cx="20" cy="20" r="4" fill="white" stroke="white" stroke-width="1.2"/>
            <circle cx="12" cy="12" r="2.5" fill="white" stroke="white" stroke-width="0.8"/>
            <circle cx="28" cy="12" r="2.5" fill="white" stroke="white" stroke-width="0.8"/>
            <circle cx="12" cy="28" r="2.5" fill="white" stroke="white" stroke-width="0.8"/>
            <circle cx="28" cy="28" r="2.5" fill="white" stroke="white" stroke-width="0.8"/>
            <line x1="20" y1="20" x2="12" y2="12" stroke="white" stroke-width="1.2"/>
            <line x1="20" y1="20" x2="28" y2="12" stroke="white" stroke-width="1.2"/>
            <line x1="20" y1="20" x2="12" y2="28" stroke="white" stroke-width="1.2"/>
            <line x1="20" y1="20" x2="28" y2="28" stroke="white" stroke-width="1.2"/>
        </svg>
    `;

    el.innerHTML = svg;
    return el;
}

// Erstelle dynamisches SVG für Operator basierend auf Farbe
function createOperatorMarkerElement(nodeId, color) {
    const el = document.createElement('div');
    el.className = 'operator-marker';

    const svg = `
        <svg width="32" height="32" viewBox="0 0 32 32" xmlns="http://www.w3.org/2000/svg">
            <circle cx="16" cy="16" r="14" fill="${color}" stroke="white" stroke-width="2"/>
            <circle cx="16" cy="12" r="4" fill="white"/>
            <path d="M 10 18 C 10 18 10 16 16 16 C 22 16 22 18 22 18 L 22 24 L 10 24 Z" fill="white"/>
        </svg>
    `;

    el.innerHTML = svg;
    return el;
}

function addDroneMarker(nodeId, lon, lat, name, altitude, speed, heading, operatorId) {
    if (!map) return;

    const color = getDroneColor(nodeId);

    // Erstelle oder aktualisiere Marker
    if (!droneMarkers[nodeId]) {
        // Neuer Marker mit Custom Icon
        const el = createDroneMarkerElement(nodeId, color);
        el.title = name;

        // Click Handler
        el.addEventListener('click', function () {
            showDroneOverlay(nodeId);
        });

        const marker = new maplibregl.Marker({
            element: el,
            anchor: 'center',
        })
            .setLngLat([lon, lat])
            .addTo(map);

        droneMarkers[nodeId] = {
            marker: marker,
            element: el,
            operatorId: operatorId || 0,
            inactive: false,
            color: color,
            data: { name: name, lon: lon, lat: lat, altitude: altitude, speed: speed, heading: heading }
        };
    } else {
        // Aktualisiere Position und Daten
        droneMarkers[nodeId].marker.setLngLat([lon, lat]);
        droneMarkers[nodeId].data = { name: name, lon: lon, lat: lat, altitude: altitude, speed: speed, heading: heading };
        droneMarkers[nodeId].operatorId = operatorId || 0;

        // Stelle sicher, dass inaktive Styles erhalten bleiben
        if (droneMarkers[nodeId].inactive) {
            droneMarkers[nodeId].element.style.setProperty('opacity', '0.4', 'important');
            droneMarkers[nodeId].element.style.setProperty('filter', 'grayscale(100%)', 'important');
        }

        // Wenn dieser Marker im Overlay angezeigt wird, aktualisiere das Overlay sofort
        if (selectedDroneId === nodeId) {
            updateDroneOverlay(nodeId);
        }
    }

}

function setDroneInactive(nodeId, inactive) {
    if (!droneMarkers[nodeId]) return;

    const el = droneMarkers[nodeId].element;
    droneMarkers[nodeId].inactive = inactive;

    if (inactive) {
        el.style.setProperty('opacity', '0.4', 'important');
        el.style.setProperty('filter', 'grayscale(100%)', 'important');
    } else {
        el.style.setProperty('opacity', '1', 'important');
        el.style.setProperty('filter', 'none', 'important');
    }
}

function removeDroneMarker(nodeId) {
    if (!droneMarkers[nodeId]) return;

    droneMarkers[nodeId].marker.remove();
    delete droneMarkers[nodeId];

    // Schließe Overlay falls offen
    if (selectedDroneId === nodeId) {
        closeDroneOverlay();
    }
}

function addOperatorMarker(nodeId, lon, lat, name) {
    if (!map) return;

    // Finde die zugehörige Drohne, um ihre Farbe zu nutzen
    let color = '#4488ff'; // Fallback: Blau
    for (const droneId in droneMarkers) {
        if (droneMarkers[droneId].operatorId === nodeId) {
            color = droneMarkers[droneId].color;
            break;
        }
    }

    // Erstelle oder aktualisiere Marker
    if (!operatorMarkers[nodeId]) {
        // Neuer Marker mit Custom Icon
        const el = createOperatorMarkerElement(nodeId, color);
        el.title = name;

        const marker = new maplibregl.Marker({ element: el })
            .setLngLat([lon, lat])
            .addTo(map);

        operatorMarkers[nodeId] = {
            marker: marker,
            element: el,
            inactive: false,
            color: color,
            data: { name: name, lon: lon, lat: lat }
        };
    } else {
        // Aktualisiere Position und Daten
        operatorMarkers[nodeId].marker.setLngLat([lon, lat]);
        operatorMarkers[nodeId].data = { name: name, lon: lon, lat: lat };

        // Stelle sicher, dass inaktive Styles erhalten bleiben
        if (operatorMarkers[nodeId].inactive) {
            operatorMarkers[nodeId].element.style.setProperty('opacity', '0.4', 'important');
            operatorMarkers[nodeId].element.style.setProperty('filter', 'grayscale(100%)', 'important');
        }
    }

}

function removeOperatorMarker(nodeId) {
    if (!operatorMarkers[nodeId]) return;

    operatorMarkers[nodeId].marker.remove();
    delete operatorMarkers[nodeId];
}

function setOperatorInactive(nodeId, inactive) {
    if (!operatorMarkers[nodeId]) return;

    const el = operatorMarkers[nodeId].element;
    operatorMarkers[nodeId].inactive = inactive;

    if (inactive) {
        el.style.setProperty('opacity', '0.4', 'important');
        el.style.setProperty('filter', 'grayscale(100%)', 'important');
    } else {
        el.style.setProperty('opacity', '1', 'important');
        el.style.setProperty('filter', 'none', 'important');
    }
}

function showDroneOverlay(nodeId) {
    if (!droneMarkers[nodeId]) return;

    selectedDroneId = nodeId;

    const overlay = document.getElementById('drone-overlay');
    const data = droneMarkers[nodeId].data;
    document.getElementById('overlay-title').textContent = data.name;

    updateDroneOverlay(nodeId);

    overlay.classList.add('visible');
}

function updateDroneOverlay(nodeId) {
    if (!droneMarkers[nodeId]) return;

    const data = droneMarkers[nodeId].data;

    document.getElementById('overlay-position').textContent =
        data.lat.toFixed(6) + ', ' + data.lon.toFixed(6);
    document.getElementById('overlay-altitude').textContent =
        data.altitude < -100 ? 'N/A' : data.altitude + ' m';
    document.getElementById('overlay-speed').textContent =
        data.speed.toFixed(1) + ' km/h';
    document.getElementById('overlay-heading').textContent =
        data.heading < 0 ? 'N/A' : data.heading.toFixed(0) + '°';
}

function closeDroneOverlay() {
    const overlay = document.getElementById('drone-overlay');
    overlay.classList.remove('visible');
    selectedDroneId = null;
}

// Standort Marker
let userMarker = null;

function setUserLocation(lon, lat) {
    if (!map) return;

    // Marker setzen oder aktualisieren
    if (userMarker == null) {
        userMarker = new maplibregl.Marker({ color: "blue" })
            .setLngLat([lon, lat])
            .addTo(map);
    } else {
        userMarker.setLngLat([lon, lat]);
    }

    // Karte auf Standort zentrieren
    map.flyTo({
        center: [lon, lat],
        zoom: 15
    });
}