# Publishing Classroom Connect on Fab

```powershell
.\Scripts\Package-Fab.ps1 -Seller 'Your Name or Studio'
```

This writes `artifacts\ClassroomConnect-<version>-UE5.8-Fab.zip`, compiles exactly that source with `RunUAT BuildPlugin` (Win64), installs the compiled result into a clean project and starts a headless editor to confirm the plugin loads and runs `UCC.Status`. The repository's own files are not changed; the copyright lines and descriptor fields are added to the copy in the zip.

## What the package satisfies

| Requirement | Source | Status |
| --- | --- | --- |
| `.uplugin`, `Source`, `Content`, `Config` (+ `Resources`); at least one code module; one plugin per zip | [Epic: Asset File Format and Structure Requirements](https://dev.epicgames.com/documentation/fab/asset-file-format-and-structure-requirements-in-fab) | Done |
| Documentation "linked or built into the editor" where needed | Epic, same page | `Documentation/README.md` in the plugin (the [user guide](USER-GUIDE.md)), kept by builds through `Config/FilterPlugin.ini`; `DocsURL` links the same guide |
| `PlatformAllowList` or `PlatformDenyList` in every module | [Fab support: Plugin Compilation Environment](https://support.fab.com/s/article/Fab-Plugin-Compilation-Environment?language=en_US) (Jan 2025) | `"PlatformAllowList": ["Win64"]`; the packager refuses a module without it |
| Self-contained: no external executable needed | Fab "self-contained plugins" (forum reports) | Project copying is built into the plugin; the C# helper is only an optional IT tool in the classroom zip, not part of the Fab product |
| No `Binaries`/`Intermediate`, `"Installed": false`, `Resources/Icon128.png`, copyright line per source file, `FabURL` once the listing exists | Common Fab pre-submission validators (not found in Epic's own pages) | Done; pass `-FabUrl` after the listing exists |
| Compiles for the claimed engine version | Good practice; Fab builds it | Compiles against 5.8.4 from the zip |
| Installs and loads in a clean project | Good practice | Verified headless |
| Works on any 5.8.x | Fab builds for "5.8" | The plugin accepts any 5.8.x; Multi-User itself refuses mixed engine versions inside a team. Only 5.8.4 has been tested. |

Not verified: Fab's full "Technical Requirements" page (a version dated 2026-09-21 is cited by other summaries, but it could not be located or read here). Read it before submitting.

## Decisions and tasks that are yours

1. **Publisher and ownership.** Who owns the code (you personally, a studio, or an employer or school: check your employment or contract terms if it was written for or with their resources), and therefore the `-Seller` name in every copyright line.
2. **Price and license.** Free or paid; Fab Standard (Personal/Professional) or, if free, Creative Commons.
3. **Network exposure.** Addressed with an optional **join code** (0.5.0): with a code, the leader's Multi-User server answers discovery only from editors sending the code's key, and the project copy refuses requests without it. Without a code, the leader's computer still sends the team project to any computer that asks for that team. The code is not encryption (files and edits travel unencrypted, as with all Multi-User Editing); the user guide says so. Decide whether the listing should recommend always using a code.
4. **Listing.** Fab account and publisher profile; a Code Plugin listing with title, description, engine version 5.8, platform Win64; one thumbnail plus at least one more media item. A 60–90 s video of two or more students joining a team, receiving the project and editing one level is the strongest proof; it needs a real multi-computer session.
5. **Upload.** Fab's "Project file link" is not shown to buyers: Fab downloads the zip from it, compiles the plugin and offers it through the launcher. It must be a direct download that needs no sign-in, not the repository page. 0.5.0 is attached to the GitHub release: `https://github.com/nsguinn/ClassroomConnect/releases/download/v0.5.0/ClassroomConnect-0.5.0-UE5.8-Fab.zip`. Keep it available until the product is live. Then rebuild with `-FabUrl 'com.epicgames.launcher://ue/Fab/product/<id>'`.
6. **Before submitting:** the two-computer classroom test (Docs/CLASSROOM-TEST.md). Fab reviewers and buyers will judge it on real networks.

## What buyers get

The plugin enables Unreal's Multi-User Editing (ConcertMain, ConcertSyncClient, MultiUserClient) and adds the Classroom window. It is editor-only and Windows-only; nothing is added to packaged games. Discovery between computers depends on the buyer's network allowing Multi-User Editing; say so in the listing.
