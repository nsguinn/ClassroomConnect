#include "ClassroomProjectSync.h"
#include "ClassroomConnectLog.h"

#include "Algo/AllOf.h"
#include "Algo/Reverse.h"
#include "Async/Async.h"
#include "Containers/ArrayBuilder.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Editor.h"
#include "Engine/World.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "Misc/PackageName.h"
#include "UObject/Package.h"
#include "HAL/PlatformFileManager.h"
#include "HAL/PlatformProcess.h"
#include "MessageEndpoint.h"
#include "MessageEndpointBuilder.h"
#include "Misc/App.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"

#include "Windows/AllowWindowsPlatformTypes.h"
#include <bcrypt.h>
#include "Windows/HideWindowsPlatformTypes.h"

#define LOCTEXT_NAMESPACE "ClassroomConnect"

/** Streaming SHA-256 using Windows CNG (the engine only offers a one-shot SHA-256 over a single buffer). */
class FClassroomSha256
{
public:
    FClassroomSha256()
    {
        if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&Algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0)))
            BCryptCreateHash(Algorithm, &Hash, nullptr, 0, nullptr, 0, 0);
    }
    ~FClassroomSha256()
    {
        if (Hash) BCryptDestroyHash(Hash);
        if (Algorithm) BCryptCloseAlgorithmProvider(Algorithm, 0);
    }
    bool IsValid() const { return Hash != nullptr; }
    void Update(const uint8* Data, int64 Size)
    {
        while (Size > 0)
        {
            const ULONG Part = static_cast<ULONG>(FMath::Min<int64>(Size, MAX_int32));
            BCryptHashData(Hash, const_cast<PUCHAR>(Data), Part, 0);
            Data += Part;
            Size -= Part;
        }
    }
    FString Finish()
    {
        uint8 Digest[32] = {};
        BCryptFinishHash(Hash, Digest, sizeof(Digest), 0);
        return BytesToHex(Digest, sizeof(Digest));
    }

private:
    BCRYPT_ALG_HANDLE Algorithm = nullptr;
    BCRYPT_HASH_HANDLE Hash = nullptr;
};

namespace
{
    TAutoConsoleVariable<int32> CVarCopyChunkKB(TEXT("UCC.CopyChunkKB"), 256,
        TEXT("Classroom Connect project copy: chunk size in KB (64-1024), read when a download starts."));
    TAutoConsoleVariable<int32> CVarCopyChunksInFlight(TEXT("UCC.CopyChunksInFlight"), 8,
        TEXT("Classroom Connect project copy: chunks requested at once across all files. Unreal UDP messaging halves its send window on packet loss, so asking for much more than about 2 MB at a time is slower, not faster."));
    constexpr int32 MaxChunkSize = 1024 * 1024;
    constexpr int32 ParallelFiles = 16;
    constexpr int32 ManifestPageSize = 400;
    constexpr double AskIntervalSeconds = 2.0;
    constexpr double AskTimeoutSeconds = 20.0;
    constexpr double RequestTimeoutSeconds = 10.0;
    constexpr int32 MaxAttempts = 5;
    const TCHAR* MarkerPath = TEXT("Saved/ClassroomConnect/Revision.txt");

    // Same exclusions as the command-line helper: generated or per-machine folders, and debug symbols.
    bool IsExcludedDirectory(const FString& Name)
    {
        for (const TCHAR* Excluded : { TEXT("Saved"), TEXT("Intermediate"), TEXT("DerivedDataCache"), TEXT(".git"), TEXT(".vs"), TEXT(".idea") })
            if (Name.Equals(Excluded, ESearchCase::IgnoreCase)) return true;
        return false;
    }

    bool IsExcludedFile(const FString& Name) { return Name.EndsWith(TEXT(".pdb"), ESearchCase::IgnoreCase); }

    void ListFiles(const FString& Root, const FString& Relative, TArray<FString>& Out)
    {
        TArray<FString> Directories;
        FPlatformFileManager::Get().GetPlatformFile().IterateDirectory(*(Relative.IsEmpty() ? Root : FPaths::Combine(Root, Relative)), [&](const TCHAR* Path, bool bIsDirectory)
        {
            const FString Name = FPaths::GetCleanFilename(Path);
            const FString Child = Relative.IsEmpty() ? Name : Relative + TEXT("/") + Name;
            if (bIsDirectory) { if (!IsExcludedDirectory(Name)) Directories.Add(Child); }
            else if (!IsExcludedFile(Name)) Out.Add(Child);
            return true;
        });
        for (const FString& Directory : Directories) ListFiles(Root, Directory, Out);
    }

    bool HashFile(const FString& Path, FString& OutHash, int64& OutSize)
    {
        TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*Path));
        FClassroomSha256 Sha;
        if (!File || !Sha.IsValid()) return false;
        OutSize = File->Size();
        TArray<uint8> Buffer;
        Buffer.SetNumUninitialized(1024 * 1024);
        for (int64 Left = OutSize; Left > 0;)
        {
            const int64 Count = FMath::Min<int64>(Left, Buffer.Num());
            if (!File->Read(Buffer.GetData(), Count)) return false;
            Sha.Update(Buffer.GetData(), Count);
            Left -= Count;
        }
        OutHash = Sha.Finish();
        return true;
    }

    bool MatchesEntry(const FString& Path, const FClassroomManifestEntry& Entry)
    {
        if (IFileManager::Get().FileSize(*Path) != Entry.Size) return false;
        FString Hash;
        int64 Size = 0;
        return HashFile(Path, Hash, Size) && Size == Entry.Size && Hash == Entry.Sha256;
    }

    /** Canonical SHA-256 over every entry, so two computers agree on a revision only if every file agrees. */
    FString ManifestRevision(const TArray<FClassroomManifestEntry>& Entries)
    {
        FClassroomSha256 Sha;
        for (const FClassroomManifestEntry& Entry : Entries)
        {
            const FTCHARToUTF8 Line(*FString::Printf(TEXT("%s\n%lld\n%s\n"), *Entry.Path, Entry.Size, *Entry.Sha256));
            Sha.Update(reinterpret_cast<const uint8*>(Line.Get()), Line.Length());
        }
        return Sha.Finish();
    }

    FClassroomManifestResult BuildManifest(const FString& Root)
    {
        FClassroomManifestResult Result;
        TArray<FString> Files;
        ListFiles(Root, FString(), Files);
        Files.Sort([](const FString& A, const FString& B) { return A.Compare(B, ESearchCase::CaseSensitive) < 0; });
        for (const FString& Relative : Files)
        {
            FClassroomManifestEntry Entry;
            Entry.Path = Relative;
            if (HashFile(FPaths::Combine(Root, Relative), Entry.Sha256, Entry.Size)) Result.Entries.Add(MoveTemp(Entry));
            else Result.Skipped.Add(Relative);
        }
        return Result;
    }

    /** Refuses anything that could escape the destination folder or is not a plain Windows file name. */
    bool IsSafeRelativePath(const FString& Path)
    {
        if (Path.IsEmpty() || Path.Len() > 400 || Path.Contains(TEXT("\\")) || Path.Contains(TEXT(":")) || Path.StartsWith(TEXT("/"))) return false;
        TArray<FString> Parts;
        Path.ParseIntoArray(Parts, TEXT("/"), false);
        if (Parts.Num() == 0 || IsExcludedDirectory(Parts[0])) return false;
        for (const FString& Part : Parts)
        {
            if (Part.IsEmpty() || Part == TEXT(".") || Part == TEXT("..") || Part.EndsWith(TEXT(".")) || Part.EndsWith(TEXT(" "))) return false;
            for (const TCHAR C : Part) if (C < 32 || FCString::Strchr(TEXT("<>\"|?*"), C)) return false;
            FString Stem = Part;
            int32 Dot;
            if (Stem.FindChar(TEXT('.'), Dot)) Stem.LeftInline(Dot);
            Stem.ToUpperInline();
            if (Stem == TEXT("CON") || Stem == TEXT("PRN") || Stem == TEXT("AUX") || Stem == TEXT("NUL")) return false;
            if (Stem.Len() == 4 && (Stem.StartsWith(TEXT("COM")) || Stem.StartsWith(TEXT("LPT"))) && FChar::IsDigit(Stem[3])) return false;
        }
        return true;
    }

    bool IsHex64(const FString& Value)
    {
        if (Value.Len() != 64) return false;
        for (const TCHAR C : Value) if (!FChar::IsHexDigit(C)) return false;
        return true;
    }

    FText Megabytes(int64 Bytes) { return FText::AsNumber(FMath::RoundToInt(Bytes / (1024.0 * 1024.0))); }

    /**
     * Sends with UDP messaging's reliable delivery. The default (EMessageFlags::None) is unreliable: losing any one of
     * the few hundred packets a chunk is split into drops the whole message, which stalled copies until a resend timeout.
     */
    template<typename MessageType>
    void SendReliable(const TSharedPtr<FMessageEndpoint, ESPMode::ThreadSafe>& Endpoint, MessageType* Message, const FMessageAddress& Recipient)
    {
        Endpoint->Send(Message, MessageType::StaticStruct(), EMessageFlags::Reliable, nullptr, TArrayBuilder<FMessageAddress>().Add(Recipient), FTimespan::Zero(), FDateTime::MaxValue());
    }

    const TCHAR* WrongJoinCodeError = TEXT("Your join code does not match the one the team leader set.");
}

// ---------------------------------------------------------------------------------------------------------------------
// Join codes

FString ClassroomJoinCode::Normalize(const FString& Code)
{
    FString Result;
    for (const TCHAR C : Code)
        if (Result.Len() < MaxLength && ((C >= TEXT('a') && C <= TEXT('z')) || (C >= TEXT('A') && C <= TEXT('Z')) || (C >= TEXT('0') && C <= TEXT('9'))))
            Result.AppendChar(FChar::ToUpper(C));
    return Result;
}

FString ClassroomJoinCode::Key(const FString& NormalizedCode)
{
    if (NormalizedCode.IsEmpty()) return FString();
    FClassroomSha256 Sha;
    const FTCHARToUTF8 Text(*(TEXT("ClassroomConnect join code:") + NormalizedCode));
    Sha.Update(reinterpret_cast<const uint8*>(Text.Get()), Text.Length());
    return TEXT("UCC1-") + Sha.Finish().Left(32);
}

FString ClassroomJoinCode::Make()
{
    const TCHAR* Alphabet = TEXT("ABCDEFGHJKMNPQRSTUVWXYZ23456789");
    const int32 AlphabetSize = FCString::Strlen(Alphabet);
    uint8 Random[6] = {};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, Random, sizeof(Random), BCRYPT_USE_SYSTEM_PREFERRED_RNG)))
        for (uint8& Byte : Random) Byte = static_cast<uint8>(FMath::Rand());
    FString Code;
    for (const uint8 Byte : Random) Code.AppendChar(Alphabet[Byte % AlphabetSize]);
    return Code;
}

void FClassroomProjectSync::Start()
{
    Endpoint = FMessageEndpoint::Builder(TEXT("ClassroomConnectProjects"))
        .ReceivingOnThread(ENamedThreads::GameThread)
        .Handling<FClassroomProjectQuery>(this, &FClassroomProjectSync::HandleQuery)
        .Handling<FClassroomManifestRequest>(this, &FClassroomProjectSync::HandleManifestRequest)
        .Handling<FClassroomChunkRequest>(this, &FClassroomProjectSync::HandleChunkRequest)
        .Handling<FClassroomProjectOffer>(this, &FClassroomProjectSync::HandleOffer)
        .Handling<FClassroomManifestPage>(this, &FClassroomProjectSync::HandleManifestPage)
        .Handling<FClassroomChunkReply>(this, &FClassroomProjectSync::HandleChunkReply)
        .Build();
    if (Endpoint.IsValid()) Endpoint->Subscribe<FClassroomProjectQuery>();
    else UE_LOG(LogClassroomConnect, Warning, TEXT("No message bus: project copying between computers is unavailable."));
    TickerHandle = FTSTicker::GetCoreTicker().AddTicker(TEXT("ClassroomConnectProjectSync"), 0.f, [this](float)
    {
        Tick(FPlatformTime::Seconds());
        return true;
    });
}

void FClassroomProjectSync::Stop()
{
    if (TickerHandle.IsValid()) FTSTicker::RemoveTicker(TickerHandle);
    TickerHandle.Reset();
    Cancel();
    StopServing();
    Endpoint.Reset();
}

FString FClassroomProjectSync::ProjectsRoot()
{
    FString Root;
    if (!FParse::Value(FCommandLine::Get(), TEXT("UCCPROJECTS="), Root) || Root.IsEmpty())
        Root = FPaths::Combine(FPlatformProcess::UserDir(), TEXT("Unreal Projects"), TEXT("ClassroomConnect"));
    return FPaths::ConvertRelativePathToFull(Root);
}

FString FClassroomProjectSync::ReadInstalledRevision(const FString& ProjectDir)
{
    FString Revision;
    FFileHelper::LoadFileToString(Revision, *FPaths::Combine(ProjectDir, MarkerPath));
    return Revision.TrimStartAndEnd();
}

// ---------------------------------------------------------------------------------------------------------------------
// Leader

void FClassroomProjectSync::StartServing(const FString& InTeamId, const FString& InJoinKey)
{
    ServeTeamId = InTeamId;
    ServeJoinKey = InJoinKey;
    const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    if (Dir == ServeDir && (bServeReady || ServeBuild.IsValid())) return; // Already hashed (or hashing) this project.
    ServeDir = Dir;
    ServeProjectName = FApp::GetProjectName();
    bServeReady = false;
    UE_LOG(LogClassroomConnect, Log, TEXT("Preparing %s to share with team %s..."), *ServeProjectName, *ServeTeamId);
    ServeBuild = Async(EAsyncExecution::ThreadPool, [Dir] { return BuildManifest(Dir); });
}

void FClassroomProjectSync::StopServing()
{
    ServeTeamId.Reset();
    ServeJoinKey.Reset();
}

void FClassroomProjectSync::HandleQuery(const FClassroomProjectQuery& Query, const TSharedRef<IMessageContext, ESPMode::ThreadSafe>& Context)
{
    if (ServeTeamId.IsEmpty() || Query.TeamId != ServeTeamId || !Endpoint.IsValid() || Context->GetSender() == Endpoint->GetAddress()) return;
    if (Query.JoinKey != ServeJoinKey)
    {
        // Say only that this team is hosted here with a different code, so the student can fix the code.
        FClassroomProjectOffer* Refusal = FMessageEndpoint::MakeMessage<FClassroomProjectOffer>();
        Refusal->TeamId = ServeTeamId;
        Refusal->Computer = FPlatformProcess::ComputerName();
        Refusal->bJoinCodeRejected = true;
        SendReliable(Endpoint, Refusal, Context->GetSender());
        return;
    }
    if (!bServeReady) return;
    FClassroomProjectOffer* Reply = FMessageEndpoint::MakeMessage<FClassroomProjectOffer>();
    Reply->TeamId = ServeTeamId;
    Reply->ProjectName = ServeProjectName;
    Reply->Revision = ServeRevision;
    Reply->Computer = FPlatformProcess::ComputerName();
    if (GEditor && GEditor->GetEditorWorldContext().World())
    {
        const FString Level = GEditor->GetEditorWorldContext().World()->GetOutermost()->GetName();
        if (FPackageName::IsValidLongPackageName(Level) && !FPackageName::IsTempPackage(Level)) Reply->Level = Level;
    }
    Reply->FileCount = ServeEntries.Num();
    Reply->TotalBytes = ServeBytes;
    SendReliable(Endpoint, Reply, Context->GetSender());
}

void FClassroomProjectSync::HandleManifestRequest(const FClassroomManifestRequest& Request, const TSharedRef<IMessageContext, ESPMode::ThreadSafe>& Context)
{
    if (!Endpoint.IsValid() || ServeTeamId.IsEmpty() || Request.TeamId != ServeTeamId) return;
    FClassroomManifestPage* Reply = FMessageEndpoint::MakeMessage<FClassroomManifestPage>();
    Reply->TeamId = ServeTeamId;
    Reply->Revision = Request.Revision;
    Reply->Page = Request.Page;
    Reply->PageCount = FMath::Max(1, FMath::DivideAndRoundUp(ServeEntries.Num(), ManifestPageSize));
    if (Request.JoinKey != ServeJoinKey)
        Reply->Error = WrongJoinCodeError;
    else if (!bServeReady || Request.Revision != ServeRevision || Request.Page < 0 || Request.Page >= Reply->PageCount)
        Reply->Error = TEXT("This computer is no longer sharing that version of the project.");
    else
        for (int32 Index = Request.Page * ManifestPageSize; Index < FMath::Min(ServeEntries.Num(), (Request.Page + 1) * ManifestPageSize); ++Index)
            Reply->Entries.Add(ServeEntries[Index]);
    SendReliable(Endpoint, Reply, Context->GetSender());
}

void FClassroomProjectSync::HandleChunkRequest(const FClassroomChunkRequest& Request, const TSharedRef<IMessageContext, ESPMode::ThreadSafe>& Context)
{
    if (!Endpoint.IsValid() || ServeTeamId.IsEmpty() || Request.TeamId != ServeTeamId) return;
    FClassroomChunkReply* Reply = FMessageEndpoint::MakeMessage<FClassroomChunkReply>();
    Reply->TeamId = ServeTeamId;
    Reply->Revision = Request.Revision;
    Reply->FileIndex = Request.FileIndex;
    Reply->Offset = Request.Offset;
    // Only files listed in this computer's own manifest can be read, by index; requests never name a path.
    if (Request.JoinKey != ServeJoinKey)
    {
        Reply->Error = WrongJoinCodeError;
    }
    else if (!bServeReady || Request.Revision != ServeRevision || !ServeEntries.IsValidIndex(Request.FileIndex))
    {
        Reply->Error = TEXT("This computer is no longer sharing that version of the project.");
    }
    else
    {
        const FClassroomManifestEntry& Entry = ServeEntries[Request.FileIndex];
        TUniquePtr<IFileHandle> File(FPlatformFileManager::Get().GetPlatformFile().OpenRead(*FPaths::Combine(ServeDir, Entry.Path)));
        if (Request.Size <= 0 || Request.Size > MaxChunkSize || Request.Offset < 0 || Request.Offset + Request.Size > Entry.Size)
            Reply->Error = TEXT("Invalid file request.");
        else if (!File || File->Size() != Entry.Size)
            Reply->Error = FString::Printf(TEXT("%s changed on the leader's computer. Ask the leader to host again."), *Entry.Path);
        else
        {
            const double ReadStart = FPlatformTime::Seconds();
            Reply->Data.SetNumUninitialized(Request.Size);
            if (!File->Seek(Request.Offset) || !File->Read(Reply->Data.GetData(), Request.Size))
            {
                Reply->Data.Reset();
                Reply->Error = FString::Printf(TEXT("Could not read %s on the leader's computer."), *Entry.Path);
            }
            ServedReadSeconds += FPlatformTime::Seconds() - ReadStart;
            ++ServedChunks;
            ServedBytes += Reply->Data.Num();
        }
    }
    SendReliable(Endpoint, Reply, Context->GetSender());
}

// ---------------------------------------------------------------------------------------------------------------------
// Member

void FClassroomProjectSync::BeginProbe(const FString& InTeamId, const FString& InJoinKey)
{
    ProbeTeamId = InTeamId;
    ProbeJoinKey = InJoinKey;
    ProbeRejectedBy.Reset();
    ProbeAcceptedBy.Reset();
    LastProbe = 0.0;
}

void FClassroomProjectSync::StopProbe()
{
    ProbeTeamId.Reset();
}

void FClassroomProjectSync::BeginGetProject(const FString& InTeamId, const FString& InHostComputer, const FString& InProjectName, const FString& InJoinKey)
{
    Cancel();
    TeamId = InTeamId;
    JoinKey = InJoinKey;
    HostComputer = InHostComputer;
    ProjectName = InProjectName;
    Offer = FClassroomProjectOffer();
    InstalledProject.Reset();
    LastSend = 0.0;
    if (!Endpoint.IsValid())
        return SetState(EClassroomProjectState::NotShared, LOCTEXT("CopyNoBus", "Project copying is unavailable in this editor (no message bus)."));
    SetState(EClassroomProjectState::Asking, FText::Format(LOCTEXT("CopyAsking", "Asking {0} for the team project..."), FText::FromString(HostComputer)));
}

void FClassroomProjectSync::Cancel()
{
    Active.Reset(); // Closes partial files; they are re-checked (and reused if complete) next time.
    Verifying.Reset();
    Retried.Reset();
    Queue.Reset();
    Pages.Reset();
    if (State != EClassroomProjectState::Idle) SetState(EClassroomProjectState::Idle, FText::GetEmpty());
}

void FClassroomProjectSync::SetState(EClassroomProjectState NewState, const FText& NewMessage)
{
    State = NewState;
    Message = NewMessage;
    PhaseStart = FPlatformTime::Seconds();
}

void FClassroomProjectSync::FailWith(const FText& Reason)
{
    Active.Reset();
    Verifying.Reset();
    Queue.Reset();
    UE_LOG(LogClassroomConnect, Warning, TEXT("Project copy failed: %s"), *Reason.ToString());
    SetState(EClassroomProjectState::Failed, Reason);
}

FString FClassroomProjectSync::StagingRoot() const { return FPaths::Combine(ProjectsRoot(), TeamId, Offer.Revision.Left(12) + TEXT(".partial")); }

FString FClassroomProjectSync::FinalRoot() const { return FPaths::Combine(ProjectsRoot(), TeamId, Offer.Revision.Left(12)); }

void FClassroomProjectSync::HandleOffer(const FClassroomProjectOffer& Received, const TSharedRef<IMessageContext, ESPMode::ThreadSafe>& Context)
{
    if (!ProbeTeamId.IsEmpty() && Received.TeamId == ProbeTeamId && !Received.Computer.IsEmpty())
    {
        FString& Seen = Received.bJoinCodeRejected ? ProbeRejectedBy : ProbeAcceptedBy;
        if (Seen.IsEmpty())
            UE_LOG(LogClassroomConnect, Log, TEXT("%s hosts team %s: %s."), *Received.Computer, *ProbeTeamId,
                !Received.bJoinCodeRejected ? TEXT("same join code as this editor") : ProbeJoinKey.IsEmpty() ? TEXT("it requires a join code") : TEXT("its join code is different"));
        Seen = Received.Computer;
    }
    if (State != EClassroomProjectState::Asking || Received.TeamId != TeamId) return;
    if (!HostComputer.IsEmpty() && Received.Computer != HostComputer) return;
    if (Received.bJoinCodeRejected)
        return FailWith(JoinKey.IsEmpty()
            ? FText::Format(LOCTEXT("CopyNeedsCode", "{0} set a join code for this team. Ask your team leader for it, type it under Join code, then click Join Team."), FText::FromString(Received.Computer))
            : FText::Format(LOCTEXT("CopyWrongCode", "The join code does not match the one set on {0}. Check it with your team leader, then click Join Team."), FText::FromString(Received.Computer)));
    if (Received.ProjectName != ProjectName || !IsHex64(Received.Revision) || Received.FileCount <= 0) return;
    Offer = Received;
    Leader = Context->GetSender();
    // The level ends up on an editor command line: accept only a plain /Game/... package path.
    const bool bPlainLevel = Offer.Level.StartsWith(TEXT("/Game/")) && Offer.Level.Len() < 200
        && !Offer.Level.Contains(TEXT("..")) && Algo::AllOf(Offer.Level, [](TCHAR C) { return FChar::IsAlnum(C) || C == TEXT('/') || C == TEXT('_') || C == TEXT('-'); });
    if (!bPlainLevel) Offer.Level.Reset();
    UE_LOG(LogClassroomConnect, Log, TEXT("%s offers %s: %d files, %lld bytes, revision %s."), *Offer.Computer, *Offer.ProjectName, Offer.FileCount, Offer.TotalBytes, *Offer.Revision.Left(12));

    // Already up to date: the open project, or an earlier verified copy of this exact revision.
    if (FApp::GetProjectName() == ProjectName && ReadInstalledRevision(FPaths::ProjectDir()) == Offer.Revision)
        return SetState(EClassroomProjectState::UpToDate, LOCTEXT("CopyUpToDate", "Your copy of the team project is up to date."));
    const FString Existing = FPaths::Combine(FinalRoot(), ProjectName, ProjectName + TEXT(".uproject"));
    if (FPaths::FileExists(Existing) && ReadInstalledRevision(FPaths::GetPath(Existing)) == Offer.Revision)
    {
        InstalledProject = Existing;
        return SetState(EClassroomProjectState::Installed, FText::Format(LOCTEXT("CopyAlreadyCopied", "{0} is already copied to this computer."), FText::FromString(ProjectName)));
    }

    const int32 PageCount = FMath::DivideAndRoundUp(Offer.FileCount, ManifestPageSize);
    Pages.SetNum(PageCount);
    PagesReceived = 0;
    PageAttempts = 0;
    SetState(EClassroomProjectState::ReadingManifest, FText::Format(LOCTEXT("CopyReadingManifest", "Getting the list of files in {0}..."), FText::FromString(ProjectName)));
    for (int32 Page = 0; Page < PageCount; ++Page) RequestManifestPage(Page);
}

void FClassroomProjectSync::RequestManifestPage(int32 Page)
{
    FClassroomManifestRequest* Request = FMessageEndpoint::MakeMessage<FClassroomManifestRequest>();
    Request->TeamId = TeamId;
    Request->Revision = Offer.Revision;
    Request->Page = Page;
    Request->JoinKey = JoinKey;
    SendReliable(Endpoint, Request, Leader);
    LastSend = FPlatformTime::Seconds();
}

void FClassroomProjectSync::HandleManifestPage(const FClassroomManifestPage& Page, const TSharedRef<IMessageContext, ESPMode::ThreadSafe>& Context)
{
    if (State != EClassroomProjectState::ReadingManifest || Page.TeamId != TeamId || Page.Revision != Offer.Revision || !Pages.IsValidIndex(Page.Page)) return;
    if (!Page.Error.IsEmpty()) return FailWith(FText::FromString(Page.Error));
    if (Pages[Page.Page].Num() > 0 || Page.Entries.Num() == 0) return;
    Pages[Page.Page] = Page.Entries;
    if (++PagesReceived == Pages.Num()) OnManifestComplete();
}

void FClassroomProjectSync::OnManifestComplete()
{
    Entries.Reset();
    for (TArray<FClassroomManifestEntry>& Page : Pages) Entries.Append(MoveTemp(Page));
    Pages.Reset();

    // Validate before touching the disk: safe paths, no duplicates, the expected project file, and the advertised revision.
    TSet<FString> Seen;
    bool bHasProjectFile = false;
    for (const FClassroomManifestEntry& Entry : Entries)
    {
        bool bDuplicate = false;
        Seen.Add(Entry.Path.ToLower(), &bDuplicate);
        if (bDuplicate || !IsSafeRelativePath(Entry.Path) || Entry.Size < 0 || !IsHex64(Entry.Sha256))
            return FailWith(FText::Format(LOCTEXT("CopyBadManifest", "The leader's file list contains an invalid entry ({0}); nothing was copied."), FText::FromString(Entry.Path)));
        bHasProjectFile |= Entry.Path == ProjectName + TEXT(".uproject");
    }
    if (!bHasProjectFile || Entries.Num() != Offer.FileCount || ManifestRevision(Entries) != Offer.Revision)
        return FailWith(LOCTEXT("CopyManifestMismatch", "The leader's file list arrived damaged or incomplete; nothing was copied. Click Retry."));

    // Compare with what is already here, off the game thread: the open project, earlier copies, and an interrupted download.
    const FString Staging = FPaths::Combine(StagingRoot(), ProjectName);
    TArray<FString> Candidates;
    const FString Current = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir());
    const bool bCurrentIsSameProject = FApp::GetProjectName() == ProjectName;
    if (bCurrentIsSameProject) Candidates.Add(Current);
    TArray<FString> Earlier;
    IFileManager::Get().FindFiles(Earlier, *FPaths::Combine(ProjectsRoot(), TeamId, TEXT("*")), false, true);
    Earlier.Sort([this](const FString& A, const FString& B)
    {
        return IFileManager::Get().GetTimeStamp(*FPaths::Combine(ProjectsRoot(), TeamId, A)) > IFileManager::Get().GetTimeStamp(*FPaths::Combine(ProjectsRoot(), TeamId, B));
    });
    for (int32 Index = 0; Index < Earlier.Num() && Index < 3; ++Index)
    {
        const FString Candidate = FPaths::Combine(ProjectsRoot(), TeamId, Earlier[Index], ProjectName);
        if (Candidate != Staging && FPaths::DirectoryExists(Candidate)) Candidates.Add(Candidate);
    }

    SetState(EClassroomProjectState::Checking, FText::Format(LOCTEXT("CopyChecking", "Checking which files of {0} are already on this computer..."), FText::FromString(ProjectName)));
    Check = Async(EAsyncExecution::ThreadPool, [ManifestEntries = Entries, Staging, Candidates, Current, bCurrentIsSameProject]
    {
        FClassroomCheckResult Result;
        TArray<bool> InCurrent;
        TArray<int32> FromCandidate;
        int32 MatchesInCurrent = 0;
        for (int32 Index = 0; Index < ManifestEntries.Num(); ++Index)
        {
            const FClassroomManifestEntry& Entry = ManifestEntries[Index];
            const bool bCurrentMatch = bCurrentIsSameProject && MatchesEntry(FPaths::Combine(Current, Entry.Path), Entry);
            MatchesInCurrent += bCurrentMatch ? 1 : 0;
            InCurrent.Add(bCurrentMatch);
        }
        if (bCurrentIsSameProject && MatchesInCurrent == ManifestEntries.Num())
        {
            // Same files; the open project is up to date only if it also has nothing extra.
            TArray<FString> Files;
            ListFiles(Current, FString(), Files);
            if (Files.Num() == ManifestEntries.Num())
            {
                Result.bCurrentProjectMatches = true;
                return Result;
            }
        }
        for (int32 Index = 0; Index < ManifestEntries.Num(); ++Index)
        {
            const FClassroomManifestEntry& Entry = ManifestEntries[Index];
            const FString Target = FPaths::Combine(Staging, Entry.Path);
            if (MatchesEntry(Target, Entry)) continue; // Completed by an interrupted earlier download.
            FString Source;
            if (InCurrent[Index]) Source = FPaths::Combine(Current, Entry.Path);
            for (int32 Candidate = bCurrentIsSameProject ? 1 : 0; Source.IsEmpty() && Candidate < Candidates.Num(); ++Candidate)
                if (MatchesEntry(FPaths::Combine(Candidates[Candidate], Entry.Path), Entry)) Source = FPaths::Combine(Candidates[Candidate], Entry.Path);
            if (!Source.IsEmpty())
            {
                IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target), true);
                if (IFileManager::Get().Copy(*Target, *Source, true, true) == COPY_OK) continue;
            }
            Result.Missing.Add(Index);
        }
        return Result;
    });
}

void FClassroomProjectSync::StartFile(int32 FileIndex, double Now)
{
    const FClassroomManifestEntry& Entry = Entries[FileIndex];
    const FString Target = FPaths::Combine(StagingRoot(), ProjectName, Entry.Path);
    IFileManager::Get().MakeDirectory(*FPaths::GetPath(Target), true);
    FActiveFile& File = Active.Add(FileIndex);
    File.Handle = MakeShareable(FPlatformFileManager::Get().GetPlatformFile().OpenWrite(*Target));
    if (!File.Handle.IsValid())
        return FailWith(FText::Format(LOCTEXT("CopyCannotWrite", "Could not write {0} on this computer. Check free disk space."), FText::FromString(Target)));
    if (Entry.Size == 0) return VerifyFile(FileIndex);
    FillWindow(FileIndex, Now);
}

void FClassroomProjectSync::StartNextFiles(double Now)
{
    while (Active.Num() < ParallelFiles && Queue.Num() > 0 && State == EClassroomProjectState::Downloading)
        StartFile(Queue.Pop(EAllowShrinking::No), Now);
}

void FClassroomProjectSync::VerifyFile(int32 FileIndex)
{
    // Hash the finished file off the game thread; its download slot is free for the next file straight away.
    Active.Remove(FileIndex);
    const FString Target = FPaths::Combine(StagingRoot(), ProjectName, Entries[FileIndex].Path);
    Verifying.Add(FileIndex, Async(EAsyncExecution::ThreadPool, [Target] { FString Hash; int64 Size = 0; return HashFile(Target, Hash, Size) ? Hash : FString(); }));
}

void FClassroomProjectSync::FillWindow(int32 FileIndex, double Now)
{
    // Keep chunks of the same file in flight, within one budget shared by all files: a round trip costs at least a frame
    // on each editor, and Unreal's UDP messaging slows down sharply if far more is requested than its send window holds.
    FActiveFile& File = Active[FileIndex];
    const int64 Size = Entries[FileIndex].Size;
    int32 InFlight = 0;
    for (const TPair<int32, FActiveFile>& Other : Active) InFlight += Other.Value.Pending.Num();
    const int32 Budget = FMath::Clamp(CVarCopyChunksInFlight.GetValueOnGameThread(), 1, 64);
    while (InFlight++ < Budget && File.NextOffset < Size)
    {
        RequestChunk(FileIndex, File.NextOffset, Now);
        File.NextOffset += FMath::Min<int64>(DownloadChunkSize, Size - File.NextOffset);
    }
}

void FClassroomProjectSync::RequestChunk(int32 FileIndex, int64 Offset, double Now)
{
    FClassroomChunkRequest* Request = FMessageEndpoint::MakeMessage<FClassroomChunkRequest>();
    Request->TeamId = TeamId;
    Request->Revision = Offer.Revision;
    Request->FileIndex = FileIndex;
    Request->Offset = Offset;
    Request->Size = static_cast<int32>(FMath::Min<int64>(DownloadChunkSize, Entries[FileIndex].Size - Offset));
    Request->JoinKey = JoinKey;
    SendReliable(Endpoint, Request, Leader);
    Active[FileIndex].Pending.Add(Offset, Now);
}

void FClassroomProjectSync::HandleChunkReply(const FClassroomChunkReply& Reply, const TSharedRef<IMessageContext, ESPMode::ThreadSafe>& Context)
{
    if (State != EClassroomProjectState::Downloading || Reply.TeamId != TeamId || Reply.Revision != Offer.Revision) return;
    FActiveFile* File = Active.Find(Reply.FileIndex);
    if (!File || !File->Handle.IsValid() || !File->Pending.Contains(Reply.Offset)) return; // Duplicate or stale reply after a resend.
    if (!Reply.Error.IsEmpty()) return FailWith(FText::FromString(Reply.Error));
    const FClassroomManifestEntry& Entry = Entries[Reply.FileIndex];
    const int64 Expected = FMath::Min<int64>(DownloadChunkSize, Entry.Size - Reply.Offset);
    if (Reply.Data.Num() != Expected || !File->Handle->Seek(Reply.Offset) || !File->Handle->Write(Reply.Data.GetData(), Reply.Data.Num()))
        return FailWith(FText::Format(LOCTEXT("CopyWriteFailed", "Could not write {0}. Check free disk space."), FText::FromString(Entry.Path)));
    const double Now = FPlatformTime::Seconds();
    StatRoundTrips += Now - File->Pending[Reply.Offset];
    ++StatChunks;
    StatBytes += Reply.Data.Num();
    File->Pending.Remove(Reply.Offset);
    File->Received += Reply.Data.Num();
    File->Attempts = 0;
    BytesDone += Reply.Data.Num();
    if (File->Received < Entry.Size) return FillWindow(Reply.FileIndex, Now);

    File->Handle.Reset();
    VerifyFile(Reply.FileIndex);
    StartNextFiles(Now);
    UpdateDownloadMessage();
}

void FClassroomProjectSync::TickDownloads(double Now)
{
    StartNextFiles(Now);

    for (TPair<int32, FActiveFile>& Pair : Active)
    {
        FActiveFile& File = Pair.Value;
        // Files started while the shared budget was full get their first chunks here.
        if (File.Handle.IsValid()) FillWindow(Pair.Key, Now);
        for (TPair<int64, double>& Chunk : File.Pending)
        {
            if (Now - Chunk.Value <= RequestTimeoutSeconds) continue;
            if (++File.Attempts > MaxAttempts)
                return FailWith(FText::Format(LOCTEXT("CopyChunkTimeout", "{0}'s computer stopped answering. Click Retry; files already copied are kept."), FText::FromString(Offer.Computer)));
            Chunk.Value = Now;
            FClassroomChunkRequest* Request = FMessageEndpoint::MakeMessage<FClassroomChunkRequest>();
            Request->TeamId = TeamId;
            Request->Revision = Offer.Revision;
            Request->FileIndex = Pair.Key;
            Request->Offset = Chunk.Key;
            Request->Size = static_cast<int32>(FMath::Min<int64>(DownloadChunkSize, Entries[Pair.Key].Size - Chunk.Key));
            Request->JoinKey = JoinKey;
            SendReliable(Endpoint, Request, Leader);
        }
    }

    TArray<int32> Done;
    for (TPair<int32, TFuture<FString>>& Pair : Verifying)
    {
        if (!Pair.Value.IsReady()) continue;
        Done.Add(Pair.Key);
        if (Pair.Value.Get() == Entries[Pair.Key].Sha256) continue;
        if (Retried.Contains(Pair.Key))
            return FailWith(FText::Format(LOCTEXT("CopyVerifyFailed", "{0} failed verification twice; nothing was installed. Click Retry."), FText::FromString(Entries[Pair.Key].Path)));
        // Corrupted in transit: download it once more.
        Retried.Add(Pair.Key);
        BytesDone -= Entries[Pair.Key].Size;
        Queue.Add(Pair.Key);
    }
    for (const int32 Index : Done) Verifying.Remove(Index);

    if (State == EClassroomProjectState::Downloading && Active.Num() == 0 && Verifying.Num() == 0 && Queue.Num() == 0) FinishDownload();
}

void FClassroomProjectSync::UpdateDownloadMessage()
{
    if (State != EClassroomProjectState::Downloading) return;
    Message = FText::Format(LOCTEXT("CopyDownloading", "Copying {0} from {1}: {2} of {3} MB ({4} files reused from this computer)..."),
        FText::FromString(ProjectName), FText::FromString(Offer.Computer), Megabytes(BytesDone), Megabytes(BytesTotal), FText::AsNumber(FilesReused));
}

void FClassroomProjectSync::FinishDownload()
{
    // Every file is verified. Record the revision, then move the folder into place in one rename.
    const FString Staging = StagingRoot();
    const FString Final = FinalRoot();
    FFileHelper::SaveStringToFile(Offer.Revision, *FPaths::Combine(Staging, ProjectName, MarkerPath));
    if (FPaths::DirectoryExists(Final) || !IFileManager::Get().Move(*Final, *Staging, false, false, true))
        return FailWith(FText::Format(LOCTEXT("CopyInstallFailed", "The copy is complete but could not be moved to {0}. Close any window using that folder and click Retry."), FText::FromString(Final)));
    InstalledProject = FPaths::Combine(Final, ProjectName, ProjectName + TEXT(".uproject"));
    UE_LOG(LogClassroomConnect, Log, TEXT("Installed %s revision %s at %s (%lld bytes downloaded, %d files reused)."), *ProjectName, *Offer.Revision.Left(12), *InstalledProject, BytesTotal, FilesReused);
    SetState(EClassroomProjectState::Installed, FText::Format(LOCTEXT("CopyInstalled", "{0} is copied and verified."), FText::FromString(ProjectName)));
}

void FClassroomProjectSync::Tick(double Now)
{
    if (Now - StatsStart >= 5.0)
    {
        const double Seconds = StatsStart > 0.0 ? Now - StatsStart : 0.0;
        if (StatChunks > 0)
        {
            int32 InFlight = 0;
            for (const TPair<int32, FActiveFile>& Pair : Active) InFlight += Pair.Value.Pending.Num();
            UE_LOG(LogClassroomConnect, Log, TEXT("Copy: %d chunks, %.1f MB in %.1f s (%.2f MB/s), average round trip %.0f ms, %d in flight, %d files active, %d verifying."),
                StatChunks, StatBytes / 1048576.0, Seconds, StatBytes / 1048576.0 / FMath::Max(Seconds, 0.001), 1000.0 * StatRoundTrips / StatChunks, InFlight, Active.Num(), Verifying.Num());
        }
        if (ServedChunks > 0)
            UE_LOG(LogClassroomConnect, Log, TEXT("Served %d chunks, %.1f MB in %.1f s, %.0f ms reading files."), ServedChunks, ServedBytes / 1048576.0, Seconds, 1000.0 * ServedReadSeconds);
        StatsStart = Now;
        StatChunks = 0; StatBytes = 0; StatRoundTrips = 0.0;
        ServedChunks = 0; ServedBytes = 0; ServedReadSeconds = 0.0;
    }

    if (ServeBuild.IsValid() && ServeBuild.IsReady())
    {
        FClassroomManifestResult Result = ServeBuild.Get();
        ServeBuild = TFuture<FClassroomManifestResult>();
        if (Result.Skipped.Num() > 0)
            UE_LOG(LogClassroomConnect, Warning, TEXT("Not sharing %d file(s) that could not be read: %s"), Result.Skipped.Num(), *FString::Join(Result.Skipped, TEXT(", ")));
        if (Result.Entries.IsEmpty())
        {
            UE_LOG(LogClassroomConnect, Warning, TEXT("Cannot share the project with teammates: no readable files in %s."), *ServeDir);
            ServeDir.Reset();
        }
        else
        {
            ServeEntries = MoveTemp(Result.Entries);
            ServeRevision = ManifestRevision(ServeEntries);
            ServeBytes = 0;
            for (const FClassroomManifestEntry& Entry : ServeEntries) ServeBytes += Entry.Size;
            bServeReady = true;
            UE_LOG(LogClassroomConnect, Log, TEXT("Sharing %s with team %s: %d files, %lld bytes, revision %s."), *ServeProjectName, *ServeTeamId, ServeEntries.Num(), ServeBytes, *ServeRevision.Left(12));
        }
    }

    if (!ProbeTeamId.IsEmpty() && Endpoint.IsValid() && Now - LastProbe >= AskIntervalSeconds)
    {
        FClassroomProjectQuery* Query = FMessageEndpoint::MakeMessage<FClassroomProjectQuery>();
        Query->TeamId = ProbeTeamId;
        Query->JoinKey = ProbeJoinKey;
        Endpoint->Publish(Query);
        LastProbe = Now;
    }

    switch (State)
    {
    case EClassroomProjectState::Asking:
        if (Now - PhaseStart > AskTimeoutSeconds)
            return SetState(EClassroomProjectState::NotShared, FText::Format(LOCTEXT("CopyNotShared", "{0} did not offer the team project."), FText::FromString(HostComputer)));
        if (Now - LastSend >= AskIntervalSeconds)
        {
            FClassroomProjectQuery* Query = FMessageEndpoint::MakeMessage<FClassroomProjectQuery>();
            Query->TeamId = TeamId;
            Query->JoinKey = JoinKey;
            Endpoint->Publish(Query);
            LastSend = Now;
        }
        return;

    case EClassroomProjectState::ReadingManifest:
        if (Now - LastSend > RequestTimeoutSeconds)
        {
            if (++PageAttempts > MaxAttempts) return FailWith(LOCTEXT("CopyManifestTimeout", "The leader's computer stopped answering while sending the file list. Click Retry."));
            for (int32 Page = 0; Page < Pages.Num(); ++Page) if (Pages[Page].Num() == 0) RequestManifestPage(Page);
        }
        return;

    case EClassroomProjectState::Checking:
        if (Check.IsValid() && Check.IsReady())
        {
            FClassroomCheckResult Result = Check.Get();
            Check = TFuture<FClassroomCheckResult>();
            if (Result.bCurrentProjectMatches)
                return SetState(EClassroomProjectState::UpToDate, LOCTEXT("CopyUpToDateHashed", "Your open project already matches the team project."));
            Queue = MoveTemp(Result.Missing);
            Algo::Reverse(Queue); // Pop from the end, in manifest order.
            BytesTotal = 0;
            for (const int32 Index : Queue) BytesTotal += Entries[Index].Size;
            BytesDone = 0;
            FilesReused = Entries.Num() - Queue.Num();
            UE_LOG(LogClassroomConnect, Log, TEXT("%d of %d files already on this computer; downloading %d (%lld bytes)."), FilesReused, Entries.Num(), Queue.Num(), BytesTotal);
            DownloadChunkSize = FMath::Clamp(CVarCopyChunkKB.GetValueOnGameThread(), 64, 1024) * 1024;
            SetState(EClassroomProjectState::Downloading, FText::GetEmpty());
            UpdateDownloadMessage();
        }
        return;

    case EClassroomProjectState::Downloading:
        return TickDownloads(Now);

    default:
        return;
    }
}

float FClassroomProjectSync::GetProgress() const
{
    if (State != EClassroomProjectState::Downloading) return -1.f;
    return BytesTotal > 0 ? static_cast<float>(static_cast<double>(BytesDone) / BytesTotal) : 1.f;
}

TArray<FString> FClassroomProjectSync::GetDiagnostics() const
{
    TArray<FString> Lines;
    Lines.Add(FString::Printf(TEXT("Project copying: %s; copies go to %s"), Endpoint.IsValid() ? TEXT("available") : TEXT("unavailable (no message bus)"), *ProjectsRoot()));
    if (!ServeTeamId.IsEmpty())
        Lines.Add((bServeReady ? FString::Printf(TEXT("Sharing %s with team %s: %d files, revision %s"), *ServeProjectName, *ServeTeamId, ServeEntries.Num(), *ServeRevision.Left(12))
                               : FString::Printf(TEXT("Preparing %s to share with team %s..."), *ServeProjectName, *ServeTeamId))
            + (ServeJoinKey.IsEmpty() ? TEXT(", with anyone who asks (no join code)") : TEXT(", only with the join code")));
    if (!ProbeRejectedBy.IsEmpty()) Lines.Add(FString::Printf(TEXT("%s hosts team %s with a different join code"), *ProbeRejectedBy, *ProbeTeamId));
    const FString Revision = ReadInstalledRevision(FPaths::ProjectDir());
    if (!Revision.IsEmpty()) Lines.Add(TEXT("This project was copied by Classroom Connect, revision ") + Revision.Left(12));
    return Lines;
}

#undef LOCTEXT_NAMESPACE
