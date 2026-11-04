#include "ClassroomTeams.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
    struct FDefaultTeam { const TCHAR* Id; const TCHAR* Hex; };

    const FDefaultTeam DefaultTeams[] = {
        { TEXT("Red"), TEXT("D32F2F") },    { TEXT("Blue"), TEXT("1E63D6") },   { TEXT("Green"), TEXT("2E9E44") },
        { TEXT("Yellow"), TEXT("F2C200") }, { TEXT("Orange"), TEXT("F57C00") }, { TEXT("Purple"), TEXT("7B3FB6") },
        { TEXT("Pink"), TEXT("F06292") },   { TEXT("Cyan"), TEXT("00ACC1") },   { TEXT("Lime"), TEXT("9CCC22") },
        { TEXT("Teal"), TEXT("00897B") },   { TEXT("Indigo"), TEXT("3949AB") }, { TEXT("Magenta"), TEXT("D01FA8") },
        { TEXT("Gold"), TEXT("C9A227") },   { TEXT("Silver"), TEXT("A8AEB5") }, { TEXT("Black"), TEXT("202124") },
        { TEXT("White"), TEXT("F5F5F5") },  { TEXT("Brown"), TEXT("795548") },  { TEXT("Coral"), TEXT("FF7F61") },
        { TEXT("Navy"), TEXT("1A2A6C") },   { TEXT("Mint"), TEXT("7FD8B0") },
    };

    constexpr int32 MaxTeams = 20;
    constexpr int32 MaxConfigBytes = 64 * 1024;

    bool IsValidId(const FString& Id)
    {
        if (Id.IsEmpty() || Id.Len() > 24) return false;
        for (const TCHAR C : Id) if (!FChar::IsAlnum(C) || C > 127) return false;
        return true;
    }

    bool IsValidHex(const FString& Hex)
    {
        const FString Digits = Hex.StartsWith(TEXT("#")) ? Hex.RightChop(1) : Hex;
        if (Digits.Len() != 6) return false;
        for (const TCHAR C : Digits) if (!FChar::IsHexDigit(C)) return false;
        return true;
    }

    TArray<FClassroomTeam> Defaults()
    {
        TArray<FClassroomTeam> Teams;
        for (const FDefaultTeam& Team : DefaultTeams)
            Teams.Add({ Team.Id, FText::FromString(Team.Id), FLinearColor(FColor::FromHex(Team.Hex)) });
        return Teams;
    }
}

TArray<FClassroomTeam> ClassroomTeams::Load(FString& OutWarning)
{
    OutWarning.Reset();
    const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(TEXT("ClassroomConnect"));
    if (!Plugin.IsValid()) return Defaults();

    const FString Path = FPaths::Combine(Plugin->GetBaseDir(), TEXT("Config"), TEXT("ClassroomTeams.json"));
    if (!FPaths::FileExists(Path)) return Defaults();

    auto Reject = [&OutWarning, &Path](const FString& Reason)
    {
        OutWarning = FString::Printf(TEXT("%s was ignored (%s). Using the built-in teams."), *Path, *Reason);
        return Defaults();
    };

    FString Text;
    if (IFileManager::Get().FileSize(*Path) > MaxConfigBytes || !FFileHelper::LoadFileToString(Text, *Path)) return Reject(TEXT("unreadable or larger than 64 KB"));

    TSharedPtr<FJsonObject> Root;
    if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid()) return Reject(TEXT("not valid JSON"));

    const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
    if (!Root->TryGetArrayField(TEXT("Teams"), Entries) || Entries->Num() == 0 || Entries->Num() > MaxTeams) return Reject(TEXT("\"Teams\" must list 1-20 teams"));

    TArray<FClassroomTeam> Teams;
    TSet<FString> SeenIds;
    for (const TSharedPtr<FJsonValue>& Value : *Entries)
    {
        const TSharedPtr<FJsonObject>* Entry = nullptr;
        FString Id, Name, Hex;
        if (!Value.IsValid() || !Value->TryGetObject(Entry) || !(*Entry)->TryGetStringField(TEXT("Id"), Id)) return Reject(TEXT("every team needs an \"Id\""));
        if (!IsValidId(Id)) return Reject(FString::Printf(TEXT("team Id \"%s\" must be 1-24 ASCII letters or digits"), *Id));
        if (SeenIds.Contains(Id)) return Reject(FString::Printf(TEXT("team Id \"%s\" is listed twice"), *Id));
        SeenIds.Add(Id);
        if (!(*Entry)->TryGetStringField(TEXT("Name"), Name) || Name.TrimStartAndEnd().IsEmpty() || Name.Len() > 40) Name = Id;
        if (!(*Entry)->TryGetStringField(TEXT("Color"), Hex) || !IsValidHex(Hex)) return Reject(FString::Printf(TEXT("team \"%s\" needs a Color like \"#D32F2F\""), *Id));
        Teams.Add({ Id, FText::FromString(Name.TrimStartAndEnd()), FLinearColor(FColor::FromHex(Hex)) });
    }
    return Teams;
}

FString ClassroomTeams::SessionName(const FClassroomTeam& Team) { return TEXT("UCC-") + Team.Id; }

FString ClassroomTeams::ServerPrefix(const FClassroomTeam& Team) { return SessionName(Team) + TEXT("-"); }

FString ClassroomTeams::ServerName(const FClassroomTeam& Team) { return ServerPrefix(Team) + FPlatformProcess::ComputerName(); }

bool ClassroomTeams::IsLight(const FLinearColor& Color) { return 0.2126f * Color.R + 0.7152f * Color.G + 0.0722f * Color.B > 0.3f; }
