#include "ClassroomConnectLog.h"
#include "ClassroomSession.h"
#include "ClassroomTeams.h"
#include "SClassroomConnectPanel.h"

#include "Containers/Ticker.h"
#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformTime.h"
#include "Framework/Docking/TabManager.h"
#include "Interfaces/IMainFrameModule.h"
#include "Misc/MessageDialog.h"
#include "Modules/ModuleManager.h"
#include "Styling/AppStyle.h"
#include "ToolMenus.h"
#include "Widgets/Docking/SDockTab.h"

#define LOCTEXT_NAMESPACE "ClassroomConnect"

DEFINE_LOG_CATEGORY(LogClassroomConnect);

namespace
{
    const FName TabName("ClassroomConnect");
}

class FClassroomConnectModule final : public IModuleInterface
{
    TSharedPtr<FClassroomSession> Session;
    TArray<FClassroomTeam> Teams;
    FString TeamsWarning;
    TArray<IConsoleObject*> Commands;
    FDelegateHandle CanCloseHandle;
    bool bCloseWithoutSaving = false;
    TArray<FString> AfterConnected;
    FTSTicker::FDelegateHandle AfterConnectedTicker;
public:
    virtual void StartupModule() override
    {
        Teams = ClassroomTeams::Load(TeamsWarning);
        if (!TeamsWarning.IsEmpty()) UE_LOG(LogClassroomConnect, Warning, TEXT("%s"), *TeamsWarning);
        Session = MakeShared<FClassroomSession>();
        Session->Start(Teams);
        FGlobalTabmanager::Get()->RegisterNomadTabSpawner(TabName, FOnSpawnTab::CreateRaw(this, &FClassroomConnectModule::Spawn))
            .SetDisplayName(LOCTEXT("Title", "Classroom Connect")).SetMenuType(ETabSpawnerMenuType::Hidden);
        UToolMenus::RegisterStartupCallback(FSimpleMulticastDelegate::FDelegate::CreateRaw(this, &FClassroomConnectModule::Menus));
        RegisterCommands();
        // Closing the editor while hosting would otherwise drop the session without writing the team's work to the project:
        // Multi-User shuts its client down on exit without asking (MultiUserClientModule.cpp HandleAppPreExit).
        if (IMainFrameModule* MainFrame = FModuleManager::LoadModulePtr<IMainFrameModule>("MainFrame"))
            CanCloseHandle = MainFrame->RegisterCanCloseEditor(IMainFrameModule::FMainFrameCanCloseEditor::CreateRaw(this, &FClassroomConnectModule::CanCloseEditor));
        UE_LOG(LogClassroomConnect, Log, TEXT("Classroom Connect loaded with %d teams."), Teams.Num());
    }
    virtual void ShutdownModule() override
    {
        if (IMainFrameModule* MainFrame = FModuleManager::GetModulePtr<IMainFrameModule>("MainFrame")) MainFrame->UnregisterCanCloseEditor(CanCloseHandle);
        if (AfterConnectedTicker.IsValid()) FTSTicker::RemoveTicker(AfterConnectedTicker);
        for (IConsoleObject* Command : Commands) IConsoleManager::Get().UnregisterConsoleObject(Command);
        Commands.Reset();
        UToolMenus::UnRegisterStartupCallback(this);
        UToolMenus::UnregisterOwner(this);
        if (FSlateApplication::IsInitialized()) FGlobalTabmanager::Get()->UnregisterNomadTabSpawner(TabName);
        if (Session.IsValid()) Session->Stop();
        Session.Reset();
    }
    /** Console equivalents of the window's buttons, for IT diagnostics and scripted tests (e.g. -ExecCmds="UCC.Host Red"). */
    void RegisterCommands()
    {
        IConsoleManager& Console = IConsoleManager::Get();
        Commands.Add(Console.RegisterConsoleCommand(TEXT("UCC.Host"), TEXT("Host a team as Team Leader: UCC.Host <TeamId> [JoinCode]"),
            FConsoleCommandWithArgsDelegate::CreateRaw(this, &FClassroomConnectModule::RunTeamCommand, EClassroomRole::Leader)));
        Commands.Add(Console.RegisterConsoleCommand(TEXT("UCC.Join"), TEXT("Join a team as Team Member: UCC.Join <TeamId> [JoinCode]"),
            FConsoleCommandWithArgsDelegate::CreateRaw(this, &FClassroomConnectModule::RunTeamCommand, EClassroomRole::Member)));
        Commands.Add(Console.RegisterConsoleCommand(TEXT("UCC.Leave"), TEXT("Leave the team session (a Team Leader saves the team's work into the project first)"),
            FConsoleCommandDelegate::CreateLambda([this] { Session->Leave(); })));
        Commands.Add(Console.RegisterConsoleCommand(TEXT("UCC.AfterConnected"), TEXT("Scripted tests: run '|'-separated editor commands 3 s after this editor reports Connected"),
            FConsoleCommandWithArgsDelegate::CreateRaw(this, &FClassroomConnectModule::QueueAfterConnected)));
        Commands.Add(Console.RegisterConsoleCommand(TEXT("UCC.Status"), TEXT("Log Classroom Connect state and diagnostics"),
            FConsoleCommandDelegate::CreateLambda([this]
            {
                UE_LOG(LogClassroomConnect, Display, TEXT("State: %s - %s"), *FClassroomSession::StateName(Session->GetState()), *Session->GetMessage().ToString());
                for (const FClassroomParticipant& Row : Session->GetParticipants())
                    UE_LOG(LogClassroomConnect, Display, TEXT("Participant: %s%s on %s, marker %s, level %s%s"), *Row.Name, Row.bIsLocal ? TEXT(" (you)") : TEXT(""), *Row.Computer, *Row.ColorName,
                        Row.Level.IsEmpty() ? TEXT("unknown") : *Row.Level, Row.bSameLevel ? TEXT(" (same as you)") : TEXT(""));
                for (const FString& Line : Session->GetDiagnostics()) UE_LOG(LogClassroomConnect, Display, TEXT("%s"), *Line);
            })));
    }
    /**
     * Saving the team's work takes a few frames (the team server must record the saves), so the first close request is
     * refused while it runs and the editor is closed again once the work is in the project.
     */
    bool CanCloseEditor()
    {
        if (bCloseWithoutSaving || !Session.IsValid() || !Session->ShouldSaveOnLeave()) return true;
        if (Session->GetState() == EClassroomState::SavingWork) return false;
        Session->BeginLeaveAndSave([this](bool bLeft)
        {
            if (!bLeft)
            {
                const EAppReturnType::Type Answer = FMessageDialog::Open(EAppMsgType::YesNo, EAppReturnType::No, FText::Format(
                    LOCTEXT("CloseAnyway", "{0}\n\nClose Unreal anyway? The team's work stays in the team session on this computer's team server, but it will not be in your project files until you host again and leave normally."),
                    Session->GetMessage()));
                if (Answer != EAppReturnType::Yes) return;
                bCloseWithoutSaving = true;
            }
            GEngine->DeferredCommands.Add(TEXT("CLOSE_SLATE_MAINFRAME"));
        });
        return false;
    }
    void QueueAfterConnected(const TArray<FString>& Args)
    {
        FString::Join(Args, TEXT(" ")).ParseIntoArray(AfterConnected, TEXT("|"));
        for (FString& Command : AfterConnected) Command.TrimStartAndEndInline();
        if (AfterConnectedTicker.IsValid()) FTSTicker::RemoveTicker(AfterConnectedTicker);
        AfterConnectedTicker = FTSTicker::GetCoreTicker().AddTicker(TEXT("ClassroomConnectAfterConnected"), 0.5f, [this, ConnectedAt = 0.0](float) mutable
        {
            if (Session->GetState() != EClassroomState::Connected) { ConnectedAt = 0.0; return true; }
            if (ConnectedAt == 0.0) ConnectedAt = FPlatformTime::Seconds();
            if (FPlatformTime::Seconds() - ConnectedAt < 3.0) return true;
            const TArray<FString> Pending = MoveTemp(AfterConnected);
            for (const FString& Command : Pending)
            {
                UE_LOG(LogClassroomConnect, Log, TEXT("Running after connect: %s"), *Command);
                GEditor->Exec(GEditor->GetEditorWorldContext().World(), *Command);
            }
            AfterConnectedTicker.Reset();
            return false;
        });
    }
    void RunTeamCommand(const TArray<FString>& Args, EClassroomRole Role)
    {
        const FClassroomTeam* Team = Args.Num() == 1 || Args.Num() == 2 ? Teams.FindByPredicate([&Args](const FClassroomTeam& Candidate) { return Candidate.Id == Args[0]; }) : nullptr;
        if (!Team)
        {
            UE_LOG(LogClassroomConnect, Error, TEXT("Usage: UCC.Host|UCC.Join <TeamId> [JoinCode]. Team Ids: %s"), *FString::JoinBy(Teams, TEXT(", "), [](const FClassroomTeam& Candidate) { return Candidate.Id; }));
            return;
        }
        // No code given means no code, so scripted runs never pick up a code typed in the window.
        const FString Code = Args.Num() == 2 ? Args[1] : FString();
        if (Role == EClassroomRole::Leader) Session->Host(*Team, Code);
        else Session->Join(*Team, Code);
    }
    void Menus()
    {
        FToolMenuOwnerScoped Owner(this);
        const FUIAction Open(FExecuteAction::CreateLambda([] { FGlobalTabmanager::Get()->TryInvokeTab(TabName); }));
        const FSlateIcon Icon(FAppStyle::GetAppStyleSetName(), "Icons.Link");
        UToolMenus::Get()->ExtendMenu("LevelEditor.MainMenu.Tools")->FindOrAddSection("ClassroomConnect").AddMenuEntry("OpenClassroomConnect",
            LOCTEXT("Open", "Classroom Connect"), LOCTEXT("OpenTip", "Host or join your classroom team"), Icon, Open);
        UToolMenus::Get()->ExtendMenu("LevelEditor.LevelEditorToolBar.User")->FindOrAddSection("ClassroomConnect").AddEntry(
            FToolMenuEntry::InitToolBarButton("ClassroomConnectToolbar", Open, LOCTEXT("ToolbarLabel", "Classroom"), LOCTEXT("ToolbarTip", "Open Classroom Connect"), Icon));
    }
    TSharedRef<SDockTab> Spawn(const FSpawnTabArgs&)
    {
        return SNew(SDockTab).TabRole(ETabRole::NomadTab)
        [ SNew(SClassroomConnectPanel).Session(Session).Teams(Teams).TeamsWarning(TeamsWarning) ];
    }
};
IMPLEMENT_MODULE(FClassroomConnectModule, ClassroomConnect)
#undef LOCTEXT_NAMESPACE
