[CmdletBinding()]
param(
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [string]$Team = 'Red',
    [int]$TimeoutSeconds = 240,
    # Optional: Content folder of a ClassroomTest project with a saved level, opened in both editors so they share a level
    # (needed to check the large teammate markers; unsaved levels are never shared).
    [string]$ContentFrom,
    [string]$Level = '/Game/test'
)
# Runs a headless Team Leader editor (UCC.Host) and a headless Team Member editor (UCC.Join) on this computer,
# each on its own copy of a test project, and reports what the Classroom Connect log says.
# Uses the newest packaged plugin from Build-Plugin.ps1. Stops only the editors and Multi-User servers it started.
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$Editor = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
$Package = Get-ChildItem (Join-Path $ProjectRoot 'artifacts') -Directory -Filter 'p*' | Where-Object { Test-Path (Join-Path $_.FullName 'Plugin/Binaries/Win64') } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (!$Package) { throw 'No packaged plugin found. Run Build-Plugin.ps1 first.' }
$Run = Join-Path $ProjectRoot ('artifacts/s' + [Guid]::NewGuid().ToString('N').Substring(0, 8))

function NewProject([string]$Name) {
    $Dir = Join-Path $Run $Name
    New-Item -ItemType Directory -Force (Join-Path $Dir 'Plugins') | Out-Null
    Copy-Item -Recurse (Join-Path $Package.FullName 'Plugin') (Join-Path $Dir 'Plugins/ClassroomConnect')
    if ($ContentFrom) { Copy-Item -Recurse $ContentFrom (Join-Path $Dir 'Content') }
    # Same .uproject name in both folders: Concert requires matching project names.
    '{"FileVersion":3,"EngineAssociation":"5.8","Plugins":[{"Name":"ClassroomConnect","Enabled":true}]}' | Set-Content -Encoding ascii (Join-Path $Dir 'ClassroomTest.uproject')
    $Dir
}
function StartEditor([string]$Dir, [string]$Command) {
    $Log = Join-Path $Dir 'editor.log'
    $EditorArgs = @(('"' + (Join-Path $Dir 'ClassroomTest.uproject') + '"'))
    if ($ContentFrom) { $EditorArgs += $Level }
    # Editors write Config/DefaultEngine.ini differently at startup, so the member usually copies the leader's project and
    # reopens on the copy: keep such copies inside this run, never in the user's Documents folder.
    $EditorArgs += @('-nullrhi', '-unattended', '-nosplash', '-nosound', '-NoLiveCoding', ('-ExecCmds="' + $Command + '"'), ('-abslog="' + $Log + '"'), ('-UCCPROJECTS="' + (Join-Path $Run 'MemberProjects') + '"'))
    [pscustomobject]@{ Process = (Start-Process -FilePath $Editor -ArgumentList $EditorArgs -PassThru -WindowStyle Hidden); Log = $Log }
}
function WaitFor([object]$Instance, [string]$Pattern, [int]$Seconds) {
    $Deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $Deadline) {
        if (Test-Path $Instance.Log) {
            $Hit = Select-String -Path $Instance.Log -Pattern $Pattern | Select-Object -Last 1
            if ($Hit) { return $Hit.Line }
        }
        if ($Instance.Process.HasExited) { return "editor exited with code $($Instance.Process.ExitCode)" }
        Start-Sleep -Seconds 2
    }
    "timed out after $Seconds s"
}

$Started = Get-Date
$Host_ = $null; $Member = $null
$Results = [ordered]@{ Package = $Package.Name; Team = $Team }
try {
    $Host_ = StartEditor (NewProject 'Host') "UCC.Host $Team"
    $Results.Host = WaitFor $Host_ 'LogClassroomConnect: .* -> (Connected|Failed):' $TimeoutSeconds
    if ($Results.Host -match '-> Connected') {
        $Member = StartEditor (NewProject 'Member') "UCC.Join $Team"
        $Results.Member = WaitFor $Member 'LogClassroomConnect: .* -> (Connected|Failed):' $TimeoutSeconds
        if ($Results.Member -match '-> Connected') {
            $Results.HostSeesMember = WaitFor $Host_ 'Connected students \(2\)' 60
            $Results.MemberSeesHost = WaitFor $Member 'Connected students \(2\)' 60
            # Each editor picks a presence marker color; both should settle on different colors and see each other's.
            $Results.HostMarker = WaitFor $Host_ 'Your marker color: ' 30
            $Results.MemberMarker = WaitFor $Member 'Your marker color: ' 30
            Start-Sleep -Seconds 5
            $Pattern = 'Connected students \(2\): .*marker (?!not assigned)[^,]+,.*marker (?!not assigned)[^,]+,'
            $Results.HostSeesBothColors = WaitFor $Host_ $Pattern 30
            $Results.MemberSeesBothColors = WaitFor $Member $Pattern 30
            $Colors = foreach ($Instance in $Host_, $Member) { (Select-String -Path $Instance.Log -Pattern 'Your marker color: (.+)$' | Select-Object -Last 1).Matches.Groups[1].Value }
            $Results.DistinctColors = ($Colors.Count -eq 2 -and $Colors[0] -and $Colors[1] -and $Colors[0] -ne $Colors[1])
            if ($ContentFrom) {
                # Same saved level open in both: each editor should report the other as in its level and show a large marker.
                $Results.HostShowsMarker = WaitFor $Host_ 'Showing a large marker for ' 60
                $Results.MemberShowsMarker = WaitFor $Member 'Showing a large marker for ' 60
            }
        }
    }
} finally {
    foreach ($Instance in @($Member, $Host_)) { if ($Instance -and !$Instance.Process.HasExited) { Stop-Process -Id $Instance.Process.Id -Force } }
    # Also an editor the member reopened on a copied project.
    Get-Process UnrealEditor-Cmd, UnrealMultiUserSlateServer, UnrealMultiUserServer -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $Started } | Stop-Process -Force
}
$Results.Logs = $Run
$Results | ConvertTo-Json | Set-Content (Join-Path $Run 'results.json')
$Results.GetEnumerator() | ForEach-Object { '{0}: {1}' -f $_.Key, $_.Value }
foreach ($Name in 'Host', 'Member') {
    $Log = Join-Path $Run "$Name/editor.log"
    if (Test-Path $Log) { "--- $Name Classroom Connect / Concert log ---"; Select-String -Path $Log -Pattern 'LogClassroomConnect|LogConcert: (Display|Warning|Error)|LogConcert:.*(Connected|session)' | ForEach-Object { $_.Line } }
}
