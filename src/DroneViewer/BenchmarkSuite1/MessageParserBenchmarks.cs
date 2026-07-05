using BenchmarkDotNet.Attributes;
using DroneViewer.Services;
using Microsoft.VSDiagnostics;

namespace DroneViewer.Benchmarks;
[CPUUsageDiagnoser]
public class MessageParserBenchmarks
{
    private readonly string _spaceSeparatedMessage = "BLE AA:BB:CC:DD:EE:FF DroneID123 476543210 85432100 150 270 45 -65 OperatorID 476540000 85430000 1";
    private readonly string _jsonMessage = "{\"type\":\"waypoint\",\"role\":\"drone\",\"source\":\"BLE\",\"mac\":\"AA:BB:CC:DD:EE:FF\",\"waypoint\":{\"id\":\"OperatorID\",\"drone\":\"DroneID123\",\"latitudeI\":476543210,\"longitudeI\":85432100,\"altitude\":150,\"speed\":45,\"heading\":270,\"rssi\":-65,\"base_latitudeI\":476540000,\"base_longitudeI\":85430000,\"base_valid\":1}}";

    [Benchmark]
    public MeshtasticMessage? ParseSpaceSeparated() => MessageParser.Parse(_spaceSeparatedMessage);

    [Benchmark]
    public MeshtasticMessage? ParseJson() => MessageParser.Parse(_jsonMessage);

    [Benchmark]
    public (DroneData? Drone, OperatorData? Operator) ParseAndConvertSpaceSeparated()
    {
        var message = MessageParser.Parse(_spaceSeparatedMessage);
        return message is null
            ? ((DroneData?)null, (OperatorData?)null)
            : (MessageParser.ToDroneData(message), MessageParser.ToOperatorData(message));
    }
}
