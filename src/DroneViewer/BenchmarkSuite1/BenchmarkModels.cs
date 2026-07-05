namespace DroneViewer.Services;

public class DroneData
{
    public uint NodeId { get; set; }
    public double Latitude { get; set; }
    public double Longitude { get; set; }
    public int Altitude { get; set; }
    public double Speed { get; set; }
    public double Heading { get; set; }
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
