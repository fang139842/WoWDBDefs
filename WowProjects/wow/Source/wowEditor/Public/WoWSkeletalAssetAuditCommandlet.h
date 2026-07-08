#pragma once

#include "Commandlets/Commandlet.h"

#include "WoWSkeletalAssetAuditCommandlet.generated.h"

UCLASS()
class WOWEDITOR_API UWoWSkeletalAssetAuditCommandlet : public UCommandlet
{
	GENERATED_BODY()

public:
	UWoWSkeletalAssetAuditCommandlet();

	virtual int32 Main(const FString& Params) override;
};
