[CmdletBinding()]
param([string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8')
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$Dotnet = Join-Path $EngineRoot 'Engine/Binaries/ThirdParty/DotNet/10.0/win-x64/dotnet.exe'
$Helper = Join-Path $ProjectRoot 'artifacts/helper/ClassroomConnectHelper.dll'
& (Join-Path $PSScriptRoot 'Build-Helper.ps1') -EngineRoot $EngineRoot
$Fixture = Join-Path $ProjectRoot ('artifacts/tests-' + [Guid]::NewGuid().ToString('N'))
$Project = Join-Path $Fixture 'Source'
New-Item -ItemType Directory -Force (Join-Path $Project 'Content'),(Join-Path $Project 'Saved') | Out-Null
'{"FileVersion":3,"EngineAssociation":"5.8"}' | Set-Content (Join-Path $Project 'Demo.uproject')
[IO.File]::WriteAllBytes((Join-Path $Project 'Content/asset.bin'), [byte[]](0..255))
'private-cache' | Set-Content (Join-Path $Project 'Saved/ignored.txt')
$Snapshot = Join-Path $Fixture 'Snapshot'
$script:Passed = 0
function Check([string]$Name, [bool]$Condition) { if (!$Condition) { throw "FAIL: $Name" }; $script:Passed++; Write-Output "PASS: $Name" }
function Run([string[]]$CliArgs, [bool]$Success) {
    # Windows PowerShell 5.1 turns native stderr into terminating errors under 'Stop'; expected failures write stderr.
    $ErrorActionPreference = 'Continue'
    & $Dotnet $Helper @CliArgs 2>&1 | ForEach-Object { "$_" } | Out-Host
    $Code = $LASTEXITCODE
    $ErrorActionPreference = 'Stop'
    Check ($CliArgs[0] + ' exit status') (($Code -eq 0) -eq $Success)
}
Run @('publish',$Project,$Snapshot,'Team01','rev01','--quiescent') $true
$ManifestPath = Join-Path $Snapshot 'manifest.json'
$Original = [IO.File]::ReadAllBytes($ManifestPath)
$Pin = (Get-FileHash $ManifestPath -Algorithm SHA256).Hash
Check 'saved cache excluded' (!(Test-Path (Join-Path $Snapshot 'Project/Saved')))
$Destination = Join-Path $Fixture 'Student'
Run @('install',$Snapshot,$Destination,'Team01','rev01',$Pin) $true
Check 'installed asset matches source' ((Get-FileHash (Join-Path $Destination 'Content/asset.bin')).Hash -eq (Get-FileHash (Join-Path $Project 'Content/asset.bin')).Hash)
Run @('install',$Snapshot,$Destination,'Team01','rev01',$Pin) $false
Check 'existing project preserved' ((Get-FileHash (Join-Path $Destination 'Content/asset.bin')).Hash -eq (Get-FileHash (Join-Path $Project 'Content/asset.bin')).Hash)
Run @('install',$Snapshot,(Join-Path $Fixture 'WrongTeam'),'Team02','rev01',$Pin) $false
Run @('install',$Snapshot,(Join-Path $Fixture 'WrongRevision'),'Team01','rev02',$Pin) $false
Run @('install',$Snapshot,(Join-Path $Fixture 'WrongPin'),'Team01','rev01',('0'*64)) $false
Run @('install',$Snapshot,(Join-Path $Snapshot 'Nested'),'Team01','rev01',$Pin) $false
Run @('publish',$Project,(Join-Path $Project 'Nested'),'Team01','rev01','--quiescent') $false
Run @('publish',$Project,$Snapshot,'Team01','rev01','--quiescent') $false
Run @('publish',$Project,(Join-Path $Fixture 'NoAcknowledgement'),'Team01','rev01') $false
$Asset = Join-Path $Snapshot 'Project/Content/asset.bin'
[IO.File]::WriteAllBytes($Asset,[byte[]](255..0))
$CorruptTarget = Join-Path $Fixture 'Corrupt'
Run @('install',$Snapshot,$CorruptTarget,'Team01','rev01',$Pin) $false
Check 'corrupt project not installed' (!(Test-Path $CorruptTarget))
[IO.File]::WriteAllBytes($Asset,[byte[]](0..255))
Run @('install',$Snapshot,(Join-Path $Fixture 'Retry'),'Team01','rev01',$Pin) $true
foreach ($BadPath in @('Project/../escape.uproject','Project/CON.uproject','Project/folder./escape.uproject','Project/C:/escape.uproject','Project/asset:stream','Project/..\escape.uproject')) {
    $M = [Text.Encoding]::UTF8.GetString($Original) | ConvertFrom-Json
    $M.Files[0].Path = $BadPath
    $M | ConvertTo-Json -Depth 8 | Set-Content $ManifestPath
    $BadPin = (Get-FileHash $ManifestPath).Hash
    Run @('install',$Snapshot,(Join-Path $Fixture ([Guid]::NewGuid().ToString('N'))),'Team01','rev01',$BadPin) $false
}
foreach ($Payload in @('null','{}','{"Format":1,"Team":"Team01","Revision":"rev01","Files":null}','{invalid')) {
    $Payload | Set-Content $ManifestPath
    Run @('install',$Snapshot,(Join-Path $Fixture ([Guid]::NewGuid().ToString('N'))),'Team01','rev01',(Get-FileHash $ManifestPath).Hash) $false
}
$M = [Text.Encoding]::UTF8.GetString($Original) | ConvertFrom-Json
$M.Files += $M.Files[0]
$M | ConvertTo-Json -Depth 8 | Set-Content $ManifestPath
Run @('install',$Snapshot,(Join-Path $Fixture 'Duplicate'),'Team01','rev01',(Get-FileHash $ManifestPath).Hash) $false
[IO.File]::WriteAllText($ManifestPath,(' '* (8*1024*1024+1)))
Run @('install',$Snapshot,(Join-Path $Fixture 'Oversize'),'Team01','rev01',(Get-FileHash $ManifestPath).Hash) $false
[IO.File]::WriteAllBytes($ManifestPath,$Original)
Check 'snapshot manifest restored' ((Get-FileHash $ManifestPath).Hash -eq $Pin)

# launch: a fake engine folder lets the editor command line be verified without starting Unreal.
function FakeEngine([string]$Name, [int]$Patch) {
    $Root = Join-Path $Fixture $Name
    New-Item -ItemType Directory -Force (Join-Path $Root 'Engine/Build'),(Join-Path $Root 'Engine/Binaries/Win64') | Out-Null
    ('{"MajorVersion":5,"MinorVersion":8,"PatchVersion":' + $Patch + ',"Changelist":58902052}') | Set-Content (Join-Path $Root 'Engine/Build/Build.version')
    'not a real editor' | Set-Content (Join-Path $Root 'Engine/Binaries/Win64/UnrealEditor.exe')
    $Root
}
function DryRun([string]$Engine, [string]$ProjectFile, [string]$Server, [string]$Session) {
    $ErrorActionPreference = 'Continue'
    $Output = & $Dotnet $Helper launch $Engine $ProjectFile $Server $Session --dry-run 2>$null
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE -ne 0) { return $null }
    ($Output -join "`n") | ConvertFrom-Json
}
$Engine584 = FakeEngine 'Engine584' 4
$Engine583 = FakeEngine 'Engine583' 3
$StudentProject = Join-Path $Destination 'Demo.uproject'
$Plan = DryRun $Engine584 $StudentProject 'UCC-Red-LAB01' 'UCC-Red'
Check 'launch dry run succeeds' ($null -ne $Plan)
Check 'launch uses engine editor' ($Plan.Editor -eq (Join-Path $Engine584 'Engine\Binaries\Win64\UnrealEditor.exe'))
Check 'launch opens project first' ($Plan.Arguments[0] -eq $StudentProject)
Check 'launch requests auto-connect' (($Plan.Arguments -contains '-CONCERTAUTOCONNECT') -and ($Plan.Arguments -contains '-CONCERTSERVER=UCC-Red-LAB01') -and ($Plan.Arguments -contains '-CONCERTSESSION=UCC-Red') -and $Plan.Arguments.Count -eq 4)
Check 'launch refuses other engine versions' ($null -eq (DryRun $Engine583 $StudentProject 'UCC-Red-LAB01' 'UCC-Red'))
Check 'launch refuses cross-team server' ($null -eq (DryRun $Engine584 $StudentProject 'UCC-Blue-LAB01' 'UCC-Red'))
Check 'launch refuses argument injection' ($null -eq (DryRun $Engine584 $StudentProject 'UCC-Red-LAB01 -ExecCmds=quit' 'UCC-Red'))
Check 'launch refuses non-project file' ($null -eq (DryRun $Engine584 (Join-Path $Destination 'Content/asset.bin') 'UCC-Red-LAB01' 'UCC-Red'))
Check 'launch refuses missing project' ($null -eq (DryRun $Engine584 (Join-Path $Fixture 'Missing/Demo.uproject') 'UCC-Red-LAB01' 'UCC-Red'))
Check 'launch refuses non-engine folder' ($null -eq (DryRun $Fixture $StudentProject 'UCC-Red-LAB01' 'UCC-Red'))
if (Test-Path (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor.exe')) {
    $RealPlan = DryRun $EngineRoot $StudentProject 'UCC-Red-LAB01' 'UCC-Red'
    Check 'launch resolves installed 5.8.4 editor' (($null -ne $RealPlan) -and (Test-Path $RealPlan.Editor))
}
$Summary = @{ Passed = $script:Passed; Failed = 0; Fixture = $Fixture; Platform = [Environment]::OSVersion.ToString(); EngineRoot = $EngineRoot }
$Summary | ConvertTo-Json | Set-Content (Join-Path $Fixture 'results.json')
Write-Output "Completed: $script:Passed checks passed. Fixtures retained at $Fixture"
