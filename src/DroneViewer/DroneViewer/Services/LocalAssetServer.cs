using System.Net;
using System.Net.Sockets;
using System.Text;

namespace DroneViewer.Services;

public interface ILocalAssetServer
{
    string BaseUrl { get; }
    Task StartAsync(CancellationToken cancellationToken = default);
    Task StopAsync(CancellationToken cancellationToken = default);
}

public sealed class LocalAssetServer : ILocalAssetServer, IAsyncDisposable
{
    private const int Port = 8080;
    private TcpListener? _listener;
    private CancellationTokenSource? _cts;
    private Task? _serverTask;

    public string BaseUrl => $"http://127.0.0.1:{Port}";

    public Task StartAsync(CancellationToken cancellationToken = default)
    {
        if (_serverTask != null)
        {
            return Task.CompletedTask;
        }

        _cts = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
        _listener = new TcpListener(IPAddress.Loopback, Port);
        _listener.Start();

        _serverTask = AcceptLoopAsync(_cts.Token);

        System.Diagnostics.Debug.WriteLine($"LocalAssetServer gestartet: {BaseUrl}");
        return Task.CompletedTask;
    }

    public async Task StopAsync(CancellationToken cancellationToken = default)
    {
        if (_cts == null)
        {
            return;
        }

        try
        {
            await _cts.CancelAsync();
        }
        catch
        {
        }

        try
        {
            _listener?.Stop();
        }
        catch
        {
        }

        if (_serverTask != null)
        {
            try
            {
                await _serverTask.WaitAsync(cancellationToken);
            }
            catch
            {
            }
        }

        _listener = null;
        _serverTask = null;

        _cts.Dispose();
        _cts = null;
    }

    private async Task AcceptLoopAsync(CancellationToken cancellationToken)
    {
        if (_listener == null)
        {
            return;
        }

        try
        {
            while (!cancellationToken.IsCancellationRequested)
            {
                var client = await _listener.AcceptTcpClientAsync(cancellationToken);
                _ = Task.Run(() => HandleClientAsync(client, cancellationToken), cancellationToken);
            }
        }
        catch (OperationCanceledException)
        {
        }
        catch (ObjectDisposedException)
        {
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"LocalAssetServer AcceptLoop Fehler: {ex}");
        }
    }

    private static async Task HandleClientAsync(TcpClient client, CancellationToken cancellationToken)
    {
        using var _ = client;
        using var networkStream = client.GetStream();
        using var reader = new StreamReader(networkStream, Encoding.ASCII, false, 8192, leaveOpen: true);

        try
        {
            var requestLine = await reader.ReadLineAsync(cancellationToken);
            if (string.IsNullOrWhiteSpace(requestLine))
            {
                return;
            }

            var parts = requestLine.Split(' ', StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 2)
            {
                await WriteResponseAsync(networkStream, 400, "text/plain", "Bad Request", cancellationToken);
                return;
            }

            var method = parts[0];
            var rawPath = parts[1];

            while (!string.IsNullOrEmpty(await reader.ReadLineAsync(cancellationToken)))
            {
            }

            if (!string.Equals(method, "GET", StringComparison.OrdinalIgnoreCase))
            {
                await WriteResponseAsync(networkStream, 405, "text/plain", "Method Not Allowed", cancellationToken);
                return;
            }

            var assetPath = NormalizeAssetPath(rawPath);
            if (assetPath == null)
            {
                await WriteResponseAsync(networkStream, 404, "text/plain", "Not Found", cancellationToken);
                return;
            }

            await using var assetStream = await FileSystem.OpenAppPackageFileAsync(assetPath);
            using var memoryStream = new MemoryStream();
            await assetStream.CopyToAsync(memoryStream, cancellationToken);

            var contentType = GetContentType(assetPath);
            await WriteBinaryResponseAsync(networkStream, 200, contentType, memoryStream.ToArray(), cancellationToken);
        }
        catch (FileNotFoundException)
        {
            await WriteResponseAsync(networkStream, 404, "text/plain", "Not Found", cancellationToken);
        }
        catch (Exception ex)
        {
            System.Diagnostics.Debug.WriteLine($"LocalAssetServer Request Fehler: {ex}");
            await WriteResponseAsync(networkStream, 500, "text/plain", "Internal Server Error", cancellationToken);
        }
    }

    private static string? NormalizeAssetPath(string rawPath)
    {
        var path = rawPath.Split('?', 2)[0];
        path = Uri.UnescapeDataString(path).TrimStart('/');

        if (!path.StartsWith("map-assets/", StringComparison.OrdinalIgnoreCase))
        {
            return null;
        }

        return path.Replace('\\', '/');
    }

    private static string GetContentType(string assetPath)
    {
        return Path.GetExtension(assetPath).ToLowerInvariant() switch
        {
            ".json" => "application/json",
            ".png" => "image/png",
            ".pbf" => "application/x-protobuf",
            ".txt" => "text/plain; charset=utf-8",
            ".css" => "text/css; charset=utf-8",
            ".js" => "application/javascript; charset=utf-8",
            ".html" => "text/html; charset=utf-8",
            _ => "application/octet-stream"
        };
    }

    private static Task WriteResponseAsync(Stream stream, int statusCode, string contentType, string body, CancellationToken cancellationToken)
    {
        var bytes = Encoding.UTF8.GetBytes(body);
        return WriteBinaryResponseAsync(stream, statusCode, contentType, bytes, cancellationToken);
    }

    private static async Task WriteBinaryResponseAsync(Stream stream, int statusCode, string contentType, byte[] body, CancellationToken cancellationToken)
    {
        var statusText = statusCode switch
        {
            200 => "OK",
            400 => "Bad Request",
            404 => "Not Found",
            405 => "Method Not Allowed",
            _ => "Internal Server Error"
        };

        var header =
            $"HTTP/1.1 {statusCode} {statusText}\r\n" +
            $"Content-Type: {contentType}\r\n" +
            $"Content-Length: {body.Length}\r\n" +
            $"Access-Control-Allow-Origin: *\r\n" +
            $"Connection: close\r\n\r\n";

        var headerBytes = Encoding.ASCII.GetBytes(header);

        await stream.WriteAsync(headerBytes, cancellationToken);
        await stream.WriteAsync(body, cancellationToken);
        await stream.FlushAsync(cancellationToken);
    }

    public async ValueTask DisposeAsync()
    {
        await StopAsync();
    }
}