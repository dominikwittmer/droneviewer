using Android.App;
using Android.Content;
using Android.OS;
using AndroidX.Core.App;

namespace DroneViewer.Platforms.Android;

[Service(ForegroundServiceType = global::Android.Content.PM.ForegroundService.TypeDataSync)]
public class DownloadForegroundService : Service
{
    public const int ServiceId = 1001;
    public const string ChannelId = "download_channel";
    
    public override IBinder? OnBind(Intent? intent) => null;

    public override StartCommandResult OnStartCommand(Intent? intent, StartCommandFlags flags, int startId)
    {
        CreateNotificationChannel();
        
        var notification = new NotificationCompat.Builder(this, ChannelId)
            .SetContentTitle("Download läuft")
            .SetContentText("Karte wird heruntergeladen...")
            .SetSmallIcon(global::Android.Resource.Drawable.StatSysDownload) // Eigenes Icon hinzufügen
            .SetOngoing(true)
            .SetProgress(100, 0, true)
            .Build();

        StartForeground(ServiceId, notification, 
            global::Android.Content.PM.ForegroundService.TypeDataSync);

        return StartCommandResult.Sticky;
    }

    private void CreateNotificationChannel()
    {
        if (Build.VERSION.SdkInt >= BuildVersionCodes.O)
        {
            var channel = new NotificationChannel(
                ChannelId,
                "Download Service",
                NotificationImportance.Low);
            
            var manager = GetSystemService(NotificationService) as NotificationManager;
            manager?.CreateNotificationChannel(channel);
        }
    }

    public void UpdateProgress(int progress, string message)
    {
        var notification = new NotificationCompat.Builder(this, ChannelId)
            .SetContentTitle("Download läuft")
            .SetContentText(message)
            .SetSmallIcon(global::Android.Resource.Drawable.StatSysDownload)
            .SetOngoing(true)
            .SetProgress(100, progress, false)
            .Build();

        var manager = GetSystemService(NotificationService) as NotificationManager;
        manager?.Notify(ServiceId, notification);
    }
}