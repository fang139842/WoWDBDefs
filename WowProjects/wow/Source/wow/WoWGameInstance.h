#pragma once

#include "CoreMinimal.h"
#include "Engine/GameInstance.h"
#include "WoWGameInstance.generated.h"

USTRUCT(BlueprintType)
struct FWoWSelectedCharacterRuntimeAppearance
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	int32 Guid = 0;

	UPROPERTY(BlueprintReadOnly)
	FString CharacterName;

	UPROPERTY(BlueprintReadOnly)
	int32 Race = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 ClassId = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 Gender = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 Skin = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 Face = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 HairStyle = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 HairColor = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 FacialStyle = 0;

	UPROPERTY(BlueprintReadOnly)
	FString EquipmentSummary;

	UPROPERTY(BlueprintReadOnly)
	FString AppearanceKey;

	UPROPERTY(BlueprintReadOnly)
	FString CharacterPreviewMeshPath;

	UPROPERTY(BlueprintReadOnly)
	FString CharacterPreviewMeshJsonPath;

	UPROPERTY(BlueprintReadOnly)
	FString CharacterPreviewMaterialPackagePath;

	UPROPERTY(BlueprintReadOnly)
	FString CharacterPreviewAnimationPath;

	UPROPERTY(BlueprintReadOnly)
	FString CharacterPreviewAnimationPackagePath;

	UPROPERTY(BlueprintReadOnly)
	FString CharacterPreviewAnimationStem;

	UPROPERTY(BlueprintReadOnly)
	FString CharacterPreviewSectionMetadataPath;

	UPROPERTY(BlueprintReadOnly)
	FString CharacterPreviewAttachmentDescriptorPath;

	UPROPERTY(BlueprintReadOnly)
	bool bHasCharacterPreviewAsset = false;

	UPROPERTY(BlueprintReadOnly)
	bool bUsesBaseRuntimeComposition = false;

	bool IsValid() const { return Guid > 0 && Race > 0; }
};

UCLASS()
class WOW_API UWoWGameInstance : public UGameInstance
{
	GENERATED_BODY()

public:
	void SetSelectedCharacterRuntimeAppearance(const FWoWSelectedCharacterRuntimeAppearance& InAppearance);
	const FWoWSelectedCharacterRuntimeAppearance& GetSelectedCharacterRuntimeAppearance() const { return SelectedCharacterRuntimeAppearance; }
	bool HasSelectedCharacterRuntimeAppearance() const { return SelectedCharacterRuntimeAppearance.IsValid(); }
	void ClearSelectedCharacterRuntimeAppearance();

private:
	UPROPERTY()
	FWoWSelectedCharacterRuntimeAppearance SelectedCharacterRuntimeAppearance;
};
