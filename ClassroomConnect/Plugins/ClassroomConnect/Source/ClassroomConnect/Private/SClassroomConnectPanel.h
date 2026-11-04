#pragma once

#include "CoreMinimal.h"
#include "Widgets/SCompoundWidget.h"
#include "ClassroomSession.h"
#include "ClassroomTeams.h"

class SVerticalBox;

/** The student-facing window: pick a team tile, pick a role, press one button. */
class SClassroomConnectPanel : public SCompoundWidget
{
public:
    SLATE_BEGIN_ARGS(SClassroomConnectPanel) {}
        SLATE_ARGUMENT(TSharedPtr<FClassroomSession>, Session)
        SLATE_ARGUMENT(TArray<FClassroomTeam>, Teams)
        SLATE_ARGUMENT(FString, TeamsWarning)
    SLATE_END_ARGS()

    void Construct(const FArguments& InArgs);
    virtual void Tick(const FGeometry& AllottedGeometry, const double InCurrentTime, const float InDeltaTime) override;

private:
    TSharedRef<SWidget> MakeTeamTile(int32 Index);
    void RebuildParticipantRows();
    TSharedRef<SWidget> MakeStatusRow(const FText& Label, const TAttribute<FText>& Value, const TAttribute<FSlateColor>& Color = FSlateColor::UseForeground());
    FReply OnPrimaryClicked();
    bool CanChangeSelection() const;
    FText GetStateText() const;
    FSlateColor GetStateColor() const;
    FText GetDiagnosticsText() const;
    void LoadPreferences();
    void SavePreferences() const;

    TSharedPtr<FClassroomSession> Session;
    TArray<FClassroomTeam> Teams;
    FString TeamsWarning;
    int32 SelectedTeam = 0;
    EClassroomRole Role = EClassroomRole::Member;
    /** What the student typed under Join code. Never written to disk. */
    FString CodeText;
    TSharedPtr<SVerticalBox> ParticipantRows;
    uint32 ShownParticipantsRevision = 0;
};
