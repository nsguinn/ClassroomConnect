[CmdletBinding()]
param([string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8')
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$Version = Get-Content (Join-Path $EngineRoot 'Engine/Build/Build.version') -Raw | ConvertFrom-Json
if ($Version.MajorVersion -ne 5 -or $Version.MinorVersion -ne 8 -or $Version.PatchVersion -ne 4) { throw 'Unreal 5.8.4 is required.' }
# Keep this short: UnrealBuildTool refuses intermediate paths over 260 characters, and the packaged
# HostProject adds about 150 characters below the package folder.
$Run = Join-Path $ProjectRoot ('artifacts/p' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
if ($Run.Length -gt 100) { throw "Move the repository to a shorter folder: $Run is $($Run.Length) characters (limit 100)." }
New-Item -ItemType Directory -Force $Run | Out-Null
$PreviousLog = $env:uebp_LogFolder
$PreviousFinalLog = $env:uebp_FinalLogFolder
try {
    $env:uebp_LogFolder = Join-Path $Run 'logs'
    $env:uebp_FinalLogFolder = $env:uebp_LogFolder
    & (Join-Path $EngineRoot 'Engine/Build/BatchFiles/RunUAT.bat') BuildPlugin "-Plugin=$ProjectRoot\Plugins\ClassroomConnect\ClassroomConnect.uplugin" "-Package=$Run\Plugin" -TargetPlatforms=Win64 -Rocket
    if ($LASTEXITCODE -ne 0) { throw "Plugin build failed. Inspect $Run/logs. No editor-load success is implied." }
} finally {
    $env:uebp_LogFolder = $PreviousLog
    $env:uebp_FinalLogFolder = $PreviousFinalLog
}
