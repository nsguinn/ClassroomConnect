[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    # Reuse the newest packaged plugin in artifacts instead of building again.
    [switch]$SkipBuild
)
# Builds the classroom install package: one zip with the compiled plugin, the install/deploy scripts, double-click
# installers, the project-distribution helper and the instructions. Copy the zip to a USB drive or a share students can
# read, or give it to IT for Deploy-ClassroomConnect.ps1.
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path $PSScriptRoot -Parent
if (!$SkipBuild) { & (Join-Path $PSScriptRoot 'Build-Plugin.ps1') -EngineRoot $EngineRoot }
& (Join-Path $PSScriptRoot 'Build-Helper.ps1') -EngineRoot $EngineRoot | Out-Null
$Package = Get-ChildItem (Join-Path $ProjectRoot 'artifacts') -Directory -Filter 'p*' | Where-Object { Test-Path (Join-Path $_.FullName 'Plugin/Binaries/Win64') } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (!$Package) { throw 'No packaged plugin found. Run without -SkipBuild.' }
$Plugin = Join-Path $Package.FullName 'Plugin'
$Version = (Get-Content (Join-Path $Plugin 'ClassroomConnect.uplugin') -Raw | ConvertFrom-Json).VersionName
$Name = "ClassroomConnect-$Version-UE5.8.4-Win64"
$Out = Join-Path $ProjectRoot "artifacts/release/$Name"
if (Test-Path $Out) { Remove-Item -LiteralPath $Out -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $Out 'ClassroomConnect'), (Join-Path $Out 'Helper') | Out-Null

Get-ChildItem -LiteralPath $Plugin | Where-Object { $_.Name -ne 'Intermediate' } | Copy-Item -Destination (Join-Path $Out 'ClassroomConnect') -Recurse
Copy-Item (Join-Path $PSScriptRoot 'Install-ClassroomConnect.ps1'), (Join-Path $PSScriptRoot 'Deploy-ClassroomConnect.ps1'), (Join-Path $PSScriptRoot 'Collect-ClassroomLogs.ps1') $Out
Copy-Item (Join-Path $ProjectRoot 'Docs/INSTALL.md'), (Join-Path $ProjectRoot 'Docs/USER-GUIDE.md'), (Join-Path $ProjectRoot 'Docs/NETWORK.md'), (Join-Path $ProjectRoot 'Docs/CLASSROOM-TEST.md'), (Join-Path $ProjectRoot 'Config/ClassroomTeams.example.json') $Out
Copy-Item (Join-Path $ProjectRoot 'artifacts/helper/ClassroomConnectHelper.dll'), (Join-Path $ProjectRoot 'artifacts/helper/ClassroomConnectHelper.runtimeconfig.json') (Join-Path $Out 'Helper')

# Double-click installers for one Windows account (no administrator needed). -ExecutionPolicy Bypass applies to this
# one PowerShell process only; it changes no system setting.
function WriteCmd([string]$File, [string]$Arguments) {
    "@echo off`r`npowershell -NoProfile -ExecutionPolicy Bypass -File `"%~dp0Install-ClassroomConnect.ps1`" $Arguments`r`npause`r`n" | Set-Content -Encoding ascii -NoNewline (Join-Path $Out $File)
}
WriteCmd 'Install Classroom Connect (this user).cmd' '-Scope User'
WriteCmd 'Uninstall Classroom Connect (this user).cmd' '-Scope User -Uninstall'
@'
[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$CliArgs
)
# Runs the project-distribution helper (publish / install / launch) with the .NET runtime that ships with Unreal 5.8.4.
& (Join-Path $EngineRoot 'Engine/Binaries/ThirdParty/DotNet/10.0/win-x64/dotnet.exe') (Join-Path $PSScriptRoot 'ClassroomConnectHelper.dll') @CliArgs
exit $LASTEXITCODE
'@ | Set-Content -Encoding ascii (Join-Path $Out 'Helper/Helper.ps1')

$Zip = "$Out.zip"
if (Test-Path $Zip) { Remove-Item -LiteralPath $Zip -Force }
Compress-Archive -Path (Join-Path $Out '*') -DestinationPath $Zip
Write-Output "Built $Zip ($([math]::Round((Get-Item $Zip).Length / 1MB, 1)) MB) from package $($Package.Name)"
Write-Output "SHA256 $((Get-FileHash $Zip -Algorithm SHA256).Hash)"
