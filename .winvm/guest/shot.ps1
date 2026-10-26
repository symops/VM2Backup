param([string]$Name = 'shot')
. C:\vm2b\guest\lib.ps1
Add-Type -TypeDefinition 'using System.Runtime.InteropServices; public class Dpi { [DllImport("user32.dll")] public static extern bool SetProcessDPIAware(); }'
[Dpi]::SetProcessDPIAware() | Out-Null
New-Item -ItemType Directory -Force -Path "$Share\results\shots" | Out-Null
Take-Screenshot "$Share\results\shots\$Name.png"
Write-Output "screenshot $Name"
