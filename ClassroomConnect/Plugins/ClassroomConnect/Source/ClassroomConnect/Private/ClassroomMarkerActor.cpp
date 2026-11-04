#include "ClassroomMarkerActor.h"

#include "Components/StaticMeshComponent.h"
#include "Components/TextRenderComponent.h"
#include "Editor.h"
#include "Engine/StaticMesh.h"
#include "LevelEditorViewport.h"
#include "IConcertClientPresenceManager.h"
#include "IConcertSyncClient.h"
#include "IMultiUserClientModule.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
    // Sizes in centimeters at 1x scale, relative to the teammate's camera position.
    constexpr float ArrowHeight = 60.f;
    constexpr float ArrowWidth = 50.f;
    constexpr float ArrowGap = 40.f;   // space between the teammate's camera and the arrow tip
    constexpr float LabelSize = 36.f;
    /** Beyond this distance the marker grows with distance, so it keeps about the same size on screen. */
    constexpr float FullSizeDistance = 1200.f;
    constexpr float MaxScale = 30.f;
    constexpr float FollowSpeed = 12.f;
}

AClassroomMarkerActor::AClassroomMarkerActor()
{
    PrimaryActorTick.bCanEverTick = true;
    PrimaryActorTick.bStartWithTickEnabled = true;
    bIsEditorOnlyActor = true;
    SetCanBeDamaged(false);

    Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
    Root->SetMobility(EComponentMobility::Movable);
    RootComponent = Root;

    // Engine cone: 100 cm tall, centered on its pivot, tip toward +Z. Flipped so the tip points down at the teammate.
    static ConstructorHelpers::FObjectFinder<UStaticMesh> Cone(TEXT("/Engine/BasicShapes/Cone.Cone"));
    Arrow = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Arrow"));
    Arrow->SetupAttachment(Root);
    Arrow->SetMobility(EComponentMobility::Movable);
    Arrow->SetStaticMesh(Cone.Object);
    Arrow->SetRelativeRotation(FRotator(180.f, 0.f, 0.f));
    Arrow->SetRelativeScale3D(FVector(ArrowWidth / 100.f, ArrowWidth / 100.f, ArrowHeight / 100.f));
    Arrow->SetRelativeLocation(FVector(0.f, 0.f, ArrowGap + ArrowHeight / 2.f));
    Arrow->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Arrow->SetCastShadow(false);
    Arrow->bSelectable = false;

    // Text renders facing +X; the actor is yawed toward the camera every frame.
    Label = CreateDefaultSubobject<UTextRenderComponent>(TEXT("Label"));
    Label->SetupAttachment(Root);
    Label->SetMobility(EComponentMobility::Movable);
    Label->SetHorizontalAlignment(EHTA_Center);
    Label->SetVerticalAlignment(EVRTA_TextBottom);
    Label->SetWorldSize(LabelSize);
    Label->SetRelativeLocation(FVector(0.f, 0.f, ArrowGap + ArrowHeight + 10.f));
    Label->SetCollisionEnabled(ECollisionEnabled::NoCollision);
    Label->SetCastShadow(false);
    Label->bSelectable = false;
}

void AClassroomMarkerActor::Configure(const FGuid& InEndpointId, const FString& InName, const FLinearColor& InColor)
{
    EndpointId = InEndpointId;
    if (InName != Name)
    {
        Name = InName;
        Label->SetText(FText::FromString(Name));
    }
    if (!InColor.Equals(Color))
    {
        Color = InColor;
        if (!ArrowMaterial)
        {
            UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Engine/BasicShapes/BasicShapeMaterial.BasicShapeMaterial"));
            ArrowMaterial = Base ? UMaterialInstanceDynamic::Create(Base, this) : nullptr;
            if (ArrowMaterial) Arrow->SetMaterial(0, ArrowMaterial);
        }
        if (ArrowMaterial) ArrowMaterial->SetVectorParameterValue(TEXT("Color"), Color);
        Label->SetTextRenderColor(Color.ToFColor(true));
    }
}

void AClassroomMarkerActor::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

    const TSharedPtr<IConcertSyncClient> SyncClient = IMultiUserClientModule::IsAvailable() ? IMultiUserClientModule::Get().GetClient() : nullptr;
    IConcertClientPresenceManager* Presence = SyncClient.IsValid() ? SyncClient->GetPresenceManager() : nullptr;
    if (!Presence) return;

    // Follow the teammate's camera, smoothing the presence updates (which arrive a few times per second).
    const FVector Target = Presence->GetPresenceTransform(EndpointId).GetLocation();
    const FVector Location = bPlaced ? FMath::VInterpTo(GetActorLocation(), Target, DeltaSeconds, FollowSpeed) : Target;
    bPlaced = true;

    // Turn toward this editor's camera (yaw only, so the arrow stays upright) and grow with distance.
    FRotator Facing = GetActorRotation();
    float Scale = 1.f;
    if (GCurrentLevelEditingViewportClient)
    {
        const FVector ToCamera = GCurrentLevelEditingViewportClient->GetViewLocation() - Location;
        Facing = FRotator(0.f, ToCamera.Rotation().Yaw, 0.f);
        Scale = FMath::Clamp(static_cast<float>(ToCamera.Size()) / FullSizeDistance, 1.f, MaxScale);
    }
    SetActorLocationAndRotation(Location, Facing);
    SetActorScale3D(FVector(Scale));
}
