#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "ClassroomMarkerActor.generated.h"

class UMaterialInstanceDynamic;
class UStaticMeshComponent;
class UTextRenderComponent;

/**
 * A large marker above a teammate in the same level: a downward arrow in their color with their name, turned toward this
 * editor's camera and growing with distance so it stays readable across large maps. It follows the teammate's Multi-User
 * presence position and supplements Unreal's own, much smaller presence avatar.
 * Transient and editor-only, like Unreal's presence actors: never saved, selectable or listed in the Outliner.
 */
UCLASS(Transient, NotPlaceable)
class AClassroomMarkerActor : public AActor
{
    GENERATED_BODY()

public:
    AClassroomMarkerActor();

    /** Sets whose position the marker follows and how it is labeled; cheap to call repeatedly. */
    void Configure(const FGuid& InEndpointId, const FString& InName, const FLinearColor& InColor);

    virtual void Tick(float DeltaSeconds) override;
    virtual bool ShouldTickIfViewportsOnly() const override { return true; }
    virtual bool IsEditorOnly() const override { return true; }
#if WITH_EDITOR
    virtual bool IsSelectable() const override { return false; }
    virtual bool IsListedInSceneOutliner() const override { return false; }
#endif

private:
    UPROPERTY()
    TObjectPtr<USceneComponent> Root;

    UPROPERTY()
    TObjectPtr<UStaticMeshComponent> Arrow;

    UPROPERTY()
    TObjectPtr<UTextRenderComponent> Label;

    UPROPERTY()
    TObjectPtr<UMaterialInstanceDynamic> ArrowMaterial;

    FGuid EndpointId;
    FString Name;
    FLinearColor Color = FLinearColor::Transparent;
    bool bPlaced = false;
};
