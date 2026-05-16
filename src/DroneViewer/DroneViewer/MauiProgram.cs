using DroneViewer.Services;
using Microsoft.Extensions.Logging;

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
                });

#if DEBUG
            builder.Logging.AddDebug();
#endif

#if ANDROID
            builder.Services.AddSingleton<IForegroundServiceHelper,
                DroneViewer.Platforms.Android.ForegroundServiceHelper>();
#endif

            return builder.Build();
        }
    }
}
