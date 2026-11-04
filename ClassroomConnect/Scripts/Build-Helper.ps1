[CmdletBinding()]
param([string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8')
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$DotnetRoot = Join-Path $EngineRoot 'Engine/Binaries/ThirdParty/DotNet/10.0/win-x64'
$Sdk = Get-ChildItem (Join-Path $DotnetRoot 'sdk') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$Ref = Get-ChildItem (Join-Path $DotnetRoot 'packs/Microsoft.NETCore.App.Ref') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$Runtime = Get-ChildItem (Join-Path $DotnetRoot 'shared/Microsoft.NETCore.App') -Directory | Sort-Object Name -Descending | Select-Object -First 1
$Out = Join-Path $ProjectRoot 'artifacts/helper'
New-Item -ItemType Directory -Force $Out | Out-Null
# Dependency-free build with the engine SDK compiler and reference assemblies.
# This avoids NuGet configuration access and does not download packages.
$Response = @('/nostdlib+','/target:exe','/nullable:enable','/warnaserror+','/langversion:latest',('/out:"' + (Join-Path $Out 'ClassroomConnectHelper.dll') + '"'))
$Response += Get-ChildItem (Join-Path $Ref.FullName 'ref/net10.0') -Filter '*.dll' | ForEach-Object { '/reference:"' + $_.FullName + '"' }
$Response += Get-ChildItem (Join-Path $ProjectRoot 'Helper') -Filter '*.cs' | ForEach-Object { '"' + $_.FullName + '"' }
$ResponseFile = Join-Path $Out 'compile.rsp'
$Response | Set-Content -LiteralPath $ResponseFile
& (Join-Path $DotnetRoot 'dotnet.exe') (Join-Path $Sdk.FullName 'Roslyn/bincore/csc.dll') ('@' + $ResponseFile)
if ($LASTEXITCODE -ne 0) { throw 'Helper compilation failed.' }
@{ runtimeOptions = @{ tfm = 'net10.0'; framework = @{ name = 'Microsoft.NETCore.App'; version = $Runtime.Name } } } | ConvertTo-Json -Depth 5 | Set-Content (Join-Path $Out 'ClassroomConnectHelper.runtimeconfig.json')
Write-Output "Built $Out/ClassroomConnectHelper.dll"

