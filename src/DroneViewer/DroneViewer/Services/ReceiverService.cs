using Plugin.BLE;
using Plugin.BLE.Abstractions.Contracts;
using Plugin.BLE.Abstractions.EventArgs;
using System.Diagnostics;
using System.Text;

namespace DroneViewer.Services;

public class ReceiverService
{
    private readonly IBluetoothLE _ble;
    private readonly IAdapter _adapter;
    private IDevice? _connectedDevice;
    private ICharacteristic? _telemetryCharacteristic;
    private ICharacteristic? _batteryCharacteristic;

    // Buffer für mehrteilige Nachrichten
    private readonly StringBuilder _messageBuffer = new();
    private readonly object _bufferLock = new();
    private System.Threading.Timer? _bufferTimeoutTimer;
    private readonly TimeSpan _bufferTimeout = TimeSpan.FromSeconds(20);

    // Reconnect-Logik
    private bool _isReconnecting = false;
    private bool _autoReconnectEnabled = true;
    private int _reconnectAttempts = 0;
    private const int MaxReconnectAttempts = 5;
    private System.Threading.Timer? _reconnectTimer;

    // Lifecycle-Status
    private bool _isPaused = false;

    // Custom Drohnen-Service UUID (aus deinem ESP32-Code)
    private static readonly Guid ServiceUuid = Guid.Parse("12345678-1234-1234-1234-1234567890ab");
    private static readonly Guid TelemetryCharacteristicUuid = Guid.Parse("12345678-1234-1234-1234-1234567890ac");
    private static readonly Guid BatteryServiceUuid = Guid.Parse("0000180f-0000-1000-8000-00805f9b34fb");
    private static readonly Guid BatteryCharacteristicUuid = Guid.Parse("00002a19-0000-1000-8000-00805f9b34fb");

    public event EventHandler<DroneData>? DroneDataReceived;
    public event EventHandler<OperatorData>? OperatorDataReceived;
    public event EventHandler<string>? StatusChanged;
    public event EventHandler<BatteryData>? BatteryDataReceived;

    public bool IsConnected => _connectedDevice?.State == Plugin.BLE.Abstractions.DeviceState.Connected;

    public ReceiverService()
    {
        _ble = CrossBluetoothLE.Current;
        _adapter = CrossBluetoothLE.Current.Adapter;
    }

    public async Task<List<IDevice>> ScanAsync(TimeSpan timeout)
    {
        if (!_ble.IsOn)
        {
            StatusChanged?.Invoke(this, "❌ Bluetooth ist ausgeschaltet");
            return new List<IDevice>();
        }

        StatusChanged?.Invoke(this, "🔍 Suche Geräte...");

        var devices = new List<IDevice>();
        void OnDeviceFound(object? sender, DeviceEventArgs e)
        {
            if (e.Device.Name?.Contains("DroneID") == true)

            {
                if (!devices.Any(d => d.Id == e.Device.Id))
                {
                    devices.Add(e.Device);
                    StatusChanged?.Invoke(this, $"✅ Gefunden: {e.Device.Name}");
                }
            }
        }

        _adapter.DeviceDiscovered += OnDeviceFound;

        try
        {
            // Starte Scan (ohne Service-Filter, um alle Geräte zu finden)
            await _adapter.StartScanningForDevicesAsync();

            // Warte die komplette Timeout-Zeit ab
            await Task.Delay(timeout);

            // Stoppe Scan
            await _adapter.StopScanningForDevicesAsync();
        }
        finally
        {
            _adapter.DeviceDiscovered -= OnDeviceFound;
        }

        if (devices.Count == 0)
        {
            StatusChanged?.Invoke(this, "⚠️ Keine Geräte gefunden");
        }
        else
        {
            StatusChanged?.Invoke(this, $"✅ {devices.Count} Gerät(e) gefunden");
        }

        return devices;
    }

    public async Task<bool> ConnectToDeviceAsync(IDevice device)
    {
        try
        {

            StatusChanged?.Invoke(this, $"⏳ Verbinde mit {device.Name}...");

            // Verbindung herstellen
            await _adapter.ConnectToDeviceAsync(device);
            _connectedDevice = device;

            // WICHTIG: Warte auf stabile Verbindung
            await Task.Delay(1000);

            _adapter.DeviceDisconnected += OnDeviceDisconnected;

            StatusChanged?.Invoke(this, "🔍 Suche Service...");

            // Service Discovery - NUR den Custom Service
            var service = await device.GetServiceAsync(ServiceUuid);
            if (service == null)
            {

                StatusChanged?.Invoke(this, $"❌ Service {ServiceUuid} nicht gefunden!");

                // Fallback: Alle Services auflisten für Debug
                await Task.Delay(500);
                var services = await device.GetServicesAsync();

                StatusChanged?.Invoke(this, $"📋 Verfügbare Services ({services.Count}):");
                foreach (var svc in services)
                {
                    StatusChanged?.Invoke(this, $"  • {svc.Id}");
                }

                return false;
            }

            StatusChanged?.Invoke(this, $"✅ Service gefunden!");

            // Warte zwischen Service und Characteristic Discovery
            await Task.Delay(300);

            // Characteristic finden
            _telemetryCharacteristic = await service.GetCharacteristicAsync(TelemetryCharacteristicUuid);
            if (_telemetryCharacteristic == null)
            {
                StatusChanged?.Invoke(this, $"❌ Characteristic {TelemetryCharacteristicUuid} nicht gefunden");

                return false;
            }


            StatusChanged?.Invoke(this, $"✅ Characteristic gefunden!");

            // Warte vor MTU-Request
            await Task.Delay(300);

            // MTU auf 247 setzen für bessere Datenübertragung
            try
            {
                StatusChanged?.Invoke(this, "📡 Setze MTU auf 247...");

                var mtuResult = await device.RequestMtuAsync(247);

                StatusChanged?.Invoke(this, $"✅ MTU gesetzt: {mtuResult}");

                Debug.WriteLine($"MTU set to: {mtuResult}");

                // Kurze Wartezeit nach MTU-Request
                await Task.Delay(200);
            }
            catch (Exception mtuEx)
            {
                // MTU-Fehler sind nicht kritisch, fortfahren

                StatusChanged?.Invoke(this, $"⚠️ MTU-Request fehlgeschlagen: {mtuEx.Message}");
                Debug.WriteLine($"MTU request failed: {mtuEx}");
            }

            // Notifications aktivieren
            _telemetryCharacteristic.ValueUpdated += OnTelemetryDataReceived;
            await _telemetryCharacteristic.StartUpdatesAsync();

            var battService = await device.GetServiceAsync(BatteryServiceUuid);
            if (battService == null)
                return false;


            _batteryCharacteristic = await battService.GetCharacteristicAsync(BatteryCharacteristicUuid);

            if (_batteryCharacteristic != null)
            {
                _batteryCharacteristic.ValueUpdated += OnBatteryDataReceived;
                await _batteryCharacteristic.StartUpdatesAsync();


                StatusChanged?.Invoke(this, "✅ Battery-Monitor aktiv");

            }

            // Finale Bestätigung
            await Task.Delay(200);

            StatusChanged?.Invoke(this, "✅ Verbunden! Warte auf Daten...");

            return true;
        }
        catch (Exception ex)
        {
            StatusChanged?.Invoke(this, $"❌ Fehler: {ex.Message}");
            Debug.WriteLine($"Connection error: {ex}");
            return false;
        }
    }

    private void OnBatteryDataReceived(object? sender, CharacteristicUpdatedEventArgs e)
    {
        try
        {
            var data = e.Characteristic.Value;

            // Battery Level ist ein einzelnes Byte (0-100%)
            if (data != null && data.Length > 0)
            {
                int batteryLevel = data[0]; // Erster Byte ist der Batterie-Level

                Debug.WriteLine($"Battery Level: {batteryLevel}%");

                var batteryData = new BatteryData
                {
                    BatteryLevel = batteryLevel,
                    Timestamp = DateTime.Now
                };

                // Event auf Main Thread auslösen
                BatteryDataReceived?.Invoke(this, batteryData);
            }
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"Battery data receive error: {ex}");
            StatusChanged?.Invoke(this, $"❌ Batterie-Fehler: {ex.Message}");
        }
    }


    private void OnTelemetryDataReceived(object? sender, CharacteristicUpdatedEventArgs e)
    {
        try
        {
            var data = e.Characteristic.Value;
            var chunk = Encoding.UTF8.GetString(data);

            string bufferContent;
            lock (_bufferLock)
            {
                _messageBuffer.Append(chunk);
                bufferContent = _messageBuffer.ToString();
            }

            // Prüfe auf Nachrichtenende: \0 oder \n
            // Sicherheit: Maximal 10 Nachrichten pro Empfang verarbeiten
            int processedCount = 0;
            const int maxIterations = 10;

            // Extrahiere alle vollständigen Nachrichten
            while (processedCount < maxIterations)
            {
                int endIndex = -1;

                // Suche nach Null-Terminator
                if (bufferContent.Contains('\0'))
                {
                    endIndex = bufferContent.IndexOf('\0');
                }

                if (endIndex >= 0)
                {
                    // Extrahiere Nachricht
                    var completeMessage = bufferContent.Substring(0, endIndex).Trim();

                    if (!string.IsNullOrWhiteSpace(completeMessage))
                    {

                        // Stoppe Timeout
                        _bufferTimeoutTimer?.Dispose();
                        _bufferTimeoutTimer = null;

                        // Parse Nachricht asynchron
                        Task.Run(() => ProcessCompleteMessage(completeMessage));
                    }

                    // Entferne verarbeitete Nachricht + Separator
                    bufferContent = endIndex + 1 < bufferContent.Length
                        ? bufferContent.Substring(endIndex + 1)
                        : string.Empty;

                    lock (_bufferLock) // ✅
                    {
                        _messageBuffer.Clear();
                        _messageBuffer.Append(bufferContent);
                    }

                    processedCount++;
                }
                else
                {
                    // Keine vollständige Nachricht mehr, warte auf mehr Daten
                    // Nur Status ausgeben wenn Buffer nicht leer ist
                    if (bufferContent.Length > 0)
                    {
                        // Starte Timeout-Timer
                        _bufferTimeoutTimer?.Dispose();
                        _bufferTimeoutTimer = new System.Threading.Timer(
                            OnBufferTimeout,
                            null,
                            _bufferTimeout,
                            System.Threading.Timeout.InfiniteTimeSpan);
                    }
                    break;
                }
            }
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"Receive error: {ex}");

            StatusChanged?.Invoke(this, $"❌ Empfangs-Fehler: {ex.Message}");
            ResetBuffer();
        }
    }

    private void OnBufferTimeout(object? state)
    {
        if (_messageBuffer.Length > 0)
        {
            StatusChanged?.Invoke(this, $"⚠️ Timeout! Buffer verworfen ({_messageBuffer.Length} chars)");
            ResetBuffer();
        }
    }

    private void ResetBuffer()
    {
        lock (_bufferLock) { _messageBuffer.Clear(); }
    }

    private void ProcessCompleteMessage(string json)
    {
        try
        {
            var message = MessageParser.Parse(json);
            if (message == null)
            {

                StatusChanged?.Invoke(this, "⚠️ Konnte JSON nicht parsen");
                return;
            }

            // Heartbeat
            if (message.Type == "heartbeat")
            {
                return;
            }

            // Drohnen-Position
            if (message.Type == "waypoint" && message.Role == "drone")
            {
                var droneData = MessageParser.ToDroneData(message);
                if (droneData != null)
                    DroneDataReceived?.Invoke(this, droneData); // ✅ Direkt feuern, kein Main-Thread-Wrap

                var operatorData = MessageParser.ToOperatorData(message);
                if (operatorData != null)
                    OperatorDataReceived?.Invoke(this, operatorData); // ✅ Direkt feuern

                return;

            }

        }
        catch (Exception ex)
        {
            Debug.WriteLine($"Parse error: {ex}");
            StatusChanged?.Invoke(this, $"❌ Parse-Fehler: {ex.Message}");
        }
    }

    public async Task DisconnectAsync()
    {
        try
        {
            // Deaktiviere Auto-Reconnect
            _autoReconnectEnabled = false;
            _reconnectTimer?.Dispose();
            _reconnectTimer = null;

            if (_connectedDevice != null)
            {
                // Cleanup Timer und Buffer
                _bufferTimeoutTimer?.Dispose();
                _bufferTimeoutTimer = null;
                ResetBuffer();

                // Notifications stoppen
                if (_telemetryCharacteristic != null)
                {
                    try
                    {
                        _telemetryCharacteristic.ValueUpdated -= OnTelemetryDataReceived;
                        await _telemetryCharacteristic.StopUpdatesAsync();
                    }
                    catch (Exception ex)
                    {
                        Debug.WriteLine($"Error stopping updates: {ex.Message}");
                    }
                }

                if (_batteryCharacteristic != null)
                {
                    try
                    {
                        _batteryCharacteristic.ValueUpdated -= OnBatteryDataReceived;
                        await _batteryCharacteristic.StopUpdatesAsync();
                    }
                    catch (Exception ex)
                    {
                        Debug.WriteLine($"Error stopping updates: {ex.Message}");
                    }
                }

                // Event Handler entfernen
                _adapter.DeviceDisconnected -= OnDeviceDisconnected;

                // Warte kurz
                await Task.Delay(200);

                // Disconnect
                await _adapter.DisconnectDeviceAsync(_connectedDevice);
                _connectedDevice = null;
                _telemetryCharacteristic = null;

                StatusChanged?.Invoke(this, "⛔ Getrennt");
            }
        }
        catch (Exception ex)
        {
            Debug.WriteLine($"Disconnect error: {ex}");
            StatusChanged?.Invoke(this, $"⚠️ Disconnect-Fehler: {ex.Message}");
        }
    }

    private void OnDeviceDisconnected(object? sender, DeviceEventArgs e)
    {
        var disconnectedDevice = e.Device;

        StatusChanged?.Invoke(this, "⚠️ Verbindung verloren!");

        _connectedDevice = null;
        _telemetryCharacteristic = null;

        // Versuche automatisch neu zu verbinden, wenn nicht pausiert
        if (_autoReconnectEnabled && !_isPaused && !_isReconnecting && disconnectedDevice != null)
        {
            TryReconnect(disconnectedDevice);
        }
    }

    private void TryReconnect(IDevice device)
    {
        if (_isReconnecting || _reconnectAttempts >= MaxReconnectAttempts)
            return;

        _isReconnecting = true;
        _reconnectAttempts++;

        StatusChanged?.Invoke(this, $"🔄 Verbindungsversuch {_reconnectAttempts}/{MaxReconnectAttempts}...");

        // Versuche nach 2 Sekunden neu zu verbinden
        _reconnectTimer = new System.Threading.Timer(async _ =>
        {
            try
            {
                if (device != null)
                {
                    var success = await ConnectToDeviceAsync(device);
                    if (success)
                    {
                        _reconnectAttempts = 0;
                        _isReconnecting = false;
                        StatusChanged?.Invoke(this, "✅ Wiederverbunden!");
                    }
                    else
                    {
                        _isReconnecting = false;
                        if (_reconnectAttempts < MaxReconnectAttempts)
                        {
                            TryReconnect(device);
                        }
                        else
                        {
                            StatusChanged?.Invoke(this, "❌ Verbindung fehlgeschlagen. Maximale Versuche erreicht.");
                        }
                    }
                }
            }
            catch (Exception ex)
            {
                Debug.WriteLine($"Reconnect error: {ex}");
                _isReconnecting = false;
            }
        }, null, TimeSpan.FromSeconds(2), Timeout.InfiniteTimeSpan);
    }

    // Lifecycle-Methoden für App-Hintergrund/Vordergrund
    public void Pause()
    {
        _isPaused = true;
        Debug.WriteLine("MeshtasticReceiverService: Paused");
    }

    public void Resume()
    {
        _isPaused = false;
        Debug.WriteLine("MeshtasticReceiverService: Resumed");

        // Prüfe ob Verbindung noch aktiv ist
        if (_connectedDevice == null && _autoReconnectEnabled)
        {
            _reconnectAttempts = 0; // Reset counter

            StatusChanged?.Invoke(this, "🔄 Prüfe Verbindung...");

        }
    }

    public IDevice? GetConnectedDevice()
    {
        return _connectedDevice;
    }
}
public class BatteryData
{
    public int BatteryLevel { get; set; } // 0-100 Prozent
    public DateTime Timestamp { get; set; } = DateTime.Now;
}

public class DroneData
{
    public uint NodeId { get; set; }
    public double Latitude { get; set; }
    public double Longitude { get; set; }
    public int Altitude { get; set; }
    public double Speed { get; set; } // in km/h oder m/s
    public double Heading { get; set; } // in Grad (0-360)
    public DateTime Timestamp { get; set; } = DateTime.Now;
    public DateTime LastUpdate { get; set; } = DateTime.Now;
    public string? DroneName { get; set; }
    public string? Source { get; set; }
    public string? Mac { get; set; }
}

public class OperatorData
{
    public uint NodeId { get; set; }
    public double Latitude { get; set; }
    public double Longitude { get; set; }
    public DateTime LastUpdate { get; set; } = DateTime.Now;
    public string? Source { get; set; }
    public string? Mac { get; set; }
}
