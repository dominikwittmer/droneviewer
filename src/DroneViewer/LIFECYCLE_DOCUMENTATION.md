# App Lifecycle & BLE Connection Management

## Was passiert wenn die App minimiert oder der Screen ausgeschaltet wird?

### ✅ **Implementierte Lösung**

#### 1. **BLE-Verbindung bleibt aktiv**
- **Android Foreground Service**: Läuft weiter, auch wenn App im Hintergrund
- **Notification**: Zeigt permanente Benachrichtigung "Drohnen-Verbindung aktiv"
- **BLE-Daten**: Werden weiter empfangen und verarbeitet
- **Reconnect**: Automatische Wiederverbindung bei Verbindungsverlust (max. 5 Versuche)

#### 2. **Timer Management**
- **Pausiert beim Minimieren**:
  - `_inactivityCheckTimer` (spart CPU/Batterie)
  - `_statusMessageTimer`

- **Reaktiviert beim Öffnen**:
  - Alle Timer werden neu gestartet
  - Inaktivitätsprüfung läuft weiter

#### 3. **State Preservation**
- **Drohnen-Daten**: Bleiben in `_activeDrones` Dictionary erhalten
- **Operator-Daten**: Bleiben in `_activeOperators` Dictionary erhalten
- **Mapping**: Drohne-zu-Operator Zuordnung bleibt erhalten
- **WebView**: Wird beim Wiederöffnen neu initialisiert, aber Marker werden aus gespeicherten Daten wiederhergestellt

#### 4. **Lifecycle Events**

```
App minimiert / Screen aus:
├─ App.OnAppDeactivated()
├─ MainPage.OnDisappearing()
│  ├─ MeshtasticReceiverService.Pause()
│  ├─ _inactivityCheckTimer disposed
│  └─ BLE Service läuft weiter (Foreground)

App wiederhergestellt:
├─ App.OnAppResumed()
├─ MainPage.OnAppearing()
│  ├─ MeshtasticReceiverService.Resume()
│  ├─ StartInactivityTimer()
│  └─ InitializeMapAsync() (Karte neu laden)
```

### 🔄 **Auto-Reconnect Mechanismus**

```csharp
Verbindung verloren:
├─ OnDeviceDisconnected Event
├─ TryReconnect() (bis zu 5 Versuche)
│  ├─ Wartet 2 Sekunden
│  ├─ ConnectToDeviceAsync()
│  └─ Bei Erfolg: Status "Wiederverbunden!"
└─ Bei 5 Fehlversuchen: "Maximale Versuche erreicht"
```

### 📱 **Android Foreground Service**

**Vorteile:**
- BLE-Verbindung wird nicht vom System getrennt
- Telemetrie wird im Hintergrund weiter empfangen
- Drohnen können getrackt werden, auch wenn App nicht sichtbar

**Sichtbare Änderungen:**
- Permanente Notification mit "Drohnen-Verbindung aktiv"
- Service läuft bis zur manuellen Trennung

### ⚡ **Batterie-Optimierung**

**Pausiert im Hintergrund:**
- Inaktivitäts-Timer (alle 30 Sek)
- WebView Rendering
- Status-Meldungen Timer

**Läuft weiter:**
- BLE-Verbindung (notwendig für Telemetrie)
- Datenempfang
- Drohnen-Tracking

### 🔧 **Konfiguration**

**Reconnect Settings:**
```csharp
MaxReconnectAttempts = 5;
ReconnectDelay = 2 Sekunden;
```

**Inaktivitäts-Thresholds:**
```csharp
InactivityWarningThreshold = 3 Minuten (Grau)
InactivityRemovalThreshold = 5 Minuten (Löschen)
```

### 🐛 **Debugging**

Alle Lifecycle-Events loggen in Debug Console:
```
MainPage: OnAppearing - App in Vordergrund
MainPage: OnDisappearing - App im Hintergrund
MeshtasticReceiverService: Paused
MeshtasticReceiverService: Resumed
BLE Foreground Service gestartet
BLE Foreground Service gestoppt
```

### ⚠️ **Bekannte Einschränkungen**

1. **Android Battery Optimization**: 
   - Bei aggressiver Batterie-Optimierung kann BLE trotzdem getrennt werden
   - Benutzer sollte App von Battery Optimization ausschließen

2. **iOS Background**: 
   - iOS hat strengere Background-Limits
   - BLE kann nach ~10 Min getrennt werden (iOS System-Limit)
   - Bei iOS muss Core Bluetooth Background Mode aktiviert sein

3. **WebView**: 
   - Wird bei jedem App-Start neu geladen
   - Marker werden aus C# State wiederhergestellt
   - Kann 1-2 Sekunden dauern

### 📋 **Testing Checkliste**

- [ ] App minimieren → Drohnen-Daten empfangen sich weiter
- [ ] Screen ausschalten → BLE bleibt verbunden
- [ ] Verbindung trennen → Auto-Reconnect funktioniert
- [ ] App lange im Hintergrund → State bleibt erhalten
- [ ] App force-kill → Foreground Service stoppt sauber
