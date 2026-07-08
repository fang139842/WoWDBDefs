#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "WoWGameMode.generated.h"

UCLASS()
class WOW_API AWoWGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AWoWGameMode();

	virtual void StartPlay() override;
};
