using Android.App;
using Android.Content;
using Android.OS;
using AndroidX.Core.App;

namespace DroneViewer.Platforms.Android;

[Service(ForegroundServiceType = global::Android.Content.PM.ForegroundService.TypeConnectedDevice)]
public class BleConnectionService : Service
{
    private const int ServiceNotificationId = 1001;
    private const string ChannelId = "ble_connection_channel";
    private const string ChannelName = "BLE Verbindung";

    public override IBinder? OnBind(Intent? intent)
    {
        return null;
    }

    public override StartCommandResult OnStartCommand(Intent? intent, StartCommandFlags flags, int startId)
    {
        CreateNotificationChannel();

        var notification = new NotificationCompat.Builder(this, ChannelId)
            .SetContentTitle("Drohnen-Verbindung aktiv")
            .SetContentText("Empfange Telemetrie-Daten...")
            .SetSmallIcon(Resource.Drawable.abc_btn_check_material)
            .SetOngoing(true)
            .SetPriority(NotificationCompat.PriorityLow)
            .Build();

        StartForeground(ServiceNotificationId, notification);

        return StartCommandResult.Sticky;
    }

    public override void OnDestroy()
    {
        base.OnDestroy();
        StopForeground(StopForegroundFlags.Remove);
    }

    private void CreateNotificationChannel()
    {
        if (Build.VERSION.SdkInt >= BuildVersionCodes.O)
        {
            var channel = new NotificationChannel(
                ChannelId,
                ChannelName,
                NotificationImportance.Low)
            {
                Description = "Zeigt Status der BLE-Verbindung zu Drohnen an"
            };

            var notificationManager = (NotificationManager?)GetSystemService(NotificationService);
            notificationManager?.CreateNotificationChannel(channel);
        }
    }
}
