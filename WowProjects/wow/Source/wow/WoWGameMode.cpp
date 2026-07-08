#include "WoWGameMode.h"

#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "UObject/ConstructorHelpers.h"
#include "WoWPlayerController.h"
#include "WoWPreviewTargetCharacter.h"

AWoWGameMode::AWoWGameMode()
{
	PlayerControllerClass = AWoWPlayerController::StaticClass();

	static ConstructorHelpers::FClassFinder<APawn> ThirdPersonCharacterClass(
		TEXT("/Game/ThirdPerson/Blueprints/BP_ThirdPersonCharacter"));
	if (ThirdPersonCharacterClass.Succeeded())
	{
		DefaultPawnClass = ThirdPersonCharacterClass.Class;
	}
}

void AWoWGameMode::StartPlay()
{
	Super::StartPlay();

	UWorld* World = GetWorld();
	const APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
	const APawn* PlayerPawn = PlayerController ? PlayerController->GetPawn() : nullptr;
	if (!World || !PlayerPawn)
	{
		return;
	}

	const FVector TargetLocation =
		PlayerPawn->GetActorLocation() +
		PlayerPawn->GetActorForwardVector() * 420.0f +
		PlayerPawn->GetActorRightVector() * 120.0f;
	const FRotator TargetRotation = (PlayerPawn->GetActorLocation() - TargetLocation).Rotation();

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.Name = TEXT("WoWPreviewAttackTarget");
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	World->SpawnActor<AWoWPreviewTargetCharacter>(AWoWPreviewTargetCharacter::StaticClass(), TargetLocation, TargetRotation, SpawnParameters);
}
