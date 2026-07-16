using Microsoft.Data.Sqlite;
using SQLitePCL;
using System.IO.Compression;

namespace DroneViewer.Services
{
    public class MBTilesReader : IDisposable
    {
        private SqliteConnection? _connection;
        private bool _disposed;

        public async Task OpenAsync(string mbtilesPath)
        {
            if (!File.Exists(mbtilesPath))
                throw new FileNotFoundException("MBTiles file not found", mbtilesPath);

            Batteries_V2.Init();

            _connection = new SqliteConnection(
                $"Data Source={mbtilesPath};Mode=ReadOnly");
            await _connection.OpenAsync();

            using var pragma = _connection.CreateCommand();
            pragma.CommandText = """
                PRAGMA cache_size=-8192;
                PRAGMA temp_store=MEMORY;
                PRAGMA mmap_size=268435456;
                """;
            await pragma.ExecuteNonQueryAsync();
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

        /// <summary>
        /// Gibt die rohen Tile-Bytes zurück (dekomprimiert).
        /// Thread-safe: jeder Aufruf erstellt einen eigenen SqliteCommand.
        /// </summary>
        public Task<byte[]?> GetTileAsync(int z, int x, int y)
        {
            if (_connection == null)
                return Task.FromResult<byte[]?>(null);

            var tmsY = (1 << z) - 1 - y;

            // Auf Thread-Pool auslagern – eigener Command pro Aufruf = thread-safe
            return Task.Run(() =>
            {
                try
                {
                    using var command = _connection.CreateCommand();
                    command.CommandText =
                        "SELECT tile_data FROM tiles WHERE zoom_level=@z AND tile_column=@x AND tile_row=@y";
                    command.Parameters.AddWithValue("@z", z);
                    command.Parameters.AddWithValue("@x", x);
                    command.Parameters.AddWithValue("@y", tmsY);

                    var result = command.ExecuteScalar();
                    if (result is not byte[] tileData)
                        return null;

                    return DecompressIfNeeded(tileData);
                }
                catch (Exception ex)
                {
                    System.Diagnostics.Debug.WriteLine($"Error getting tile z={z}, x={x}, y={y}: {ex.Message}");
                    return null;
                }
            });
        }

        private static byte[]? DecompressIfNeeded(byte[] data)
        {
            if (data.Length < 2 || data[0] != 0x1f || data[1] != 0x8b)
                return data;

            try
            {
                using var compressedStream = new MemoryStream(data);
                using var gzipStream = new GZipStream(compressedStream, CompressionMode.Decompress);
                using var decompressedStream = new MemoryStream(data.Length * 3);
                gzipStream.CopyTo(decompressedStream);
                return decompressedStream.ToArray();
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error decompressing tile: {ex.Message}");
                return null;
            }
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
