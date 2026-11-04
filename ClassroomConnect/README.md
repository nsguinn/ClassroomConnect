# Unreal Classroom Connect — developer notes

Building, testing and the internals of the plugin, the optional helper and the scripts. For an overview and install steps, see the [repository README](../README.md).

## Verified on Windows

- Actual installed Unreal metadata: **5.8.4**, changelist **58902052**, compatible changelist **55116800**.
- Concert public headers and the Multi-User server executable are installed.
- The helper compiles with the engine's bundled .NET 10.0.203 SDK and passes **40 executable checks** under Windows PowerShell 5.1 (see [TESTING.md](TESTING.md)).
- **The plugin compiles** with `RunUAT BuildPlugin` against the installed 5.8.4 engine using Visual Studio 2026 (MSVC 14.51.36260, which UBT flags as newer than its preferred 14.50) and Windows SDK 10.0.26100: `BUILD SUCCESSFUL`, no warnings from plugin code (2026-11-03).
- Hosting, joining, presence markers, leader save, project copy and join codes have been exercised with headless editors on one computer ([TESTING.md](TESTING.md)). Between computers, each computer's firewall must accept Unreal's Multi-User traffic: check with `Scripts/Check-ClassroomNetwork.ps1` ([Docs/NETWORK.md](Docs/NETWORK.md)); see [ROADMAP.md](ROADMAP.md).

## Install in a classroom, publish on Fab

- `.\Scripts\Build-Release.ps1` makes `artifacts\release\ClassroomConnect-<version>-UE5.8.4-Win64.zip`: the compiled plugin, a double-click installer per student account (no administrator), `Deploy-ClassroomConnect.ps1` for IT to install on every lab computer at once, the log collector and the instructions. See [Docs/INSTALL.md](Docs/INSTALL.md).
- [Docs/CLASSROOM-TEST.md](Docs/CLASSROOM-TEST.md) is the plan and checklist for a multi-team classroom test.
- `.\Scripts\Package-Fab.ps1 -Seller '<name>'` makes a Fab code-plugin upload and checks that it compiles. See [Docs/FAB.md](Docs/FAB.md).

## Build and test

Open PowerShell in this folder under a normal development account with permission to use Unreal's per-user build folders:

```powershell
.\Scripts\Build-Helper.ps1
.\Scripts\Test-Helper.ps1
.\Scripts\Build-Plugin.ps1
```

The scripts default to `C:\Program Files\Epic Games\UE_5.8`; use `-EngineRoot` to specify another installation. The helper compiler script uses the installed SDK and reference assemblies directly without NuGet or downloads. Output stays under `artifacts`. The packaged helper DLL requires a .NET 10 runtime, provided by this engine installation; it is not self-contained.

## Distribute a project using a folder or an already accessible share

Close editors and stop every writer before publication. Choose new snapshot and student destination paths. The helper does not mount a share or change permissions.

```powershell
.\Scripts\Helper.ps1 publish 'D:\Projects\Demo' 'D:\Baselines\Demo-rev01' Team01 rev01 --quiescent
# Record the printed manifest SHA256 and give students that exact value through a trusted channel.
.\Scripts\Helper.ps1 install 'D:\Baselines\Demo-rev01' 'D:\StudentProjects\Demo-rev01' Team01 rev01 MANIFEST_SHA256
```

Snapshots contain a manifest and `Project/`. Cache, intermediate, binary and source-control folders are excluded. Every installed file must match its recorded SHA256 and size. The exact manifest hash, team and revision are required. Installation stages into a new sibling directory and renames it into place only after verification. Existing destinations are refused. Linked paths and Windows device/path aliases are refused. Store snapshots and staging parents in folders writable only by trusted operators/current users; this is not a defense against another process with permission to rewrite those folders during an operation. Quiescence is acknowledged by the operator, not enforced by the helper.

Failed operation staging directories remain as diagnostics. A retry creates new staging and recopies all files; verified-file reuse, incremental updates and automatic cleanup are not implemented. Do not open an incomplete staging folder as a project.

## Open a project from the helper and auto-join a team session

```powershell
.\Scripts\Helper.ps1 launch 'C:\Program Files\Epic Games\UE_5.8' 'D:\StudentProjects\Demo-rev01\Demo.uproject' UCC-Red-LAB12 UCC-Red --dry-run
```

`launch` checks for engine 5.8.4, an existing `.uproject`, and team-matched server/session names (letters, digits, `-`, `_` only), then starts `UnrealEditor.exe` directly (no shell) with Unreal's own `-CONCERTAUTOCONNECT -CONCERTSERVER= -CONCERTSESSION=` flags. `--dry-run` prints the command instead of starting it. Unreal's auto-connect *creates* the session if it is missing, so launch only once the team session exists.

## Editor workflow

Install the packaged `ClassroomConnect` plugin (engine `Plugins/Marketplace` or a project's `Plugins` folder) and enable it; it enables Multi-User Editing. Open **Classroom** on the level editor toolbar or **Tools → Classroom Connect**.

1. Click your team's color tile.
2. Choose **Team Leader** or **Team Member**.
3. Click **HOST PROJECT** (leader) or **JOIN TEAM** (member).

The leader's editor checks that no other computer is hosting that team, starts this computer's Multi-User server as `UCC-<Team>-<COMPUTERNAME>`, then creates or rejoins session `UCC-<Team>`, and starts sharing its project with teammates. A member's editor finds the one server for that team and the session, then makes sure it has the leader's exact project:

- **Up to date** (same files, checked by SHA-256): it joins straight away.
- **Missing or outdated** (for example a student who opened any other project, or a new class day): it copies the leader's project over the same network connection Multi-User uses (no shared folder, no new ports), reusing every identical file already on the computer and downloading only the rest, verifies every file, and installs it into a new folder `Documents\Unreal Projects\ClassroomConnect\<Team>\<revision>\<Project>`. Nothing existing is overwritten. It then opens that project in a new Unreal window, in the level the leader has open, which joins the session by itself, and closes the old window.

Members never create sessions. If the leader's computer runs an older Classroom Connect that does not share projects, a member whose open project has the same name joins as before; otherwise it explains what to do. Each student gets their own marker color (Orange, Sky Blue, Lime, Violet, …; no two students in a session share one). Teammates appear in your viewport as Unreal's Multi-User presence avatar in their color with their name above it, whenever you have the same saved level open. Because that avatar is small in large maps, Classroom Connect also draws a large marker over each teammate: a downward arrow in their color with their name, turned toward your camera and growing with distance (up to 30×) so it stays readable across an Open World map. It can be switched off with **Show large name markers above teammates**. The markers are local to your editor, never saved, and not selectable. **Connected Students** lists everyone with their color, computer and level, and **Go To** moves your view to just behind them. The status panel shows the team, project, host computer and plain-language errors; **View Diagnostics** shows engine, discovery and server details.

While connected, edits live in Multi-User's session copy, not in the project files. For the **Team Leader**, Leave Team, End Session and closing Unreal all save every changed asset and write the session's work into the leader's project before leaving, with no "persist the changes?" question (whose **No** would throw the work away). If that fails, the leader stays connected and nothing is discarded. **End Session** then stops this computer's Multi-User server. Team Members leave through Unreal's own prompt; the leader's project holds the team's work. Levels must be saved before they are shared: Multi-User ignores never-saved levels.

Team colors and names come from `Config/ClassroomTeams.json` in the plugin folder if present (see `Config/ClassroomTeams.example.json`), otherwise 20 built-in teams. The last team and role are remembered per Windows user. Teams on their own are labels, not access controls; a leader can add an optional **join code**, and then only editors with the same code can discover the team's Multi-User server (Unreal's own `AuthorizedClientKeys` check, set per launch) or copy the project. The code is never saved to disk, and it is not encryption (see the [user guide](Docs/USER-GUIDE.md#join-code-optional)). No firewall, transport, network policy or authentication configuration is changed. If discovery finds no servers, multicast may be blocked; Unreal's UdpMessaging static endpoints are the supported fallback (see [Docs/CONCERT-5.8.4.md](Docs/CONCERT-5.8.4.md)).

## Remaining work

- Validate hosting and joining with two real editor instances on an authorized network, and record which asset edits synchronise.
- Connect helper distribution to the editor workflow: when Join finds a different project, install the leader's baseline and relaunch with `launch`.
- Decide whether End Session should call `PersistAllSessionChanges()` so the leader cannot lose session work by answering No.
- Add authenticated HTTPS distribution, interrupted-transfer reuse and incremental installation if required.
- Validate persistence/recovery, Windows SMB and disconnections, four students per team, and 20 teams. Independent access isolation requires a separate design and acceptance test.

No real classroom collaboration, recovery, load, SMB, active-editor snapshot consistency or plugin loading has been validated. This is an initial reconstruction, not a finished replacement for the earlier project.
