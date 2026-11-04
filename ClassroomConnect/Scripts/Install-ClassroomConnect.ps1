[CmdletBinding()]
param(
    # User: no administrator needed; this Windows account only (each student account runs it once).
    # Engine: needs an administrator; every account on this computer.
    [ValidateSet('User', 'Engine')][string]$Scope = 'User',
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    # The packaged plugin folder (contains ClassroomConnect.uplugin and Binaries\Win64). Defaults to the copy next to this script.
    [string]$PluginSource,
    # Tests only: install into this folder and change no environment variable.
    [string]$TestRoot,
    [switch]$Uninstall
)
# Installs Classroom Connect so it appears in every Unreal Engine 5.8.4 project on this computer.
#   User   -> %LOCALAPPDATA%\ClassroomConnect\Plugins\ClassroomConnect, listed in the user's UE_ADDITIONAL_PLUGIN_PATHS.
#             Unreal loads precompiled plugins from those folders and enables them in every project (PluginManager.cpp).
#   Engine -> <engine>\Engine\Plugins\Marketplace\ClassroomConnect with EnabledByDefault, like a Fab plugin installed to the engine.
# Changes nothing else: no services, firewall rules, policies or other plugins.
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 leaves $PSScriptRoot empty in param() defaults, so resolve it here.
if (!$PluginSource) { $PluginSource = Join-Path $PSScriptRoot 'ClassroomConnect' }
function Fail([string]$Message) { Write-Host "Classroom Connect was not installed: $Message" -ForegroundColor Red; exit 1 }
function ReadJson([string]$Path) { Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json }
function WriteJson([string]$Path, $Value) { [IO.File]::WriteAllText($Path, ($Value | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false))) }

$VersionFile = Join-Path $EngineRoot 'Engine\Build\Build.version'
if (!(Test-Path -LiteralPath $VersionFile)) { Fail "Unreal Engine 5.8.4 was not found at $EngineRoot. Pass -EngineRoot." }
$Version = ReadJson $VersionFile
$VersionText = "$($Version.MajorVersion).$($Version.MinorVersion).$($Version.PatchVersion)"
if ($VersionText -ne '5.8.4') { Fail "this package is for Unreal Engine 5.8.4, but $EngineRoot is $VersionText." }

$PluginsDir = if ($TestRoot) { $TestRoot } elseif ($Scope -eq 'User') { Join-Path $env:LOCALAPPDATA 'ClassroomConnect\Plugins' } else { Join-Path $EngineRoot 'Engine\Plugins\Marketplace' }
$Target = Join-Path $PluginsDir 'ClassroomConnect'
$EnvName = 'UE_ADDITIONAL_PLUGIN_PATHS'
function UserPluginPaths { @(([string][Environment]::GetEnvironmentVariable($EnvName, 'User')) -split ';' | Where-Object { $_ }) }
function SameDir([string]$A, [string]$B) { $A.TrimEnd('\').Equals($B.TrimEnd('\'), [StringComparison]::OrdinalIgnoreCase) }

if (!$TestRoot -and (Get-Process UnrealEditor, UnrealEditor-Cmd -ErrorAction SilentlyContinue)) { Fail 'close every Unreal Editor window first, then run this again.' }

if ($Uninstall) {
    # Only ever removes a folder that holds this plugin.
    if (Test-Path -LiteralPath (Join-Path $Target 'ClassroomConnect.uplugin')) { Remove-Item -LiteralPath $Target -Recurse -Force }
    if ($Scope -eq 'User' -and !$TestRoot) {
        $Remaining = @(UserPluginPaths | Where-Object { !(SameDir $_ $PluginsDir) })
        [Environment]::SetEnvironmentVariable($EnvName, $(if ($Remaining.Count) { $Remaining -join ';' } else { $null }), 'User')
    }
    Write-Host "Classroom Connect removed from $Target." -ForegroundColor Green
    exit 0
}

$Descriptor = Join-Path $PluginSource 'ClassroomConnect.uplugin'
$Modules = Join-Path $PluginSource 'Binaries\Win64\UnrealEditor.modules'
if (!(Test-Path -LiteralPath $Descriptor) -or !(Test-Path -LiteralPath (Join-Path $PluginSource 'Binaries\Win64\UnrealEditor-ClassroomConnect.dll')) -or !(Test-Path -LiteralPath $Modules)) {
    Fail "$PluginSource is not a packaged Classroom Connect plugin (expected ClassroomConnect.uplugin and Binaries\Win64)."
}
# Unreal refuses precompiled modules built for another engine build, so check before copying.
$EngineBuildId = (ReadJson (Join-Path $EngineRoot 'Engine\Binaries\Win64\UnrealEditor.modules')).BuildId
$PluginBuildId = (ReadJson $Modules).BuildId
if ($EngineBuildId -ne $PluginBuildId) { Fail "this package was built for a different Unreal build ($PluginBuildId; this engine is $EngineBuildId). Rebuild it with Build-Release.ps1 on this engine." }

try { New-Item -ItemType Directory -Force $PluginsDir | Out-Null; [IO.File]::WriteAllText((Join-Path $PluginsDir '.write-test'), ''); Remove-Item -LiteralPath (Join-Path $PluginsDir '.write-test') }
catch { Fail "no permission to write to $PluginsDir. $(if ($Scope -eq 'Engine') { 'An engine install needs an administrator; use -Scope User otherwise.' })" }

# Copy to a staging folder, then swap it in; the previous version is kept next to it until the next install.
$Staging = "$Target.installing"
$Previous = "$Target.previous"
if (Test-Path -LiteralPath $Staging) { Remove-Item -LiteralPath $Staging -Recurse -Force }
New-Item -ItemType Directory $Staging | Out-Null
Get-ChildItem -LiteralPath $PluginSource | Where-Object { $_.Name -ne 'Intermediate' } | Copy-Item -Destination $Staging -Recurse
if ($Scope -eq 'Engine' -and !$TestRoot) {
    # Engine plugins are off unless their descriptor says otherwise (FPlugin::IsEnabledByDefault).
    $Json = ReadJson (Join-Path $Staging 'ClassroomConnect.uplugin')
    $Json | Add-Member -NotePropertyName EnabledByDefault -NotePropertyValue $true -Force
    WriteJson (Join-Path $Staging 'ClassroomConnect.uplugin') $Json
}
if (Test-Path -LiteralPath $Target) {
    if (Test-Path -LiteralPath $Previous) { Remove-Item -LiteralPath $Previous -Recurse -Force }
    Rename-Item -LiteralPath $Target -NewName (Split-Path $Previous -Leaf)
}
Rename-Item -LiteralPath $Staging -NewName 'ClassroomConnect'

if ($Scope -eq 'User' -and !$TestRoot) {
    $Paths = UserPluginPaths
    if (!($Paths | Where-Object { SameDir $_ $PluginsDir })) { [Environment]::SetEnvironmentVariable($EnvName, (@($Paths) + $PluginsDir) -join ';', 'User') }
}

$Installed = ReadJson (Join-Path $Target 'ClassroomConnect.uplugin')
Write-Host "Classroom Connect $($Installed.VersionName) installed for Unreal Engine $VersionText ($Scope) in $Target." -ForegroundColor Green
if ($Scope -eq 'User' -and !$TestRoot) {
    Write-Host 'Close the Epic Games Launcher completely (including its icon next to the clock) so it picks up the change, then open your project.'
}
Write-Host 'Look for the Classroom button on the level editor toolbar.'
