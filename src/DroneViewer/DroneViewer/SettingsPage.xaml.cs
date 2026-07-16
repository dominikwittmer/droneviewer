using DroneViewer.Services;

namespace DroneViewer
{
    public partial class SettingsPage : ContentPage
    {
        private CancellationTokenSource? _downloadCts;
        private readonly IForegroundServiceHelper? _foregroundService;
        private const string OfflineModeKey = "OfflineMode";
        private const string MapFilePathKey = "MapFilePath";
        private const string KeepScreenOnKey = "KeepScreenOn";
        private const string MapTilerKeyKey = "MapTilerKey";

        public SettingsPage()
        {
            InitializeComponent();

#if ANDROID
            _foregroundService = Application.Current?.Handler?.MauiContext?
                .Services.GetService<IForegroundServiceHelper>();
#endif

            OfflineModeSwitch.IsToggled = Preferences.Get(OfflineModeKey, false);
            KeepScreenOnSwitch.IsToggled = Preferences.Get(KeepScreenOnKey, true);
        }

        private void OfflineModeSwitch_Toggled(object sender, ToggledEventArgs e)
        {
            DownloadMapLabel.IsEnabled = e.Value;
            DownLoadMapButton.IsEnabled = e.Value;
            DownloadUrlLabel.IsEnabled = e.Value;
            DownloadUrlInput.IsEnabled = e.Value;
            Preferences.Set(OfflineModeKey, e.Value);
        }

        private void KeepScreenOnSwitch_Toggled(object sender, ToggledEventArgs e)
        {
            Preferences.Set(KeepScreenOnKey, e.Value);

            // Aktualisiere sofort den Screen-Status wenn MainPage aktiv ist
            if (Application.Current?.MainPage is NavigationPage navPage)
            {
                if (navPage.Navigation.NavigationStack.FirstOrDefault() is MainPage mainPage)
                {
                    mainPage.UpdateKeepScreenOn(e.Value);
                }
            }
        }

        private void CancelButton_Clicked(object sender, EventArgs e)
        {
            _downloadCts?.Cancel();
        }

        private async void DownLoadMapButton_Clicked(object sender, EventArgs e)
        {
            try
            {
                _foregroundService?.StartService();

                DeviceDisplay.Current.KeepScreenOn = true;
                var url = DownloadUrlInput.Text?.Trim();

                if (string.IsNullOrWhiteSpace(url))
                {
                    await DisplayAlertAsync("Fehler", "Bitte geben Sie eine URL ein.", "OK");
                    return;
                }

                // Validate URL
                if (!Uri.TryCreate(url, UriKind.Absolute, out var uri))
                {
                    await DisplayAlertAsync("Fehler", "Ungültige URL.", "OK");
                    return;
                }

                // Create new cancellation token
                _downloadCts = new CancellationTokenSource();

                // Show progress UI
                ProgressContainer.IsVisible = true;
                DownloadProgressBar.Progress = 0;
                ProgressLabel.Text = "0%";

                // Disable controls during download
                DownLoadMapButton.IsEnabled = false;
                DownloadUrlInput.IsEnabled = false;
                OfflineModeSwitch.IsEnabled = false;

                // Download file
                using var httpClient = new HttpClient();
                httpClient.Timeout = TimeSpan.FromMinutes(10);

                var response = await httpClient.GetAsync(uri, 
                    HttpCompletionOption.ResponseHeadersRead, 
                    _downloadCts.Token);
                response.EnsureSuccessStatusCode();

                var fileName = "ch.swisstopo.base.vt.mbtiles";
                // Save to app data directory
                var filePath = Path.Combine(FileSystem.AppDataDirectory, fileName);
                Preferences.Set(MapFilePathKey, filePath);

                // Prüfe ob Datei bereits existiert
                if (File.Exists(filePath))
                {
                    var overwrite = await DisplayAlertAsync(
                        "Datei existiert",
                        $"Die Datei {fileName} existiert bereits. Überschreiben?",
                        "Ja", "Nein");

                    if (!overwrite)
                        return;
                }

                // Download and save with progress
                await using var fileStream = File.Create(filePath);
                await using var downloadStream = await response.Content.ReadAsStreamAsync(_downloadCts.Token);

                var totalBytes = response.Content.Headers.ContentLength ?? -1;
                var totalRead = 0L;
                var buffer = new byte[8192];
                var isMoreToRead = true;
                var lastProgressUpdate = DateTime.MinValue;

                do
                {
                    var read = await downloadStream.ReadAsync(buffer, _downloadCts.Token);
                    if (read == 0)
                    {
                        isMoreToRead = false;
                    }
                    else
                    {
                        await fileStream.WriteAsync(buffer.AsMemory(0, read), _downloadCts.Token);
                        totalRead += read;

                        // Update progress
                        if (totalBytes > 0)
                        {
                            var progressValue = (double)totalRead / totalBytes;
                            var progressPercent = progressValue * 100;
                            var progressText = $"{progressPercent}% ({totalRead / 1024 / 1024:F1} MB / {totalBytes / 1024 / 1024:F1} MB)";

                            // Throttle updates (max alle 500ms)
                            if (DateTime.Now - lastProgressUpdate > TimeSpan.FromMilliseconds(500))
                            {
                                lastProgressUpdate = DateTime.Now;

                                // Foreground Service Notification aktualisieren
                                _foregroundService?.UpdateProgress(progressPercent, progressText);
                            }

                            // Update UI on main thread
                            MainThread.BeginInvokeOnMainThread(() =>
                            {
                                DownloadProgressBar.Progress = progressValue;
                                ProgressLabel.Text = $"{progressPercent:F1}% ({totalRead / 1024 / 1024:F1} MB / {totalBytes / 1024 / 1024:F1} MB)";
                            });
                        }
                    }
                }
                while (isMoreToRead);

                _foregroundService?.UpdateProgress(100, "Download abgeschlossen!");

                await DisplayAlertAsync("Erfolg",
                    $"Datei erfolgreich heruntergeladen:\n{fileName}",
                    "OK");
            }
            catch (OperationCanceledException)
            {
                await DisplayAlertAsync("Abgebrochen", "Download wurde abgebrochen.", "OK");
            }
            catch (HttpRequestException ex)
            {
                await DisplayAlertAsync("Netzwerkfehler",
                    $"Fehler beim Herunterladen: {ex.Message}",
                    "OK");
            }
            catch (Exception ex)
            {
                await DisplayAlertAsync("Fehler",
                    $"Fehler beim Speichern der Datei: {ex.Message}",
                    "OK");
            }
            finally
            {
                _foregroundService?.StopService();

                DeviceDisplay.Current.KeepScreenOn = false;

                // Hide progress and re-enable controls
                ProgressContainer.IsVisible = false;
                DownLoadMapButton.IsEnabled = true;
                DownloadUrlInput.IsEnabled = true;
                OfflineModeSwitch.IsEnabled = true;

                _downloadCts?.Dispose();
                _downloadCts = null;
            }
        }

        private void MapTilerKeyInput_TextChanged(object sender, TextChangedEventArgs e)
        {
            Preferences.Set(MapTilerKeyKey, e.NewTextValue);
        }
    }
}