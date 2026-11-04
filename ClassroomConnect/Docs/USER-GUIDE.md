# Classroom Connect: user guide

Classroom Connect puts Unreal Engine's built-in Multi-User Editing behind one window: pick a team, pick a role, press one button. A team leader hosts; teammates join, receive the leader's project automatically, and edit the same level together. Everything runs on your local network; there is no cloud service, account or central server.

## Requirements

- Unreal Engine **5.8** on **Windows 10/11 (64-bit)**. Editor only: nothing is added to packaged games.
- Every computer in a team on the same local network, able to reach the others with Unreal's Multi-User Editing (UDP). Many school and office networks allow this within a room; some block it (see Troubleshooting).
- The plugin enables Unreal's **Multi-User Editing** plugins (ConcertMain, ConcertSyncClient, MultiUserClient), which ship with the engine.

## Install

- **From Fab:** install the plugin to your engine with the Epic Games Launcher, then enable **Classroom Connect** in **Edit → Plugins** for each project (restart when asked).
- **For a whole lab:** the classroom package (`ClassroomConnect-<version>-UE5.8.4-Win64.zip`, built with `Scripts\Build-Release.ps1`) installs it for every project on each computer without opening the Plugins window; see Docs/INSTALL.md.

Open the window from **Classroom** on the level editor toolbar or **Tools → Classroom Connect**.

## Use

**Team leader** (one per team):
1. Open the team's project and a **saved** level. Multi-User Editing never shares unsaved levels.
2. Pick the team's color tile and choose **Team Leader**. Optionally type a **join code** or press **New code** (see below).
3. Press **HOST PROJECT** and wait for **Connected**. Your computer now runs the team server and shares the project with teammates. If you set a code, the Status box shows it: read it out to your team.

**Team members:**
1. Open any project, pick the same team tile, choose **Team Member**, type the leader's join code if they set one, and press **JOIN TEAM**.
2. If you do not have the leader's exact project, Classroom Connect copies it (only the files you are missing), verifies every file, installs it into `Documents\Unreal Projects\ClassroomConnect\<Team>\<revision>\<Project>`, and reopens Unreal on it, in the leader's level, already joined. Nothing on your computer is overwritten.

## Join code (optional)

A join code keeps other students out of your team, whether they picked your color by mistake or on purpose.

- The leader types any letters and numbers (up to 16) or presses **New code** for a random 6-character code. Capitals, spaces and dashes do not matter: `ab-12cd` and `AB12CD` are the same code.
- With a code, the leader's team server only answers editors that send the same code, so other editors (including Unreal's own Multi-User browser) do not see it, and the leader's computer refuses project-copy requests without it.
- A member without the code is told "set a join code"; a member with a different code is told it "does not match". Neither gets the project.
- Without a code, everything works as before: anyone who picks the team can join.
- The code is not saved anywhere. Type it again after restarting Unreal. A member whose Unreal reopens on the copied project keeps the code for that join.
- If the leader changes the code, or hosts with a code on a computer whose team server was started without one, press **End Session**, then **HOST PROJECT** again.

Console equivalents: `UCC.Host <Team> <Code>`, `UCC.Join <Team> <Code>`.

**While connected:** each student has their own marker color. Teammates in the same level appear as a colored marker with their name, plus a large arrow that grows with distance. **Connected Students** lists everyone; **Go To** moves your view to a teammate.

**Finishing:** the leader presses **End Session**. Classroom Connect saves every changed asset and writes the session's work into the leader's project before leaving, so nothing depends on answering a "persist changes?" question; closing Unreal while hosting does the same. Members press **Leave Team**.

## What is shared, and with whom

- Live edits travel through Unreal's Multi-User Editing to everyone in the team session.
- **Without a join code**, the leader's computer answers project-copy requests for its team from **any computer on the network that asks for that team**, and any editor on the network can find and join the team session. Team names are labels, not passwords.
- **With a join code**, only editors that send the code can find the team server or copy the project. This keeps classmates out; it is **not encryption**. Only a fingerprint of the code (a SHA-256 hash) travels, but it travels unencrypted in every discovery message, and project files and edits travel unencrypted too, as with all of Unreal's Multi-User Editing. Someone recording the network traffic could reuse the fingerprint. Do not host projects you are not willing to share with the local network.
- Classroom Connect does not change firewall rules, Windows settings, or group policies, and does not use shared folders.

## Limits

- Multi-User Editing does not synchronize every asset type or every kind of change equally; test the kinds of edits your class makes. Two people editing the same asset at once can conflict.
- Copying speed depends on the network. Very large projects take correspondingly long the first time; later copies only fetch changed files.
- Tested on Unreal Engine 5.8.4. Hosting and joining have been verified with editors on one computer; check your own network with two computers before relying on it in class.

## Troubleshooting

| Problem | What to do |
| --- | --- |
| A member sees "Could not find the team leader" | Make sure the leader pressed HOST PROJECT (and, if they set a join code, that you typed it). Open **View Diagnostics**: if the leader sees its own server but members see "Servers discovered: 0", the Windows Firewall on the computers is most likely blocking Unreal's Multi-User traffic between computers. Static endpoints do not get past a firewall. Give IT [NETWORK.md](NETWORK.md): it lists the inbound rule needed and a read-only check script (`Scripts/Check-ClassroomNetwork.ps1`). |
| Windows asks to allow Unreal or "UnrealMultiUserSlateServer" on the network | The first time a computer hosts. Decide with your IT staff; Classroom Connect never changes firewall settings. |
| "Session storage is still locked" when hosting | A team server on this computer did not close properly earlier. Press **End Session**, then **HOST PROJECT** again; Classroom Connect clears the stale lock before starting a new server. |
| "Classroom Connect needs Unreal Engine 5.8" | Use Unreal Engine 5.8. Everyone in a team needs the same 5.8 version; Multi-User Editing refuses to join a session created by a different engine version. |
| A copy stopped part-way | Press **Retry**. Files already copied and verified are reused. |
| "...set a join code" or "...does not match" | Ask the leader for the code shown in their Status box, type it under **Join Code**, press **JOIN TEAM**. |
| A leader sees "...already hosted from computer X, with a different join code" | Another computer hosts this team color with its own code. Join it as a member with that code, or pick another color. |
| A leader sees "...not started with this join code" | Press **End Session** (stops this computer's team server), then **HOST PROJECT** again. |

Console commands for support staff: `UCC.Status` (state and diagnostics), `UCC.Host <Team> [Code]`, `UCC.Join <Team> [Code]`, `UCC.Leave`. Log category: `LogClassroomConnect`.

Support: https://github.com/nsguinn/ClassroomConnect/issues
