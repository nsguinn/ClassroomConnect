#pragma once

#include "CoreMinimal.h"

/** A classroom team. Id is stable and used in Multi-User server/session names; DisplayName is an administrator-editable label. */
struct FClassroomTeam
{
    FString Id;
    FText DisplayName;
    FLinearColor Color = FLinearColor::White;
};

namespace ClassroomTeams
{
    /** Loads Config/ClassroomTeams.json from the plugin folder. Falls back to the 20 built-in teams and explains why in OutWarning. */
    TArray<FClassroomTeam> Load(FString& OutWarning);

    /** Session every member of the team joins, e.g. "UCC-Red". */
    FString SessionName(const FClassroomTeam& Team);

    /** Server name advertised by the hosting computer, e.g. "UCC-Red-LAB12". */
    FString ServerName(const FClassroomTeam& Team);

    /** Prefix shared by every server hosting this team; used to find the host and detect duplicates. */
    FString ServerPrefix(const FClassroomTeam& Team);

    /** True when dark text reads better than white text on this color. */
    bool IsLight(const FLinearColor& Color);
}
