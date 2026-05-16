using Microsoft.Data.Sqlite;
using SQLitePCL;

namespace DroneViewer.Services
{
    /// <summary>
    /// Liest Tiles aus einer MBTiles-Datei ohne HTTP-Server
    /// </summary>
    public class MBTilesReader : IDisposable
    {
        private SqliteConnection? _connection;
        private bool _disposed;

        public async Task OpenAsync(string mbtilesPath)
        {
            if (!File.Exists(mbtilesPath))
                throw new FileNotFoundException("MBTiles file not found", mbtilesPath);

            Batteries_V2.Init();

            _connection = new SqliteConnection($"Data Source={mbtilesPath};Mode=ReadOnly");
            await _connection.OpenAsync();
        }

        /// <summary>
        /// Liest die Metadaten aus der MBTiles-Datei
        /// </summary>
        public async Task<Dictionary<string, string>> GetMetadataAsync()
        {
            var metadata = new Dictionary<string, string>();

            if (_connection == null)
                return metadata;

            try
            {
                using var command = _connection.CreateCommand();
                command.CommandText = "SELECT name, value FROM metadata";

                using var reader = await command.ExecuteReaderAsync();
                while (await reader.ReadAsync())
                {
                    var name = reader.GetString(0);
                    var value = reader.GetString(1);
                    metadata[name] = value;
                    System.Diagnostics.Debug.WriteLine($"MBTiles Metadata: {name} = {value}");
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error reading metadata: {ex.Message}");
            }

            return metadata;
        }

        /// <summary>
        /// Holt ein Tile als Base64-String für die Verwendung in JavaScript
        /// </summary>
        public async Task<string?> GetTileAsBase64Async(int z, int x, int y)
        {
            if (_connection == null)
                return null;

            try
            {

                // TMS zu Slippy Map Konvertierung (Y-Achse invertieren)
                var tmsY = (1 << z) - 1 - y;

                System.Diagnostics.Debug.WriteLine($"Tile z={z}, x={x}, y={y} (TMS y={tmsY}) requested");

                // Database-Query auf Background-Thread ausführen
                var tileData = await Task.Run(async () =>
                {
                    using var command = _connection.CreateCommand();
                    command.CommandText = "SELECT tile_data FROM tiles WHERE zoom_level = @z AND tile_column = @x AND tile_row = @y";
                    command.Parameters.AddWithValue("@z", z);
                    command.Parameters.AddWithValue("@x", x);
                    command.Parameters.AddWithValue("@y", tmsY);

                    var result = await command.ExecuteScalarAsync();
                    return result as byte[];
                });

                if (tileData != null)
                {
                    // Base64-Konvertierung auch auf Background-Thread
                    return await Task.Run(() =>
                    {
                        // Prüfe ob Tile gzip-komprimiert ist und dekomprimiere wenn nötig
                        var decompressedData = DecompressIfNeeded(tileData);

                        var contentType = GetContentType(decompressedData);
                        var base64 = Convert.ToBase64String(decompressedData);

                        System.Diagnostics.Debug.WriteLine($"Tile z={z}, x={x}, y={y}: original={tileData.Length} bytes, decompressed={decompressedData.Length} bytes, type={contentType}");

                        return $"data:{contentType};base64,{base64}";
                    });
                }
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error getting tile z={z}, x={x}, y={y}: {ex.Message}");
            }

            return null;
        }

        private static byte[] DecompressIfNeeded(byte[] data)
        {
            // Prüfe auf gzip Magic Number (0x1f 0x8b)
            if (data.Length >= 2 && data[0] == 0x1f && data[1] == 0x8b)
            {
                System.Diagnostics.Debug.WriteLine("Tile is gzip-compressed, decompressing...");
                try
                {
                    using var compressedStream = new MemoryStream(data);
                    using var gzipStream = new System.IO.Compression.GZipStream(compressedStream, System.IO.Compression.CompressionMode.Decompress);
                    using var decompressedStream = new MemoryStream();
                    gzipStream.CopyTo(decompressedStream);
                    return decompressedStream.ToArray();
                }
                catch (Exception ex)
                {
                    System.Diagnostics.Debug.WriteLine($"Error decompressing tile: {ex.Message}");
                    return data; // Gib komprimierte Daten zurück falls Dekompression fehlschlägt
                }
            }

            // Keine Kompression erkannt
            return data;
        }

        private static string GetContentType(byte[] data)
        {
            // PNG Magic Number
            if (data.Length >= 8 && 
                data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47)
            {
                return "image/png";
            }
            
            // JPEG Magic Number
            if (data.Length >= 2 && data[0] == 0xFF && data[1] == 0xD8)
            {
                return "image/jpeg";
            }
            
            // WebP Magic Number
            if (data.Length >= 12 && 
                data[0] == 0x52 && data[1] == 0x49 && data[2] == 0x46 && data[3] == 0x46)
            {
                return "image/webp";
            }
            
            // Protocol Buffer (Vector Tiles)
            return "application/x-protobuf";
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            _connection?.Close();
            _connection?.Dispose();
        }
    }
}
