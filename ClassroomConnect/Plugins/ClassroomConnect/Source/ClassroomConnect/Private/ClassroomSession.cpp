#include "ClassroomSession.h"
#include "ClassroomConnectLog.h"
#include "ClassroomMarkerActor.h"
#include "ClassroomProjectSync.h"

#include "Async/Async.h"
#include "ConcertClientPersistData.h"
#include "ConcertClientSettings.h"
#include "ConcertMessages.h"
#include "CoreGlobals.h"
#include "Editor.h"
#include "Engine/World.h"
#include "FileHelpers.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"
#include "IConcertClient.h"
#include "IConcertClientPresenceManager.h"
#include "IConcertClientWorkspace.h"
#include "IConcertSyncClient.h"
#include "IMultiUserClientModule.h"
#include "Misc/App.h"
#include "Dom/JsonObject.h"
#include "Misc/CommandLine.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/EngineVersion.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"

#define LOCTEXT_NAMESPACE "ClassroomConnect"

namespace
{
    constexpr float TickSeconds = 0.5f;
    /** How long a leader listens for another computer already hosting the team before starting a server. */
    constexpr double LeaderCheckSeconds = 4.0;
    constexpr double ServerStartTimeoutSeconds = 45.0;
    constexpr double FindHostTimeoutSeconds = 60.0;
    constexpr double RequestIntervalSeconds = 2.0;
    /** After a successful create/join reply, how long Concert may take to report the session as Connected. */
    constexpr double ConnectSettleSeconds = 15.0;
    /**
     * A member who finds no team server, but a leader for the team that refused its join code, is told after this long
     * (servers on the network are normally discovered within a couple of seconds).
     */
    constexpr double JoinCodeVerdictSeconds = 6.0;
    /** While a leader leaves: how long the session's recorded changes must stay unchanged before writing them anyway. */
    constexpr double SaveSettleSeconds = 3.0;

    FString ResponseCodeName(EConcertResponseCode Code) { return StaticEnum<EConcertResponseCode>()->GetNameStringByValue(static_cast<int64>(Code)); }

    /** Presence marker colors, chosen to stand apart from each other in a viewport. Concert's default (white) means "not assigned yet". */
    struct FMarkerColor { const TCHAR* Name; FColor Color; };
    const FMarkerColor MarkerColors[] = {
        { TEXT("Orange"), FColor(0xFF, 0x8C, 0x00) }, { TEXT("Sky Blue"), FColor(0x29, 0xB6, 0xF6) },
        { TEXT("Lime"), FColor(0x76, 0xFF, 0x03) },   { TEXT("Violet"), FColor(0xE0, 0x40, 0xFB) },
        { TEXT("Yellow"), FColor(0xFF, 0xEB, 0x3B) }, { TEXT("Red"), FColor(0xFF, 0x52, 0x52) },
        { TEXT("Aqua"), FColor(0x1D, 0xE9, 0xB6) },   { TEXT("Pink"), FColor(0xFF, 0x80, 0xAB) },
    };
    constexpr int32 MarkerColorCount = UE_ARRAY_COUNT(MarkerColors);
    const TCHAR* MarkerPreferencesSection = TEXT("ClassroomConnect");

    int32 MarkerIndex(const FLinearColor& Color)
    {
        for (int32 Index = 0; Index < MarkerColorCount; ++Index)
            if (Color.Equals(FLinearColor(MarkerColors[Index].Color), 0.002f)) return Index;
        return INDEX_NONE;
    }

    /**
     * Concert treats a session repository as locked while the process id recorded in Repositories.json is running
     * (ConcertServer.cpp: IsApplicationRunning(ProcessId)). When a server dies without unmounting (crash, End Task), Windows
     * can reuse that id for another program, and every later server on this computer fails to create sessions with
     * "Repository locked by another process". Called only before starting a server: if no Multi-User server is running at
     * all, every lock recorded there is stale and is cleared.
     */
    void ClearStaleRepositoryLocks()
    {
        const TCHAR* ServerPrograms[] = { TEXT("UnrealMultiUserSlateServer"), TEXT("UnrealMultiUserServer") };
        for (const TCHAR* Program : ServerPrograms)
            if (FPlatformProcess::IsApplicationRunning(*(FString(Program) + TEXT(".exe")))) return;
        for (const TCHAR* Program : ServerPrograms)
        {
            const FString File = FPaths::Combine(FPaths::EngineDir(), TEXT("Programs"), Program, TEXT("Saved/MultiUser/Repositories.json"));
            FString Text;
            TSharedPtr<FJsonObject> Root;
            const TArray<TSharedPtr<FJsonValue>>* Repositories = nullptr;
            if (!FFileHelper::LoadFileToString(Text, *File) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid()
                || !Root->TryGetArrayField(TEXT("Repositories"), Repositories)) continue;
            int32 Cleared = 0;
            for (const TSharedPtr<FJsonValue>& Value : *Repositories)
            {
                const TSharedPtr<FJsonObject>* Repository = nullptr;
                bool bMounted = false;
                if (!Value.IsValid() || !Value->TryGetObject(Repository) || !(*Repository)->TryGetBoolField(TEXT("bMounted"), bMounted) || !bMounted) continue;
                (*Repository)->SetBoolField(TEXT("bMounted"), false);
                (*Repository)->SetNumberField(TEXT("ProcessId"), 0);
                ++Cleared;
            }
            if (Cleared == 0) continue;
            FString Updated;
            if (FJsonSerializer::Serialize(Root.ToSharedRef(), TJsonWriterFactory<>::Create(&Updated))
                && FFileHelper::SaveStringToFile(Updated, *File, FFileHelper::EEncodingOptions::ForceUnicode))
            {
                UE_LOG(LogClassroomConnect, Log, TEXT("Cleared %d stale Multi-User repository lock(s) left by a server that did not shut down cleanly (%s)."), Cleared, *File);
            }
            else
            {
                UE_LOG(LogClassroomConnect, Warning, TEXT("Could not clear stale Multi-User repository locks in %s."), *File);
            }
        }
    }

    /** The Multi-User server program the editor is configured to start, as IMultiUserClientModule::LaunchConcertServer picks it. */
    FString MultiUserServerPath()
    {
        const TCHAR* Name = GetDefault<UConcertClientConfig>()->ServerType == EConcertServerType::Console ? TEXT("UnrealMultiUserServer") : TEXT("UnrealMultiUserSlateServer");
        FString Path = FPlatformProcess::GenerateApplicationPath(Name, FApp::GetBuildConfiguration());
        if (!FPaths::FileExists(Path) && FApp::GetBuildConfiguration() != EBuildConfiguration::Development)
            Path = FPlatformProcess::GenerateApplicationPath(Name, EBuildConfiguration::Development);
        return Path;
    }

    /**
     * The same arguments IMultiUserClientModule::LaunchConcertServer passes (MultiUserClientModule.cpp
     * GenerateConcertServerCommandLine in 5.8: this editor's UDP/QUIC messaging endpoints and the server name), plus a
     * per-launch config override that makes the server answer discovery only from clients sending JoinKey
     * (UConcertServerConfig::AuthorizedClientKeys, checked in FConcertServer::HandleDiscoverServersEvent).
     * Nothing is written to any config file.
     */
    FString TeamServerArguments(const FString& ServerName, const FString& JoinKey)
    {
        FString Unicast, Multicast, QuicUnicast;
        TArray<FString> Static;
        if (FConfigFile* Engine = GConfig ? GConfig->FindConfigFileWithBaseName(FName(TEXT("Engine"))) : nullptr)
        {
            Engine->GetString(TEXT("/Script/UdpMessaging.UdpMessagingSettings"), TEXT("UnicastEndpoint"), Unicast);
            Engine->GetString(TEXT("/Script/UdpMessaging.UdpMessagingSettings"), TEXT("MulticastEndpoint"), Multicast);
            Engine->GetArray(TEXT("/Script/UdpMessaging.UdpMessagingSettings"), TEXT("StaticEndpoints"), Static);
            Engine->GetString(TEXT("/Script/QuicMessaging.QuicMessagingSettings"), TEXT("UnicastEndpoint"), QuicUnicast);
        }
        // A bound unicast port is moved for the server: to the configured server port, or the editor's port + 1.
        TArray<FString> Parts;
        if (Unicast.ParseIntoArray(Parts, TEXT(":"), false) == 2)
        {
            const int32 ServerPort = GetDefault<UConcertClientConfig>()->ClientSettings.ServerPort;
            if (ServerPort != 0) Unicast = FString::Printf(TEXT("%s:%d"), *Parts[0], ServerPort);
            else if (Parts[1] != TEXT("0")) Unicast = FString::Printf(TEXT("%s:%d"), *Parts[0], FCString::Atoi(*Parts[1]) + 1);
        }
        FString Arguments = TEXT("-UDPMESSAGING_TRANSPORT_UNICAST=") + Unicast + TEXT(" -UDPMESSAGING_TRANSPORT_MULTICAST=") + Multicast;
        if (Static.Num() > 0) Arguments += TEXT(" -UDPMESSAGING_TRANSPORT_STATIC=") + FString::Join(Static, TEXT(","));
        Arguments += TEXT(" -QUICMESSAGING_TRANSPORT_UNICAST=") + QuicUnicast;
        Arguments += FString::Printf(TEXT(" -CONCERTSERVER=\"%s\""), *ServerName);
        if (!JoinKey.IsEmpty()) Arguments += TEXT(" -ini:Engine:[/Script/ConcertServer.ConcertServerConfig]:AuthorizedClientKeys=") + JoinKey;
        Arguments += TEXT(" -QUICMESSAGING_TRANSPORT_ISCLIENT=False");
        return Arguments;
    }

    /** "/Game/Maps/Island.Island" -> "Island"; unsaved levels live under /Temp/ and are not shared with the team. */
    FString LevelLabel(const FString& WorldPath)
    {
        if (WorldPath.IsEmpty()) return FString();
        if (WorldPath.StartsWith(TEXT("/Temp/"))) return TEXT("an unsaved level");
        FString Package = WorldPath;
        int32 Index;
        if (Package.FindChar(TEXT('.'), Index)) Package.LeftInline(Index);
        if (Package.FindLastChar(TEXT('/'), Index)) Package.RightChopInline(Index + 1);
        return Package;
    }
}

FString FClassroomSession::StateName(EClassroomState Value)
{
    switch (Value)
    {
    case EClassroomState::CheckingTeam:   return TEXT("CheckingTeam");
    case EClassroomState::StartingServer: return TEXT("StartingServer");
    case EClassroomState::FindingHost:    return TEXT("FindingHost");
    case EClassroomState::Connecting:     return TEXT("Connecting");
    case EClassroomState::Connected:      return TEXT("Connected");
    case EClassroomState::SavingWork:     return TEXT("SavingWork");
    case EClassroomState::GettingProject: return TEXT("GettingProject");
    case EClassroomState::Reopening:      return TEXT("Reopening");
    case EClassroomState::Failed:         return TEXT("Failed");
    default:                              return TEXT("Idle");
    }
}

void FClassroomSession::Start(const TArray<FClassroomTeam>& InTeams)
{
    Teams = InTeams;
    GConfig->GetBool(MarkerPreferencesSection, TEXT("ShowMarkers"), bShowMarkers, GEditorSettingsIni);
    ProjectSync = MakeShared<FClassroomProjectSync>();
    ProjectSync->Start();
    // An editor reopened on a copied project (ReopenOn) carries the member's join code: Unreal's auto-connect must send
    // its key to discover the leader's server.
    FString CarriedCode;
    if (FParse::Value(FCommandLine::Get(), TEXT("UCCJOINCODE="), CarriedCode))
    {
        JoinCode = ClassroomJoinCode::Normalize(CarriedCode);
        JoinKey = ClassroomJoinCode::Key(JoinCode);
        if (!JoinKey.IsEmpty()) SetClientJoinKey(JoinKey);
    }
    if (!TickerHandle.IsValid())
        TickerHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateSP(this, &FClassroomSession::Tick), TickSeconds);
}

void FClassroomSession::SetClientJoinKey(const FString& Key)
{
    PendingClientKey = Key;
    ApplyClientJoinKey();
}

void FClassroomSession::ApplyClientJoinKey()
{
    const TSharedPtr<IConcertClient, ESPMode::ThreadSafe> Client = GetClient();
    const UConcertClientConfig* Config = Client.IsValid() ? Client->GetConfiguration() : nullptr;
    if (!PendingClientKey.IsSet() || !Config) return; // Retried from Tick until Multi-User's client exists.
    const FString Key = PendingClientKey.GetValue();
    PendingClientKey.Reset();
    if (!OriginalClientKey.IsSet()) OriginalClientKey = Config->ClientSettings.ClientAuthenticationKey;
    if (Config->ClientSettings.ClientAuthenticationKey == Key) return;
    // Change the key on the client's own settings object: its discovery re-reads the key every second
    // (FConcertClient::StartDiscovery), and Unreal's auto-connect keeps a pointer to this object, so it must not be
    // replaced. That object is normally a private copy made at startup (ParseClientSettings). If it is the shared
    // default (Multi-User settings were saved in this editor), configure a copy instead, so the key is never saved.
    if (Config == GetDefault<UConcertClientConfig>())
    {
        UConcertClientConfig* Copy = DuplicateObject(Config, GetTransientPackage());
        Copy->ClientSettings.ClientAuthenticationKey = Key;
        Client->Configure(Copy);
    }
    else
    {
        const_cast<UConcertClientConfig*>(Config)->ClientSettings.ClientAuthenticationKey = Key;
    }
    UE_LOG(LogClassroomConnect, Log, TEXT("Multi-User discovery now %s."), Key.IsEmpty() ? TEXT("sends no join code") : TEXT("sends this editor's join code"));
}

bool FClassroomSession::LaunchTeamServer(const FString& ServerName)
{
    IMultiUserClientModule& MultiUser = IMultiUserClientModule::Get();
    if (JoinKey.IsEmpty())
    {
        FServerLaunchOverrides Overrides;
        Overrides.ServerName = ServerName;
        if (!MultiUser.LaunchConcertServer(Overrides).IsSet()) return false;
        LaunchedServerKey = FString();
        return true;
    }
    // Unreal's LaunchConcertServer cannot pass server settings, so start the same program with the same arguments plus
    // the join key. IsConcertServerRunning and ShutdownConcertServer find it by program name as usual.
    const FString Path = MultiUserServerPath();
    const FString Arguments = TeamServerArguments(ServerName, JoinKey);
    if (!FPaths::FileExists(Path))
    {
        UE_LOG(LogClassroomConnect, Warning, TEXT("Multi-User server program not found: %s"), *Path);
        return false;
    }
    FProcHandle Server = FPlatformProcess::CreateProc(*Path, *Arguments, true, false, false, nullptr, 0, nullptr, nullptr, nullptr);
    if (!Server.IsValid()) return false;
    FPlatformProcess::CloseProc(Server);
    UE_LOG(LogClassroomConnect, Log, TEXT("Started the team server with a join code: %s %s"), *Path, *Arguments.Replace(*JoinKey, TEXT("<join key>")));
    LaunchedServerKey = JoinKey;
    return true;
}

void FClassroomSession::Stop()
{
    if (TickerHandle.IsValid())
    {
        FTSTicker::RemoveTicker(TickerHandle);
        TickerHandle.Reset();
    }
    AttemptToken.Reset();
    RemoveMarkers();
    if (ProjectSync.IsValid()) ProjectSync->Stop();
    ProjectSync.Reset();
}

void FClassroomSession::Host(const FClassroomTeam& InTeam, const FString& InJoinCode) { Begin(InTeam, EClassroomRole::Leader, InJoinCode); }

void FClassroomSession::Join(const FClassroomTeam& InTeam, const FString& InJoinCode) { Begin(InTeam, EClassroomRole::Member, InJoinCode); }

void FClassroomSession::Retry()
{
    if (Team.IsSet()) Begin(Team.GetValue(), Role, JoinCode);
}

void FClassroomSession::Leave()
{
    if (ShouldSaveOnLeave())
    {
        BeginLeaveAndSave();
        return;
    }
    // DisconnectSession(true) shows Unreal's own prompt to keep or discard session changes. Its return value is not
    // trustworthy: Cancel on the owner's persist prompt returns true without disconnecting (MultiUserClientModule.cpp:752-763),
    // so check the real connection afterwards.
    if (IMultiUserClientModule::IsAvailable()) IMultiUserClientModule::Get().DisconnectSession(true);
    if (IsInSession()) return; // The student chose to stay.
    Reset(LOCTEXT("Left", "You left the team session. Your project files stay on this computer."));
}

void FClassroomSession::EndHosting()
{
    if (!IMultiUserClientModule::IsAvailable() || State == EClassroomState::SavingWork) return;
    auto StopServer = [this](bool bWasInSession)
    {
        IMultiUserClientModule::Get().ShutdownConcertServer();
        LaunchedServerKey.Reset();
        const FText Stopped = LOCTEXT("ServerStopped", "This computer's team server was stopped.");
        Enter(EClassroomState::Idle, bWasInSession ? FText::Format(LOCTEXT("Ended", "{0} {1}"), Message, Stopped) : Stopped);
    };
    if (!IsInSession()) return StopServer(false);
    if (ShouldSaveOnLeave())
    {
        // Never stop the server under a connected leader: only once the work is in the project and we have left.
        BeginLeaveAndSave([StopServer](bool bLeft) { if (bLeft) StopServer(true); });
        return;
    }
    Leave();
    if (!IsInSession()) StopServer(true);
}

bool FClassroomSession::ShouldSaveOnLeave() const
{
    return Role == EClassroomRole::Leader && IsInSession();
}

void FClassroomSession::BeginLeaveAndSave(TFunction<void(bool bLeft)> OnDone)
{
    if (State == EClassroomState::SavingWork) return;
    OnLeaveDone = MoveTemp(OnDone);
    StateBeforeSaving = State;
    FText Error;
    if (!SaveChangedAssets(Error)) return FinishLeave(false, Error);
    LastRecordedChanges.Reset();
    LastRecordedChangeSeconds = FPlatformTime::Seconds();
    Enter(EClassroomState::SavingWork, LOCTEXT("SavingWork", "Saving your team's work into this project..."));
}

bool FClassroomSession::SaveChangedAssets(FText& OutError)
{
    // Save every changed asset without prompting. While connected, saves go into the session's sandbox and are sent to
    // the team server; only saved files can be written into the project afterwards. Levels that were never saved (/Temp)
    // are not shared by Multi-User and would open a Save As dialog, so they are skipped.
    SavedForLeave.Reset();
    bSkippedUnsavedLevel = false;
    TArray<UPackage*> Dirty, ToSave;
    FEditorFileUtils::GetDirtyPackages(Dirty);
    for (UPackage* Package : Dirty)
    {
        const FString Name = Package->GetName();
        if (FPackageName::IsTempPackage(Name) || !FPackageName::IsValidLongPackageName(Name))
            bSkippedUnsavedLevel |= Package->ContainsMap();
        else
            ToSave.Add(Package);
    }
    if (ToSave.Num() > 0 && !UEditorLoadingAndSavingUtils::SavePackages(ToSave, /*bOnlyDirty=*/true))
    {
        OutError = LOCTEXT("SaveAssetsFailed", "Some changed assets could not be saved. Check the Message Log, then try again.");
        return false;
    }
    for (UPackage* Package : ToSave) SavedForLeave.Add(Package->GetFName());
    UE_LOG(LogClassroomConnect, Log, TEXT("Saved %d changed assets; waiting for the session to record them."), ToSave.Num());
    return true;
}

void FClassroomSession::TickSavingWork(double Now)
{
    const TSharedPtr<IConcertSyncClient> SyncClient = GetSyncClient();
    const TSharedPtr<IConcertClientWorkspace> Workspace = SyncClient.IsValid() ? SyncClient->GetWorkspace() : nullptr;
    if (!Workspace.IsValid() || !IsInSession())
        return FinishLeave(false, LOCTEXT("DroppedWhileSaving", "The connection to the team server dropped while saving. The team's work is still in the team session: host again, then leave to save it."));

    // A save reaches the session database only after the team server records it, so writing to the project straight
    // after saving finds nothing. Wait until every saved package is recorded, or until the recorded set has stopped
    // growing for a while (Multi-User does not track some asset types; those are written from the save directly).
    TSet<FName> Recorded;
    Recorded.Append(Workspace->GatherSessionChanges());
    if (Recorded.Num() != LastRecordedChanges.Num() || !Recorded.Includes(LastRecordedChanges))
    {
        LastRecordedChanges = Recorded;
        LastRecordedChangeSeconds = Now;
    }
    const bool bAllRecorded = Recorded.Includes(SavedForLeave);
    const bool bSettled = Now - LastRecordedChangeSeconds >= SaveSettleSeconds;
    if (!bAllRecorded && !bSettled) return;
    if (!bAllRecorded)
        UE_LOG(LogClassroomConnect, Log, TEXT("%d saved assets were not recorded by the session; writing them from the save."), SavedForLeave.Difference(Recorded).Num());

    // Write the session's version of every changed package into the project files: what Unreal's "persist the
    // changes?" Yes does, without offering the No that throws the team's work away.
    const TArray<FName> ToPersist = Recorded.Union(SavedForLeave).Array();
    if (ToPersist.Num() > 0)
    {
        FPersistParameters Params;
        Params.PackagesToPersist = ToPersist;
        Params.bShouldMakeWritableIfNoSourceControl = true;
        const FPersistResult Result = Workspace->PersistSessionChanges(Params);
        if (Result.PersistStatus != EPersistStatus::Success)
        {
            TArray<FString> Reasons;
            for (const FText& Reason : Result.FailureReasons) Reasons.Add(Reason.ToString());
            return FinishLeave(false, FText::FromString(Reasons.IsEmpty() ? TEXT("Unreal did not report a reason.") : FString::Join(Reasons, TEXT(" "))));
        }
    }
    UE_LOG(LogClassroomConnect, Log, TEXT("Wrote %d session changes into the project."), ToPersist.Num());

    // Disconnect directly: Multi-User's own DisconnectSession would ask the owner whether to persist, and "No" discards.
    if (const TSharedPtr<IConcertClient, ESPMode::ThreadSafe> Client = GetClient()) Client->DisconnectSession();
    FText Saved = FText::Format(LOCTEXT("SavedAndLeft", "Saved your team's work into this project ({0} changed assets) and left the session."), FText::AsNumber(ToPersist.Num()));
    if (bSkippedUnsavedLevel)
        Saved = FText::Format(LOCTEXT("SkippedUnsaved", "{0} A level that was never saved is not part of the team project and was not kept."), Saved);
    FinishLeave(!IsInSession(), Saved);
}

void FClassroomSession::FinishLeave(bool bLeft, const FText& NewMessage)
{
    if (bLeft)
    {
        Reset(NewMessage);
    }
    else
    {
        UE_LOG(LogClassroomConnect, Warning, TEXT("Leader save failed; staying connected: %s"), *NewMessage.ToString());
        Enter(StateBeforeSaving == EClassroomState::SavingWork ? EClassroomState::Connected : StateBeforeSaving,
            FText::Format(LOCTEXT("SaveFailed", "Your team's work could not be saved into this project, so you are still connected and nothing was thrown away. {0}"), NewMessage));
    }
    TFunction<void(bool)> Done = MoveTemp(OnLeaveDone);
    OnLeaveDone = nullptr;
    if (Done) Done(bLeft);
}

void FClassroomSession::JumpTo(const FGuid& EndpointId)
{
    const TSharedPtr<IConcertSyncClient> SyncClient = GetSyncClient();
    IConcertClientPresenceManager* Presence = SyncClient.IsValid() ? SyncClient->GetPresenceManager() : nullptr;
    if (!Presence) return;
    // Land two meters behind the teammate, facing the way they face, so their marker is in view.
    const FTransform Theirs = Presence->GetPresenceTransform(EndpointId);
    Presence->InitiateJumpToPresence(EndpointId, FTransform(Theirs.GetRotation().GetForwardVector() * -200.0 + FVector(0.0, 0.0, 50.0)));
}

void FClassroomSession::SetShowMarkers(bool bShow)
{
    bShowMarkers = bShow;
    GConfig->SetBool(MarkerPreferencesSection, TEXT("ShowMarkers"), bShowMarkers, GEditorSettingsIni);
    UpdateMarkers();
}

void FClassroomSession::UpdateMarkers()
{
    // Markers live in the editor world only (presence is not reported for Play In Editor worlds).
    UWorld* World = GEditor && !GEditor->PlayWorld ? GEditor->GetEditorWorldContext().World() : nullptr;
    TSet<FGuid> Wanted;
    if (World && bShowMarkers)
        for (const FClassroomParticipant& Row : Participants)
            if (!Row.bIsLocal && Row.bSameLevel) Wanted.Add(Row.EndpointId);

    for (auto It = Markers.CreateIterator(); It; ++It)
    {
        AClassroomMarkerActor* Marker = It.Value().Get();
        if (Marker && Wanted.Contains(It.Key()) && Marker->GetWorld() == World) continue;
        if (Marker && Marker->GetWorld())
        {
            UPackage* Package = Marker->GetWorld()->GetOutermost();
            const bool bWasDirty = Package->IsDirty();
            Marker->Destroy();
            if (!bWasDirty) Package->SetDirtyFlag(false);
        }
        It.RemoveCurrent();
    }

    for (const FClassroomParticipant& Row : Participants)
    {
        if (!Wanted.Contains(Row.EndpointId)) continue;
        TWeakObjectPtr<AClassroomMarkerActor>& Marker = Markers.FindOrAdd(Row.EndpointId);
        if (!Marker.IsValid())
        {
            // Same approach as Unreal's own presence actors: transient, and the level is not left marked as modified.
            const bool bWasDirty = World->GetOutermost()->IsDirty();
            FActorSpawnParameters Params;
            Params.ObjectFlags = RF_Transient | RF_DuplicateTransient;
            Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
            Marker = World->SpawnActor<AClassroomMarkerActor>(Params);
            if (!bWasDirty) World->GetOutermost()->SetDirtyFlag(false);
            if (Marker.IsValid()) UE_LOG(LogClassroomConnect, Log, TEXT("Showing a large marker for %s (%s)."), *Row.Name, *Row.ColorName);
        }
        if (Marker.IsValid()) Marker->Configure(Row.EndpointId, Row.Name, Row.Color);
    }
}

void FClassroomSession::RemoveMarkers()
{
    // During editor shutdown the world is being torn down with its actors; just drop the references.
    if (IsEngineExitRequested())
    {
        Markers.Reset();
        return;
    }
    const bool bWasShowing = bShowMarkers;
    bShowMarkers = false;
    UpdateMarkers();
    bShowMarkers = bWasShowing;
    Markers.Reset();
}

void FClassroomSession::Begin(const FClassroomTeam& InTeam, EClassroomRole InRole, const FString& InJoinCode)
{
    Team = InTeam;
    Role = InRole;
    HostComputer.Reset();
    bRequestInFlight = false;
    LastRequestSeconds = 0.0;
    AttemptToken = MakeShared<uint8>();

    const FEngineVersion& Engine = FEngineVersion::Current();
    // Any 5.8.x: Fab builds the plugin for "5.8". Multi-User itself refuses to join a session created by a different
    // engine version (FConcertEngineVersionInfo::Validate), so mixed patch versions within a team are still caught.
    if (Engine.GetMajor() != 5 || Engine.GetMinor() != 8)
        return Fail(FText::Format(LOCTEXT("WrongEngine", "Classroom Connect needs Unreal Engine 5.8. This editor is {0}."), FText::FromString(Engine.ToString(EVersionComponent::Patch))));
    if (!FApp::HasProjectName())
        return Fail(LOCTEXT("NoProject", "Open your team's project first."));

    const TSharedPtr<IConcertClient, ESPMode::ThreadSafe> Client = GetClient();
    if (!Client.IsValid() || !Client->IsStarted())
        return Fail(LOCTEXT("NoMultiUser", "Multi-User Editing is not running in this editor. Ask your instructor to enable the Multi-User Editing plugin."));

    if (const TSharedPtr<IConcertClientSession> Current = Client->GetCurrentSession())
    {
        if (Current->GetName() != ClassroomTeams::SessionName(InTeam))
            return Fail(FText::Format(LOCTEXT("OtherSession", "This editor is already in session \"{0}\". Click Leave Team first."), FText::FromString(Current->GetName())));
        return Enter(EClassroomState::Connecting, LOCTEXT("AlreadyIn", "Already in this team's session. Checking the connection..."));
    }

    // The join code: Multi-User discovery sends its key (a leader's server with a code answers only that key), and the
    // project copy sends it with every request. Without a code, put back whatever key the client had before.
    JoinCode = ClassroomJoinCode::Normalize(InJoinCode);
    JoinKey = ClassroomJoinCode::Key(JoinCode);
    if (!JoinKey.IsEmpty()) SetClientJoinKey(JoinKey);
    else if (OriginalClientKey.IsSet()) SetClientJoinKey(OriginalClientKey.GetValue());
    UE_LOG(LogClassroomConnect, Log, TEXT("%s %s %s."), InRole == EClassroomRole::Leader ? TEXT("Hosting") : TEXT("Joining"), *InTeam.Id,
        JoinCode.IsEmpty() ? TEXT("without a join code") : TEXT("with a join code"));

    if (!Client->IsDiscoveryEnabled()) Client->StartDiscovery();
    if (InRole == EClassroomRole::Leader)
    {
        // Start hashing the project for teammates now, while the team server starts.
        ProjectSync->StartServing(InTeam.Id, JoinKey);
        ProjectSync->BeginProbe(InTeam.Id, JoinKey);
        Enter(EClassroomState::CheckingTeam, FText::Format(LOCTEXT("Checking", "Checking that no one else is hosting {0}..."), TeamName()));
    }
    else
    {
        ProjectSync->StopServing();
        ProjectSync->Cancel();
        ProjectSync->BeginProbe(InTeam.Id, JoinKey);
        Enter(EClassroomState::FindingHost, FText::Format(LOCTEXT("Finding", "Looking for {0}'s team leader..."), TeamName()));
    }
}

void FClassroomSession::Enter(EClassroomState NewState, const FText& NewMessage)
{
    UE_LOG(LogClassroomConnect, Log, TEXT("[%s %s] %s -> %s: %s"), Team.IsSet() ? *Team->Id : TEXT("-"), Role == EClassroomRole::Leader ? TEXT("Leader") : TEXT("Member"),
        *StateName(State), *StateName(NewState), *NewMessage.ToString());
    State = NewState;
    Message = NewMessage;
    PhaseStartSeconds = FPlatformTime::Seconds();
    // Asking around for the team's leader only matters while looking for one.
    if (ProjectSync.IsValid() && NewState != EClassroomState::CheckingTeam && NewState != EClassroomState::FindingHost) ProjectSync->StopProbe();
    // Not in a team any more: Unreal's own Multi-User browser should discover servers as it did before.
    if (NewState == EClassroomState::Idle && OriginalClientKey.IsSet()) SetClientJoinKey(OriginalClientKey.GetValue());
}

void FClassroomSession::Fail(const FText& Reason)
{
    AttemptToken.Reset();
    bRequestInFlight = false;
    // A leader who did not get the team going stops answering for it, so another leader's check is not misled.
    if (Role == EClassroomRole::Leader && ProjectSync.IsValid() && !IsInSession()) ProjectSync->StopServing();
    Enter(EClassroomState::Failed, Reason);
}

void FClassroomSession::Reset(const FText& NewMessage)
{
    AttemptToken.Reset();
    bRequestInFlight = false;
    HostComputer.Reset();
    bPresenceEnabled = false;
    if (ProjectSync.IsValid())
    {
        ProjectSync->StopServing();
        ProjectSync->Cancel();
    }
    Enter(EClassroomState::Idle, NewMessage);
}

bool FClassroomSession::Tick(float)
{
    const double Now = FPlatformTime::Seconds();
    const TSharedPtr<IConcertClient, ESPMode::ThreadSafe> Client = GetClient();
    if (!Client.IsValid()) return true;
    ApplyClientJoinKey();
    RefreshParticipants(*Client);
    UpdateMarkers();
    if (State == EClassroomState::Idle) AdoptExistingSession(*Client);
    if (!Team.IsSet() || State == EClassroomState::Idle || State == EClassroomState::Failed || State == EClassroomState::Reopening) return true;

    const double Elapsed = Now - PhaseStartSeconds;
    const TSharedPtr<IConcertClientSession> Current = Client->GetCurrentSession();
    const bool bInTeamSession = Current.IsValid() && Current->GetName() == ClassroomTeams::SessionName(*Team) && Current->GetConnectionStatus() == EConcertConnectionStatus::Connected;

    if (State == EClassroomState::SavingWork)
    {
        TickSavingWork(Now);
        return true;
    }
    if (State == EClassroomState::GettingProject)
    {
        TickGettingProject();
        return true;
    }
    if (State == EClassroomState::Connected)
    {
        if (!bInTeamSession) Fail(LOCTEXT("Dropped", "Disconnected from the team session. Click Retry to reconnect."));
        return true;
    }
    if (State == EClassroomState::Connecting)
    {
        if (bInTeamSession)
            Enter(EClassroomState::Connected, FText::Format(LOCTEXT("Connected", "Connected to {0}. Edits you make are shared with your team."), TeamName()));
        else if (!bRequestInFlight && Elapsed > ConnectSettleSeconds && (!Current.IsValid() || Current->GetConnectionStatus() == EConcertConnectionStatus::Disconnected))
            Fail(FText::Format(LOCTEXT("NeverConnected", "Could not finish connecting to {0}. {1}"), TeamName(), Client->GetLastConnectionError().ErrorText));
        return true;
    }

    // Discovery phases: inspect every server currently advertising this team.
    const FString Prefix = ClassroomTeams::ServerPrefix(*Team);
    TArray<FConcertServerInfo> TeamServers;
    for (const FConcertServerInfo& Server : Client->GetKnownServers())
        if (Server.ServerName.StartsWith(Prefix, ESearchCase::CaseSensitive)) TeamServers.Add(Server);
    const bool bCanRequest = !bRequestInFlight && Now - LastRequestSeconds >= RequestIntervalSeconds;

    if (Role == EClassroomRole::Leader)
    {
        const FString OwnName = ClassroomTeams::ServerName(*Team);
        const FConcertServerInfo* Own = TeamServers.FindByPredicate([&OwnName](const FConcertServerInfo& Server) { return Server.ServerName == OwnName; });
        if (const FConcertServerInfo* Other = TeamServers.FindByPredicate([&OwnName](const FConcertServerInfo& Server) { return Server.ServerName != OwnName; }))
        {
            // Two leaders raced: this computer's server would make members see two hosts, so say how to remove it.
            const FText OtherComputer = FText::FromString(Other->ServerName.RightChop(Prefix.Len()));
            Fail(Own
                ? FText::Format(LOCTEXT("AlreadyHostedOwn", "{0} is also hosted from computer {1}. Agree on one Team Leader. If it is not you, click End Session, then choose Team Member."), TeamName(), OtherComputer)
                : FText::Format(LOCTEXT("AlreadyHosted", "{0} is already hosted from computer {1}. Choose Team Member to join it, or pick a different team."), TeamName(), OtherComputer));
            return true;
        }
        // A leader with a different join code (or none) cannot be discovered through Multi-User; the project-copy probe
        // still reaches it. So does a leader with the same code, which discovery would also have found.
        const FString& RefusedBy = ProjectSync->GetProbeRejectedBy();
        const FString& AcceptedBy = ProjectSync->GetProbeAcceptedBy();
        if (State == EClassroomState::CheckingTeam && (!RefusedBy.IsEmpty() || !AcceptedBy.IsEmpty()))
        {
            Fail(!RefusedBy.IsEmpty()
                ? FText::Format(LOCTEXT("AlreadyHostedCode", "{0} is already hosted from computer {1}, with a different join code. If you are on that team, choose Team Member and enter its join code; otherwise pick a different team."), TeamName(), FText::FromString(RefusedBy))
                : FText::Format(LOCTEXT("AlreadyHostedSameCode", "{0} is already hosted from computer {1}. Choose Team Member to join it, or pick a different team."), TeamName(), FText::FromString(AcceptedBy)));
            return true;
        }
        // A server this editor did not start with this join code may answer anyone: never host a coded team on it.
        // (An editor that sends a key still discovers servers that have none.)
        const bool bRunningServerMatches = JoinKey.IsEmpty()
            ? !LaunchedServerKey.IsSet() || LaunchedServerKey->IsEmpty()
            : LaunchedServerKey.IsSet() && LaunchedServerKey.GetValue() == JoinKey;
        const FText ServerOtherCode = LOCTEXT("ServerOtherCode", "The team server already running on this computer was not started with this join code. Click End Session, then Host Project again.");
        if (Own)
        {
            if (!bRunningServerMatches)
            {
                Fail(ServerOtherCode);
                return true;
            }
            HostComputer = FPlatformProcess::ComputerName();
            if (bCanRequest) RequestSessions(*Own);
        }
        else if (State == EClassroomState::CheckingTeam && Elapsed >= LeaderCheckSeconds)
        {
            if (IMultiUserClientModule::Get().IsConcertServerRunning())
            {
                if (!bRunningServerMatches)
                {
                    Fail(ServerOtherCode);
                    return true;
                }
            }
            else
            {
                ClearStaleRepositoryLocks();
                if (!LaunchTeamServer(OwnName))
                {
                    Fail(LOCTEXT("LaunchFailed", "The team server could not start on this computer. Check the Unreal notification, then open View Diagnostics."));
                    return true;
                }
            }
            Enter(EClassroomState::StartingServer, LOCTEXT("Starting", "Starting the team server on this computer..."));
        }
        else if (State == EClassroomState::StartingServer && Elapsed > ServerStartTimeoutSeconds)
        {
            Fail(IMultiUserClientModule::Get().IsConcertServerRunning()
                ? LOCTEXT("WrongServer", "A Multi-User server is running on this computer but is not advertising this team. It may be left over from an earlier session: click End Session, then Host Project again.")
                : LOCTEXT("ServerTimeout", "The team server did not start. Open View Diagnostics for details."));
        }
        return true;
    }

    if (TeamServers.Num() > 1)
    {
        Fail(FText::Format(LOCTEXT("TwoHosts", "More than one computer is hosting {0} ({1} and {2}). Your team should keep one Team Leader; then click Retry."),
            TeamName(), FText::FromString(TeamServers[0].ServerName.RightChop(Prefix.Len())), FText::FromString(TeamServers[1].ServerName.RightChop(Prefix.Len()))));
        return true;
    }
    if (TeamServers.Num() == 1)
    {
        HostComputer = TeamServers[0].ServerName.RightChop(Prefix.Len());
        if (bCanRequest) RequestSessions(TeamServers[0]);
    }
    // No server to be seen, but a leader for this team answered that our join code is missing or wrong: say so now
    // instead of after the find-host timeout.
    const FString& RefusedBy = ProjectSync->GetProbeRejectedBy();
    if (TeamServers.Num() == 0 && !RefusedBy.IsEmpty() && Elapsed >= JoinCodeVerdictSeconds)
    {
        Fail(JoinKey.IsEmpty()
            ? FText::Format(LOCTEXT("NeedsCode", "{0}'s leader on {1} set a join code. Ask your team leader for it, type it under Join code, then click Join Team."), TeamName(), FText::FromString(RefusedBy))
            : FText::Format(LOCTEXT("WrongCode", "Your join code does not match the one {0}'s leader set on {1}. Check it with your team leader, then click Join Team."), TeamName(), FText::FromString(RefusedBy)));
        return true;
    }
    if (Elapsed > FindHostTimeoutSeconds)
    {
        Fail(HostComputer.IsEmpty()
            ? FText::Format(LOCTEXT("NoHost", "Could not find {0}'s team leader. Check that they clicked Host Project, then click Retry. If this keeps happening, open View Diagnostics."), TeamName())
            : FText::Format(LOCTEXT("NoSession", "Found {0}'s leader on {1}, but the team session has not started yet. Click Retry in a moment."), TeamName(), FText::FromString(HostComputer)));
    }
    return true;
}

void FClassroomSession::RequestSessions(const FConcertServerInfo& Server)
{
    const TSharedPtr<IConcertClient, ESPMode::ThreadSafe> Client = GetClient();
    if (!Client.IsValid()) return;
    bRequestInFlight = true;
    LastRequestSeconds = FPlatformTime::Seconds();
    PendingServerName = Server.ServerName;

    const TWeakPtr<FClassroomSession> WeakThis = AsWeak();
    const TWeakPtr<uint8> Token = AttemptToken;
    const FGuid ServerId = Server.AdminEndpointId;
    Client->GetLiveSessions(ServerId).Next([WeakThis, Token, ServerId](FConcertAdmin_GetSessionsResponse Response)
    {
        AsyncTask(ENamedThreads::GameThread, [WeakThis, Token, ServerId, Response = MoveTemp(Response)]
        {
            const TSharedPtr<FClassroomSession> This = WeakThis.Pin();
            if (This.IsValid() && Token.IsValid()) This->HandleSessions(ServerId, Response);
        });
    });
}

void FClassroomSession::HandleSessions(const FGuid& ServerId, const FConcertAdmin_GetSessionsResponse& Response)
{
    bRequestInFlight = false;
    if (!Team.IsSet() || (State != EClassroomState::CheckingTeam && State != EClassroomState::StartingServer && State != EClassroomState::FindingHost)) return;
    const TSharedPtr<IConcertClient, ESPMode::ThreadSafe> Client = GetClient();
    if (!Client.IsValid()) return;

    if (Response.ResponseCode != EConcertResponseCode::Success)
    {
        // Members keep polling until the find-host timeout; a leader's own server should always answer.
        if (Role == EClassroomRole::Leader)
            Fail(FText::Format(LOCTEXT("ListFailed", "The team server did not answer ({0}). Click Retry."), FText::FromString(ResponseCodeName(Response.ResponseCode))));
        return;
    }

    const FString SessionName = ClassroomTeams::SessionName(*Team);
    const FConcertSessionInfo* Existing = Response.Sessions.FindByPredicate([&SessionName](const FConcertSessionInfo& Info) { return Info.SessionName == SessionName; });

    if (Existing && Role == EClassroomRole::Member)
    {
        // Make sure this computer has the leader's exact project first: copy it if missing or outdated, then either
        // join from this editor or reopen Unreal on the copy (see TickGettingProject).
        PendingServerId = ServerId;
        PendingSessionId = Existing->SessionId;
        TeamProjectName = Existing->Settings.ProjectName;
        ProjectSync->BeginGetProject(Team->Id, HostComputer, TeamProjectName, JoinKey);
        Enter(EClassroomState::GettingProject, ProjectSync->GetMessage());
        return;
    }
    // Concert also refuses mismatched projects; checking first gives the leader an actionable message.
    if (Existing && Existing->Settings.ProjectName != FApp::GetProjectName())
    {
        return Fail(FText::Format(LOCTEXT("WrongProject", "{0} is working on project \"{1}\", but this editor has \"{2}\" open. Open the team project, then try again."),
            TeamName(), FText::FromString(Existing->Settings.ProjectName), FText::FromString(FApp::GetProjectName())));
    }

    if (Existing)
    {
        JoinTeamSession(ServerId, Existing->SessionId);
    }
    else if (Role == EClassroomRole::Leader)
    {
        FConcertCreateSessionArgs Args;
        Args.SessionName = SessionName;
        Args.ArchiveNameOverride = SessionName + FDateTime::Now().ToString(TEXT("-%Y%m%d-%H%M"));
        bRequestInFlight = true;
        Enter(EClassroomState::Connecting, FText::Format(LOCTEXT("Creating", "Starting {0}'s session..."), TeamName()));
        Client->CreateSession(ServerId, Args).Next(MakeConnectResultHandler());
    }
    else
    {
        Message = FText::Format(LOCTEXT("WaitingForSession", "Found {0}'s leader on {1}. Waiting for them to finish starting the session..."), TeamName(), FText::FromString(HostComputer));
    }
}

TFunction<void(EConcertResponseCode)> FClassroomSession::MakeConnectResultHandler()
{
    const TWeakPtr<FClassroomSession> WeakThis = AsWeak();
    const TWeakPtr<uint8> Token = AttemptToken;
    return [WeakThis, Token](EConcertResponseCode Code)
    {
        AsyncTask(ENamedThreads::GameThread, [WeakThis, Token, Code]
        {
            const TSharedPtr<FClassroomSession> This = WeakThis.Pin();
            if (This.IsValid() && Token.IsValid()) This->HandleConnectResult(Code);
        });
    };
}

void FClassroomSession::JoinTeamSession(const FGuid& ServerId, const FGuid& SessionId)
{
    const TSharedPtr<IConcertClient, ESPMode::ThreadSafe> Client = GetClient();
    if (!Client.IsValid()) return Fail(LOCTEXT("NoClientForJoin", "Multi-User Editing stopped running in this editor."));
    bRequestInFlight = true;
    Enter(EClassroomState::Connecting, FText::Format(LOCTEXT("Joining", "Joining {0}..."), TeamName()));
    Client->JoinSession(ServerId, SessionId).Next(MakeConnectResultHandler());
}

void FClassroomSession::TickGettingProject()
{
    switch (ProjectSync->GetState())
    {
    case EClassroomProjectState::UpToDate:
        return JoinTeamSession(PendingServerId, PendingSessionId);
    case EClassroomProjectState::Installed:
        return ReopenOn(ProjectSync->GetInstalledProject());
    case EClassroomProjectState::NotShared:
        // The leader's editor does not share projects (older Classroom Connect, or the message was blocked).
        if (FApp::GetProjectName() == TeamProjectName)
        {
            UE_LOG(LogClassroomConnect, Warning, TEXT("%s did not offer the project; joining with this editor's copy of %s unchecked."), *HostComputer, *TeamProjectName);
            return JoinTeamSession(PendingServerId, PendingSessionId);
        }
        return Fail(FText::Format(LOCTEXT("ProjectNotShared", "{0} is working on project \"{1}\", which is not on this computer, and the leader's computer did not send it. Make sure the leader has the same version of Classroom Connect, or copy the project and open it, then try again."),
            TeamName(), FText::FromString(TeamProjectName)));
    case EClassroomProjectState::Failed:
        return Fail(ProjectSync->GetMessage());
    default:
        Message = ProjectSync->GetMessage();
        return;
    }
}

void FClassroomSession::ReopenOn(const FString& ProjectFile)
{
    // Open the verified copy in a new editor that joins the session by itself (Unreal's own -CONCERTAUTOCONNECT; the
    // session exists, so auto-connect will not create one), then close this editor through the normal close path.
    // Start in the level the leader has open, so the student sees the team and their edits immediately.
    const FString Level = ProjectSync->GetLeaderLevel();
    FString Arguments = FString::Printf(TEXT("\"%s\"%s -CONCERTAUTOCONNECT -CONCERTSERVER=%s -CONCERTSESSION=%s -UCCROLE=Member"),
        *ProjectFile, Level.IsEmpty() ? TEXT("") : *(TEXT(" ") + Level), *PendingServerName, *ClassroomTeams::SessionName(*Team));
    for (const TCHAR* Carried : { TEXT("nullrhi"), TEXT("unattended"), TEXT("nosplash"), TEXT("nosound"), TEXT("NoLiveCoding") })
        if (FParse::Param(FCommandLine::Get(), Carried)) Arguments += FString(TEXT(" -")) + Carried;
    FString ProjectsRoot;
    if (FParse::Value(FCommandLine::Get(), TEXT("UCCPROJECTS="), ProjectsRoot)) Arguments += FString::Printf(TEXT(" -UCCPROJECTS=\"%s\""), *ProjectsRoot);
    // The new editor's auto-connect must send the join code's key to discover the leader (see Start). Normalized codes
    // are letters and digits only, so they cannot break the command line.
    if (!JoinCode.IsEmpty()) Arguments += TEXT(" -UCCJOINCODE=") + JoinCode;

    FProcHandle Editor = FPlatformProcess::CreateProc(FPlatformProcess::ExecutablePath(), *Arguments, true, false, false, nullptr, 0, nullptr, nullptr);
    if (!Editor.IsValid())
        return Fail(FText::Format(LOCTEXT("ReopenFailed", "The team project is ready at {0}, but Unreal could not be started on it. Open it from there."), FText::FromString(ProjectFile)));
    FPlatformProcess::CloseProc(Editor);
    UE_LOG(LogClassroomConnect, Log, TEXT("Opened %s in a new editor (%s); closing this one."), *ProjectFile, *Arguments);
    Enter(EClassroomState::Reopening, FText::Format(LOCTEXT("Reopening", "Opening {0} in a new Unreal window, which joins {1} by itself. This window will close."),
        FText::FromString(FPaths::GetBaseFilename(ProjectFile)), TeamName()));
    GEngine->DeferredCommands.Add(TEXT("CLOSE_SLATE_MAINFRAME"));
}

float FClassroomSession::GetTransferProgress() const
{
    return State == EClassroomState::GettingProject && ProjectSync.IsValid() ? ProjectSync->GetProgress() : -1.f;
}

void FClassroomSession::AdoptExistingSession(const IConcertClient& Client)
{
    // An editor reopened by ReopenOn (or joined through Unreal's own Multi-User browser) connects without Begin(); pick up
    // the team from the session name so the panel, colors and markers work.
    const TSharedPtr<IConcertClientSession> Current = Client.GetCurrentSession();
    if (!Current.IsValid() || Current->GetConnectionStatus() != EConcertConnectionStatus::Connected || !Current->GetName().StartsWith(TEXT("UCC-"))) return;
    const FString Id = Current->GetName().RightChop(4);
    const FClassroomTeam* Found = Teams.FindByPredicate([&Id](const FClassroomTeam& Candidate) { return Candidate.Id == Id; });
    if (!Found) return;
    Team = *Found;
    // An editor reopened for a member says so on its command line; otherwise a local team server means this is the leader.
    FString RoleName;
    if (FParse::Value(FCommandLine::Get(), TEXT("UCCROLE="), RoleName))
        Role = RoleName == TEXT("Leader") ? EClassroomRole::Leader : EClassroomRole::Member;
    else
        Role = IMultiUserClientModule::IsAvailable() && IMultiUserClientModule::Get().IsConcertServerRunning() ? EClassroomRole::Leader : EClassroomRole::Member;
    if (Role == EClassroomRole::Leader) ProjectSync->StartServing(Found->Id, JoinKey);
    Enter(EClassroomState::Connected, FText::Format(LOCTEXT("Adopted", "Connected to {0}. Edits you make are shared with your team."), TeamName()));
}

void FClassroomSession::HandleConnectResult(EConcertResponseCode Code)
{
    bRequestInFlight = false;
    if (State != EClassroomState::Connecting) return;
    if (Code == EConcertResponseCode::Success)
        return Enter(EClassroomState::Connecting, LOCTEXT("Syncing", "Reached the team server. Loading your team's latest changes..."));

    const TSharedPtr<IConcertClient, ESPMode::ThreadSafe> Client = GetClient();
    const FText Detail = Client.IsValid() ? Client->GetLastConnectionError().ErrorText : FText::GetEmpty();
    if (Role == EClassroomRole::Leader && Detail.ToString().Contains(TEXT("Repository locked")))
        return Fail(LOCTEXT("RepositoryLocked", "The team server's session storage is still locked by an earlier server that did not close properly. Click End Session, then Host Project again (Classroom Connect clears the stale lock before starting a new server)."));
    Fail(FText::Format(LOCTEXT("ConnectFailed", "Could not connect to {0} ({1}). {2}"), TeamName(), FText::FromString(ResponseCodeName(Code)), Detail));
}

void FClassroomSession::RefreshParticipants(const IConcertClient& Client)
{
    TArray<FClassroomParticipant> Current;
    const TSharedPtr<IConcertClientSession> Session = Client.GetCurrentSession();
    if (Session.IsValid() && Session->GetConnectionStatus() == EConcertConnectionStatus::Connected)
    {
        const TSharedPtr<IConcertSyncClient> SyncClient = GetSyncClient();
        IConcertClientPresenceManager* Presence = SyncClient.IsValid() ? SyncClient->GetPresenceManager() : nullptr;
        if (Presence && !bPresenceEnabled)
        {
            Presence->SetPresenceEnabled(true);
            bPresenceEnabled = true;
        }
        AssignMarkerColor(*Session);

        EEditorPlayMode PlayMode;
        const FGuid LocalId = Session->GetSessionClientEndpointId();
        const FString LocalWorld = Presence ? Presence->GetPresenceWorldPath(LocalId, PlayMode) : FString();
        auto Describe = [&](const FGuid& Id, const FConcertClientInfo& Info, bool bIsLocal)
        {
            FClassroomParticipant Row;
            Row.EndpointId = Id;
            Row.Name = Info.DisplayName;
            Row.Computer = Info.DeviceName;
            Row.Color = Info.AvatarColor;
            const int32 Marker = MarkerIndex(Info.AvatarColor);
            Row.ColorName = Marker == INDEX_NONE ? TEXT("not assigned") : MarkerColors[Marker].Name;
            Row.bIsLocal = bIsLocal;
            const FString World = Presence ? (bIsLocal ? LocalWorld : Presence->GetPresenceWorldPath(Id, PlayMode)) : FString();
            Row.Level = LevelLabel(World);
            Row.bSameLevel = !bIsLocal && !World.IsEmpty() && World == LocalWorld;
            return Row;
        };
        // GetSessionClients() lists everyone except the local editor.
        Current.Add(Describe(LocalId, Session->GetLocalClientInfo(), true));
        for (const FConcertSessionClientInfo& Other : Session->GetSessionClients())
            Current.Add(Describe(Other.ClientEndpointId, Other.ClientInfo, false));
    }
    if (Current == Participants) return;

    TArray<FString> Lines;
    for (const FClassroomParticipant& Row : Current)
        Lines.Add(FString::Printf(TEXT("%s (%s%s, marker %s, level %s)"), *Row.Name, Row.bIsLocal ? TEXT("you, ") : TEXT(""), *Row.Computer, *Row.ColorName, Row.Level.IsEmpty() ? TEXT("unknown") : *Row.Level));
    UE_LOG(LogClassroomConnect, Log, TEXT("Connected students (%d): %s"), Current.Num(), *FString::Join(Lines, TEXT("; ")));
    Participants = MoveTemp(Current);
    ++ParticipantsRevision;
}

void FClassroomSession::AssignMarkerColor(IConcertClientSession& Session)
{
    // Every student starts with Concert's default white marker. Pick a palette color nobody else in the session uses.
    // If two students raced to the same color, the one with the greater endpoint id moves, so the choice converges.
    const FGuid LocalId = Session.GetSessionClientEndpointId();
    const int32 Current = MarkerIndex(Session.GetLocalClientInfo().AvatarColor);
    TSet<int32> Taken;
    bool bMustMove = Current == INDEX_NONE;
    for (const FConcertSessionClientInfo& Other : Session.GetSessionClients())
    {
        const int32 Theirs = MarkerIndex(Other.ClientInfo.AvatarColor);
        if (Theirs == INDEX_NONE) continue;
        Taken.Add(Theirs);
        if (Theirs == Current && Other.ClientEndpointId < LocalId) bMustMove = true;
    }
    if (!bMustMove) return;

    int32 Preferred = INDEX_NONE;
    GConfig->GetInt(MarkerPreferencesSection, TEXT("MarkerColor"), Preferred, GEditorSettingsIni);
    int32 Choice = Preferred >= 0 && Preferred < MarkerColorCount && !Taken.Contains(Preferred) ? Preferred : INDEX_NONE;
    for (int32 Index = 0; Choice == INDEX_NONE && Index < MarkerColorCount; ++Index)
        if (!Taken.Contains(Index)) Choice = Index;
    if (Choice == INDEX_NONE) Choice = GetTypeHash(LocalId) % MarkerColorCount; // More than 8 students: colors repeat.
    if (Choice == Current) return;

    FConcertClientInfoUpdate Update;
    Update.AvatarColor = FLinearColor(MarkerColors[Choice].Color);
    Session.UpdateLocalClientInfo(Update);
    GConfig->SetInt(MarkerPreferencesSection, TEXT("MarkerColor"), Choice, GEditorSettingsIni);
    UE_LOG(LogClassroomConnect, Log, TEXT("Your marker color: %s"), MarkerColors[Choice].Name);
}

bool FClassroomSession::IsInSession() const
{
    const TSharedPtr<IConcertClient, ESPMode::ThreadSafe> Client = GetClient();
    const TSharedPtr<IConcertClientSession> Session = Client.IsValid() ? Client->GetCurrentSession() : nullptr;
    return Session.IsValid() && Session->GetConnectionStatus() != EConcertConnectionStatus::Disconnected;
}

TArray<FString> FClassroomSession::GetDiagnostics() const
{
    TArray<FString> Lines;
    Lines.Add(TEXT("Engine: ") + FEngineVersion::Current().ToString());
    Lines.Add(TEXT("Project: ") + FString(FApp::GetProjectName()));
    Lines.Add(TEXT("This computer: ") + FString(FPlatformProcess::ComputerName()));
    if (Team.IsSet())
    {
        Lines.Add(FString::Printf(TEXT("Team session: %s (this computer hosts as %s)"), *ClassroomTeams::SessionName(*Team), *ClassroomTeams::ServerName(*Team)));
        Lines.Add(JoinCode.IsEmpty() ? TEXT("Join code: none (anyone on the network can find and join this team)") : TEXT("Join code: set (only editors with the same code can find this team's server)"));
    }

    const TSharedPtr<IConcertClient, ESPMode::ThreadSafe> Client = GetClient();
    if (!Client.IsValid())
    {
        Lines.Add(TEXT("Multi-User client: unavailable (MultiUserClient plugin not loaded)"));
        return Lines;
    }
    Lines.Add(FString::Printf(TEXT("Multi-User client: %s, discovery %s"), Client->IsStarted() ? TEXT("started") : TEXT("not started"), Client->IsDiscoveryEnabled() ? TEXT("on") : TEXT("off")));
    Lines.Add(FString::Printf(TEXT("Local Multi-User server running: %s"), IMultiUserClientModule::Get().IsConcertServerRunning() ? TEXT("yes") : TEXT("no")));
    const TArray<FConcertServerInfo> Servers = Client->GetKnownServers();
    Lines.Add(FString::Printf(TEXT("Servers discovered: %d"), Servers.Num()));
    for (const FConcertServerInfo& Server : Servers) Lines.Add(TEXT("  ") + Server.ServerName);
    if (Servers.Num() == 0)
        Lines.Add(TEXT("No servers found. The firewall on this or the leader's computer may block Unreal Multi-User Editing: IT can run Scripts/Check-ClassroomNetwork.ps1 (see Docs/NETWORK.md). A leader with a join code is also invisible until you enter that code."));
    const FConcertConnectionError Error = Client->GetLastConnectionError();
    if (Error.ErrorCode != 0)
        Lines.Add(FString::Printf(TEXT("Last connection error %u: %s"), Error.ErrorCode, *Error.ErrorText.ToString()));
    if (ProjectSync.IsValid()) Lines.Append(ProjectSync->GetDiagnostics());
    return Lines;
}

TSharedPtr<IConcertSyncClient> FClassroomSession::GetSyncClient() const
{
    return IMultiUserClientModule::IsAvailable() ? IMultiUserClientModule::Get().GetClient() : nullptr;
}

TSharedPtr<IConcertClient, ESPMode::ThreadSafe> FClassroomSession::GetClient() const
{
    const TSharedPtr<IConcertSyncClient> SyncClient = GetSyncClient();
    if (!SyncClient.IsValid()) return nullptr;
    return SyncClient->GetConcertClient();
}

FText FClassroomSession::TeamName() const { return Team.IsSet() ? Team->DisplayName : FText::GetEmpty(); }

#undef LOCTEXT_NAMESPACE
