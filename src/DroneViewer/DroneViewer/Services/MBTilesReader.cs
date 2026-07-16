using Microsoft.Data.Sqlite;
using SQLitePCL;
using System.IO.Compression;

namespace DroneViewer.Services
{
    public class MBTilesReader : IDisposable
    {
        private SqliteConnection? _connection;
        private SqliteCommand? _tileCommand;
        private bool _disposed;

        public async Task OpenAsync(string mbtilesPath)
        {
            if (!File.Exists(mbtilesPath))
                throw new FileNotFoundException("MBTiles file not found", mbtilesPath);

            Batteries_V2.Init();

            // Shared cache + WAL für schnellere Lesezugriffe
            _connection = new SqliteConnection(
                $"Data Source={mbtilesPath};Mode=ReadOnly");
            await _connection.OpenAsync();

            // SQLite-Pragmas für bessere Read-Performance
            using var pragma = _connection.CreateCommand();
            pragma.CommandText = """
                PRAGMA cache_size=-8192;
                PRAGMA temp_store=MEMORY;
                PRAGMA mmap_size=268435456;
                """;
            await pragma.ExecuteNonQueryAsync();

            // Prepared Statement einmalig kompilieren
            _tileCommand = _connection.CreateCommand();
            _tileCommand.CommandText =
                "SELECT tile_data FROM tiles WHERE zoom_level=@z AND tile_column=@x AND tile_row=@y";
            _tileCommand.Parameters.Add("@z", SqliteType.Integer);
            _tileCommand.Parameters.Add("@x", SqliteType.Integer);
            _tileCommand.Parameters.Add("@y", SqliteType.Integer);
            _tileCommand.Prepare();
        }

        public async Task<Dictionary<string, string>> GetMetadataAsync()
        {
            var metadata = new Dictionary<string, string>();
            if (_connection == null) return metadata;

            try
            {
                using var command = _connection.CreateCommand();
                command.CommandText = "SELECT name, value FROM metadata";
                using var reader = await command.ExecuteReaderAsync();
                while (await reader.ReadAsync())
                    metadata[reader.GetString(0)] = reader.GetString(1);
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error reading metadata: {ex.Message}");
            }

            return metadata;
        }

        public async Task<string?> GetTileAsBase64Async(int z, int x, int y)
        {
            if (_connection == null || _tileCommand == null)
                return null;

            try
            {
                var tmsY = (1 << z) - 1 - y;

                // Einziges Task.Run – kein doppeltes Wrapping mehr
                return await Task.Run(async () =>
                {
                    _tileCommand.Parameters["@z"].Value = z;
                    _tileCommand.Parameters["@x"].Value = x;
                    _tileCommand.Parameters["@y"].Value = tmsY;

                    var result = await _tileCommand.ExecuteScalarAsync();
                    if (result is not byte[] tileData)
                        return null;

                    var decompressedData = await DecompressIfNeededAsync(tileData);
                    var contentType = GetContentType(decompressedData);
                    var base64 = Convert.ToBase64String(decompressedData);
                    return $"data:{contentType};base64,{base64}";
                });
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error getting tile z={z}, x={x}, y={y}: {ex.Message}");
                return null;
            }
        }

        private static async Task<byte[]> DecompressIfNeededAsync(byte[] data)
        {
            if (data.Length < 2 || data[0] != 0x1f || data[1] != 0x8b)
                return data;

            try
            {
                using var compressedStream = new MemoryStream(data);
                using var gzipStream = new GZipStream(compressedStream, CompressionMode.Decompress);
                using var decompressedStream = new MemoryStream(data.Length * 3);
                await gzipStream.CopyToAsync(decompressedStream);
                return decompressedStream.ToArray();
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error decompressing tile: {ex.Message}");
                return data;
            }
        }

        private static string GetContentType(byte[] data)
        {
            if (data.Length >= 8 &&
                data[0] == 0x89 && data[1] == 0x50 && data[2] == 0x4E && data[3] == 0x47)
                return "image/png";

            if (data.Length >= 2 && data[0] == 0xFF && data[1] == 0xD8)
                return "image/jpeg";

            if (data.Length >= 12 &&
                data[0] == 0x52 && data[1] == 0x49 && data[2] == 0x46 && data[3] == 0x46)
                return "image/webp";

            return "application/x-protobuf";
        }

        public void Dispose()
        {
            if (_disposed) return;
            _disposed = true;
            _tileCommand?.Dispose();
            _connection?.Close();
            _connection?.Dispose();
        }
    }
}