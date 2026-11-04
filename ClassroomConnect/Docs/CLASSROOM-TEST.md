# Classroom test: about 5 teams

Goal: find out, with real students and computers, whether Classroom Connect works across a real school network for several teams at once, and collect evidence. Everything so far was verified with editors on **one** computer; discovery between computers on a real network has never been tried and is the biggest unknown.

## Before class (instructor or IT, about 30 minutes)

1. **Install** on every lab computer (INSTALL.md, Option A). Keep the deploy CSV.
2. **Network preflight with two computers.** On computer A: open a project, Classroom → Red → Team Leader → HOST PROJECT. On computer B, with a copy of the *same* project: Red → Team Member → JOIN TEAM. Wait up to a minute.
   - Connected on both: discovery works between these two computers; continue.
   - B says "Could not find Red's team leader" and **View Diagnostics** shows "Servers discovered: 0": the network blocks Multi-User discovery. Stop and ask IT about UDP multicast within the room, or configuring Unreal's UdpMessaging static endpoints (Docs/CONCERT-5.8.4.md). Running the class test would only reproduce this.
   - Then End Session on A.
3. **Projects.** Only each **leader** needs the team project, with at least one **saved** level open (unsaved levels are never shared). Members can open any project; when they join, Classroom Connect copies the leader's project to them, verifies it, and reopens Unreal on it. (Copying a project by hand, or with the included helper, still works if a network blocks it.)
4. **Team sheet.** Fill in the table at the end: team color, leader's computer, members' computers.

## In class (about 40 minutes)

Students open their team project first, then Classroom. Note times and anything unexpected.

| Step | What to do | What should happen | Record |
| --- | --- | --- | --- |
| 1 | All 5 leaders: team tile → Team Leader → HOST PROJECT, at the same time | Each reaches Connected; no "already hosted" | Time to Connected per team |
| 2 | Members: team tile → Team Member → JOIN TEAM (from any project) | "Getting team project" with a progress bar, then a new Unreal window opens in the leader's level and reaches Connected; Connected Students lists the team with different colors | Copy time and project size; any errors |
| 3 | Everyone opens the team's saved level | Teammates show as colored markers with names; Go To works | Markers visible? readable from far away? |
| 4 | Each member moves one actor and adds one actor | Everyone in the team sees the change within a few seconds | Delay; failures |
| 5 | One member edits a material, another a Blueprint, then saves | Record what synchronizes and what does not | Per asset type |
| 6 | Check other teams | No student appears in another team's list or level | Any cross-team sightings |
| 7 | One member: Leave Team, then JOIN TEAM again | Rejoins and sees the current state | Works? time |
| 8 | One member: close Unreal without leaving (or end it in Task Manager), reopen, rejoin | Rejoins | Works? |
| 9 | Leaders: End Session | "Saved your team's work into this project … This computer's team server was stopped." | Message text |
| 10 | Leaders: reopen the project | The session's changes are in the project | Present? |

Optional: a leader closes Unreal mid-session. Record what members see. Members' work since the last save stays on the leader's team server, not in the leader's project.

Optional, join codes (0.5.0): before step 1, have two of the five leaders press **New code** and write their code on the team sheet. In step 2 their members type it under **Join Code**. Also have one student from another team try to join a coded team without the code (expected: "…set a join code" within about 10 seconds, no project copied) and with a wrong code (expected: "…does not match"). Record whether coded teams took longer to connect.

## Collect evidence

On every computer, after closing Unreal:

```powershell
.\Collect-ClassroomLogs.ps1 -ProjectPath 'D:\Projects\TeamRed' -Destination '\\share\class\logs'
```

It copies the Classroom Connect and Multi-User lines from the project's recent Unreal logs (plus crash folders, if any) into one zip named after the computer, without changing anything. Send the zips and this sheet back for analysis.

## Team sheet

| Team | Leader computer | Member computers | Leader Connected (s) | All members Connected (s) | Edits synced? | Problems |
| --- | --- | --- | --- | --- | --- | --- |
| | | | | | | |
| | | | | | | |
| | | | | | | |
| | | | | | | |
| | | | | | | |
