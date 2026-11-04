# Network and firewall requirements

Classroom Connect uses Unreal's Multi-User Editing as it is. Every computer in a team must be able to receive UDP from the others: the team leader's computer runs the Multi-User server, and every editor exchanges messages with it. Classroom Connect never changes firewall rules, Windows settings or group policies.

## Check a computer

From an **administrator** PowerShell (Windows lets only administrators read the firewall rules in force; the check changes nothing):

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File .\Scripts\Check-ClassroomNetwork.ps1
```

It reports the firewall profile in use (Domain, Private or Public), whether rules made on the computer count, where the rules in force come from (Intune/MDM, Group Policy, local), and for `UnrealEditor.exe`, `UnrealMultiUserSlateServer.exe` and `UnrealMultiUserServer.exe` whether incoming UDP is allowed and by which rule. Exit code 0 = ready, 2 = blocked, 1 = could not check. Add `-Csv <file>` to append a summary row, to collect every lab computer in one file; `-EngineRoot` if Unreal is not in `C:\Program Files\Epic Games\UE_5.8`.

## What the firewall must allow

| Direction | Protocol | Programs | Ports | From |
| --- | --- | --- | --- | --- |
| Inbound, Allow | UDP | `<UE_5.8>\Engine\Binaries\Win64\UnrealEditor.exe`, `UnrealMultiUserSlateServer.exe`, `UnrealMultiUserServer.exe` | any | the lab's computers only |

On the profile the lab network uses (Domain on a domain network).

Why programs and not just port 6666: 6666 is only discovery (multicast group `230.0.0.1:6666`). After that, the editor and the server talk unicast in both directions on ports Unreal picks at random, because the default `UnicastEndpoint` is `0.0.0.0:0`. A rule for UDP 6666 alone lets computers hear each other's announcements but not connect.

A port-based alternative: give every lab computer a fixed port with `UnicastEndpoint=0.0.0.0:6666` (Unreal then starts the server on the next port, 6667: `GenerateConcertServerCommandLine` in MultiUserClientModule.cpp) and allow UDP 6666-6667 from the lab. More to manage than the program rule.

If the computers are managed (Intune or Group Policy) and "apply local firewall rules" is off, nothing done on the computer counts: not a rule added by an administrator, not clicking **Allow** on Windows' firewall prompt. The rule has to come from whoever manages the policy.

## What a blocked network looks like

The leader's **View Diagnostics** shows `Local Multi-User server running: yes` and `Servers discovered: 1` (its own server), while members show `Servers discovered: 0`, and `Check-ClassroomNetwork.ps1` reports the Unreal programs **Blocked** (exit code 2). Traffic within one computer is not filtered, so the leader still sees its own server; traffic between computers is. Dismissing Windows' firewall prompt can also leave a local **Block** rule for `UnrealEditor.exe`. Once the inbound rule is in place, run the check on every computer in the team, then the two-computer test (CLASSROOM-TEST.md).

## Static endpoints and multicast (for the network team)

Facts from the 5.8 source (`UdpMessagingSettings.h`, `UdpMessagingModule.cpp`):

- `StaticEndpoints` is a list: each computer can list every lab computer (`+StaticEndpoints=<ip>:6666`, one line each). That needs fixed addresses (DHCP reservations) and a fixed `UnicastEndpoint` port on every computer.
- Multicast cannot be switched off by settings: an empty `MulticastEndpoint` falls back to the default `230.0.0.1:6666`. Static endpoints add direct traffic; they do not replace the multicast announcements.
- `bAllowTargetDiscovery` is not a UdpMessaging setting in 5.8; a line with it is ignored.
- Static endpoints do not get past a firewall: the same inbound rule is still needed.
- Pointing every student computer at the instructor's computer only (one static endpoint) means only the instructor's computer can host. Classroom Connect lets any student be a team leader, so every computer would need to list every other.

Recommended order: add the inbound rule, confirm the two-computer test works with discovery as it is, then decide with the network team whether static endpoints are worth the extra configuration.
