#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "WoWPreviewTargetCharacter.generated.h"

class UWoWCharacterPreviewComponent;

UCLASS()
class WOW_API AWoWPreviewTargetCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	AWoWPreviewTargetCharacter();

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

	void TriggerTestAttack();
	void BeginAutoAttackPlayer(ACharacter* InTarget);
	void StopAutoAttack(float LingerSeconds);

private:
	UPROPERTY(VisibleAnywhere, Category = "WoW Preview")
	TObjectPtr<UWoWCharacterPreviewComponent> PreviewComponent;

	UPROPERTY(EditDefaultsOnly, Category = "WoW Combat")
	float AttackIntervalSeconds = 2.0f;

	UPROPERTY(EditDefaultsOnly, Category = "WoW Combat")
	float AttackRange = 320.0f;

	UPROPERTY(EditDefaultsOnly, Category = "WoW Combat")
	float CombatReadyLingerSeconds = 0.75f;

	TWeakObjectPtr<ACharacter> AutoAttackTarget;
	float AttackTimerSeconds = 0.0f;
};
