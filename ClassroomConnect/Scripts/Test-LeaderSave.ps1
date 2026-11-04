[CmdletBinding()]
param(
    # Content folder of a ClassroomTest project that holds a saved World Partition level (unsaved levels are never shared).
    [Parameter(Mandatory = $true)][string]$ContentFrom,
    [string]$Level = '/Game/test',
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [int]$TimeoutSeconds = 300
)
# Proves that a Team Leader's session edits end up in the project files without any prompt, in two cases:
#   Leave - host, add a Point Light during the session, run UCC.Leave
#   Close - host, add a Point Light during the session, close the editor window (CLOSE_SLATE_MAINFRAME, the same path as the X)
# Each case uses its own team and a fresh project copy, and stops only the editors and Multi-User servers it started.
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$Editor = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
$Package = Get-ChildItem (Join-Path $ProjectRoot 'artifacts') -Directory -Filter 'p*' | Where-Object { Test-Path (Join-Path $_.FullName 'Plugin/Binaries/Win64') } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (!$Package) { throw 'No packaged plugin found. Run Build-Plugin.ps1 first.' }
if (!(Test-Path (Join-Path $ContentFrom ((Split-Path $Level -Leaf) + '.umap')))) { throw "$ContentFrom does not contain $Level." }
$Run = Join-Path $ProjectRoot ('artifacts/l' + [Guid]::NewGuid().ToString('N').Substring(0, 8))

function ActorFiles([string]$Dir) { @(Get-ChildItem (Join-Path $Dir 'Content/__ExternalActors__') -Recurse -File -Filter *.uasset -ErrorAction SilentlyContinue) }
function RunCase([string]$Name, [string]$Team, [string]$Finish, [bool]$Unattended) {
    $Dir = Join-Path $Run $Name
    New-Item -ItemType Directory -Force (Join-Path $Dir 'Plugins') | Out-Null
    Copy-Item -Recurse (Join-Path $Package.FullName 'Plugin') (Join-Path $Dir 'Plugins/ClassroomConnect')
    Copy-Item -Recurse $ContentFrom (Join-Path $Dir 'Content')
    '{"FileVersion":3,"EngineAssociation":"5.8","Plugins":[{"Name":"ClassroomConnect","Enabled":true}]}' | Set-Content -Encoding ascii (Join-Path $Dir 'ClassroomTest.uproject')
    $Before = (ActorFiles $Dir).Count
    $Log = Join-Path $Dir 'editor.log'
    $Started = Get-Date
    $Commands = "UCC.Host $Team,UCC.AfterConnected ACTOR ADD CLASS=/Script/Engine.PointLight|$Finish"
    $EditorArgs = @(('"' + (Join-Path $Dir 'ClassroomTest.uproject') + '"'), $Level, '-nullrhi', '-nosplash', '-nosound', '-NoLiveCoding', ('-ExecCmds="' + $Commands + '"'), ('-abslog="' + $Log + '"'))
    # -unattended makes the editor skip its close checks entirely ("Unattended mode can always exit", MainFrameHandler.h),
    # which would bypass the plugin's close hook, so the Close case runs attended.
    if ($Unattended) { $EditorArgs += '-unattended' }
    $Process = Start-Process -FilePath $Editor -ArgumentList $EditorArgs -PassThru -WindowStyle Hidden
    $Deadline = $Started.AddSeconds($TimeoutSeconds)
    $Outcome = 'timed out'
    while ((Get-Date) -lt $Deadline) {
        Start-Sleep -Seconds 2
        $Hit = if (Test-Path $Log) { Select-String -Path $Log -Pattern 'LogClassroomConnect: .*(-> Idle: Saved your team|Leader save failed|-> Failed:)' | Select-Object -Last 1 }
        if ($Hit) { $Outcome = $Hit.Line; if ($Finish -ne 'CLOSE_SLATE_MAINFRAME') { break } }
        if ($Process.HasExited) { if ($Outcome -eq 'timed out') { $Outcome = "editor exited with code $($Process.ExitCode)" }; break }
    }
    $Exited = $Process.HasExited
    if (!$Exited) { Stop-Process -Id $Process.Id -Force }
    Get-Process UnrealMultiUserSlateServer, UnrealMultiUserServer -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $Started } | Stop-Process -Force
    $After = ActorFiles $Dir
    $Added = @($After | Where-Object { $_.LastWriteTime -ge $Started })
    [ordered]@{
        Case = $Name; Team = $Team; Outcome = $Outcome; EditorClosedItself = $Exited
        ActorFilesBefore = $Before; ActorFilesAfter = $After.Count; WrittenDuringTest = $Added.Count
        Saved = ($Outcome -match 'Saved your team') -and $Added.Count -ge 1
        Log = (Select-String -Path $Log -Pattern 'LogClassroomConnect|Persist|LogConcert: (Warning|Error)' -ErrorAction SilentlyContinue | ForEach-Object { $_.Line -replace '^\[[^\]]*\]\[ *\d+\]', '' })
    }
}

$Results = @((RunCase 'Leave' 'Mint' 'UCC.Leave' $true), (RunCase 'Close' 'Navy' 'CLOSE_SLATE_MAINFRAME' $false))
$Results | ConvertTo-Json -Depth 4 | Set-Content (Join-Path $Run 'results.json')
foreach ($R in $Results) {
    "=== $($R.Case) (team $($R.Team)): Saved=$($R.Saved) EditorClosedItself=$($R.EditorClosedItself) actor files $($R.ActorFilesBefore) -> $($R.ActorFilesAfter), written during test $($R.WrittenDuringTest)"
    "Outcome: $($R.Outcome)"
    $R.Log | Select-Object -Last 14
}
"Logs: $Run"
