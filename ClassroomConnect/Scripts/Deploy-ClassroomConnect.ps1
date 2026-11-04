[CmdletBinding(PositionalBinding = $false)]
param(
    # Lab computer names, e.g. (Get-Content .\lab-computers.txt), or "PC01,PC02" when called through powershell -File
    [Parameter(Mandatory = $true)][string[]]$ComputerName,
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    [string]$PluginSource,
    # Report what would happen on each computer without copying anything.
    [switch]$CheckOnly
)
# For IT: installs Classroom Connect into the engine on many lab computers at once (Engine scope: enabled in every project
# for every account). Copies through each computer's administrative share (\\PC\C$), so run it as an account that is an
# administrator on the targets, from any computer on the network. Uses no remote execution and changes nothing else.
# Computers that are off, unreachable, on another engine build, or have Unreal open are reported and skipped.
$ErrorActionPreference = 'Stop'
# Windows PowerShell 5.1 leaves $PSScriptRoot empty in param() defaults, so resolve it here.
if (!$PluginSource) { $PluginSource = Join-Path $PSScriptRoot 'ClassroomConnect' }
$Modules = Join-Path $PluginSource 'Binaries\Win64\UnrealEditor.modules'
if (!(Test-Path -LiteralPath (Join-Path $PluginSource 'ClassroomConnect.uplugin')) -or !(Test-Path -LiteralPath $Modules)) { throw "$PluginSource is not a packaged Classroom Connect plugin." }
$PluginBuildId = (Get-Content -LiteralPath $Modules -Raw | ConvertFrom-Json).BuildId
$Drive = $EngineRoot.Substring(0, 1)
$EngineRelative = $EngineRoot.Substring(2).TrimStart('\')

$Results = foreach ($Computer in $ComputerName | ForEach-Object { $_ -split '[,;\s]+' } | Where-Object { $_ } | Select-Object -Unique) {
    $Result = [ordered]@{ Computer = $Computer; Status = ''; Detail = '' }
    try {
        $RemoteEngine = "\\$Computer\$Drive`$\$EngineRelative"
        $VersionFile = Join-Path $RemoteEngine 'Engine\Build\Build.version'
        if (!(Test-Path -LiteralPath $VersionFile)) { throw "cannot reach $VersionFile (computer off, no admin share access, or engine elsewhere)" }
        $V = Get-Content -LiteralPath $VersionFile -Raw | ConvertFrom-Json
        if ("$($V.MajorVersion).$($V.MinorVersion).$($V.PatchVersion)" -ne '5.8.4') { throw "engine is $($V.MajorVersion).$($V.MinorVersion).$($V.PatchVersion), not 5.8.4" }
        $EngineBuildId = (Get-Content -LiteralPath (Join-Path $RemoteEngine 'Engine\Binaries\Win64\UnrealEditor.modules') -Raw | ConvertFrom-Json).BuildId
        if ($EngineBuildId -ne $PluginBuildId) { throw "engine build $EngineBuildId does not match the package ($PluginBuildId)" }
        $Target = Join-Path $RemoteEngine 'Engine\Plugins\Marketplace\ClassroomConnect'
        if ($CheckOnly) { $Result.Status = 'Ready'; $Result.Detail = "would install to $Target"; [pscustomobject]$Result; continue }

        # /MIR keeps the target an exact copy of the package (it only touches the ClassroomConnect folder). A locked DLL means Unreal is open.
        $Log = robocopy $PluginSource $Target /MIR /XD Intermediate /R:1 /W:2 /NP /NFL /NDL /NJH /NJS
        if ($LASTEXITCODE -ge 8) { throw "copy failed (robocopy $LASTEXITCODE); is Unreal open on $Computer? $($Log -join ' ')" }
        $DescriptorPath = Join-Path $Target 'ClassroomConnect.uplugin'
        $Descriptor = Get-Content -LiteralPath $DescriptorPath -Raw | ConvertFrom-Json
        $Descriptor | Add-Member -NotePropertyName EnabledByDefault -NotePropertyValue $true -Force
        [IO.File]::WriteAllText($DescriptorPath, ($Descriptor | ConvertTo-Json -Depth 8), (New-Object Text.UTF8Encoding($false)))
        $Result.Status = 'Installed'; $Result.Detail = "$($Descriptor.VersionName) in $Target"
    }
    catch { $Result.Status = 'Skipped'; $Result.Detail = $_.Exception.Message }
    [pscustomobject]$Result
}

$Results | Format-Table -AutoSize | Out-String -Width 220 | Write-Host
$Csv = Join-Path (Get-Location) ('ClassroomConnect-deploy-' + (Get-Date -Format 'yyyyMMdd-HHmm') + '.csv')
$Results | Export-Csv -NoTypeInformation -Path $Csv
Write-Host "$(@($Results | Where-Object Status -eq 'Installed').Count) installed, $(@($Results | Where-Object Status -eq 'Ready').Count) ready, $(@($Results | Where-Object Status -eq 'Skipped').Count) skipped. Results saved to $Csv"
