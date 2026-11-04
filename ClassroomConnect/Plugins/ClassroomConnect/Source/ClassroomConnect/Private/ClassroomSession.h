#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "ClassroomTeams.h"

class IConcertClient;
class IConcertClientSession;
class IConcertSyncClient;
class FClassroomProjectSync;
struct FConcertServerInfo;
struct FConcertAdmin_GetSessionsResponse;
enum class EConcertResponseCode : uint8;

enum class EClassroomRole : uint8 { Leader, Member };

enum class EClassroomState : uint8
{
    Idle,
    CheckingTeam,   // Leader: looking for another computer already hosting the team
    StartingServer, // Leader: waiting for the local Multi-User server to be discovered
    FindingHost,    // Member: waiting for the team leader's server and session
    Connecting,     // Create/join request sent; waiting for Concert to report Connected
    Connected,
    SavingWork,     // Leader leaving: saved assets, waiting for the session to record them before writing them to the project
    GettingProject, // Member: checking or copying the leader's project before joining
    Reopening,      // Member: a new editor was started on the copied project; this one is closing
    Failed,
};

/** A student in the current session, as shown in the panel. Color is the student's Multi-User presence marker color. */
struct FClassroomParticipant
{
    FGuid EndpointId;
    FString Name;
    FString Computer;
    FLinearColor Color = FLinearColor::White;
    FString ColorName;
    FString Level;          // Short level name, or empty when unknown
    bool bIsLocal = false;
    bool bSameLevel = false; // Their marker is drawn in this editor's viewport and Go To works

    bool operator==(const FClassroomParticipant& Other) const
    {
        return EndpointId == Other.EndpointId && Name == Other.Name && Computer == Other.Computer && Color.Equals(Other.Color) && Level == Other.Level && bSameLevel == Other.bSameLevel;
    }
};

/**
 * Hosts or joins a team's Multi-User session using the public Concert client owned by the MultiUserClient module.
 * Members only ever join an existing session; only the leader creates one.
 * Without a join code, team selection is a label, not an access control. With one (ClassroomJoinCode), the leader's
 * Multi-User server can only be discovered by editors that send the code's key, and the project copy refuses others.
 */
class FClassroomSession : public TSharedFromThis<FClassroomSession>
{
public:
    void Start(const TArray<FClassroomTeam>& InTeams);
    void Stop();

    /** JoinCode: optional; members must enter the same code. Case, spaces and dashes do not matter. */
    void Host(const FClassroomTeam& Team, const FString& JoinCode = FString());
    void Join(const FClassroomTeam& Team, const FString& JoinCode = FString());
    void Retry();
    /** The normalized code of the current or last attempt (empty when none), e.g. for the leader to read out. */
    const FString& GetJoinCode() const { return JoinCode; }
    /**
     * Team Leader: saves the team's work into this project, then leaves (see LeaveAndSave).
     * Team Member: leaves through Unreal's own prompt; the leader's project holds the team's work.
     */
    void Leave();
    /** Leader only: Leave(), then shuts down this computer's Multi-User server. */
    void EndHosting();
    /** True when this editor is the Team Leader in the team session, so leaving or closing must save the team's work first. */
    bool ShouldSaveOnLeave() const;
    /**
     * Leader: saves every changed asset, waits for the session to record those saves, writes all session changes into
     * this project's files without a prompt, and only then disconnects. On failure stays connected (nothing is discarded)
     * and explains why in GetMessage(). OnDone(bLeft) runs on the game thread when finished.
     */
    void BeginLeaveAndSave(TFunction<void(bool bLeft)> OnDone = nullptr);
    /** Moves this editor's viewport to just behind a teammate in the same level. */
    void JumpTo(const FGuid& EndpointId);
    /** Large name markers above teammates in the same level (on by default, remembered per Windows user). */
    bool GetShowMarkers() const { return bShowMarkers; }
    void SetShowMarkers(bool bShow);

    EClassroomState GetState() const { return State; }
    EClassroomRole GetRole() const { return Role; }
    bool IsBusy() const
    {
        return State == EClassroomState::CheckingTeam || State == EClassroomState::StartingServer || State == EClassroomState::FindingHost || State == EClassroomState::Connecting
            || State == EClassroomState::SavingWork || State == EClassroomState::GettingProject || State == EClassroomState::Reopening;
    }
    /** 0..1 while copying the team project, otherwise negative. */
    float GetTransferProgress() const;
    const FText& GetMessage() const { return Message; }
    const FString& GetHostComputer() const { return HostComputer; }
    const TArray<FClassroomParticipant>& GetParticipants() const { return Participants; }
    /** Increments whenever GetParticipants() changes, so the panel only rebuilds rows when needed. */
    uint32 GetParticipantsRevision() const { return ParticipantsRevision; }
    TArray<FString> GetDiagnostics() const;
    static FString StateName(EClassroomState Value);

private:
    bool Tick(float DeltaTime);
    void Begin(const FClassroomTeam& InTeam, EClassroomRole InRole, const FString& InJoinCode);
    /** Sets the key this editor's Multi-User client sends when discovering servers (applied once the client exists). */
    void SetClientJoinKey(const FString& Key);
    void ApplyClientJoinKey();
    /** Starts this computer's Multi-User server; with a join code, one that only answers clients sending its key. */
    bool LaunchTeamServer(const FString& ServerName);
    void Enter(EClassroomState NewState, const FText& NewMessage);
    void Fail(const FText& Reason);
    void Reset(const FText& NewMessage);
    void RequestSessions(const FConcertServerInfo& Server);
    void HandleSessions(const FGuid& ServerId, const FConcertAdmin_GetSessionsResponse& Response);
    void HandleConnectResult(EConcertResponseCode Code);
    void RefreshParticipants(const IConcertClient& Client);
    void AssignMarkerColor(IConcertClientSession& Session);
    void UpdateMarkers();
    void RemoveMarkers();
    bool SaveChangedAssets(FText& OutError);
    TFunction<void(EConcertResponseCode)> MakeConnectResultHandler();
    void JoinTeamSession(const FGuid& ServerId, const FGuid& SessionId);
    void TickGettingProject();
    void ReopenOn(const FString& ProjectFile);
    void AdoptExistingSession(const IConcertClient& Client);
    void TickSavingWork(double Now);
    void FinishLeave(bool bLeft, const FText& NewMessage);
    bool IsInSession() const;
    TSharedPtr<IConcertSyncClient> GetSyncClient() const;
    TSharedPtr<IConcertClient, ESPMode::ThreadSafe> GetClient() const;
    FText TeamName() const;

    FTSTicker::FDelegateHandle TickerHandle;
    TOptional<FClassroomTeam> Team;
    FString JoinCode;
    FString JoinKey;
    /** The discovery key the Multi-User client had before Classroom Connect changed it, restored when leaving. */
    TOptional<FString> OriginalClientKey;
    TOptional<FString> PendingClientKey;
    /** Join key of the Multi-User server this editor started (empty: started without a code); unset if not started here. */
    TOptional<FString> LaunchedServerKey;
    EClassroomRole Role = EClassroomRole::Member;
    EClassroomState State = EClassroomState::Idle;
    FText Message;
    FString HostComputer;
    TArray<FClassroomParticipant> Participants;
    uint32 ParticipantsRevision = 0;
    bool bPresenceEnabled = false;
    bool bShowMarkers = true;
    TMap<FGuid, TWeakObjectPtr<class AClassroomMarkerActor>> Markers;

    // Leader leave-and-save in progress
    TSet<FName> SavedForLeave;
    TSet<FName> LastRecordedChanges;
    double LastRecordedChangeSeconds = 0.0;
    bool bSkippedUnsavedLevel = false;
    EClassroomState StateBeforeSaving = EClassroomState::Connected;
    TFunction<void(bool)> OnLeaveDone;

    // Team project copying
    TArray<FClassroomTeam> Teams;
    TSharedPtr<FClassroomProjectSync> ProjectSync;
    FGuid PendingServerId;
    FGuid PendingSessionId;
    FString PendingServerName;
    FString TeamProjectName;
    double PhaseStartSeconds = 0.0;
    double LastRequestSeconds = 0.0;
    bool bRequestInFlight = false;
    /** Replaced on every new attempt so late replies from an abandoned attempt are ignored. */
    TSharedPtr<uint8> AttemptToken;
};
