#pragma once

#include "CoreMinimal.h"

class USkeletalMeshComponent;
class UTexture2D;

struct WOW_API FWoWRuntimeCharacterAppearanceDescriptor
{
	int32 Guid = 0;
	int32 Race = 0;
	int32 Gender = 0;
	int32 Skin = 0;
	int32 Face = 0;
	int32 HairStyle = 0;
	int32 HairColor = 0;
	int32 FacialStyle = 0;
	FString EquipmentSummary;
};

struct WOW_API FWoWRuntimeCharacterMaterialApplyResult
{
	int32 BodySlots = 0;
	int32 FaceSlots = 0;
	int32 HairSlots = 0;
	int32 FurSlots = 0;
	int32 AppliedItemLayers = 0;
	int32 MissingItemLayers = 0;
	int32 MissingSlots = 0;
};

namespace WoWRuntimeCharacterAppearance
{
	WOW_API bool WarmupRuntimeDbc();
	WOW_API bool ResolveItemDisplayObjectSkinPng(int32 DisplayId, const FString& AttachmentMeshPath, FString& OutPreviewPng);
	WOW_API bool ResolveRuntimeSectionTexturePng(
		const FWoWRuntimeCharacterAppearanceDescriptor& Appearance,
		int32 TextureType,
		int32 SubmeshId,
		const FString& ExportPreviewPng,
		FString& OutPreviewPng);
	WOW_API bool ShouldUseComposedBodyAtlasForSection(int32 TextureType, int32 SubmeshId, const FString& ExportPreviewPng);
	WOW_API FString BuildBodyAtlasSignature(const FWoWRuntimeCharacterAppearanceDescriptor& Appearance);
	WOW_API UTexture2D* ComposeBodyAtlas(const FWoWRuntimeCharacterAppearanceDescriptor& Appearance, int32& OutAppliedLayers, int32& OutMissingLayers);
	WOW_API UTexture2D* PrewarmBodyAtlas(const FWoWRuntimeCharacterAppearanceDescriptor& Appearance);
	WOW_API UTexture2D* FindCachedBodyAtlas(const FWoWRuntimeCharacterAppearanceDescriptor& Appearance);
	WOW_API bool HasTextureSupport(const FWoWRuntimeCharacterAppearanceDescriptor& Appearance);
	WOW_API FWoWRuntimeCharacterMaterialApplyResult ApplyRuntimeBodyTextures(
		USkeletalMeshComponent* SkeletalMeshComponent,
		const FWoWRuntimeCharacterAppearanceDescriptor& Appearance,
		const FString& SectionMetadataPath,
		bool bAllowComposeIfMissing = true);
}
