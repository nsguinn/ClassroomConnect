#include "SClassroomConnectPanel.h"
#include "ClassroomProjectSync.h"

#include "CoreGlobals.h"
#include "IMultiUserClientModule.h"
#include "Misc/App.h"
#include "Misc/ConfigCacheIni.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Input/SEditableTextBox.h"
#include "Widgets/Input/SSegmentedControl.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SExpandableArea.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SUniformGridPanel.h"
#include "Widgets/Notifications/SProgressBar.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#define LOCTEXT_NAMESPACE "ClassroomConnect"

namespace
{
    constexpr int32 TilesPerRow = 5;
    const TCHAR* PreferencesSection = TEXT("ClassroomConnect");

    TSharedRef<SWidget> Heading(const FText& Text)
    {
        return SNew(STextBlock).Text(Text).Font(FCoreStyle::GetDefaultFontStyle("Bold", 13));
    }
}

void SClassroomConnectPanel::Construct(const FArguments& InArgs)
{
    Session = InArgs._Session;
    Teams = InArgs._Teams;
    TeamsWarning = InArgs._TeamsWarning;
    check(Session.IsValid() && Teams.Num() > 0);
    LoadPreferences();
    CodeText = Session->GetJoinCode(); // e.g. carried into an editor reopened on the team project

    const TSharedRef<SUniformGridPanel> TeamGrid = SNew(SUniformGridPanel).SlotPadding(FMargin(4.f)).MinDesiredSlotHeight(52.f);
    for (int32 Index = 0; Index < Teams.Num(); ++Index)
        TeamGrid->AddSlot(Index % TilesPerRow, Index / TilesPerRow)[ MakeTeamTile(Index) ];

    ChildSlot
    [
        SNew(SScrollBox)
        + SScrollBox::Slot().Padding(16.f)
        [
            SNew(SVerticalBox)
            + SVerticalBox::Slot().AutoHeight()
            [ SNew(STextBlock).Text(LOCTEXT("PanelTitle", "CLASSROOM CONNECT")).Font(FCoreStyle::GetDefaultFontStyle("Bold", 22)) ]

            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 16.f, 0.f, 6.f)
            [ Heading(LOCTEXT("SelectTeam", "Select Team")) ]
            + SVerticalBox::Slot().AutoHeight()
            [ TeamGrid ]

            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 16.f, 0.f, 6.f)
            [ Heading(LOCTEXT("ChooseRole", "Choose Role")) ]
            + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left)
            [
                SNew(SSegmentedControl<EClassroomRole>)
                .IsEnabled(this, &SClassroomConnectPanel::CanChangeSelection)
                .TextStyle(FAppStyle::Get(), "NormalText")
                .UniformPadding(FMargin(28.f, 8.f))
                .Value_Lambda([this] { return Role; })
                .OnValueChanged_Lambda([this](EClassroomRole NewRole) { Role = NewRole; SavePreferences(); })
                + SSegmentedControl<EClassroomRole>::Slot(EClassroomRole::Leader)
                    .Text(LOCTEXT("Leader", "Team Leader"))
                    .ToolTip(LOCTEXT("LeaderTip", "Hosts the team's project from this computer. Each team has one leader."))
                + SSegmentedControl<EClassroomRole>::Slot(EClassroomRole::Member)
                    .Text(LOCTEXT("Member", "Team Member"))
                    .ToolTip(LOCTEXT("MemberTip", "Joins your team leader's live session."))
            ]

            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 16.f, 0.f, 6.f)
            [ Heading(LOCTEXT("JoinCodeHeading", "Join Code (optional)")) ]
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
                [
                    SNew(SBox).WidthOverride(180.f)
                    [
                        SNew(SEditableTextBox)
                        .IsEnabled(this, &SClassroomConnectPanel::CanChangeSelection)
                        .Font(FCoreStyle::GetDefaultFontStyle("Bold", 14))
                        .HintText(LOCTEXT("JoinCodeHint", "no code"))
                        .ToolTipText(LOCTEXT("JoinCodeTip", "Letters and numbers; capitals, spaces and dashes do not matter."))
                        .Text_Lambda([this] { return FText::FromString(CodeText); })
                        .OnTextChanged_Lambda([this](const FText& NewText) { CodeText = NewText.ToString(); })
                    ]
                ]
                + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(8.f, 0.f, 0.f, 0.f)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("NewCode", "New code"))
                    .ToolTipText(LOCTEXT("NewCodeTip", "Make up a random 6-character code to read out to your team."))
                    .Visibility_Lambda([this] { return Role == EClassroomRole::Leader ? EVisibility::Visible : EVisibility::Collapsed; })
                    .IsEnabled(this, &SClassroomConnectPanel::CanChangeSelection)
                    .OnClicked_Lambda([this] { CodeText = ClassroomJoinCode::Make(); return FReply::Handled(); })
                ]
            ]
            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 4.f, 0.f, 0.f)
            [
                SNew(STextBlock)
                .AutoWrapText(true)
                .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                .Text_Lambda([this]
                {
                    return Role == EClassroomRole::Leader
                        ? LOCTEXT("JoinCodeHelpLeader", "With a code, only teammates who type the same code can find your team and get the project. Leave it empty to let anyone in the room join.")
                        : LOCTEXT("JoinCodeHelpMember", "If your team leader set a join code, type it here.");
                })
            ]

            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 20.f, 0.f, 0.f)
            [
                SNew(SButton)
                .ButtonStyle(FAppStyle::Get(), "PrimaryButton")
                .HAlign(HAlign_Center)
                .ContentPadding(FMargin(24.f, 12.f))
                .IsEnabled(this, &SClassroomConnectPanel::CanChangeSelection)
                .OnClicked(this, &SClassroomConnectPanel::OnPrimaryClicked)
                [
                    SNew(STextBlock)
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 16))
                    .Text_Lambda([this] { return Role == EClassroomRole::Leader ? LOCTEXT("Host", "HOST PROJECT") : LOCTEXT("Join", "JOIN TEAM"); })
                ]
            ]

            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 20.f, 0.f, 6.f)
            [ Heading(LOCTEXT("Status", "Status")) ]
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SBorder)
                .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
                .Padding(12.f)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()
                    [ MakeStatusRow(LOCTEXT("StateLabel", "Session"), TAttribute<FText>::CreateSP(this, &SClassroomConnectPanel::GetStateText), TAttribute<FSlateColor>::CreateSP(this, &SClassroomConnectPanel::GetStateColor)) ]
                    + SVerticalBox::Slot().AutoHeight()
                    [ MakeStatusRow(LOCTEXT("TeamLabel", "Team"), TAttribute<FText>::CreateLambda([this] { return Teams[SelectedTeam].DisplayName; })) ]
                    + SVerticalBox::Slot().AutoHeight()
                    [ MakeStatusRow(LOCTEXT("ProjectLabel", "Project"), TAttribute<FText>::CreateLambda([] { return FText::FromString(FApp::GetProjectName()); })) ]
                    + SVerticalBox::Slot().AutoHeight()
                    [ MakeStatusRow(LOCTEXT("HostLabel", "Host computer"), TAttribute<FText>::CreateLambda([this]
                        { return Session->GetHostComputer().IsEmpty() ? LOCTEXT("NoHostValue", "-") : FText::FromString(Session->GetHostComputer()); })) ]
                    + SVerticalBox::Slot().AutoHeight()
                    [
                        SNew(SBox)
                        .Visibility_Lambda([this] { return Session->GetState() != EClassroomState::Idle && !Session->GetJoinCode().IsEmpty() ? EVisibility::Visible : EVisibility::Collapsed; })
                        [ MakeStatusRow(LOCTEXT("JoinCodeLabel", "Join code"), TAttribute<FText>::CreateLambda([this] { return FText::FromString(Session->GetJoinCode()); })) ]
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
                    [ SNew(STextBlock).AutoWrapText(true).Text_Lambda([this] { return Session->GetMessage(); }) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.f, 6.f, 0.f, 0.f)
                    [
                        SNew(SBox).HeightOverride(10.f)
                        .Visibility_Lambda([this] { return Session->GetTransferProgress() >= 0.f ? EVisibility::Visible : EVisibility::Collapsed; })
                        [ SNew(SProgressBar).Percent_Lambda([this] { return TOptional<float>(FMath::Max(0.f, Session->GetTransferProgress())); }) ]
                    ]
                ]
            ]

            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 20.f, 0.f, 6.f)
            [ Heading(LOCTEXT("Teammates", "Connected Students")) ]
            + SVerticalBox::Slot().AutoHeight()
            [
                SNew(SBorder)
                .BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
                .Padding(12.f)
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()
                    [ SAssignNew(ParticipantRows, SVerticalBox) ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
                    [
                        SNew(STextBlock)
                        .AutoWrapText(true)
                        .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                        .Text(LOCTEXT("MarkerHelp", "Teammates appear in your viewport, in their color with their name, when you have the same saved level open. Go To moves your view to just behind them."))
                        .Visibility_Lambda([this] { return Session->GetParticipants().IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible; })
                    ]
                    + SVerticalBox::Slot().AutoHeight().Padding(0.f, 8.f, 0.f, 0.f)
                    [
                        SNew(SCheckBox)
                        .IsChecked_Lambda([this] { return Session->GetShowMarkers() ? ECheckBoxState::Checked : ECheckBoxState::Unchecked; })
                        .OnCheckStateChanged_Lambda([this](ECheckBoxState NewState) { Session->SetShowMarkers(NewState == ECheckBoxState::Checked); })
                        .ToolTipText(LOCTEXT("ShowMarkersTip", "A large arrow and name in each teammate's color, which grows with distance so you can find them across big maps"))
                        [ SNew(STextBlock).Text(LOCTEXT("ShowMarkers", "Show large name markers above teammates")) ]
                    ]
                ]
            ]

            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 12.f, 0.f, 0.f)
            [
                SNew(SHorizontalBox)
                + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("Leave", "Leave Team"))
                    .ToolTipText_Lambda([this] { return Role == EClassroomRole::Leader ? LOCTEXT("LeaveTipLeader", "Save your team's work into this project, then leave the live session.") : LOCTEXT("LeaveTip", "Leave the live session. Your team leader's project keeps the team's work."); })
                    .IsEnabled_Lambda([this] { return Session->GetState() == EClassroomState::Connected || Session->GetState() == EClassroomState::Connecting; })
                    .OnClicked_Lambda([this] { Session->Leave(); return FReply::Handled(); })
                ]
                + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 0.f, 8.f, 0.f)
                [
                    SNew(SButton)
                    .Text(LOCTEXT("Retry", "Retry"))
                    .IsEnabled_Lambda([this] { return Session->GetState() == EClassroomState::Failed; })
                    .OnClicked_Lambda([this] { Session->Retry(); return FReply::Handled(); })
                ]
                + SHorizontalBox::Slot().AutoWidth()
                [
                    SNew(SButton)
                    .Text(LOCTEXT("End", "End Session"))
                    .ToolTipText(LOCTEXT("EndTip", "Team Leader only: save your team's work into this project, leave the session, and stop this computer's team server."))
                    .Visibility_Lambda([this] { return Role == EClassroomRole::Leader ? EVisibility::Visible : EVisibility::Collapsed; })
                    .IsEnabled_Lambda([this] { return Session->GetRole() == EClassroomRole::Leader && Session->GetState() != EClassroomState::Idle && Session->GetState() != EClassroomState::SavingWork; })
                    .OnClicked_Lambda([this] { Session->EndHosting(); return FReply::Handled(); })
                ]
            ]

            + SVerticalBox::Slot().AutoHeight().Padding(0.f, 16.f, 0.f, 0.f)
            [
                SNew(SExpandableArea)
                .InitiallyCollapsed(true)
                .AreaTitle(LOCTEXT("Diagnostics", "View Diagnostics"))
                .BodyContent()
                [
                    SNew(SVerticalBox)
                    + SVerticalBox::Slot().AutoHeight()
                    [ SNew(STextBlock).AutoWrapText(true).Text(this, &SClassroomConnectPanel::GetDiagnosticsText) ]
                    + SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Left).Padding(0.f, 8.f, 0.f, 0.f)
                    [
                        SNew(SButton)
                        .Text(LOCTEXT("Browser", "Open Multi-User Browser (advanced)"))
                        .OnClicked_Lambda([] { IMultiUserClientModule::Get().OpenBrowser(); return FReply::Handled(); })
                    ]
                ]
            ]
        ]
    ];
}

TSharedRef<SWidget> SClassroomConnectPanel::MakeTeamTile(int32 Index)
{
    const FClassroomTeam& Team = Teams[Index];
    return SNew(SBorder)
        .BorderImage(FAppStyle::GetBrush("WhiteBrush"))
        .Padding(3.f)
        .BorderBackgroundColor_Lambda([this, Index] { return FSlateColor(Index == SelectedTeam ? FLinearColor::White : FLinearColor::Transparent); })
        [
            SNew(SButton)
            .ButtonStyle(FAppStyle::Get(), "NoBorder")
            .ContentPadding(0.f)
            .IsEnabled(this, &SClassroomConnectPanel::CanChangeSelection)
            .ToolTipText(FText::Format(LOCTEXT("TeamTip", "Team {0}"), Team.DisplayName))
            .OnClicked_Lambda([this, Index] { SelectedTeam = Index; SavePreferences(); return FReply::Handled(); })
            [
                SNew(SBorder)
                .BorderImage(FAppStyle::GetBrush("WhiteBrush"))
                .BorderBackgroundColor(Team.Color)
                .HAlign(HAlign_Center)
                .VAlign(VAlign_Center)
                .Padding(FMargin(6.f, 12.f))
                [
                    SNew(STextBlock)
                    .Text(Team.DisplayName)
                    .Justification(ETextJustify::Center)
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 12))
                    .ColorAndOpacity(ClassroomTeams::IsLight(Team.Color) ? FLinearColor::Black : FLinearColor::White)
                ]
            ]
        ];
}

void SClassroomConnectPanel::Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime)
{
    SCompoundWidget::Tick(AllottedGeometry, InCurrentTime, InDeltaTime);
    if (ShownParticipantsRevision != Session->GetParticipantsRevision() || ParticipantRows->NumSlots() == 0)
    {
        ShownParticipantsRevision = Session->GetParticipantsRevision();
        RebuildParticipantRows();
    }
}

void SClassroomConnectPanel::RebuildParticipantRows()
{
    ParticipantRows->ClearChildren();
    const TArray<FClassroomParticipant>& Participants = Session->GetParticipants();
    if (Participants.IsEmpty())
    {
        ParticipantRows->AddSlot().AutoHeight()
        [ SNew(STextBlock).ColorAndOpacity(FSlateColor::UseSubduedForeground()).Text(LOCTEXT("NoStudents", "Nobody yet. Students appear here once you are connected.")) ];
        return;
    }
    for (const FClassroomParticipant& Row : Participants)
    {
        FText Where;
        if (Row.bIsLocal) Where = Row.Level.IsEmpty() ? FText::GetEmpty() : FText::Format(LOCTEXT("YouAreIn", "in {0}"), FText::FromString(Row.Level));
        else if (Row.Level.IsEmpty()) Where = LOCTEXT("LevelUnknown", "level not reported yet");
        else if (Row.bSameLevel) Where = FText::Format(LOCTEXT("SameLevel", "in {0} with you"), FText::FromString(Row.Level));
        else Where = FText::Format(LOCTEXT("OtherLevel", "in {0}: open that level to see their marker"), FText::FromString(Row.Level));

        const FGuid EndpointId = Row.EndpointId;
        ParticipantRows->AddSlot().AutoHeight().Padding(0.f, 3.f)
        [
            SNew(SHorizontalBox)
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0.f, 0.f, 10.f, 0.f)
            [
                SNew(SBox).WidthOverride(18.f).HeightOverride(18.f)
                [ SNew(SImage).Image(FAppStyle::GetBrush("WhiteBrush")).ColorAndOpacity(Row.Color).ToolTipText(FText::FromString(Row.ColorName)) ]
            ]
            + SHorizontalBox::Slot().FillWidth(1.f).VAlign(VAlign_Center)
            [
                SNew(SVerticalBox)
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock)
                    .Font(FCoreStyle::GetDefaultFontStyle("Bold", 11))
                    .Text(FText::Format(Row.bIsLocal ? LOCTEXT("YouRow", "{0} (you) - {1}") : LOCTEXT("OtherRow", "{0} - {1}"), FText::FromString(Row.Name), FText::FromString(Row.ColorName)))
                ]
                + SVerticalBox::Slot().AutoHeight()
                [
                    SNew(STextBlock)
                    .ColorAndOpacity(FSlateColor::UseSubduedForeground())
                    .Text(Where.IsEmpty() ? FText::FromString(Row.Computer) : FText::Format(LOCTEXT("ComputerWhere", "{0}, {1}"), FText::FromString(Row.Computer), Where))
                ]
            ]
            + SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
            [
                SNew(SButton)
                .Text(LOCTEXT("GoTo", "Go To"))
                .ToolTipText(LOCTEXT("GoToTip", "Move your view to just behind this student"))
                .Visibility(Row.bIsLocal ? EVisibility::Collapsed : EVisibility::Visible)
                .IsEnabled(Row.bSameLevel)
                .OnClicked_Lambda([this, EndpointId] { Session->JumpTo(EndpointId); return FReply::Handled(); })
            ]
        ];
    }
}

TSharedRef<SWidget> SClassroomConnectPanel::MakeStatusRow(const FText& Label, const TAttribute<FText>& Value, const TAttribute<FSlateColor>& Color)
{
    return SNew(SHorizontalBox)
        + SHorizontalBox::Slot().AutoWidth().Padding(0.f, 2.f, 12.f, 2.f)
        [
            SNew(SBox).WidthOverride(150.f)
            [ SNew(STextBlock).Text(Label).ColorAndOpacity(FSlateColor::UseSubduedForeground()) ]
        ]
        + SHorizontalBox::Slot().FillWidth(1.f).Padding(0.f, 2.f)
        [ SNew(STextBlock).Text(Value).ColorAndOpacity(Color).AutoWrapText(true) ];
}

FReply SClassroomConnectPanel::OnPrimaryClicked()
{
    const FClassroomTeam& Team = Teams[SelectedTeam];
    if (Role == EClassroomRole::Leader) Session->Host(Team, CodeText);
    else Session->Join(Team, CodeText);
    CodeText = ClassroomJoinCode::Normalize(CodeText); // Show the code as it is compared: "ab 12" -> "AB12".
    return FReply::Handled();
}

bool SClassroomConnectPanel::CanChangeSelection() const
{
    return !Session->IsBusy() && Session->GetState() != EClassroomState::Connected;
}

FText SClassroomConnectPanel::GetStateText() const
{
    switch (Session->GetState())
    {
    case EClassroomState::CheckingTeam:   return LOCTEXT("StateChecking", "Checking team");
    case EClassroomState::StartingServer: return LOCTEXT("StateStarting", "Starting team server");
    case EClassroomState::FindingHost:    return LOCTEXT("StateFinding", "Finding team leader");
    case EClassroomState::Connecting:     return LOCTEXT("StateConnecting", "Connecting");
    case EClassroomState::Connected:      return LOCTEXT("StateConnected", "Connected");
    case EClassroomState::SavingWork:     return LOCTEXT("StateSaving", "Saving team work");
    case EClassroomState::GettingProject: return LOCTEXT("StateGettingProject", "Getting team project");
    case EClassroomState::Reopening:      return LOCTEXT("StateReopening", "Opening team project");
    case EClassroomState::Failed:         return LOCTEXT("StateFailed", "Needs attention");
    default:                              return LOCTEXT("StateIdle", "Not connected");
    }
}

FSlateColor SClassroomConnectPanel::GetStateColor() const
{
    switch (Session->GetState())
    {
    case EClassroomState::Connected: return FSlateColor(FLinearColor(0.2f, 0.8f, 0.3f));
    case EClassroomState::Failed:    return FSlateColor(FLinearColor(1.0f, 0.35f, 0.25f));
    case EClassroomState::Idle:      return FSlateColor::UseSubduedForeground();
    default:                         return FSlateColor(FLinearColor(1.0f, 0.75f, 0.2f));
    }
}

FText SClassroomConnectPanel::GetDiagnosticsText() const
{
    FString Text = FString::Join(Session->GetDiagnostics(), TEXT("\n"));
    if (!TeamsWarning.IsEmpty()) Text += TEXT("\n") + TeamsWarning;
    return FText::FromString(Text);
}

void SClassroomConnectPanel::LoadPreferences()
{
    // Stored per user (EditorSettings.ini), so the choice survives switching to the team project.
    FString TeamId, RoleName;
    if (GConfig->GetString(PreferencesSection, TEXT("Team"), TeamId, GEditorSettingsIni))
    {
        const int32 Found = Teams.IndexOfByPredicate([&TeamId](const FClassroomTeam& Team) { return Team.Id == TeamId; });
        if (Found != INDEX_NONE) SelectedTeam = Found;
    }
    if (GConfig->GetString(PreferencesSection, TEXT("Role"), RoleName, GEditorSettingsIni))
        Role = RoleName == TEXT("Leader") ? EClassroomRole::Leader : EClassroomRole::Member;
}

void SClassroomConnectPanel::SavePreferences() const
{
    GConfig->SetString(PreferencesSection, TEXT("Team"), *Teams[SelectedTeam].Id, GEditorSettingsIni);
    GConfig->SetString(PreferencesSection, TEXT("Role"), Role == EClassroomRole::Leader ? TEXT("Leader") : TEXT("Member"), GEditorSettingsIni);
    GConfig->Flush(false, GEditorSettingsIni);
}

#undef LOCTEXT_NAMESPACE
