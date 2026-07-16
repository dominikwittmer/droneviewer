using DroneViewer.Services;
using Microsoft.Extensions.Logging;
using Microsoft.Maui.Handlers;

namespace DroneViewer
{
    public static class MauiProgram
    {
        public static MauiApp CreateMauiApp()
        {
            var builder = MauiApp.CreateBuilder();
            builder
                .UseMauiApp<App>()
                .ConfigureFonts(fonts =>
                {
                    fonts.AddFont("OpenSans-Regular.ttf", "OpenSansRegular");
                    fonts.AddFont("OpenSans-Semibold.ttf", "OpenSansSemibold");
                    fonts.AddFont("MaterialSymbolsOutlined_28pt-Light.ttf", "Material");
                });

#if DEBUG
            builder.Logging.AddDebug();
#endif

#if ANDROID
            builder.Services.AddSingleton<IForegroundServiceHelper,
                DroneViewer.Platforms.Android.ForegroundServiceHelper>();

#endif

            builder.Services.AddSingleton<ILocalAssetServer, LocalAssetServer>();
            builder.Services.AddSingleton<MainPage>();

            var app = builder.Build();
            _ = app.Services.GetRequiredService<ILocalAssetServer>().StartAsync();

            return app;
        }
    }
}
