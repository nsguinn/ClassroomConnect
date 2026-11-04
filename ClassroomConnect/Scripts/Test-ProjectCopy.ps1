[CmdletBinding()]
param(
    # Content folder of a ClassroomTest project with a saved level; the leader's project is built from it.
    [Parameter(Mandatory = $true)][string]$ContentFrom,
    [string]$Level = '/Game/test',
    [string]$Team = 'Gold',
    # Optional transfer tuning for the member, e.g. 'UCC.CopyChunkKB 256,UCC.CopyChunksInFlight 8'
    [string]$CopySettings,
    # Also run an update round: the leader's project gains a file, and a member with the earlier copy should fetch only that.
    [switch]$Update,
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [int]$TimeoutSeconds = 300
)
# End-to-end check of team project copying on one computer, with headless editors:
#   leader  - ClassroomTest (copied from -ContentFrom), runs UCC.Host
#   member  - an unrelated empty project "Lobby", runs UCC.Join; it should copy ClassroomTest from the leader, verify it,
#             install it under <run>\MemberProjects, start a new editor on the copy and close itself
#   reopened member editor - should join the team session by itself
# Then every file of the leader's project is compared with the member's copy by SHA-256.
# Stops only the editors and Multi-User servers started during the test.
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$Editor = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
$Package = Get-ChildItem (Join-Path $ProjectRoot 'artifacts') -Directory -Filter 'p*' | Where-Object { Test-Path (Join-Path $_.FullName 'Plugin/Binaries/Win64') } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (!$Package) { throw 'No packaged plugin found. Run Build-Plugin.ps1 first.' }
$Run = Join-Path $ProjectRoot ('artifacts/c' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
$MemberProjects = Join-Path $Run 'MemberProjects'

function NewProject([string]$Dir, [string]$Name, [bool]$WithContent) {
    New-Item -ItemType Directory -Force (Join-Path $Dir 'Plugins') | Out-Null
    Copy-Item -Recurse (Join-Path $Package.FullName 'Plugin') (Join-Path $Dir 'Plugins/ClassroomConnect')
    if ($WithContent) { Copy-Item -Recurse $ContentFrom (Join-Path $Dir 'Content') }
    '{"FileVersion":3,"EngineAssociation":"5.8","Plugins":[{"Name":"ClassroomConnect","Enabled":true}]}' | Set-Content -Encoding ascii (Join-Path $Dir "$Name.uproject")
    Join-Path $Dir "$Name.uproject"
}
function StartEditor([string]$ProjectFile, [string[]]$Extra, [string]$LogName) {
    # Logs go next to the project folder, not inside it: a locked log inside the project would be part of what is shared.
    if (!$LogName) { $LogName = (Split-Path (Split-Path $ProjectFile) -Leaf) + '.log' }
    $Log = Join-Path (Split-Path (Split-Path $ProjectFile)) $LogName
    $EditorArgs = @(('"' + $ProjectFile + '"')) + $Extra + @('-nullrhi', '-unattended', '-nosplash', '-nosound', '-NoLiveCoding', ('-abslog="' + $Log + '"'))
    [pscustomobject]@{ Process = (Start-Process -FilePath $Editor -ArgumentList $EditorArgs -PassThru -WindowStyle Hidden); Log = $Log }
}
function WaitForLog([string]$Log, [string]$Pattern, [int]$Seconds, $Process) {
    $Deadline = (Get-Date).AddSeconds($Seconds)
    while ((Get-Date) -lt $Deadline) {
        if (Test-Path $Log) { $Hit = Select-String -Path $Log -Pattern $Pattern | Select-Object -Last 1; if ($Hit) { return $Hit.Line -replace '^\[[^\]]*\]\[ *\d+\]', '' } }
        if ($Process -and $Process.HasExited) { return "(process exited with code $($Process.ExitCode))" }
        Start-Sleep -Seconds 2
    }
    "(timed out after $Seconds s)"
}
function InstalledCopies {
    # Installs live at <MemberProjects>\<team>\<revision>\ClassroomTest; look only there (never inside a copy, whose
    # Intermediate folder holds paths over 260 characters, and never in a .partial staging folder).
    Get-ChildItem -LiteralPath (Join-Path $MemberProjects $Team) -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -notlike '*.partial' } |
        ForEach-Object { Get-Item -LiteralPath (Join-Path $_.FullName 'ClassroomTest\ClassroomTest.uproject') -ErrorAction SilentlyContinue } | Sort-Object LastWriteTime
}
function ProjectHashes([string]$Dir, [string]$Root = $Dir) {
    # Same exclusions as the plugin; never descends into them (Unreal's sandbox under Intermediate exceeds 260 characters).
    foreach ($Item in Get-ChildItem -LiteralPath $Dir) {
        if ($Item.PSIsContainer) { if ($Item.Name -notin 'Saved', 'Intermediate', 'DerivedDataCache', '.git', '.vs', '.idea') { ProjectHashes $Item.FullName $Root } }
        elseif ($Item.Extension -ne '.pdb') { [pscustomobject]@{ Path = $Item.FullName.Substring($Root.Length + 1); Hash = (Get-FileHash -LiteralPath $Item.FullName -Algorithm SHA256).Hash } }
    }
}

$Started = Get-Date
$R = [ordered]@{ Package = $Package.Name; Team = $Team }
try {
    $LeaderProject = NewProject (Join-Path $Run 'Leader') 'ClassroomTest' $true
    $Leader = StartEditor $LeaderProject @($Level, ('-ExecCmds="UCC.Host ' + $Team + '"'))
    $R.LeaderConnected = WaitForLog $Leader.Log '-> Connected:|-> Failed:' $TimeoutSeconds $Leader.Process
    $R.LeaderSharing = WaitForLog $Leader.Log 'Sharing ClassroomTest with team|Cannot share' 120 $Leader.Process
    if ($R.LeaderConnected -notmatch '-> Connected' -or $R.LeaderSharing -notmatch 'Sharing') { throw 'Leader did not host and share; see the leader log.' }

    $MemberProject = NewProject (Join-Path $Run 'Lobby') 'Lobby' $false
    $JoinCommands = if ($CopySettings) { "$CopySettings,UCC.Join $Team" } else { "UCC.Join $Team" }
    $Member = StartEditor $MemberProject @(('-ExecCmds="' + $JoinCommands + '"'), ('-UCCPROJECTS="' + $MemberProjects + '"'))
    $R.MemberGotOffer = WaitForLog $Member.Log 'offers ClassroomTest' $TimeoutSeconds $Member.Process
    $R.MemberDownloadPlan = WaitForLog $Member.Log 'files already on this computer; downloading' 120 $Member.Process
    $R.MemberInstalled = WaitForLog $Member.Log 'Installed ClassroomTest revision|Project copy failed|-> Failed:' $TimeoutSeconds $Member.Process
    $Times = Select-String -Path $Member.Log -Pattern '; downloading|Installed ClassroomTest' | ForEach-Object { [datetime]::ParseExact($_.Line.Substring(1, 23), 'yyyy.MM.dd-HH.mm.ss:fff', $null) }
    if (@($Times).Count -ge 2) { $R.DownloadSeconds = [math]::Round(($Times[-1] - $Times[0]).TotalSeconds, 1) }
    $R.MemberReopened = WaitForLog $Member.Log 'Opened .* in a new editor|-> Failed:' 60 $Member.Process
    $R.MemberClosedItself = $Member.Process.WaitForExit(120000)

    # Only a finished install counts, never the .partial staging folder of an interrupted copy.
    $Installed = InstalledCopies | Select-Object -First 1
    $R.InstalledAt = if ($Installed) { $Installed.FullName } else { '(not found)' }
    if ($Installed) {
        $ReopenedLog = Join-Path $Installed.DirectoryName 'Saved\Logs\ClassroomTest.log'
        $R.ReopenedJoined = WaitForLog $ReopenedLog 'LogClassroomConnect: .*-> Connected:' $TimeoutSeconds $null
        $R.ReopenedRole = WaitForLog $ReopenedLog '\[\w+ (Leader|Member)\] Idle -> Connected' 5 $null
        $R.ReopenedSeesTwo = WaitForLog $ReopenedLog 'Connected students \(2\)' 60 $null
        $R.ReopenedInLeadersLevel = WaitForLog $ReopenedLog ('Connected students \(2\): \S+ \(you, [^,]+, marker [^,]+, level ' + (Split-Path $Level -Leaf) + '\)') 60 $null
        $R.LeaderSeesTwo = WaitForLog $Leader.Log 'Connected students \(2\)' 60 $Leader.Process

        $Source = @(ProjectHashes (Split-Path $LeaderProject))
        $Copy = @(ProjectHashes $Installed.DirectoryName)
        $Diff = Compare-Object $Source $Copy -Property Path, Hash
        $R.FilesInLeaderProject = $Source.Count
        $R.FilesInMemberCopy = $Copy.Count
        $R.IdenticalByHash = ($Diff.Count -eq 0)
        if ($Diff) { $R.Differences = ($Diff | Select-Object -First 10 | ForEach-Object { "$($_.SideIndicator) $($_.Path)" }) -join '; ' }
        $R.RecordedRevision = (Get-Content (Join-Path $Installed.DirectoryName 'Saved\ClassroomConnect\Revision.txt') -ErrorAction SilentlyContinue)
    }

    if ($Update -and $R.IdenticalByHash) {
        # Round 2: the leader's project gains one file; a member who already has round 1's copy should download only that.
        Get-Process UnrealEditor-Cmd, UnrealMultiUserSlateServer, UnrealMultiUserServer -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $Started } | Stop-Process -Force
        Start-Sleep -Seconds 3
        $ConfigDir = Join-Path (Split-Path $LeaderProject) 'Config'
        New-Item -ItemType Directory -Force $ConfigDir | Out-Null
        "; changed for the Classroom Connect update test $(Get-Date -Format s)" | Set-Content -Encoding ascii (Join-Path $ConfigDir 'ClassroomCopyUpdate.ini')
        $Leader = StartEditor $LeaderProject @($Level, ('-ExecCmds="UCC.Host ' + $Team + '"')) 'Leader-update.log'
        $R.UpdateLeaderSharing = WaitForLog $Leader.Log 'Sharing ClassroomTest with team|Cannot share|-> Failed:' $TimeoutSeconds $Leader.Process
        $Member = StartEditor (NewProject (Join-Path $Run 'Lobby2') 'Lobby' $false) @(('-ExecCmds="UCC.Join ' + $Team + '"'), ('-UCCPROJECTS="' + $MemberProjects + '"')) 'Lobby2.log'
        $R.UpdatePlan = WaitForLog $Member.Log 'files already on this computer; downloading' $TimeoutSeconds $Member.Process
        $R.UpdateInstalled = WaitForLog $Member.Log 'Installed ClassroomTest revision|Project copy failed|-> Failed:' $TimeoutSeconds $Member.Process
        $Updated = InstalledCopies | Where-Object { $_.DirectoryName -ne $Installed.DirectoryName } | Select-Object -First 1
        if ($Updated) {
            $Diff = Compare-Object @(ProjectHashes (Split-Path $LeaderProject)) @(ProjectHashes $Updated.DirectoryName) -Property Path, Hash
            $R.UpdateIdenticalByHash = ($Diff.Count -eq 0)
            $R.Round1CopyStillThere = Test-Path -LiteralPath $Installed.FullName
        }
    }
} catch {
    $R.Stopped = $_.Exception.Message
} finally {
    Get-Process UnrealEditor-Cmd, UnrealEditor, UnrealMultiUserSlateServer, UnrealMultiUserServer -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $Started } | Stop-Process -Force
}
$R.Logs = $Run
$R | ConvertTo-Json | Set-Content (Join-Path $Run 'results.json')
$R.GetEnumerator() | ForEach-Object { '{0}: {1}' -f $_.Key, $_.Value }
