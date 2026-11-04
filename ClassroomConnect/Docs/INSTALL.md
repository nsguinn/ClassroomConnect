# Installing Classroom Connect

The package `ClassroomConnect-<version>-UE5.8.4-Win64.zip` (made by `Scripts\Build-Release.ps1`) installs a compiled plugin for **Unreal Engine 5.8.4 on Windows**. It only loads in an engine with exactly the same build; the installers check this first and refuse otherwise. After installing, every project opened in 5.8.4 on that computer shows a **Classroom** button on the level editor toolbar.

Nothing else is changed: no services, firewall rules, group policies or other plugins.

## Option A: IT installs on all lab computers at once (recommended for 15–20 workstations)

Unzip the package on any computer on the network and run, as an account that is an administrator on the lab computers:

```powershell
# One computer name per line in lab-computers.txt
.\Deploy-ClassroomConnect.ps1 -ComputerName (Get-Content .\lab-computers.txt) -CheckOnly
.\Deploy-ClassroomConnect.ps1 -ComputerName (Get-Content .\lab-computers.txt)
```

`-CheckOnly` reports, without copying, which computers are reachable and have the matching engine build. The real run copies the plugin to `C:\Program Files\Epic Games\UE_5.8\Engine\Plugins\Marketplace\ClassroomConnect` on each computer through its administrative share (`\\PC\C$`) and enables it in every project. Unreal must be closed on the targets. Results are shown and saved to `ClassroomConnect-deploy-<date>.csv`; computers that were off or skipped can simply be run again later.

If the lab uses a deployment tool (SCCM, Intune, PDQ, a startup script), have it run this on each computer instead:

```
powershell -NoProfile -ExecutionPolicy Bypass -File Install-ClassroomConnect.ps1 -Scope Engine
```

## Option B: each student account installs it (no administrator)

Double-click **Install Classroom Connect (this user).cmd**. It copies the plugin to `%LOCALAPPDATA%\ClassroomConnect\Plugins` and adds that folder to the account's `UE_ADDITIONAL_PLUGIN_PATHS` environment variable, which Unreal 5.8.4 reads to load precompiled plugins in every project. Then **quit the Epic Games Launcher completely** (right-click its icon next to the clock → Exit) so it picks up the change, and open the project.

This has to be done once per student account on each computer (`%LOCALAPPDATA%` does not roam between computers).

## Check that it worked

Open any project in 5.8.4: the toolbar shows **Classroom**. For details, open **Window → Output Log** and type `UCC.Status`: it prints the engine build, the Multi-User state and the servers found on the network.

## Update or remove

- Update: install the newer package the same way. The previous version is kept as `ClassroomConnect.previous` next to the new one until the following update.
- Remove for one account: double-click **Uninstall Classroom Connect (this user).cmd**.
- Remove an engine install: delete `…\UE_5.8\Engine\Plugins\Marketplace\ClassroomConnect` (administrator).

## Troubleshooting

| Problem | Cause and fix |
| --- | --- |
| "built for a different Unreal build" | The engine was updated (for example a hotfix). Rebuild the package on an updated machine with `Build-Release.ps1`. |
| No Classroom button after a per-user install | The Epic Games Launcher was still running with the old environment. Exit it completely, or sign out of Windows and back in. |
| The .cmd window says scripts are disabled | A group policy enforces a PowerShell execution policy. Ask IT to run Option A instead. |
| Windows Security asks to allow Unreal or "UnrealMultiUserSlateServer" on the network | Shown the first time a computer hosts. Students usually cannot approve it; IT decides. Do not try to work around it. Same-room discovery may still work; check with the network preflight in CLASSROOM-TEST.md. |
| "Could not find the team leader" | The leader has not hosted yet, or the network blocks Multi-User discovery (UDP multicast). Open **View Diagnostics**: "Servers discovered: 0" on every computer points to the network. |
