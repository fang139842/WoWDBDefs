#include "WoWPreviewTargetCharacter.h"

#include "WoWCharacterPreviewComponent.h"

#include "Components/CapsuleComponent.h"

AWoWPreviewTargetCharacter::AWoWPreviewTargetCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	if (UCapsuleComponent* Capsule = GetCapsuleComponent())
	{
		Capsule->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
		Capsule->SetCollisionResponseToChannel(ECC_Visibility, ECR_Block);
	}
}

void AWoWPreviewTargetCharacter::BeginPlay()
{
	Super::BeginPlay();

	// Enemy/target visuals must be driven by the Creature runtime, not by the selected player-character preview.
	// Keep this actor as a combat-click target only until the creature display system is wired in.
}

void AWoWPreviewTargetCharacter::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	ACharacter* Target = AutoAttackTarget.Get();
	if (!Target || !PreviewComponent)
	{
		return;
	}

	PreviewComponent->SetCombatReady(true, 0.0f);

	const FVector ToTarget = Target->GetActorLocation() - GetActorLocation();
	const float Distance2D = FVector(ToTarget.X, ToTarget.Y, 0.0f).Size();
	if (Distance2D > AttackRange)
	{
		AttackTimerSeconds = 0.0f;
		return;
	}

	if (!ToTarget.IsNearlyZero())
	{
		SetActorRotation(FRotator(0.0f, ToTarget.Rotation().Yaw, 0.0f));
	}

	AttackTimerSeconds -= DeltaSeconds;
	if (AttackTimerSeconds <= 0.0f)
	{
		PreviewComponent->TriggerAttack();
		AttackTimerSeconds = AttackIntervalSeconds;
	}
}

void AWoWPreviewTargetCharacter::TriggerTestAttack()
{
	if (PreviewComponent)
	{
		PreviewComponent->TriggerAttack();
	}
}

void AWoWPreviewTargetCharacter::BeginAutoAttackPlayer(ACharacter* InTarget)
{
	AutoAttackTarget = InTarget;
	AttackTimerSeconds = 0.0f;
	if (PreviewComponent)
	{
		PreviewComponent->SetCombatReady(true, 0.0f);
	}
}

void AWoWPreviewTargetCharacter::StopAutoAttack(float LingerSeconds)
{
	AutoAttackTarget.Reset();
	AttackTimerSeconds = 0.0f;
	if (PreviewComponent)
	{
		PreviewComponent->SetCombatReady(false, LingerSeconds > 0.0f ? LingerSeconds : CombatReadyLingerSeconds);
	}
}
