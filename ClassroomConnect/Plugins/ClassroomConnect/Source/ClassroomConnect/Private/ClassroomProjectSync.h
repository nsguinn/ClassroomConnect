#pragma once

#include "CoreMinimal.h"
#include "Async/Future.h"
#include "Containers/Ticker.h"
#include "IMessageContext.h"
#include "ClassroomProjectMessages.h"

class FClassroomSha256;
class FMessageEndpoint;
class IFileHandle;

enum class EClassroomProjectState : uint8
{
    Idle,
    Asking,          // Waiting for the leader's offer
    ReadingManifest, // Downloading the leader's file list
    Checking,        // Comparing with copies already on this computer
    Downloading,
    UpToDate,        // The open project already matches the leader's
    Installed,       // A verified copy is ready: GetInstalledProject()
    NotShared,       // The leader did not answer (older Classroom Connect or blocked); the caller decides
    Failed,
};

/** Result of hashing a project folder. */
struct FClassroomManifestResult
{
    TArray<FClassroomManifestEntry> Entries;
    /** Files that could not be read (for example locked by another program); they are not shared. */
    TArray<FString> Skipped;
};

/** Result of comparing a manifest with files already on this computer. */
struct FClassroomCheckResult
{
    TArray<int32> Missing;
    bool bCurrentProjectMatches = false;
    FString Error;
};

/**
 * Optional team join code. The leader picks one; members must type the same one. Codes are compared after Normalize
 * (case and spaces do not matter), and only a hash of the code (Key) travels over the network. The key is what Unreal's
 * Multi-User server checks before it lets a client discover it (AuthorizedClientKeys), and what the project copy checks
 * on every request. It is not encryption: project files and edits still travel unencrypted on the local network.
 */
namespace ClassroomJoinCode
{
    constexpr int32 MaxLength = 16;
    /** Upper case letters and digits only, at most MaxLength. "ab 12-cd" -> "AB12CD". */
    FString Normalize(const FString& Code);
    /** "UCC1-" + 32 hex digits of SHA-256 over the normalized code; empty for an empty code. */
    FString Key(const FString& NormalizedCode);
    /** A random 6-character code without easily confused characters (no 0/O, 1/I/L). */
    FString Make();
}

/**
 * Copies the team leader's project to team members over Unreal's message bus.
 * Leader: StartServing() hashes the open project in the background and answers members. The files on disk do not change
 * during a Multi-User session (session edits live in Multi-User's sandbox), so the baseline stays consistent while served.
 * Member: BeginGetProject() fetches the file list, reuses identical files already on this computer, downloads the rest,
 * verifies every file's SHA-256 and installs the result into a new folder per revision. Nothing existing is overwritten.
 * Without a join code, team names are labels: anyone on the network who asks for a team's project receives it. With a
 * join code, the leader answers only requests carrying the code's key, and tells other askers their code does not match.
 */
class FClassroomProjectSync : public TSharedFromThis<FClassroomProjectSync>
{
public:
    void Start();
    void Stop();
    /** Runs every frame (own ticker), so transfers are not limited by the session's slower tick. */
    void Tick(double Now);

    /** JoinKey: ClassroomJoinCode::Key of the leader's code, or empty to answer anyone asking for this team. */
    void StartServing(const FString& TeamId, const FString& JoinKey);
    void StopServing();

    void BeginGetProject(const FString& InTeamId, const FString& InHostComputer, const FString& InProjectName, const FString& InJoinKey);
    void Cancel();

    /**
     * While a member looks for a leader (or a leader checks nobody else hosts the team), ask every computer serving the
     * team. A leader whose join code differs is invisible to Multi-User discovery, so this is how the member learns that
     * its code is missing or wrong, and how a second leader learns the team is already hosted.
     */
    void BeginProbe(const FString& InTeamId, const FString& InJoinKey);
    void StopProbe();
    /** A computer serving the probed team that refused the probe's join code, or empty. */
    const FString& GetProbeRejectedBy() const { return ProbeRejectedBy; }
    /** A computer serving the probed team that accepted the probe's join code, or empty. */
    const FString& GetProbeAcceptedBy() const { return ProbeAcceptedBy; }

    EClassroomProjectState GetState() const { return State; }
    const FText& GetMessage() const { return Message; }
    /** 0..1 while downloading, otherwise negative. */
    float GetProgress() const;
    const FString& GetInstalledProject() const { return InstalledProject; }
    /** The level the leader had open when offering the project, or empty. */
    const FString& GetLeaderLevel() const { return Offer.Level; }
    TArray<FString> GetDiagnostics() const;

    /** Where members' copies go: <Documents>/Unreal Projects/ClassroomConnect, or -UCCPROJECTS=<folder>. */
    static FString ProjectsRoot();
    /** Revision this project was installed at by Classroom Connect, or empty. */
    static FString ReadInstalledRevision(const FString& ProjectDir);

private:
    /** A file being downloaded: several chunks are requested at once and written at their offsets as they arrive. */
    struct FActiveFile
    {
        TSharedPtr<IFileHandle> Handle;
        int64 NextOffset = 0;      // First byte not requested yet
        int64 Received = 0;        // Bytes written
        TMap<int64, double> Pending; // Requested chunk offset -> time requested
        int32 Attempts = 0;
    };

    void HandleQuery(const FClassroomProjectQuery& Message, const TSharedRef<IMessageContext, ESPMode::ThreadSafe>& Context);
    void HandleManifestRequest(const FClassroomManifestRequest& Message, const TSharedRef<IMessageContext, ESPMode::ThreadSafe>& Context);
    void HandleChunkRequest(const FClassroomChunkRequest& Message, const TSharedRef<IMessageContext, ESPMode::ThreadSafe>& Context);
    void HandleOffer(const FClassroomProjectOffer& Message, const TSharedRef<IMessageContext, ESPMode::ThreadSafe>& Context);
    void HandleManifestPage(const FClassroomManifestPage& Message, const TSharedRef<IMessageContext, ESPMode::ThreadSafe>& Context);
    void HandleChunkReply(const FClassroomChunkReply& Message, const TSharedRef<IMessageContext, ESPMode::ThreadSafe>& Context);

    void SetState(EClassroomProjectState NewState, const FText& NewMessage);
    void FailWith(const FText& Reason);
    void RequestManifestPage(int32 Page);
    void OnManifestComplete();
    void StartFile(int32 FileIndex, double Now);
    void StartNextFiles(double Now);
    void VerifyFile(int32 FileIndex);
    void FillWindow(int32 FileIndex, double Now);
    void RequestChunk(int32 FileIndex, int64 Offset, double Now);
    void TickDownloads(double Now);
    void FinishDownload();
    void UpdateDownloadMessage();
    FString StagingRoot() const;
    FString FinalRoot() const;

    TSharedPtr<FMessageEndpoint, ESPMode::ThreadSafe> Endpoint;

    // Leader
    FString ServeTeamId;
    FString ServeDir;
    FString ServeRevision;
    FString ServeProjectName;
    int64 ServeBytes = 0;
    TArray<FClassroomManifestEntry> ServeEntries;
    TFuture<FClassroomManifestResult> ServeBuild;
    bool bServeReady = false;
    FString ServeJoinKey;

    // Probe (see BeginProbe)
    FString ProbeTeamId;
    FString ProbeJoinKey;
    FString ProbeRejectedBy;
    FString ProbeAcceptedBy;
    double LastProbe = 0.0;

    // Member
    EClassroomProjectState State = EClassroomProjectState::Idle;
    FText Message;
    FString TeamId;
    FString JoinKey;
    FString HostComputer;
    FString ProjectName;
    FClassroomProjectOffer Offer;
    FMessageAddress Leader;
    double PhaseStart = 0.0;
    double LastSend = 0.0;
    TArray<TArray<FClassroomManifestEntry>> Pages;
    int32 PagesReceived = 0;
    int32 PageAttempts = 0;
    TArray<FClassroomManifestEntry> Entries;
    TFuture<FClassroomCheckResult> Check;
    TArray<int32> Queue;
    TMap<int32, FActiveFile> Active;
    /** Finished files being hashed off the game thread; they no longer hold a download slot. */
    TMap<int32, TFuture<FString>> Verifying;
    TSet<int32> Retried;
    FTSTicker::FDelegateHandle TickerHandle;

    // Throughput statistics, logged every few seconds while copying or serving
    double StatsStart = 0.0;
    int32 StatChunks = 0;
    int64 StatBytes = 0;
    double StatRoundTrips = 0.0;
    int32 ServedChunks = 0;
    int64 ServedBytes = 0;
    double ServedReadSeconds = 0.0;
    int32 DownloadChunkSize = 256 * 1024;
    int64 BytesTotal = 0;
    int64 BytesDone = 0;
    int32 FilesReused = 0;
    FString InstalledProject;
};
