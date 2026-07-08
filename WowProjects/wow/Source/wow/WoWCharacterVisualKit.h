#pragma once

#include "CoreMinimal.h"
#include "WoWCharacterEquipment.h"

struct WOW_API FWoWCharacterVisualInput
{
	int32 Guid = 0;
	FString CharacterName;
	int32 ClassId = 0;
	int32 Race = 0;
	int32 Gender = 0;
	int32 Skin = 0;
	int32 Face = 0;
	int32 HairStyle = 0;
	int32 HairColor = 0;
	int32 FacialStyle = 0;
	FString EquipmentSummary;
};

struct WOW_API FWoWCharacterVisualBaseDefinition
{
	FString Id;
	FString SourceModelPath;
	FString GeneratedAssetStem;
	FString MeshJsonPath;
	FString MeshPath;
	FString StandAnimationPath;
	FString LookupAnimationPath;
	FString AnimationPackagePath;
	FString MaterialPackagePath;
	FString SectionMetadataPath;
};

struct WOW_API FWoWCharacterVisualCacheCandidate
{
	FString MeshPath;
	FString MeshJsonPath;
	FString AnimationPath;
	FString AnimationPackagePath;
	FString MaterialPackagePath;
	FString SectionMetadataPath;
	FString LookupAnimationPath;
};

struct WOW_API FWoWCharacterVisualAttachmentDefinition
{
	FString Name;
	FString ComponentType;
	int32 Slot = INDEX_NONE;
	int32 DisplayId = 0;
	int32 InventoryType = 0;
	FName SocketName;
	FString MeshPath;
	FString AnimationPath;
	FString MeshJsonPath;
	FVector RelativeLocation = FVector::ZeroVector;
	FRotator RelativeRotation = FRotator::ZeroRotator;
	FVector RelativeScale = FVector::OneVector;
};

struct WOW_API FWoWItemObjectComponentEntry
{
	FString Component;
	TSet<int32> DisplayIds;
	TSet<int32> InventoryTypes;
	TSet<int32> Slots;
	int32 Race = 0;
	int32 Gender = INDEX_NONE;
	FString ModelPath;
	FString MeshJsonPath;
	FString GeneratedPackageRoot;
	FString AnimationPackagePath;
};

struct WOW_API FWoWItemObjectComponentLibrary
{
	bool bLoaded = false;
	FString ManifestPath;
	TArray<FWoWItemObjectComponentEntry> Entries;
	TMultiMap<int32, int32> EntryIndicesByDisplayId;
};

struct WOW_API FWoWCharacterVisualKit
{
	FWoWCharacterVisualInput Input;
	FString CacheKey;
	FString AppearanceSignature;
	FString CharacterPreviewStem;
	FString CharacterPreviewPackageRoot;
	FString LegacyCharacterPreviewPackageRoot;
	FString MetadataRoot;
	FString AttachmentDescriptorPath;
	FString MeshJsonPath;
	FString MeshPath;
	FString AnimationPath;
	FString AnimationPackagePath;
	FString AnimationStem;
	FString MaterialPackagePath;
	FString SectionMetadataPath;
	FString LookupAnimationPath;
	bool bHasCharacterPreviewAsset = false;
	bool bUsesBaseRuntimeComposition = true;
	int32 EquipmentEntryCount = 0;
	TArray<FWoWCharacterEquipmentDisplay> Equipment;
	TArray<FWoWCharacterVisualCacheCandidate> CacheCandidates;
};

namespace WoWCharacterVisualKit
{
	WOW_API FString BuildAppearanceSignature(const FWoWCharacterVisualInput& Input);
	WOW_API FString BuildCacheKey(const FWoWCharacterVisualInput& Input);
	WOW_API FString BuildPreviewStemForSignature(const FString& SourceModelPath, const FString& GeneratedAssetStem, const FString& Signature);
	WOW_API FString BuildAnimationStem(const FString& SourceModelPath);
	WOW_API int32 GetEquippedWeaponGripSlot(const FString& EquipmentSummary);
	WOW_API FString BuildGripAnimationObjectPath(const FString& AnimationObjectPath, int32 WeaponSlot);
	WOW_API FString ResolveAnimationObjectPath(const FString& DefaultAnimationObjectPath, const FString& EquipmentSummary, TFunctionRef<bool(const FString&)> DoesAssetExist);
	WOW_API bool DoesEquipmentSummaryContainAttachment(const FString& EquipmentSummary, int32 Slot, int32 DisplayId);
	WOW_API bool ReadAttachmentDefinitionsFromDescriptor(const FString& DescriptorPath, TArray<FWoWCharacterVisualAttachmentDefinition>& OutAttachments);
	WOW_API FString BuildAttachmentStandAnimationPath(const FString& AttachmentMeshPath);
	WOW_API FString BuildAttachmentMeshJsonPath(const FString& AttachmentMeshPath, const FString& AttachmentDescriptorPath);
	WOW_API FString BuildAttachmentMaterialPackagePath(const FString& AttachmentMeshPath);
	WOW_API bool ReadItemObjectComponentLibrary(const FString& ManifestPath, FWoWItemObjectComponentLibrary& OutLibrary);
	WOW_API FString BuildItemObjectComponentMeshObjectPath(const FWoWItemObjectComponentEntry& Entry);
	WOW_API FString BuildItemObjectComponentStandAnimationObjectPath(const FWoWItemObjectComponentEntry& Entry);
	WOW_API FName ResolveSharedItemAttachmentSocketName(const FString& Component, int32 Slot, int32 SequenceIndex);
	WOW_API bool DoesItemObjectComponentEntryMatchEquipment(
		const FWoWItemObjectComponentEntry& Entry,
		const FWoWCharacterEquipmentDisplay& Equipment,
		const FWoWCharacterVisualInput& Input);
	WOW_API bool BuildSharedItemObjectAttachmentDefinitions(
		const FWoWCharacterVisualInput& Input,
		const FWoWItemObjectComponentLibrary& Library,
		TArray<FWoWCharacterVisualAttachmentDefinition>& OutAttachments);
	WOW_API FString BuildPreviewRequestKey(
		int32 Guid,
		const FString& CacheKey,
		const FString& MeshPath,
		const FString& AnimationPath,
		const FString& SectionMetadataPath,
		const FString& AttachmentDescriptorPath);
	WOW_API void CollectCoreAsyncAssetObjectPaths(
		const FString& MeshPath,
		const FString& AnimationPath,
		const FString& MaterialPackagePath,
		int32 MaterialSlotProbeCount,
		TFunctionRef<bool(const FString&)> DoesAssetExist,
		TArray<FString>& OutObjectPaths);
	WOW_API void CollectAttachmentAsyncAssetObjectPaths(
		const FString& EquipmentSummary,
		const FString& AttachmentDescriptorPath,
		const TArray<FWoWCharacterVisualAttachmentDefinition>& AttachmentDefinitions,
		int32 MaterialSlotProbeCount,
		TFunctionRef<bool(const FString&)> DoesAssetExist,
		TArray<FString>& OutObjectPaths);
	WOW_API FWoWCharacterVisualKit Build(
		const FWoWCharacterVisualInput& Input,
		const FWoWCharacterVisualBaseDefinition& Definition,
		const FString& ProjectSavedDir);
}
