using System.Text.Json;
using System.Text.Json.Serialization;

namespace DroneViewer.Services;

// Root-Nachricht
public class MeshtasticMessage
{
    [JsonPropertyName("type")]
    public string? Type { get; set; }

    [JsonPropertyName("role")]
    public string? Role { get; set; }

    [JsonPropertyName("source")]
    public string? Source { get; set; }

    [JsonPropertyName("mac")]
    public string? Mac { get; set; }

    [JsonPropertyName("waypoint")]
    public WaypointData? Waypoint { get; set; }
}

// Waypoint-Daten
public class WaypointData
{
    [JsonPropertyName("id")]
    public string? Id { get; set; }

    [JsonPropertyName("drone")]
    public string? Drone { get; set; }

    [JsonPropertyName("latitudeI")]
    public long LatitudeI { get; set; }

    [JsonPropertyName("longitudeI")]
    public long LongitudeI { get; set; }

    [JsonPropertyName("altitude")]
    public int Altitude { get; set; }

    [JsonPropertyName("speed")]
    public double Speed { get; set; }

    [JsonPropertyName("heading")]
    public double Heading { get; set; }

    [JsonPropertyName("rssi")]
    public int Rssi { get; set; }

    [JsonPropertyName("base_latitudeI")]
    public long BaseLatitudeI { get; set; }

    [JsonPropertyName("base_longitudeI")]
    public long BaseLongitudeI { get; set; }

    [JsonPropertyName("base_valid")]
    public int BaseValid { get; set; }

    // Konvertiere latitudeI/longitudeI zu Dezimalgrad
    public double Latitude => LatitudeI / 1e7;
    public double Longitude => LongitudeI / 1e7;

    public double BaseLatitude => BaseLatitudeI / 1e7;
    public double BaseLongitude => BaseLongitudeI / 1e7;
}

public class MessageParser
{
    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        PropertyNameCaseInsensitive = true,
        AllowTrailingCommas = true,
        ReadCommentHandling = JsonCommentHandling.Skip
    };

    /// <summary>
    /// Parse-Funktion mit Auto-Detection: JSON oder Space-separated Format
    /// </summary>
    public static MeshtasticMessage? Parse(string data)
    {
        if (string.IsNullOrWhiteSpace(data))
            return null;

        // Auto-detect: JSON beginnt mit '{', Space-separated nicht
        if (data.TrimStart().StartsWith("{"))
        {
            return ParseJson(data);
        }
        else
        {
            return ParseSpaceSeparated(data);
        }
    }

    /// <summary>
    /// Parse JSON Format (Legacy)
    /// </summary>
    private static MeshtasticMessage? ParseJson(string json)
    {
        try
        {
            return JsonSerializer.Deserialize<MeshtasticMessage>(json, JsonOptions);
        }
        catch (JsonException ex)
        {
            System.Diagnostics.Debug.WriteLine($"JSON Parse error: {ex.Message}");
            return null;
        }
    }

    /// <summary>
    /// Parse Space-separated Format vom ESP32
    /// Format: source mac uav_id lat_e7 lon_e7 altitude heading speed rssi op_id base_lat_e7 base_lon_e7 base_valid
    /// Beispiel: "BLE AA:BB:CC:DD:EE:FF DroneID123 476543210 85432100 150 270 45 -65 OperatorID 476540000 85430000 1"
    /// </summary>
    private static MeshtasticMessage? ParseSpaceSeparated(string data)
    {
        try
        {
            var parts = data.Split(' ', StringSplitOptions.RemoveEmptyEntries);

            // Mindestens 13 Felder erforderlich
            if (parts.Length < 13)
            {
                System.Diagnostics.Debug.WriteLine($"Space-separated parse error: Expected 13 fields, got {parts.Length}");
                return null;
            }

            var message = new MeshtasticMessage
            {
                Type = "waypoint",
                Role = "drone",
                Source = parts[0],      // source (z.B. "BLE")
                Mac = parts[1],         // mac (z.B. "AA:BB:CC:DD:EE:FF")
                Waypoint = new WaypointData
                {
                    Drone = parts[2],                           // uav_id oder mac
                    LatitudeI = long.Parse(parts[3]),          // lat_e7
                    LongitudeI = long.Parse(parts[4]),         // lon_e7
                    Altitude = int.Parse(parts[5]),            // altitude_msl
                    Heading = double.Parse(parts[6]),          // heading
                    Speed = double.Parse(parts[7]),            // speed
                    Rssi = int.Parse(parts[8]),                // rssi
                    Id = parts[9],                             // op_id oder mac
                    BaseLatitudeI = long.Parse(parts[10]),     // base_lat_e7
                    BaseLongitudeI = long.Parse(parts[11]),    // base_lon_e7
                    BaseValid = int.Parse(parts[12])           // base_valid (0 oder 1)
                }
            };

            System.Diagnostics.Debug.WriteLine($"Parsed: Drone={message.Waypoint.Drone}, " +
                $"Lat={message.Waypoint.Latitude:F6}, Lon={message.Waypoint.Longitude:F6}, " +
                $"Alt={message.Waypoint.Altitude}m, Speed={message.Waypoint.Speed}km/h, " +
                $"RSSI={message.Waypoint.Rssi}dBm");

            return message;
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"Space-separated parse error: {ex.Message}");
            System.Diagnostics.Debug.WriteLine($"Raw data: {data}");
            return null;
        }
    }

    public static MeshtasticMessage? Parse(byte[] data)
    {
        try
        {
            var text = System.Text.Encoding.UTF8.GetString(data);
            return Parse(text);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"Data parse error: {ex.Message}");
            return null;
        }
    }

    // Konvertiere Nachricht zu DroneData
    public static DroneData? ToDroneData(MeshtasticMessage message)
    {
        if (message.Waypoint == null)
            return null;

        return new DroneData
        {
            NodeId = GetNodeIdFromMac(message.Mac),
            Latitude = message.Waypoint.Latitude,
            Longitude = message.Waypoint.Longitude,
            Altitude = message.Waypoint.Altitude,
            Speed = message.Waypoint.Speed,
            Heading = message.Waypoint.Heading,
            Timestamp = DateTime.Now,
            LastUpdate = DateTime.Now,
            DroneName = message.Waypoint.Drone,
            Source = message.Source,
            Mac = message.Mac
        };
    }

    // Konvertiere Nachricht zu OperatorData
    public static OperatorData? ToOperatorData(MeshtasticMessage message)
    {
        if (message.Waypoint == null)
            return null;

        if (message.Waypoint.BaseValid == 0)
            return null;

        return new OperatorData
        {
            NodeId = GetNodeIdFromMac(message.Mac),
            Latitude = message.Waypoint.BaseLatitude,
            Longitude = message.Waypoint.BaseLongitude,
            LastUpdate = DateTime.Now,
            Source = message.Source,
            Mac = message.Mac
        };
    }

    public static uint GetNodeIdFromMac(string? mac)
    {
        if (string.IsNullOrEmpty(mac))
            return 0;

        try
        {
            // MAC im Format "AA:BB:CC:DD:EE:FF"
            var bytes = mac.Split(':')
                .Select(b => Convert.ToByte(b, 16))
                .ToArray();

            if (bytes.Length >= 4)
            {
                // Nimm die letzten 4 Bytes als Node-ID
                return BitConverter.ToUInt32(bytes.Skip(bytes.Length - 4).ToArray(), 0);
            }
        }
        catch
        {
            // Fallback: Hash des Strings
            return (uint)mac.GetHashCode();
        }

        return 0;
    }
}
