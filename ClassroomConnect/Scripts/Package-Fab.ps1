[CmdletBinding()]
param(
    # Name that appears in the copyright line of every source file and as CreatedBy, e.g. your name or studio.
    [Parameter(Mandatory = $true)][string]$Seller,
    [string]$SellerUrl = 'https://github.com/nsguinn',
    [string]$DocsUrl = 'https://github.com/nsguinn/ClassroomConnect/blob/main/ClassroomConnect/Docs/USER-GUIDE.md',
    [string]$SupportUrl = 'https://github.com/nsguinn/ClassroomConnect/issues',
    # Fab adds the listing link after the listing exists; pass it here for later versions (com.epicgames.launcher://ue/Fab/product/...).
    [string]$FabUrl,
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    # Skip compiling the packaged source with RunUAT BuildPlugin and loading the result in a clean project.
    [switch]$SkipVerifyBuild
)
# Makes a Fab code-plugin upload: a zip with one plugin folder holding the .uplugin, Source, Config, Content and Resources
# (Epic's Fab structure requirements), a PlatformAllowList in every module (Fab's plugin compilation requirements), the
# user guide as Documentation/README.md, without Binaries or Intermediate, with "Installed": false,
# Config/FilterPlugin.ini, Resources/Icon128.png and a copyright line in every source file (checked by common Fab
# pre-submission validators). Then compiles the zipped source and loads the result in a clean project.
# The repository's own files are not modified. Upload the zip to a file host and give Fab the link.
$ErrorActionPreference = 'Stop'
$ProjectRoot = Split-Path $PSScriptRoot -Parent
$Source = Join-Path $ProjectRoot 'Plugins/ClassroomConnect'
$Work = Join-Path $ProjectRoot ('artifacts/fab-' + [Guid]::NewGuid().ToString('N').Substring(0, 6))
$Plugin = Join-Path $Work 'ClassroomConnect'
New-Item -ItemType Directory -Force $Plugin | Out-Null
foreach ($Item in 'ClassroomConnect.uplugin', 'Source', 'Config', 'Resources') { Copy-Item -Recurse (Join-Path $Source $Item) $Plugin }
New-Item -ItemType Directory -Force (Join-Path $Plugin 'Content'), (Join-Path $Plugin 'Documentation') | Out-Null
Copy-Item (Join-Path $ProjectRoot 'Docs/USER-GUIDE.md') (Join-Path $Plugin 'Documentation/README.md')
foreach ($Required in 'Config/FilterPlugin.ini', 'Resources/Icon128.png', 'Documentation/README.md') { if (!(Test-Path (Join-Path $Plugin $Required))) { throw "Missing $Required" } }
if (!(Select-String -Path (Join-Path $Plugin 'Config/FilterPlugin.ini') -Pattern '^/Documentation/' -Quiet)) { throw 'Config/FilterPlugin.ini must list /Documentation/... so builds keep the guide.' }

$Year = (Get-Date).Year
$Notice = "// Copyright $Year $Seller. All Rights Reserved."
foreach ($File in Get-ChildItem (Join-Path $Plugin 'Source') -Recurse -Include *.h, *.cpp, *.cs) {
    $Text = [IO.File]::ReadAllText($File.FullName)
    if (!$Text.StartsWith('// Copyright')) { [IO.File]::WriteAllText($File.FullName, "$Notice`n`n$Text", (New-Object Text.UTF8Encoding($false))) }
}

$Descriptor = Get-Content (Join-Path $Plugin 'ClassroomConnect.uplugin') -Raw | ConvertFrom-Json
$Fields = [ordered]@{ EngineVersion = '5.8.0'; Installed = $false; CanContainContent = $true; CreatedBy = $Seller; CreatedByURL = $SellerUrl; DocsURL = $DocsUrl; SupportURL = $SupportUrl }
if ($FabUrl) { $Fields.FabURL = $FabUrl }
foreach ($Field in $Fields.GetEnumerator()) { $Descriptor | Add-Member -NotePropertyName $Field.Key -NotePropertyValue $Field.Value -Force }
foreach ($Module in $Descriptor.Modules) { if (!$Module.PlatformAllowList -and !$Module.PlatformDenyList) { throw "Module $($Module.Name) needs a PlatformAllowList (Fab plugin compilation requirement)." } }
[IO.File]::WriteAllText((Join-Path $Plugin 'ClassroomConnect.uplugin'), ($Descriptor | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false)))

$Zip = Join-Path $ProjectRoot "artifacts/ClassroomConnect-$($Descriptor.VersionName)-UE5.8-Fab.zip"
if (Test-Path $Zip) { Remove-Item -LiteralPath $Zip -Force }
# Not Compress-Archive: in Windows PowerShell 5.1 it stores paths with backslashes, which zip tools outside Windows
# read as file names rather than folders. Store standard '/' paths, and keep the empty Content folder.
Add-Type -AssemblyName System.IO.Compression, System.IO.Compression.FileSystem
$Base = Split-Path $Plugin -Parent
$Archive = [IO.Compression.ZipFile]::Open($Zip, [IO.Compression.ZipArchiveMode]::Create)
try {
    foreach ($Item in Get-ChildItem -LiteralPath $Plugin -Recurse -Force) {
        $Entry = $Item.FullName.Substring($Base.Length + 1).Replace('\', '/')
        if ($Item.PSIsContainer) { if (!(Get-ChildItem -LiteralPath $Item.FullName -Force)) { [void]$Archive.CreateEntry("$Entry/") } }
        else { [void][IO.Compression.ZipFileExtensions]::CreateEntryFromFile($Archive, $Item.FullName, $Entry, [IO.Compression.CompressionLevel]::Optimal) }
    }
} finally { $Archive.Dispose() }
$Check = [IO.Compression.ZipFile]::OpenRead($Zip)
try { $Bad = @($Check.Entries | Where-Object { $_.FullName.Contains('\') -or !$_.FullName.StartsWith('ClassroomConnect/') }) } finally { $Check.Dispose() }
if ($Bad.Count -gt 0) { throw "Zip entries must be ClassroomConnect/... with '/' separators: $($Bad[0].FullName)" }
Write-Output "Built $Zip"

if (!$SkipVerifyBuild) {
    # Compile exactly what is in the zip, as Fab will.
    $Check = Join-Path $Work 'VerifyBuild'
    & (Join-Path $EngineRoot 'Engine/Build/BatchFiles/RunUAT.bat') BuildPlugin "-Plugin=$Plugin\ClassroomConnect.uplugin" "-Package=$Check" -TargetPlatforms=Win64 -Rocket | Out-Null
    if ($LASTEXITCODE -ne 0) { throw "The Fab package does not compile (RunUAT exit $LASTEXITCODE). Logs: $Check" }
    Write-Output 'Verified: the packaged source compiles with RunUAT BuildPlugin (Win64).'

    # Install the compiled result into a clean project and start a headless editor, as a buyer would.
    $LoadProject = Join-Path $Work 'CleanProject'
    New-Item -ItemType Directory -Force (Join-Path $LoadProject 'Plugins') | Out-Null
    Copy-Item -Recurse $Check (Join-Path $LoadProject 'Plugins/ClassroomConnect')
    '{"FileVersion":3,"EngineAssociation":"5.8","Plugins":[{"Name":"ClassroomConnect","Enabled":true}]}' | Set-Content -Encoding ascii (Join-Path $LoadProject 'CleanProject.uproject')
    $Log = Join-Path $Work 'clean-project.log'
    $EditorArgs = @(('"' + (Join-Path $LoadProject 'CleanProject.uproject') + '"'), '-nullrhi', '-unattended', '-nosplash', '-nosound', '-NoLiveCoding', '-ExecCmds="UCC.Status,QUIT_EDITOR"', ('-abslog="' + $Log + '"'))
    $Editor = Start-Process -FilePath (Join-Path $EngineRoot 'Engine/Binaries/Win64/UnrealEditor-Cmd.exe') -ArgumentList $EditorArgs -PassThru -WindowStyle Hidden
    if (!$Editor.WaitForExit(600000)) { $Editor.Kill(); throw "Clean-project load test timed out. Log: $Log" }
    if (!(Select-String -Path $Log -Pattern 'Classroom Connect loaded with \d+ teams' -Quiet) -or !(Select-String -Path $Log -Pattern 'LogClassroomConnect: Display: Engine:' -Quiet)) {
        throw "The compiled package did not load in a clean project. Log: $Log"
    }
    Write-Output "Verified: the compiled package loads in a clean project and UCC.Status runs. Log: $Log"
    if (!(Test-Path (Join-Path $Check 'Documentation/README.md'))) { Write-Warning 'The compiled package dropped Documentation/README.md; check Config/FilterPlugin.ini.' }
}
Write-Output "Files: $((Get-ChildItem $Plugin -Recurse -File).Count); no Binaries or Intermediate: $(!(Test-Path (Join-Path $Plugin 'Binaries')) -and !(Test-Path (Join-Path $Plugin 'Intermediate')))"
