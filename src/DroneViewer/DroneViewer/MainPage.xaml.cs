using DroneViewer.Services;
using Microsoft.Maui.Devices.Sensors;
using System.Collections.Generic;
using System.Text.Json;

namespace DroneViewer
{
    public partial class MainPage : ContentPage
    {
        private const string MbtilesFileName = "ch.swisstopo.base.vt.mbtiles";
        private const string OfflineModeKey = "OfflineMode";
        private const string MapFilePathKey = "MapFilePath";
        private const string KeepScreenOnKey = "KeepScreenOn";
        private MBTilesReader? _tileReader;
        private ReceiverService? _receiverService;

        // Tracking für Drohnen und Operatoren
        private readonly Dictionary<uint, DroneData> _activeDrones = new();
        private readonly Dictionary<uint, OperatorData> _activeOperators = new();
        private readonly Dictionary<uint, uint> _droneToOperatorMap = new(); // DroneNodeId -> OperatorNodeId
        private System.Threading.Timer? _inactivityCheckTimer;
        private System.Threading.Timer? _statusMessageTimer;

        // Update-Throttling für Map-Updates
        private DateTime _lastMapUpdate = DateTime.MinValue;
        private const int MAP_UPDATE_INTERVAL_MS = 100; // Max 10 updates/second
        private bool _mapUpdatePending = false;

        private static readonly TimeSpan InactivityWarningThreshold = TimeSpan.FromMinutes(3);
        private static readonly TimeSpan InactivityRemovalThreshold = TimeSpan.FromMinutes(5);

        public MainPage()
        {
            InitializeComponent();
            MapView.Navigating += MapView_Navigating;
            InitializeMeshtastic();
            StartInactivityTimer();

            // App-Lifecycle Events registrieren
            Microsoft.Maui.Controls.Application.Current!.RequestedThemeChanged += OnAppLifecycleChanged;
        }

        private void OnAppLifecycleChanged(object? sender, AppThemeChangedEventArgs e)
        {
            // Dieser Event wird nicht für Sleep/Resume verwendet
            // Wir nutzen OnAppearing/OnDisappearing stattdessen
        }

        private void StartInactivityTimer()
        {
            // Timer alle 30 Sekunden ausführen
            _inactivityCheckTimer = new System.Threading.Timer(
                CheckInactivity,
                null,
                TimeSpan.FromSeconds(30),
                TimeSpan.FromSeconds(30));
        }

        private void CheckInactivity(object? state)
        {
            var now = DateTime.Now;
            var dronesToRemove = new List<uint>();
            var dronesToGray = new List<uint>();
            var operatorsToRemove = new List<uint>();
            var operatorsToGray = new List<uint>();

            lock (_activeDrones)
            {
                foreach (var kvp in _activeDrones)
                {
                    var timeSinceUpdate = now - kvp.Value.LastUpdate;

                    if (timeSinceUpdate >= InactivityRemovalThreshold)
                    {
                        dronesToRemove.Add(kvp.Key);
                    }
                    else if (timeSinceUpdate >= InactivityWarningThreshold)
                    {
                        dronesToGray.Add(kvp.Key);
                    }
                }
            }

            lock (_activeOperators)
            {
                foreach (var kvp in _activeOperators)
                {
                    var timeSinceUpdate = now - kvp.Value.LastUpdate;

                    if (timeSinceUpdate >= InactivityRemovalThreshold)
                    {
                        operatorsToRemove.Add(kvp.Key);
                    }
                    else if (timeSinceUpdate >= InactivityWarningThreshold)
                    {
                        operatorsToGray.Add(kvp.Key);
                    }
                }
            }

            // Aktualisierungen auf Main Thread
            MainThread.BeginInvokeOnMainThread(async () =>
            {
                foreach (var nodeId in dronesToRemove)
                {
                    string droneName = "Drohne";
                    lock (_activeDrones)
                    {
                        if (_activeDrones.TryGetValue(nodeId, out var drone))
                        {
                            droneName = drone.DroneName ?? "Drohne";
                        }
                        _activeDrones.Remove(nodeId);
                        _droneToOperatorMap.Remove(nodeId);
                    }
                    await MapView.EvaluateJavaScriptAsync($"removeDroneMarker({nodeId})");

                    // Zeige Meldung wenn Drohne verloren wurde
                    ShowTemporaryStatus($"📡 Drohne verloren: {droneName}");
                }

                foreach (var nodeId in dronesToGray)
                {
                    await MapView.EvaluateJavaScriptAsync($"setDroneInactive({nodeId}, true)");
                }

                foreach (var nodeId in operatorsToRemove)
                {
                    lock (_activeOperators)
                    {
                        _activeOperators.Remove(nodeId);
                    }
                    await MapView.EvaluateJavaScriptAsync($"removeOperatorMarker({nodeId})");
                }

                foreach (var nodeId in operatorsToGray)
                {
                    await MapView.EvaluateJavaScriptAsync($"setOperatorInactive({nodeId}, true)");
                }

                // Aktualisiere Statusleiste nach allen Änderungen
                if (dronesToRemove.Count == 0 && dronesToGray.Count == 0)
                {
                    UpdateStatusLabel();
                }
            });
        }

        private void InitializeMeshtastic()
        {
            _receiverService = new ReceiverService();
            _receiverService.StatusChanged += OnMeshtasticStatus;
            _receiverService.DroneDataReceived += OnDroneDataReceived;
            _receiverService.OperatorDataReceived += OnOperatorDataReceived;
            _receiverService.BatteryDataReceived += OnBatteryDataReceived;
        }

        protected override async void OnAppearing()
        {
            base.OnAppearing();

            // Resume BLE Service
            _receiverService?.Resume();

            // Resume Inactivity Timer
            if (_inactivityCheckTimer == null)
            {
                StartInactivityTimer();
            }

            // Screen Wake Lock aktivieren (wenn Setting aktiv)
            var keepScreenOn = Preferences.Get(KeepScreenOnKey, true);
            KeepScreenOn(keepScreenOn);

            await InitializeMapAsync();
            UpdateStatusLabel();

            System.Diagnostics.Debug.WriteLine("MainPage: OnAppearing - App in Vordergrund");
        }

        protected override void OnDisappearing()
        {
            base.OnDisappearing();

            // Pause BLE Service (aber nicht trennen)
            _receiverService?.Pause();

            // Pause Inactivity Timer (spart Batterie)
            _inactivityCheckTimer?.Dispose();
            _inactivityCheckTimer = null;

            // Screen Wake Lock deaktivieren
            KeepScreenOn(false);

            _tileReader?.Dispose();
            _statusMessageTimer?.Dispose();

            System.Diagnostics.Debug.WriteLine("MainPage: OnDisappearing - App im Hintergrund");
        }

        private async Task InitializeMapAsync()
        {
            // Prüfe ob Offline-Modus aktiv ist
            Preferences.Remove(MapFilePathKey);
            var isOffline = Preferences.Get(OfflineModeKey, false);
            var mapFilePath = Preferences.Get(MapFilePathKey, Path.Combine(FileSystem.AppDataDirectory, "ch.swisstopo.base.vt.mbtiles"));

            MapView.Source = "map.html";

            // Warte bis HTML geladen ist
            await Task.Delay(1000);

            // JavaScript ausführen um MapLibre-Modus zu setzen
            await MapView.EvaluateJavaScriptAsync($"loadMapLibre({(isOffline ? "true" : "false")})");

            // Warte bis MapLibre geladen ist
            await Task.Delay(2000);

            if (isOffline && !string.IsNullOrEmpty(mapFilePath) && File.Exists(mapFilePath))
            {
                try
                {
                    // MBTiles Reader öffnen
                    _tileReader = new MBTilesReader();
                    await _tileReader.OpenAsync(mapFilePath);

                    // Metadaten auslesen und loggen
                    var metadata = await _tileReader.GetMetadataAsync();
                    if (metadata.TryGetValue("json", out var jsonMetadata))
                    {
                        System.Diagnostics.Debug.WriteLine($"MBTiles JSON Metadata: {jsonMetadata}");
                    }

                    // Lade Style JSON aus Resources
                    await LoadAndApplyOfflineStyleAsync();

                    System.Diagnostics.Debug.WriteLine($"MBTiles reader opened: {mapFilePath}");
                }
                catch (Exception ex)
                {
                    await DisplayAlertAsync("Fehler", $"Fehler beim Öffnen der MBTiles-Datei: {ex.Message}", "OK");
                }
            }

            OnLocationClicked(this, EventArgs.Empty);
        }


        private async void MapView_Navigating(object? sender, WebNavigatingEventArgs e)
        {
            // Tile-Anfragen von JavaScript abfangen
            if (e.Url.StartsWith("tile://"))
            {
                e.Cancel = true;

                try
                {
                    System.Diagnostics.Debug.WriteLine($"Tile request received: {e.Url}");

                    // Format: tile://requestId/z/x/y
                    var parts = e.Url.Replace("tile://", "").Split('/');
                    if (parts.Length == 4)
                    {
                        var requestId = parts[0];
                        var z = int.Parse(parts[1]);
                        var x = int.Parse(parts[2]);
                        var y = int.Parse(parts[3]);

                        System.Diagnostics.Debug.WriteLine($"Parsed tile request: ID={requestId}, z={z}, x={x}, y={y}");

                        if (_tileReader == null)
                        {
                            System.Diagnostics.Debug.WriteLine($"ERROR: _tileReader is null!");
                            await MapView.EvaluateJavaScriptAsync($"receiveTile({requestId}, null)");
                            return;
                        }

                        var tileData = await _tileReader.GetTileAsBase64Async(z, x, y);
                        var dataUrl = tileData ?? "null";

                        System.Diagnostics.Debug.WriteLine($"Tile data loaded for ID={requestId}, length={dataUrl.Length}");

                        // Tile-Daten an JavaScript zurücksenden
                        await MapView.EvaluateJavaScriptAsync($"receiveTile({requestId}, '{dataUrl}')");

                        System.Diagnostics.Debug.WriteLine($"Tile sent to JavaScript for ID={requestId}");
                    }
                    else
                    {
                        System.Diagnostics.Debug.WriteLine($"ERROR: Invalid tile URL format: {e.Url}");
                    }
                }
                catch (Exception ex)
                {
                    System.Diagnostics.Debug.WriteLine($"ERROR processing tile request: {ex.Message}");
                    System.Diagnostics.Debug.WriteLine($"Stack trace: {ex.StackTrace}");
                }
            }
        }

        // Meshtastic Event Handlers
        private void OnMeshtasticStatus(object? sender, string status)
        {
            MainThread.BeginInvokeOnMainThread(() =>
            {
                StatusLabel.Text = status;
            });
        }

        private void UpdateStatusLabel()
        {
            int droneCount;
            lock (_activeDrones)
            {
                droneCount = _activeDrones.Count;
            }

            MainThread.BeginInvokeOnMainThread(() =>
            {
                StatusLabel.Text = droneCount == 0
                    ? "⚡ Bereit - Keine Drohnen"
                    : $"✈️ {droneCount} Drohne{(droneCount == 1 ? "" : "n")} aktiv";
            });
        }

        private void ShowTemporaryStatus(string message, int durationSeconds = 3)
        {
            MainThread.BeginInvokeOnMainThread(() =>
            {
                StatusLabel.Text = message;

                // Timer zurücksetzen falls einer läuft
                _statusMessageTimer?.Dispose();

                // Nach Ablauf wieder Standardstatus anzeigen
                _statusMessageTimer = new System.Threading.Timer(
                    _ => UpdateStatusLabel(),
                    null,
                    TimeSpan.FromSeconds(durationSeconds),
                    Timeout.InfiniteTimeSpan);
            });
        }

        private void OnDroneDataReceived(object? sender, DroneData data)
        {
            bool isNewDrone = false;
            string droneName;

            lock (_activeDrones)
            {
                // Update oder füge Drohne hinzu
                if (_activeDrones.ContainsKey(data.NodeId))
                {
                    var existingDrone = _activeDrones[data.NodeId];
                    existingDrone.Latitude = data.Latitude;
                    existingDrone.Longitude = data.Longitude;
                    existingDrone.Altitude = data.Altitude;
                    existingDrone.Speed = data.Speed;
                    existingDrone.Heading = data.Heading;
                    existingDrone.LastUpdate = DateTime.Now;
                    existingDrone.DroneName = data.DroneName;
                }
                else
                {
                    data.LastUpdate = DateTime.Now;
                    _activeDrones[data.NodeId] = data;
                    isNewDrone = true;
                }

                droneName = data.DroneName ?? "Drohne";

                // Versuche Operator-Zuordnung aus OperatorMac
                if (!string.IsNullOrEmpty(data.Mac))
                {
                    var operatorNodeId = MessageParser.GetNodeIdFromMac(data.Mac);
                    _droneToOperatorMap[data.NodeId] = operatorNodeId;
                }
            }

            // Zeige Meldung wenn neue Drohne gefunden wurde (nicht throttled)
            if (isNewDrone)
            {
                MainThread.BeginInvokeOnMainThread(() =>
                {
                    ShowTemporaryStatus($"🔍 Drohne gefunden: {droneName}");

                    // Update Drohnen-Liste wenn sichtbar
                    if (DroneListOverlay.IsVisible)
                    {
                        UpdateDroneList();
                    }
                });
            }
            else
            {
                MainThread.BeginInvokeOnMainThread(() => 
                {
                    UpdateStatusLabel();

                    // Update Drohnen-Liste wenn sichtbar
                    if (DroneListOverlay.IsVisible)
                    {
                        UpdateDroneList();
                    }
                });
            }

            // Throttle Map-Updates
            ThrottleMapUpdate(data.NodeId, isNewDrone);
        }

        private void OnOperatorDataReceived(object? sender, OperatorData data)
        {
            lock (_activeOperators)
            {
                if (_activeOperators.ContainsKey(data.NodeId))
                {
                    var existingOperator = _activeOperators[data.NodeId];
                    existingOperator.Latitude = data.Latitude;
                    existingOperator.Longitude = data.Longitude;
                    existingOperator.LastUpdate = DateTime.Now;
                }
                else
                {
                    data.LastUpdate = DateTime.Now;
                    _activeOperators[data.NodeId] = data;
                }
            }

            // Throttle Map-Updates (operator updates sind seltener, können aber auch throttled werden)
            ThrottleMapUpdate(data.NodeId, false);
        }

        private void OnBatteryDataReceived(object? sender, BatteryData data)
        {
            MainThread.BeginInvokeOnMainThread(async () =>
            {
                BatteryLabel.Text = $"🔋 {data.BatteryLevel,3}%";
            });
        }

        private void ThrottleMapUpdate(uint nodeId, bool isNewDrone)
        {
            var now = DateTime.Now;
            var timeSinceLastUpdate = (now - _lastMapUpdate).TotalMilliseconds;

            // Bei neuen Drohnen sofort updaten, sonst throttlen
            if (isNewDrone || timeSinceLastUpdate >= MAP_UPDATE_INTERVAL_MS)
            {
                _lastMapUpdate = now;
                _mapUpdatePending = false;
                MainThread.BeginInvokeOnMainThread(async () => await UpdateMapAsync());
            }
            else if (!_mapUpdatePending)
            {
                // Schedule Update für später
                _mapUpdatePending = true;
                var delay = MAP_UPDATE_INTERVAL_MS - (int)timeSinceLastUpdate;
                Task.Delay(delay).ContinueWith(_ =>
                {
                    _lastMapUpdate = DateTime.Now;
                    _mapUpdatePending = false;
                    MainThread.BeginInvokeOnMainThread(async () => await UpdateMapAsync());
                });
            }
        }

        private async Task UpdateMapAsync()
        {
            List<DroneData> drones;
            List<OperatorData> operators;
            Dictionary<uint, uint> droneOperatorMap;

            // Schnell Daten kopieren mit Lock
            lock (_activeDrones)
            {
                drones = _activeDrones.Values.ToList();
            }

            lock (_activeOperators)
            {
                operators = _activeOperators.Values.ToList();
            }

            lock (_droneToOperatorMap)
            {
                droneOperatorMap = new Dictionary<uint, uint>(_droneToOperatorMap);
            }

            // Map-Updates außerhalb des Locks
            foreach (var data in drones)
            {
                var escapedName = (data.DroneName ?? "Drohne").Replace("'", "\\'");
                var operatorId = droneOperatorMap.ContainsKey(data.NodeId) ? droneOperatorMap[data.NodeId] : 0;

                await MapView.EvaluateJavaScriptAsync(
                    $"addDroneMarker({data.NodeId}, {data.Longitude.ToString(System.Globalization.CultureInfo.InvariantCulture)}, {data.Latitude.ToString(System.Globalization.CultureInfo.InvariantCulture)}, '{escapedName}', {data.Altitude}, {data.Speed.ToString(System.Globalization.CultureInfo.InvariantCulture)}, {data.Heading.ToString(System.Globalization.CultureInfo.InvariantCulture)}, {operatorId})");

                await MapView.EvaluateJavaScriptAsync($"setDroneInactive({data.NodeId}, false)");
            }

            foreach (var data in operators)
            {
                await MapView.EvaluateJavaScriptAsync(
                    $"addOperatorMarker({data.NodeId}, {data.Longitude.ToString(System.Globalization.CultureInfo.InvariantCulture)}, {data.Latitude.ToString(System.Globalization.CultureInfo.InvariantCulture)}, 'Operator')");

                await MapView.EvaluateJavaScriptAsync($"setOperatorInactive({data.NodeId}, false)");
            }
        }

        // Bluetooth Buttons
        private async void OnScanClicked(object sender, EventArgs e)
        {
            try
            {
                // Android 12+ benötigt Bluetooth-Berechtigungen
                var bluetoothStatus = await Permissions.CheckStatusAsync<Permissions.Bluetooth>();
                if (bluetoothStatus != PermissionStatus.Granted)
                {
                    bluetoothStatus = await Permissions.RequestAsync<Permissions.Bluetooth>();
                    if (bluetoothStatus != PermissionStatus.Granted)
                    {
                        await DisplayAlertAsync("Berechtigung erforderlich", "Bluetooth-Berechtigung wird benötigt.", "OK");
                        return;
                    }
                }

                // Android 12+ benötigt Location für Bluetooth-Scan
                if (DeviceInfo.Platform == DevicePlatform.Android)
                {
                    var locationStatus = await Permissions.CheckStatusAsync<Permissions.LocationWhenInUse>();
                    if (locationStatus != PermissionStatus.Granted)
                    {
                        locationStatus = await Permissions.RequestAsync<Permissions.LocationWhenInUse>();
                        if (locationStatus != PermissionStatus.Granted)
                        {
                            await DisplayAlertAsync("Berechtigung erforderlich", 
                                "Standort-Berechtigung wird für Bluetooth-Scan benötigt (Android-Anforderung).", "OK");
                            return;
                        }
                    }
                }

                var devices = await _receiverService!.ScanAsync(TimeSpan.FromSeconds(2));
                if (devices.Count == 0)
                {
                    await DisplayAlertAsync("Kein Gerät gefunden", "Kein Meshtastic-Empfänger gefunden.", "OK");
                    return;
                }

                var deviceNames = devices.Select(d => d.Name ?? "Unbekannt").ToArray();
                var selected = await DisplayActionSheetAsync("Empfänger auswählen", "Abbrechen", null, deviceNames);

                if (selected != null && selected != "Abbrechen")
                {
                    var device = devices[Array.IndexOf(deviceNames, selected)];
                    var connected = await _receiverService.ConnectToDeviceAsync(device);

                    if (connected)
                    {
                        // Starte Foreground Service auf Android
                        StartBleForegroundService();
                    }
                }
            }
            catch (Exception ex)
            {
                await DisplayAlertAsync("Fehler", $"Scan-Fehler: {ex.Message}", "OK");
            }
        }

        private async void OnDisconnectClicked(object sender, EventArgs e)
        {
            if (_receiverService != null)
            {
                await _receiverService.DisconnectAsync();

                // Stoppe Foreground Service
                StopBleForegroundService();
            }
        }



        private async void OnLocationClicked(object sender, EventArgs e)
        {
            try
            {
                // Check and request location permissions
                var status = await CheckAndRequestLocationPermission();
                if (status != PermissionStatus.Granted)
                {
                    await DisplayAlertAsync("Berechtigung erforderlich", 
                        "Standortberechtigung wird benötigt, um Ihre Position anzuzeigen.", 
                        "OK");
                    return;
                }

                // Get current location
                var location = await GetCurrentLocation();
                if (location != null)
                {
                    // Send location to JavaScript
                    await MapView.EvaluateJavaScriptAsync(
                        $"setUserLocation({location.Longitude.ToString(System.Globalization.CultureInfo.InvariantCulture)}, {location.Latitude.ToString(System.Globalization.CultureInfo.InvariantCulture)})");
                }
                else
                {
                    await DisplayAlertAsync("Fehler", "Standort konnte nicht ermittelt werden.", "OK");
                }
            }
            catch (Exception ex)
            {
                await DisplayAlertAsync("Fehler", $"Fehler beim Abrufen des Standorts: {ex.Message}", "OK");
            }
        }

        private async Task<PermissionStatus> CheckAndRequestLocationPermission()
        {
            var status = await Permissions.CheckStatusAsync<Permissions.LocationWhenInUse>();

            if (status == PermissionStatus.Granted)
                return status;

            if (status == PermissionStatus.Denied && DeviceInfo.Platform == DevicePlatform.iOS)
            {
                // Prompt the user to turn on in settings
                // On iOS once a permission has been denied it may not be requested again from the application
                return status;
            }

            status = await Permissions.RequestAsync<Permissions.LocationWhenInUse>();
            return status;
        }

        private async Task<Location?> GetCurrentLocation()
        {
            try
            {
                var request = new GeolocationRequest(GeolocationAccuracy.Best, TimeSpan.FromSeconds(10));
                var location = await Geolocation.Default.GetLocationAsync(request);
                return location;
            }
            catch (FeatureNotSupportedException)
            {
                // Handle not supported on device exception
                await DisplayAlertAsync("Nicht unterstützt", "Geolocation wird auf diesem Gerät nicht unterstützt.", "OK");
            }
            catch (FeatureNotEnabledException)
            {
                // Handle not enabled on device exception
                await DisplayAlertAsync("Nicht aktiviert", "Bitte aktivieren Sie die Standortdienste in den Geräteeinstellungen.", "OK");
            }
            catch (PermissionException)
            {
                // Handle permission exception
                await DisplayAlertAsync("Berechtigung verweigert", "Standortberechtigung wurde verweigert.", "OK");
            }
            catch (Exception)
            {
                // Unable to get location
            }

            return null;
        }

        private async void OnSettingsClicked(object sender, EventArgs e)
        {
            // Navigate to settings page
            await Shell.Current.GoToAsync("settings");
        }

        // Foreground Service für BLE (Android)
        private void StartBleForegroundService()
        {
#if ANDROID
            try
            {
                var intent = new Android.Content.Intent(Platform.CurrentActivity, typeof(Platforms.Android.BleConnectionService));

                if (Android.OS.Build.VERSION.SdkInt >= Android.OS.BuildVersionCodes.O)
                {
                    Platform.CurrentActivity?.StartForegroundService(intent);
                }
                else
                {
                    Platform.CurrentActivity?.StartService(intent);
                }

                System.Diagnostics.Debug.WriteLine("BLE Foreground Service gestartet");
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Fehler beim Starten des Foreground Service: {ex.Message}");
            }
#endif
        }

        private void StopBleForegroundService()
        {
#if ANDROID
            try
            {
                var intent = new Android.Content.Intent(Platform.CurrentActivity, typeof(Platforms.Android.BleConnectionService));
                Platform.CurrentActivity?.StopService(intent);

                System.Diagnostics.Debug.WriteLine("BLE Foreground Service gestoppt");
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Fehler beim Stoppen des Foreground Service: {ex.Message}");
            }
#endif
        }

        private async Task LoadAndApplyOfflineStyleAsync()
        {
            try
            {
                System.Diagnostics.Debug.WriteLine("Loading offline style JSON...");

                // Style JSON aus Resources laden
                using var stream = await FileSystem.OpenAppPackageFileAsync("swisstopo-style-offline.json");
                using var reader = new StreamReader(stream);
                string styleJson = await reader.ReadToEndAsync();

                System.Diagnostics.Debug.WriteLine($"Style JSON loaded, length: {styleJson.Length}");

                // JSON für JavaScript escapen (muss als String übergeben werden)
                string escapedJson = System.Text.Json.JsonSerializer.Serialize(styleJson);

                System.Diagnostics.Debug.WriteLine("Calling setOfflineModeWithStyle...");

                // JavaScript-Funktion aufrufen
                await MapView.EvaluateJavaScriptAsync($"setOfflineModeWithStyle({escapedJson});");

                System.Diagnostics.Debug.WriteLine("Offline mode with custom style applied successfully");
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error loading offline style: {ex.Message}");
                System.Diagnostics.Debug.WriteLine($"Stack trace: {ex.StackTrace}");

                // Fallback auf alte Methode wenn Style nicht geladen werden kann
                await MapView.EvaluateJavaScriptAsync("setOfflineMode()");
            }
        }

        // Drohnen-Liste Event Handlers
        private void OnDroneListClicked(object sender, EventArgs e)
        {
            UpdateDroneList();
            DroneListOverlay.IsVisible = true;
        }

        private void OnCloseDroneListClicked(object sender, EventArgs e)
        {
            DroneListOverlay.IsVisible = false;
        }

        private async void OnLayerClicked(object sender, TappedEventArgs e)
        {
            if (LayerMenu.IsVisible)
            {
                await LayerMenu.FadeTo(0, 120);

                LayerMenu.IsVisible = false;
            }
            else
            {
                LayerMenu.Opacity = 0;

                LayerMenu.IsVisible = true;

                await LayerMenu.FadeTo(1, 120);
            }
        }

        private void OnOpenMapClicked(object sender, TappedEventArgs e)
        {
            LayerMenu.IsVisible = false;

            // OpenMap aktivieren
        }

        private void OnSwissTopoClicked(object sender, TappedEventArgs e)
        {
            LayerMenu.IsVisible = false;

            // SwissTopo aktivieren
        }

        private void UpdateDroneList()
        {
            DroneListContainer.Children.Clear();

            List<DroneData> drones;
            lock (_activeDrones)
            {
                drones = _activeDrones.Values.OrderBy(d => d.DroneName).ToList();
            }

            DroneCountLabel.Text = $"{drones.Count} Drohne{(drones.Count == 1 ? "" : "n")} aktiv";

            if (drones.Count == 0)
            {
                var emptyLabel = new Label
                {
                    Text = "Keine aktiven Drohnen",
                    TextColor = Colors.Gray,
                    FontSize = 14,
                    HorizontalOptions = LayoutOptions.Center,
                    Margin = new Thickness(0, 40, 0, 0)
                };
                DroneListContainer.Children.Add(emptyLabel);
                return;
            }

            foreach (var drone in drones)
            {
                var droneFrame = CreateDroneListItem(drone);
                DroneListContainer.Children.Add(droneFrame);
            }
        }

        private Frame CreateDroneListItem(DroneData drone)
        {
            var timeSinceUpdate = DateTime.Now - drone.LastUpdate;
            var isInactive = timeSinceUpdate >= InactivityWarningThreshold;

            var frame = new Frame
            {
                BackgroundColor = isInactive ? Color.FromArgb("#FFF3E0") : Color.FromArgb("#F5F5F5"),
                BorderColor = isInactive ? Color.FromArgb("#FF9800") : Color.FromArgb("#E0E0E0"),
                CornerRadius = 8,
                Padding = 12,
                Margin = new Thickness(0, 4),
                HasShadow = false
            };

            var grid = new Grid
            {
                ColumnDefinitions =
                {
                    new ColumnDefinition { Width = GridLength.Auto },
                    new ColumnDefinition { Width = GridLength.Star },
                    new ColumnDefinition { Width = GridLength.Auto }
                },
                RowDefinitions =
                {
                    new RowDefinition { Height = GridLength.Auto },
                    new RowDefinition { Height = GridLength.Auto }
                }
            };

            // Drohnen-Icon
            var icon = new Label
            {
                Text = isInactive ? "✈" : "✈",
                FontSize = 24,
                TextColor = isInactive ? Color.FromArgb("#FF9800") : Color.FromArgb("#2196F3"),
                VerticalOptions = LayoutOptions.Center,
                HorizontalOptions = LayoutOptions.Center
            };
            Grid.SetColumn(icon, 0);
            Grid.SetRowSpan(icon, 2);
            grid.Children.Add(icon);

            // Drohnen-Name
            var nameLabel = new Label
            {
                Text = drone.DroneName ?? "Unbekannt",
                FontSize = 16,
                FontAttributes = FontAttributes.Bold,
                TextColor = Colors.Black,
                Margin = new Thickness(12, 0, 0, 0)
            };
            Grid.SetColumn(nameLabel, 1);
            Grid.SetRow(nameLabel, 0);
            grid.Children.Add(nameLabel);

            // Drohnen-Info (Position, Höhe, Speed)
            var infoLabel = new Label
            {
                Text = $"Alt: {drone.Altitude}m • {drone.Speed:F1} km/h • {drone.Heading:F0}°",
                FontSize = 12,
                TextColor = Colors.Gray,
                Margin = new Thickness(12, 2, 0, 0)
            };
            Grid.SetColumn(infoLabel, 1);
            Grid.SetRow(infoLabel, 1);
            grid.Children.Add(infoLabel);

            // Status Badge
            var statusLabel = new Label
            {
                Text = isInactive ? "⚠" : "✓",
                FontSize = 20,
                TextColor = isInactive ? Color.FromArgb("#FF9800") : Color.FromArgb("#4CAF50"),
                VerticalOptions = LayoutOptions.Center,
                HorizontalOptions = LayoutOptions.Center
            };
            Grid.SetColumn(statusLabel, 2);
            Grid.SetRowSpan(statusLabel, 2);
            grid.Children.Add(statusLabel);

            frame.Content = grid;

            // Tap Gesture: Drohne auf Karte fokussieren
            var tapGesture = new TapGestureRecognizer();
            tapGesture.Tapped += async (s, e) =>
            {
                DroneListOverlay.IsVisible = false;
                await MapView.EvaluateJavaScriptAsync($"showDroneOverlay({drone.NodeId})");
                await MapView.EvaluateJavaScriptAsync(
                    $"map.flyTo({{center: [{drone.Longitude.ToString(System.Globalization.CultureInfo.InvariantCulture)}, {drone.Latitude.ToString(System.Globalization.CultureInfo.InvariantCulture)}], zoom: 16}})");
            };
            frame.GestureRecognizers.Add(tapGesture);

            return frame;
        }

        // Screen Wake Lock Management
        private void KeepScreenOn(bool keepOn)
        {
#if ANDROID
            if (Platform.CurrentActivity?.Window != null)
            {
                if (keepOn)
                {
                    Platform.CurrentActivity.Window.AddFlags(Android.Views.WindowManagerFlags.KeepScreenOn);
                    System.Diagnostics.Debug.WriteLine("✅ Screen Wake Lock aktiviert");
                }
                else
                {
                    Platform.CurrentActivity.Window.ClearFlags(Android.Views.WindowManagerFlags.KeepScreenOn);
                    System.Diagnostics.Debug.WriteLine("❌ Screen Wake Lock deaktiviert");
                }
            }
#elif IOS || MACCATALYST
            UIKit.UIApplication.SharedApplication.IdleTimerDisabled = keepOn;
            System.Diagnostics.Debug.WriteLine($"iOS Idle Timer disabled: {keepOn}");
#elif WINDOWS
            // Windows: Display Request
            if (keepOn)
            {
                var displayRequest = new Windows.System.Display.DisplayRequest();
                displayRequest.RequestActive();
            }
#endif
        }

        // Öffentliche Methode zum Aktualisieren von Settings
        public void UpdateKeepScreenOn(bool keepOn)
        {
            KeepScreenOn(keepOn);
        }

    }
}
