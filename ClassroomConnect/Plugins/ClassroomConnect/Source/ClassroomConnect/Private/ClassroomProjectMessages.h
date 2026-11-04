#pragma once

#include "CoreMinimal.h"
#include "ClassroomProjectMessages.generated.h"

// Messages for copying a team leader's project to team members. They travel over Unreal's message bus (UDP messaging),
// the same transport Multi-User uses, so no additional ports, shares or firewall rules are involved.
// A member publishes a query; the leader's editor answers with an offer, then serves the manifest and file chunks.
// When the leader set a join code, every request must carry its key (ClassroomJoinCode::Key) or it is refused.

/** One file of a project baseline. Path is relative to the project folder and uses '/'. */
USTRUCT()
struct FClassroomManifestEntry
{
    GENERATED_BODY()

    UPROPERTY()
    FString Path;

    UPROPERTY()
    int64 Size = 0;

    /** Uppercase hex SHA-256 of the file contents. */
    UPROPERTY()
    FString Sha256;
};

USTRUCT()
struct FClassroomProjectQuery
{
    GENERATED_BODY()

    UPROPERTY()
    FString TeamId;

    /** ClassroomJoinCode::Key of the member's join code, or empty without a code. Must equal the leader's. */
    UPROPERTY()
    FString JoinKey;
};

USTRUCT()
struct FClassroomProjectOffer
{
    GENERATED_BODY()

    UPROPERTY()
    FString TeamId;

    UPROPERTY()
    FString ProjectName;

    /** SHA-256 over the whole manifest; changes whenever any file changes. */
    UPROPERTY()
    FString Revision;

    UPROPERTY()
    FString Computer;

    /** The saved level the leader has open (e.g. /Game/Maps/Island), so a member's reopened editor starts there. */
    UPROPERTY()
    FString Level;

    UPROPERTY()
    int32 FileCount = 0;

    UPROPERTY()
    int64 TotalBytes = 0;

    /** The leader hosts this team with a join code and the query's JoinKey did not match it. Only TeamId and Computer are set. */
    UPROPERTY()
    bool bJoinCodeRejected = false;
};

USTRUCT()
struct FClassroomManifestRequest
{
    GENERATED_BODY()

    UPROPERTY()
    FString TeamId;

    UPROPERTY()
    FString Revision;

    UPROPERTY()
    int32 Page = 0;

    UPROPERTY()
    FString JoinKey;
};

USTRUCT()
struct FClassroomManifestPage
{
    GENERATED_BODY()

    UPROPERTY()
    FString TeamId;

    UPROPERTY()
    FString Revision;

    UPROPERTY()
    int32 Page = 0;

    UPROPERTY()
    int32 PageCount = 0;

    UPROPERTY()
    TArray<FClassroomManifestEntry> Entries;

    UPROPERTY()
    FString Error;
};

USTRUCT()
struct FClassroomChunkRequest
{
    GENERATED_BODY()

    UPROPERTY()
    FString TeamId;

    UPROPERTY()
    FString Revision;

    UPROPERTY()
    int32 FileIndex = 0;

    UPROPERTY()
    int64 Offset = 0;

    UPROPERTY()
    int32 Size = 0;

    UPROPERTY()
    FString JoinKey;
};

USTRUCT()
struct FClassroomChunkReply
{
    GENERATED_BODY()

    UPROPERTY()
    FString TeamId;

    UPROPERTY()
    FString Revision;

    UPROPERTY()
    int32 FileIndex = 0;

    UPROPERTY()
    int64 Offset = 0;

    UPROPERTY()
    TArray<uint8> Data;

    UPROPERTY()
    FString Error;
};
