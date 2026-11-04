[CmdletBinding(PositionalBinding = $false)]
param(
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [Parameter(ValueFromRemainingArguments = $true)][string[]]$CliArgs
)
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$Dotnet = Join-Path $EngineRoot 'Engine/Binaries/ThirdParty/DotNet/10.0/win-x64/dotnet.exe'
& $Dotnet (Join-Path $ProjectRoot 'artifacts/helper/ClassroomConnectHelper.dll') @CliArgs
exit $LASTEXITCODE
