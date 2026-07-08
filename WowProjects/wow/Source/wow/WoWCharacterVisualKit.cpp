#include "WoWCharacterVisualKit.h"

#include "Dom/JsonObject.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"

namespace
{
FString SanitizeVisualKitPackageSegment(const FString& Value)
{
	FString Result = Value;
	Result.ReplaceInline(TEXT("\\"), TEXT("_"));
	Result.ReplaceInline(TEXT("/"), TEXT("_"));
	Result.ReplaceInline(TEXT(" "), TEXT("_"));
	Result.ReplaceInline(TEXT("."), TEXT("_"));
	return Result;
}

FString MakeObjectPathFromPackagePathLocal(const FString& PackagePath)
{
	if (PackagePath.IsEmpty())
	{
		return FString();
	}
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
	return PackagePath + TEXT(".") + AssetName;
}

uint64 UpdateFnv1a64Local(uint64 Hash, const FString& Value)
{
	FTCHARToUTF8 Utf8(*Value);
	const ANSICHAR* Bytes = Utf8.Get();
	for (int32 Index = 0; Index < Utf8.Length(); ++Index)
	{
		Hash ^= static_cast<uint8>(Bytes[Index]);
		Hash *= 0x00000100000001B3ull;
	}
	return Hash;
}

void AddCacheCandidate(
	FWoWCharacterVisualKit& Kit,
	const FString& PackageRoot,
	const FString& MeshSuffix,
	const FString& AnimationFolder)
{
	FWoWCharacterVisualCacheCandidate Candidate;
	Candidate.MeshPath = MakeObjectPathFromPackagePathLocal(PackageRoot / (Kit.CharacterPreviewStem + MeshSuffix));
	Candidate.MeshJsonPath = Kit.MetadataRoot / FString::Printf(TEXT("%s.mesh.json"), *Kit.CharacterPreviewStem);
	Candidate.AnimationPath = MakeObjectPathFromPackagePathLocal(PackageRoot / AnimationFolder / (TEXT("A_") + Kit.AnimationStem + TEXT("_000_Stand_00")));
	Candidate.AnimationPackagePath = PackageRoot / AnimationFolder;
	Candidate.MaterialPackagePath = PackageRoot / (Kit.CharacterPreviewStem + TEXT("_Materials"));
	Candidate.SectionMetadataPath = Kit.MetadataRoot / FString::Printf(TEXT("%s.sections.json"), *Kit.CharacterPreviewStem);
	Candidate.LookupAnimationPath = MakeObjectPathFromPackagePathLocal(PackageRoot / TEXT("Animations_Lookup") / (TEXT("A_") + Kit.AnimationStem + TEXT("_000_Stand_00")));
	Kit.CacheCandidates.Add(MoveTemp(Candidate));
}

TSharedPtr<FJsonObject> TryReadJsonObjectLocal(const TSharedPtr<FJsonValue>& Value)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object || !Object->IsValid())
	{
		return nullptr;
	}
	return *Object;
}

bool ReadOptionalJsonVector3Local(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FVector& OutValue)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values || Values->Num() < 3)
	{
		return false;
	}

	OutValue = FVector(
		static_cast<float>((*Values)[0]->AsNumber()),
		static_cast<float>((*Values)[1]->AsNumber()),
		static_cast<float>((*Values)[2]->AsNumber()));
	return true;
}

bool ReadJsonIntSetFieldLocal(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TSet<int32>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		if (Value.IsValid())
		{
			OutValues.Add(FMath::RoundToInt(Value->AsNumber()));
		}
	}
	return true;
}

FString NormalizeVisualKitModelKey(FString Value)
{
	Value.ReplaceInline(TEXT("\\"), TEXT("/"));
	Value.ToLowerInline();
	while (Value.Contains(TEXT("//")))
	{
		Value.ReplaceInline(TEXT("//"), TEXT("/"));
	}
	return Value;
}

const TMap<int64, TSet<int32>>& GetEquivalentPlayerCharacterRaces()
{
	static TMap<int64, TSet<int32>> EquivalentRacesByRaceGender;
	static bool bAttemptedLoad = false;
	if (bAttemptedLoad)
	{
		return EquivalentRacesByRaceGender;
	}
	bAttemptedLoad = true;

	const FString ManifestPath = FPaths::ProjectSavedDir() / TEXT("WoWData/PlayerCharacters.lookup.manifest.json");
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *ManifestPath))
	{
		return EquivalentRacesByRaceGender;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	const TArray<TSharedPtr<FJsonValue>>* EntryValues = nullptr;
	if (!FJsonSerializer::Deserialize(Reader, RootObject) ||
		!RootObject.IsValid() ||
		!RootObject->TryGetArrayField(TEXT("entries"), EntryValues) ||
		!EntryValues)
	{
		return EquivalentRacesByRaceGender;
	}

	TMap<FString, TArray<TPair<int32, int32>>> RacesByModelGender;
	for (const TSharedPtr<FJsonValue>& EntryValue : *EntryValues)
	{
		const TSharedPtr<FJsonObject> EntryObject = TryReadJsonObjectLocal(EntryValue);
		if (!EntryObject.IsValid())
		{
			continue;
		}

		double RaceNumber = 0.0;
		double GenderNumber = -1.0;
		FString ModelPath;
		if (!EntryObject->TryGetNumberField(TEXT("race_id"), RaceNumber) ||
			!EntryObject->TryGetNumberField(TEXT("gender"), GenderNumber) ||
			!EntryObject->TryGetStringField(TEXT("model_path"), ModelPath) ||
			ModelPath.IsEmpty())
		{
			continue;
		}

		const int32 Race = FMath::RoundToInt(RaceNumber);
		const int32 Gender = FMath::RoundToInt(GenderNumber);
		if (Race <= 0 || Gender == INDEX_NONE)
		{
			continue;
		}

		RacesByModelGender.FindOrAdd(FString::Printf(TEXT("%s|%d"), *NormalizeVisualKitModelKey(ModelPath), Gender))
			.Add(TPair<int32, int32>(Race, Gender));
	}

	for (const TPair<FString, TArray<TPair<int32, int32>>>& Group : RacesByModelGender)
	{
		TSet<int32> GroupRaces;
		for (const TPair<int32, int32>& RaceGender : Group.Value)
		{
			GroupRaces.Add(RaceGender.Key);
		}
		if (GroupRaces.Num() <= 1)
		{
			continue;
		}
		for (const TPair<int32, int32>& RaceGender : Group.Value)
		{
			const int64 Key = (static_cast<int64>(RaceGender.Key) << 32) | static_cast<uint32>(RaceGender.Value);
			EquivalentRacesByRaceGender.FindOrAdd(Key).Append(GroupRaces);
		}
	}

	return EquivalentRacesByRaceGender;
}

bool ArePlayerCharacterRacesEquivalentForGender(int32 LeftRace, int32 RightRace, int32 Gender)
{
	if (LeftRace == RightRace)
	{
		return true;
	}
	const int64 Key = (static_cast<int64>(LeftRace) << 32) | static_cast<uint32>(Gender);
	if (const TSet<int32>* EquivalentRaces = GetEquivalentPlayerCharacterRaces().Find(Key))
	{
		return EquivalentRaces->Contains(RightRace);
	}
	return false;
}
}

namespace WoWCharacterVisualKit
{
FString BuildAppearanceSignature(const FWoWCharacterVisualInput& Input)
{
	const FString Key = FString::Printf(
		TEXT("class=%d|race=%d|gender=%d|skin=%d|face=%d|hairStyle=%d|hairColor=%d|facial=%d|equipment=%s"),
		Input.ClassId,
		Input.Race,
		Input.Gender,
		Input.Skin,
		Input.Face,
		Input.HairStyle,
		Input.HairColor,
		Input.FacialStyle,
		*Input.EquipmentSummary);
	const uint64 Hash = UpdateFnv1a64Local(0xCBF29CE484222325ull, Key);
	return FString::Printf(TEXT("%016llx"), static_cast<unsigned long long>(Hash));
}

FString BuildCacheKey(const FWoWCharacterVisualInput& Input)
{
	return FString::Printf(
		TEXT("guid=%d|class=%d|race=%d|gender=%d|skin=%d|face=%d|hairStyle=%d|hairColor=%d|facial=%d|equipment=%s"),
		Input.Guid,
		Input.ClassId,
		Input.Race,
		Input.Gender,
		Input.Skin,
		Input.Face,
		Input.HairStyle,
		Input.HairColor,
		Input.FacialStyle,
		*Input.EquipmentSummary);
}

FString BuildPreviewStemForSignature(const FString& SourceModelPath, const FString& GeneratedAssetStem, const FString& Signature)
{
	const FString Base = !GeneratedAssetStem.IsEmpty()
		? GeneratedAssetStem
		: SanitizeVisualKitPackageSegment(FPaths::GetBaseFilename(SourceModelPath));
	return FString::Printf(TEXT("%s_%s"), *SanitizeVisualKitPackageSegment(Base), *Signature);
}

FString BuildAnimationStem(const FString& SourceModelPath)
{
	FString ModelStem = SanitizeVisualKitPackageSegment(FPaths::GetBaseFilename(SourceModelPath));
	ModelStem.ToLowerInline();
	return ModelStem;
}

int32 GetEquippedWeaponGripSlot(const FString& EquipmentSummary)
{
	for (const FWoWCharacterEquipmentDisplay& Entry : WoWCharacterEquipment::ParseEquipmentSummary(EquipmentSummary))
	{
		if (Entry.DisplayId <= 0)
		{
			continue;
		}
		if (Entry.Slot == 15)
		{
			return 15;
		}
		if (Entry.Slot == 16)
		{
			return 16;
		}
	}
	return INDEX_NONE;
}

FString BuildGripAnimationObjectPath(const FString& AnimationObjectPath, int32 WeaponSlot)
{
	if (WeaponSlot != 15 && WeaponSlot != 16)
	{
		return FString();
	}

	const int32 DotIndex = AnimationObjectPath.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (DotIndex == INDEX_NONE)
	{
		return FString();
	}

	const FString PackagePath = AnimationObjectPath.Left(DotIndex);
	const FString AssetName = AnimationObjectPath.Mid(DotIndex + 1);
	const FString Suffix = WeaponSlot == 16 ? TEXT("_GripL") : TEXT("_GripR");
	return PackagePath + Suffix + TEXT(".") + AssetName + Suffix;
}

FString ResolveAnimationObjectPath(const FString& DefaultAnimationObjectPath, const FString& EquipmentSummary, TFunctionRef<bool(const FString&)> DoesAssetExist)
{
	const int32 WeaponSlot = GetEquippedWeaponGripSlot(EquipmentSummary);
	const FString GripAnimationObjectPath = BuildGripAnimationObjectPath(DefaultAnimationObjectPath, WeaponSlot);
	if (!GripAnimationObjectPath.IsEmpty() && DoesAssetExist(GripAnimationObjectPath))
	{
		return GripAnimationObjectPath;
	}
	return DefaultAnimationObjectPath;
}

bool DoesEquipmentSummaryContainAttachment(const FString& EquipmentSummary, int32 Slot, int32 DisplayId)
{
	for (const FWoWCharacterEquipmentDisplay& Entry : WoWCharacterEquipment::ParseEquipmentSummary(EquipmentSummary))
	{
		if ((Slot == INDEX_NONE || Entry.Slot == Slot) &&
			(DisplayId <= 0 || Entry.DisplayId == DisplayId))
		{
			return true;
		}
	}
	return false;
}

bool ReadAttachmentDefinitionsFromDescriptor(const FString& DescriptorPath, TArray<FWoWCharacterVisualAttachmentDefinition>& OutAttachments)
{
	OutAttachments.Reset();

	static TMap<FString, TArray<FWoWCharacterVisualAttachmentDefinition>> AttachmentDefinitionCache;
	if (const TArray<FWoWCharacterVisualAttachmentDefinition>* CachedAttachments = AttachmentDefinitionCache.Find(DescriptorPath))
	{
		OutAttachments = *CachedAttachments;
		return true;
	}

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *DescriptorPath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* AttachmentValues = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("attachments"), AttachmentValues) || !AttachmentValues)
	{
		return false;
	}

	OutAttachments.Reserve(AttachmentValues->Num());
	for (const TSharedPtr<FJsonValue>& AttachmentValue : *AttachmentValues)
	{
		const TSharedPtr<FJsonObject> AttachmentObject = TryReadJsonObjectLocal(AttachmentValue);
		if (!AttachmentObject.IsValid())
		{
			continue;
		}

		FWoWCharacterVisualAttachmentDefinition Definition;
		AttachmentObject->TryGetStringField(TEXT("name"), Definition.Name);
		AttachmentObject->TryGetStringField(TEXT("component"), Definition.ComponentType);
		double Number = 0.0;
		if (AttachmentObject->TryGetNumberField(TEXT("slot"), Number))
		{
			Definition.Slot = FMath::RoundToInt(Number);
		}
		if (AttachmentObject->TryGetNumberField(TEXT("display_id"), Number))
		{
			Definition.DisplayId = FMath::RoundToInt(Number);
		}
		if (AttachmentObject->TryGetNumberField(TEXT("inventory_type"), Number))
		{
			Definition.InventoryType = FMath::RoundToInt(Number);
		}

		FString SocketName;
		if (AttachmentObject->TryGetStringField(TEXT("socket"), SocketName))
		{
			Definition.SocketName = FName(*SocketName);
		}
		AttachmentObject->TryGetStringField(TEXT("mesh"), Definition.MeshPath);
		AttachmentObject->TryGetStringField(TEXT("animation"), Definition.AnimationPath);
		AttachmentObject->TryGetStringField(TEXT("mesh_json"), Definition.MeshJsonPath);

		FVector RotationVector = FVector::ZeroVector;
		ReadOptionalJsonVector3Local(AttachmentObject, TEXT("relative_location"), Definition.RelativeLocation);
		if (ReadOptionalJsonVector3Local(AttachmentObject, TEXT("relative_rotation"), RotationVector))
		{
			Definition.RelativeRotation = FRotator(RotationVector.X, RotationVector.Y, RotationVector.Z);
		}
		ReadOptionalJsonVector3Local(AttachmentObject, TEXT("relative_scale"), Definition.RelativeScale);

		if (!Definition.SocketName.IsNone() && !Definition.MeshPath.IsEmpty())
		{
			OutAttachments.Add(MoveTemp(Definition));
		}
	}

	AttachmentDefinitionCache.Add(DescriptorPath, OutAttachments);
	return true;
}

FString BuildAttachmentStandAnimationPath(const FString& AttachmentMeshPath)
{
	const FString MeshPackageName = FPackageName::ObjectPathToPackageName(AttachmentMeshPath);
	FString MeshAssetName = FPackageName::ObjectPathToObjectName(AttachmentMeshPath);
	if (MeshPackageName.IsEmpty() || MeshAssetName.IsEmpty())
	{
		return FString();
	}

	if (!MeshAssetName.RemoveFromEnd(TEXT("_M2Mesh"), ESearchCase::CaseSensitive))
	{
		return FString();
	}

	FString ParentPackagePath = MeshPackageName;
	const int32 LastSlashIndex = ParentPackagePath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (LastSlashIndex != INDEX_NONE)
	{
		ParentPackagePath.LeftInline(LastSlashIndex, EAllowShrinking::No);
	}

	const FString AnimationAssetName = TEXT("A_") + MeshAssetName + TEXT("_000_Stand_00");
	const FString AnimationPackagePath = ParentPackagePath / MeshAssetName / TEXT("Animations") / AnimationAssetName;
	return MakeObjectPathFromPackagePathLocal(AnimationPackagePath);
}

FString BuildAttachmentMeshJsonPath(const FString& AttachmentMeshPath, const FString& AttachmentDescriptorPath)
{
	FString AssetName = FPackageName::ObjectPathToObjectName(AttachmentMeshPath);
	if (AssetName.IsEmpty())
	{
		AttachmentMeshPath.Split(TEXT("."), nullptr, &AssetName, ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	}
	if (AssetName.EndsWith(TEXT("_M2Mesh"), ESearchCase::IgnoreCase))
	{
		AssetName.LeftChopInline(7);
	}
	if (AssetName.IsEmpty())
	{
		return FString();
	}

	return FPaths::GetPath(AttachmentDescriptorPath) / TEXT("Attachments") / (AssetName + TEXT(".mesh.json"));
}

FString BuildAttachmentMaterialPackagePath(const FString& AttachmentMeshPath)
{
	FString MeshPackageName = FPackageName::ObjectPathToPackageName(AttachmentMeshPath);
	FString MeshAssetName = FPackageName::ObjectPathToObjectName(AttachmentMeshPath);
	if (MeshPackageName.IsEmpty() || MeshAssetName.IsEmpty())
	{
		return FString();
	}

	if (!MeshAssetName.RemoveFromEnd(TEXT("_M2Mesh"), ESearchCase::CaseSensitive))
	{
		return FString();
	}

	FString ParentPackagePath = MeshPackageName;
	const int32 LastSlashIndex = ParentPackagePath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
	if (LastSlashIndex != INDEX_NONE)
	{
		ParentPackagePath.LeftInline(LastSlashIndex, EAllowShrinking::No);
	}

	return ParentPackagePath / (MeshAssetName + TEXT("_Materials"));
}

bool ReadItemObjectComponentLibrary(const FString& ManifestPath, FWoWItemObjectComponentLibrary& OutLibrary)
{
	OutLibrary = FWoWItemObjectComponentLibrary();
	OutLibrary.ManifestPath = ManifestPath;

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *ManifestPath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* EntryValues = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("entries"), EntryValues) || !EntryValues)
	{
		return false;
	}

	OutLibrary.Entries.Reserve(EntryValues->Num());
	for (const TSharedPtr<FJsonValue>& EntryValue : *EntryValues)
	{
		const TSharedPtr<FJsonObject> EntryObject = TryReadJsonObjectLocal(EntryValue);
		if (!EntryObject.IsValid())
		{
			continue;
		}

		FWoWItemObjectComponentEntry Entry;
		double Number = 0.0;
		EntryObject->TryGetStringField(TEXT("component"), Entry.Component);
		EntryObject->TryGetStringField(TEXT("model_path"), Entry.ModelPath);
		EntryObject->TryGetStringField(TEXT("mesh_json_path"), Entry.MeshJsonPath);
		EntryObject->TryGetStringField(TEXT("generated_package_root"), Entry.GeneratedPackageRoot);
		EntryObject->TryGetStringField(TEXT("animation_package_path"), Entry.AnimationPackagePath);
		if (EntryObject->TryGetNumberField(TEXT("race"), Number))
		{
			Entry.Race = FMath::RoundToInt(Number);
		}
		if (EntryObject->TryGetNumberField(TEXT("gender"), Number))
		{
			Entry.Gender = FMath::RoundToInt(Number);
		}
		ReadJsonIntSetFieldLocal(EntryObject, TEXT("display_ids"), Entry.DisplayIds);
		ReadJsonIntSetFieldLocal(EntryObject, TEXT("inventory_types"), Entry.InventoryTypes);
		ReadJsonIntSetFieldLocal(EntryObject, TEXT("slots"), Entry.Slots);

		if (Entry.Component.IsEmpty() || Entry.ModelPath.IsEmpty() || Entry.GeneratedPackageRoot.IsEmpty() || Entry.DisplayIds.IsEmpty())
		{
			continue;
		}

		const int32 EntryIndex = OutLibrary.Entries.Add(MoveTemp(Entry));
		for (const int32 DisplayId : OutLibrary.Entries[EntryIndex].DisplayIds)
		{
			OutLibrary.EntryIndicesByDisplayId.Add(DisplayId, EntryIndex);
		}
	}

	OutLibrary.bLoaded = true;
	return true;
}

FString BuildItemObjectComponentMeshObjectPath(const FWoWItemObjectComponentEntry& Entry)
{
	FString PackageRoot = Entry.GeneratedPackageRoot;
	PackageRoot.RemoveFromEnd(TEXT("/"));
	const FString AssetStem = SanitizeVisualKitPackageSegment(FPaths::GetBaseFilename(Entry.ModelPath));
	return AssetStem.IsEmpty() ? FString() : MakeObjectPathFromPackagePathLocal(PackageRoot / (AssetStem + TEXT("_M2Mesh")));
}

FString BuildItemObjectComponentStandAnimationObjectPath(const FWoWItemObjectComponentEntry& Entry)
{
	FString AnimationPackagePath = Entry.AnimationPackagePath;
	AnimationPackagePath.RemoveFromEnd(TEXT("/"));
	const FString AssetStem = SanitizeVisualKitPackageSegment(FPaths::GetBaseFilename(Entry.ModelPath));
	return (AssetStem.IsEmpty() || AnimationPackagePath.IsEmpty())
		? FString()
		: MakeObjectPathFromPackagePathLocal(AnimationPackagePath / (TEXT("A_") + AssetStem + TEXT("_000_Stand_00")));
}

FName ResolveSharedItemAttachmentSocketName(const FString& Component, int32 Slot, int32 SequenceIndex)
{
	if (Component.Equals(TEXT("Head"), ESearchCase::IgnoreCase))
	{
		return FName(TEXT("ATT_011_ID_011"));
	}
	if (Component.Equals(TEXT("Shoulder"), ESearchCase::IgnoreCase))
	{
		return FName(SequenceIndex == 0 ? TEXT("ATT_006_ID_006") : TEXT("ATT_005_ID_005"));
	}
	if (Component.Equals(TEXT("Weapon"), ESearchCase::IgnoreCase))
	{
		return FName(Slot == 16 ? TEXT("ATT_002_ID_002") : TEXT("ATT_001_ID_001"));
	}
	return NAME_None;
}

bool DoesItemObjectComponentEntryMatchEquipment(
	const FWoWItemObjectComponentEntry& Entry,
	const FWoWCharacterEquipmentDisplay& Equipment,
	const FWoWCharacterVisualInput& Input)
{
	if (!Entry.DisplayIds.Contains(Equipment.DisplayId))
	{
		return false;
	}
	if (!Entry.Slots.IsEmpty() && !Entry.Slots.Contains(Equipment.Slot))
	{
		return false;
	}
	if (!Entry.Component.Equals(TEXT("Weapon"), ESearchCase::IgnoreCase) &&
		!Entry.InventoryTypes.IsEmpty() &&
		Equipment.InventoryType > 0 &&
		!Entry.InventoryTypes.Contains(Equipment.InventoryType))
	{
		return false;
	}
	if (Entry.Component.Equals(TEXT("Head"), ESearchCase::IgnoreCase))
	{
		if (Entry.Race <= 0 || Entry.Gender == INDEX_NONE)
		{
			return false;
		}
		if (Entry.Gender != Input.Gender ||
			!ArePlayerCharacterRacesEquivalentForGender(Input.Race, Entry.Race, Input.Gender))
		{
			return false;
		}
	}
	return true;
}

bool BuildSharedItemObjectAttachmentDefinitions(
	const FWoWCharacterVisualInput& Input,
	const FWoWItemObjectComponentLibrary& Library,
	TArray<FWoWCharacterVisualAttachmentDefinition>& OutAttachments)
{
	OutAttachments.Reset();
	if (!Library.bLoaded)
	{
		return false;
	}

	for (const FWoWCharacterEquipmentDisplay& EquippedItem : WoWCharacterEquipment::ParseEquipmentSummary(Input.EquipmentSummary))
	{
		TArray<int32> CandidateIndices;
		Library.EntryIndicesByDisplayId.MultiFind(EquippedItem.DisplayId, CandidateIndices);
		if (CandidateIndices.IsEmpty())
		{
			continue;
		}

		CandidateIndices.Sort([&Library](int32 Left, int32 Right)
		{
			const FWoWItemObjectComponentEntry& LeftEntry = Library.Entries[Left];
			const FWoWItemObjectComponentEntry& RightEntry = Library.Entries[Right];
			return LeftEntry.ModelPath < RightEntry.ModelPath;
		});

		int32 AttachmentSequence = 0;
		for (const int32 CandidateIndex : CandidateIndices)
		{
			if (!Library.Entries.IsValidIndex(CandidateIndex))
			{
				continue;
			}

			const FWoWItemObjectComponentEntry& Entry = Library.Entries[CandidateIndex];
			if (!DoesItemObjectComponentEntryMatchEquipment(Entry, EquippedItem, Input))
			{
				continue;
			}

			FWoWCharacterVisualAttachmentDefinition Definition;
			Definition.ComponentType = Entry.Component;
			Definition.Slot = EquippedItem.Slot;
			Definition.DisplayId = EquippedItem.DisplayId;
			Definition.InventoryType = EquippedItem.InventoryType;
			Definition.SocketName = ResolveSharedItemAttachmentSocketName(Entry.Component, EquippedItem.Slot, AttachmentSequence);
			Definition.MeshPath = BuildItemObjectComponentMeshObjectPath(Entry);
			Definition.AnimationPath = BuildItemObjectComponentStandAnimationObjectPath(Entry);
			Definition.MeshJsonPath = Entry.MeshJsonPath;

			if (Definition.ComponentType.Equals(TEXT("Head"), ESearchCase::IgnoreCase))
			{
				Definition.Name = TEXT("helm");
			}
			else if (Definition.ComponentType.Equals(TEXT("Shoulder"), ESearchCase::IgnoreCase))
			{
				Definition.Name = AttachmentSequence == 0 ? TEXT("left_shoulder") : TEXT("right_shoulder");
			}
			else if (Definition.ComponentType.Equals(TEXT("Weapon"), ESearchCase::IgnoreCase))
			{
				Definition.Name = EquippedItem.Slot == 16 ? TEXT("off_hand") : TEXT("main_hand");
			}
			else
			{
				Definition.Name = Definition.ComponentType;
			}

			if (!Definition.SocketName.IsNone() && !Definition.MeshPath.IsEmpty())
			{
				OutAttachments.Add(MoveTemp(Definition));
				++AttachmentSequence;
			}
		}
	}

	return true;
}

FString BuildPreviewRequestKey(
	int32 Guid,
	const FString& CacheKey,
	const FString& MeshPath,
	const FString& AnimationPath,
	const FString& SectionMetadataPath,
	const FString& AttachmentDescriptorPath)
{
	return FString::Printf(
		TEXT("guid=%d|appearance=%s|mesh=%s|anim=%s|sections=%s|attachments=%s"),
		Guid,
		*CacheKey,
		*MeshPath,
		*AnimationPath,
		*SectionMetadataPath,
		*AttachmentDescriptorPath);
}

void CollectCoreAsyncAssetObjectPaths(
	const FString& MeshPath,
	const FString& AnimationPath,
	const FString& MaterialPackagePath,
	int32 MaterialSlotProbeCount,
	TFunctionRef<bool(const FString&)> DoesAssetExist,
	TArray<FString>& OutObjectPaths)
{
	auto AddPath = [&OutObjectPaths](const FString& ObjectPath)
	{
		const FString TrimmedObjectPath = ObjectPath.TrimStartAndEnd();
		if (!TrimmedObjectPath.IsEmpty())
		{
			OutObjectPaths.AddUnique(TrimmedObjectPath);
		}
	};

	AddPath(MeshPath);
	AddPath(AnimationPath);
	for (int32 SectionIndex = 0; SectionIndex < MaterialSlotProbeCount; ++SectionIndex)
	{
		if (MaterialPackagePath.IsEmpty())
		{
			break;
		}
		const FString MaterialPath = FString::Printf(
			TEXT("%s/MI_M2_Section_%03d.MI_M2_Section_%03d"),
			*MaterialPackagePath,
			SectionIndex,
			SectionIndex);
		if (DoesAssetExist(MaterialPath))
		{
			AddPath(MaterialPath);
		}
	}
}

void CollectAttachmentAsyncAssetObjectPaths(
	const FString& EquipmentSummary,
	const FString& AttachmentDescriptorPath,
	const TArray<FWoWCharacterVisualAttachmentDefinition>& AttachmentDefinitions,
	int32 MaterialSlotProbeCount,
	TFunctionRef<bool(const FString&)> DoesAssetExist,
	TArray<FString>& OutObjectPaths)
{
	auto AddPath = [&OutObjectPaths](const FString& ObjectPath)
	{
		const FString TrimmedObjectPath = ObjectPath.TrimStartAndEnd();
		if (!TrimmedObjectPath.IsEmpty())
		{
			OutObjectPaths.AddUnique(TrimmedObjectPath);
		}
	};

	for (const FWoWCharacterVisualAttachmentDefinition& AttachmentDefinition : AttachmentDefinitions)
	{
		if (!DoesEquipmentSummaryContainAttachment(EquipmentSummary, AttachmentDefinition.Slot, AttachmentDefinition.DisplayId))
		{
			continue;
		}
		if (AttachmentDefinition.MeshPath.IsEmpty() || !DoesAssetExist(AttachmentDefinition.MeshPath))
		{
			continue;
		}

		AddPath(AttachmentDefinition.MeshPath);
		const FString AttachmentMaterialPackagePath = BuildAttachmentMaterialPackagePath(AttachmentDefinition.MeshPath);
		for (int32 SectionIndex = 0; SectionIndex < MaterialSlotProbeCount; ++SectionIndex)
		{
			if (AttachmentMaterialPackagePath.IsEmpty())
			{
				break;
			}
			const FString MaterialPath = FString::Printf(
				TEXT("%s/MI_M2_Section_%03d.MI_M2_Section_%03d"),
				*AttachmentMaterialPackagePath,
				SectionIndex,
				SectionIndex);
			if (DoesAssetExist(MaterialPath))
			{
				AddPath(MaterialPath);
			}
		}

		const FString AttachmentAnimationPath = AttachmentDefinition.AnimationPath.IsEmpty()
			? BuildAttachmentStandAnimationPath(AttachmentDefinition.MeshPath)
			: AttachmentDefinition.AnimationPath;
		if (!AttachmentAnimationPath.IsEmpty())
		{
			AddPath(AttachmentAnimationPath);
		}
	}
}

FWoWCharacterVisualKit Build(
	const FWoWCharacterVisualInput& Input,
	const FWoWCharacterVisualBaseDefinition& Definition,
	const FString& ProjectSavedDir)
{
	FWoWCharacterVisualKit Kit;
	Kit.Input = Input;
	Kit.CacheKey = BuildCacheKey(Input);
	Kit.AppearanceSignature = BuildAppearanceSignature(Input);
	Kit.CharacterPreviewStem = BuildPreviewStemForSignature(Definition.SourceModelPath, Definition.GeneratedAssetStem, Kit.AppearanceSignature);
	Kit.AnimationStem = BuildAnimationStem(Definition.SourceModelPath);
	Kit.CharacterPreviewPackageRoot = FString::Printf(TEXT("/Game/WoW/Generated/Cache/CharacterPreview/Appearance/%s"), *Kit.AppearanceSignature);
	Kit.LegacyCharacterPreviewPackageRoot = FString::Printf(TEXT("/Game/WoW/Generated/CharacterPreview/Appearance/%s"), *Kit.AppearanceSignature);
	Kit.MetadataRoot = FPaths::ConvertRelativePathToFull(ProjectSavedDir / TEXT("M2Preview/CharacterPreview/Appearance") / Kit.AppearanceSignature);
	Kit.AttachmentDescriptorPath = Kit.MetadataRoot / FString::Printf(TEXT("%s.attachments.json"), *Kit.CharacterPreviewStem);
	Kit.MeshJsonPath = Definition.MeshJsonPath;
	Kit.MeshPath = Definition.MeshPath;
	Kit.AnimationPath = Definition.StandAnimationPath;
	Kit.AnimationPackagePath = Definition.AnimationPackagePath;
	Kit.MaterialPackagePath = Definition.MaterialPackagePath;
	Kit.SectionMetadataPath = Definition.SectionMetadataPath;
	Kit.LookupAnimationPath = Definition.LookupAnimationPath;
	Kit.bHasCharacterPreviewAsset = false;
	Kit.bUsesBaseRuntimeComposition = true;
	Kit.Equipment = WoWCharacterEquipment::ParseEquipmentSummary(Input.EquipmentSummary);
	Kit.EquipmentEntryCount = Kit.Equipment.Num();

	const TArray<TPair<FString, FString>> CacheVariants = {
		{TEXT("_EquipFixed6_M2Mesh"), TEXT("Animations_EquipFixed6")},
		{TEXT("_EquipFixed5_M2Mesh"), TEXT("Animations_EquipFixed5")},
		{TEXT("_EquipFixed4_M2Mesh"), TEXT("Animations_EquipFixed4")},
		{TEXT("_EquipFixed3_M2Mesh"), TEXT("Animations_EquipFixed3")},
		{TEXT("_EquipFixed2_M2Mesh"), TEXT("Animations_EquipFixed2")},
		{TEXT("_EquipFixed_M2Mesh"), TEXT("Animations_EquipFixed")},
		{TEXT("_SkinFixed_M2Mesh"), TEXT("Animations_SkinFixed")},
		{TEXT("_AllGeosets_M2Mesh"), TEXT("Animations_AllGeosets")},
		{TEXT("_M2Mesh"), TEXT("Animations")}
	};
	for (const TPair<FString, FString>& Variant : CacheVariants)
	{
		AddCacheCandidate(Kit, Kit.CharacterPreviewPackageRoot, Variant.Key, Variant.Value);
	}
	for (const TPair<FString, FString>& Variant : CacheVariants)
	{
		AddCacheCandidate(Kit, Kit.LegacyCharacterPreviewPackageRoot, Variant.Key, Variant.Value);
	}

	return Kit;
}
}
