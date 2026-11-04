[CmdletBinding()]
param(
    # The Unreal project folder (contains the .uproject).
    [Parameter(Mandatory = $true)][string]$ProjectPath,
    # Where to put the zip, e.g. a class share. Defaults to the current folder.
    [string]$Destination = (Get-Location).Path,
    # How many recent log files to read.
    [int]$Logs = 5
)
# Classroom test evidence: copies the Classroom Connect and Multi-User lines from a project's recent Unreal logs, plus any
# crash folders, into <COMPUTER>-<project>-<time>.zip. Reads only; changes nothing in the project.
$ErrorActionPreference = 'Stop'
$LogDir = Join-Path $ProjectPath 'Saved\Logs'
if (!(Test-Path -LiteralPath $LogDir)) { throw "No Saved\Logs folder in $ProjectPath." }
$Project = (Get-ChildItem -LiteralPath $ProjectPath -Filter *.uproject | Select-Object -First 1).BaseName
$Stamp = Get-Date -Format 'yyyyMMdd-HHmm'
$Work = Join-Path $env:TEMP "ClassroomLogs-$Stamp"
New-Item -ItemType Directory -Force $Work | Out-Null

$Summary = @("Computer: $env:COMPUTERNAME", "User: $env:USERNAME", "Project: $ProjectPath", "Collected: $(Get-Date -Format s)", '')
foreach ($Log in Get-ChildItem -LiteralPath $LogDir -Filter *.log | Sort-Object LastWriteTime -Descending | Select-Object -First $Logs) {
    $Lines = Select-String -LiteralPath $Log.FullName -Pattern 'LogClassroomConnect|LogConcert|LogUdpMessaging|LogMultiUser|Error:|Fatal' | ForEach-Object { $_.Line }
    $Lines | Set-Content -Encoding utf8 (Join-Path $Work ($Log.BaseName + '.classroom.txt'))
    $Summary += "$($Log.Name) ($($Log.LastWriteTime)): $(@($Lines).Count) relevant lines"
}
$Crashes = Join-Path $ProjectPath 'Saved\Crashes'
if (Test-Path -LiteralPath $Crashes) {
    Get-ChildItem -LiteralPath $Crashes -Directory | Sort-Object LastWriteTime -Descending | Select-Object -First 3 | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName (Join-Path $Work "Crash-$($_.Name)") -Recurse
        $Summary += "Crash folder: $($_.Name)"
    }
}
$Summary | Set-Content -Encoding utf8 (Join-Path $Work 'summary.txt')

New-Item -ItemType Directory -Force $Destination | Out-Null
$Zip = Join-Path $Destination "$env:COMPUTERNAME-$Project-$Stamp.zip"
Compress-Archive -Path (Join-Path $Work '*') -DestinationPath $Zip -Force
Remove-Item -LiteralPath $Work -Recurse -Force
Write-Host "Saved $Zip"
