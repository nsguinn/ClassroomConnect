# Testing

Only results actually observed are recorded here. Re-run the commands to reproduce.

## Helper (independent of Unreal)

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Scripts\Test-Helper.ps1
```

Builds the helper with the engine's bundled C# compiler (`/warnaserror+`), then runs CLI checks against synthetic fixtures under `artifacts/tests-*` (kept for inspection).

| Date | Machine | Shell | Result |
| --- | --- | --- | --- |
| 2026-11-03 | Windows 11, UE 5.8.4 CL 58902052 | Windows PowerShell 5.1 | **40 passed, 0 failed** (29 publish/install + 11 launch) |

Covered: publish/install round trip, cache exclusion, refusal to overwrite, team/revision/pin mismatch, nested/overlapping paths, missing quiescence flag, corrupt file, retry after repair, path traversal / device names / ADS / backslash paths in manifests, malformed and oversize manifests, duplicate entries; `launch` command line, engine version gate, cross-team server, argument injection, non-project and missing project, non-engine folder, and resolution of the installed `UnrealEditor.exe` (dry run only; Unreal was not started).

Before 2026-11-03 the script aborted on its first expected failure under Windows PowerShell 5.1 (native stderr became a terminating error); it now runs on 5.1, the default on classroom PCs.

## Plugin

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Scripts\Build-Plugin.ps1
```

Runs `RunUAT BuildPlugin` for Win64 against the installed engine and packages to `artifacts/p<id>/Plugin`. Logs are in `artifacts/p<id>/logs`.

| Date | Toolchain | Result |
| --- | --- | --- |
| 2026-11-03 | VS 2026 Community, MSVC 14.51.36260 (UBT: "not a preferred version"; preferred is 14.50), Windows SDK 10.0.26100 | **BUILD SUCCESSFUL.** Four plugin sources compiled and linked; the only warnings are C4996 deprecations inside engine headers. Two earlier attempts the same day failed for environmental reasons: SDK not yet installed, then output paths over 260 characters (fixed by shortening the package folder). |

### Load test (headless editor)

A throwaway project at `artifacts/LoadTest` with the packaged plugin, run as:

```powershell
& 'C:\Program Files\Epic Games\UE_5.8\Engine\Binaries\Win64\UnrealEditor-Cmd.exe' "<path>\LoadTest.uproject" -nullrhi -unattended -nosplash -nosound -NoLiveCoding -ExecCmds="UCC.Status,QUIT_EDITOR" -abslog="<path>\loadtest.log"
```

2026-11-03 result: `InternalLoadLibrary: 'ClassroomConnect'`, `LogClassroomConnect: Classroom Connect loaded with 20 teams.`, `UCC.Status` → engine `5.8.4-58902052`, Multi-User client started, discovery off, no local server, 0 servers; editor exited normally. `-nullrhi` draws no UI, so the window itself was not checked.

### Host + join on one computer

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Scripts\Test-TwoEditors.ps1 -Team Red
```

Starts a headless leader editor (`UCC.Host Red`) and, once it reports Connected, a headless member editor (`UCC.Join Red`) on a second copy of an empty project; waits for each to report Connected and for both to list 2 connected students; then stops only the editors and Multi-User servers it started. Results go to `artifacts/s<id>/results.json`.

2026-11-03 result (run `sf08aa2a9`, team Red, both editors on computer PC-A):

| Step | Log evidence |
| --- | --- |
| Leader checks for another host | `Idle -> CheckingTeam` then, after 4.4 s with no other `UCC-Red-*` server, `CheckingTeam -> StartingServer` |
| Leader's server starts and is discovered | `LogConcert: Server UCC-Red-PC-A discovered.` 5.3 s later |
| Leader creates and joins the session | `Created Session 'UCC-Red'`, owner <user>; `Connecting -> Connected` |
| Member finds the host and joins (no create) | `Server UCC-Red-PC-A discovered`, `Joined Session 'UCC-Red'`, `Connecting -> Connected` 1.4 s after starting the search |
| Both list each other | `Connected students (2)` in both logs |

2026-11-03 rerun with presence markers (run `s716f5434`, package `p878afe49`): leader logged `Your marker color: Orange`; member saw Orange taken and logged `Your marker color: Sky Blue`; within 0.5 s each editor listed the other with the correct color (`DistinctColors: True`). Both reported "an unsaved level" as their level (the test projects have no saved map). Headless editors draw no viewport, so the markers themselves were not seen.

Interactive check by the user the same day (both editors on one PC, same Windows user): host and join from the window worked and both listed 2 students. The logs showed one problem, now fixed: pressing Cancel on Unreal's persist prompt made the panel say "You left" while still connected.

2026-11-03, large markers (run `s74a342ad`, team Teal, `-ContentFrom` the user's saved World Partition level so both editors open `/Game/test`): both listed each other "level test"; the leader logged `Showing a large marker for <user> (Sky Blue).` and the member `Showing a large marker for <user> (Orange).`; no warnings or errors from the marker (only the engine's start-up `UnifiedErrorTest` self-test lines), map check 0 errors. Headless editors draw nothing, so how the marker looks and scales still needs a check on screen.

### Automatic project copy

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Scripts\Test-ProjectCopy.ps1 -Update -ContentFrom '<a ClassroomTest project>\Content'
```

Leader: `ClassroomTest` (from `-ContentFrom`) runs `UCC.Host`. Member: an unrelated empty project `Lobby` runs `UCC.Join` with `-UCCPROJECTS=<run>\MemberProjects`. The script follows the copy, the reopened editor and the join, compares every file of the two projects by SHA-256, and with `-Update` repeats after adding one file to the leader's project.

2026-11-03 results (one computer, headless editors), in order:

| Run | Result |
| --- | --- |
| `c7917d523` | **Leader failed**: `Repository locked by another process`. `Repositories.json` recorded id 13644 for a killed server; Windows had reused 13644 for `conhost.exe`. Also "could not read editor.log" (the test wrote a locked log inside the project). Fixed: stale-lock repair before starting a server; unreadable files are skipped with a warning; test logs moved out of the project. |
| `cc12bf003` | Copy, install, reopen and join **worked** (166 files, 33.7 MB, 63.5 s), but the reopened editor called itself Leader and opened a blank level. Fixed: `-UCCROLE=Member` and the leader's level passed on reopen. |
| `c75a7ce16` | **Passed**: identical by hash; reopened as Member in level `test`. Copy took 63.5 s. |
| `c7785d864`, `c45206286`, `c29a18e88`, `cae13d213` | Speed work: 39 s (several chunks in flight), 39.5 s (smaller window), 30.6 s (per-frame copier). Statistics showed ~12 chunks/s with an 80 ms round trip: replies were being dropped. `cae13d213` failed with "stopped answering". |
| `c97f9e7cd` | **Passed, 0.5 s** for 166 files / 33.7 MB after switching to reliable delivery (`EMessageFlags::Reliable`); identical by hash. |
| `c89cccbdd` (`-Update`) | **Passed.** Round 1: 166 files in 0.4 s, member editor closed itself, reopened editor joined as Member in level `test`, identical by hash. Round 2: leader added one file; member logged `166 of 167 files already on this computer; downloading 1 (69 bytes)`, installed the new revision, identical by hash; the round-1 copy was left untouched. |

Not shown: copying between two computers (speed will be bounded by the network), several members copying at once, multi-gigabyte projects, interrupting a copy and resuming.

### Join code

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Scripts\Test-JoinCode.ps1
```

Leader `ClassroomTest` runs `UCC.Host Teal ab-12cd`. Three members, one after another, each in an unrelated project `Lobby` with `-UCCPROJECTS` inside the run: `UCC.Join Teal` (no code), `UCC.Join Teal WRONG1`, `UCC.Join Teal AB12CD` (the same code typed differently). A member "discovered" the leader's server only if Concert logged `Server UCC-Teal-<PC> discovered`. The script refuses to start while any Multi-User server is running, so it cannot touch a live class session.

2026-11-04, one computer (PC-A), headless editors:

| Run | Package | Result |
| --- | --- | --- |
| `j040c48f5` | `p781b0873` | **Passed.** Leader: `Started the team server with a join code` (same arguments as Unreal's own launch plus `-ini:Engine:[/Script/ConcertServer.ConcertServerConfig]:AuthorizedClientKeys=<join key>`), discovered its own server 4.4 s later, Connected. **No code:** never discovered the server; the leader's computer refused the probe 0.3 s after the search began; failed after 6.3 s with "Teal's leader on PC-A set a join code…". **Wrong code:** never discovered the server; failed after 6.3 s with "Your join code does not match…". **Right code:** discovered the server 1.0 s after the search began, copied ClassroomTest (21 files, 1 MB), reopened with `-UCCJOINCODE=AB12CD`, the reopened editor logged `Multi-User discovery now sends this editor's join code` and joined as Member; both editors listed 2 students; copy identical by SHA-256. |
| `j69293457` | `p4549b87c` (final source: two message strings reworded, version 0.5.0) | **Passed**, same results in every case. |

So the per-launch `-ini:` override does fill the server's `AuthorizedClientKeys`: an editor with the key discovers the server within a second, editors without it did not in 6 s (about six discovery rounds). Unreal's own Multi-User browser sends the same (empty by default) key, so it should not see a coded server either; that was not opened.

Regression, same day: `Test-TwoEditors.ps1 -Team Gold` (no code) hosted through Unreal's `LaunchConcertServer` as before and the member found the leader. Because each editor writes `Config/DefaultEngine.ini` differently at startup, the member then copied the leader's project and reopened, which this older script does not follow: run `s23715852` put that copy in the real `Documents\Unreal Projects\ClassroomConnect` (removed afterwards) and left the reopened editor running. The script now passes `-UCCPROJECTS` inside its run folder and stops reopened editors; rerun `s18c87c62` kept the copy in the run folder. Use `Test-JoinCode.ps1` or `Test-ProjectCopy.ps1` to check a complete join.

Not shown: join codes between two computers, two leaders hosting the same color with different codes (the second should be told "already hosted… with a different join code"), a leader re-hosting with a changed code (should be told to End Session first), the code box in the window.

### Leader saves the team's work on leaving

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Scripts\Test-LeaderSave.ps1 -ContentFrom '<a ClassroomTest project>\Content'
```

Needs a `Content` folder from a project named ClassroomTest that holds a saved level (`-Level`, default `/Game/test`). Each case hosts in a headless editor with that level open, adds a Point Light 3 s after connecting (`UCC.AfterConnected ACTOR ADD CLASS=/Script/Engine.PointLight|…`), then either runs `UCC.Leave` or closes the editor window (`CLOSE_SLATE_MAINFRAME`, run without `-unattended` because unattended editors skip close checks). It passes when the plugin logs the save and new actor files appear in the project's `Content/__ExternalActors__`.

2026-11-03, World Partition level from the user's interactive test (138 actor files):

| Run | Case | Result |
| --- | --- | --- |
| `lb6377c1c` (first version) | Leave | **Failed:** saved 1 asset but wrote **0** session changes; files unchanged. A save is recorded by the session only after the server round trip. |
| `lb6377c1c` | Close | **Not exercised:** `-unattended` skipped the close hook. |
| `l063d4ce9` (wait for recording) | Leave | **Passed:** `Saved 1 changed assets` → `SavingWork` → `Wrote 3 session changes into the project` → left; actor files 138 → 140. |
| `l063d4ce9` | Close | **Passed:** same sequence, then the editor closed itself; actor files 138 → 140. |

Both passing cases rejoined sessions left over from the failed run (the server had restored them), so each also saved that run's light: two new actor files instead of one.

Not shown by these runs: synchronisation of an actual edit, discovery between two computers on a real network, the window UI, leaving/ending through the prompts (they are modal dialogs, which `-unattended` auto-answers). The member logged a Concert warning `Failed to load presence actor class ''` (presence avatars are not shown headless).

### Between computers

Not shown working yet. Before testing, run `Scripts/Check-ClassroomNetwork.ps1` on every computer ([Docs/NETWORK.md](Docs/NETWORK.md)): if a firewall blocks the Unreal programs, the leader still sees its own server but members see `Servers discovered: 0`.

## Installation and packaging

2026-11-03, package `pd697d978` (0.3.0):

| Check | Result |
| --- | --- |
| `Build-Release.ps1` | Built `ClassroomConnect-0.3.0-UE5.8.4-Win64.zip`, 14.7 MB. |
| `Install-ClassroomConnect.ps1` from the release folder | First run **failed**: Windows PowerShell 5.1 leaves `$PSScriptRoot` empty in `param()` defaults, so the installer could not find the plugin next to it. Fixed (also in the deploy script). |
| Per-user route without changing the account | Installed with `-TestRoot`, then started a headless editor on an empty project that does not list the plugin, with `UE_ADDITIONAL_PLUGIN_PATHS` set for that process only: `Mounting External plugin ClassroomConnect`, `Classroom Connect loaded with 20 teams.`, Multi-User client started. The account's variable stayed empty. |
| `Deploy-ClassroomConnect.ps1 -CheckOnly` for this PC and a non-existent one | This PC `Ready` (admin share reachable, 5.8.4, build id matches); the other `Skipped` with the reason. First attempt mis-bound a comma list passed through `powershell -File`; fixed (comma lists accepted, no positional binding). |
| `Package-Fab.ps1 -Seller 'Example Seller'` | Zip with `.uplugin`, `Source`, `Config/FilterPlugin.ini`, `Content`, `Resources/Icon128.png`, no `Binaries`/`Intermediate`, copyright line in every source file; `RunUAT BuildPlugin` compiled the zipped source. |
| `Package-Fab.ps1` again, 2026-11-04 (0.4.0, run `fab-d1a827`) | Zip (48 KB) adds `Documentation/README.md` and `"PlatformAllowList": ["Win64"]` (required by Fab's plugin compilation page). Compiled from the zip; the compiled plugin loaded in a clean project (`Classroom Connect loaded with 20 teams`, `UCC.Status` ran); the compiled output kept `Documentation/README.md`. |
| `Package-Fab.ps1 -Seller 'nsguinn'`, 2026-11-04 (0.5.0, run `fab-a36b20`) | The first 0.5.0 zip (`fab-e54c11`) stored paths with backslashes (`ClassroomConnect\Source\...`, what `Compress-Archive` does in Windows PowerShell 5.1), which zip tools outside Windows read as file names. The packager now writes `ClassroomConnect/...` paths and refuses any other. Rebuilt zip: 19 entries including the empty `Content/`, extracts to exactly the 18 packaged files (same SHA-256), compiles with `RunUAT BuildPlugin`, loads in a clean project. Descriptor: `CreatedBy` nsguinn, `DocsURL` on `main`. SHA-256 `867E285FED061761A1753EA99C16E820229D79AA2F271D25B7029D504ECE2AFF (rebuilt with links to the public repository)`. |
| `Build-Release.ps1`, 2026-11-04 (0.5.0, package `p625c5f0f`) | `ClassroomConnect-0.5.0-UE5.8.4-Win64.zip`, 15.5 MB, now with the user guide and NETWORK.md. A first build (`pcc90232d`) embedded the build folder, including the Windows account name, in the DLL and PDB; this one was built from a `subst` drive (`U:\`), and a scan of all 34 files found no account name or user-folder path. SHA-256 `B9EB35235C4CB80C43F533F474F05F1123EFB7C1359625207AAD7C20D7459570 (rebuilt with links to the public repository and the updated install guide)`. Not installed from this zip on another computer yet. |

Not run: an Engine-scope install (writes under Program Files, needs an administrator), deploying to other computers, the double-click .cmd on a fresh student account.

## Not yet tested

Loading the plugin in the editor, the Slate window, hosting, joining, two-editor synchronisation, discovery between computers, SMB shares, crash/disconnect recovery, multiple teams. See ROADMAP.md.
