namespace DroneViewer
{
    public partial class App : Application
    {
        private const string MbtilesFileName = "ch.swisstopo.base.vt.mbtiles";

        public App()
        {
            InitializeComponent();

            _ = CopyMbtilesToAppDataAsync();
        }

        private static async Task CopyMbtilesToAppDataAsync()
        {
            var targetPath = Path.Combine(FileSystem.AppDataDirectory, MbtilesFileName);

            // Nur kopieren, wenn Datei noch nicht existiert
            if (File.Exists(targetPath))
                return;

            try
            {
                // Aus Raw Assets lesen
                using var sourceStream = await FileSystem.OpenAppPackageFileAsync(MbtilesFileName);
                using var targetStream = File.Create(targetPath);
                await sourceStream.CopyToAsync(targetStream);

                System.Diagnostics.Debug.WriteLine($"MBTiles copied to: {targetPath}");
            }
            catch (Exception ex)
            {
                System.Diagnostics.Debug.WriteLine($"Error copying MBTiles: {ex.Message}");
            }
        }

        protected override Window CreateWindow(IActivationState? activationState)
        {
            var window = new Window(new AppShell());

            // Lifecycle-Events registrieren
            window.Resumed += OnAppResumed;
            window.Stopped += OnAppStopped;
            window.Deactivated += OnAppDeactivated;

            return window;
        }

        private void OnAppResumed(object? sender, EventArgs e)
        {
            System.Diagnostics.Debug.WriteLine("App: Resumed (Vordergrund)");
            // MainPage wird OnAppearing aufrufen
        }

        private void OnAppStopped(object? sender, EventArgs e)
        {
            System.Diagnostics.Debug.WriteLine("App: Stopped (Hintergrund/Beendet)");
            // MainPage wird OnDisappearing aufrufen
        }

        private void OnAppDeactivated(object? sender, EventArgs e)
        {
            System.Diagnostics.Debug.WriteLine("App: Deactivated (Screen aus oder minimiert)");
            // MainPage wird OnDisappearing aufrufen
        }
    }
}