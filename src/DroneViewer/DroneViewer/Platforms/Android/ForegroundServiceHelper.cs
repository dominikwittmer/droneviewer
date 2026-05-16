using Android.App;
using Android.Content;
using Android.OS;
using DroneViewer.Services;


namespace DroneViewer.Platforms.Android
{
    internal class ForegroundServiceHelper : IForegroundServiceHelper
    {
        public void StartService()
        {
            var context = Platform.CurrentActivity ?? Platform.AppContext;
            var intent = new Intent(context, typeof(DownloadForegroundService));

            if (Build.VERSION.SdkInt >= BuildVersionCodes.O)
                context.StartForegroundService(intent);
            else
                context.StartService(intent);
        }

        public void StopService()
        {
            var context = Platform.CurrentActivity ?? Platform.AppContext;
            var intent = new Intent(context, typeof(DownloadForegroundService));
            context.StopService(intent);
        }

        public void UpdateProgress(double progress, string message)
        {
            // Notification direkt aktualisieren
            var context = Platform.AppContext;
            var notification = new AndroidX.Core.App.NotificationCompat.Builder(context,
                DownloadForegroundService.ChannelId)
                .SetContentTitle("Download läuft")
                .SetContentText(message)
                .SetSmallIcon(global::Android.Resource.Drawable.StatSysDownload)
                .SetOngoing(true)
                .SetProgress(100,(int)progress, false)
                .Build();

            var manager = context.GetSystemService(Context.NotificationService) as NotificationManager;
            manager?.Notify(DownloadForegroundService.ServiceId, notification);
        }
    }
}
