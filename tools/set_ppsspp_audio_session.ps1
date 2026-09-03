[CmdletBinding()]
param(
    [switch]$Apply,
    [switch]$Mute,
    [ValidateRange(1, [int]::MaxValue)]
    [int]$TargetPid
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$allowedProcessNames = @('PPSSPPWindows64', 'PPSSPPWindows')
$ppssppProcesses = @(
    Get-Process | Where-Object { $allowedProcessNames -contains $_.ProcessName }
)

if ($ppssppProcesses.Count -eq 0) {
    throw 'No PPSSPP process is running; refusing to inspect or change audio sessions.'
}
if ($ppssppProcesses.Count -ne 1) {
    $identities = ($ppssppProcesses | ForEach-Object {
        '{0}:{1}' -f $_.ProcessName, $_.Id
    }) -join ', '
    throw "Expected exactly one PPSSPP process, found $($ppssppProcesses.Count): $identities"
}

$ppsspp = $ppssppProcesses[0]
$targetPidWasSpecified = $PSBoundParameters.ContainsKey('TargetPid')
if ($Apply -and -not $targetPidWasSpecified) {
    throw 'Apply mode requires an explicit -TargetPid. No audio state was changed.'
}
if ($targetPidWasSpecified -and $TargetPid -ne $ppsspp.Id) {
    throw "Target PID $TargetPid is not the sole PPSSPP process $($ppsspp.ProcessName):$($ppsspp.Id)."
}
$resolvedPid = [uint32]$ppsspp.Id
$processStartTime = $ppsspp.StartTime

if (-not ('Th08PspAudio.CoreAudioSessions' -as [type])) {
    Add-Type -Language CSharp -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

namespace Th08PspAudio
{
    internal enum EDataFlow
    {
        Render = 0,
        Capture = 1,
        All = 2
    }

    [Flags]
    internal enum ClsCtx : uint
    {
        All = 23
    }

    [ComImport]
    [Guid("BCDE0395-E52F-467C-8E3D-C4579291692E")]
    internal class MMDeviceEnumerator
    {
    }

    [ComImport]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    [Guid("A95664D2-9614-4F35-A746-DE8DB63617E6")]
    internal interface IMMDeviceEnumerator
    {
        [PreserveSig]
        int EnumAudioEndpoints(EDataFlow flow, uint stateMask,
                               out IMMDeviceCollection devices);
        [PreserveSig]
        int GetDefaultAudioEndpoint(EDataFlow flow, int role, out IMMDevice device);
        [PreserveSig]
        int GetDevice([MarshalAs(UnmanagedType.LPWStr)] string id,
                      out IMMDevice device);
        [PreserveSig]
        int RegisterEndpointNotificationCallback(IntPtr client);
        [PreserveSig]
        int UnregisterEndpointNotificationCallback(IntPtr client);
    }

    [ComImport]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    [Guid("0BD7A1BE-7A1A-44DB-8397-C0A3D73B7741")]
    internal interface IMMDeviceCollection
    {
        [PreserveSig]
        int GetCount(out uint count);
        [PreserveSig]
        int Item(uint index, out IMMDevice device);
    }

    [ComImport]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    [Guid("D666063F-1587-4E43-81F1-B948E807363F")]
    internal interface IMMDevice
    {
        [PreserveSig]
        int Activate(ref Guid iid, ClsCtx context, IntPtr activationParameters,
                     [MarshalAs(UnmanagedType.IUnknown)] out object activatedObject);
        [PreserveSig]
        int OpenPropertyStore(int access, out IntPtr propertyStore);
        [PreserveSig]
        int GetId([MarshalAs(UnmanagedType.LPWStr)] out string id);
        [PreserveSig]
        int GetState(out uint state);
    }

    [ComImport]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    [Guid("77AA99A0-1BD6-484F-8BC7-2C654C9A9B6F")]
    internal interface IAudioSessionManager2
    {
        [PreserveSig]
        int GetAudioSessionControl(IntPtr sessionGuid, uint streamFlags,
                                   out IntPtr sessionControl);
        [PreserveSig]
        int GetSimpleAudioVolume(IntPtr sessionGuid, uint streamFlags,
                                 out IntPtr simpleAudioVolume);
        [PreserveSig]
        int GetSessionEnumerator(out IAudioSessionEnumerator sessionEnumerator);
        [PreserveSig]
        int RegisterSessionNotification(IntPtr notification);
        [PreserveSig]
        int UnregisterSessionNotification(IntPtr notification);
        [PreserveSig]
        int RegisterDuckNotification([MarshalAs(UnmanagedType.LPWStr)] string sessionId,
                                     IntPtr notification);
        [PreserveSig]
        int UnregisterDuckNotification(IntPtr notification);
    }

    [ComImport]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    [Guid("E2F5BB11-0570-40CA-ACDD-3AA01277DEE8")]
    internal interface IAudioSessionEnumerator
    {
        [PreserveSig]
        int GetCount(out int count);
        [PreserveSig]
        int GetSession(int index, out IAudioSessionControl sessionControl);
    }

    [ComImport]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    [Guid("F4B1A599-7266-4319-A8CA-E70ACB11E8CD")]
    internal interface IAudioSessionControl
    {
        [PreserveSig]
        int GetState(out int state);
        [PreserveSig]
        int GetDisplayName([MarshalAs(UnmanagedType.LPWStr)] out string displayName);
        [PreserveSig]
        int SetDisplayName([MarshalAs(UnmanagedType.LPWStr)] string displayName,
                           IntPtr eventContext);
        [PreserveSig]
        int GetIconPath([MarshalAs(UnmanagedType.LPWStr)] out string iconPath);
        [PreserveSig]
        int SetIconPath([MarshalAs(UnmanagedType.LPWStr)] string iconPath,
                        IntPtr eventContext);
        [PreserveSig]
        int GetGroupingParam(out Guid groupingId);
        [PreserveSig]
        int SetGroupingParam(ref Guid groupingId, IntPtr eventContext);
        [PreserveSig]
        int RegisterAudioSessionNotification(IntPtr client);
        [PreserveSig]
        int UnregisterAudioSessionNotification(IntPtr client);
    }

    [ComImport]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    [Guid("BFB7FF88-7239-4FC9-8FA2-07C950BE9C6D")]
    internal interface IAudioSessionControl2 : IAudioSessionControl
    {
        [PreserveSig]
        new int GetState(out int state);
        [PreserveSig]
        new int GetDisplayName([MarshalAs(UnmanagedType.LPWStr)] out string displayName);
        [PreserveSig]
        new int SetDisplayName([MarshalAs(UnmanagedType.LPWStr)] string displayName,
                               IntPtr eventContext);
        [PreserveSig]
        new int GetIconPath([MarshalAs(UnmanagedType.LPWStr)] out string iconPath);
        [PreserveSig]
        new int SetIconPath([MarshalAs(UnmanagedType.LPWStr)] string iconPath,
                            IntPtr eventContext);
        [PreserveSig]
        new int GetGroupingParam(out Guid groupingId);
        [PreserveSig]
        new int SetGroupingParam(ref Guid groupingId, IntPtr eventContext);
        [PreserveSig]
        new int RegisterAudioSessionNotification(IntPtr client);
        [PreserveSig]
        new int UnregisterAudioSessionNotification(IntPtr client);
        [PreserveSig]
        int GetSessionIdentifier([MarshalAs(UnmanagedType.LPWStr)] out string sessionId);
        [PreserveSig]
        int GetSessionInstanceIdentifier(
            [MarshalAs(UnmanagedType.LPWStr)] out string sessionInstanceId);
        [PreserveSig]
        int GetProcessId(out uint processId);
        [PreserveSig]
        int IsSystemSoundsSession();
        [PreserveSig]
        int SetDuckingPreference([MarshalAs(UnmanagedType.Bool)] bool optOut);
    }

    [ComImport]
    [InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    [Guid("87CE5498-68D6-44E5-9215-6DA47EF883D8")]
    internal interface ISimpleAudioVolume
    {
        [PreserveSig]
        int SetMasterVolume(float level, IntPtr eventContext);
        [PreserveSig]
        int GetMasterVolume(out float level);
        [PreserveSig]
        int SetMute([MarshalAs(UnmanagedType.Bool)] bool mute, IntPtr eventContext);
        [PreserveSig]
        int GetMute([MarshalAs(UnmanagedType.Bool)] out bool mute);
    }

    public sealed class AudioSessionInfo
    {
        public uint ProcessId { get; set; }
        public int State { get; set; }
        public float Volume { get; set; }
        public bool Muted { get; set; }
        public string EndpointId { get; set; }
        public string SessionId { get; set; }
        public string SessionInstanceId { get; set; }
        public string DisplayName { get; set; }
    }

    public static class CoreAudioSessions
    {
        private const uint DeviceStateActive = 1;

        private static void ThrowIfFailed(int result)
        {
            if (result < 0)
            {
                Marshal.ThrowExceptionForHR(result);
            }
        }

        private static AudioSessionInfo ReadSession(
            string endpointId, IAudioSessionControl baseControl,
            IAudioSessionControl2 control, ISimpleAudioVolume volume)
        {
            uint processId;
            int state;
            float level;
            bool muted;
            string sessionId;
            string sessionInstanceId;
            string displayName;
            ThrowIfFailed(control.GetProcessId(out processId));
            ThrowIfFailed(control.GetState(out state));
            ThrowIfFailed(volume.GetMasterVolume(out level));
            ThrowIfFailed(volume.GetMute(out muted));
            ThrowIfFailed(control.GetSessionIdentifier(out sessionId));
            ThrowIfFailed(control.GetSessionInstanceIdentifier(out sessionInstanceId));
            ThrowIfFailed(control.GetDisplayName(out displayName));
            return new AudioSessionInfo
            {
                ProcessId = processId,
                State = state,
                Volume = level,
                Muted = muted,
                EndpointId = endpointId,
                SessionId = sessionId,
                SessionInstanceId = sessionInstanceId,
                DisplayName = displayName
            };
        }

        public static AudioSessionInfo[] Find(uint wantedProcessId)
        {
            List<AudioSessionInfo> matches = new List<AudioSessionInfo>();
            IMMDeviceEnumerator deviceEnumerator =
                (IMMDeviceEnumerator)(new MMDeviceEnumerator());
            IMMDevice device = null;
            try
            {
                ThrowIfFailed(deviceEnumerator.GetDefaultAudioEndpoint(
                    EDataFlow.Render, 0, out device));
                string endpointId;
                ThrowIfFailed(device.GetId(out endpointId));
                Guid managerId = typeof(IAudioSessionManager2).GUID;
                object activated;
                ThrowIfFailed(device.Activate(
                    ref managerId, ClsCtx.All, IntPtr.Zero, out activated));
                IAudioSessionManager2 manager =
                    (IAudioSessionManager2)activated;
                IAudioSessionEnumerator sessions = null;
                try
                {
                    ThrowIfFailed(manager.GetSessionEnumerator(out sessions));
                    int sessionCount;
                    ThrowIfFailed(sessions.GetCount(out sessionCount));
                    for (int sessionIndex = 0;
                         sessionIndex < sessionCount; ++sessionIndex)
                    {
                        IAudioSessionControl baseControl = null;
                        try
                        {
                            ThrowIfFailed(sessions.GetSession(
                                sessionIndex, out baseControl));
                            IAudioSessionControl2 control =
                                (IAudioSessionControl2)baseControl;
                            uint processId;
                            ThrowIfFailed(control.GetProcessId(out processId));
                            if (processId != wantedProcessId)
                            {
                                continue;
                            }
                            ISimpleAudioVolume volume =
                                (ISimpleAudioVolume)baseControl;
                            matches.Add(ReadSession(
                                endpointId, baseControl, control, volume));
                        }
                        finally
                        {
                            if (baseControl != null)
                            {
                                Marshal.ReleaseComObject(baseControl);
                            }
                        }
                    }
                }
                finally
                {
                    if (sessions != null)
                    {
                        Marshal.ReleaseComObject(sessions);
                    }
                    Marshal.ReleaseComObject(manager);
                }
            }
            finally
            {
                if (device != null)
                {
                    Marshal.ReleaseComObject(device);
                }
                Marshal.ReleaseComObject(deviceEnumerator);
            }
            return matches.ToArray();
        }

        public static AudioSessionInfo SetExact(
            uint wantedProcessId, string wantedEndpointId,
            string wantedSessionInstanceId, float level, bool muted)
        {
            if (level < 0.0f || level > 1.0f)
            {
                throw new ArgumentOutOfRangeException("level");
            }

            AudioSessionInfo result = null;
            int exactMatches = 0;
            IMMDeviceEnumerator deviceEnumerator =
                (IMMDeviceEnumerator)(new MMDeviceEnumerator());
            IMMDevice device = null;
            try
            {
                ThrowIfFailed(deviceEnumerator.GetDefaultAudioEndpoint(
                    EDataFlow.Render, 0, out device));
                string endpointId;
                ThrowIfFailed(device.GetId(out endpointId));
                if (!String.Equals(endpointId, wantedEndpointId,
                                   StringComparison.OrdinalIgnoreCase))
                {
                    throw new InvalidOperationException(
                        "The default render endpoint changed after inspection.");
                }
                Guid managerId = typeof(IAudioSessionManager2).GUID;
                object activated;
                ThrowIfFailed(device.Activate(
                    ref managerId, ClsCtx.All, IntPtr.Zero, out activated));
                IAudioSessionManager2 manager =
                    (IAudioSessionManager2)activated;
                IAudioSessionEnumerator sessions = null;
                try
                {
                    ThrowIfFailed(manager.GetSessionEnumerator(out sessions));
                    int sessionCount;
                    ThrowIfFailed(sessions.GetCount(out sessionCount));
                    for (int sessionIndex = 0;
                         sessionIndex < sessionCount; ++sessionIndex)
                    {
                        IAudioSessionControl baseControl = null;
                        try
                        {
                            ThrowIfFailed(sessions.GetSession(
                                sessionIndex, out baseControl));
                            IAudioSessionControl2 control =
                                (IAudioSessionControl2)baseControl;
                            uint processId;
                            string sessionInstanceId;
                            ThrowIfFailed(control.GetProcessId(out processId));
                            ThrowIfFailed(control.GetSessionInstanceIdentifier(
                                out sessionInstanceId));
                            if (processId != wantedProcessId ||
                                !String.Equals(sessionInstanceId,
                                    wantedSessionInstanceId,
                                    StringComparison.Ordinal))
                            {
                                continue;
                            }
                            ++exactMatches;
                            if (exactMatches != 1)
                            {
                                continue;
                            }
                            ISimpleAudioVolume volume =
                                (ISimpleAudioVolume)baseControl;
                            ThrowIfFailed(volume.SetMasterVolume(
                                level, IntPtr.Zero));
                            ThrowIfFailed(volume.SetMute(
                                muted, IntPtr.Zero));
                            result = ReadSession(
                                endpointId, baseControl, control, volume);
                        }
                        finally
                        {
                            if (baseControl != null)
                            {
                                Marshal.ReleaseComObject(baseControl);
                            }
                        }
                    }
                }
                finally
                {
                    if (sessions != null)
                    {
                        Marshal.ReleaseComObject(sessions);
                    }
                    Marshal.ReleaseComObject(manager);
                }
            }
            finally
            {
                if (device != null)
                {
                    Marshal.ReleaseComObject(device);
                }
                Marshal.ReleaseComObject(deviceEnumerator);
            }
            if (exactMatches != 1 || result == null)
            {
                throw new InvalidOperationException(
                    "The previously inspected PPSSPP audio session is no longer unique; no safe readback is available.");
            }
            return result;
        }
    }
}
'@
}

$sessions = @([Th08PspAudio.CoreAudioSessions]::Find($resolvedPid))
if ($sessions.Count -eq 0) {
    throw "PPSSPP process $resolvedPid has no Core Audio render session; no audio state was changed."
}
if ($sessions.Count -ne 1) {
    $sessionIdentities = ($sessions | ForEach-Object {
        '{0} @ {1}' -f $_.SessionInstanceId, $_.EndpointId
    }) -join ', '
    throw "PPSSPP process $resolvedPid has $($sessions.Count) render sessions; refusing ambiguous operation: $sessionIdentities"
}

$before = $sessions[0]
Write-Output ('MODE={0}' -f $(if ($Apply) { 'APPLY' } else { 'DRY_RUN' }))
Write-Output ('PROCESS_NAME={0}' -f $ppsspp.ProcessName)
Write-Output ('PROCESS_ID={0}' -f $ppsspp.Id)
Write-Output ('PROCESS_START={0:o}' -f $processStartTime)
Write-Output ('ENDPOINT_ID={0}' -f $before.EndpointId)
Write-Output ('SESSION_ID={0}' -f $before.SessionId)
Write-Output ('SESSION_INSTANCE_ID={0}' -f $before.SessionInstanceId)
Write-Output ('SESSION_STATE={0}' -f $before.State)
Write-Output ('BEFORE_VOLUME={0}' -f $before.Volume.ToString('R', [Globalization.CultureInfo]::InvariantCulture))
Write-Output ('BEFORE_MUTED={0}' -f $before.Muted)

if (-not $Apply) {
    Write-Output 'ACTION=NONE (use -Apply [-Mute] -TargetPid <PID> after reviewing this identity)'
    exit 0
}

$processReadback = Get-Process -Id $ppsspp.Id -ErrorAction Stop
if (($allowedProcessNames -notcontains $processReadback.ProcessName) -or
    $processReadback.StartTime -ne $processStartTime) {
    throw 'PPSSPP process identity changed after inspection; no audio state was changed.'
}

$desiredVolume = $(if ($Mute) { $before.Volume } else { 1.0 })
$desiredMuted = [bool]$Mute
$after = [Th08PspAudio.CoreAudioSessions]::SetExact(
    $resolvedPid, $before.EndpointId, $before.SessionInstanceId,
    $desiredVolume, $desiredMuted)
Write-Output ('AFTER_VOLUME={0}' -f $after.Volume.ToString('R', [Globalization.CultureInfo]::InvariantCulture))
Write-Output ('AFTER_MUTED={0}' -f $after.Muted)
if ($after.Volume -ne $desiredVolume -or $after.Muted -ne $desiredMuted) {
    throw ('PPSSPP session audio readback did not reach volume={0} and muted={1}.' -f `
        $desiredVolume, $desiredMuted)
}
Write-Output ('ACTION={0}' -f $(if ($Mute) {
    'PPSSPP_SESSION_MUTED'
} else {
    'PPSSPP_SESSION_UNMUTED'
}))
