using System;
using System.Collections.Generic;
using System.Text;

namespace DroneViewer.Services
{
    internal interface IForegroundServiceHelper
    {
        void StartService();
        void StopService();
        void UpdateProgress(double progress, string message);
    }
}
