!macro NSIS_HOOK_PREINSTALL
  FindWindow $0 "WatchmanWindowClass"
  ${If} $0 != 0
    ; Native ExitMessage is WM_APP + 14. Let the engine flush its pending samples.
    SendMessage $0 32782 0 0 /TIMEOUT=8000
    Sleep 500
  ${EndIf}
!macroend

; Update mode deliberately skips the old uninstaller. Remove only shipped v1 files.
!macro NSIS_HOOK_POSTINSTALL
  Delete "$INSTDIR\scripts\temperature_probe.ps1"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\Aga.Controls.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\BlackSharp.Core.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\DiskInfoToolkit.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\HidSharp.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\LibreHardwareMonitorLib.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\Microsoft.Bcl.AsyncInterfaces.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\Microsoft.Bcl.HashCode.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\Microsoft.Win32.TaskScheduler.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\OxyPlot.WindowsForms.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\OxyPlot.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\RAMSPDToolkit-NDD.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\System.Buffers.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\System.CodeDom.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\System.Collections.Immutable.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\System.Formats.Nrbf.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\System.IO.Pipelines.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\System.Memory.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\System.Numerics.Vectors.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\System.Reflection.Metadata.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\System.Resources.Extensions.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\System.Runtime.CompilerServices.Unsafe.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\System.Security.AccessControl.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\System.Security.Principal.Windows.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\System.Text.Encodings.Web.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\System.Text.Json.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\System.Threading.AccessControl.dll"
  Delete "$INSTDIR\vendor\libre-hardware-monitor\System.Threading.Tasks.Extensions.dll"
  RMDir "$INSTDIR\scripts"
  RMDir "$INSTDIR\vendor\libre-hardware-monitor"
  RMDir "$INSTDIR\vendor"
!macroend
