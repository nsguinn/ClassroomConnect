[CmdletBinding()]
param(
    [string]$Team = 'Teal',
    # The leader's code, typed with a dash and lower case on purpose: codes are compared after removing those.
    [string]$Code = 'ab-12cd',
    # The right code as a member might type it.
    [string]$MemberCode = 'AB12CD',
    [string]$WrongCode = 'WRONG1',
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [int]$TimeoutSeconds = 300
)
# Join code check on one computer, with headless editors:
#   leader          - ClassroomTest, runs UCC.Host <Team> <Code>: its Multi-User server is started with the code's key
#   member, no code - Lobby, UCC.Join <Team>: must not discover the leader's server and must be told a code is needed
#   member, wrong   - Lobby, UCC.Join <Team> <WrongCode>: same, told the code does not match
#   member, right   - Lobby, UCC.Join <Team> <MemberCode>: copies ClassroomTest, reopens on the copy carrying the code,
#                     and the reopened editor joins the session; the copy is compared with the leader's files by SHA-256
# Stops only the editors and Multi-User servers started during the test.
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$Editor = Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe'
$Package = Get-ChildItem (Join-Path $ProjectRoot 'artifacts') -Directory -Filter 'p*' | Where-Object { Test-Path (Join-Path $_.FullName 'Plugin/Binaries/Win64') } | Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (!$Package) { throw 'No packaged plugin found. Run Build-Plugin.ps1 first.' }
$Running = Get-Process UnrealMultiUserSlateServer, UnrealMultiUserServer -ErrorAction SilentlyContinue
if ($Running) { throw "A Multi-User server is already running on this computer (process $($Running[0].Id)). Stop it first; this test starts its own." }
$Run = Join-Path $ProjectRoot ('artifacts/j' + [Guid]::NewGuid().ToString('N').Substring(0, 8))
$MemberProjects = Join-Path $Run 'MemberProjects'

function NewProject([string]$Dir, [string]$Name) {
    New-Item -ItemType Directory -Force (Join-Path $Dir 'Plugins') | Out-Null
    Copy-Item -Recurse (Join-Path $Package.FullName 'Plugin') (Join-Path $Dir 'Plugins/ClassroomConnect')
    '{"FileVersion":3,"EngineAssociation":"5.8","Plugins":[{"Name":"ClassroomConnect","Enabled":true}]}' | Set-Content -Encoding ascii (Join-Path $Dir "$Name.uproject")
    Join-Path $Dir "$Name.uproject"
}
function StartEditor([string]$ProjectFile, [string[]]$Extra, [string]$LogName) {
    # Logs go next to the project folder, not inside it: a locked log inside the project would be part of what is shared.
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
function Discovered([string]$Log) {
    # Concert logs every server it discovers; a member without the right key must never see the leader's server.
    [bool](Select-String -Path $Log -Pattern ('LogConcert: .*Server UCC-' + $Team + '-\S+ discovered') -Quiet)
}
function ProjectHashes([string]$Dir, [string]$Root = $Dir) {
    foreach ($Item in Get-ChildItem -LiteralPath $Dir) {
        if ($Item.PSIsContainer) { if ($Item.Name -notin 'Saved', 'Intermediate', 'DerivedDataCache', '.git', '.vs', '.idea') { ProjectHashes $Item.FullName $Root } }
        elseif ($Item.Extension -ne '.pdb') { [pscustomobject]@{ Path = $Item.FullName.Substring($Root.Length + 1); Hash = (Get-FileHash -LiteralPath $Item.FullName -Algorithm SHA256).Hash } }
    }
}
function TryMember([string]$Name, [string]$JoinCommand) {
    $Member = StartEditor (NewProject (Join-Path $Run $Name) 'Lobby') @(('-ExecCmds="' + $JoinCommand + '"'), ('-UCCPROJECTS="' + $MemberProjects + '"')) "$Name.log"
    $Result = WaitForLog $Member.Log '-> Failed:|offers ClassroomTest' 180 $Member.Process
    if (!$Member.Process.HasExited) { Stop-Process -Id $Member.Process.Id -Force }
    [pscustomobject]@{ Result = $Result; DiscoveredLeader = (Discovered $Member.Log) }
}

$Started = Get-Date
$R = [ordered]@{ Package = $Package.Name; Team = $Team; LeaderCode = $Code }
try {
    $LeaderProject = NewProject (Join-Path $Run 'Leader') 'ClassroomTest'
    $Leader = StartEditor $LeaderProject @(('-ExecCmds="UCC.Host ' + $Team + ' ' + $Code + '"')) 'Leader.log'
    $R.LeaderServer = WaitForLog $Leader.Log 'Started the team server with a join code|Started the team server' 120 $Leader.Process
    $R.LeaderConnected = WaitForLog $Leader.Log '-> Connected:|-> Failed:' $TimeoutSeconds $Leader.Process
    $R.LeaderSharing = WaitForLog $Leader.Log 'Sharing ClassroomTest with team|Cannot share' 120 $Leader.Process
    if ($R.LeaderConnected -notmatch '-> Connected' -or $R.LeaderSharing -notmatch 'Sharing') { throw 'Leader did not host and share; see the leader log.' }

    $NoCode = TryMember 'NoCode' "UCC.Join $Team"
    $R.NoCodeResult = $NoCode.Result
    $R.NoCodeDiscoveredLeaderServer = $NoCode.DiscoveredLeader
    $R.NoCodePassed = ($NoCode.Result -match '-> Failed:.*set a join code') -and !$NoCode.DiscoveredLeader

    $Wrong = TryMember 'WrongCode' "UCC.Join $Team $WrongCode"
    $R.WrongCodeResult = $Wrong.Result
    $R.WrongCodeDiscoveredLeaderServer = $Wrong.DiscoveredLeader
    $R.WrongCodePassed = ($Wrong.Result -match '-> Failed:.*does not match') -and !$Wrong.DiscoveredLeader

    $Member = StartEditor (NewProject (Join-Path $Run 'RightCode') 'Lobby') @(('-ExecCmds="UCC.Join ' + $Team + ' ' + $MemberCode + '"'), ('-UCCPROJECTS="' + $MemberProjects + '"')) 'RightCode.log'
    $R.RightCodeOffer = WaitForLog $Member.Log 'offers ClassroomTest|-> Failed:' $TimeoutSeconds $Member.Process
    $R.RightCodeDiscoveredLeaderServer = Discovered $Member.Log
    $R.RightCodeInstalled = WaitForLog $Member.Log 'Installed ClassroomTest revision|Project copy failed|-> Failed:|Your copy of the team project is up to date' $TimeoutSeconds $Member.Process
    $R.RightCodeReopened = WaitForLog $Member.Log 'Opened .* in a new editor|-> Failed:' 60 $Member.Process
    $Installed = Get-ChildItem -LiteralPath (Join-Path $MemberProjects $Team) -Directory -ErrorAction SilentlyContinue | Where-Object { $_.Name -notlike '*.partial' } |
        ForEach-Object { Get-Item -LiteralPath (Join-Path $_.FullName 'ClassroomTest\ClassroomTest.uproject') -ErrorAction SilentlyContinue } | Select-Object -First 1
    if ($Installed) {
        $ReopenedLog = Join-Path $Installed.DirectoryName 'Saved\Logs\ClassroomTest.log'
        $R.ReopenedSendsCode = WaitForLog $ReopenedLog "discovery now sends this editor's join code" 120 $null
        $R.ReopenedJoined = WaitForLog $ReopenedLog 'LogClassroomConnect: .*-> Connected:' $TimeoutSeconds $null
        $R.ReopenedSeesTwo = WaitForLog $ReopenedLog 'Connected students \(2\)' 60 $null
        $R.LeaderSeesTwo = WaitForLog $Leader.Log 'Connected students \(2\)' 60 $Leader.Process
        $Diff = Compare-Object @(ProjectHashes (Split-Path $LeaderProject)) @(ProjectHashes $Installed.DirectoryName) -Property Path, Hash
        $R.CopyIdenticalByHash = ($Diff.Count -eq 0)
    }
    $R.RightCodePassed = ($R.RightCodeDiscoveredLeaderServer -eq $true) -and ($R.ReopenedJoined -match '-> Connected') -and ($R.CopyIdenticalByHash -eq $true)
} catch {
    $R.Stopped = $_.Exception.Message
} finally {
    Get-Process UnrealEditor-Cmd, UnrealEditor, UnrealMultiUserSlateServer, UnrealMultiUserServer -ErrorAction SilentlyContinue | Where-Object { $_.StartTime -ge $Started } | Stop-Process -Force
}
$R.Logs = $Run
$R | ConvertTo-Json | Set-Content (Join-Path $Run 'results.json')
$R.GetEnumerator() | ForEach-Object { '{0}: {1}' -f $_.Key, $_.Value }
