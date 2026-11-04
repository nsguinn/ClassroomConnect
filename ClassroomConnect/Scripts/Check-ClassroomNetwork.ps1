[CmdletBinding()]
param(
    # Unreal Engine folder whose programs must receive traffic from other computers.
    [string]$EngineRoot = 'C:\Program Files\Epic Games\UE_5.8',
    # Append one summary row per network profile to this CSV, to collect results from every lab computer in one file.
    [string]$Csv
)
# Read-only check: can Unreal Multi-User Editing traffic from other computers get through this computer's Windows
# Firewall? Changes nothing. Reports which firewall profile applies, whether rules made on this computer count (Group
# Policy or Intune can turn that off), where the rules in force come from, and, for the editor and the Multi-User
# server programs, whether incoming UDP is allowed and by which rule.
# Run it from an administrator PowerShell: Windows lets only administrators read the firewall rules in force (a
# standard account gets "Access is denied"). Nothing is changed either way.
# Exit code: 0 the editor and the Multi-User server can receive UDP; 2 something blocks them; 1 the check failed.
$ErrorActionPreference = 'Stop'

$Programs = [ordered]@{
    'Unreal Editor'                = 'Engine\Binaries\Win64\UnrealEditor.exe'
    'Multi-User server (window)'   = 'Engine\Binaries\Win64\UnrealMultiUserSlateServer.exe'
    'Multi-User server (console)'  = 'Engine\Binaries\Win64\UnrealMultiUserServer.exe'
}
$Required = 'Unreal Editor', 'Multi-User server (window)' # What Classroom Connect starts by default.

function Expand-RulePath([string]$Path) {
    if (!$Path -or $Path -eq 'Any') { return $Path }
    try { return [IO.Path]::GetFullPath([Environment]::ExpandEnvironmentVariables($Path)).ToLowerInvariant() } catch { return $Path.ToLowerInvariant() }
}

function Test-PortIncludes($Ports, [int]$Port) {
    foreach ($Spec in @($Ports)) {
        if ($Spec -eq 'Any' -or $Spec -eq "$Port") { return $true }
        if ($Spec -match '^(\d+)-(\d+)$' -and [int]$Matches[1] -le $Port -and [int]$Matches[2] -ge $Port) { return $true }
    }
    return $false
}

try {
    $Computer = $env:COMPUTERNAME
    $Admin = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

    # Which firewall profile each connected network uses.
    $ProfileOf = @{ DomainAuthenticated = 'Domain'; Private = 'Private'; Public = 'Public' }
    $Connections = @(Get-NetConnectionProfile)
    if ($Connections.Count -eq 0) { throw 'This computer is not connected to a network.' }
    $Addresses = @(Get-NetIPAddress -AddressFamily IPv4 | Where-Object { $_.IPAddress -notlike '127.*' -and $_.IPAddress -notlike '169.254.*' })
    $ActiveProfiles = @($Connections | ForEach-Object { $ProfileOf[$_.NetworkCategory.ToString()] } | Where-Object { $_ } | Select-Object -Unique)

    Write-Output "Computer: $Computer$(if ($Admin) { ' (checked as administrator)' } else { ' (checked as a standard user)' })"
    foreach ($Connection in $Connections) {
        $Ips = @($Addresses | Where-Object { $_.InterfaceAlias -eq $Connection.InterfaceAlias } | ForEach-Object { "$($_.IPAddress)/$($_.PrefixLength)" }) -join ', '
        Write-Output ("Network: {0} {1}, {2} -> firewall profile {3}" -f $Connection.InterfaceAlias, $Ips, $Connection.Name, $ProfileOf[$Connection.NetworkCategory.ToString()])
    }

    # Effective firewall settings and rules (ActiveStore: what is in force, whatever its source).
    $Profiles = @{}
    foreach ($P in Get-NetFirewallProfile -PolicyStore ActiveStore) { $Profiles[$P.Name] = $P }
    $Rules = @(Get-NetFirewallRule -PolicyStore ActiveStore -Direction Inbound -Enabled True)
    $AppFilter = @{}; foreach ($F in Get-NetFirewallApplicationFilter -PolicyStore ActiveStore -All) { $AppFilter[$F.InstanceID] = $F }
    $PortFilter = @{}; foreach ($F in Get-NetFirewallPortFilter -PolicyStore ActiveStore -All) { $PortFilter[$F.InstanceID] = $F }
    $AddressFilter = @{}; foreach ($F in Get-NetFirewallAddressFilter -PolicyStore ActiveStore -All) { $AddressFilter[$F.InstanceID] = $F }
    $ServiceFilter = @{}; foreach ($F in Get-NetFirewallServiceFilter -PolicyStore ActiveStore -All) { $ServiceFilter[$F.InstanceID] = $F }

    $Sources = ($Rules | Group-Object { $_.PolicyStoreSourceType.ToString() } | Sort-Object Count -Descending | ForEach-Object { "$($_.Name) $($_.Count)" }) -join ', '
    Write-Output "Inbound rules in force by source: $Sources (MDM = Intune, GroupPolicy = Group Policy, Local = made on this computer)"

    # UDP rules, reduced to what matters here: program, ports, remote addresses, action and source.
    $Udp = foreach ($Rule in $Rules) {
        $Port = $PortFilter[$Rule.InstanceID]
        if (!$Port -or @('UDP', 'Any', '17') -notcontains "$($Port.Protocol)") { continue }
        $App = $AppFilter[$Rule.InstanceID]
        $Service = $ServiceFilter[$Rule.InstanceID]
        if ($Service -and $Service.Service -and $Service.Service -ne 'Any') { continue } # Applies to one Windows service only.
        if ($App -and $App.Package -and $App.Package -ne 'Any' -and $App.Package -ne '*') { continue } # Applies to one Store app only.
        $Address = $AddressFilter[$Rule.InstanceID]
        [pscustomobject]@{
            Name     = $Rule.DisplayName
            Action   = $Rule.Action.ToString()
            Source   = $Rule.PolicyStoreSourceType.ToString()
            Profiles = $Rule.Profile.ToString()
            Program  = Expand-RulePath $(if ($App) { $App.Program } else { 'Any' })
            Ports    = @($Port.LocalPort)
            Remote   = (@($Address.RemoteAddress) -join ',')
        }
    }

    $Summary = @()
    $AllRequiredAllowed = $true
    foreach ($ProfileName in $ActiveProfiles) {
        $P = $Profiles[$ProfileName]
        $LocalCounts = "$($P.AllowLocalFirewallRules)" -ne 'False'
        $DefaultIn = if ("$($P.DefaultInboundAction)" -eq 'Allow') { 'Allow' } else { 'Block' }
        $FirewallOn = "$($P.Enabled)" -ne 'False'
        Write-Output ''
        Write-Output "== $ProfileName profile"
        Write-Output ("Firewall on: {0}; incoming traffic without a rule: {1}; rules made on this computer count: {2}" -f $FirewallOn, $DefaultIn, $(if ($LocalCounts) { 'yes' } else { 'NO (turned off by Group Policy or Intune)' }))

        $InProfile = @($Udp | Where-Object { $_.Profiles -eq 'Any' -or ($_.Profiles -split ', ') -contains $ProfileName })
        $Enforced = @($InProfile | Where-Object { $_.Source -ne 'Local' -or $LocalCounts })
        $Row = [ordered]@{ Date = (Get-Date).ToString('s'); Computer = $Computer; IPv4 = (@($Addresses | ForEach-Object { $_.IPAddress }) -join ' '); Profile = $ProfileName; FirewallOn = $FirewallOn; DefaultInbound = $DefaultIn; LocalRulesCount = $LocalCounts; RuleSources = $Sources }

        foreach ($Entry in $Programs.GetEnumerator()) {
            $Path = Expand-RulePath (Join-Path $EngineRoot $Entry.Value)
            $Installed = Test-Path $Path
            $ForProgram = @($Enforced | Where-Object { $_.Program -eq $Path })
            $ForAll = @($Enforced | Where-Object { $_.Program -eq 'Any' })
            $Block = @($ForProgram + @($ForAll | Where-Object { Test-PortIncludes $_.Ports 0 }) | Where-Object { $_.Action -eq 'Block' }) | Select-Object -First 1
            $Allow = @($ForProgram + @($ForAll | Where-Object { Test-PortIncludes $_.Ports 0 }) | Where-Object { $_.Action -eq 'Allow' }) | Select-Object -First 1
            $DiscoveryOnly = @($ForAll | Where-Object { $_.Action -eq 'Allow' -and (Test-PortIncludes $_.Ports 6666) }) | Select-Object -First 1
            if (!$FirewallOn) { $Result = 'Allowed'; $Why = 'the firewall is off for this profile' }
            elseif ($Block) { $Result = 'Blocked'; $Why = "rule '$($Block.Name)' ($($Block.Source)) blocks it" }
            elseif ($Allow) { $Result = 'Allowed'; $Why = "rule '$($Allow.Name)' ($($Allow.Source)), ports $($Allow.Ports -join ','), from $($Allow.Remote)" }
            elseif ($DefaultIn -eq 'Allow') { $Result = 'Allowed'; $Why = 'incoming traffic is allowed by default' }
            elseif ($DiscoveryOnly) { $Result = 'Partly'; $Why = "only port 6666 is allowed (rule '$($DiscoveryOnly.Name)'); Multi-User also uses ports Unreal picks at random" }
            else { $Result = 'Blocked'; $Why = 'no rule in force allows it, and incoming traffic is blocked by default' }
            Write-Output ("{0,-28} {1,-8} {2}{3}" -f $Entry.Key, $Result, $Why, $(if ($Installed) { '' } else { ' [not installed here]' }))
            if (!$LocalCounts) {
                foreach ($Ignored in @($InProfile | Where-Object { $_.Source -eq 'Local' -and $_.Program -eq $Path })) {
                    Write-Output ("{0,-28} {1,-8} ignored: local rule '{2}' ({3}), because rules made on this computer do not count" -f '', '', $Ignored.Name, $Ignored.Action)
                }
            }
            $Row[$Entry.Key] = $Result
            if ($Required -contains $Entry.Key -and $Result -ne 'Allowed') { $AllRequiredAllowed = $false }
        }
        $Summary += [pscustomobject]$Row
    }

    $Unreal = @(Get-Process | Where-Object { $_.ProcessName -match '^Unreal(Editor|MultiUser)' })
    if ($Unreal.Count -gt 0) {
        Write-Output ''
        Write-Output '== Unreal programs running now and their UDP ports'
        foreach ($Process in $Unreal) {
            $Ports = @(Get-NetUDPEndpoint -OwningProcess $Process.Id -ErrorAction SilentlyContinue | ForEach-Object { "$($_.LocalAddress):$($_.LocalPort)" } | Select-Object -Unique) -join ', '
            Write-Output "$($Process.ProcessName) (pid $($Process.Id)): $Ports"
        }
    }

    Write-Output ''
    $Managed = $Rules | Where-Object { $_.PolicyStoreSourceType.ToString() -eq 'MDM' } | Select-Object -First 1
    $Owner = if ($Managed) { 'Intune (whoever manages endpoint security firewall rules)' } else { 'Group Policy (whoever manages the firewall GPO)' }
    if ($AllRequiredAllowed) {
        Write-Output 'Result: READY. Incoming UDP to the Unreal editor and Multi-User server is allowed on this computer.'
        Write-Output 'If computers still do not find each other, check that every computer passes this check, then see Docs/NETWORK.md.'
    } else {
        Write-Output 'Result: BLOCKED. Other computers cannot reach Unreal Multi-User Editing on this computer.'
        if (@($ActiveProfiles | Where-Object { "$($Profiles[$_].AllowLocalFirewallRules)" -eq 'False' }).Count -gt 0) {
            Write-Output "Rules made on this computer do not count here, so this cannot be fixed on the computer itself. Ask $Owner for an"
        } else {
            Write-Output 'With IT approval, an administrator could add a rule on this computer; managed computers should get the same rule from IT as'
        }
        Write-Output "inbound Allow rule for UDP, any local port, profile $($ActiveProfiles -join '/'), remote addresses limited to the lab, for:"
        foreach ($Entry in $Programs.GetEnumerator()) { Write-Output "  $(Join-Path $EngineRoot $Entry.Value)" }
        Write-Output 'Details: Docs/NETWORK.md.'
    }

    if ($Csv) {
        $Summary | Export-Csv -Path $Csv -Append -NoTypeInformation -Encoding UTF8
        Write-Output "Appended $($Summary.Count) row(s) to $Csv"
    }
    if ($AllRequiredAllowed) { exit 0 } else { exit 2 }
} catch {
    Write-Output "The check could not run: $($_.Exception.Message)"
    if (!$Admin -and $_.Exception.Message -match 'denied') {
        Write-Output 'Reading the firewall needs an administrator PowerShell (right-click Windows PowerShell > Run as administrator). The check only reads.'
    }
    exit 1
}
