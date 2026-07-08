#include "WoWLoginGameMode.h"

#include "WoWAzerothCoreService.h"
#include "WoWCharacterEquipment.h"
#include "WoWCharacterVisualKit.h"
#include "WoWGameInstance.h"
#include "WoWGlueLuaRuntime.h"
#include "WoWGlueUI.h"
#include "WoWLoginScenePresetAsset.h"
#include "WoWPlayerController.h"
#include "WoWM2ModelComponent.h"
#include "WoWLoginSceneDebugPanel.h"
#include "WoWRuntimeCharacterAppearance.h"
#include "Animation/AnimSingleNodeInstance.h"
#include "Animation/Skeleton.h"
#include "Async/Async.h"
#include "Camera/CameraActor.h"
#include "Camera/CameraComponent.h"
#include "Components/BrushComponent.h"
#include "Components/LightComponent.h"
#include "Components/PointLightComponent.h"
#include "ProceduralMeshComponent.h"
#include "Components/SkyLightComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/AssetManager.h"
#include "Engine/Brush.h"
#include "Engine/Engine.h"
#include "Engine/GameViewportClient.h"
#include "Engine/LevelScriptActor.h"
#include "Engine/PostProcessVolume.h"
#include "Engine/SkyLight.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StreamableManager.h"
#include "Animation/SkeletalMeshActor.h"
#include "Engine/StaticMeshActor.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/DefaultPawn.h"
#include "GameFramework/PlayerStart.h"
#include "HAL/FileManager.h"
#include "HAL/IConsoleManager.h"
#include "HAL/PlatformProcess.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Json.h"
#include "Kismet/GameplayStatics.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Misc/Crc.h"
#include "Misc/ConfigCacheIni.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "Misc/IQueuedWork.h"
#include "Misc/PackageName.h"
#include "Misc/QueuedThreadPool.h"
#include "Modules/ModuleManager.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "SkeletalRenderPublic.h"
#include "Styling/CoreStyle.h"
#include "Templates/UnrealTemplate.h"
#include "TimerManager.h"
#include "UObject/Package.h"
#include "UObject/SavePackage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SMultiLineEditableTextBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/SOverlay.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"

#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetRegistryModule.h"

static TAutoConsoleVariable<int32> CVarWoWLoginSceneVisualUpdateHz(
	TEXT("wow.LoginSceneVisualUpdateHz"),
	30,
	TEXT("Caps login scene material, camera, and light updates. 0 or lower updates every frame."));

static TAutoConsoleVariable<int32> CVarWoWLoginParticles(
	TEXT("wow.LoginParticles"),
	1,
	TEXT("Enables CPU-side login scene and character-select attachment particle updates."));

static TAutoConsoleVariable<int32> CVarWoWLoginParticleUpdateHz(
	TEXT("wow.LoginParticleUpdateHz"),
	20,
	TEXT("Caps CPU-side login particle simulation and procedural mesh updates. 0 or lower updates every frame."));

static TAutoConsoleVariable<int32> CVarWoWLoginMaxParticlesPerEmitter(
	TEXT("wow.LoginMaxParticlesPerEmitter"),
	240,
	TEXT("Runtime cap for login scene particles per emitter."));

static TAutoConsoleVariable<int32> CVarWoWLoginMaxAttachmentParticlesPerEmitter(
	TEXT("wow.LoginMaxAttachmentParticlesPerEmitter"),
	32,
	TEXT("Runtime cap for character-select attachment particles per emitter."));

namespace
{
constexpr const TCHAR* TestWorldMapPath = TEXT("/Game/Maps/Default/ThirdPersonMap");
constexpr float WowScalarToUE = 100.0f;
constexpr float M2CameraFovToProjectionDegrees = 34.5f;
constexpr int32 MaxParticlesPerEmitter = 840;
constexpr int32 MaxAttachmentParticlesPerEmitter = 64;

float GetCurrentViewportAspectRatio();
TSharedPtr<FJsonObject> TryReadJsonObject(const TSharedPtr<FJsonValue>& Value);
bool ReadJsonVector3(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FVector& OutValue);
bool ReadJsonLinearColorArray3(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FLinearColor OutValues[3]);
bool ReadJsonFloatArray3(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, float OutValues[3]);
FString BuildCharacterAppearanceSignature(
	int32 Race,
	int32 Gender,
	int32 Skin,
	int32 Face,
	int32 HairStyle,
	int32 HairColor,
	int32 FacialStyle,
	const FString& EquipmentSummary);
bool ReadParticleFloatSamples(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* FieldName,
	TArray<AWoWLoginGameMode::FParticleFloatSample>& OutSamples);
FWoWCharacterVisualBaseDefinition MakeCharacterVisualBaseDefinition(const AWoWLoginGameMode::FPlayerCharacterPreviewDefinition& Definition);

class FGlueLoginQueuedWork final : public IQueuedWork
{
public:
	FGlueLoginQueuedWork(
		int32 InRequestSerial,
		FString InAccountName,
		FString InPassword,
		TSharedPtr<TQueue<FGlueLoginAsyncResult, EQueueMode::Mpsc>, ESPMode::ThreadSafe> InResultQueue)
		: RequestSerial(InRequestSerial)
		, AccountName(MoveTemp(InAccountName))
		, Password(MoveTemp(InPassword))
		, ResultQueue(MoveTemp(InResultQueue))
	{
	}

	virtual void DoThreadedWork() override
	{
		FGlueLoginAsyncResult Result;
		Result.RequestSerial = RequestSerial;
		Result.AccountName = AccountName;
		Result.WorkerThreadId = FPlatformTLS::GetCurrentThreadId();
		const double StartSeconds = FPlatformTime::Seconds();
		UE_LOG(LogTemp, Display, TEXT("Glue login worker start: serial=%d thread=%u account=%s"), RequestSerial, Result.WorkerThreadId, *AccountName);
		Result.bAuthenticated = FWoWAzerothCoreService::AuthenticateAccount(AccountName, Password, Result.AccountRecord, Result.ErrorText);
		if (Result.bAuthenticated)
		{
			FString RealmError;
			if (FWoWAzerothCoreService::LoadRealmList(Result.Realms, RealmError) && Result.Realms.Num() > 0)
			{
				FString WorldError;
				Result.bWorldConnected = FWoWAzerothCoreService::ConnectToRealm(Result.Realms[0].Index, WorldError);
				if (!Result.bWorldConnected)
				{
					UE_LOG(LogTemp, Warning, TEXT("Glue login worker world auto-connect failed: serial=%d realm=%d error=%s"),
						RequestSerial,
						Result.Realms[0].Index,
						*WorldError);
				}
				else
				{
					Result.bCharactersLoaded = FWoWAzerothCoreService::GetCachedCharactersForAccount(
						Result.AccountRecord.AccountId,
						Result.Characters,
						Result.CharacterErrorText);
					if (!Result.bCharactersLoaded)
					{
						UE_LOG(LogTemp, Warning, TEXT("Glue login worker character enum failed: serial=%d error=%s"),
							RequestSerial,
							*Result.CharacterErrorText);
					}
				}
			}
			else
			{
				UE_LOG(LogTemp, Warning, TEXT("Glue login worker realm list unavailable after auth: serial=%d error=%s"),
					RequestSerial,
					*RealmError);
			}
		}
		Result.ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;
		UE_LOG(LogTemp, Display, TEXT("Glue login worker done: serial=%d thread=%u authenticated=%d worldConnected=%d elapsed=%.3f error=%s"),
			RequestSerial,
			Result.WorkerThreadId,
			Result.bAuthenticated ? 1 : 0,
			Result.bWorldConnected ? 1 : 0,
			Result.ElapsedSeconds,
			*Result.ErrorText);
		if (ResultQueue.IsValid())
		{
			ResultQueue->Enqueue(MoveTemp(Result));
		}
		delete this;
	}

	virtual void Abandon() override
	{
		delete this;
	}

private:
	int32 RequestSerial = 0;
	FString AccountName;
	FString Password;
	TSharedPtr<TQueue<FGlueLoginAsyncResult, EQueueMode::Mpsc>, ESPMode::ThreadSafe> ResultQueue;
};

class FGlueRealmQueuedWork final : public IQueuedWork
{
public:
	FGlueRealmQueuedWork(
		int32 InRequestSerial,
		int32 InRealmIndex,
		TSharedPtr<TQueue<FGlueRealmAsyncResult, EQueueMode::Mpsc>, ESPMode::ThreadSafe> InResultQueue)
		: RequestSerial(InRequestSerial)
		, RealmIndex(InRealmIndex)
		, ResultQueue(MoveTemp(InResultQueue))
	{
	}

	virtual void DoThreadedWork() override
	{
		FGlueRealmAsyncResult Result;
		Result.RequestSerial = RequestSerial;
		Result.RealmIndex = RealmIndex;
		Result.WorkerThreadId = FPlatformTLS::GetCurrentThreadId();
		const double StartSeconds = FPlatformTime::Seconds();
		UE_LOG(LogTemp, Display, TEXT("Glue realm worker start: serial=%d thread=%u realmIndex=%d"), RequestSerial, Result.WorkerThreadId, RealmIndex);
		Result.bConnected = FWoWAzerothCoreService::ConnectToRealm(RealmIndex, Result.ErrorText);
		if (Result.bConnected)
		{
			Result.bCharactersLoaded = FWoWAzerothCoreService::GetCachedCharactersForAccount(
				0,
				Result.Characters,
				Result.CharacterErrorText);
			if (!Result.bCharactersLoaded)
			{
				UE_LOG(LogTemp, Warning, TEXT("Glue realm worker character enum failed: serial=%d realmIndex=%d error=%s"),
					RequestSerial,
					RealmIndex,
					*Result.CharacterErrorText);
			}
		}
		Result.ElapsedSeconds = FPlatformTime::Seconds() - StartSeconds;
		UE_LOG(LogTemp, Display, TEXT("Glue realm worker done: serial=%d thread=%u connected=%d elapsed=%.3f error=%s"),
			RequestSerial,
			Result.WorkerThreadId,
			Result.bConnected ? 1 : 0,
			Result.ElapsedSeconds,
			*Result.ErrorText);
		if (ResultQueue.IsValid())
		{
			ResultQueue->Enqueue(MoveTemp(Result));
		}
		delete this;
	}

	virtual void Abandon() override
	{
		delete this;
	}

private:
	int32 RequestSerial = 0;
	int32 RealmIndex = 0;
	TSharedPtr<TQueue<FGlueRealmAsyncResult, EQueueMode::Mpsc>, ESPMode::ThreadSafe> ResultQueue;
};

using FCharacterPreviewEquipmentDisplay = FWoWCharacterEquipmentDisplay;

using FCharacterPreviewAttachmentDefinition = FWoWCharacterVisualAttachmentDefinition;

struct FCharacterAppearanceDbcItemDisplay
{
	int32 DisplayId = 0;
	int32 GeosetGroups[3] = {};
	int32 HelmetGeosetVisMale = 0;
	int32 HelmetGeosetVisFemale = 0;
};

struct FCharacterAppearanceDbcHelmetVis
{
	int32 Id = 0;
	uint32 HairFlags = 0;
	uint32 Facial1Flags = 0;
	uint32 Facial2Flags = 0;
	uint32 Facial3Flags = 0;
	uint32 EarsFlags = 0;
};

struct FCharacterAppearanceDbcHairGeoset
{
	int32 Race = 0;
	int32 Gender = 0;
	int32 HairStyle = 0;
	int32 GeosetId = 0;
	bool bBald = false;
};

struct FCharacterAppearanceDbcFacialHairStyle
{
	int32 Race = 0;
	int32 Gender = 0;
	int32 Style = 0;
	int32 Geoset100 = 0;
	int32 Geoset200 = 0;
	int32 Geoset300 = 0;
};

struct FCharacterAppearanceDbcCache
{
	bool bLoaded = false;
	TMap<int32, FCharacterAppearanceDbcItemDisplay> ItemDisplayInfo;
	TMap<int32, FCharacterAppearanceDbcHelmetVis> HelmetGeosetVisData;
	TArray<FCharacterAppearanceDbcHairGeoset> HairGeosets;
	TArray<FCharacterAppearanceDbcFacialHairStyle> FacialHairStyles;
};

using FItemObjectComponentManifestEntry = FWoWItemObjectComponentEntry;
using FItemObjectComponentLibrary = FWoWItemObjectComponentLibrary;

void ApplyGlueCameraProjection(UCameraComponent* CameraComponent, float M2ProjectionFovDegrees, float NearClip, float FarClip)
{
	if (!CameraComponent)
	{
		return;
	}

	const float ViewportAspectRatio = GetCurrentViewportAspectRatio();
	// WoW Glue ModelFFX presents login M2 cameras through the full UI viewport. In practice the
	// M2 camera FOV behaves as the horizontal view angle for the login background, while UE's
	// MaintainYFOV path expects FieldOfView to be authored with CameraComponent.AspectRatio.
	CameraComponent->SetFieldOfView(M2ProjectionFovDegrees);
	CameraComponent->SetAspectRatio(ViewportAspectRatio);
	CameraComponent->SetConstraintAspectRatio(false);
	CameraComponent->bOverrideAspectRatioAxisConstraint = true;
	CameraComponent->SetAspectRatioAxisConstraint(AspectRatio_MaintainYFOV);
	CameraComponent->SetOrthoNearClipPlane(NearClip);
	CameraComponent->SetOrthoFarClipPlane(FarClip);
}

FString MakeObjectPathFromPackagePath(const FString& PackagePath)
{
	return PackagePath + TEXT(".") + FPackageName::GetLongPackageAssetName(PackagePath);
}

FString SanitizePackageSegment(FString Value)
{
	for (TCHAR& Character : Value)
	{
		if (!FChar::IsAlnum(Character) && Character != TEXT('_'))
		{
			Character = TEXT('_');
		}
	}
	return Value;
}

FString BuildEquipmentDisplaySummary(const TArray<FWoWGlueCharacterEquipmentSlot>& Equipment)
{
	TArray<FString> Parts;
	Parts.Reserve(Equipment.Num());
	for (int32 Slot = 0; Slot < Equipment.Num(); ++Slot)
	{
		const FWoWGlueCharacterEquipmentSlot& Item = Equipment[Slot];
		if (Item.DisplayId == 0 && Item.InventoryType == 0 && Item.EnchantId == 0)
		{
			continue;
		}
		Parts.Add(FString::Printf(TEXT("%d:%u:%u:%u"), Slot, Item.DisplayId, static_cast<uint32>(Item.InventoryType), Item.EnchantId));
	}
	return FString::Join(Parts, TEXT(";"));
}

void WriteJsonNumber(TSharedRef<FJsonObject> Object, const TCHAR* FieldName, int32 Value)
{
	Object->SetNumberField(FieldName, Value);
}

FString QuoteCommandArgument(const FString& Value)
{
	return FString::Printf(TEXT("\"%s\""), *Value.Replace(TEXT("\""), TEXT("\\\"")));
}

FString GetColumnOrEmpty(const TArray<FString>& Columns, int32 Index)
{
	return Columns.IsValidIndex(Index) ? Columns[Index] : FString();
}

bool ShouldWriteCharacterAppearanceDebugDescriptors()
{
	bool bValue = true;
	if (GConfig)
	{
		GConfig->GetBool(TEXT("/Script/wow.WoWLoginGameMode"), TEXT("bWriteCharacterAppearanceDebugDescriptors"), bValue, GGameIni);
	}
	return bValue;
}

int32 CountEquipmentDisplayEntries(const FString& EquipmentSummary)
{
	if (EquipmentSummary.IsEmpty())
	{
		return 0;
	}

	TArray<FString> Parts;
	EquipmentSummary.ParseIntoArray(Parts, TEXT(";"), true);
	return Parts.Num();
}

bool DoesEquipmentSummaryRequireObjectComponents(const FString& EquipmentSummary)
{
	if (EquipmentSummary.IsEmpty())
	{
		return false;
	}

	TArray<FString> Parts;
	EquipmentSummary.ParseIntoArray(Parts, TEXT(";"), true);
	for (const FString& Part : Parts)
	{
		TArray<FString> Fields;
		Part.ParseIntoArray(Fields, TEXT(":"), false);
		if (Fields.Num() < 3)
		{
			continue;
		}
		const int32 Slot = FCString::Atoi(*Fields[0]);
		const int32 DisplayId = FCString::Atoi(*Fields[1]);
		const int32 InventoryType = FCString::Atoi(*Fields[2]);
		if (DisplayId <= 0)
		{
			continue;
		}
		if (Slot == 0 || Slot == 2 || Slot == 15 || Slot == 16)
		{
			return true;
		}
		if (InventoryType == 1 || InventoryType == 3 || InventoryType == 13 || InventoryType == 17)
		{
			return true;
		}
	}
	return false;
}

bool HasRequiredCharacterPreviewAttachmentDescriptor(const FString& DescriptorPath, const FString& EquipmentSummary)
{
	if (!DoesEquipmentSummaryRequireObjectComponents(EquipmentSummary))
	{
		return true;
	}
	if (!FPaths::FileExists(DescriptorPath))
	{
		return false;
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
	return RootObject->TryGetArrayField(TEXT("attachments"), AttachmentValues) && AttachmentValues && AttachmentValues->Num() > 0;
}

TArray<FCharacterPreviewEquipmentDisplay> ParseCharacterPreviewEquipmentSummary(const FString& EquipmentSummary)
{
	return WoWCharacterEquipment::ParseEquipmentSummary(EquipmentSummary);
}

TSharedRef<FJsonObject> BuildCharacterPreviewAppearanceDescriptorObject(
	const AWoWLoginGameMode::FCharacterSelectPreviewAppearance& Appearance,
	int32 AccountId)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("format"), TEXT("wow_character_appearance_descriptor_v1"));
	Root->SetStringField(TEXT("source"), TEXT("worldserver character enum in-memory snapshot"));
	WriteJsonNumber(Root, TEXT("guid"), Appearance.Guid);
	WriteJsonNumber(Root, TEXT("account"), AccountId);
	Root->SetStringField(TEXT("name"), Appearance.CharacterName);
	WriteJsonNumber(Root, TEXT("race"), Appearance.Race);
	WriteJsonNumber(Root, TEXT("class"), Appearance.ClassId);
	WriteJsonNumber(Root, TEXT("gender"), Appearance.Gender);
	WriteJsonNumber(Root, TEXT("level"), 0);
	WriteJsonNumber(Root, TEXT("skinColor"), Appearance.Skin);
	WriteJsonNumber(Root, TEXT("face"), Appearance.Face);
	WriteJsonNumber(Root, TEXT("hairStyle"), Appearance.HairStyle);
	WriteJsonNumber(Root, TEXT("hairColor"), Appearance.HairColor);
	WriteJsonNumber(Root, TEXT("facialStyle"), Appearance.FacialStyle);
	WriteJsonNumber(Root, TEXT("zone"), 0);
	WriteJsonNumber(Root, TEXT("map"), 0);

	TArray<TSharedPtr<FJsonValue>> EquipmentDisplayIds;
	EquipmentDisplayIds.SetNumZeroed(23);
	for (int32 Index = 0; Index < EquipmentDisplayIds.Num(); ++Index)
	{
		EquipmentDisplayIds[Index] = MakeShared<FJsonValueNumber>(0);
	}

	TArray<TSharedPtr<FJsonValue>> EquipmentValues;
	for (const FCharacterPreviewEquipmentDisplay& Entry : ParseCharacterPreviewEquipmentSummary(Appearance.EquipmentSummary))
	{
		if (EquipmentDisplayIds.IsValidIndex(Entry.Slot))
		{
			EquipmentDisplayIds[Entry.Slot] = MakeShared<FJsonValueNumber>(Entry.DisplayId);
		}

		TSharedRef<FJsonObject> EquipmentObject = MakeShared<FJsonObject>();
		WriteJsonNumber(EquipmentObject, TEXT("slot"), Entry.Slot);
		WriteJsonNumber(EquipmentObject, TEXT("itemEntry"), 0);
		WriteJsonNumber(EquipmentObject, TEXT("displayId"), Entry.DisplayId);
		WriteJsonNumber(EquipmentObject, TEXT("inventoryType"), Entry.InventoryType);
		EquipmentObject->SetStringField(TEXT("name"), FString());
		WriteJsonNumber(EquipmentObject, TEXT("enchantOrFlags"), Entry.EnchantId);
		EquipmentValues.Add(MakeShared<FJsonValueObject>(EquipmentObject));
	}
	Root->SetArrayField(TEXT("equipmentDisplayIds"), EquipmentDisplayIds);
	Root->SetArrayField(TEXT("equipment"), EquipmentValues);
	return Root;
}

bool DoesEquipmentSummaryContainAttachment(const FString& EquipmentSummary, int32 Slot, int32 DisplayId)
{
	return WoWCharacterVisualKit::DoesEquipmentSummaryContainAttachment(EquipmentSummary, Slot, DisplayId);
}

bool TryResolveAttachmentSocketById(USkeletalMeshComponent* MeshComponent, const FName RequestedSocketName, FName& OutSocketName)
{
	OutSocketName = RequestedSocketName;
	if (!MeshComponent || RequestedSocketName.IsNone())
	{
		return false;
	}

	if (MeshComponent->DoesSocketExist(RequestedSocketName))
	{
		return true;
	}

	const FString RequestedText = RequestedSocketName.ToString();
	int32 IdStart = INDEX_NONE;
	if (!RequestedText.FindLastChar(TEXT('_'), IdStart) || IdStart + 1 >= RequestedText.Len())
	{
		return false;
	}

	const FString IdText = RequestedText.Mid(IdStart + 1);
	if (IdText.IsEmpty() || !IdText.IsNumeric())
	{
		return false;
	}

	const FString IdSuffix = FString::Printf(TEXT("_ID_%03d"), FCString::Atoi(*IdText));
	const TArray<FName> SocketNames = MeshComponent->GetAllSocketNames();
	for (const FName& SocketName : SocketNames)
	{
		if (SocketName.ToString().EndsWith(IdSuffix, ESearchCase::IgnoreCase))
		{
			OutSocketName = SocketName;
			return true;
		}
	}

	return false;
}

FString BuildCharacterPreviewAttachmentMaterialPackagePath(const FString& AttachmentMeshPath)
{
	return WoWCharacterVisualKit::BuildAttachmentMaterialPackagePath(AttachmentMeshPath);
}

bool ReadJsonIntSetField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TSet<int32>& OutValues)
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

bool ReadItemObjectComponentLibrary(const FString& ManifestPath, FItemObjectComponentLibrary& OutLibrary)
{
	if (!WoWCharacterVisualKit::ReadItemObjectComponentLibrary(ManifestPath, OutLibrary))
	{
		return false;
	}

	UE_LOG(LogTemp, Display, TEXT("Items ObjectComponent 清单已加载: entries=%d displayIndex=%d path=%s"),
		OutLibrary.Entries.Num(),
		OutLibrary.EntryIndicesByDisplayId.Num(),
		*ManifestPath);
	return true;
}

const FItemObjectComponentLibrary& GetItemObjectComponentLibrary()
{
	static FItemObjectComponentLibrary Library;
	static bool bAttemptedLoad = false;
	if (!bAttemptedLoad)
	{
		bAttemptedLoad = true;
		const FString ContentManifestPath = FPaths::ProjectContentDir() / TEXT("WoW/Generated/Manifests/ItemObjectComponents.all.manifest.json");
		const FString SavedManifestPath = FPaths::ProjectSavedDir() / TEXT("WoWData/AssetLibraryBuild/ItemObjectComponents.all.manifest.json");
		if (!ReadItemObjectComponentLibrary(SavedManifestPath, Library) &&
			!ReadItemObjectComponentLibrary(ContentManifestPath, Library))
		{
			UE_LOG(LogTemp, Warning, TEXT("Items ObjectComponent 清单不存在或不可读: content=%s saved=%s。请运行 Tools/资源提取/02_物品资源/物品组件提取.py"),
				*ContentManifestPath,
				*SavedManifestPath);
		}
	}
	return Library;
}

FString BuildItemObjectComponentMeshObjectPath(const FItemObjectComponentManifestEntry& Entry)
{
	return WoWCharacterVisualKit::BuildItemObjectComponentMeshObjectPath(Entry);
}

FString BuildItemObjectComponentStandAnimationObjectPath(const FItemObjectComponentManifestEntry& Entry)
{
	return WoWCharacterVisualKit::BuildItemObjectComponentStandAnimationObjectPath(Entry);
}

FName ResolveSharedItemAttachmentSocketName(const FString& Component, int32 Slot, int32 SequenceIndex)
{
	return WoWCharacterVisualKit::ResolveSharedItemAttachmentSocketName(Component, Slot, SequenceIndex);
}

bool DoesItemObjectComponentEntryMatchEquipment(
	const FItemObjectComponentManifestEntry& Entry,
	const FCharacterPreviewEquipmentDisplay& Equipment,
	const AWoWLoginGameMode::FCharacterSelectPreviewAppearance& Appearance)
{
	FWoWCharacterVisualInput Input;
	Input.Guid = Appearance.Guid;
	Input.CharacterName = Appearance.CharacterName;
	Input.ClassId = Appearance.ClassId;
	Input.Race = Appearance.Race;
	Input.Gender = Appearance.Gender;
	Input.Skin = Appearance.Skin;
	Input.Face = Appearance.Face;
	Input.HairStyle = Appearance.HairStyle;
	Input.HairColor = Appearance.HairColor;
	Input.FacialStyle = Appearance.FacialStyle;
	Input.EquipmentSummary = Appearance.EquipmentSummary;
	return WoWCharacterVisualKit::DoesItemObjectComponentEntryMatchEquipment(Entry, Equipment, Input);
}

bool BuildSharedItemObjectAttachmentDefinitions(
	const AWoWLoginGameMode::FCharacterSelectPreviewAppearance& Appearance,
	TArray<FCharacterPreviewAttachmentDefinition>& OutAttachments)
{
	OutAttachments.Reset();
	const FItemObjectComponentLibrary& Library = GetItemObjectComponentLibrary();
	FWoWCharacterVisualInput Input;
	Input.Guid = Appearance.Guid;
	Input.CharacterName = Appearance.CharacterName;
	Input.ClassId = Appearance.ClassId;
	Input.Race = Appearance.Race;
	Input.Gender = Appearance.Gender;
	Input.Skin = Appearance.Skin;
	Input.Face = Appearance.Face;
	Input.HairStyle = Appearance.HairStyle;
	Input.HairColor = Appearance.HairColor;
	Input.FacialStyle = Appearance.FacialStyle;
	Input.EquipmentSummary = Appearance.EquipmentSummary;
	return WoWCharacterVisualKit::BuildSharedItemObjectAttachmentDefinitions(Input, Library, OutAttachments);
}

int32 GetEquipmentDisplayIdForSlot(const FString& EquipmentSummary, int32 Slot)
{
	const TArray<FCharacterPreviewEquipmentDisplay> Equipment = ParseCharacterPreviewEquipmentSummary(EquipmentSummary);
	for (const FCharacterPreviewEquipmentDisplay& Entry : Equipment)
	{
		if (Entry.Slot == Slot && Entry.DisplayId > 0)
		{
			return Entry.DisplayId;
		}
	}
	return 0;
}

bool DoesGeneratedAssetExist(const FString& ObjectPath)
{
	const FString TrimmedObjectPath = ObjectPath.TrimStartAndEnd();
	if (TrimmedObjectPath.IsEmpty())
	{
		return false;
	}
	if (FindObject<UObject>(nullptr, *TrimmedObjectPath))
	{
		return true;
	}

	if (!FModuleManager::Get().IsModuleLoaded(TEXT("AssetRegistry")))
	{
		FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	}
	FAssetRegistryModule& AssetRegistryModule = FModuleManager::GetModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	const FSoftObjectPath SoftObjectPath(TrimmedObjectPath);
	const FAssetData AssetData = AssetRegistryModule.Get().GetAssetByObjectPath(SoftObjectPath, true);
	if (AssetData.IsValid())
	{
		return true;
	}

	const FString PackageName = FPackageName::ObjectPathToPackageName(TrimmedObjectPath);
	const FString ObjectName = FPackageName::ObjectPathToObjectName(TrimmedObjectPath);
	if (PackageName.IsEmpty() || ObjectName.IsEmpty())
	{
		return false;
	}

	FARFilter Filter;
	Filter.PackageNames.Add(*PackageName);
	TArray<FAssetData> Assets;
	AssetRegistryModule.Get().GetAssets(Filter, Assets);
	if (Assets.ContainsByPredicate([&ObjectName](const FAssetData& Candidate)
	{
		return Candidate.AssetName.ToString().Equals(ObjectName, ESearchCase::CaseSensitive);
	}))
	{
		return true;
	}

	FString PackageFilename;
	if (FPackageName::TryConvertLongPackageNameToFilename(PackageName, PackageFilename, FPackageName::GetAssetPackageExtension()) &&
		FPaths::FileExists(PackageFilename))
	{
		return true;
	}

	return false;
}

template <typename TObjectType>
TObjectType* ResolveLoadedObject(const FString& ObjectPath)
{
	const FString TrimmedObjectPath = ObjectPath.TrimStartAndEnd();
	if (TrimmedObjectPath.IsEmpty())
	{
		return nullptr;
	}
	const FSoftObjectPath SoftObjectPath(TrimmedObjectPath);
	return Cast<TObjectType>(SoftObjectPath.ResolveObject());
}

FString ResolveCharacterPreviewAnimationObjectPath(const FString& DefaultAnimationObjectPath, const FString& EquipmentSummary)
{
	const int32 WeaponSlot = WoWCharacterVisualKit::GetEquippedWeaponGripSlot(EquipmentSummary);
	const FString GripAnimationObjectPath = WoWCharacterVisualKit::BuildGripAnimationObjectPath(DefaultAnimationObjectPath, WeaponSlot);
	const FString ResolvedAnimationObjectPath = WoWCharacterVisualKit::ResolveAnimationObjectPath(
		DefaultAnimationObjectPath,
		EquipmentSummary,
		[](const FString& ObjectPath)
		{
			return DoesGeneratedAssetExist(ObjectPath);
		});
	if (ResolvedAnimationObjectPath.Equals(GripAnimationObjectPath, ESearchCase::CaseSensitive) && !GripAnimationObjectPath.IsEmpty())
	{
		UE_LOG(LogTemp, Display, TEXT("角色预览装备动作选择: weaponSlot=%d selected=Grip anim=%s default=%s equipment=%s"),
			WeaponSlot,
			*ResolvedAnimationObjectPath,
			*DefaultAnimationObjectPath,
			*EquipmentSummary);
		return ResolvedAnimationObjectPath;
	}
	UE_LOG(LogTemp, Display, TEXT("角色预览装备动作选择: weaponSlot=%d selected=Default gripCandidate=%s default=%s equipment=%s"),
		WeaponSlot,
		*GripAnimationObjectPath,
		*DefaultAnimationObjectPath,
		*EquipmentSummary);
	return ResolvedAnimationObjectPath;
}

bool IsCharacterPreviewSelectableAnimationPath(const FString& ObjectPath)
{
	FString AssetName = FPackageName::ObjectPathToObjectName(ObjectPath);
	if (AssetName.IsEmpty())
	{
		ObjectPath.Split(TEXT("."), nullptr, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	}

	if (AssetName.IsEmpty())
	{
		return false;
	}

	return !AssetName.Contains(TEXT("_HandsClosed_"), ESearchCase::IgnoreCase) &&
		!AssetName.EndsWith(TEXT("_GripL"), ESearchCase::IgnoreCase) &&
		!AssetName.EndsWith(TEXT("_GripR"), ESearchCase::IgnoreCase);
}

const TCHAR* GetCharacterPreviewAnimationNameById(int32 AnimationId)
{
	switch (AnimationId)
	{
	case 0: return TEXT("Stand");
	case 1: return TEXT("Death");
	case 4: return TEXT("Walk");
	case 5: return TEXT("Run");
	case 16: return TEXT("AttackUnarmed");
	case 17: return TEXT("Attack1H");
	case 18: return TEXT("Attack2H");
	case 19: return TEXT("Attack2HL");
	case 25: return TEXT("ReadyUnarmed");
	case 26: return TEXT("Ready1H");
	case 27: return TEXT("Ready2H");
	case 28: return TEXT("Ready2HL");
	case 37: return TEXT("JumpStart");
	case 38: return TEXT("Jump");
	case 39: return TEXT("JumpEnd");
	case 46: return TEXT("AttackBow");
	case 49: return TEXT("AttackRifle");
	case 89: return TEXT("Sheath");
	case 90: return TEXT("HipSheath");
	case 187: return TEXT("JumpLandRun");
	default: return nullptr;
	}
}

FString GetCharacterPreviewAnimationDisplayName(int32 AnimationId, const FString& ObjectPath)
{
	if (const TCHAR* KnownName = GetCharacterPreviewAnimationNameById(AnimationId))
	{
		return KnownName;
	}

	FString AssetName = FPackageName::ObjectPathToObjectName(ObjectPath);
	if (AssetName.IsEmpty())
	{
		ObjectPath.Split(TEXT("."), nullptr, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	}

	const FString Token = FString::Printf(TEXT("_%03d_"), AnimationId);
	FString Prefix;
	FString Tail;
	if (AssetName.Split(Token, &Prefix, &Tail, ESearchCase::IgnoreCase, ESearchDir::FromStart))
	{
		FString Name;
		if (Tail.Split(TEXT("_00"), &Name, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd) && !Name.IsEmpty())
		{
			return Name;
		}
		return Tail;
	}

	return FString::Printf(TEXT("Anim_%03d"), AnimationId);
}

FString BuildCharacterPreviewLogicalAnimationObjectPath(const FString& StandAnimationObjectPath, int32 AnimationId)
{
	if (StandAnimationObjectPath.IsEmpty())
	{
		return FString();
	}

	const TCHAR* AnimationName = GetCharacterPreviewAnimationNameById(AnimationId);
	if (!AnimationName)
	{
		return FString();
	}

	FString PackagePath;
	FString AssetName;
	if (!StandAnimationObjectPath.Split(TEXT("."), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
	{
		return FString();
	}

	FString Prefix;
	if (AssetName.Split(TEXT("_000_Stand_00"), &Prefix, nullptr, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
	{
		const FString NewAssetName = FString::Printf(TEXT("%s_%03d_%s_00"), *Prefix, AnimationId, AnimationName);
		FString DirectoryPath = PackagePath;
		const int32 LastSlashIndex = DirectoryPath.Find(TEXT("/"), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (LastSlashIndex != INDEX_NONE)
		{
			DirectoryPath.LeftInline(LastSlashIndex, EAllowShrinking::No);
		}
		return DirectoryPath / NewAssetName + TEXT(".") + NewAssetName;
	}

	return FString();
}

bool TryParseCharacterPreviewAnimationIdFromObjectPath(const FString& ObjectPath, int32& OutAnimationId)
{
	FString AssetName = FPackageName::ObjectPathToObjectName(ObjectPath);
	if (AssetName.IsEmpty())
	{
		ObjectPath.Split(TEXT("."), nullptr, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	}

	TArray<FString> Tokens;
	AssetName.ParseIntoArray(Tokens, TEXT("_"), true);
	for (const FString& Token : Tokens)
	{
		if (Token.Len() != 3 || !Token.IsNumeric())
		{
			continue;
		}

		OutAnimationId = FCString::Atoi(*Token);
		return true;
	}

	for (int32 Index = 0; Index + 4 < ObjectPath.Len(); ++Index)
	{
		if (ObjectPath[Index] != TEXT('_') ||
			ObjectPath[Index + 4] != TEXT('_') ||
			!FChar::IsDigit(ObjectPath[Index + 1]) ||
			!FChar::IsDigit(ObjectPath[Index + 2]) ||
			!FChar::IsDigit(ObjectPath[Index + 3]))
		{
			continue;
		}

		OutAnimationId = FCString::Atoi(*ObjectPath.Mid(Index + 1, 3));
		return true;
	}

	return false;
}

TMap<int32, FString> BuildCharacterPreviewAnimationPathById(const TArray<FString>& AnimationPaths)
{
	TMap<int32, FString> AnimationPathById;
	for (const FString& AnimationPath : AnimationPaths)
	{
		int32 AnimationId = INDEX_NONE;
		if (!TryParseCharacterPreviewAnimationIdFromObjectPath(AnimationPath, AnimationId))
		{
			continue;
		}

		if (!AnimationPathById.Contains(AnimationId))
		{
			AnimationPathById.Add(AnimationId, AnimationPath);
		}
	}

	return AnimationPathById;
}

bool IsCharacterPreviewAnimationCompatibleWithMesh(
	USkeletalMesh* PreviewMesh,
	UAnimSequence* PreviewAnimation,
	const FString& MeshObjectPath,
	const FString& AnimationObjectPath,
	const FString& Context,
	bool bLogRejection = true)
{
	if (!PreviewMesh || !PreviewAnimation)
	{
		return false;
	}

	const USkeleton* AnimSkeleton = PreviewAnimation->GetSkeleton();
	const USkeleton* MeshSkeleton = PreviewMesh->GetSkeleton();
	if (!AnimSkeleton || !AnimSkeleton->IsCompatibleMesh(PreviewMesh, false))
	{
		if (bLogRejection)
		{
			UE_LOG(LogTemp, Error, TEXT("角色预览拒绝不兼容动画: context=%s mesh=%s meshSkeleton=%s anim=%s animSkeleton=%s"),
				*Context,
				*MeshObjectPath,
				MeshSkeleton ? *MeshSkeleton->GetPathName() : TEXT("<null>"),
				*AnimationObjectPath,
				AnimSkeleton ? *AnimSkeleton->GetPathName() : TEXT("<null>"));
		}
		return false;
	}

	return true;
}

bool IsCharacterPreviewAnimationPlayable(UAnimSequence* PreviewAnimation)
{
	return PreviewAnimation &&
		PreviewAnimation->GetPlayLength() >= 0.35f &&
		PreviewAnimation->GetNumberOfSampledKeys() > 2;
}

void CollectCharacterPreviewIdleFallbackAnimationPaths(
	const FString& StandAnimationObjectPath,
	const FString& EquipmentSummary,
	TArray<FString>& OutAnimationObjectPaths)
{
	if (StandAnimationObjectPath.IsEmpty())
	{
		return;
	}

	auto AddAnimationPath = [&OutAnimationObjectPaths](const FString& ObjectPath)
	{
		const FString TrimmedObjectPath = ObjectPath.TrimStartAndEnd();
		if (!TrimmedObjectPath.IsEmpty() && DoesGeneratedAssetExist(TrimmedObjectPath))
		{
			OutAnimationObjectPaths.AddUnique(TrimmedObjectPath);
		}
	};

	const int32 WeaponSlot = WoWCharacterVisualKit::GetEquippedWeaponGripSlot(EquipmentSummary);
	TArray<int32> CandidateAnimationIds;
	if (WeaponSlot == 15 || WeaponSlot == 16)
	{
		CandidateAnimationIds = {26, 27, 28, 25, 0};
	}
	else
	{
		CandidateAnimationIds = {25, 0};
	}

	for (const int32 AnimationId : CandidateAnimationIds)
	{
		const FString CandidatePath = AnimationId == 0
			? StandAnimationObjectPath
			: BuildCharacterPreviewLogicalAnimationObjectPath(StandAnimationObjectPath, AnimationId);
		if (CandidatePath.IsEmpty())
		{
			continue;
		}

		AddAnimationPath(CandidatePath);
		if (AnimationId != 0)
		{
			AddAnimationPath(ResolveCharacterPreviewAnimationObjectPath(CandidatePath, EquipmentSummary));
		}
	}
}

bool TryResolvePlayableCharacterPreviewAnimation(
	USkeletalMesh* PreviewMesh,
	const FString& MeshObjectPath,
	const FString& DefinitionId,
	const FString& CandidatePath,
	UAnimSequence*& OutAnimation,
	FString& OutAnimationObjectPath)
{
	UAnimSequence* CandidateAnimation = ResolveLoadedObject<UAnimSequence>(CandidatePath);
	if (!CandidateAnimation ||
		!IsCharacterPreviewAnimationPlayable(CandidateAnimation) ||
		!IsCharacterPreviewAnimationCompatibleWithMesh(PreviewMesh, CandidateAnimation, MeshObjectPath, CandidatePath, DefinitionId, false))
	{
		return false;
	}

	OutAnimation = CandidateAnimation;
	OutAnimationObjectPath = CandidatePath;
	return true;
}

bool ResolveCharacterPreviewCompatibleAnimationPathById(
	USkeletalMesh* PreviewMesh,
	int32 AnimationId,
	const TArray<FString>& CandidateAnimationPaths,
	const FString& MeshObjectPath,
	const FString& Context,
	FString& OutAnimationObjectPath,
	bool bLogFailure = true)
{
	if (!PreviewMesh || AnimationId == INDEX_NONE)
	{
		return false;
	}

	for (const FString& CandidatePath : CandidateAnimationPaths)
	{
		int32 CandidateAnimationId = INDEX_NONE;
		if (!TryParseCharacterPreviewAnimationIdFromObjectPath(CandidatePath, CandidateAnimationId) || CandidateAnimationId != AnimationId)
		{
			continue;
		}

		UAnimSequence* CandidateAnimation = ResolveLoadedObject<UAnimSequence>(CandidatePath);
		if (!CandidateAnimation)
		{
			continue;
		}

		if (IsCharacterPreviewAnimationCompatibleWithMesh(PreviewMesh, CandidateAnimation, MeshObjectPath, CandidatePath, Context, bLogFailure))
		{
			OutAnimationObjectPath = CandidatePath;
			return true;
		}
	}

	if (bLogFailure)
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览没有找到兼容动作: context=%s mesh=%s animId=%d candidates=%d"),
			*Context,
			*MeshObjectPath,
			AnimationId,
			CandidateAnimationPaths.Num());
	}
	return false;
}

void AddCharacterPreviewAnimationPathsForSearchObjectPath(const FString& SearchObjectPath, TArray<FString>& InOutAnimationPaths)
{
#if WITH_EDITOR
	static TMap<FString, TArray<FString>> CachedAnimationPathsByPackagePath;

	FString PackagePath;
	FString AssetName;
	if (!SearchObjectPath.Split(TEXT("."), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
	{
		return;
	}

	int32 LastSlashIndex = INDEX_NONE;
	if (!PackagePath.FindLastChar(TEXT('/'), LastSlashIndex) || LastSlashIndex <= 0)
	{
		return;
	}

	PackagePath.LeftInline(LastSlashIndex, EAllowShrinking::No);

	if (const TArray<FString>* CachedAnimationPaths = CachedAnimationPathsByPackagePath.Find(PackagePath))
	{
		for (const FString& AnimationPath : *CachedAnimationPaths)
		{
			InOutAnimationPaths.AddUnique(AnimationPath);
		}
		return;
	}

	FAssetRegistryModule& AssetRegistryModule = FModuleManager::LoadModuleChecked<FAssetRegistryModule>(TEXT("AssetRegistry"));
	TArray<FString> PackagePathsToScan;
	PackagePathsToScan.Add(PackagePath);
	AssetRegistryModule.Get().ScanPathsSynchronous(PackagePathsToScan, true);

	FARFilter Filter;
	Filter.PackagePaths.Add(*PackagePath);
	Filter.ClassPaths.Add(UAnimSequence::StaticClass()->GetClassPathName());
	Filter.bRecursivePaths = false;

	TArray<FAssetData> AssetDataList;
	AssetRegistryModule.Get().GetAssets(Filter, AssetDataList);
	TArray<FString> CachedAnimationPaths;
	CachedAnimationPaths.Reserve(AssetDataList.Num());
	for (const FAssetData& AssetData : AssetDataList)
	{
		CachedAnimationPaths.AddUnique(AssetData.GetSoftObjectPath().ToString());
	}
	for (const FString& AnimationPath : CachedAnimationPaths)
	{
		InOutAnimationPaths.AddUnique(AnimationPath);
	}
	CachedAnimationPathsByPackagePath.Add(PackagePath, MoveTemp(CachedAnimationPaths));
#endif
}

TArray<FString> BuildCharacterPreviewAvailableAnimationPaths(
	const FString& StandAnimationPath,
	const FString& CharacterPreviewAnimationPath,
	const FString& ActiveAnimationPath,
	const FString& OverrideAnimationPath,
	const FString& CharacterPreviewLookupAnimationPath = FString(),
	const FString& BaseLookupAnimationPath = FString())
{
	TArray<FString> AnimationPaths;

	auto AddAnimationPath = [&AnimationPaths](const FString& ObjectPath)
	{
		const FString TrimmedPath = ObjectPath.TrimStartAndEnd();
		if (!TrimmedPath.IsEmpty())
		{
			AnimationPaths.AddUnique(TrimmedPath);
		}
	};

	AddAnimationPath(CharacterPreviewAnimationPath);
	AddAnimationPath(ActiveAnimationPath);
	AddAnimationPath(OverrideAnimationPath);
	AddAnimationPath(CharacterPreviewLookupAnimationPath);
	AddAnimationPath(StandAnimationPath);
	AddAnimationPath(BaseLookupAnimationPath);

	AddCharacterPreviewAnimationPathsForSearchObjectPath(CharacterPreviewAnimationPath, AnimationPaths);
	AddCharacterPreviewAnimationPathsForSearchObjectPath(CharacterPreviewLookupAnimationPath, AnimationPaths);
	AddCharacterPreviewAnimationPathsForSearchObjectPath(ActiveAnimationPath, AnimationPaths);
	AddCharacterPreviewAnimationPathsForSearchObjectPath(OverrideAnimationPath, AnimationPaths);
	AddCharacterPreviewAnimationPathsForSearchObjectPath(StandAnimationPath, AnimationPaths);
	AddCharacterPreviewAnimationPathsForSearchObjectPath(BaseLookupAnimationPath, AnimationPaths);

	AnimationPaths.RemoveAll([](const FString& AnimationPath)
	{
		return !IsCharacterPreviewSelectableAnimationPath(AnimationPath);
	});

	return AnimationPaths;
}

bool ReadCharacterAppearanceDbcUInt32(const TArray<uint8>& Data, int32 Offset, uint32& OutValue)
{
	if (Offset < 0 || Offset + 4 > Data.Num())
	{
		return false;
	}

	OutValue =
		static_cast<uint32>(Data[Offset]) |
		(static_cast<uint32>(Data[Offset + 1]) << 8) |
		(static_cast<uint32>(Data[Offset + 2]) << 16) |
		(static_cast<uint32>(Data[Offset + 3]) << 24);
	return true;
}

bool ReadCharacterAppearanceWdbcHeader(
	const FString& Path,
	TArray<uint8>& OutData,
	uint32& OutRecordCount,
	uint32& OutFieldCount,
	uint32& OutRecordSize)
{
	if (!FFileHelper::LoadFileToArray(OutData, *Path) || OutData.Num() < 20)
	{
		return false;
	}

	if (OutData[0] != 'W' || OutData[1] != 'D' || OutData[2] != 'B' || OutData[3] != 'C')
	{
		return false;
	}

	uint32 StringBlockSize = 0;
	if (!ReadCharacterAppearanceDbcUInt32(OutData, 4, OutRecordCount) ||
		!ReadCharacterAppearanceDbcUInt32(OutData, 8, OutFieldCount) ||
		!ReadCharacterAppearanceDbcUInt32(OutData, 12, OutRecordSize) ||
		!ReadCharacterAppearanceDbcUInt32(OutData, 16, StringBlockSize))
	{
		return false;
	}

	const int64 ExpectedMinSize = 20ll + static_cast<int64>(OutRecordCount) * static_cast<int64>(OutRecordSize);
	return ExpectedMinSize <= OutData.Num();
}

bool ReadCharacterAppearanceDbcRecordUInt32(const TArray<uint8>& Data, uint32 RecordIndex, uint32 RecordSize, int32 FieldIndex, uint32& OutValue)
{
	const int32 Offset = 20 + static_cast<int32>(RecordIndex * RecordSize) + FieldIndex * 4;
	return ReadCharacterAppearanceDbcUInt32(Data, Offset, OutValue);
}

bool ReadCharacterAppearanceDbcFiles(const FString& DbcRoot, FCharacterAppearanceDbcCache& OutCache)
{
	OutCache = FCharacterAppearanceDbcCache();

	{
		const FString Path = DbcRoot / TEXT("ItemDisplayInfo.dbc");
		TArray<uint8> Data;
		uint32 RecordCount = 0;
		uint32 FieldCount = 0;
		uint32 RecordSize = 0;
		if (!ReadCharacterAppearanceWdbcHeader(Path, Data, RecordCount, FieldCount, RecordSize) || FieldCount != 25 || RecordSize != 100)
		{
			UE_LOG(LogTemp, Warning, TEXT("角色外观 ItemDisplayInfo DBC 读取失败: %s"), *Path);
			return false;
		}

		for (uint32 RecordIndex = 0; RecordIndex < RecordCount; ++RecordIndex)
		{
			uint32 Value = 0;
			FCharacterAppearanceDbcItemDisplay Record;
			ReadCharacterAppearanceDbcRecordUInt32(Data, RecordIndex, RecordSize, 0, Value);
			Record.DisplayId = static_cast<int32>(Value);
			ReadCharacterAppearanceDbcRecordUInt32(Data, RecordIndex, RecordSize, 7, Value);
			Record.GeosetGroups[0] = static_cast<int32>(Value);
			ReadCharacterAppearanceDbcRecordUInt32(Data, RecordIndex, RecordSize, 8, Value);
			Record.GeosetGroups[1] = static_cast<int32>(Value);
			ReadCharacterAppearanceDbcRecordUInt32(Data, RecordIndex, RecordSize, 9, Value);
			Record.GeosetGroups[2] = static_cast<int32>(Value);
			ReadCharacterAppearanceDbcRecordUInt32(Data, RecordIndex, RecordSize, 13, Value);
			Record.HelmetGeosetVisMale = static_cast<int32>(Value);
			ReadCharacterAppearanceDbcRecordUInt32(Data, RecordIndex, RecordSize, 14, Value);
			Record.HelmetGeosetVisFemale = static_cast<int32>(Value);
			if (Record.DisplayId > 0)
			{
				OutCache.ItemDisplayInfo.Add(Record.DisplayId, Record);
			}
		}
	}

	{
		const FString Path = DbcRoot / TEXT("helmetgeosetvisdata.dbc");
		TArray<uint8> Data;
		uint32 RecordCount = 0;
		uint32 FieldCount = 0;
		uint32 RecordSize = 0;
		if (!ReadCharacterAppearanceWdbcHeader(Path, Data, RecordCount, FieldCount, RecordSize) || FieldCount != 8 || RecordSize != 32)
		{
			UE_LOG(LogTemp, Warning, TEXT("角色外观 HelmetGeosetVisData DBC 读取失败: %s"), *Path);
			return false;
		}

		for (uint32 RecordIndex = 0; RecordIndex < RecordCount; ++RecordIndex)
		{
			uint32 Values[8] = {};
			for (int32 FieldIndex = 0; FieldIndex < 8; ++FieldIndex)
			{
				ReadCharacterAppearanceDbcRecordUInt32(Data, RecordIndex, RecordSize, FieldIndex, Values[FieldIndex]);
			}
			FCharacterAppearanceDbcHelmetVis Record;
			Record.Id = static_cast<int32>(Values[0]);
			Record.HairFlags = Values[1];
			Record.Facial1Flags = Values[2];
			Record.Facial2Flags = Values[3];
			Record.Facial3Flags = Values[4];
			Record.EarsFlags = Values[5];
			if (Record.Id > 0)
			{
				OutCache.HelmetGeosetVisData.Add(Record.Id, Record);
			}
		}
	}

	{
		const FString Path = DbcRoot / TEXT("CharHairGeosets.dbc");
		TArray<uint8> Data;
		uint32 RecordCount = 0;
		uint32 FieldCount = 0;
		uint32 RecordSize = 0;
		if (!ReadCharacterAppearanceWdbcHeader(Path, Data, RecordCount, FieldCount, RecordSize) || FieldCount != 6 || RecordSize != 24)
		{
			UE_LOG(LogTemp, Warning, TEXT("角色外观 CharHairGeosets DBC 读取失败: %s"), *Path);
			return false;
		}

		for (uint32 RecordIndex = 0; RecordIndex < RecordCount; ++RecordIndex)
		{
			uint32 Value = 0;
			FCharacterAppearanceDbcHairGeoset Record;
			ReadCharacterAppearanceDbcRecordUInt32(Data, RecordIndex, RecordSize, 1, Value);
			Record.Race = static_cast<int32>(Value);
			ReadCharacterAppearanceDbcRecordUInt32(Data, RecordIndex, RecordSize, 2, Value);
			Record.Gender = static_cast<int32>(Value);
			ReadCharacterAppearanceDbcRecordUInt32(Data, RecordIndex, RecordSize, 3, Value);
			Record.HairStyle = static_cast<int32>(Value);
			ReadCharacterAppearanceDbcRecordUInt32(Data, RecordIndex, RecordSize, 4, Value);
			Record.GeosetId = static_cast<int32>(Value);
			ReadCharacterAppearanceDbcRecordUInt32(Data, RecordIndex, RecordSize, 5, Value);
			Record.bBald = Value != 0;
			if (Record.Race > 0 && Record.Gender >= 0)
			{
				OutCache.HairGeosets.Add(Record);
			}
		}
	}

	{
		const FString Path = DbcRoot / TEXT("CharacterFacialHairStyles.dbc");
		TArray<uint8> Data;
		uint32 RecordCount = 0;
		uint32 FieldCount = 0;
		uint32 RecordSize = 0;
		if (!ReadCharacterAppearanceWdbcHeader(Path, Data, RecordCount, FieldCount, RecordSize) || FieldCount != 8 || RecordSize != 32)
		{
			UE_LOG(LogTemp, Warning, TEXT("角色外观 CharacterFacialHairStyles DBC 读取失败: %s"), *Path);
			return false;
		}

		for (uint32 RecordIndex = 0; RecordIndex < RecordCount; ++RecordIndex)
		{
			uint32 Values[8] = {};
			for (int32 FieldIndex = 0; FieldIndex < 8; ++FieldIndex)
			{
				ReadCharacterAppearanceDbcRecordUInt32(Data, RecordIndex, RecordSize, FieldIndex, Values[FieldIndex]);
			}
			FCharacterAppearanceDbcFacialHairStyle Record;
			Record.Race = static_cast<int32>(Values[0]);
			Record.Gender = static_cast<int32>(Values[1]);
			Record.Style = static_cast<int32>(Values[2]);
			Record.Geoset100 = static_cast<int32>(Values[3]);
			Record.Geoset300 = static_cast<int32>(Values[4]);
			Record.Geoset200 = static_cast<int32>(Values[5]);
			if (Record.Race > 0 && Record.Gender >= 0)
			{
				OutCache.FacialHairStyles.Add(Record);
			}
		}
	}

	OutCache.bLoaded = true;
	return true;
}

const FCharacterAppearanceDbcCache& GetCharacterAppearanceDbcCache()
{
	static FCharacterAppearanceDbcCache Cache;
	static bool bAttemptedLoad = false;
	if (!bAttemptedLoad)
	{
		bAttemptedLoad = true;
		const FString DbcRoot = FPaths::ProjectDir() / TEXT("Data/DBFilesClient");
		if (ReadCharacterAppearanceDbcFiles(DbcRoot, Cache))
		{
			UE_LOG(LogTemp, Display, TEXT("角色外观DBC已直接加载到内存: itemDisplay=%d helmetVis=%d hair=%d facialHair=%d source=%s"),
				Cache.ItemDisplayInfo.Num(),
				Cache.HelmetGeosetVisData.Num(),
				Cache.HairGeosets.Num(),
				Cache.FacialHairStyles.Num(),
				*DbcRoot);
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("角色外观DBC不存在或不可读: %s"),
				*DbcRoot);
		}
	}
	return Cache;
}

bool ShouldApplyItemTextureFieldToEquipment(const FString& Field, const FCharacterPreviewEquipmentDisplay& Item, TOptional<int32> RobeDisplayId)
{
	if (Field == TEXT("arm_upper") || Field == TEXT("arm_lower"))
	{
		return Item.Slot == 3 || Item.Slot == 4 || Item.Slot == 8 || Item.Slot == 9 ||
			Item.InventoryType == 4 || Item.InventoryType == 5 || Item.InventoryType == 9 || Item.InventoryType == 10 || Item.InventoryType == 20;
	}
	if (Field == TEXT("hand"))
	{
		return Item.Slot == 9 || Item.InventoryType == 10;
	}
	if (Field == TEXT("torso_upper") || Field == TEXT("torso_lower"))
	{
		return Item.Slot == 3 || Item.Slot == 4 || Item.InventoryType == 4 || Item.InventoryType == 5 || Item.InventoryType == 20;
	}
	if (Field == TEXT("leg_upper"))
	{
		if (RobeDisplayId.IsSet() && Item.DisplayId != RobeDisplayId.GetValue() && Item.InventoryType != 6)
		{
			return false;
		}
		return Item.Slot == 4 || Item.Slot == 5 || Item.Slot == 6 || Item.InventoryType == 6 || Item.InventoryType == 7 || Item.InventoryType == 20;
	}
	if (Field == TEXT("leg_lower"))
	{
		if (RobeDisplayId.IsSet() && Item.DisplayId != RobeDisplayId.GetValue() && Item.InventoryType != 6 && Item.InventoryType != 8)
		{
			return false;
		}
		return Item.Slot == 4 || Item.Slot == 5 || Item.Slot == 6 || Item.Slot == 7 ||
			Item.InventoryType == 6 || Item.InventoryType == 7 || Item.InventoryType == 8 || Item.InventoryType == 20;
	}
	if (Field == TEXT("foot"))
	{
		return Item.Slot == 7 || Item.InventoryType == 8;
	}
	return false;
}

FString BuildRuntimeBodyAtlasSignature(const AWoWLoginGameMode::FCharacterSelectPreviewAppearance& Appearance)
{
	return FString::Printf(
		TEXT("race=%d|gender=%d|skin=%d|face=%d|hair=%d|hairColor=%d|facial=%d|equipment=%s"),
		Appearance.Race,
		Appearance.Gender,
		Appearance.Skin,
		Appearance.Face,
		Appearance.HairStyle,
		Appearance.HairColor,
		Appearance.FacialStyle,
		*Appearance.EquipmentSummary);
}

UTexture2D* FindCachedRuntimeCharacterBodyAtlas(const AWoWLoginGameMode::FCharacterSelectPreviewAppearance& Appearance, int32& OutAppliedLayers, int32& OutMissingLayers)
{
	OutAppliedLayers = 0;
	OutMissingLayers = 0;
	FWoWRuntimeCharacterAppearanceDescriptor RuntimeDescriptor;
	RuntimeDescriptor.Guid = Appearance.Guid;
	RuntimeDescriptor.Race = Appearance.Race;
	RuntimeDescriptor.Gender = Appearance.Gender;
	RuntimeDescriptor.Skin = Appearance.Skin;
	RuntimeDescriptor.Face = Appearance.Face;
	RuntimeDescriptor.HairStyle = Appearance.HairStyle;
	RuntimeDescriptor.HairColor = Appearance.HairColor;
	RuntimeDescriptor.FacialStyle = Appearance.FacialStyle;
	RuntimeDescriptor.EquipmentSummary = Appearance.EquipmentSummary;
	UTexture2D* BodyAtlas = WoWRuntimeCharacterAppearance::ComposeBodyAtlas(RuntimeDescriptor, OutAppliedLayers, OutMissingLayers);
	if (!BodyAtlas)
	{
		++OutMissingLayers;
	}
	return BodyAtlas;
}

bool HasRuntimeCharacterAppearanceTextureSupport(const AWoWLoginGameMode::FCharacterSelectPreviewAppearance& Appearance)
{
	FWoWRuntimeCharacterAppearanceDescriptor RuntimeDescriptor;
	RuntimeDescriptor.Guid = Appearance.Guid;
	RuntimeDescriptor.Race = Appearance.Race;
	RuntimeDescriptor.Gender = Appearance.Gender;
	RuntimeDescriptor.Skin = Appearance.Skin;
	RuntimeDescriptor.Face = Appearance.Face;
	RuntimeDescriptor.HairStyle = Appearance.HairStyle;
	RuntimeDescriptor.HairColor = Appearance.HairColor;
	RuntimeDescriptor.FacialStyle = Appearance.FacialStyle;
	RuntimeDescriptor.EquipmentSummary = Appearance.EquipmentSummary;
	return WoWRuntimeCharacterAppearance::HasTextureSupport(RuntimeDescriptor);
}

int32 ResolveSelectedHairGeosetId(const FCharacterAppearanceDbcCache& Cache, int32 Race, int32 Gender, int32 HairStyle)
{
	for (const FCharacterAppearanceDbcHairGeoset& Record : Cache.HairGeosets)
	{
		if (Record.Race == Race &&
			Record.Gender == Gender &&
			Record.HairStyle == HairStyle)
		{
			return Record.bBald ? 0 : Record.GeosetId;
		}
	}
	return 0;
}

const FCharacterAppearanceDbcFacialHairStyle* ResolveSelectedFacialHairStyle(
	const FCharacterAppearanceDbcCache& Cache,
	int32 Race,
	int32 Gender,
	int32 FacialStyle)
{
	for (const FCharacterAppearanceDbcFacialHairStyle& Record : Cache.FacialHairStyles)
	{
		if (Record.Race == Race &&
			Record.Gender == Gender &&
			Record.Style == FacialStyle)
		{
			return &Record;
		}
	}
	return nullptr;
}

bool ShouldHideHairForEquippedHelmet(const FCharacterAppearanceDbcCache& Cache, const FString& EquipmentSummary, int32 Gender, int32& OutHelmetDisplayId, int32& OutHelmetVisId)
{
	OutHelmetDisplayId = GetEquipmentDisplayIdForSlot(EquipmentSummary, 0);
	OutHelmetVisId = 0;
	if (OutHelmetDisplayId <= 0)
	{
		return false;
	}

	const FCharacterAppearanceDbcItemDisplay* DisplayRecord = Cache.ItemDisplayInfo.Find(OutHelmetDisplayId);
	if (!DisplayRecord)
	{
		return false;
	}

	OutHelmetVisId = Gender == 1 ? DisplayRecord->HelmetGeosetVisFemale : DisplayRecord->HelmetGeosetVisMale;
	const FCharacterAppearanceDbcHelmetVis* HelmetVisRecord = Cache.HelmetGeosetVisData.Find(OutHelmetVisId);
	return HelmetVisRecord && HelmetVisRecord->HairFlags != 0;
}

bool DoHelmetGeosetFlagsHideRace(uint32 Flags, int32 RaceId)
{
	if (Flags == 0 || RaceId <= 0 || RaceId > 32)
	{
		return false;
	}

	return (Flags & (1u << (RaceId - 1))) != 0;
}

const FCharacterAppearanceDbcHelmetVis* ResolveHelmetVisForEquippedHelmet(
	const FCharacterAppearanceDbcCache& Cache,
	const FString& EquipmentSummary,
	int32 Gender,
	int32& OutHelmetDisplayId,
	int32& OutHelmetVisId)
{
	OutHelmetDisplayId = GetEquipmentDisplayIdForSlot(EquipmentSummary, 0);
	OutHelmetVisId = 0;
	if (OutHelmetDisplayId <= 0)
	{
		return nullptr;
	}

	const FCharacterAppearanceDbcItemDisplay* DisplayRecord = Cache.ItemDisplayInfo.Find(OutHelmetDisplayId);
	if (!DisplayRecord)
	{
		return nullptr;
	}

	OutHelmetVisId = Gender == 1 ? DisplayRecord->HelmetGeosetVisFemale : DisplayRecord->HelmetGeosetVisMale;
	return Cache.HelmetGeosetVisData.Find(OutHelmetVisId);
}

FString BuildGeneratedCharacterPreviewStem(const FString& SourceModelPath, int32 Guid)
{
	const FString ModelStem = SanitizePackageSegment(FPaths::GetBaseFilename(SourceModelPath));
	return FString::Printf(TEXT("%s_%d"), *ModelStem, Guid);
}

uint64 UpdateFnv1a64(uint64 Hash, const FString& Value)
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

FString BuildCharacterAppearanceSignature(
	int32 Race,
	int32 Gender,
	int32 Skin,
	int32 Face,
	int32 HairStyle,
	int32 HairColor,
	int32 FacialStyle,
	const FString& EquipmentSummary)
{
	const FString Key = FString::Printf(
		TEXT("race=%d|gender=%d|skin=%d|face=%d|hairStyle=%d|hairColor=%d|facial=%d|equipment=%s"),
		Race,
		Gender,
		Skin,
		Face,
		HairStyle,
		HairColor,
		FacialStyle,
		*EquipmentSummary);
	const uint64 Hash = UpdateFnv1a64(0xCBF29CE484222325ull, Key);
	return FString::Printf(TEXT("%016llx"), static_cast<unsigned long long>(Hash));
}

FString BuildGeneratedCharacterPreviewStemForSignature(const FString& SourceModelPath, const FString& GeneratedAssetStem, const FString& Signature)
{
	const FString Base = !GeneratedAssetStem.IsEmpty()
		? GeneratedAssetStem
		: SanitizePackageSegment(FPaths::GetBaseFilename(SourceModelPath));
	return FString::Printf(TEXT("%s_%s"), *SanitizePackageSegment(Base), *Signature);
}

FString BuildGeneratedCharacterPreviewAnimationStem(const FString& SourceModelPath)
{
	FString ModelStem = SanitizePackageSegment(FPaths::GetBaseFilename(SourceModelPath));
	ModelStem.ToLowerInline();
	return ModelStem;
}

FWoWCharacterVisualBaseDefinition MakeCharacterVisualBaseDefinition(const AWoWLoginGameMode::FPlayerCharacterPreviewDefinition& Definition)
{
	FWoWCharacterVisualBaseDefinition VisualDefinition;
	VisualDefinition.Id = Definition.Id;
	VisualDefinition.SourceModelPath = Definition.SourceModelPath;
	VisualDefinition.GeneratedAssetStem = Definition.GeneratedAssetStem;
	VisualDefinition.MeshJsonPath = Definition.MeshJsonPath;
	VisualDefinition.MeshPath = Definition.MeshPath;
	VisualDefinition.StandAnimationPath = Definition.StandAnimationPath;
	VisualDefinition.LookupAnimationPath = Definition.LookupAnimationPath;
	VisualDefinition.AnimationPackagePath = Definition.AnimationPackagePath;
	VisualDefinition.MaterialPackagePath = Definition.MaterialPackagePath;
	VisualDefinition.SectionMetadataPath = Definition.SectionMetadataPath;
	return VisualDefinition;
}

FString CharacterPreviewBuildStatusToString(AWoWLoginGameMode::ECharacterPreviewBuildStatus Status)
{
	switch (Status)
	{
	case AWoWLoginGameMode::ECharacterPreviewBuildStatus::Complete:
		return TEXT("Complete");
	case AWoWLoginGameMode::ECharacterPreviewBuildStatus::NeedsRebuild:
		return TEXT("NeedsRebuild");
	case AWoWLoginGameMode::ECharacterPreviewBuildStatus::MissingCriticalSource:
		return TEXT("MissingCriticalSource");
	case AWoWLoginGameMode::ECharacterPreviewBuildStatus::FailedPreviously:
		return TEXT("FailedPreviously");
	default:
		return TEXT("Unknown");
	}
}

FString BuildPreviewMetadataPath(const FString& ModelPath, const FString& GeneratedAssetStem, const FString& Extension)
{
	FString Normalized = ModelPath;
	Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
	TArray<FString> Parts;
	Normalized.ParseIntoArray(Parts, TEXT("/"), true);
	if (Parts.IsEmpty())
	{
		return FString();
	}

	const FString Stem = !GeneratedAssetStem.IsEmpty()
		? GeneratedAssetStem
		: FPaths::GetBaseFilename(Parts.Last());
	Parts.Last() = Stem + Extension;

	FString RelativePath = TEXT("M2Preview");
	for (const FString& Part : Parts)
	{
		RelativePath /= Part;
	}
	return FPaths::ProjectSavedDir() / RelativePath;
}

constexpr int32 PreviewHairGeosetGroup = 0;
constexpr int32 PreviewStandardGeosetGroupCount = 20;

bool TryResolvePreviewSubmeshGroupValue(int32 SubmeshId, int32& OutGroup, int32& OutValue)
{
	if (SubmeshId <= 0)
	{
		return false;
	}

	if (SubmeshId < 100)
	{
		OutGroup = PreviewHairGeosetGroup;
		OutValue = SubmeshId;
		return true;
	}

	// WoW/WMV character geosets are one-based inside each hundred range:
	// group 1 owns 101..199, group 4 owns 401..499, and so on.
	OutGroup = SubmeshId / 100;
	OutValue = SubmeshId - OutGroup * 100;
	return OutValue > 0;
}

bool IsPreviewSubmeshVisible(int32 SubmeshId, const TMap<int32, int32>& GeosetGroups)
{
	if (SubmeshId == 0)
	{
		return true;
	}

	int32 Group = 0;
	int32 Value = 0;
	if (!TryResolvePreviewSubmeshGroupValue(SubmeshId, Group, Value))
	{
		return false;
	}

	if (const int32* SelectedValue = GeosetGroups.Find(Group))
	{
		return *SelectedValue > 0 && Value == *SelectedValue;
	}

	return Group >= PreviewStandardGeosetGroupCount;
}

bool IsPreviewSectionVisible(int32 SubmeshId, int32 TextureType, const TMap<int32, int32>& GeosetGroups)
{
	if (SubmeshId == 0)
	{
		return true;
	}
	return IsPreviewSubmeshVisible(SubmeshId, GeosetGroups);
}

bool IsPreviewFaceSectionSubmesh(int32 SubmeshId, int32 TextureType, const FString& PreviewPng)
{
	int32 Group = 0;
	int32 Value = 0;
	if (!TryResolvePreviewSubmeshGroupValue(SubmeshId, Group, Value) || Group != 15 || Value <= 0)
	{
		return false;
	}
	if (TextureType != 2)
	{
		return false;
	}

	const FString BaseName = FPaths::GetBaseFilename(PreviewPng).ToLower();
	return BaseName.Contains(TEXT("faceupper")) || BaseName.Contains(TEXT("facelower"));
}

bool TryReadJsonVector3Field(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FVector& OutValue)
{
	if (!Object.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(FieldName, Values) || !Values || Values->Num() < 3)
	{
		return false;
	}

	OutValue.X = static_cast<float>((*Values)[0]->AsNumber());
	OutValue.Y = static_cast<float>((*Values)[1]->AsNumber());
	OutValue.Z = static_cast<float>((*Values)[2]->AsNumber());
	return true;
}

bool IsHelmetHiddenHeadOverlaySubmesh(
	int32 SubmeshId,
	int32 TextureType,
	const FVector& BoundsMin,
	const FVector& BoundsMax,
	const FVector& MeshBoundsMin,
	const FVector& MeshBoundsMax)
{
	int32 Group = 0;
	int32 Value = 0;
	if (!TryResolvePreviewSubmeshGroupValue(SubmeshId, Group, Value) || Group != 15 || Value <= 0)
	{
		return false;
	}
	const bool bLooksLikeHeadAttachmentTexture = TextureType == 2 || TextureType == 6 || TextureType == 8;
	if (!bLooksLikeHeadAttachmentTexture)
	{
		return false;
	}

	const float Height = BoundsMax.Z - BoundsMin.Z;
	const float WidthX = BoundsMax.X - BoundsMin.X;
	const float WidthY = BoundsMax.Y - BoundsMin.Y;
	const float MeshHeight = MeshBoundsMax.Z - MeshBoundsMin.Z;
	const float HeadRangeMinZ = MeshHeight > KINDA_SMALL_NUMBER
		? MeshBoundsMin.Z + MeshHeight * 0.64f
		: 150.0f;
	const bool bLivesInHeadRange = BoundsMax.Z >= HeadRangeMinZ && BoundsMin.Z >= HeadRangeMinZ - FMath::Max(MeshHeight * 0.12f, 25.0f);
	const bool bCompactEnoughForHeadOverlay = Height <= 55.0f && WidthX <= 65.0f && WidthY <= 65.0f;
	return bLivesInHeadRange && bCompactEnoughForHeadOverlay;
}

void OverridePreviewGeosetGroupFromSubmesh(int32 SubmeshId, TMap<int32, int32>& InOutGeosetGroups)
{
	int32 Group = 0;
	int32 Value = 0;
	if (!TryResolvePreviewSubmeshGroupValue(SubmeshId, Group, Value))
	{
		return;
	}

	InOutGeosetGroups.Add(Group, Value);
}

void AddLowestAvailablePreviewGeosets(const TSet<int32>& AvailableSubmeshIds, TMap<int32, int32>& InOutGeosetGroups)
{
	for (const int32 SubmeshId : AvailableSubmeshIds)
	{
		if (SubmeshId == 0)
		{
			continue;
		}

		int32 Group = 0;
		int32 Value = 0;
		if (!TryResolvePreviewSubmeshGroupValue(SubmeshId, Group, Value))
		{
			continue;
		}
		int32& SelectedValue = InOutGeosetGroups.FindOrAdd(Group, Value);
		SelectedValue = FMath::Min(SelectedValue, Value);
	}
}

enum class ECharacterPreviewGeosetGroup : int32
{
	HairStyle = 0,
	Facial1 = 1,
	Facial2 = 2,
	Facial3 = 3,
	Gloves = 4,
	Boots = 5,
	Ears = 7,
	Wristbands = 8,
	Kneepads = 9,
	Pants = 10,
	Pants2 = 11,
	Tabard = 12,
	Trousers = 13,
	Tabard2 = 14,
	Cape = 15,
	EyeGlow = 17,
	Belt = 18,
	Tail = 19,
};

void SetPreviewGeosetGroup(TMap<int32, int32>& InOutGeosetGroups, ECharacterPreviewGeosetGroup Group, int32 Value)
{
	InOutGeosetGroups.Add(static_cast<int32>(Group), Value);
}

bool PreviewGeosetGroupHasSection(const TSet<int32>& AvailableSubmeshIds, ECharacterPreviewGeosetGroup Group, int32 Value)
{
	if (Value <= 0)
	{
		return true;
	}

	const int32 WantedGroup = static_cast<int32>(Group);
	for (const int32 SubmeshId : AvailableSubmeshIds)
	{
		int32 CandidateGroup = 0;
		int32 CandidateValue = 0;
		if (TryResolvePreviewSubmeshGroupValue(SubmeshId, CandidateGroup, CandidateValue) &&
			CandidateGroup == WantedGroup &&
			CandidateValue == Value)
		{
			return true;
		}
	}
	return false;
}

bool PreviewGeosetGroupHasValue(const TSet<int32>& AvailableSubmeshIds, ECharacterPreviewGeosetGroup Group, int32 Value)
{
	const int32 WantedSubmeshId = static_cast<int32>(Group) * 100 + Value;
	return Value > 0 && AvailableSubmeshIds.Contains(WantedSubmeshId);
}

struct FCharacterPreviewSectionGeosetInfo
{
	int32 SubmeshId = 0;
	int32 TextureType = INDEX_NONE;
	int32 RenderMode = INDEX_NONE;
	int32 MaterialFlags = 0;
	bool bUnlit = false;
	bool bNoDepthWrite = false;
	FString PreviewPng;
};

bool PreviewEyeGlowCandidateHasOrdinaryTextureType(
	const TArray<FCharacterPreviewSectionGeosetInfo>& Sections,
	const TSet<int32>& AvailableSubmeshIds,
	int32 CandidateValue)
{
	const int32 EyeGlowGroup = static_cast<int32>(ECharacterPreviewGeosetGroup::EyeGlow);
	const int32 WantedSubmeshId = EyeGlowGroup * 100 + CandidateValue;
	if (CandidateValue <= 0 || !AvailableSubmeshIds.Contains(WantedSubmeshId))
	{
		return false;
	}

	for (const FCharacterPreviewSectionGeosetInfo& Section : Sections)
	{
		if (Section.SubmeshId != WantedSubmeshId)
		{
			continue;
		}

		if (Section.TextureType == 6 || Section.TextureType == 8)
		{
			return true;
		}
	}
	return false;
}

bool PreviewEyeGlowCandidateExists(
	const TSet<int32>& AvailableSubmeshIds,
	int32 CandidateValue)
{
	const int32 EyeGlowGroup = static_cast<int32>(ECharacterPreviewGeosetGroup::EyeGlow);
	return CandidateValue > 0 && AvailableSubmeshIds.Contains(EyeGlowGroup * 100 + CandidateValue);
}

int32 ResolveCharacterEyeGlowGeosetValue(
	const TSet<int32>& AvailableSubmeshIds,
	const TArray<FCharacterPreviewSectionGeosetInfo>& Sections,
	int32 ClassId)
{
	// Group 17 is the M2/SKIN eye-glow group. Imported HD models differ in whether
	// the ordinary eye pass is tagged like hair (6) or fur/skin overlay (8), so use
	// the exported section metadata instead of treating 1702/1703 as universal.
	if (ClassId == 6)
	{
		for (const int32 Candidate : { 3, 2, 1 })
		{
			if (PreviewGeosetGroupHasValue(AvailableSubmeshIds, ECharacterPreviewGeosetGroup::EyeGlow, Candidate))
			{
				UE_LOG(LogTemp, Verbose, TEXT("角色预览 EyeGlow geoset 选择: class=%d value=%d reason=death_knight_prefer_highest"), ClassId, Candidate);
				return Candidate;
			}
		}
		return 0;
	}

	for (const int32 Candidate : { 2, 1 })
	{
		if (PreviewEyeGlowCandidateExists(AvailableSubmeshIds, Candidate))
		{
			UE_LOG(LogTemp, Verbose, TEXT("角色预览 EyeGlow geoset 选择: class=%d value=%d reason=non_dk_standard_eye_value"), ClassId, Candidate);
			return Candidate;
		}
	}
	for (const int32 Candidate : { 3 })
	{
		if (PreviewEyeGlowCandidateHasOrdinaryTextureType(Sections, AvailableSubmeshIds, Candidate))
		{
			UE_LOG(LogTemp, Verbose, TEXT("角色预览 EyeGlow geoset 选择: class=%d value=%d reason=ordinary_eye_texture_type"), ClassId, Candidate);
			return Candidate;
		}
	}
	for (const int32 Candidate : { 3, 2, 1 })
	{
		if (PreviewEyeGlowCandidateExists(AvailableSubmeshIds, Candidate))
		{
			UE_LOG(LogTemp, Display, TEXT("角色预览 EyeGlow geoset 跳过: class=%d value=%d reason=non_dk_no_standard_or_ordinary_eye"), ClassId, Candidate);
		}
	}
	return 0;
}

void BuildWmvDefaultPreviewGeosets(const TSet<int32>& AvailableSubmeshIds, TMap<int32, int32>& OutGeosetGroups)
{
	OutGeosetGroups.Reset();
	for (const int32 SubmeshId : AvailableSubmeshIds)
	{
		int32 Group = 0;
		int32 Value = 0;
		if (TryResolvePreviewSubmeshGroupValue(SubmeshId, Group, Value) && Group > 0 && Group < PreviewStandardGeosetGroupCount)
		{
			OutGeosetGroups.FindOrAdd(Group, 1);
		}
	}

	SetPreviewGeosetGroup(OutGeosetGroups, ECharacterPreviewGeosetGroup::HairStyle, 0);
	SetPreviewGeosetGroup(OutGeosetGroups, ECharacterPreviewGeosetGroup::Facial1, 0);
	SetPreviewGeosetGroup(OutGeosetGroups, ECharacterPreviewGeosetGroup::Facial2, 0);
	SetPreviewGeosetGroup(OutGeosetGroups, ECharacterPreviewGeosetGroup::Facial3, 0);
	SetPreviewGeosetGroup(OutGeosetGroups, ECharacterPreviewGeosetGroup::Ears, 2);
	SetPreviewGeosetGroup(OutGeosetGroups, ECharacterPreviewGeosetGroup::EyeGlow, 0);
}

void ApplyWmvEquipmentGeosetRules(
	const FCharacterAppearanceDbcCache& Cache,
	const FString& EquipmentSummary,
	const TSet<int32>& AvailableSubmeshIds,
	TMap<int32, int32>& InOutGeosetGroups)
{
	const TArray<FCharacterPreviewEquipmentDisplay> Equipment = ParseCharacterPreviewEquipmentSummary(EquipmentSummary);
	auto FindEquipmentBySlot = [&Equipment](int32 Slot) -> const FCharacterPreviewEquipmentDisplay*
	{
		return Equipment.FindByPredicate([Slot](const FCharacterPreviewEquipmentDisplay& Entry)
		{
			return Entry.Slot == Slot && Entry.DisplayId > 0;
		});
	};
	auto FindDisplay = [&Cache](int32 DisplayId) -> const FCharacterAppearanceDbcItemDisplay*
	{
		return DisplayId > 0 ? Cache.ItemDisplayInfo.Find(DisplayId) : nullptr;
	};
	bool bHadRobe = false;
	for (const int32 Slot : {4, 6})
	{
		const FCharacterPreviewEquipmentDisplay* EquipmentEntry = FindEquipmentBySlot(Slot);
		const FCharacterAppearanceDbcItemDisplay* Display = EquipmentEntry ? FindDisplay(EquipmentEntry->DisplayId) : nullptr;
		if (Display && (EquipmentEntry->InventoryType == 20 || Display->GeosetGroups[2] == 1))
		{
			bHadRobe = true;
			break;
		}
	}
	auto ApplyEquipmentSlot = [&](int32 Slot)
	{
		const FCharacterPreviewEquipmentDisplay* EquipmentEntry = FindEquipmentBySlot(Slot);
		const FCharacterAppearanceDbcItemDisplay* Display = EquipmentEntry ? FindDisplay(EquipmentEntry->DisplayId) : nullptr;
		if (!Display)
		{
			return;
		}

		switch (Slot)
		{
		case 3: // shirt
		case 4: // chest/robe
			SetPreviewGeosetGroup(InOutGeosetGroups, ECharacterPreviewGeosetGroup::Wristbands, 1 + Display->GeosetGroups[0]);
			break;
		case 6: // pants
			if (const int32* Trousers = InOutGeosetGroups.Find(static_cast<int32>(ECharacterPreviewGeosetGroup::Trousers)); Trousers && *Trousers == 2)
			{
				return;
			}
			SetPreviewGeosetGroup(InOutGeosetGroups, ECharacterPreviewGeosetGroup::Kneepads, 1 + Display->GeosetGroups[1]);
			break;
		case 7: // boots
			SetPreviewGeosetGroup(InOutGeosetGroups, ECharacterPreviewGeosetGroup::Boots, 1 + Display->GeosetGroups[0]);
			break;
		case 9: // gloves
			SetPreviewGeosetGroup(InOutGeosetGroups, ECharacterPreviewGeosetGroup::Gloves, 1 + Display->GeosetGroups[0]);
			break;
		case 14: // cape
			SetPreviewGeosetGroup(InOutGeosetGroups, ECharacterPreviewGeosetGroup::Cape, 1 + Display->GeosetGroups[0]);
			break;
		case 18: // tabard
			SetPreviewGeosetGroup(InOutGeosetGroups, ECharacterPreviewGeosetGroup::Tabard, 2);
			break;
		default:
			break;
		}

		if (const int32* Trousers = InOutGeosetGroups.Find(static_cast<int32>(ECharacterPreviewGeosetGroup::Trousers)); Trousers && *Trousers == 1)
		{
			SetPreviewGeosetGroup(InOutGeosetGroups, ECharacterPreviewGeosetGroup::Trousers, 1 + Display->GeosetGroups[2]);
		}
		if (const int32* Trousers = InOutGeosetGroups.Find(static_cast<int32>(ECharacterPreviewGeosetGroup::Trousers)); Trousers && *Trousers == 2)
		{
			SetPreviewGeosetGroup(InOutGeosetGroups, ECharacterPreviewGeosetGroup::Boots, 0);
			SetPreviewGeosetGroup(InOutGeosetGroups, ECharacterPreviewGeosetGroup::Tabard, 0);
		}
		if (const int32* Gloves = InOutGeosetGroups.Find(static_cast<int32>(ECharacterPreviewGeosetGroup::Gloves)); Gloves && *Gloves > 1)
		{
			SetPreviewGeosetGroup(InOutGeosetGroups, ECharacterPreviewGeosetGroup::Wristbands, 0);
		}
	};

	const int32 SlotOrder[] = {3, 0, 1, 2, 6, 7, 4, 18, 5, 8, 9, 15, 16, 14, 17};
	const int32 SlotOrderWithRobe[] = {3, 0, 1, 2, 6, 7, 5, 4, 18, 8, 9, 15, 16, 14, 17};
	const int32* EffectiveSlotOrder = bHadRobe ? SlotOrderWithRobe : SlotOrder;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(SlotOrder); ++Index)
	{
		ApplyEquipmentSlot(EffectiveSlotOrder[Index]);
	}

	for (auto It = InOutGeosetGroups.CreateIterator(); It; ++It)
	{
		if (It.Value() > 0 && !PreviewGeosetGroupHasSection(AvailableSubmeshIds, static_cast<ECharacterPreviewGeosetGroup>(It.Key()), It.Value()))
		{
			It.Value() = 0;
		}
	}
}

void ConfigureCharacterSelectPreviewSkeletalComponent(USkeletalMeshComponent* MeshComponent)
{
	if (!MeshComponent)
	{
		return;
	}

	MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MeshComponent->SetCastShadow(false);
	MeshComponent->SetEnableAnimation(true);
	MeshComponent->SetComponentTickEnabled(true);
	MeshComponent->PrimaryComponentTick.SetTickFunctionEnable(true);
	MeshComponent->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
	MeshComponent->bPauseAnims = false;
	MeshComponent->GlobalAnimRateScale = 1.0f;
}

bool PlayCharacterSelectPreviewAnimation(
	USkeletalMeshComponent* MeshComponent,
	UAnimSequence* PreviewAnimation,
	const FString& DefinitionId,
	const FString& AnimationObjectPath,
	const FString& MeshObjectPath,
	bool bForceImmediatePoseRefresh = true,
	bool bLogPlayback = true)
{
	if (!MeshComponent || !PreviewAnimation)
	{
		return false;
	}

	USkeletalMesh* PreviewMesh = MeshComponent->GetSkeletalMeshAsset();
	const FString EffectiveMeshObjectPath = !MeshObjectPath.IsEmpty() && MeshObjectPath.StartsWith(TEXT("/Game/"))
		? MeshObjectPath
		: (PreviewMesh ? PreviewMesh->GetPathName() : MeshObjectPath);
	if (!IsCharacterPreviewAnimationCompatibleWithMesh(PreviewMesh, PreviewAnimation, EffectiveMeshObjectPath, AnimationObjectPath, DefinitionId))
	{
		return false;
	}

	MeshComponent->PlayAnimation(PreviewAnimation, true);
	if (UAnimSingleNodeInstance* SingleNodeInstance = MeshComponent->GetSingleNodeInstance())
	{
		SingleNodeInstance->SetLooping(true);
		SingleNodeInstance->SetPlaying(true);
		SingleNodeInstance->SetPlayRate(1.0f);
		SingleNodeInstance->SetPosition(0.0f, false);
	}
	if (bForceImmediatePoseRefresh)
	{
		MeshComponent->TickAnimation(0.0f, false);
		MeshComponent->RefreshBoneTransforms();
	}

	if (bLogPlayback)
	{
		const USkeleton* MeshSkeleton = PreviewMesh ? PreviewMesh->GetSkeleton() : nullptr;
		const USkeleton* AnimSkeleton = PreviewAnimation->GetSkeleton();
		UE_LOG(LogTemp, Display, TEXT("角色预览动画已播放: id=%s meshSkeleton=%s animSkeleton=%s frames=%d length=%.3f anim=%s mesh=%s"),
			*DefinitionId,
			MeshSkeleton ? *MeshSkeleton->GetPathName() : TEXT("<null>"),
			AnimSkeleton ? *AnimSkeleton->GetPathName() : TEXT("<null>"),
			PreviewAnimation->GetNumberOfSampledKeys(),
			PreviewAnimation->GetPlayLength(),
			*AnimationObjectPath,
			*MeshObjectPath);
	}
	return true;
}

FString MakeGeneratedModelRoot(const FString& ModelName)
{
	return TEXT("/Game/WoW/Generated/") + SanitizePackageSegment(ModelName);
}

FString MakeGeneratedMeshPackage(const FString& ModelName)
{
	const FString Root = MakeGeneratedModelRoot(ModelName);
	return Root / (SanitizePackageSegment(ModelName) + TEXT("_M2Mesh"));
}

FString MakeGeneratedAnimationPackage(const FString& ModelName)
{
	const FString Root = MakeGeneratedModelRoot(ModelName);
	return Root / TEXT("Animations") / (TEXT("A_") + SanitizePackageSegment(ModelName) + TEXT("_000_Stand_00"));
}

FString MakeGeneratedTuningPackage(const FString& ModelName)
{
	const FString Root = MakeGeneratedModelRoot(ModelName);
	return Root / (TEXT("DA_") + SanitizePackageSegment(ModelName) + TEXT("_Tuning"));
}

FString MakeGeneratedPresetPackage(const FString& ModelName)
{
	const FString Root = MakeGeneratedModelRoot(ModelName);
	return Root / (TEXT("DA_") + SanitizePackageSegment(ModelName) + TEXT("_LoginScenePreset"));
}

FString MakeGeneratedMaterialPackagePath(const FString& ModelName)
{
	return MakeGeneratedModelRoot(ModelName) / TEXT("Materials");
}

FString MakeGeneratedInterfaceGlueModelRoot(const FString& ModelName)
{
	return TEXT("/Game/WoW/Generated/Interface/GLUES/MODELS/") + SanitizePackageSegment(ModelName);
}

FString MakeBaseModelNameFromMeshJsonModelName(FString ModelName)
{
	const int32 SkinSuffixIndex = ModelName.Find(TEXT("_"), ESearchCase::IgnoreCase, ESearchDir::FromEnd);
	if (SkinSuffixIndex != INDEX_NONE && ModelName.Mid(SkinSuffixIndex).EndsWith(TEXT("skin"), ESearchCase::IgnoreCase))
	{
		ModelName.LeftInline(SkinSuffixIndex);
	}
	return ModelName;
}

FString MakeGeneratedInterfaceGlueModelRootForAsset(const FString& ModelName)
{
	return TEXT("/Game/WoW/Generated/Interface/GLUES/MODELS/") + SanitizePackageSegment(MakeBaseModelNameFromMeshJsonModelName(ModelName));
}

FString MakeGeneratedInterfaceGlueModelMeshPackage(const FString& ModelName)
{
	const FString Root = MakeGeneratedInterfaceGlueModelRootForAsset(ModelName);
	return Root / (SanitizePackageSegment(ModelName) + TEXT("_M2Mesh"));
}

FString MakeGeneratedInterfaceGlueModelAnimationPackage(const FString& ModelName)
{
	const FString Root = MakeGeneratedInterfaceGlueModelRootForAsset(ModelName);
	return Root / TEXT("Animations") / (TEXT("A_") + SanitizePackageSegment(ModelName) + TEXT("_000_Stand_00"));
}

FString MakeGeneratedInterfaceGlueModelTuningPackage(const FString& ModelName)
{
	const FString Root = MakeGeneratedInterfaceGlueModelRootForAsset(ModelName);
	return Root / (TEXT("DA_") + SanitizePackageSegment(ModelName) + TEXT("_Tuning"));
}

FString MakeGeneratedInterfaceGlueModelPresetPackage(const FString& ModelName)
{
	const FString Root = MakeGeneratedInterfaceGlueModelRootForAsset(ModelName);
	return Root / (TEXT("DA_") + SanitizePackageSegment(ModelName) + TEXT("_LoginScenePreset"));
}

FString MakeGeneratedInterfaceGlueModelMaterialPackagePath(const FString& ModelName)
{
	return MakeGeneratedInterfaceGlueModelRootForAsset(ModelName) / (SanitizePackageSegment(ModelName) + TEXT("_Materials"));
}

FString MakeJsonRelativePathFromFullPath(const FString& FullJsonPath)
{
	FString Normalized = FPaths::ConvertRelativePathToFull(FullJsonPath);
	Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));

	const FString Anchor = TEXT("/Saved/M2Preview/");
	int32 AnchorIndex = INDEX_NONE;
	AnchorIndex = Normalized.Find(Anchor, ESearchCase::IgnoreCase);
	if (AnchorIndex != INDEX_NONE)
	{
		return FString(TEXT("M2Preview/")) + Normalized.Mid(AnchorIndex + Anchor.Len());
	}

	const FString AnchorWithoutLeadingSlash = TEXT("Saved/M2Preview/");
	AnchorIndex = Normalized.Find(AnchorWithoutLeadingSlash, ESearchCase::IgnoreCase);
	if (AnchorIndex != INDEX_NONE)
	{
		return FString(TEXT("M2Preview/")) + Normalized.Mid(AnchorIndex + AnchorWithoutLeadingSlash.Len());
	}

	FString PreviewRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("M2Preview"));
	PreviewRoot.ReplaceInline(TEXT("\\"), TEXT("/"));
	PreviewRoot.RemoveFromEnd(TEXT("/"));
	if (Normalized.StartsWith(PreviewRoot + TEXT("/"), ESearchCase::IgnoreCase))
	{
		return FString(TEXT("M2Preview/")) + Normalized.Mid(PreviewRoot.Len() + 1);
	}

	return FullJsonPath;
}

FString MakeSavedJsonPathFromRelativePath(const FString& JsonRelativePath)
{
	FString Normalized = JsonRelativePath;
	Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));

	const FString Anchor = TEXT("/Saved/M2Preview/");
	int32 AnchorIndex = Normalized.Find(Anchor, ESearchCase::IgnoreCase);
	if (AnchorIndex != INDEX_NONE)
	{
		Normalized = Normalized.Mid(AnchorIndex + Anchor.Len());
	}
	else
	{
		const FString AnchorWithoutLeadingSlash = TEXT("Saved/M2Preview/");
		AnchorIndex = Normalized.Find(AnchorWithoutLeadingSlash, ESearchCase::IgnoreCase);
		if (AnchorIndex != INDEX_NONE)
		{
			Normalized = Normalized.Mid(AnchorIndex + AnchorWithoutLeadingSlash.Len());
		}
	}

	Normalized.RemoveFromStart(TEXT("Saved/"), ESearchCase::IgnoreCase);
	Normalized.RemoveFromStart(TEXT("M2Preview/"), ESearchCase::IgnoreCase);
	return FPaths::ProjectSavedDir() / TEXT("M2Preview") / Normalized;
}

FString MakeLoginSceneKeyFromJsonRelativePath(const FString& JsonRelativePath)
{
	FString Key = JsonRelativePath;
	Key.RemoveFromStart(TEXT("M2Preview/"));
	Key.RemoveFromEnd(TEXT(".mesh.json"));
	Key.ReplaceInline(TEXT("\\"), TEXT("/"));
	return Key;
}

bool IsGeneratedGlueMeshJsonCandidate(const FString& JsonFile)
{
	const FString FileName = FPaths::GetCleanFilename(JsonFile);
	if (!FileName.EndsWith(TEXT(".mesh.json"), ESearchCase::IgnoreCase) ||
		FileName.Contains(TEXT("..mesh.json"), ESearchCase::IgnoreCase))
	{
		return false;
	}

	const FString ModelName = FPaths::GetBaseFilename(FPaths::GetBaseFilename(JsonFile));
	if (ModelName.EndsWith(TEXT("skin"), ESearchCase::IgnoreCase))
	{
		return false;
	}

	return IFileManager::Get().FileSize(*JsonFile) > 0;
}

FString MakeDefaultLoginSceneDisplayName(const FString& ModelName)
{
	if (ModelName.Equals(TEXT("UI_MainMenu_Northrend"), ESearchCase::IgnoreCase))
	{
		return TEXT("巫妖王之怒");
	}
	if (ModelName.Equals(TEXT("UI_MainMenu"), ESearchCase::IgnoreCase))
	{
		return TEXT("经典旧界面");
	}
	return ModelName;
}

const AWoWLoginGameMode::FLoginSceneDefinition* GetActiveLoginSceneDefinition(const AWoWLoginGameMode* GameMode)
{
	if (!GameMode)
	{
		return nullptr;
	}
	const TArray<AWoWLoginGameMode::FLoginSceneDefinition>& Definitions = GameMode->GetLoginSceneDefinitions();
	const int32 ActiveIndex = GameMode->GetActiveLoginSceneIndex();
	return Definitions.IsValidIndex(ActiveIndex) ? &Definitions[ActiveIndex] : nullptr;
}

float GetCurrentViewportAspectRatio()
{
	if (GEngine && GEngine->GameViewport && GEngine->GameViewport->Viewport)
	{
		const FIntPoint ViewportSize = GEngine->GameViewport->Viewport->GetSizeXY();
		if (ViewportSize.X > 0 && ViewportSize.Y > 0)
		{
			return static_cast<float>(ViewportSize.X) / static_cast<float>(ViewportSize.Y);
		}
	}

	return 16.0f / 9.0f;
}

FVector ConvertUEPositionToM2(const FVector& UEPosition)
{
	return FVector(
		UEPosition.X / WowScalarToUE,
		-UEPosition.Y / WowScalarToUE,
		UEPosition.Z / WowScalarToUE);
}

FVector ConvertM2PositionToUE(const FVector& M2Position)
{
	return FVector(
		M2Position.X * WowScalarToUE,
		-M2Position.Y * WowScalarToUE,
		M2Position.Z * WowScalarToUE);
}

void ApplyLoginSceneMaterials(USkeletalMeshComponent* SkeletalMeshComponent, const AWoWLoginGameMode::FLoginSceneDefinition& SceneDefinition)
{
	if (!SkeletalMeshComponent)
	{
		return;
	}

	const int32 MaterialSlotCount = SkeletalMeshComponent->GetNumMaterials();
	int32 AppliedCount = 0;
	for (int32 SlotIndex = 0; SlotIndex < MaterialSlotCount; ++SlotIndex)
	{
		const FString MaterialPath = FString::Printf(
			TEXT("%s/MI_M2_Section_%03d.MI_M2_Section_%03d"),
			*SceneDefinition.MaterialPackagePath,
			SlotIndex,
			SlotIndex);
		UMaterialInterface* Material = ResolveLoadedObject<UMaterialInterface>(MaterialPath);
		if (Material)
		{
			SkeletalMeshComponent->SetMaterial(SlotIndex, Material);
			++AppliedCount;
		}
	}

	UE_LOG(LogTemp, Display, TEXT("登录界面材质绑定: scene=%s slots=%d applied=%d"), *SceneDefinition.DisplayName, MaterialSlotCount, AppliedCount);
}

int32 ApplyGeneratedSectionMaterials(USkeletalMeshComponent* SkeletalMeshComponent, const FString& MaterialPackagePath)
{
	if (!SkeletalMeshComponent || MaterialPackagePath.IsEmpty())
	{
		return 0;
	}

	const int32 MaterialSlotCount = SkeletalMeshComponent->GetNumMaterials();
	int32 AppliedCount = 0;
	const TArray<FName> MaterialSlotNames = SkeletalMeshComponent->GetMaterialSlotNames();
	static TMap<FString, TWeakObjectPtr<UMaterialInterface>> MaterialCache;
	auto LoadGeneratedSectionMaterial = [&MaterialPackagePath](int32 SectionIndex) -> UMaterialInterface*
	{
		const FString MaterialPath = FString::Printf(
			TEXT("%s/MI_M2_Section_%03d.MI_M2_Section_%03d"),
			*MaterialPackagePath,
			SectionIndex,
			SectionIndex);
		UMaterialInterface* Material = nullptr;
		if (TWeakObjectPtr<UMaterialInterface>* CachedMaterial = MaterialCache.Find(MaterialPath))
		{
			Material = CachedMaterial->Get();
		}
		if (!Material)
		{
			Material = ResolveLoadedObject<UMaterialInterface>(MaterialPath);
			if (Material)
			{
				MaterialCache.Add(MaterialPath, Material);
			}
		}
		return Material;
	};

	for (int32 SlotIndex = 0; SlotIndex < MaterialSlotCount; ++SlotIndex)
	{
		const FName SlotName = MaterialSlotNames.IsValidIndex(SlotIndex)
			? MaterialSlotNames[SlotIndex]
			: NAME_None;
		int32 SourceSectionIndex = INDEX_NONE;
		FString SlotNameString = SlotName.ToString();
		if (SlotNameString.StartsWith(TEXT("M2_Section_")))
		{
			SourceSectionIndex = FCString::Atoi(*SlotNameString.RightChop(11));
		}
		UMaterialInterface* Material = SourceSectionIndex != INDEX_NONE
			? LoadGeneratedSectionMaterial(SourceSectionIndex)
			: nullptr;
		if (!Material && SourceSectionIndex != SlotIndex)
		{
			Material = LoadGeneratedSectionMaterial(SlotIndex);
		}
		if (Material)
		{
			SkeletalMeshComponent->SetMaterial(SlotIndex, Material);
			++AppliedCount;
		}
	}
	return AppliedCount;
}

int32 ResolveMaterialSlotForM2Section(const USkeletalMeshComponent* SkeletalMeshComponent, int32 SectionIndex)
{
	if (!SkeletalMeshComponent || SectionIndex < 0)
	{
		return INDEX_NONE;
	}

	const TArray<FName> MaterialSlotNames = SkeletalMeshComponent->GetMaterialSlotNames();
	for (int32 SlotIndex = 0; SlotIndex < MaterialSlotNames.Num(); ++SlotIndex)
	{
		const FString SlotName = MaterialSlotNames[SlotIndex].ToString();
		if (!SlotName.StartsWith(TEXT("M2_Section_")))
		{
			continue;
		}

		int32 SourceSectionIndex = INDEX_NONE;
		LexFromString(SourceSectionIndex, *SlotName.RightChop(11));
		if (SourceSectionIndex == SectionIndex)
		{
			return SlotIndex;
		}
	}

	return SectionIndex < SkeletalMeshComponent->GetNumMaterials()
		? SectionIndex
		: INDEX_NONE;
}

void ResetCharacterPreviewRenderState(USkeletalMeshComponent* SkeletalMeshComponent)
{
	if (!SkeletalMeshComponent)
	{
		return;
	}

	SkeletalMeshComponent->ShowAllMaterialSections(0);
	const USkeletalMesh* SkeletalMesh = SkeletalMeshComponent->GetSkeletalMeshAsset();
	const FSkeletalMeshRenderData* RenderData = SkeletalMesh ? SkeletalMesh->GetResourceForRendering() : nullptr;
	if (RenderData && RenderData->LODRenderData.IsValidIndex(0))
	{
		const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0];
		for (int32 RenderSectionIndex = 0; RenderSectionIndex < LODData.RenderSections.Num(); ++RenderSectionIndex)
		{
			const int32 MaterialIndex = LODData.RenderSections[RenderSectionIndex].MaterialIndex;
			SkeletalMeshComponent->ShowMaterialSection(MaterialIndex, RenderSectionIndex, true, 0);
		}
	}

	const int32 MaterialSlotCount = SkeletalMeshComponent->GetNumMaterials();
	for (int32 SlotIndex = 0; SlotIndex < MaterialSlotCount; ++SlotIndex)
	{
		SkeletalMeshComponent->SetMaterial(SlotIndex, nullptr);
	}
}

int32 ResolveLoginPreviewM2SectionIndexFromMaterialSlot(const USkeletalMesh* SkeletalMesh, int32 MaterialIndex)
{
	if (!SkeletalMesh || !SkeletalMesh->GetMaterials().IsValidIndex(MaterialIndex))
	{
		return INDEX_NONE;
	}
	const FString SlotName = SkeletalMesh->GetMaterials()[MaterialIndex].MaterialSlotName.ToString();
	if (!SlotName.StartsWith(TEXT("M2_Section_")))
	{
		return INDEX_NONE;
	}
	int32 SectionIndex = INDEX_NONE;
	LexFromString(SectionIndex, *SlotName.RightChop(11));
	return SectionIndex;
}

void ApplyLoginPreviewSkeletalM2SectionVisibility(
	USkeletalMeshComponent* MeshComponent,
	const TMap<int32, bool>& VisibilityByM2SectionIndex,
	int32& OutApplied,
	int32& OutVisible)
{
	OutApplied = 0;
	OutVisible = 0;
	if (!MeshComponent)
	{
		return;
	}

	const USkeletalMesh* SkeletalMesh = MeshComponent->GetSkeletalMeshAsset();
	const FSkeletalMeshRenderData* RenderData = SkeletalMesh ? SkeletalMesh->GetResourceForRendering() : nullptr;
	if (!SkeletalMesh || !RenderData || !RenderData->LODRenderData.IsValidIndex(0))
	{
		return;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0];
	for (int32 RenderSectionIndex = 0; RenderSectionIndex < LODData.RenderSections.Num(); ++RenderSectionIndex)
	{
		const int32 MaterialIndex = LODData.RenderSections[RenderSectionIndex].MaterialIndex;
		const int32 M2SectionIndex = ResolveLoginPreviewM2SectionIndexFromMaterialSlot(SkeletalMesh, MaterialIndex);
		const bool bVisible = VisibilityByM2SectionIndex.Contains(M2SectionIndex)
			? VisibilityByM2SectionIndex[M2SectionIndex]
			: true;
		MeshComponent->ShowMaterialSection(MaterialIndex, RenderSectionIndex, bVisible, 0);
		if (!bVisible || M2SectionIndex >= 80)
		{
			const FName SlotName = SkeletalMesh->GetMaterials().IsValidIndex(MaterialIndex)
				? SkeletalMesh->GetMaterials()[MaterialIndex].MaterialSlotName
				: NAME_None;
			UE_LOG(LogTemp, Display, TEXT("角色预览 UE section 显隐诊断: renderSection=%d material=%d slot=%s m2Section=%d hasRule=%d visible=%d"),
				RenderSectionIndex,
				MaterialIndex,
				*SlotName.ToString(),
				M2SectionIndex,
				VisibilityByM2SectionIndex.Contains(M2SectionIndex) ? 1 : 0,
				bVisible ? 1 : 0);
		}
		++OutApplied;
		OutVisible += bVisible ? 1 : 0;
	}
}

void ApplyCharacterPreviewMaterials(
	USkeletalMeshComponent* SkeletalMeshComponent,
	const AWoWLoginGameMode::FPlayerCharacterPreviewDefinition& Definition,
	const AWoWLoginGameMode::FCharacterSelectPreviewAppearance& Appearance,
	const FString& CharacterPreviewPackageRoot,
	const FString& CharacterPreviewStem,
	const FString& ActiveMeshObjectPath,
	bool bUsingAppearanceCacheAsset)
{
	if (!SkeletalMeshComponent)
	{
		return;
	}

	ResetCharacterPreviewRenderState(SkeletalMeshComponent);

	int32 AppliedCount = 0;
	FString MaterialSource = Definition.MaterialPackagePath;
	if (bUsingAppearanceCacheAsset)
	{
		FString MeshPackagePath = ActiveMeshObjectPath;
		const int32 DotIndex = MeshPackagePath.Find(TEXT("."), ESearchCase::CaseSensitive, ESearchDir::FromEnd);
		if (DotIndex != INDEX_NONE)
		{
			MeshPackagePath = MeshPackagePath.Left(DotIndex);
		}
		FString MeshAssetName = FPackageName::GetLongPackageAssetName(MeshPackagePath);
		for (const FString& Suffix : {
			FString(TEXT("_EquipFixed6_M2Mesh")),
			FString(TEXT("_EquipFixed5_M2Mesh")),
			FString(TEXT("_EquipFixed4_M2Mesh")),
			FString(TEXT("_EquipFixed3_M2Mesh")),
			FString(TEXT("_EquipFixed2_M2Mesh")),
			FString(TEXT("_EquipFixed_M2Mesh")),
			FString(TEXT("_SkinFixed_M2Mesh")),
			FString(TEXT("_AllGeosets_M2Mesh")),
			FString(TEXT("_M2Mesh"))})
		{
			if (MeshAssetName.RemoveFromEnd(Suffix, ESearchCase::CaseSensitive))
			{
				break;
			}
		}
		const FString ActivePackageRoot = FPaths::GetPath(MeshPackagePath);
		const FString ActiveMaterialPackagePath = ActivePackageRoot / (MeshAssetName + TEXT("_Materials"));
		AppliedCount = ApplyGeneratedSectionMaterials(SkeletalMeshComponent, ActiveMaterialPackagePath);
		if (AppliedCount > 0)
		{
			MaterialSource = ActiveMaterialPackagePath;
		}
	}

	if (AppliedCount <= 0)
	{
		AppliedCount = ApplyGeneratedSectionMaterials(SkeletalMeshComponent, Definition.MaterialPackagePath);
		MaterialSource = Definition.MaterialPackagePath;
	}

	UE_LOG(LogTemp, Display, TEXT("角色预览材质绑定: id=%s appearanceCache=%d slots=%d applied=%d package=%s"),
		*Definition.Id,
		bUsingAppearanceCacheAsset ? 1 : 0,
		SkeletalMeshComponent->GetNumMaterials(),
		AppliedCount,
		*MaterialSource);

	if (bUsingAppearanceCacheAsset)
	{
		return;
	}

	UE_LOG(LogTemp, Display, TEXT("角色预览运行时外观纹理改为分帧 pass: guid=%d race=%d gender=%d skin=%d hair=%d/%d"),
		Appearance.Guid,
		Appearance.Race,
		Appearance.Gender,
		Appearance.Skin,
		Appearance.HairStyle,
		Appearance.HairColor);
}

bool ReadJsonVector2(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FVector2D& OutValue)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values || Values->Num() < 2)
	{
		return false;
	}

	OutValue = FVector2D(
		static_cast<float>((*Values)[0]->AsNumber()),
		static_cast<float>((*Values)[1]->AsNumber()));
	return true;
}

TSharedPtr<FJsonObject> TryReadJsonObject(const TSharedPtr<FJsonValue>& Value)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object || !Object->IsValid())
	{
		return nullptr;
	}
	return *Object;
}

bool ReadOptionalJsonVector3(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FVector& OutValue)
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

bool ReadCharacterPreviewAttachmentDefinitions(const FString& DescriptorPath, TArray<FCharacterPreviewAttachmentDefinition>& OutAttachments)
{
	const bool bRead = WoWCharacterVisualKit::ReadAttachmentDefinitionsFromDescriptor(DescriptorPath, OutAttachments);
	if (!bRead)
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览附件 descriptor 解析失败或缺少 attachments: %s"), *DescriptorPath);
	}
	return bRead;
}

FString BuildCharacterPreviewAttachmentStandAnimationPath(const FString& AttachmentMeshPath)
{
	return WoWCharacterVisualKit::BuildAttachmentStandAnimationPath(AttachmentMeshPath);
}

FString BuildCharacterPreviewAttachmentMeshJsonPath(const FString& AttachmentMeshPath, const FString& AttachmentDescriptorPath)
{
	return WoWCharacterVisualKit::BuildAttachmentMeshJsonPath(AttachmentMeshPath, AttachmentDescriptorPath);
}

bool ShouldUseCharacterPreviewM2RuntimeAttachment(
	const FCharacterPreviewAttachmentDefinition& AttachmentDefinition,
	const FString& MeshJsonPath)
{
	if (!AttachmentDefinition.MeshPath.IsEmpty() && DoesGeneratedAssetExist(AttachmentDefinition.MeshPath))
	{
		return false;
	}
	return !MeshJsonPath.IsEmpty() && FPaths::FileExists(MeshJsonPath);
}

FString FormatCharacterPreviewTransformForLog(const FTransform& Transform)
{
	return FString::Printf(
		TEXT("loc=%s rot=%s scale=%s"),
		*Transform.GetLocation().ToCompactString(),
		*Transform.GetRotation().Rotator().ToCompactString(),
		*Transform.GetScale3D().ToCompactString());
}

void LogCharacterPreviewAttachmentTransformDiagnostic(
	int32 Guid,
	const FCharacterPreviewAttachmentDefinition& AttachmentDefinition,
	USkeletalMeshComponent* ParentMeshComponent,
	USkeletalMeshComponent* AttachmentComponent,
	USkeletalMesh* AttachmentMesh)
{
	if (!ParentMeshComponent || !AttachmentComponent)
	{
		return;
	}

	const FTransform SocketWorldTransform = ParentMeshComponent->GetSocketTransform(AttachmentDefinition.SocketName, RTS_World);
	const FName SocketBoneName = ParentMeshComponent->GetSocketBoneName(AttachmentDefinition.SocketName);
	const FTransform BoneWorldTransform = SocketBoneName.IsNone()
		? FTransform::Identity
		: ParentMeshComponent->GetBoneTransform(SocketBoneName, RTS_World);
	const FTransform SocketRelativeToBone = SocketBoneName.IsNone()
		? FTransform::Identity
		: SocketWorldTransform.GetRelativeTransform(BoneWorldTransform);
	const FTransform AttachmentWorldTransform = AttachmentComponent->GetComponentTransform();
	const FTransform AttachmentRelativeToSocket = AttachmentWorldTransform.GetRelativeTransform(SocketWorldTransform);
	const FBoxSphereBounds RuntimeBounds = AttachmentComponent->Bounds;
	const FBoxSphereBounds AssetBounds = AttachmentMesh ? AttachmentMesh->GetBounds() : FBoxSphereBounds();
	const FString SocketWorldText = FormatCharacterPreviewTransformForLog(SocketWorldTransform);
	const FString BoneWorldText = SocketBoneName.IsNone() ? TEXT("<none>") : FormatCharacterPreviewTransformForLog(BoneWorldTransform);
	const FString SocketRelativeToBoneText = SocketBoneName.IsNone() ? TEXT("<none>") : FormatCharacterPreviewTransformForLog(SocketRelativeToBone);
	const FString AttachmentWorldText = FormatCharacterPreviewTransformForLog(AttachmentWorldTransform);
	const FString AttachmentRelativeToSocketText = FormatCharacterPreviewTransformForLog(AttachmentRelativeToSocket);

	UE_LOG(LogTemp, VeryVerbose, TEXT("角色预览附件变换诊断: guid=%d name=%s socket=%s bone=%s socketWorld={%s} boneWorld={%s} socketRelBone={%s} attachmentWorld={%s} attachmentRelSocket={%s} assetBoundsOrigin=%s assetBoundsExtent=%s runtimeBoundsOrigin=%s runtimeBoundsExtent=%s"),
		Guid,
		*AttachmentDefinition.Name,
		*AttachmentDefinition.SocketName.ToString(),
		*SocketBoneName.ToString(),
		*SocketWorldText,
		*BoneWorldText,
		*SocketRelativeToBoneText,
		*AttachmentWorldText,
		*AttachmentRelativeToSocketText,
		*AssetBounds.Origin.ToCompactString(),
		*AssetBounds.BoxExtent.ToCompactString(),
		*RuntimeBounds.Origin.ToCompactString(),
		*RuntimeBounds.BoxExtent.ToCompactString());
}

bool ReadCharacterPreviewAttachmentParticleData(
	const FString& MeshJsonPath,
	TArray<AWoWLoginGameMode::FLoginParticleEmitter>& OutEmitters,
	TArray<FVector>& OutBonePivots)
{
	OutEmitters.Reset();
	OutBonePivots.Reset();

	struct FAttachmentParticleDataCacheEntry
	{
		TArray<AWoWLoginGameMode::FLoginParticleEmitter> Emitters;
		TArray<FVector> BonePivots;
		bool bHasParticleData = false;
	};
	static TMap<FString, FAttachmentParticleDataCacheEntry> ParticleDataCache;
	if (const FAttachmentParticleDataCacheEntry* CachedData = ParticleDataCache.Find(MeshJsonPath))
	{
		OutEmitters = CachedData->Emitters;
		OutBonePivots = CachedData->BonePivots;
		return CachedData->bHasParticleData;
	}

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *MeshJsonPath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览附件粒子 mesh JSON 解析失败: %s"), *MeshJsonPath);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
	if (RootObject->TryGetArrayField(TEXT("bones"), Bones) && Bones)
	{
		for (const TSharedPtr<FJsonValue>& BoneValue : *Bones)
		{
			const TSharedPtr<FJsonObject> BoneObject = TryReadJsonObject(BoneValue);
			if (!BoneObject.IsValid())
			{
				continue;
			}

			double Number = 0.0;
			const int32 BoneIndex = BoneObject->TryGetNumberField(TEXT("index"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			if (BoneIndex < 0)
			{
				continue;
			}

			if (BoneIndex >= OutBonePivots.Num())
			{
				OutBonePivots.SetNum(BoneIndex + 1);
			}
			ReadJsonVector3(BoneObject, TEXT("pivot"), OutBonePivots[BoneIndex]);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* ParticleEmitterValues = nullptr;
	if (RootObject->TryGetArrayField(TEXT("particle_emitters"), ParticleEmitterValues) && ParticleEmitterValues)
	{
		for (const TSharedPtr<FJsonValue>& EmitterValue : *ParticleEmitterValues)
		{
			const TSharedPtr<FJsonObject> EmitterObject = TryReadJsonObject(EmitterValue);
			if (!EmitterObject.IsValid())
			{
				continue;
			}

			AWoWLoginGameMode::FLoginParticleEmitter Emitter;
			double Number = 0.0;
			if (EmitterObject->TryGetNumberField(TEXT("index"), Number))
			{
				Emitter.Index = FMath::RoundToInt(Number);
			}
			if (EmitterObject->TryGetNumberField(TEXT("flags"), Number))
			{
				Emitter.Flags = FMath::RoundToInt(Number);
			}
			if (EmitterObject->TryGetNumberField(TEXT("bone"), Number))
			{
				Emitter.BoneIndex = FMath::RoundToInt(Number);
			}
			ReadJsonVector3(EmitterObject, TEXT("position"), Emitter.Position);
			EmitterObject->TryGetStringField(TEXT("preview_png"), Emitter.PreviewPng);
			if (EmitterObject->TryGetNumberField(TEXT("blend"), Number))
			{
				Emitter.Blend = FMath::RoundToInt(Number);
			}
			if (EmitterObject->TryGetNumberField(TEXT("emitter_type"), Number))
			{
				Emitter.EmitterType = FMath::RoundToInt(Number);
			}
			if (EmitterObject->TryGetNumberField(TEXT("particle_type"), Number))
			{
				Emitter.ParticleType = FMath::RoundToInt(Number);
			}
			if (EmitterObject->TryGetNumberField(TEXT("head_or_tail"), Number))
			{
				Emitter.HeadOrTail = FMath::RoundToInt(Number);
			}
			if (EmitterObject->TryGetNumberField(TEXT("texture_tile_rotation"), Number))
			{
				Emitter.TextureTileRotation = FMath::RoundToInt(Number);
			}
			if (EmitterObject->TryGetNumberField(TEXT("texture_cols"), Number))
			{
				Emitter.TextureColumns = FMath::Max(FMath::RoundToInt(Number), 1);
			}
			if (EmitterObject->TryGetNumberField(TEXT("texture_rows"), Number))
			{
				Emitter.TextureRows = FMath::Max(FMath::RoundToInt(Number), 1);
			}
			ReadJsonLinearColorArray3(EmitterObject, TEXT("lifecycle_colors"), Emitter.LifecycleColors);
			ReadJsonFloatArray3(EmitterObject, TEXT("lifecycle_sizes"), Emitter.LifecycleSizes);
			const FString EmitterTextureName = FPaths::GetBaseFilename(Emitter.PreviewPng).ToLower();
			Emitter.bSnow = EmitterTextureName.Contains(TEXT("snow"));
			Emitter.bSmoke = EmitterTextureName.Contains(TEXT("smoke")) || EmitterTextureName.Contains(TEXT("mist"));
			Emitter.bFire =
				EmitterTextureName.Contains(TEXT("flame")) ||
				EmitterTextureName.Contains(TEXT("fire"));
			if (EmitterObject->TryGetNumberField(TEXT("mid_point"), Number))
			{
				Emitter.MidPoint = static_cast<float>(Number);
			}
			if (EmitterObject->TryGetNumberField(TEXT("slowdown"), Number))
			{
				Emitter.Slowdown = static_cast<float>(Number);
			}
			if (EmitterObject->TryGetNumberField(TEXT("rotation"), Number))
			{
				Emitter.Rotation = static_cast<float>(Number);
			}

			ReadParticleFloatSamples(EmitterObject, TEXT("emission_speed_samples"), Emitter.EmissionSpeedSamples);
			ReadParticleFloatSamples(EmitterObject, TEXT("speed_variation_samples"), Emitter.SpeedVariationSamples);
			ReadParticleFloatSamples(EmitterObject, TEXT("lifespan_samples"), Emitter.LifespanSamples);
			ReadParticleFloatSamples(EmitterObject, TEXT("emission_rate_samples"), Emitter.EmissionRateSamples);
			ReadParticleFloatSamples(EmitterObject, TEXT("emission_area_length_samples"), Emitter.EmissionAreaLengthSamples);
			ReadParticleFloatSamples(EmitterObject, TEXT("emission_area_width_samples"), Emitter.EmissionAreaWidthSamples);
			ReadParticleFloatSamples(EmitterObject, TEXT("gravity_samples"), Emitter.GravitySamples);
			ReadParticleFloatSamples(EmitterObject, TEXT("gravity2_samples"), Emitter.Gravity2Samples);
			ReadParticleFloatSamples(EmitterObject, TEXT("vertical_range_samples"), Emitter.VerticalRangeSamples);
			ReadParticleFloatSamples(EmitterObject, TEXT("horizontal_range_samples"), Emitter.HorizontalRangeSamples);
			ReadParticleFloatSamples(EmitterObject, TEXT("z_source_samples"), Emitter.ZSourceSamples);

			if (!Emitter.PreviewPng.IsEmpty())
			{
				OutEmitters.Add(MoveTemp(Emitter));
			}
		}
	}

	FAttachmentParticleDataCacheEntry CacheEntry;
	CacheEntry.Emitters = OutEmitters;
	CacheEntry.BonePivots = OutBonePivots;
	CacheEntry.bHasParticleData = !OutEmitters.IsEmpty();
	ParticleDataCache.Add(MeshJsonPath, MoveTemp(CacheEntry));
	return !OutEmitters.IsEmpty();
}

bool ReadJsonVector3(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FVector& OutValue)
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

bool ReadJsonMatrixTransform(const TSharedPtr<FJsonObject>& Object, const FString& FieldName, FTransform& OutTransform)
{
	if (!Object.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Rows = nullptr;
	if (!Object->TryGetArrayField(FieldName, Rows) || !Rows || Rows->Num() < 3)
	{
		return false;
	}

	float M[4][4] =
	{
		{ 1.0f, 0.0f, 0.0f, 0.0f },
		{ 0.0f, 1.0f, 0.0f, 0.0f },
		{ 0.0f, 0.0f, 1.0f, 0.0f },
		{ 0.0f, 0.0f, 0.0f, 1.0f }
	};

	for (int32 RowIndex = 0; RowIndex < FMath::Min(Rows->Num(), 4); ++RowIndex)
	{
		const TArray<TSharedPtr<FJsonValue>>* Columns = nullptr;
		if (!(*Rows)[RowIndex].IsValid() || !(*Rows)[RowIndex]->TryGetArray(Columns) || !Columns)
		{
			continue;
		}
		for (int32 ColumnIndex = 0; ColumnIndex < FMath::Min(Columns->Num(), 4); ++ColumnIndex)
		{
			M[RowIndex][ColumnIndex] = static_cast<float>((*Columns)[ColumnIndex]->AsNumber());
		}
	}

	const FMatrix Matrix(
		FPlane(M[0][0], M[0][1], M[0][2], M[0][3]),
		FPlane(M[1][0], M[1][1], M[1][2], M[1][3]),
		FPlane(M[2][0], M[2][1], M[2][2], M[2][3]),
		FPlane(M[3][0], M[3][1], M[3][2], M[3][3]));
	OutTransform = FTransform(Matrix);
	OutTransform.SetTranslation(FVector(M[0][3], M[1][3], M[2][3]));
	return true;
}

bool ReadJsonLinearColor(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FLinearColor& OutValue)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values || Values->Num() < 4)
	{
		return false;
	}

	OutValue = FLinearColor(
		static_cast<float>((*Values)[0]->AsNumber()),
		static_cast<float>((*Values)[1]->AsNumber()),
		static_cast<float>((*Values)[2]->AsNumber()),
		static_cast<float>((*Values)[3]->AsNumber()));
	return true;
}

bool ReadJsonLinearColorArray3(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FLinearColor OutValues[3])
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values || Values->Num() < 3)
	{
		return false;
	}

	for (int32 Index = 0; Index < 3; ++Index)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
		if (!(*Values)[Index].IsValid() || !(*Values)[Index]->TryGetArray(Tuple) || !Tuple || Tuple->Num() < 4)
		{
			return false;
		}

		OutValues[Index] = FLinearColor(
			static_cast<float>((*Tuple)[0]->AsNumber()),
			static_cast<float>((*Tuple)[1]->AsNumber()),
			static_cast<float>((*Tuple)[2]->AsNumber()),
			static_cast<float>((*Tuple)[3]->AsNumber()));
	}
	return true;
}

bool ReadJsonFloatArray3(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, float OutValues[3])
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values || Values->Num() < 3)
	{
		return false;
	}

	for (int32 Index = 0; Index < 3; ++Index)
	{
		OutValues[Index] = static_cast<float>((*Values)[Index]->AsNumber());
	}
	return true;
}

float ResolveLoopTimeMs(float TimeSeconds, float DurationMs)
{
	if (DurationMs <= KINDA_SMALL_NUMBER)
	{
		return 0.0f;
	}
	return FMath::Fmod(FMath::Max(0.0f, TimeSeconds) * 1000.0f, DurationMs);
}

int32 FindSampleSegmentByTime(float TimeMs, int32 SampleCount, TFunctionRef<float(int32)> GetSampleTime)
{
	if (SampleCount <= 1 || TimeMs <= GetSampleTime(0))
	{
		return 0;
	}

	for (int32 Index = 1; Index < SampleCount; ++Index)
	{
		if (TimeMs <= GetSampleTime(Index))
		{
			return Index - 1;
		}
	}
	return SampleCount - 1;
}

template <typename SampleType>
void ResolveLoopingSamples(
	const TArray<SampleType>& Samples,
	float TimeMs,
	float DurationMs,
	TFunctionRef<float(const SampleType&)> GetSampleTime,
	const SampleType*& OutA,
	const SampleType*& OutB,
	float& OutRatio)
{
	OutA = nullptr;
	OutB = nullptr;
	OutRatio = 0.0f;
	if (Samples.Num() == 0)
	{
		return;
	}

	OutA = &Samples[0];
	OutB = &Samples[0];
	if (Samples.Num() == 1)
	{
		return;
	}

	const float FirstTime = GetSampleTime(Samples[0]);
	const float LastTime = GetSampleTime(Samples.Last());
	if (TimeMs <= FirstTime)
	{
		OutA = &Samples[0];
		OutB = &Samples[1];
		const float SegmentDuration = GetSampleTime(*OutB) - GetSampleTime(*OutA);
		OutRatio = SegmentDuration > KINDA_SMALL_NUMBER ? (TimeMs - GetSampleTime(*OutA)) / SegmentDuration : 0.0f;
		return;
	}

	for (int32 Index = 1; Index < Samples.Num(); ++Index)
	{
		const float NextTime = GetSampleTime(Samples[Index]);
		if (TimeMs <= NextTime)
		{
			OutA = &Samples[Index - 1];
			OutB = &Samples[Index];
			const float SegmentDuration = NextTime - GetSampleTime(*OutA);
			OutRatio = SegmentDuration > KINDA_SMALL_NUMBER ? (TimeMs - GetSampleTime(*OutA)) / SegmentDuration : 0.0f;
			return;
		}
	}

	OutA = &Samples.Last();
	OutB = &Samples[0];
	const float LoopEndTime = FMath::Max(DurationMs, LastTime);
	const float SegmentDuration = LoopEndTime - LastTime + FirstTime;
	OutRatio = SegmentDuration > KINDA_SMALL_NUMBER ? (TimeMs - LastTime) / SegmentDuration : 0.0f;
}

FString SanitizeAssetName(const FString& RawName)
{
	FString Result;
	Result.Reserve(RawName.Len());
	for (const TCHAR Ch : RawName)
	{
		Result.AppendChar(FChar::IsAlnum(Ch) ? Ch : TEXT('_'));
	}
	return Result.IsEmpty() ? TEXT("Unnamed") : Result;
}

FString SanitizePackagePathSegment(const FString& RawSegment)
{
	FString Result;
	Result.Reserve(RawSegment.Len());
	for (const TCHAR Ch : RawSegment)
	{
		Result.AppendChar(FChar::IsAlnum(Ch) || Ch == TEXT('_') || Ch == TEXT('-') ? Ch : TEXT('_'));
	}
	return Result.IsEmpty() ? TEXT("Unnamed") : Result;
}

FString MakeGeneratedTextureDestinationPath(const FString& PreviewPng)
{
	FString NormalizedPath = PreviewPng;
	FPaths::NormalizeFilename(NormalizedPath);

	const FString Marker = TEXT("/Saved/WoWTextures/");
	FString RelativePath;
	if (NormalizedPath.Split(Marker, nullptr, &RelativePath))
	{
		const FString RelativeDir = FPaths::GetPath(RelativePath);
		if (!RelativeDir.IsEmpty())
		{
			TArray<FString> Segments;
			RelativeDir.ParseIntoArray(Segments, TEXT("/"), true);
			FString SanitizedRelativeDir;
			for (const FString& Segment : Segments)
			{
				SanitizedRelativeDir /= SanitizePackagePathSegment(Segment);
			}
			if (!SanitizedRelativeDir.IsEmpty())
			{
				return TEXT("/Game/WoW/Generated/Textures") / SanitizedRelativeDir;
			}
		}
	}

	return TEXT("/Game/WoW/Generated/Textures/Misc");
}

FString ResolvePreviewPngPathForLogin(const FString& PreviewPng)
{
	if (PreviewPng.IsEmpty())
	{
		return FString();
	}
	if (FPaths::FileExists(PreviewPng))
	{
		return PreviewPng;
	}

	const FString NormalizedPreviewPng = PreviewPng.Replace(TEXT("\\"), TEXT("/"));
	const FString SavedRelativePath = FPaths::Combine(FPaths::ProjectSavedDir(), NormalizedPreviewPng);
	if (FPaths::FileExists(SavedRelativePath))
	{
		return SavedRelativePath;
	}

	const FString SavedWoWTexturePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("WoWTextures"), NormalizedPreviewPng);
	if (FPaths::FileExists(SavedWoWTexturePath))
	{
		return SavedWoWTexturePath;
	}

	const FString SavedFileNamePath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("WoWTextures"), FPaths::GetCleanFilename(NormalizedPreviewPng));
	return FPaths::FileExists(SavedFileNamePath) ? SavedFileNamePath : FString();
}

UTexture2D* LoadPreviewPngAsTransientTextureForLogin(const FString& PreviewPng)
{
	const FString TexturePath = ResolvePreviewPngPathForLogin(PreviewPng);
	if (TexturePath.IsEmpty())
	{
		return nullptr;
	}

	static TMap<FString, TWeakObjectPtr<UTexture2D>> TransientTextureCache;
	if (TWeakObjectPtr<UTexture2D>* CachedTexture = TransientTextureCache.Find(TexturePath))
	{
		if (UTexture2D* Texture = CachedTexture->Get())
		{
			return Texture;
		}
	}

	TArray<uint8> CompressedData;
	if (!FFileHelper::LoadFileToArray(CompressedData, *TexturePath))
	{
		return nullptr;
	}

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!ImageWrapper.IsValid() || !ImageWrapper->SetCompressed(CompressedData.GetData(), CompressedData.Num()))
	{
		return nullptr;
	}

	TArray<uint8> RawData;
	if (!ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, RawData))
	{
		return nullptr;
	}

	const int32 Width = ImageWrapper->GetWidth();
	const int32 Height = ImageWrapper->GetHeight();
	if (Width <= 0 || Height <= 0 || RawData.Num() < Width * Height * 4)
	{
		return nullptr;
	}

	const FString TextureName = TEXT("WoWLoginPreview_") + SanitizeAssetName(FPaths::GetBaseFilename(TexturePath));
	UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8, *TextureName);
	if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.IsEmpty())
	{
		return nullptr;
	}

	Texture->SRGB = true;
	Texture->MipGenSettings = TMGS_NoMipmaps;
	Texture->CompressionSettings = TC_Default;
	Texture->NeverStream = true;
	Texture->Filter = TF_Bilinear;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;

	void* TextureData = Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(TextureData, RawData.GetData(), RawData.Num());
	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
	Texture->UpdateResource();

	TransientTextureCache.Add(TexturePath, Texture);
	return Texture;
}

UTexture2D* LoadGeneratedTextureFromPreviewPng(const FString& PreviewPng)
{
	if (PreviewPng.IsEmpty())
	{
		return nullptr;
	}

	const FString DestinationPath = MakeGeneratedTextureDestinationPath(PreviewPng);
	const FString AssetName = SanitizeAssetName(FPaths::GetBaseFilename(PreviewPng));
	const FString ObjectPath = DestinationPath / AssetName + TEXT(".") + AssetName;
	static TMap<FString, TWeakObjectPtr<UTexture2D>> TextureCache;
	if (TWeakObjectPtr<UTexture2D>* CachedTexture = TextureCache.Find(ObjectPath))
	{
		if (UTexture2D* Texture = CachedTexture->Get())
		{
			return Texture;
		}
	}
	UTexture2D* Texture = ResolveLoadedObject<UTexture2D>(ObjectPath);
	if (Texture)
	{
		TextureCache.Add(ObjectPath, Texture);
		return Texture;
	}
	return LoadPreviewPngAsTransientTextureForLogin(PreviewPng);
}

UMaterialInterface* SelectParticleParentMaterial(int32 BlendMode)
{
	const TCHAR* ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2AlphaBlendUnlit.M_WoWM2AlphaBlendUnlit");
	if (BlendMode == 4)
	{
		ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2AdditiveAlphaUnlit.M_WoWM2AdditiveAlphaUnlit");
	}
	else if (BlendMode == 3 || BlendMode == 5 || BlendMode == 6)
	{
		ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2AdditiveSrcColorUnlit.M_WoWM2AdditiveSrcColorUnlit");
	}
	static TMap<FString, TWeakObjectPtr<UMaterialInterface>> ParentMaterialCache;
	const FString ParentPathString(ParentPath);
	if (TWeakObjectPtr<UMaterialInterface>* CachedMaterial = ParentMaterialCache.Find(ParentPathString))
	{
		if (UMaterialInterface* Material = CachedMaterial->Get())
		{
			return Material;
		}
	}
	UMaterialInterface* Material = ResolveLoadedObject<UMaterialInterface>(ParentPathString);
	if (Material)
	{
		ParentMaterialCache.Add(ParentPathString, Material);
	}
	return Material;
}

UMaterialInterface* SelectTextureOverrideParentMaterial(EWoWLoginTextureRenderOverride OverrideMode, int32 EffectiveRenderMode, int32 EffectiveRenderFlags, bool bAllowPhysicalLighting)
{
	const bool bUnfogged = (EffectiveRenderFlags & 0x0002) != 0;
	const bool bM2TwoSided = (EffectiveRenderFlags & 0x0004) != 0;
	// WotLK glue/login M2s use many camera-facing translucent planes whose material flag does not always carry RF_TwoSided.
	// UE culls backfaces after our M2->UE winding conversion, so alpha/additive/masked bridge variants must stay two-sided
	// for faithful visibility. The original Materials.flag value is still preserved in the debug panel and asset data.
	const bool bTwoSided = bM2TwoSided || EffectiveRenderMode != 0;
	const bool bNoDepthWrite = (EffectiveRenderFlags & 0x0010) != 0;
	const auto MakeFlagVariantPath = [](const TCHAR* Family, bool bUseUnlit, bool bUseTwoSided, bool bUseNoDepthWrite, bool bUseUnfogged)
	{
		const FString AssetName = FString::Printf(
			TEXT("M_WoWM2_%s_%s_%s_%s_%s"),
			Family,
			bUseUnlit ? TEXT("Unlit") : TEXT("Lit"),
			bUseTwoSided ? TEXT("TwoSided") : TEXT("OneSided"),
			bUseNoDepthWrite ? TEXT("NoDepthWrite") : TEXT("DepthWrite"),
			bUseUnfogged ? TEXT("NoFog") : TEXT("Fog"));
		return FString::Printf(TEXT("/Game/WoW/Materials/%s.%s"), *AssetName, *AssetName);
	};

	const TCHAR* Family = TEXT("Opaque");
	const TCHAR* FallbackPath = TEXT("/Game/WoW/Materials/M_WoWM2Opaque.M_WoWM2Opaque");
	if (OverrideMode == EWoWLoginTextureRenderOverride::AdditiveAlpha || OverrideMode == EWoWLoginTextureRenderOverride::Additive || (OverrideMode == EWoWLoginTextureRenderOverride::M2Original && EffectiveRenderMode == 4))
	{
		Family = TEXT("AdditiveAlpha");
		FallbackPath = TEXT("/Game/WoW/Materials/M_WoWM2AdditiveAlphaUnlit.M_WoWM2AdditiveAlphaUnlit");
	}
	else if (OverrideMode == EWoWLoginTextureRenderOverride::AlphaBlend || OverrideMode == EWoWLoginTextureRenderOverride::AlphaComposite || (OverrideMode == EWoWLoginTextureRenderOverride::M2Original && EffectiveRenderMode == 2))
	{
		Family = TEXT("AlphaBlend");
		FallbackPath = TEXT("/Game/WoW/Materials/M_WoWM2AlphaBlendUnlit.M_WoWM2AlphaBlendUnlit");
	}
	else if (OverrideMode == EWoWLoginTextureRenderOverride::Opaque || (OverrideMode == EWoWLoginTextureRenderOverride::M2Original && EffectiveRenderMode == 0))
	{
		Family = TEXT("Opaque");
		FallbackPath = TEXT("/Game/WoW/Materials/M_WoWM2Opaque.M_WoWM2Opaque");
	}
	else if (OverrideMode == EWoWLoginTextureRenderOverride::Modulate || (OverrideMode == EWoWLoginTextureRenderOverride::M2Original && (EffectiveRenderMode == 3 || EffectiveRenderMode == 5 || EffectiveRenderMode == 6)))
	{
		Family = TEXT("AdditiveSrcColor");
		FallbackPath = TEXT("/Game/WoW/Materials/M_WoWM2AdditiveSrcColorUnlit.M_WoWM2AdditiveSrcColorUnlit");
	}
	else if (OverrideMode == EWoWLoginTextureRenderOverride::Masked || (OverrideMode == EWoWLoginTextureRenderOverride::M2Original && EffectiveRenderMode == 1))
	{
		Family = TEXT("Masked");
		FallbackPath = TEXT("/Game/WoW/Materials/M_WoWM2MaskedTwoSided.M_WoWM2MaskedTwoSided");
	}

	// WotLK Glue/login M2s are authored for the old fixed-function pipeline. Fully mapping them
	// to UE PBR makes stone, cloth, and sky planes read as metal/plastic. The debug light bridge
	// therefore only enables matte DefaultLit for real opaque/masked scene surfaces, and keeps
	// translucent/additive effect planes on the unlit WoW blend bridge.
	const bool bSupportsMattePhysicalLighting =
		FCString::Strcmp(Family, TEXT("Opaque")) == 0 ||
		FCString::Strcmp(Family, TEXT("Masked")) == 0;
	const bool bUseUnlitBridge = !(bAllowPhysicalLighting && bSupportsMattePhysicalLighting);
	const FString VariantPath = MakeFlagVariantPath(Family, bUseUnlitBridge, bTwoSided, bNoDepthWrite, bUnfogged);
	if (UMaterialInterface* VariantMaterial = ResolveLoadedObject<UMaterialInterface>(VariantPath))
	{
		return VariantMaterial;
	}
	return ResolveLoadedObject<UMaterialInterface>(FallbackPath);
}

FString TextureRenderOverrideToString(EWoWLoginTextureRenderOverride OverrideMode)
{
	switch (OverrideMode)
	{
	case EWoWLoginTextureRenderOverride::M2Original: return TEXT("M2Original");
	case EWoWLoginTextureRenderOverride::AdditiveAlpha: return TEXT("AdditiveAlpha");
	case EWoWLoginTextureRenderOverride::AlphaBlend: return TEXT("AlphaBlend");
	case EWoWLoginTextureRenderOverride::Opaque: return TEXT("Opaque");
	case EWoWLoginTextureRenderOverride::Masked: return TEXT("Masked");
	case EWoWLoginTextureRenderOverride::Additive: return TEXT("Additive");
	case EWoWLoginTextureRenderOverride::Modulate: return TEXT("Modulate");
	case EWoWLoginTextureRenderOverride::AlphaComposite: return TEXT("AlphaComposite");
	default: return TEXT("Unknown");
	}
}

float SampleParticleFloatTrack(const TArray<AWoWLoginGameMode::FParticleFloatSample>& Samples, float TimeSeconds, float DefaultValue)
{
	if (Samples.IsEmpty())
	{
		return DefaultValue;
	}
	if (Samples.Num() == 1)
	{
		return Samples[0].Value;
	}

	float DurationMs = 0.0f;
	for (const AWoWLoginGameMode::FParticleFloatSample& Sample : Samples)
	{
		DurationMs = FMath::Max(DurationMs, Sample.TimeMs);
	}
	const float TimeMs = ResolveLoopTimeMs(TimeSeconds, DurationMs);
	const int32 SegmentIndex = FindSampleSegmentByTime(TimeMs, Samples.Num(), [&Samples](int32 Index)
	{
		return Samples[Index].TimeMs;
	});
	const AWoWLoginGameMode::FParticleFloatSample& A = Samples[SegmentIndex];
	const AWoWLoginGameMode::FParticleFloatSample& B = Samples[FMath::Min(SegmentIndex + 1, Samples.Num() - 1)];
	const float Ratio = B.TimeMs > A.TimeMs ? (TimeMs - A.TimeMs) / (B.TimeMs - A.TimeMs) : 0.0f;
	return FMath::Lerp(A.Value, B.Value, Ratio);
}

bool ReadParticleFloatSamples(
	const TSharedPtr<FJsonObject>& Object,
	const TCHAR* FieldName,
	TArray<AWoWLoginGameMode::FParticleFloatSample>& OutSamples)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values)
	{
		return false;
	}

	OutSamples.Reset();
	OutSamples.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject> SampleObject = TryReadJsonObject(Value);
		if (!SampleObject.IsValid())
		{
			continue;
		}

		AWoWLoginGameMode::FParticleFloatSample Sample;
		Sample.TimeMs = static_cast<float>(SampleObject->GetNumberField(TEXT("time_ms")));
		Sample.Value = static_cast<float>(SampleObject->GetNumberField(TEXT("value")));
		OutSamples.Add(Sample);
	}
	return !OutSamples.IsEmpty();
}

template <typename T>
T ParticleLifeRamp(float LifeRatio, float MidPoint, const T& A, const T& B, const T& C)
{
	const float SafeMid = FMath::Clamp(MidPoint, 0.001f, 0.999f);
	if (LifeRatio <= SafeMid)
	{
		return FMath::Lerp(A, B, LifeRatio / SafeMid);
	}
	return FMath::Lerp(B, C, (LifeRatio - SafeMid) / (1.0f - SafeMid));
}

FVector ConvertWMVParticleVectorToUE(const FVector& WMVVector)
{
	// WMV particle.cpp runs after fixCoordSystem(x, z, -y), where Y is the vertical particle axis.
	// The UE exporter stores login M2 data as (x, -y, z), so WMV particle vectors convert as (x, z, y).
	return FVector(WMVVector.X, WMVVector.Z, WMVVector.Y);
}

FVector ApplyWMVSpreadMatrixToVector(
	const FVector& LocalM2Vector,
	float VerticalSpread,
	float HorizontalSpread,
	float Width,
	float Length,
	FRandomStream& Random)
{
	const float A0 = Random.FRandRange(-VerticalSpread, VerticalSpread) * 0.5f;
	const float A1 = Random.FRandRange(-HorizontalSpread, HorizontalSpread) * 0.5f;
	const float C0 = FMath::Cos(A0);
	const float S0 = FMath::Sin(A0);
	const float C1 = FMath::Cos(A1);
	const float S1 = FMath::Sin(A1);

	FVector Rotated = LocalM2Vector;
	Rotated = FVector(
		Rotated.X,
		(Rotated.Y * C0) - (Rotated.Z * S0),
		(Rotated.Y * S0) + (Rotated.Z * C0));
	Rotated = FVector(
		(Rotated.X * C1) - (Rotated.Y * S1),
		(Rotated.X * S1) + (Rotated.Y * C1),
		Rotated.Z);

	const float Size = (FMath::Abs(C0) * Length) + (FMath::Abs(S0) * Width);
	return Rotated * Size;
}

FVector BuildWMVSphereEmitterSpreadM2(
	float VerticalSpread,
	float HorizontalSpread,
	float Width,
	float Length,
	float Radius,
	FRandomStream& Random)
{
	return ApplyWMVSpreadMatrixToVector(
		FVector(0.0f, 1.0f, 0.0f),
		VerticalSpread * 2.0f,
		HorizontalSpread * 2.0f,
		Width,
		Length,
		Random) * Radius;
}

bool ShouldKeepParticleInModelSpace(int32 ParticleFlags, int32 EmitterIndex)
{
	// WotLK M2 particle flags: 0x80 is ModelSpace. WMV particle.cpp otherwise bakes the
	// emitter's parent transform into p.pos at spawn time and then advances p.pos in world space.
	return (ParticleFlags & 0x80) != 0;
}

float GetLoginParticleBaseBrightness(const AWoWLoginGameMode::FLoginParticleEmitter& Emitter)
{
	if (Emitter.bSnow)
	{
		return 1.45f;
	}
	if (Emitter.bFire)
	{
		return 2.4f;
	}
	return 0.45f;
}

float GetLoginParticleLegacyGammaWeight(const AWoWLoginGameMode::FLoginParticleEmitter& Emitter)
{
	// FlameLick in Classic login is a 4x4 low-alpha sequence texture. It needs the
	// old gamma-space additive lift per material instance; keeping this opt-in prevents
	// weapon/ribbon additive padding from returning as square halos.
	return Emitter.bFire ? 1.0f : 0.0f;
}

template <typename TObjectType>
TObjectType* LoadOrCreateObjectAsset(const TCHAR* ObjectPath, const TCHAR* PackagePath, EObjectFlags ObjectFlags = RF_Public | RF_Standalone)
{
	if (TObjectType* Existing = LoadObject<TObjectType>(nullptr, ObjectPath))
	{
		return Existing;
	}

	UPackage* Package = CreatePackage(*FString(PackagePath));
	if (!Package)
	{
		return nullptr;
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(PackagePath);
	TObjectType* NewAsset = NewObject<TObjectType>(Package, *AssetName, ObjectFlags);
	if (!NewAsset)
	{
		return nullptr;
	}

#if WITH_EDITOR
	FAssetRegistryModule::AssetCreated(NewAsset);
#endif

	NewAsset->MarkPackageDirty();
	Package->MarkPackageDirty();
	return NewAsset;
}

FWoWLoginSceneTextureTransformSample MakePresetTextureSample(const AWoWLoginGameMode::FTextureTransformSample& Source)
{
	FWoWLoginSceneTextureTransformSample Target;
	Target.TimeMs = Source.TimeMs;
	Target.Translation = Source.Translation;
	Target.Scaling = Source.Scaling;
	Target.RotationDegrees = Source.RotationDegrees;
	return Target;
}

AWoWLoginGameMode::FTextureTransformSample MakeRuntimeTextureSample(const FWoWLoginSceneTextureTransformSample& Source)
{
	AWoWLoginGameMode::FTextureTransformSample Target;
	Target.TimeMs = Source.TimeMs;
	Target.Translation = Source.Translation;
	Target.Scaling = Source.Scaling;
	Target.RotationDegrees = Source.RotationDegrees;
	return Target;
}

FWoWLoginSceneTextureSectionTrack MakePresetTextureTrack(const AWoWLoginGameMode::FSectionTextureTransformTrack& Source)
{
	FWoWLoginSceneTextureSectionTrack Target;
	Target.SlotIndex = Source.SlotIndex;
	Target.SubmeshIndex = Source.SubmeshIndex;
	Target.SubmeshId = Source.SubmeshId;
	Target.BatchFlags = Source.BatchFlags;
	Target.MaterialIndex = Source.MaterialIndex;
	Target.MaterialFlags = Source.MaterialFlags;
	Target.PriorityPlane = Source.PriorityPlane;
	Target.ShaderId = Source.ShaderId;
	Target.RenderMode = Source.RenderMode;
	Target.TextureCount = Source.TextureCount;
	Target.TextureComboIndex = Source.TextureComboIndex;
	Target.TextureCoordComboIndex = Source.TextureCoordComboIndex;
	Target.TextureWeightComboIndex = Source.TextureWeightComboIndex;
	Target.TextureTransformComboIndex = Source.TextureTransformComboIndex;
	Target.TextureCombinerIndex = Source.TextureCombinerIndex;
	Target.TextureCombinerRaw0 = Source.TextureCombinerRaw0;
	Target.TextureCombinerRaw1 = Source.TextureCombinerRaw1;
	Target.SkinTexUnitFlags = Source.SkinTexUnitFlags;
	Target.SkinTexUnitFlags2 = Source.SkinTexUnitFlags2;
	Target.SkinTexUnitOrder = Source.SkinTexUnitOrder;
	Target.SkinSubmesh = Source.SkinSubmesh;
	Target.SkinSubmesh2 = Source.SkinSubmesh2;
	Target.SkinRenderFlagsIndex = Source.SkinRenderFlagsIndex;
	Target.SkinTexUnit = Source.SkinTexUnit;
	Target.SkinTexUnitMode = Source.SkinTexUnitMode;
	Target.SkinTextureLookupIndex = Source.SkinTextureLookupIndex;
	Target.SkinTexUnit2 = Source.SkinTexUnit2;
	Target.SkinTransparencyLookupIndex = Source.SkinTransparencyLookupIndex;
	Target.SkinTextureAnimLookupIndex = Source.SkinTextureAnimLookupIndex;
	Target.TextureUnit = Source.TextureUnit;
	Target.ColorIndex = Source.ColorIndex;
	Target.TransparencyIndex = Source.TransparencyIndex;
	Target.TextureTransformIndex = Source.TextureTransformIndex;
	Target.bUseEnvMap = Source.bUseEnvMap;
	Target.bTwoSided = Source.bTwoSided;
	Target.bUnlit = Source.bUnlit;
	Target.bBillboard = Source.bBillboard;
	Target.bNoDepthWrite = Source.bNoDepthWrite;
	Target.TextureIndex = Source.TextureIndex;
	Target.TextureType = Source.TextureType;
	Target.TextureFlags = Source.TextureFlags;
	Target.SourceBlp = Source.SourceBlp;
	Target.PreviewPng = Source.PreviewPng;
	Target.DurationMs = Source.DurationMs;
	Target.Samples.Reserve(Source.Samples.Num());
	for (const AWoWLoginGameMode::FTextureTransformSample& Sample : Source.Samples)
	{
		Target.Samples.Add(MakePresetTextureSample(Sample));
	}
	return Target;
}

AWoWLoginGameMode::FSectionTextureTransformTrack MakeRuntimeTextureTrack(const FWoWLoginSceneTextureSectionTrack& Source)
{
	AWoWLoginGameMode::FSectionTextureTransformTrack Target;
	Target.SlotIndex = Source.SlotIndex;
	Target.SubmeshIndex = Source.SubmeshIndex;
	Target.SubmeshId = Source.SubmeshId;
	Target.BatchFlags = Source.BatchFlags;
	Target.MaterialIndex = Source.MaterialIndex;
	Target.MaterialFlags = Source.MaterialFlags;
	Target.PriorityPlane = Source.PriorityPlane;
	Target.ShaderId = Source.ShaderId;
	Target.RenderMode = Source.RenderMode;
	Target.TextureCount = Source.TextureCount;
	Target.TextureComboIndex = Source.TextureComboIndex;
	Target.TextureCoordComboIndex = Source.TextureCoordComboIndex;
	Target.TextureWeightComboIndex = Source.TextureWeightComboIndex;
	Target.TextureTransformComboIndex = Source.TextureTransformComboIndex;
	Target.TextureCombinerIndex = Source.TextureCombinerIndex;
	Target.TextureCombinerRaw0 = Source.TextureCombinerRaw0;
	Target.TextureCombinerRaw1 = Source.TextureCombinerRaw1;
	Target.SkinTexUnitFlags = Source.SkinTexUnitFlags;
	Target.SkinTexUnitFlags2 = Source.SkinTexUnitFlags2;
	Target.SkinTexUnitOrder = Source.SkinTexUnitOrder;
	Target.SkinSubmesh = Source.SkinSubmesh;
	Target.SkinSubmesh2 = Source.SkinSubmesh2;
	Target.SkinRenderFlagsIndex = Source.SkinRenderFlagsIndex;
	Target.SkinTexUnit = Source.SkinTexUnit;
	Target.SkinTexUnitMode = Source.SkinTexUnitMode;
	Target.SkinTextureLookupIndex = Source.SkinTextureLookupIndex;
	Target.SkinTexUnit2 = Source.SkinTexUnit2;
	Target.SkinTransparencyLookupIndex = Source.SkinTransparencyLookupIndex;
	Target.SkinTextureAnimLookupIndex = Source.SkinTextureAnimLookupIndex;
	Target.TextureUnit = Source.TextureUnit;
	Target.ColorIndex = Source.ColorIndex;
	Target.TransparencyIndex = Source.TransparencyIndex;
	Target.TextureTransformIndex = Source.TextureTransformIndex;
	Target.bUseEnvMap = Source.bUseEnvMap;
	Target.bTwoSided = Source.bTwoSided;
	Target.bUnlit = Source.bUnlit;
	Target.bBillboard = Source.bBillboard;
	Target.bNoDepthWrite = Source.bNoDepthWrite;
	Target.TextureIndex = Source.TextureIndex;
	Target.TextureType = Source.TextureType;
	Target.TextureFlags = Source.TextureFlags;
	Target.SourceBlp = Source.SourceBlp;
	Target.PreviewPng = Source.PreviewPng;
	Target.DurationMs = Source.DurationMs;
	Target.Samples.Reserve(Source.Samples.Num());
	for (const FWoWLoginSceneTextureTransformSample& Sample : Source.Samples)
	{
		Target.Samples.Add(MakeRuntimeTextureSample(Sample));
	}
	return Target;
}

FWoWLoginSceneSectionColorTrack MakePresetColorTrack(const AWoWLoginGameMode::FSectionColorTrack& Source)
{
	FWoWLoginSceneSectionColorTrack Target;
	Target.SlotIndex = Source.SlotIndex;
	Target.BaseColor = Source.BaseColor;
	Target.DurationMs = Source.DurationMs;
	Target.Samples.Reserve(Source.Samples.Num());
	for (const AWoWLoginGameMode::FColorSample& Sample : Source.Samples)
	{
		FWoWLoginSceneColorSample PresetSample;
		PresetSample.TimeMs = Sample.TimeMs;
		PresetSample.Color = Sample.Color;
		Target.Samples.Add(PresetSample);
	}
	return Target;
}

AWoWLoginGameMode::FSectionColorTrack MakeRuntimeColorTrack(const FWoWLoginSceneSectionColorTrack& Source)
{
	AWoWLoginGameMode::FSectionColorTrack Target;
	Target.SlotIndex = Source.SlotIndex;
	Target.BaseColor = Source.BaseColor;
	Target.DurationMs = Source.DurationMs;
	Target.Samples.Reserve(Source.Samples.Num());
	for (const FWoWLoginSceneColorSample& Sample : Source.Samples)
	{
		AWoWLoginGameMode::FColorSample RuntimeSample;
		RuntimeSample.TimeMs = Sample.TimeMs;
		RuntimeSample.Color = Sample.Color;
		Target.Samples.Add(RuntimeSample);
	}
	return Target;
}

FWoWLoginSceneParticleFloatSample MakePresetParticleSample(const AWoWLoginGameMode::FParticleFloatSample& Source)
{
	FWoWLoginSceneParticleFloatSample Target;
	Target.TimeMs = Source.TimeMs;
	Target.Value = Source.Value;
	return Target;
}

AWoWLoginGameMode::FParticleFloatSample MakeRuntimeParticleSample(const FWoWLoginSceneParticleFloatSample& Source)
{
	AWoWLoginGameMode::FParticleFloatSample Target;
	Target.TimeMs = Source.TimeMs;
	Target.Value = Source.Value;
	return Target;
}

FWoWLoginSceneParticleEmitterSnapshot MakePresetParticleEmitter(const AWoWLoginGameMode::FLoginParticleEmitter& Source)
{
	FWoWLoginSceneParticleEmitterSnapshot Target;
	Target.Index = Source.Index;
	Target.Flags = Source.Flags;
	Target.BoneIndex = Source.BoneIndex;
	Target.Position = Source.Position;
	Target.PreviewPng = Source.PreviewPng;
	Target.Blend = Source.Blend;
	Target.EmitterType = Source.EmitterType;
	Target.ParticleType = Source.ParticleType;
	Target.HeadOrTail = Source.HeadOrTail;
	Target.TextureTileRotation = Source.TextureTileRotation;
	Target.TextureColumns = Source.TextureColumns;
	Target.TextureRows = Source.TextureRows;
	Target.bSnow = Source.bSnow;
	Target.bSmoke = Source.bSmoke;
	Target.bFire = Source.bFire;
	Target.LifecycleColors = { Source.LifecycleColors[0], Source.LifecycleColors[1], Source.LifecycleColors[2] };
	Target.LifecycleSizes = { Source.LifecycleSizes[0], Source.LifecycleSizes[1], Source.LifecycleSizes[2] };
	Target.MidPoint = Source.MidPoint;
	Target.Slowdown = Source.Slowdown;
	Target.Rotation = Source.Rotation;
	auto CopySamples = [](const TArray<AWoWLoginGameMode::FParticleFloatSample>& InSamples, TArray<FWoWLoginSceneParticleFloatSample>& OutSamples)
	{
		OutSamples.Reserve(InSamples.Num());
		for (const AWoWLoginGameMode::FParticleFloatSample& Sample : InSamples)
		{
			OutSamples.Add(MakePresetParticleSample(Sample));
		}
	};
	CopySamples(Source.EmissionSpeedSamples, Target.EmissionSpeedSamples);
	CopySamples(Source.SpeedVariationSamples, Target.SpeedVariationSamples);
	CopySamples(Source.LifespanSamples, Target.LifespanSamples);
	CopySamples(Source.EmissionRateSamples, Target.EmissionRateSamples);
	CopySamples(Source.EmissionAreaLengthSamples, Target.EmissionAreaLengthSamples);
	CopySamples(Source.EmissionAreaWidthSamples, Target.EmissionAreaWidthSamples);
	CopySamples(Source.GravitySamples, Target.GravitySamples);
	CopySamples(Source.Gravity2Samples, Target.Gravity2Samples);
	CopySamples(Source.VerticalRangeSamples, Target.VerticalRangeSamples);
	CopySamples(Source.HorizontalRangeSamples, Target.HorizontalRangeSamples);
	CopySamples(Source.ZSourceSamples, Target.ZSourceSamples);
	return Target;
}

AWoWLoginGameMode::FLoginParticleEmitter MakeRuntimeParticleEmitter(const FWoWLoginSceneParticleEmitterSnapshot& Source)
{
	AWoWLoginGameMode::FLoginParticleEmitter Target;
	Target.Index = Source.Index;
	Target.Flags = Source.Flags;
	Target.BoneIndex = Source.BoneIndex;
	Target.Position = Source.Position;
	Target.PreviewPng = Source.PreviewPng;
	Target.Blend = Source.Blend;
	Target.EmitterType = Source.EmitterType;
	Target.ParticleType = Source.ParticleType;
	Target.HeadOrTail = Source.HeadOrTail;
	Target.TextureTileRotation = Source.TextureTileRotation;
	Target.TextureColumns = Source.TextureColumns;
	Target.TextureRows = Source.TextureRows;
	Target.bSnow = Source.bSnow;
	Target.bSmoke = Source.bSmoke;
	Target.bFire = Source.bFire;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Target.LifecycleColors[Index] = Source.LifecycleColors.IsValidIndex(Index) ? Source.LifecycleColors[Index] : FLinearColor::White;
		Target.LifecycleSizes[Index] = Source.LifecycleSizes.IsValidIndex(Index) ? Source.LifecycleSizes[Index] : 1.0f;
	}
	Target.MidPoint = Source.MidPoint;
	Target.Slowdown = Source.Slowdown;
	Target.Rotation = Source.Rotation;
	auto CopySamples = [](const TArray<FWoWLoginSceneParticleFloatSample>& InSamples, TArray<AWoWLoginGameMode::FParticleFloatSample>& OutSamples)
	{
		OutSamples.Reserve(InSamples.Num());
		for (const FWoWLoginSceneParticleFloatSample& Sample : InSamples)
		{
			OutSamples.Add(MakeRuntimeParticleSample(Sample));
		}
	};
	CopySamples(Source.EmissionSpeedSamples, Target.EmissionSpeedSamples);
	CopySamples(Source.SpeedVariationSamples, Target.SpeedVariationSamples);
	CopySamples(Source.LifespanSamples, Target.LifespanSamples);
	CopySamples(Source.EmissionRateSamples, Target.EmissionRateSamples);
	CopySamples(Source.EmissionAreaLengthSamples, Target.EmissionAreaLengthSamples);
	CopySamples(Source.EmissionAreaWidthSamples, Target.EmissionAreaWidthSamples);
	CopySamples(Source.GravitySamples, Target.GravitySamples);
	CopySamples(Source.Gravity2Samples, Target.Gravity2Samples);
	CopySamples(Source.VerticalRangeSamples, Target.VerticalRangeSamples);
	CopySamples(Source.HorizontalRangeSamples, Target.HorizontalRangeSamples);
	CopySamples(Source.ZSourceSamples, Target.ZSourceSamples);
	return Target;
}

void BuildParticleTileUVs(int32 TileIndex, int32 Columns, int32 Rows, FVector2D OutUVs[4])
{
	const int32 SafeColumns = FMath::Max(Columns, 1);
	const int32 SafeRows = FMath::Max(Rows, 1);
	const int32 TileCount = SafeColumns * SafeRows;
	const int32 SafeTile = TileCount > 0 ? FMath::Clamp(TileIndex, 0, TileCount - 1) : 0;
	const int32 X = SafeTile % SafeColumns;
	const int32 Y = SafeTile / SafeColumns;
	const float U0 = static_cast<float>(X) / static_cast<float>(SafeColumns);
	const float V0 = static_cast<float>(Y) / static_cast<float>(SafeRows);
	const float U1 = static_cast<float>(X + 1) / static_cast<float>(SafeColumns);
	const float V1 = static_cast<float>(Y + 1) / static_cast<float>(SafeRows);
	OutUVs[0] = FVector2D(U0, V1);
	OutUVs[1] = FVector2D(U1, V1);
	OutUVs[2] = FVector2D(U1, V0);
	OutUVs[3] = FVector2D(U0, V0);
}

void ApplyParticleTileRotation(FVector2D (&UVs)[4], int32 TextureTileRotation)
{
	const int32 RotationSteps = ((TextureTileRotation % 4) + 4) % 4;
	for (int32 Step = 0; Step < RotationSteps; ++Step)
	{
		const FVector2D Last = UVs[3];
		UVs[3] = UVs[2];
		UVs[2] = UVs[1];
		UVs[1] = UVs[0];
		UVs[0] = Last;
	}
}

void EnsureParticleRenderBuffers(AWoWLoginGameMode::FLoginParticleSystem& System)
{
	const int32 RenderParticleCapacity = FMath::Max(System.MaxParticles, 1);
	const int32 VertexCapacity = RenderParticleCapacity * 4;
	const int32 TriangleCapacity = RenderParticleCapacity * 6;
	if (System.RenderVertices.Num() == VertexCapacity &&
		System.RenderTriangles.Num() == TriangleCapacity &&
		System.RenderNormals.Num() == VertexCapacity &&
		System.RenderUVs.Num() == VertexCapacity &&
		System.RenderVertexColors.Num() == VertexCapacity)
	{
		return;
	}

	System.RenderVertices.SetNumUninitialized(VertexCapacity);
	System.RenderNormals.SetNumUninitialized(VertexCapacity);
	System.RenderUVs.SetNumUninitialized(VertexCapacity);
	System.RenderVertexColors.SetNumUninitialized(VertexCapacity);
	System.RenderTriangles.SetNumUninitialized(TriangleCapacity);

	for (int32 ParticleIndex = 0; ParticleIndex < RenderParticleCapacity; ++ParticleIndex)
	{
		const int32 BaseVertex = ParticleIndex * 4;
		const int32 BaseTriangle = ParticleIndex * 6;
		System.RenderTriangles[BaseTriangle + 0] = BaseVertex + 0;
		System.RenderTriangles[BaseTriangle + 1] = BaseVertex + 2;
		System.RenderTriangles[BaseTriangle + 2] = BaseVertex + 1;
		System.RenderTriangles[BaseTriangle + 3] = BaseVertex + 0;
		System.RenderTriangles[BaseTriangle + 4] = BaseVertex + 3;
		System.RenderTriangles[BaseTriangle + 5] = BaseVertex + 2;
	}
}
}

AWoWLoginGameMode::AWoWLoginGameMode()
{
	PrimaryActorTick.bCanEverTick = true;
	PlayerControllerClass = APlayerController::StaticClass();
	DefaultPawnClass = nullptr;
	GlueLoginResultQueue = MakeShared<TQueue<FGlueLoginAsyncResult, EQueueMode::Mpsc>, ESPMode::ThreadSafe>();
	GlueRealmResultQueue = MakeShared<TQueue<FGlueRealmAsyncResult, EQueueMode::Mpsc>, ESPMode::ThreadSafe>();
}

void AWoWLoginGameMode::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (ActiveCharacterPreviewBuildProcess.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览自动构建随登录场景结束而终止: pid=%u signature=%s"),
			ActiveCharacterPreviewBuildProcessId,
			*ActiveCharacterPreviewBuildSignature);
		FPlatformProcess::TerminateProc(ActiveCharacterPreviewBuildProcess, true);
		ResetCharacterPreviewBuildProcess();
	}
	Super::EndPlay(EndPlayReason);
}

FText AWoWLoginGameMode::GetActiveLoginSceneDisplayText() const
{
	return LoginSceneDefinitions.IsValidIndex(ActiveLoginSceneIndex)
		? FText::FromString(LoginSceneDefinitions[ActiveLoginSceneIndex].DisplayName)
		: FText::FromString(TEXT("无可用登录界面"));
}

void AWoWLoginGameMode::SetActiveLoginSceneIndex(int32 NewIndex)
{
	if (!LoginSceneDefinitions.IsValidIndex(NewIndex) || NewIndex == ActiveLoginSceneIndex)
	{
		return;
	}

	ActiveLoginSceneIndex = NewIndex;
	ReloadActiveLoginScene();
}

void AWoWLoginGameMode::RequestActiveLoginSceneIndex(int32 NewIndex)
{
	if (!LoginSceneDefinitions.IsValidIndex(NewIndex))
	{
		return;
	}
	if (NewIndex == ActiveLoginSceneIndex && LoginSceneActor.IsValid())
	{
		return;
	}
	if (ActiveLoginSceneAsyncTargetIndex == NewIndex &&
		ActiveLoginSceneStreamableHandle.IsValid() &&
		ActiveLoginSceneStreamableHandle->IsLoadingInProgress())
	{
		return;
	}

	ActiveLoginSceneAsyncTargetIndex = NewIndex;
	StartLoginSceneAsyncLoad(++LoginSceneAsyncRequestSerial, NewIndex);
}

void AWoWLoginGameMode::StartLoginSceneAsyncLoad(int32 RequestSerial, int32 SceneIndex, bool bAllowPresetBootstrap)
{
	if (!LoginSceneDefinitions.IsValidIndex(SceneIndex))
	{
		return;
	}

	const FLoginSceneDefinition& Definition = LoginSceneDefinitions[SceneIndex];
	const double AsyncStartSeconds = FPlatformTime::Seconds();
	if (bAllowPresetBootstrap &&
		!Definition.PresetAssetPath.IsEmpty() &&
		DoesGeneratedAssetExist(Definition.PresetAssetPath) &&
		!ResolveLoginScenePresetAsset(SceneIndex))
	{
		FStreamableManager& StreamableManager = UAssetManager::GetStreamableManager();
		ActiveLoginSceneStreamableHandle = StreamableManager.RequestAsyncLoad(
			FSoftObjectPath(Definition.PresetAssetPath),
			FStreamableDelegate::CreateWeakLambda(this, [this, RequestSerial, SceneIndex, AsyncStartSeconds]()
			{
				if (RequestSerial != LoginSceneAsyncRequestSerial)
				{
					return;
				}
				const double PresetElapsedMs = (FPlatformTime::Seconds() - AsyncStartSeconds) * 1000.0;
				UE_LOG(LogTemp, Display, TEXT("Glue scene preset async load completed: serial=%d scene=%d elapsed=%.2fms"),
					RequestSerial,
					SceneIndex,
					PresetElapsedMs);
				StartLoginSceneAsyncLoad(RequestSerial, SceneIndex, false);
			}),
			FStreamableManager::AsyncLoadHighPriority);
		UE_LOG(LogTemp, Display, TEXT("Glue scene preset async load requested: serial=%d scene=%d name=%s preset=%s"),
			RequestSerial,
			SceneIndex,
			*Definition.DisplayName,
			*Definition.PresetAssetPath);
		return;
	}

	TArray<FSoftObjectPath> AssetsToLoad;
	CollectLoginSceneAsyncAssets(SceneIndex, AssetsToLoad);
	UE_LOG(LogTemp, Display, TEXT("Glue scene async load requested: serial=%d scene=%d name=%s assets=%d mesh=%s anim=%s"),
		RequestSerial,
		SceneIndex,
		*Definition.DisplayName,
		AssetsToLoad.Num(),
		*Definition.MeshPath,
		*Definition.AnimationPath);
	if (AssetsToLoad.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("Glue scene async load has no valid assets, applying scene immediately for diagnostics: serial=%d scene=%d name=%s"),
			RequestSerial,
			SceneIndex,
			*Definition.DisplayName);
		ActiveLoginSceneAsyncTargetIndex = INDEX_NONE;
		if (SceneIndex == ActiveLoginSceneIndex)
		{
			ReloadActiveLoginScene();
		}
		else
		{
			SetActiveLoginSceneIndex(SceneIndex);
		}
		if (bPendingGlueWaitingForScene && !PendingGlueSceneScreen.IsEmpty())
		{
			const FString TargetScreen = PendingGlueSceneScreen;
			bPendingGlueWaitingForScene = false;
			PendingGlueSceneScreen.Empty();
			PendingGlueSceneWaitStartSeconds = 0.0;
			if (TryPrepareGlueScreenCharacterPreview(TargetScreen))
			{
				CompletePendingGlueScreenTransition(TargetScreen);
			}
		}
		return;
	}

	FStreamableManager& StreamableManager = UAssetManager::GetStreamableManager();
	ActiveLoginSceneStreamableHandle = StreamableManager.RequestAsyncLoad(
		AssetsToLoad,
		FStreamableDelegate::CreateWeakLambda(this, [this, RequestSerial, SceneIndex, AsyncStartSeconds]()
		{
			if (RequestSerial != LoginSceneAsyncRequestSerial)
			{
				return;
			}
			ActiveLoginSceneAsyncTargetIndex = INDEX_NONE;
			const double AsyncElapsedMs = (FPlatformTime::Seconds() - AsyncStartSeconds) * 1000.0;
			UE_LOG(LogTemp, Display, TEXT("Glue scene async load completed: serial=%d scene=%d elapsed=%.2fms pendingScreen=%s"),
				RequestSerial,
				SceneIndex,
				AsyncElapsedMs,
				*PendingGlueSceneScreen);
			if (SceneIndex == ActiveLoginSceneIndex)
			{
				ReloadActiveLoginScene();
			}
			else
			{
				SetActiveLoginSceneIndex(SceneIndex);
			}
			if (bPendingGlueWaitingForScene && !PendingGlueSceneScreen.IsEmpty())
			{
				const FString TargetScreen = PendingGlueSceneScreen;
				bPendingGlueWaitingForScene = false;
				PendingGlueSceneScreen.Empty();
				const double WaitElapsedMs = PendingGlueSceneWaitStartSeconds > 0.0
					? (FPlatformTime::Seconds() - PendingGlueSceneWaitStartSeconds) * 1000.0
					: 0.0;
				PendingGlueSceneWaitStartSeconds = 0.0;
				UE_LOG(LogTemp, Display, TEXT("Glue pending screen scene is ready: target=%s wait=%.2fms"),
					*TargetScreen,
					WaitElapsedMs);
				if (TryPrepareGlueScreenCharacterPreview(TargetScreen))
				{
					CompletePendingGlueScreenTransition(TargetScreen);
				}
			}
		}),
		FStreamableManager::AsyncLoadHighPriority);
}

void AWoWLoginGameMode::CollectLoginSceneAsyncAssets(int32 SceneIndex, TArray<FSoftObjectPath>& InOutAssetsToLoad, bool bIncludePresetRequiredAssets)
{
	if (!LoginSceneDefinitions.IsValidIndex(SceneIndex))
	{
		return;
	}

	auto AddAsset = [&InOutAssetsToLoad](const FString& ObjectPath)
	{
		const FString TrimmedObjectPath = ObjectPath.TrimStartAndEnd();
		if (TrimmedObjectPath.IsEmpty())
		{
			return;
		}
		FString PackageName;
		FString ObjectName;
		if (!TrimmedObjectPath.Split(TEXT("."), &PackageName, &ObjectName, ESearchCase::CaseSensitive, ESearchDir::FromEnd) ||
			!FPackageName::IsValidLongPackageName(PackageName, false) ||
			ObjectName.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("Glue scene async asset skipped invalid object path: %s"), *TrimmedObjectPath);
			return;
		}
		InOutAssetsToLoad.AddUnique(FSoftObjectPath(TrimmedObjectPath));
	};

	const FLoginSceneDefinition& Definition = LoginSceneDefinitions[SceneIndex];
	AddAsset(Definition.PresetAssetPath);
	AddAsset(Definition.MeshPath);
	AddAsset(Definition.AnimationPath);
	AddAsset(Definition.TuningAssetPath);

	if (bIncludePresetRequiredAssets)
	{
		if (const UWoWLoginScenePresetAsset* PresetAsset = ResolveLoginScenePresetAsset(SceneIndex))
		{
			if (!LoginSceneRuntimeCaches.Contains(SceneIndex))
			{
				ApplyLoginScenePresetRuntimeCache(SceneIndex, PresetAsset);
			}
			for (const FSoftObjectPath& RequiredAsset : PresetAsset->RequiredAssets)
			{
				AddAsset(RequiredAsset.ToString());
			}
		}
	}
	const bool bHasRuntimeCache = LoginSceneRuntimeCaches.Contains(SceneIndex);

	const FString OpaqueParent = TEXT("/Game/WoW/Materials/M_WoWM2Opaque.M_WoWM2Opaque");
	const FString AlphaBlendParent = TEXT("/Game/WoW/Materials/M_WoWM2AlphaBlendUnlit.M_WoWM2AlphaBlendUnlit");
	const FString AdditiveAlphaParent = TEXT("/Game/WoW/Materials/M_WoWM2AdditiveAlphaUnlit.M_WoWM2AdditiveAlphaUnlit");
	const FString AdditiveSrcColorParent = TEXT("/Game/WoW/Materials/M_WoWM2AdditiveSrcColorUnlit.M_WoWM2AdditiveSrcColorUnlit");
	const FString MaskedTwoSidedParent = TEXT("/Game/WoW/Materials/M_WoWM2MaskedTwoSided.M_WoWM2MaskedTwoSided");
	AddAsset(OpaqueParent);
	AddAsset(AlphaBlendParent);
	AddAsset(AdditiveAlphaParent);
	AddAsset(AdditiveSrcColorParent);
	AddAsset(MaskedTwoSidedParent);
	AddAsset(TEXT("/Game/WoW/Materials/M_WoWM2Opaque.M_WoWM2Opaque"));

	auto AddMaterialSlot = [&AddAsset, &Definition](int32 SlotIndex)
	{
		const FString MaterialPath = FString::Printf(
			TEXT("%s/MI_M2_Section_%03d.MI_M2_Section_%03d"),
			*Definition.MaterialPackagePath,
			SlotIndex,
			SlotIndex);
		AddAsset(MaterialPath);
	};

	if (const FLoginSceneRuntimeCache* Cache = LoginSceneRuntimeCaches.Find(SceneIndex))
	{
		for (const FSectionTextureTransformTrack& Section : Cache->RenderSections)
		{
			AddMaterialSlot(Section.SlotIndex);
			const FString Family =
				(Section.RenderMode == 4) ? TEXT("AdditiveAlpha") :
				(Section.RenderMode == 2) ? TEXT("AlphaBlend") :
				(Section.RenderMode == 3 || Section.RenderMode == 5 || Section.RenderMode == 6) ? TEXT("AdditiveSrcColor") :
				(Section.RenderMode == 1) ? TEXT("Masked") :
				TEXT("Opaque");
			const bool bTwoSided = (Section.MaterialFlags & 0x0004) != 0 || Section.RenderMode != 0;
			const bool bNoDepthWrite = (Section.MaterialFlags & 0x0010) != 0;
			const bool bUnfogged = (Section.MaterialFlags & 0x0002) != 0;
			for (const TCHAR* LightingMode : { TEXT("Unlit"), TEXT("Lit") })
			{
				const FString VariantName = FString::Printf(
					TEXT("M_WoWM2_%s_%s_%s_%s_%s"),
					*Family,
					LightingMode,
					bTwoSided ? TEXT("TwoSided") : TEXT("OneSided"),
					bNoDepthWrite ? TEXT("NoDepthWrite") : TEXT("DepthWrite"),
					bUnfogged ? TEXT("NoFog") : TEXT("Fog"));
				const FString VariantPath = FString::Printf(TEXT("/Game/WoW/Materials/%s.%s"), *VariantName, *VariantName);
				if (DoesGeneratedAssetExist(VariantPath))
				{
					AddAsset(VariantPath);
				}
			}
			if (!Section.PreviewPng.IsEmpty())
			{
				const FString TexturePackagePath = MakeGeneratedTextureDestinationPath(Section.PreviewPng);
				const FString TextureAssetName = SanitizeAssetName(FPaths::GetBaseFilename(Section.PreviewPng));
				AddAsset(TexturePackagePath / TextureAssetName + TEXT(".") + TextureAssetName);
			}
		}
		for (const FLoginParticleEmitter& Emitter : Cache->ParticleEmitters)
		{
			if (!Emitter.PreviewPng.IsEmpty())
			{
				const FString TexturePackagePath = MakeGeneratedTextureDestinationPath(Emitter.PreviewPng);
				const FString TextureAssetName = SanitizeAssetName(FPaths::GetBaseFilename(Emitter.PreviewPng));
				AddAsset(TexturePackagePath / TextureAssetName + TEXT(".") + TextureAssetName);
			}
		}
	}
	else if (!bIncludePresetRequiredAssets || !bHasRuntimeCache)
	{
		for (int32 SlotIndex = 0; SlotIndex < 64; ++SlotIndex)
		{
			AddMaterialSlot(SlotIndex);
		}
	}
}

bool AWoWLoginGameMode::TryResolveCharacterSelectSceneIndex(int32& OutSceneIndex) const
{
	OutSceneIndex = INDEX_NONE;
	if (CachedGlueCharacterRows.IsEmpty())
	{
		return false;
	}

	int32 SelectedIndex = 0;
	for (int32 RowIndex = 0; RowIndex < CachedGlueCharacterRows.Num(); ++RowIndex)
	{
		TArray<FString> Columns;
		CachedGlueCharacterRows[RowIndex].ParseIntoArray(Columns, TEXT("\t"), false);
		if (Columns.Num() >= 5 && FCString::Atoi(*Columns[0]) == SelectedGlueCharacterGuid)
		{
			SelectedIndex = RowIndex;
			break;
		}
	}

	TArray<FString> Columns;
	CachedGlueCharacterRows[SelectedIndex].ParseIntoArray(Columns, TEXT("\t"), false);
	if (Columns.Num() < 5)
	{
		return false;
	}

	const int32 Race = FCString::Atoi(*Columns[2]);

	static const TMap<int32, FString> BackgroundModels = {
		{1, TEXT("Interface\\Glues\\Models\\UI_Human\\UI_Human.m2")},
		{2, TEXT("Interface\\Glues\\Models\\UI_Orc\\UI_Orc.m2")},
		{3, TEXT("Interface\\Glues\\Models\\UI_Dwarf\\UI_Dwarf.m2")},
		{4, TEXT("Interface\\Glues\\Models\\UI_NightElf\\UI_NightElf.m2")},
		{5, TEXT("Interface\\Glues\\Models\\UI_Scourge\\UI_Scourge.m2")},
		{6, TEXT("Interface\\Glues\\Models\\UI_Tauren\\UI_Tauren.m2")},
		{7, TEXT("Interface\\Glues\\Models\\UI_Gnome\\UI_Gnome.m2")},
		{8, TEXT("Interface\\Glues\\Models\\UI_Troll\\UI_Troll.m2")},
		{10, TEXT("Interface\\Glues\\Models\\UI_BloodElf\\UI_BloodElf.m2")},
		{11, TEXT("Interface\\Glues\\Models\\UI_Draenei\\UI_Draenei.m2")}
	};
	static const TMap<int32, int32> ServerOrderToDbcRace = {
		{1, 1}, {2, 3}, {3, 4}, {4, 7}, {5, 11}, {6, 11}, {7, 1}, {8, 1}, {9, 11}, {10, 1},
		{11, 3}, {12, 1}, {13, 11}, {14, 4}, {15, 3}, {16, 2}, {17, 5}, {18, 6}, {19, 8}, {20, 2},
		{21, 10}, {22, 8}, {23, 10}, {24, 8}, {25, 8}, {26, 6}, {27, 2}, {28, 10}, {29, 10}, {30, 3}
	};
	static const TMap<int32, int32> RaceAliases = {
		{9, 2}, {12, 11}, {13, 8}, {14, 10}, {15, 1}, {16, 1}, {17, 8}, {18, 11}, {19, 1}, {20, 3},
		{21, 8}, {22, 1}, {23, 6}, {24, 11}, {25, 10}, {26, 2}, {27, 4}, {28, 10}, {29, 3}, {30, 3}
	};

	auto ResolveBackgroundRace = [](int32 InRace) -> int32
	{
		if (BackgroundModels.Contains(InRace))
		{
			return InRace;
		}
		if (const int32* AliasRace = RaceAliases.Find(InRace))
		{
			if (BackgroundModels.Contains(*AliasRace))
			{
				return *AliasRace;
			}
		}
		if (const int32* DbcRace = ServerOrderToDbcRace.Find(InRace))
		{
			if (const int32* AliasRace = RaceAliases.Find(*DbcRace))
			{
				if (BackgroundModels.Contains(*AliasRace))
				{
					return *AliasRace;
				}
			}
			if (BackgroundModels.Contains(*DbcRace))
			{
				return *DbcRace;
			}
		}
		return INDEX_NONE;
	};

	const int32 BackgroundRace = ResolveBackgroundRace(Race);
	if (const FString* ResolvedModelPath = BackgroundModels.Find(BackgroundRace))
	{
		OutSceneIndex = FindLoginSceneIndexByModelPath(*ResolvedModelPath);
	}
	return LoginSceneDefinitions.IsValidIndex(OutSceneIndex);
}

bool AWoWLoginGameMode::TryPrepareGlueScreenScene(const FString& TargetScreen)
{
	int32 SceneIndex = INDEX_NONE;
	if (TargetScreen.Equals(TEXT("login"), ESearchCase::IgnoreCase))
	{
		SceneIndex = LoginScreenSceneIndex;
	}
	else if (TargetScreen.Equals(TEXT("charselect"), ESearchCase::IgnoreCase) ||
		TargetScreen.Equals(TEXT("charcreate"), ESearchCase::IgnoreCase))
	{
		TryResolveCharacterSelectSceneIndex(SceneIndex);
	}

	if (!LoginSceneDefinitions.IsValidIndex(SceneIndex))
	{
		return true;
	}
	if (SceneIndex == ActiveLoginSceneIndex)
	{
		if (!IsGlueSceneVisualReadyForScreen(TargetScreen))
		{
			UE_LOG(LogTemp, Warning, TEXT("Glue target scene is active but not visual-ready, reloading scene: target=%s scene=%d"),
				*TargetScreen,
				SceneIndex);
			ReloadActiveLoginScene();
			return IsGlueSceneVisualReadyForScreen(TargetScreen);
		}
		return true;
	}

	bPendingGlueWaitingForScene = true;
	PendingGlueSceneScreen = TargetScreen;
	PendingGlueSceneWaitStartSeconds = FPlatformTime::Seconds();
	UE_LOG(LogTemp, Display, TEXT("Glue pending screen waits for scene: target=%s currentScene=%d targetScene=%d"),
		*TargetScreen,
		ActiveLoginSceneIndex,
		SceneIndex);
	RequestActiveLoginSceneIndex(SceneIndex);
	return false;
}

bool AWoWLoginGameMode::TryPrepareGlueScreenCharacterPreview(const FString& TargetScreen)
{
	if (!TargetScreen.Equals(TEXT("charselect"), ESearchCase::IgnoreCase))
	{
		return true;
	}
	if (CachedGlueCharacterRows.IsEmpty())
	{
		return true;
	}
	if (SelectedGlueCharacterGuid <= 0)
	{
		TArray<FString> FirstColumns;
		CachedGlueCharacterRows[0].ParseIntoArray(FirstColumns, TEXT("\t"), false);
		if (FirstColumns.Num() >= 1)
		{
			SelectedGlueCharacterGuid = FCString::Atoi(*FirstColumns[0]);
		}
	}
	if (SelectedGlueCharacterGuid <= 0)
	{
		return true;
	}

	FString SelectedRow;
	for (const FString& Row : CachedGlueCharacterRows)
	{
		TArray<FString> Columns;
		Row.ParseIntoArray(Columns, TEXT("\t"), false);
		if (Columns.Num() >= 5 && FCString::Atoi(*Columns[0]) == SelectedGlueCharacterGuid)
		{
			SelectedRow = Row;
			break;
		}
	}
	if (SelectedRow.IsEmpty())
	{
		return true;
	}

	TArray<FString> Columns;
	SelectedRow.ParseIntoArray(Columns, TEXT("\t"), false);
	FCharacterSelectPreviewAppearance Appearance;
	const FPlayerCharacterPreviewDefinition* Definition = nullptr;
	FString AppearanceSignature;
	FString CharacterPreviewStem;
	FString CharacterPreviewPackageRoot;
	FString CharacterPreviewLookupAnimationPath;
	if (!ResolveCharacterPreviewRuntimeAssets(Columns, Appearance, Definition, AppearanceSignature, CharacterPreviewStem, CharacterPreviewPackageRoot, CharacterPreviewLookupAnimationPath))
	{
		return true;
	}

	StoreSelectedCharacterRuntimeAppearance(Appearance);
	if (!EnsureCharacterPreviewCacheAssetReady(
		Appearance,
		Definition,
		AppearanceSignature,
		CharacterPreviewStem,
		CharacterPreviewPackageRoot,
		TEXT("Glue charselect requires a complete equipped preview cache before visual commit")))
	{
		UE_LOG(LogTemp, Warning, TEXT("Glue charselect full cache is unavailable; using runtime visual kit path when possible: guid=%d signature=%s baseRuntime=%d"),
			Appearance.Guid,
			*AppearanceSignature,
			Appearance.bUsesBaseRuntimeComposition ? 1 : 0);
		if (!Appearance.bUsesBaseRuntimeComposition)
		{
			bPendingGlueWaitingForCharacterPreview = true;
			PendingGlueCharacterPreviewScreen = TargetScreen;
			if (PendingGlueCharacterPreviewWaitStartSeconds <= 0.0)
			{
				PendingGlueCharacterPreviewWaitStartSeconds = FPlatformTime::Seconds();
			}
			return false;
		}
	}
	const FString RequestedAnimationPath = ResolveCharacterPreviewAnimationObjectPath(Appearance.CharacterPreviewAnimationPath, Appearance.EquipmentSummary);
	const FString RequestKey = WoWCharacterVisualKit::BuildPreviewRequestKey(
		SelectedGlueCharacterGuid,
		Appearance.CacheKey,
		Appearance.CharacterPreviewMeshPath,
		RequestedAnimationPath,
		Appearance.CharacterPreviewSectionMetadataPath,
		Appearance.CharacterPreviewAttachmentDescriptorPath);

	if (PreparedCharacterPreviewLoadKey.Equals(RequestKey, ESearchCase::CaseSensitive) ||
		(ActiveCharacterPreviewLoadKey.Equals(RequestKey, ESearchCase::CaseSensitive) &&
			ActiveCharacterPreviewStreamableHandle.IsValid() &&
			!ActiveCharacterPreviewStreamableHandle->IsLoadingInProgress()))
	{
		PreparedCharacterPreviewLoadKey = RequestKey;
		return true;
	}
	if (ActiveCharacterPreviewLoadKey.Equals(RequestKey, ESearchCase::CaseSensitive) &&
		ActiveCharacterPreviewStreamableHandle.IsValid() &&
		ActiveCharacterPreviewStreamableHandle->IsLoadingInProgress())
	{
		bPendingGlueWaitingForCharacterPreview = true;
		PendingGlueCharacterPreviewScreen = TargetScreen;
		return false;
	}

	bPendingGlueWaitingForCharacterPreview = true;
	PendingGlueCharacterPreviewScreen = TargetScreen;
	PendingGlueCharacterPreviewWaitStartSeconds = FPlatformTime::Seconds();
	UE_LOG(LogTemp, Display, TEXT("Glue pending screen waits for selected character preview assets: target=%s guid=%d key=%s"),
		*TargetScreen,
		SelectedGlueCharacterGuid,
		*RequestKey);
	StartSelectedCharacterPreviewAsyncLoad(++CharacterPreviewAsyncRequestSerial, SelectedGlueCharacterGuid, RequestKey, Appearance, false);
	return false;
}

FWoWLoginSceneTuningState AWoWLoginGameMode::GetLoginSceneTuningState() const
{
	return LoginSceneTuningState;
}

void AWoWLoginGameMode::SetLoginSceneTuningState(const FWoWLoginSceneTuningState& NewState)
{
	const bool bLightBridgeChanged = LoginSceneTuningState.bUseM2PhysicalLightBridge != NewState.bUseM2PhysicalLightBridge;
	const bool bLightIntensityChanged = !FMath::IsNearlyEqual(LoginSceneTuningState.M2PhysicalLightIntensityScale, NewState.M2PhysicalLightIntensityScale);
	LoginSceneTuningState = NewState;
	if (LoginSceneTuningState.CharacterPreviewAnimationOverrideId == INDEX_NONE && !LoginSceneTuningState.CharacterPreviewAnimationOverridePath.IsEmpty())
	{
		TryParseCharacterPreviewAnimationIdFromObjectPath(LoginSceneTuningState.CharacterPreviewAnimationOverridePath, LoginSceneTuningState.CharacterPreviewAnimationOverrideId);
	}
	if (LoginSceneTuningState.CharacterPreviewAnimationOverrideId != INDEX_NONE)
	{
		LoginSceneTuningState.CharacterPreviewAnimationOverridePath.Empty();
	}
	if (LoginSceneTuningAsset)
	{
		LoginSceneTuningAsset->Tuning = LoginSceneTuningState;
		MarkLoginSceneTuningAssetDirty();
	}

	ApplyLoginSceneTuning();
	if (bLightBridgeChanged)
	{
		ReapplyLoginSceneRenderOverrides();
	}
	if (bLightBridgeChanged || bLightIntensityChanged)
	{
		SpawnLoginSceneLights();
	}
}

void AWoWLoginGameMode::ResetLoginSceneTuningState()
{
	SetLoginSceneTuningState(FWoWLoginSceneTuningState());
}

AWoWLoginGameMode::FCharacterSelectPreviewDebugState AWoWLoginGameMode::GetCharacterSelectPreviewDebugState() const
{
	return CharacterSelectPreviewDebugState;
}

FString AWoWLoginGameMode::BuildCharacterPreviewAnimationOptionsCacheKey() const
{
	return FString::Printf(TEXT("%s|%s|%s|%s"),
		*SelectedGlueCharacterModelKey,
		*CharacterSelectPreviewDebugState.StandAnimationPath,
		*CharacterSelectPreviewDebugState.CharacterPreviewAnimationPath,
		*(CharacterSelectPreviewDebugState.CharacterPreviewLookupAnimationPath + TEXT("|") + CharacterSelectPreviewDebugState.BaseLookupAnimationPath));
}

AWoWLoginGameMode::FCharacterPreviewAnimationCatalogState AWoWLoginGameMode::GetCharacterPreviewAnimationCatalogState() const
{
	return CachedCharacterPreviewAnimationCatalogState;
}

void AWoWLoginGameMode::UpdateCharacterPreviewAnimationCatalogSnapshot()
{
	CachedCharacterPreviewAnimationOptionsKey = ActiveCharacterPreviewAnimationCatalogBuildKey;
	CachedCharacterPreviewAnimationPaths = ActiveCharacterPreviewAnimationCatalogCandidatePaths;
	CachedCharacterPreviewAnimationPathById = ActiveCharacterPreviewAnimationCatalogPathById;
	CachedCharacterPreviewImportedAnimationIds = ActiveCharacterPreviewAnimationCatalogImportedAnimationIds;
	CachedCharacterPreviewAnimationOptions = ActiveCharacterPreviewAnimationCatalogResolvedOptions;
	CachedCharacterPreviewAnimationCatalogState.bReady = true;
	CachedCharacterPreviewAnimationCatalogState.bWarming = false;
	CachedCharacterPreviewAnimationCatalogState.CandidatePathCount = CachedCharacterPreviewAnimationPaths.Num();
	CachedCharacterPreviewAnimationCatalogState.ImportedAnimationIdCount = CachedCharacterPreviewImportedAnimationIds.Num();
	CachedCharacterPreviewAnimationCatalogState.ResolvedOptionCount = CachedCharacterPreviewAnimationOptions.Num();
	CachedCharacterPreviewAnimationCatalogState.RemainingAnimationIdCount = 0;
}

void AWoWLoginGameMode::ResetCharacterPreviewAnimationCatalogBuild()
{
	bCharacterPreviewAnimationCatalogBuildActive = false;
	ActiveCharacterPreviewAnimationCatalogBuildKey.Empty();
	ActiveCharacterPreviewAnimationCatalogBuildMeshObjectPath.Empty();
	ActiveCharacterPreviewAnimationCatalogBuildMesh = nullptr;
	ActiveCharacterPreviewAnimationCatalogSearchPaths.Reset();
	ActiveCharacterPreviewAnimationCatalogNextSearchPathIndex = 0;
	ActiveCharacterPreviewAnimationCatalogNextResolveIndex = 0;
	ActiveCharacterPreviewAnimationCatalogCandidateKey.Empty();
	ActiveCharacterPreviewAnimationCatalogCandidatePaths.Reset();
	ActiveCharacterPreviewAnimationCatalogPathById.Reset();
	ActiveCharacterPreviewAnimationCatalogImportedAnimationIds.Reset();
	ActiveCharacterPreviewAnimationCatalogResolvedOptions.Reset();
	CachedCharacterPreviewAnimationCatalogState.bWarming = false;
	CachedCharacterPreviewAnimationCatalogState.RemainingAnimationIdCount = 0;
}

FString AWoWLoginGameMode::FindCharacterPreviewAnimationCatalogDiskPath() const
{
	const FString SearchObjectPaths[] =
	{
		CharacterSelectPreviewDebugState.CharacterPreviewAnimationPath,
		CharacterSelectPreviewDebugState.CharacterPreviewLookupAnimationPath,
		CharacterSelectPreviewDebugState.ActiveAnimationPath,
		CharacterSelectPreviewDebugState.StandAnimationPath,
		CharacterSelectPreviewDebugState.BaseLookupAnimationPath,
	};

	for (const FString& SearchObjectPath : SearchObjectPaths)
	{
		FString PackagePath;
		FString AssetName;
		if (!SearchObjectPath.Split(TEXT("."), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
		{
			continue;
		}

		int32 LastSlashIndex = INDEX_NONE;
		if (!PackagePath.FindLastChar(TEXT('/'), LastSlashIndex) || LastSlashIndex <= 0)
		{
			continue;
		}

		const FString AnimationPackagePath = PackagePath.Left(LastSlashIndex);
		const FString CandidateDiskPaths[] =
		{
			FPackageName::LongPackageNameToFilename(AnimationPackagePath / TEXT("AnimationCatalog"), TEXT(".wowanimcatalog")),
			FPackageName::LongPackageNameToFilename(AnimationPackagePath / TEXT("AnimationCatalog"), TEXT(".json")),
		};
		for (const FString& CandidateDiskPath : CandidateDiskPaths)
		{
			if (FPaths::FileExists(CandidateDiskPath))
			{
				return CandidateDiskPath;
			}
		}

		if (!AnimationPackagePath.EndsWith(TEXT("/Animations_Lookup"), ESearchCase::IgnoreCase))
		{
			const FString LookupCandidateDiskPaths[] =
			{
				FPackageName::LongPackageNameToFilename(AnimationPackagePath / TEXT("Animations_Lookup") / TEXT("AnimationCatalog"), TEXT(".wowanimcatalog")),
				FPackageName::LongPackageNameToFilename(AnimationPackagePath / TEXT("Animations_Lookup") / TEXT("AnimationCatalog"), TEXT(".json")),
			};
			for (const FString& CandidateDiskPath : LookupCandidateDiskPaths)
			{
				if (FPaths::FileExists(CandidateDiskPath))
				{
					return CandidateDiskPath;
				}
			}
		}
	}

	return FString();
}

bool AWoWLoginGameMode::TryLoadCharacterPreviewAnimationCatalog(const FString& CacheKey)
{
	const FString CatalogPath = FindCharacterPreviewAnimationCatalogDiskPath();
	if (CatalogPath.IsEmpty())
	{
		return false;
	}

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *CatalogPath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览 AnimationCatalog 解析失败: %s"), *CatalogPath);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* AnimationValues = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("animations"), AnimationValues) || !AnimationValues)
	{
		return false;
	}

	TArray<FCharacterPreviewAnimationOption> Options;
	TArray<FString> AnimationPaths;
	TMap<int32, FString> PathById;
	TArray<int32> ImportedAnimationIds;
	for (const TSharedPtr<FJsonValue>& Value : *AnimationValues)
	{
		const TSharedPtr<FJsonObject>* EntryObject = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(EntryObject) || !EntryObject || !EntryObject->IsValid())
		{
			continue;
		}

		double Number = 0.0;
		int32 AnimationId = INDEX_NONE;
		if ((*EntryObject)->TryGetNumberField(TEXT("anim_id"), Number))
		{
			AnimationId = FMath::RoundToInt(Number);
		}
		FString ObjectPath;
		(*EntryObject)->TryGetStringField(TEXT("object_path"), ObjectPath);
		FString Variant;
		(*EntryObject)->TryGetStringField(TEXT("variant"), Variant);
		if (AnimationId == INDEX_NONE || ObjectPath.IsEmpty() || !Variant.IsEmpty())
		{
			continue;
		}

		AnimationPaths.AddUnique(ObjectPath);
		if (!PathById.Contains(AnimationId))
		{
			PathById.Add(AnimationId, ObjectPath);
			ImportedAnimationIds.Add(AnimationId);
			CacheResolvedCharacterPreviewAnimationPath(SelectedGlueCharacterModelKey, CacheKey, AnimationId, ObjectPath);
			FCharacterPreviewAnimationOption Option;
			Option.AnimationId = AnimationId;
			Option.ObjectPath = ObjectPath;
			Option.Name = GetCharacterPreviewAnimationDisplayName(AnimationId, ObjectPath);
			Option.bAvailable = true;
			Options.Add(MoveTemp(Option));
		}
	}

	if (Options.IsEmpty())
	{
		return false;
	}

	const int32 PreferredOrder[] = {0, 4, 5, 25, 26, 27, 28, 16, 17, 18, 19, 37, 38, 39, 89, 90, 187, 1};
	auto FindPreferredIndex = [&PreferredOrder](const int32 AnimationId) -> int32
	{
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(PreferredOrder); ++Index)
		{
			if (PreferredOrder[Index] == AnimationId)
			{
				return Index;
			}
		}
		return INDEX_NONE;
	};
	Options.Sort([&FindPreferredIndex](const FCharacterPreviewAnimationOption& A, const FCharacterPreviewAnimationOption& B)
	{
		const int32 APreferredIndex = FindPreferredIndex(A.AnimationId);
		const int32 BPreferredIndex = FindPreferredIndex(B.AnimationId);
		const bool bAIsPreferred = APreferredIndex != INDEX_NONE;
		const bool bBIsPreferred = BPreferredIndex != INDEX_NONE;
		if (bAIsPreferred != bBIsPreferred)
		{
			return bAIsPreferred;
		}
		if (bAIsPreferred && bBIsPreferred)
		{
			return APreferredIndex < BPreferredIndex;
		}
		return A.AnimationId < B.AnimationId;
	});
	ImportedAnimationIds.Sort();

	CachedCharacterPreviewAnimationOptionsKey = CacheKey;
	CachedCharacterPreviewAnimationPaths = MoveTemp(AnimationPaths);
	CachedCharacterPreviewAnimationPathById = MoveTemp(PathById);
	CachedCharacterPreviewImportedAnimationIds = MoveTemp(ImportedAnimationIds);
	CachedCharacterPreviewAnimationOptions = MoveTemp(Options);
	CachedCharacterPreviewAnimationCatalogState.bReady = true;
	CachedCharacterPreviewAnimationCatalogState.bWarming = false;
	CachedCharacterPreviewAnimationCatalogState.CandidatePathCount = CachedCharacterPreviewAnimationPaths.Num();
	CachedCharacterPreviewAnimationCatalogState.ImportedAnimationIdCount = CachedCharacterPreviewImportedAnimationIds.Num();
	CachedCharacterPreviewAnimationCatalogState.ResolvedOptionCount = CachedCharacterPreviewAnimationOptions.Num();
	CachedCharacterPreviewAnimationCatalogState.RemainingAnimationIdCount = 0;
	UE_LOG(LogTemp, Display, TEXT("角色预览已加载 AnimationCatalog: guid=%d actions=%d path=%s"),
		SelectedGlueCharacterGuid,
		CachedCharacterPreviewAnimationOptions.Num(),
		*CatalogPath);
	return true;
}

bool AWoWLoginGameMode::TryGetResolvedCharacterPreviewAnimationPath(const FString& MeshObjectPath, const FString& CacheKey, int32 AnimationId, FString& OutAnimationObjectPath) const
{
	if (MeshObjectPath.IsEmpty() || CacheKey.IsEmpty() || AnimationId == INDEX_NONE)
	{
		return false;
	}
	const FString ResolvedKey = FString::Printf(TEXT("%s|%d|%s"), *MeshObjectPath, AnimationId, *CacheKey);
	if (MissingCharacterPreviewResolvedAnimationKeys.Contains(ResolvedKey))
	{
		return false;
	}
	if (const FString* CachedPath = CachedCharacterPreviewResolvedAnimationPaths.Find(ResolvedKey))
	{
		OutAnimationObjectPath = *CachedPath;
		return !OutAnimationObjectPath.IsEmpty();
	}
	return false;
}

void AWoWLoginGameMode::CacheResolvedCharacterPreviewAnimationPath(const FString& MeshObjectPath, const FString& CacheKey, int32 AnimationId, const FString& AnimationObjectPath)
{
	if (MeshObjectPath.IsEmpty() || CacheKey.IsEmpty() || AnimationId == INDEX_NONE || AnimationObjectPath.IsEmpty())
	{
		return;
	}
	const FString ResolvedKey = FString::Printf(TEXT("%s|%d|%s"), *MeshObjectPath, AnimationId, *CacheKey);
	CachedCharacterPreviewResolvedAnimationPaths.Add(ResolvedKey, AnimationObjectPath);
	MissingCharacterPreviewResolvedAnimationKeys.Remove(ResolvedKey);
}

void AWoWLoginGameMode::CacheMissingResolvedCharacterPreviewAnimation(const FString& MeshObjectPath, const FString& CacheKey, int32 AnimationId)
{
	if (MeshObjectPath.IsEmpty() || CacheKey.IsEmpty() || AnimationId == INDEX_NONE)
	{
		return;
	}
	const FString ResolvedKey = FString::Printf(TEXT("%s|%d|%s"), *MeshObjectPath, AnimationId, *CacheKey);
	MissingCharacterPreviewResolvedAnimationKeys.Add(ResolvedKey);
}

void AWoWLoginGameMode::StartCharacterPreviewAnimationCatalogBuild(const FString& CacheKey)
{
	ResetCharacterPreviewAnimationCatalogBuild();
	if (CacheKey.IsEmpty() || SelectedGlueCharacterModelKey.IsEmpty())
	{
		return;
	}
	if (TryLoadCharacterPreviewAnimationCatalog(CacheKey))
	{
		return;
	}
	USkeletalMesh* PreviewMesh = ResolveLoadedObject<USkeletalMesh>(SelectedGlueCharacterModelKey);
	if (!PreviewMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览 AnimationCatalog 等待 mesh 预加载，跳过同步加载: mesh=%s key=%s"),
			*SelectedGlueCharacterModelKey,
			*CacheKey);
		return;
	}
	bCharacterPreviewAnimationCatalogBuildActive = true;
	ActiveCharacterPreviewAnimationCatalogBuildSerial = CharacterPreviewAnimationCatalogRequestSerial;
	ActiveCharacterPreviewAnimationCatalogBuildKey = CacheKey;
	ActiveCharacterPreviewAnimationCatalogBuildMeshObjectPath = SelectedGlueCharacterModelKey;
	ActiveCharacterPreviewAnimationCatalogBuildMesh = PreviewMesh;
	ActiveCharacterPreviewAnimationCatalogSearchPaths = {
		CharacterSelectPreviewDebugState.CharacterPreviewAnimationPath,
		CharacterSelectPreviewDebugState.ActiveAnimationPath,
		CharacterSelectPreviewDebugState.CharacterPreviewLookupAnimationPath,
		CharacterSelectPreviewDebugState.StandAnimationPath,
		CharacterSelectPreviewDebugState.BaseLookupAnimationPath
	};
	CachedCharacterPreviewAnimationCatalogState.bReady = false;
	CachedCharacterPreviewAnimationCatalogState.bWarming = true;
	CachedCharacterPreviewAnimationCatalogState.CandidatePathCount = 0;
	CachedCharacterPreviewAnimationCatalogState.ImportedAnimationIdCount = 0;
	CachedCharacterPreviewAnimationCatalogState.ResolvedOptionCount = 0;
	CachedCharacterPreviewAnimationCatalogState.RemainingAnimationIdCount = 0;
}

void AWoWLoginGameMode::ProcessCharacterPreviewAnimationCatalogBuild(float DeltaSeconds)
{
	if (!bCharacterPreviewAnimationCatalogBuildActive)
	{
		return;
	}
	if (ActiveCharacterPreviewAnimationCatalogBuildSerial != CharacterPreviewAnimationCatalogRequestSerial)
	{
		ResetCharacterPreviewAnimationCatalogBuild();
		return;
	}
	if (!BuildCharacterPreviewAnimationOptionsCacheKey().Equals(ActiveCharacterPreviewAnimationCatalogBuildKey, ESearchCase::CaseSensitive))
	{
		ResetCharacterPreviewAnimationCatalogBuild();
		return;
	}
	if (ActiveCharacterPreviewAnimationCatalogNextSearchPathIndex < ActiveCharacterPreviewAnimationCatalogSearchPaths.Num())
	{
		const FString SearchPath = ActiveCharacterPreviewAnimationCatalogSearchPaths[ActiveCharacterPreviewAnimationCatalogNextSearchPathIndex++];
		if (!SearchPath.IsEmpty())
		{
			AddCharacterPreviewAnimationPathsForSearchObjectPath(SearchPath, ActiveCharacterPreviewAnimationCatalogCandidatePaths);
		}
		CachedCharacterPreviewAnimationCatalogState.bWarming = true;
		CachedCharacterPreviewAnimationCatalogState.CandidatePathCount = ActiveCharacterPreviewAnimationCatalogCandidatePaths.Num();
		return;
	}
	if (ActiveCharacterPreviewAnimationCatalogPathById.Num() == 0)
	{
		ActiveCharacterPreviewAnimationCatalogPathById = BuildCharacterPreviewAnimationPathById(ActiveCharacterPreviewAnimationCatalogCandidatePaths);
		ActiveCharacterPreviewAnimationCatalogPathById.GetKeys(ActiveCharacterPreviewAnimationCatalogImportedAnimationIds);
		const int32 PreferredOrder[] = {0, 4, 5, 25, 26, 27, 28, 16, 17, 18, 19, 37, 38, 39, 89, 90, 187, 1};
		ActiveCharacterPreviewAnimationCatalogImportedAnimationIds.Sort([&PreferredOrder](const int32 A, const int32 B)
		{
			auto FindPreferredIndex = [&PreferredOrder](const int32 InAnimationId) -> int32
			{
				for (int32 Index = 0; Index < UE_ARRAY_COUNT(PreferredOrder); ++Index)
				{
					if (PreferredOrder[Index] == InAnimationId)
					{
						return Index;
					}
				}
				return INDEX_NONE;
			};
			const int32 APreferredIndex = FindPreferredIndex(A);
			const int32 BPreferredIndex = FindPreferredIndex(B);
			const bool bAIsPreferred = APreferredIndex != INDEX_NONE;
			const bool bBIsPreferred = BPreferredIndex != INDEX_NONE;
			if (bAIsPreferred != bBIsPreferred)
			{
				return bAIsPreferred;
			}
			if (bAIsPreferred && bBIsPreferred)
			{
				return APreferredIndex < BPreferredIndex;
			}
			return A < B;
		});
		CachedCharacterPreviewAnimationCatalogState.CandidatePathCount = ActiveCharacterPreviewAnimationCatalogCandidatePaths.Num();
		CachedCharacterPreviewAnimationCatalogState.ImportedAnimationIdCount = ActiveCharacterPreviewAnimationCatalogImportedAnimationIds.Num();
		CachedCharacterPreviewAnimationCatalogState.RemainingAnimationIdCount = ActiveCharacterPreviewAnimationCatalogImportedAnimationIds.Num();
		return;
	}
	const int32 MaxResolvePerTick = 2;
	int32 ResolvedThisTick = 0;
	while (ActiveCharacterPreviewAnimationCatalogNextResolveIndex < ActiveCharacterPreviewAnimationCatalogImportedAnimationIds.Num() && ResolvedThisTick < MaxResolvePerTick)
	{
		const int32 AnimationId = ActiveCharacterPreviewAnimationCatalogImportedAnimationIds[ActiveCharacterPreviewAnimationCatalogNextResolveIndex++];
		FString CompatibleAnimationPath;
		if (ResolveCharacterPreviewCompatibleAnimationPathById(
			ActiveCharacterPreviewAnimationCatalogBuildMesh,
			AnimationId,
			ActiveCharacterPreviewAnimationCatalogCandidatePaths,
			ActiveCharacterPreviewAnimationCatalogBuildMeshObjectPath,
			TEXT("ProcessCharacterPreviewAnimationCatalogBuild"),
			CompatibleAnimationPath,
			false))
		{
			FCharacterPreviewAnimationOption Option;
			Option.AnimationId = AnimationId;
			Option.ObjectPath = CompatibleAnimationPath;
			Option.Name = GetCharacterPreviewAnimationDisplayName(AnimationId, Option.ObjectPath);
			Option.bAvailable = true;
			ActiveCharacterPreviewAnimationCatalogResolvedOptions.Add(MoveTemp(Option));
			CacheResolvedCharacterPreviewAnimationPath(ActiveCharacterPreviewAnimationCatalogBuildMeshObjectPath, ActiveCharacterPreviewAnimationCatalogBuildKey, AnimationId, CompatibleAnimationPath);
		}
		else
		{
			CacheMissingResolvedCharacterPreviewAnimation(ActiveCharacterPreviewAnimationCatalogBuildMeshObjectPath, ActiveCharacterPreviewAnimationCatalogBuildKey, AnimationId);
		}
		++ResolvedThisTick;
	}
	CachedCharacterPreviewAnimationCatalogState.bWarming = true;
	CachedCharacterPreviewAnimationCatalogState.ResolvedOptionCount = ActiveCharacterPreviewAnimationCatalogResolvedOptions.Num();
	CachedCharacterPreviewAnimationCatalogState.RemainingAnimationIdCount = FMath::Max(ActiveCharacterPreviewAnimationCatalogImportedAnimationIds.Num() - ActiveCharacterPreviewAnimationCatalogNextResolveIndex, 0);
	if (ActiveCharacterPreviewAnimationCatalogNextResolveIndex >= ActiveCharacterPreviewAnimationCatalogImportedAnimationIds.Num())
	{
		UpdateCharacterPreviewAnimationCatalogSnapshot();
		ResetCharacterPreviewAnimationCatalogBuild();
	}
}

TArray<AWoWLoginGameMode::FCharacterPreviewAnimationOption> AWoWLoginGameMode::GetCharacterPreviewLogicalAnimationOptions() const
{
	return CachedCharacterPreviewAnimationOptions;
}

void AWoWLoginGameMode::QueueCurrentCharacterPreviewAnimationOptionsPrewarm()
{
	UWorld* World = GetWorld();
	if (!World || SelectedGlueCharacterModelKey.IsEmpty())
	{
		return;
	}
	const FString ExpectedCacheKey = BuildCharacterPreviewAnimationOptionsCacheKey();
	if (ExpectedCacheKey.IsEmpty() || CachedCharacterPreviewAnimationOptionsKey.Equals(ExpectedCacheKey, ESearchCase::CaseSensitive))
	{
		return;
	}
	if (PendingCharacterPreviewAnimationOptionsPrewarmKey.Equals(ExpectedCacheKey, ESearchCase::CaseSensitive))
	{
		return;
	}
	PendingCharacterPreviewAnimationOptionsPrewarmKey = ExpectedCacheKey;
	World->GetTimerManager().ClearTimer(CharacterPreviewAnimationOptionsPrewarmTimerHandle);
	World->GetTimerManager().SetTimer(
		CharacterPreviewAnimationOptionsPrewarmTimerHandle,
		FTimerDelegate::CreateWeakLambda(
		this,
		[this, ExpectedCacheKey]()
		{
			PrewarmCurrentCharacterPreviewAnimationOptions(ExpectedCacheKey);
		}),
		0.35f,
		false);
}

void AWoWLoginGameMode::PrewarmCurrentCharacterPreviewAnimationOptions(const FString& ExpectedCacheKey)
{
	if (!PendingCharacterPreviewAnimationOptionsPrewarmKey.Equals(ExpectedCacheKey, ESearchCase::CaseSensitive))
	{
		return;
	}
	PendingCharacterPreviewAnimationOptionsPrewarmKey.Empty();
	if (!BuildCharacterPreviewAnimationOptionsCacheKey().Equals(ExpectedCacheKey, ESearchCase::CaseSensitive))
	{
		return;
	}
	if (CachedCharacterPreviewAnimationOptionsKey.Equals(ExpectedCacheKey, ESearchCase::CaseSensitive))
	{
		return;
	}
	StartCharacterPreviewAnimationCatalogBuild(ExpectedCacheKey);
}

TArray<FString> AWoWLoginGameMode::GetCharacterPreviewAvailableAnimationPaths() const
{
	return CachedCharacterPreviewAnimationPaths;
}

void AWoWLoginGameMode::SetCharacterPreviewAnimationOverridePath(const FString& ObjectPath)
{
	FWoWLoginSceneTuningState NewState = LoginSceneTuningState;
	const FString TrimmedPath = ObjectPath.TrimStartAndEnd();
	int32 AnimationId = INDEX_NONE;
	if (!TrimmedPath.IsEmpty() && IsCharacterPreviewSelectableAnimationPath(TrimmedPath) && TryParseCharacterPreviewAnimationIdFromObjectPath(TrimmedPath, AnimationId))
	{
		NewState.CharacterPreviewAnimationOverrideId = AnimationId;
		NewState.CharacterPreviewAnimationOverridePath.Empty();
	}
	else
	{
		NewState.CharacterPreviewAnimationOverrideId = INDEX_NONE;
		NewState.CharacterPreviewAnimationOverridePath.Empty();
	}
	SetLoginSceneTuningState(NewState);
	if (!ApplyCharacterPreviewAnimationOnlyById(NewState.CharacterPreviewAnimationOverrideId))
	{
		RequestSelectedCharacterPreviewRefresh();
	}
}

void AWoWLoginGameMode::SetCharacterPreviewAnimationOverrideById(int32 AnimationId)
{
	FWoWLoginSceneTuningState NewState = LoginSceneTuningState;
	NewState.CharacterPreviewAnimationOverrideId = AnimationId;
	NewState.CharacterPreviewAnimationOverridePath.Empty();
	SetLoginSceneTuningState(NewState);
	if (!ApplyCharacterPreviewAnimationOnlyById(AnimationId))
	{
		RequestSelectedCharacterPreviewRefresh();
	}
}

void AWoWLoginGameMode::ClearCharacterPreviewAnimationOverridePath()
{
	FWoWLoginSceneTuningState NewState = LoginSceneTuningState;
	NewState.CharacterPreviewAnimationOverrideId = INDEX_NONE;
	NewState.CharacterPreviewAnimationOverridePath.Empty();
	SetLoginSceneTuningState(NewState);
	if (!ApplyCharacterPreviewAnimationOnlyById(INDEX_NONE))
	{
		RequestSelectedCharacterPreviewRefresh();
	}
}

FWoWLoginSceneM2CameraState AWoWLoginGameMode::GetLoginSceneBaseM2CameraState() const
{
	FWoWLoginSceneM2CameraState State;
	State.Id = BaseCameraId;
	State.Pos = ConvertUEPositionToM2(BaseCameraPosition);
	State.Target = ConvertUEPositionToM2(BaseCameraTarget);
	State.FovRadians = BaseCameraFovDegrees;
	State.NearClipping = BaseCameraNearClip / WowScalarToUE;
	State.FarClipping = BaseCameraFarClip / WowScalarToUE;
	State.RollRadians = 0.0f;
	return State;
}

FWoWLoginSceneM2CameraState AWoWLoginGameMode::GetLoginSceneM2CameraState() const
{
	FWoWLoginSceneM2CameraState State = GetLoginSceneBaseM2CameraState();
	State.Pos = ConvertUEPositionToM2(BaseCameraPosition + LoginSceneTuningState.CameraPositionOffset);
	State.Target = ConvertUEPositionToM2(BaseCameraTarget + LoginSceneTuningState.CameraTargetOffset);
	State.FovRadians = BaseCameraFovDegrees + LoginSceneTuningState.CameraVerticalFovOffsetDegrees;
	State.NearClipping = (BaseCameraNearClip / WowScalarToUE) + LoginSceneTuningState.CameraNearClipOffsetWowUnits;
	State.FarClipping = (BaseCameraFarClip / WowScalarToUE) + LoginSceneTuningState.CameraFarClipOffsetWowUnits;
	State.RollRadians = FMath::DegreesToRadians(LoginSceneTuningState.CameraRollOffsetDegrees);
	return State;
}

void AWoWLoginGameMode::SetLoginSceneM2CameraState(const FWoWLoginSceneM2CameraState& NewState)
{
	FWoWLoginSceneTuningState Tuning = LoginSceneTuningState;
	Tuning.CameraPositionOffset = ConvertM2PositionToUE(NewState.Pos) - BaseCameraPosition;
	Tuning.CameraTargetOffset = ConvertM2PositionToUE(NewState.Target) - BaseCameraTarget;
	Tuning.CameraVerticalFovOffsetDegrees = NewState.FovRadians - BaseCameraFovDegrees;
	Tuning.CameraNearClipOffsetWowUnits = NewState.NearClipping - (BaseCameraNearClip / WowScalarToUE);
	Tuning.CameraFarClipOffsetWowUnits = NewState.FarClipping - (BaseCameraFarClip / WowScalarToUE);
	Tuning.CameraRollOffsetDegrees = FMath::RadiansToDegrees(NewState.RollRadians);
	SetLoginSceneTuningState(Tuning);
}

FString AWoWLoginGameMode::ExportLoginSceneTuningState() const
{
	const FWoWLoginSceneTuningState& State = LoginSceneTuningState;
	return FString::Printf(
		TEXT("SceneLocationOffset=%s; SceneRotationOffset=%s; SceneScaleMultiplier=%s; CameraPositionOffset=%s; CameraTargetOffset=%s; CameraFovRawOffset=%.6f; CameraRollOffsetDegrees=%.4f; CameraNearClipOffsetWowUnits=%.4f; CameraFarClipOffsetWowUnits=%.4f; GlobalBrightnessScale=%.4f"),
		*State.SceneLocationOffset.ToString(),
		*State.SceneRotationOffset.ToString(),
		*State.SceneScaleMultiplier.ToString(),
		*State.CameraPositionOffset.ToString(),
		*State.CameraTargetOffset.ToString(),
		State.CameraVerticalFovOffsetDegrees,
		State.CameraRollOffsetDegrees,
		State.CameraNearClipOffsetWowUnits,
		State.CameraFarClipOffsetWowUnits,
		State.GlobalBrightnessScale);
}

FString AWoWLoginGameMode::ExportLoginSceneBaseCameraState() const
{
	return FString::Printf(
		TEXT("Id=%d; Pos=%s; Target=%s; FOV(rad)=%.6f; NearClipping=%.6f; FarClipping=%.6f"),
		BaseCameraId,
		*ConvertUEPositionToM2(BaseCameraPosition).ToString(),
		*ConvertUEPositionToM2(BaseCameraTarget).ToString(),
		BaseCameraFovDegrees,
		BaseCameraNearClip / WowScalarToUE,
		BaseCameraFarClip / WowScalarToUE);
}

FString AWoWLoginGameMode::ExportLoginSceneFinalCameraState() const
{
	const FWoWLoginSceneM2CameraState FinalState = GetLoginSceneM2CameraState();
	return FString::Printf(
		TEXT("FinalPos=%s; FinalTarget=%s; FinalFOV(raw)=%.6f; FinalProjectionFOV(deg)=%.6f; FinalNearClipping=%.6f; FinalFarClipping=%.6f; FinalRoll(rad)=%.6f"),
		*FinalState.Pos.ToString(),
		*FinalState.Target.ToString(),
		FinalState.FovRadians,
		FinalState.FovRadians * M2CameraFovToProjectionDegrees,
		FinalState.NearClipping,
		FinalState.FarClipping,
		FinalState.RollRadians);
}

void AWoWLoginGameMode::SaveLoginSceneTuningAsset()
{
	MarkLoginSceneTuningAssetDirty();

#if WITH_EDITOR
	if (!LoginSceneTuningAsset)
	{
		return;
	}

	UPackage* Package = LoginSceneTuningAsset->GetPackage();
	if (!Package)
	{
		return;
	}

	const FString PackageName = Package->GetName();
	FString PackageFileName;
	if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, PackageFileName, FPackageName::GetAssetPackageExtension()))
	{
		return;
	}

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	UPackage::SavePackage(Package, LoginSceneTuningAsset, *PackageFileName, SaveArgs);
#endif
}

void AWoWLoginGameMode::SaveLoginScenePresetRuntimeCache(UWoWLoginScenePresetAsset* PresetAsset, const FLoginSceneRuntimeCache& RuntimeCache) const
{
	if (!PresetAsset)
	{
		return;
	}

	FWoWLoginSceneRuntimeCacheSnapshot& Snapshot = PresetAsset->RuntimeCache;
	Snapshot = FWoWLoginSceneRuntimeCacheSnapshot();
	Snapshot.bCameraLoaded = RuntimeCache.bCameraLoaded;
	Snapshot.CameraId = RuntimeCache.CameraId;
	Snapshot.CameraPosition = RuntimeCache.CameraPosition;
	Snapshot.CameraTarget = RuntimeCache.CameraTarget;
	Snapshot.CameraFovDegrees = RuntimeCache.CameraFovDegrees;
	Snapshot.CameraNearClip = RuntimeCache.CameraNearClip;
	Snapshot.CameraFarClip = RuntimeCache.CameraFarClip;
	Snapshot.bRuntimeLoaded = RuntimeCache.bRuntimeLoaded;
	Snapshot.CameraTrackDurationMs = RuntimeCache.CameraTrackDurationMs;
	Snapshot.LateAdditiveOverlaySlots = RuntimeCache.LateAdditiveOverlaySlots;
	Snapshot.BonePivots = RuntimeCache.BonePivots;

	Snapshot.TextureTransformTracks.Reserve(RuntimeCache.TextureTransformTracks.Num());
	for (const FSectionTextureTransformTrack& Track : RuntimeCache.TextureTransformTracks)
	{
		Snapshot.TextureTransformTracks.Add(MakePresetTextureTrack(Track));
	}

	Snapshot.RenderSections.Reserve(RuntimeCache.RenderSections.Num());
	for (const FSectionTextureTransformTrack& Track : RuntimeCache.RenderSections)
	{
		Snapshot.RenderSections.Add(MakePresetTextureTrack(Track));
	}

	Snapshot.ColorTracks.Reserve(RuntimeCache.ColorTracks.Num());
	for (const FSectionColorTrack& Track : RuntimeCache.ColorTracks)
	{
		Snapshot.ColorTracks.Add(MakePresetColorTrack(Track));
	}

	Snapshot.CameraPositionSamples.Reserve(RuntimeCache.CameraPositionSamples.Num());
	for (const FCameraVectorSample& Sample : RuntimeCache.CameraPositionSamples)
	{
		FWoWLoginSceneCameraVectorSample PresetSample;
		PresetSample.TimeMs = Sample.TimeMs;
		PresetSample.Value = Sample.Value;
		Snapshot.CameraPositionSamples.Add(PresetSample);
	}

	Snapshot.CameraTargetSamples.Reserve(RuntimeCache.CameraTargetSamples.Num());
	for (const FCameraVectorSample& Sample : RuntimeCache.CameraTargetSamples)
	{
		FWoWLoginSceneCameraVectorSample PresetSample;
		PresetSample.TimeMs = Sample.TimeMs;
		PresetSample.Value = Sample.Value;
		Snapshot.CameraTargetSamples.Add(PresetSample);
	}

	Snapshot.CameraRollSamples.Reserve(RuntimeCache.CameraRollSamples.Num());
	for (const FCameraRollSample& Sample : RuntimeCache.CameraRollSamples)
	{
		FWoWLoginSceneCameraRollSample PresetSample;
		PresetSample.TimeMs = Sample.TimeMs;
		PresetSample.Degrees = Sample.Degrees;
		Snapshot.CameraRollSamples.Add(PresetSample);
	}

	Snapshot.LoginBones.Reserve(RuntimeCache.LoginBones.Num());
	for (const FLoginBoneInfo& Bone : RuntimeCache.LoginBones)
	{
		FWoWLoginSceneBoneInfo PresetBone;
		PresetBone.Index = Bone.Index;
		PresetBone.KeyBoneId = Bone.KeyBoneId;
		PresetBone.Flags = Bone.Flags;
		PresetBone.Parent = Bone.Parent;
		PresetBone.GeosetId = Bone.GeosetId;
		PresetBone.Pivot = Bone.Pivot;
		Snapshot.LoginBones.Add(PresetBone);
	}

	Snapshot.BoneAnimationTracks.Reserve(RuntimeCache.BoneAnimationSamples.Num());
	for (const TPair<int32, TArray<FLoginBoneFrameSample>>& Pair : RuntimeCache.BoneAnimationSamples)
	{
		FWoWLoginSceneBoneAnimationTrack Track;
		Track.BoneIndex = Pair.Key;
		Track.Samples.Reserve(Pair.Value.Num());
		for (const FLoginBoneFrameSample& Sample : Pair.Value)
		{
			FWoWLoginSceneBoneFrameSample PresetSample;
			PresetSample.FrameIndex = Sample.FrameIndex;
			PresetSample.TimeMs = Sample.TimeMs;
			PresetSample.Transform = Sample.Transform;
			Track.Samples.Add(PresetSample);
		}
		Snapshot.BoneAnimationTracks.Add(MoveTemp(Track));
	}

	Snapshot.LoginSceneLights.Reserve(RuntimeCache.LoginSceneLights.Num());
	for (const FLoginSceneLight& Light : RuntimeCache.LoginSceneLights)
	{
		FWoWLoginSceneLightSnapshot PresetLight;
		PresetLight.Index = Light.Index;
		PresetLight.LightType = Light.LightType;
		PresetLight.BoneIndex = Light.BoneIndex;
		PresetLight.Position = Light.Position;
		PresetLight.AmbientColor = Light.AmbientColor;
		PresetLight.AmbientIntensity = Light.AmbientIntensity;
		PresetLight.DiffuseColor = Light.DiffuseColor;
		PresetLight.DiffuseIntensity = Light.DiffuseIntensity;
		PresetLight.AttenuationStart = Light.AttenuationStart;
		PresetLight.AttenuationEnd = Light.AttenuationEnd;
		Snapshot.LoginSceneLights.Add(PresetLight);
	}

	Snapshot.ParticleEmitters.Reserve(RuntimeCache.ParticleEmitters.Num());
	for (const FLoginParticleEmitter& Emitter : RuntimeCache.ParticleEmitters)
	{
		Snapshot.ParticleEmitters.Add(MakePresetParticleEmitter(Emitter));
	}
}

bool AWoWLoginGameMode::SaveLoginScenePresetAsset(FString* OutStatusMessage)
{
	return SaveLoginScenePresetAssetForIndex(ActiveLoginSceneIndex, OutStatusMessage);
}

bool AWoWLoginGameMode::SaveAllLoginScenePresetAssets(FString* OutStatusMessage)
{
#if WITH_EDITOR
	int32 SuccessCount = 0;
	int32 FailureCount = 0;
	TArray<FString> FailureMessages;
	for (int32 SceneIndex = 0; SceneIndex < LoginSceneDefinitions.Num(); ++SceneIndex)
	{
		FString SceneStatus;
		if (SaveLoginScenePresetAssetForIndex(SceneIndex, &SceneStatus))
		{
			++SuccessCount;
		}
		else
		{
			++FailureCount;
			FailureMessages.Add(SceneStatus);
		}
	}

	if (OutStatusMessage)
	{
		*OutStatusMessage = FString::Printf(
			TEXT("批量烘焙完成：成功=%d 失败=%d 场景总数=%d"),
			SuccessCount,
			FailureCount,
			LoginSceneDefinitions.Num());
		if (!FailureMessages.IsEmpty())
		{
			*OutStatusMessage += TEXT(" | ");
			*OutStatusMessage += FString::Join(FailureMessages, TEXT("；"));
		}
	}
	UE_LOG(LogTemp, Display, TEXT("登录场景Preset批量烘焙完成: success=%d failed=%d total=%d"),
		SuccessCount,
		FailureCount,
		LoginSceneDefinitions.Num());
	return FailureCount == 0;
#else
	if (OutStatusMessage)
	{
		*OutStatusMessage = TEXT("批量烘焙失败：非编辑器构建不能保存资产。");
	}
	return false;
#endif
}

bool AWoWLoginGameMode::SaveLoginScenePresetAssetForIndex(int32 SceneIndex, FString* OutStatusMessage)
{
#if WITH_EDITOR
	if (!LoginSceneDefinitions.IsValidIndex(SceneIndex))
	{
		if (OutStatusMessage)
		{
			*OutStatusMessage = TEXT("烘焙失败：当前没有有效登录场景。");
		}
		return false;
	}

	FLoginSceneDefinition& SceneDefinition = LoginSceneDefinitions[SceneIndex];
	UWoWLoginScenePresetAsset* PresetAsset = LoadOrCreateObjectAsset<UWoWLoginScenePresetAsset>(*SceneDefinition.PresetAssetPath, *SceneDefinition.PresetAssetPackagePath);
	if (!PresetAsset)
	{
		UE_LOG(LogTemp, Warning, TEXT("登录场景Preset资产创建失败: %s"), *SceneDefinition.PresetAssetPath);
		if (OutStatusMessage)
		{
			*OutStatusMessage = FString::Printf(TEXT("烘焙失败：Preset资产创建失败 %s"), *SceneDefinition.PresetAssetPath);
		}
		return false;
	}

	if (SceneIndex == ActiveLoginSceneIndex)
	{
		LoginScenePresetAsset = PresetAsset;
	}
	PresetAsset->RequiredAssets.Reset();
	PresetAsset->SceneKey = SceneDefinition.Key;
	PresetAsset->DisplayName = SceneDefinition.DisplayName;
	PresetAsset->JsonRelativePath = SceneDefinition.JsonRelativePath;
	PresetAsset->MeshPath = SceneDefinition.MeshPath;
	PresetAsset->AnimationPath = SceneDefinition.AnimationPath;
	PresetAsset->MaterialPackagePath = SceneDefinition.MaterialPackagePath;
	PresetAsset->TuningAssetPath = SceneDefinition.TuningAssetPath;
	if (SceneIndex == ActiveLoginSceneIndex)
	{
		PresetAsset->Tuning = LoginSceneTuningState;
	}
	else
	{
		if (UWoWLoginSceneTuningAsset* SceneTuningAsset = LoadObject<UWoWLoginSceneTuningAsset>(nullptr, *SceneDefinition.TuningAssetPath))
		{
			PresetAsset->Tuning = SceneTuningAsset->Tuning;
		}
		else
		{
			PresetAsset->Tuning = FWoWLoginSceneTuningState();
		}
	}

	FString JsonText;
	if (FFileHelper::LoadFileToString(JsonText, *MakeSavedJsonPathFromRelativePath(SceneDefinition.JsonRelativePath)))
	{
		PresetAsset->SourceJsonCrc = FCrc::StrCrc32(*JsonText);
	}
	else
	{
		PresetAsset->SourceJsonCrc = 0;
	}

	FLoginSceneRuntimeCache RuntimeSnapshot;
	if (SceneIndex == ActiveLoginSceneIndex)
	{
		RuntimeSnapshot.bCameraLoaded = true;
		RuntimeSnapshot.CameraId = BaseCameraId;
		RuntimeSnapshot.CameraPosition = BaseCameraPosition;
		RuntimeSnapshot.CameraTarget = BaseCameraTarget;
		RuntimeSnapshot.CameraFovDegrees = BaseCameraFovDegrees;
		RuntimeSnapshot.CameraNearClip = BaseCameraNearClip;
		RuntimeSnapshot.CameraFarClip = BaseCameraFarClip;
		RuntimeSnapshot.bRuntimeLoaded = !RenderSections.IsEmpty() || !TextureTransformTracks.IsEmpty() || !ParticleEmitters.IsEmpty();
		RuntimeSnapshot.TextureTransformTracks = TextureTransformTracks;
		RuntimeSnapshot.RenderSections = RenderSections;
		RuntimeSnapshot.ColorTracks = ColorTracks;
		RuntimeSnapshot.LateAdditiveOverlaySlots = LoginSceneLateAdditiveOverlaySlots;
		RuntimeSnapshot.CameraPositionSamples = CameraPositionSamples;
		RuntimeSnapshot.CameraTargetSamples = CameraTargetSamples;
		RuntimeSnapshot.CameraRollSamples = CameraRollSamples;
		RuntimeSnapshot.BonePivots = BonePivots;
		RuntimeSnapshot.LoginBones = LoginBones;
		RuntimeSnapshot.BoneAnimationSamples = BoneAnimationSamples;
		RuntimeSnapshot.LoginSceneLights = LoginSceneLights;
		RuntimeSnapshot.ParticleEmitters = ParticleEmitters;
		RuntimeSnapshot.CameraTrackDurationMs = CameraTrackDurationMs;
	}

	if (!RuntimeSnapshot.bRuntimeLoaded)
	{
		LoginSceneRuntimeCaches.Remove(SceneIndex);
		TGuardValue<bool> BypassPresetCacheForBakeGuard(bBypassLoginScenePresetCacheForBake, true);
		BuildLoginSceneRuntimeCache(SceneIndex);
	}
	if (!RuntimeSnapshot.bRuntimeLoaded)
	{
		if (const FLoginSceneRuntimeCache* RuntimeCache = LoginSceneRuntimeCaches.Find(SceneIndex))
		{
			RuntimeSnapshot = *RuntimeCache;
		}
	}
	SaveLoginScenePresetRuntimeCache(PresetAsset, RuntimeSnapshot);

	TArray<FSoftObjectPath> RequiredAssets;
	CollectLoginSceneAsyncAssets(SceneIndex, RequiredAssets, false);
	RequiredAssets.Remove(FSoftObjectPath(SceneDefinition.PresetAssetPath));
	PresetAsset->RequiredAssets.Reset();
	for (const FSoftObjectPath& RequiredAsset : RequiredAssets)
	{
		const FString ObjectPath = RequiredAsset.ToString();
		if (DoesGeneratedAssetExist(ObjectPath))
		{
			PresetAsset->RequiredAssets.AddUnique(RequiredAsset);
		}
		else
		{
			UE_LOG(LogTemp, Verbose, TEXT("登录场景Preset依赖跳过不存在资产: %s"), *ObjectPath);
		}
	}

	PresetAsset->MarkPackageDirty();
	UPackage* Package = PresetAsset->GetPackage();
	if (!Package)
	{
		if (OutStatusMessage)
		{
			*OutStatusMessage = TEXT("烘焙失败：Preset资产没有有效Package。");
		}
		return false;
	}
	Package->MarkPackageDirty();

	const FString PackageName = Package->GetName();
	FString PackageFileName;
	if (!FPackageName::TryConvertLongPackageNameToFilename(PackageName, PackageFileName, FPackageName::GetAssetPackageExtension()))
	{
		if (OutStatusMessage)
		{
			*OutStatusMessage = FString::Printf(TEXT("烘焙失败：无法解析Package文件名 %s"), *PackageName);
		}
		return false;
	}

	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	const bool bSaved = UPackage::SavePackage(Package, PresetAsset, *PackageFileName, SaveArgs);
	UE_LOG(LogTemp, Display, TEXT("登录场景Preset资产已保存: scene=%s required=%d textureTracks=%d renderSections=%d particles=%d path=%s"),
		*SceneDefinition.DisplayName,
		PresetAsset->RequiredAssets.Num(),
		PresetAsset->RuntimeCache.TextureTransformTracks.Num(),
		PresetAsset->RuntimeCache.RenderSections.Num(),
		PresetAsset->RuntimeCache.ParticleEmitters.Num(),
		*SceneDefinition.PresetAssetPath);
	if (OutStatusMessage)
	{
		*OutStatusMessage = FString::Printf(
			TEXT("%s：%s | 依赖=%d RenderSections=%d 粒子=%d"),
			bSaved ? TEXT("烘焙完成") : TEXT("烘焙失败"),
			*SceneDefinition.PresetAssetPath,
			PresetAsset->RequiredAssets.Num(),
			PresetAsset->RuntimeCache.RenderSections.Num(),
			PresetAsset->RuntimeCache.ParticleEmitters.Num());
	}
	return bSaved;
#else
	if (OutStatusMessage)
	{
		*OutStatusMessage = TEXT("烘焙失败：非编辑器构建不能保存资产。");
	}
	return false;
#endif
}

const TArray<FWoWLoginParticleEmitterTuning>& AWoWLoginGameMode::GetLoginParticleEmitterTunings() const
{
	return LoginSceneTuningState.ParticleEmitterTunings;
}

const AWoWLoginGameMode::FLoginParticleEmitter* AWoWLoginGameMode::GetLoginParticleEmitterByIndex(int32 EmitterIndex) const
{
	for (const FLoginParticleEmitter& Emitter : ParticleEmitters)
	{
		if (Emitter.Index == EmitterIndex)
		{
			return &Emitter;
		}
	}
	return nullptr;
}

TArray<int32> AWoWLoginGameMode::GetLoginParticleEmitterIndices() const
{
	TArray<int32> Indices;
	Indices.Reserve(ParticleEmitters.Num());
	for (const FLoginParticleEmitter& Emitter : ParticleEmitters)
	{
		Indices.Add(Emitter.Index);
	}
	return Indices;
}

FWoWLoginParticleEmitterTuning AWoWLoginGameMode::GetLoginParticleEmitterTuning(int32 EmitterIndex) const
{
	for (const FWoWLoginParticleEmitterTuning& Tuning : LoginSceneTuningState.ParticleEmitterTunings)
	{
		if (Tuning.EmitterIndex == EmitterIndex)
		{
			return Tuning;
		}
	}

	FWoWLoginParticleEmitterTuning DefaultTuning;
	DefaultTuning.EmitterIndex = EmitterIndex;
	return DefaultTuning;
}

void AWoWLoginGameMode::SetLoginParticleEmitterTuning(const FWoWLoginParticleEmitterTuning& NewTuning)
{
	if (NewTuning.EmitterIndex == INDEX_NONE)
	{
		return;
	}

	FWoWLoginSceneTuningState NewState = LoginSceneTuningState;
	bool bUpdated = false;
	for (FWoWLoginParticleEmitterTuning& Tuning : NewState.ParticleEmitterTunings)
	{
		if (Tuning.EmitterIndex == NewTuning.EmitterIndex)
		{
			Tuning = NewTuning;
			bUpdated = true;
			break;
		}
	}
	if (!bUpdated)
	{
		NewState.ParticleEmitterTunings.Add(NewTuning);
	}

	SetLoginSceneTuningState(NewState);

	for (FLoginParticleSystem& System : ParticleSystems)
	{
			if (System.Emitter.Index == NewTuning.EmitterIndex && System.DynamicMaterial)
			{
				System.DynamicMaterial->SetScalarParameterValue(
					TEXT("Brightness"),
					GetLoginParticleBaseBrightness(System.Emitter) * FMath::Max(NewTuning.BrightnessScale, 0.0f));
				System.DynamicMaterial->SetScalarParameterValue(
					TEXT("AlphaCutoff"),
					FMath::Clamp(NewTuning.AlphaCutoff, 0.0f, 0.95f));
				System.DynamicMaterial->SetScalarParameterValue(
					TEXT("LegacyGammaWeight"),
					GetLoginParticleLegacyGammaWeight(System.Emitter));
			}
		}
	}

void AWoWLoginGameMode::ResetLoginParticleEmitterTuning(int32 EmitterIndex)
{
	FWoWLoginSceneTuningState NewState = LoginSceneTuningState;
	for (FWoWLoginParticleEmitterTuning& Tuning : NewState.ParticleEmitterTunings)
	{
		if (Tuning.EmitterIndex == EmitterIndex)
		{
			const FString Note = Tuning.Note;
			Tuning = FWoWLoginParticleEmitterTuning();
			Tuning.EmitterIndex = EmitterIndex;
			Tuning.Note = Note;
			break;
		}
	}
	SetLoginSceneTuningState(NewState);
}

void AWoWLoginGameMode::ResetAllLoginParticleEmitterTunings()
{
	FWoWLoginSceneTuningState NewState = LoginSceneTuningState;
	for (FWoWLoginParticleEmitterTuning& Tuning : NewState.ParticleEmitterTunings)
	{
		const int32 EmitterIndex = Tuning.EmitterIndex;
		const FString Note = Tuning.Note;
		Tuning = FWoWLoginParticleEmitterTuning();
		Tuning.EmitterIndex = EmitterIndex;
		Tuning.Note = Note;
	}
	SetLoginSceneTuningState(NewState);
}

int32 AWoWLoginGameMode::GetLoginParticleCount(int32 EmitterIndex) const
{
	for (const FLoginParticleSystem& System : ParticleSystems)
	{
		if (System.Emitter.Index == EmitterIndex)
		{
			return System.Particles.Num();
		}
	}
	return 0;
}

TArray<int32> AWoWLoginGameMode::GetLoginBoneIndices() const
{
	TArray<int32> Indices;
	Indices.Reserve(LoginBones.Num());
	for (const FLoginBoneInfo& BoneInfo : LoginBones)
	{
		Indices.Add(BoneInfo.Index);
	}
	Indices.Sort();
	return Indices;
}

const AWoWLoginGameMode::FLoginBoneInfo* AWoWLoginGameMode::GetLoginBoneInfoByIndex(int32 BoneIndex) const
{
	return LoginBones.FindByPredicate([BoneIndex](const FLoginBoneInfo& BoneInfo)
	{
		return BoneInfo.Index == BoneIndex;
	});
}

AWoWLoginGameMode::FLoginBoneDebugSummary AWoWLoginGameMode::GetLoginBoneDebugSummary(int32 BoneIndex) const
{
	FLoginBoneDebugSummary Summary;
	Summary.Index = BoneIndex;
	if (const FLoginBoneInfo* BoneInfo = GetLoginBoneInfoByIndex(BoneIndex))
	{
		Summary.Parent = BoneInfo->Parent;
		Summary.Flags = BoneInfo->Flags;
		Summary.GeosetId = BoneInfo->GeosetId;
		Summary.Pivot = BoneInfo->Pivot;
	}

	const TArray<FLoginBoneFrameSample>* Samples = BoneAnimationSamples.Find(BoneIndex);
	if (!Samples || Samples->IsEmpty())
	{
		return Summary;
	}

	Summary.SampleCount = Samples->Num();
	Summary.DurationMs = Samples->Last().TimeMs;
	Summary.FirstTranslation = (*Samples)[0].Transform.GetTranslation();
	Summary.LastTranslation = Samples->Last().Transform.GetTranslation();
	Summary.MinTranslation = Summary.FirstTranslation;
	Summary.MaxTranslation = Summary.FirstTranslation;
	for (const FLoginBoneFrameSample& Sample : *Samples)
	{
		const FVector Translation = Sample.Transform.GetTranslation();
		Summary.MinTranslation.X = FMath::Min(Summary.MinTranslation.X, Translation.X);
		Summary.MinTranslation.Y = FMath::Min(Summary.MinTranslation.Y, Translation.Y);
		Summary.MinTranslation.Z = FMath::Min(Summary.MinTranslation.Z, Translation.Z);
		Summary.MaxTranslation.X = FMath::Max(Summary.MaxTranslation.X, Translation.X);
		Summary.MaxTranslation.Y = FMath::Max(Summary.MaxTranslation.Y, Translation.Y);
		Summary.MaxTranslation.Z = FMath::Max(Summary.MaxTranslation.Z, Translation.Z);
	}
	Summary.bHasAnimation = !Summary.MinTranslation.Equals(Summary.MaxTranslation, 0.01f);
	return Summary;
}

TArray<int32> AWoWLoginGameMode::GetLoginTextureSectionIndices() const
{
	TArray<int32> Indices;
	for (const FSectionTextureTransformTrack& Track : TextureTransformTracks)
	{
		Indices.Add(Track.SlotIndex);
	}
	Indices.Sort();
	return Indices;
}

const AWoWLoginGameMode::FSectionTextureTransformTrack* AWoWLoginGameMode::GetLoginTextureSectionTrackByIndex(int32 SectionIndex) const
{
	return TextureTransformTracks.FindByPredicate([SectionIndex](const FSectionTextureTransformTrack& Track)
	{
		return Track.SlotIndex == SectionIndex;
	});
}

TArray<int32> AWoWLoginGameMode::GetLoginRenderSectionIndices() const
{
	TArray<int32> Indices;
	for (const FSectionTextureTransformTrack& Section : RenderSections)
	{
		Indices.Add(Section.SlotIndex);
	}
	Indices.Sort();
	return Indices;
}

const AWoWLoginGameMode::FSectionTextureTransformTrack* AWoWLoginGameMode::GetLoginRenderSectionByIndex(int32 SectionIndex) const
{
	if (const FSectionTextureTransformTrack* Section = RenderSections.FindByPredicate([SectionIndex](const FSectionTextureTransformTrack& Candidate)
	{
		return Candidate.SlotIndex == SectionIndex;
	}))
	{
		return Section;
	}

	return GetLoginTextureSectionTrackByIndex(SectionIndex);
}

FWoWLoginTextureSectionTuning AWoWLoginGameMode::GetLoginTextureSectionTuning(int32 SectionIndex) const
{
	for (const FWoWLoginTextureSectionTuning& Tuning : LoginSceneTuningState.TextureSectionTunings)
	{
		if (Tuning.SectionIndex == SectionIndex)
		{
			return Tuning;
		}
	}

	FWoWLoginTextureSectionTuning DefaultTuning;
	DefaultTuning.SectionIndex = SectionIndex;
	return DefaultTuning;
}

FWoWLoginLightTuning AWoWLoginGameMode::GetLoginSceneLightTuning(int32 LightIndex) const
{
	for (const FWoWLoginLightTuning& Tuning : LoginSceneTuningState.LightTunings)
	{
		if (Tuning.LightIndex == LightIndex)
		{
			return Tuning;
		}
	}

	FWoWLoginLightTuning DefaultTuning;
	DefaultTuning.LightIndex = LightIndex;
	return DefaultTuning;
}

void AWoWLoginGameMode::SetLoginSceneLightTuning(const FWoWLoginLightTuning& NewTuning)
{
	FWoWLoginSceneTuningState NewState = LoginSceneTuningState;
	bool bUpdated = false;
	for (FWoWLoginLightTuning& Tuning : NewState.LightTunings)
	{
		if (Tuning.LightIndex == NewTuning.LightIndex)
		{
			Tuning = NewTuning;
			bUpdated = true;
			break;
		}
	}
	if (!bUpdated)
	{
		NewState.LightTunings.Add(NewTuning);
	}
	SetLoginSceneTuningState(NewState);
	SpawnLoginSceneLights();
}

void AWoWLoginGameMode::ResetLoginSceneLightTuning(int32 LightIndex)
{
	FWoWLoginSceneTuningState NewState = LoginSceneTuningState;
	for (FWoWLoginLightTuning& Tuning : NewState.LightTunings)
	{
		if (Tuning.LightIndex == LightIndex)
		{
			const FString Note = Tuning.Note;
			Tuning = FWoWLoginLightTuning();
			Tuning.LightIndex = LightIndex;
			Tuning.Note = Note;
			break;
		}
	}
	SetLoginSceneTuningState(NewState);
	SpawnLoginSceneLights();
}

void AWoWLoginGameMode::ResetAllLoginSceneLightTunings()
{
	FWoWLoginSceneTuningState NewState = LoginSceneTuningState;
	for (FWoWLoginLightTuning& Tuning : NewState.LightTunings)
	{
		const int32 LightIndex = Tuning.LightIndex;
		const FString Note = Tuning.Note;
		Tuning = FWoWLoginLightTuning();
		Tuning.LightIndex = LightIndex;
		Tuning.Note = Note;
	}
	NewState.GlobalBrightnessScale = 1.0f;
	NewState.M2PhysicalLightIntensityScale = 80.0f;
	SetLoginSceneTuningState(NewState);
	SpawnLoginSceneLights();
}

void AWoWLoginGameMode::SetLoginTextureSectionTuning(const FWoWLoginTextureSectionTuning& NewTuning)
{
	FWoWLoginSceneTuningState NewState = LoginSceneTuningState;
	bool bUpdated = false;
	for (FWoWLoginTextureSectionTuning& Tuning : NewState.TextureSectionTunings)
	{
		if (Tuning.SectionIndex == NewTuning.SectionIndex)
		{
			Tuning = NewTuning;
			bUpdated = true;
			break;
		}
	}
	if (!bUpdated)
	{
		NewState.TextureSectionTunings.Add(NewTuning);
	}
	SetLoginSceneTuningState(NewState);
	ApplyTextureSectionRenderOverride(NewTuning.SectionIndex);
	ApplyTextureSectionVisibility(NewTuning.SectionIndex);
}

void AWoWLoginGameMode::ResetLoginTextureSectionTuning(int32 SectionIndex)
{
	FWoWLoginSceneTuningState NewState = LoginSceneTuningState;
	for (FWoWLoginTextureSectionTuning& Tuning : NewState.TextureSectionTunings)
	{
		if (Tuning.SectionIndex == SectionIndex)
		{
			const FString Note = Tuning.Note;
			Tuning = FWoWLoginTextureSectionTuning();
			Tuning.SectionIndex = SectionIndex;
			Tuning.Note = Note;
			break;
		}
	}
	SetLoginSceneTuningState(NewState);
	ApplyTextureSectionRenderOverride(SectionIndex);
	ApplyTextureSectionVisibility(SectionIndex);
}

void AWoWLoginGameMode::ResetAllLoginTextureSectionTunings()
{
	FWoWLoginSceneTuningState NewState = LoginSceneTuningState;
	for (FWoWLoginTextureSectionTuning& Tuning : NewState.TextureSectionTunings)
	{
		const int32 SectionIndex = Tuning.SectionIndex;
		const FString Note = Tuning.Note;
		Tuning = FWoWLoginTextureSectionTuning();
		Tuning.SectionIndex = SectionIndex;
		Tuning.Note = Note;
	}
	SetLoginSceneTuningState(NewState);
	ReapplyLoginSceneRenderOverrides();
}

void AWoWLoginGameMode::SetLoginTextureDebugHighlightSection(int32 SectionIndex)
{
	HighlightedTextureSectionIndex = SectionIndex;
	if (SectionIndex != INDEX_NONE && !LoginSceneDebugHighlightComponent.IsValid())
	{
		if (LoginSceneActor.IsValid())
		{
			CreateLoginSceneDebugHighlightOverlay(LoginSceneActor->GetSkeletalMeshComponent());
		}
	}

	USkeletalMeshComponent* HighlightComponent = LoginSceneDebugHighlightComponent.Get();
	if (!HighlightComponent)
	{
		return;
	}

	const int32 MaterialSlotCount = HighlightComponent->GetNumMaterials();
	for (int32 SlotIndex = 0; SlotIndex < MaterialSlotCount; ++SlotIndex)
	{
		HighlightComponent->ShowMaterialSection(SlotIndex, SlotIndex, SlotIndex == SectionIndex, 0);
	}

	const FSectionTextureTransformTrack* Track = GetLoginTextureSectionTrackByIndex(SectionIndex);
	if (!Track || !LoginSceneDebugHighlightDynamicMaterials.IsValidIndex(SectionIndex))
	{
		return;
	}

	UMaterialInstanceDynamic* DynamicMaterial = LoginSceneDebugHighlightDynamicMaterials[SectionIndex];
	UTexture2D* Texture = LoadGeneratedTextureFromPreviewPng(Track->PreviewPng);
	if (DynamicMaterial && Texture)
	{
		DynamicMaterial->SetTextureParameterValue(TEXT("DiffuseTexture"), Texture);
		DynamicMaterial->SetVectorParameterValue(TEXT("TintColor"), FLinearColor(0.15f, 0.95f, 1.0f, 1.0f));
		DynamicMaterial->SetScalarParameterValue(TEXT("Brightness"), 6.0f);
	}
}

void AWoWLoginGameMode::BuildLoginSceneDefinitions()
{
	LoginSceneDefinitions.Reset();

	auto AddDefinition = [this](const FString& ModelName, const FString& JsonRelativePath, const FString& MeshPackage, const FString& AnimationPackage, const FString& MaterialPackagePath, const FString& TuningPackagePath, const FString& PresetPackagePath)
	{
		FLoginSceneDefinition Definition;
		Definition.Key = MakeLoginSceneKeyFromJsonRelativePath(JsonRelativePath);
		Definition.DisplayName = MakeDefaultLoginSceneDisplayName(ModelName);
		Definition.MeshPath = MakeObjectPathFromPackagePath(MeshPackage);
		Definition.AnimationPath = MakeObjectPathFromPackagePath(AnimationPackage);
		Definition.JsonRelativePath = JsonRelativePath;
		Definition.MaterialPackagePath = MaterialPackagePath;
		Definition.TuningAssetPackagePath = TuningPackagePath;
		Definition.TuningAssetPath = MakeObjectPathFromPackagePath(TuningPackagePath);
		Definition.PresetAssetPackagePath = PresetPackagePath;
		Definition.PresetAssetPath = MakeObjectPathFromPackagePath(PresetPackagePath);
		if (!DoesGeneratedAssetExist(Definition.MeshPath))
		{
			return;
		}

		const bool bAlreadyExists = LoginSceneDefinitions.ContainsByPredicate([&Definition](const FLoginSceneDefinition& Existing)
		{
			return Existing.Key.Equals(Definition.Key, ESearchCase::IgnoreCase);
		});
		if (!bAlreadyExists)
		{
			LoginSceneDefinitions.Add(MoveTemp(Definition));
		}
	};

	// 中文说明：Northrend 是历史上先生成的资产，保留它的原始包路径，避免破坏已经保存的调参资产。
	AddDefinition(
		TEXT("UI_MainMenu_Northrend"),
		TEXT("M2Preview/Interface/GLUES/MODELS/UI_MAINMENU_NORTHREND/UI_MainMenu_Northrend.mesh.json"),
		TEXT("/Game/WoW/Generated/Interface/GLUES/MODELS/UI_MAINMENU_NORTHREND/UI_MainMenu_Northrend_M2Mesh"),
		TEXT("/Game/WoW/Generated/Interface/GLUES/MODELS/UI_MAINMENU_NORTHREND/Animations/A_UI_MainMenu_Northrend_000_Stand_00"),
		TEXT("/Game/WoW/Generated/Interface/GLUES/MODELS/UI_MAINMENU_NORTHREND/Materials"),
		TEXT("/Game/WoW/Generated/Interface/GLUES/MODELS/UI_MAINMENU_NORTHREND/DA_UI_MainMenu_Northrend_Tuning"),
		TEXT("/Game/WoW/Generated/Interface/GLUES/MODELS/UI_MAINMENU_NORTHREND/DA_UI_MainMenu_Northrend_LoginScenePreset"));

	// 中文说明：后续登录界面默认按 WoWSkeletalAssetAudit 的 ModelName 规则生成到 /Game/WoW/Generated/<ModelName>。
	AddDefinition(
		TEXT("UI_MainMenu"),
		TEXT("M2Preview/Interface/GLUES/MODELS/UI_MAINMENU/UI_MainMenu.mesh.json"),
		MakeGeneratedMeshPackage(TEXT("UI_MainMenu")),
		MakeGeneratedAnimationPackage(TEXT("UI_MainMenu")),
		MakeGeneratedMaterialPackagePath(TEXT("UI_MainMenu")),
		MakeGeneratedTuningPackage(TEXT("UI_MainMenu")),
		MakeGeneratedPresetPackage(TEXT("UI_MainMenu")));

	TArray<FString> JsonFiles;
	const FString PreviewRoot = FPaths::ProjectSavedDir() / TEXT("M2Preview/Interface/GLUES/MODELS");
	IFileManager::Get().FindFilesRecursive(JsonFiles, *PreviewRoot, TEXT("*.mesh.json"), true, false);
	for (const FString& JsonFile : JsonFiles)
	{
		if (!IsGeneratedGlueMeshJsonCandidate(JsonFile))
		{
			continue;
		}

		const FString ModelName = FPaths::GetBaseFilename(FPaths::GetBaseFilename(JsonFile));
		AddDefinition(
			ModelName,
			MakeJsonRelativePathFromFullPath(JsonFile),
			MakeGeneratedInterfaceGlueModelMeshPackage(ModelName),
			MakeGeneratedInterfaceGlueModelAnimationPackage(ModelName),
			MakeGeneratedInterfaceGlueModelMaterialPackagePath(ModelName),
			MakeGeneratedInterfaceGlueModelTuningPackage(ModelName),
			MakeGeneratedInterfaceGlueModelPresetPackage(ModelName));
	}

	ActiveLoginSceneIndex = LoginSceneDefinitions.IsValidIndex(ActiveLoginSceneIndex) ? ActiveLoginSceneIndex : 0;
	LoginScreenSceneIndex = ActiveLoginSceneIndex;
	UE_LOG(LogTemp, Display, TEXT("登录界面可用资产数量: %d"), LoginSceneDefinitions.Num());
}

void AWoWLoginGameMode::ReloadActiveLoginScene()
{
	const double ReloadStartSeconds = FPlatformTime::Seconds();
	SetLoginTextureDebugHighlightSection(INDEX_NONE);

	const double DestroyStartSeconds = FPlatformTime::Seconds();
	for (FLoginParticleSystem& System : ParticleSystems)
	{
		if (System.MeshComponent)
		{
			System.MeshComponent->DestroyComponent();
		}
	}
	ParticleSystems.Reset();

	for (FLoginSceneLightComponent& LightComponent : LoginSceneLightComponents)
	{
		if (LightComponent.Component.IsValid())
		{
			LightComponent.Component->DestroyComponent();
		}
	}
	LoginSceneLightComponents.Reset();
	if (LoginSceneAmbientLightComponent.IsValid())
	{
		LoginSceneAmbientLightComponent->DestroyComponent();
	}
	LoginSceneAmbientLightComponent.Reset();

	if (LoginSceneDebugHighlightComponent.IsValid())
	{
		LoginSceneDebugHighlightComponent->DestroyComponent();
	}
	LoginSceneDebugHighlightComponent.Reset();
	LoginSceneDebugHighlightDynamicMaterials.Reset();

	if (LoginSceneLateAdditiveOverlayComponent.IsValid())
	{
		LoginSceneLateAdditiveOverlayComponent->DestroyComponent();
	}
	LoginSceneLateAdditiveOverlayComponent.Reset();
	LoginSceneLateAdditiveOverlayDynamicMaterials.Reset();

	if (LoginSceneActor.IsValid())
	{
		LoginSceneActor->Destroy();
	}
	LoginSceneActor.Reset();
	LoginSceneDynamicMaterials.Reset();

	if (LoginCameraActor.IsValid())
	{
		LoginCameraActor->Destroy();
	}
	LoginCameraActor.Reset();
	const double DestroyElapsedMs = (FPlatformTime::Seconds() - DestroyStartSeconds) * 1000.0;

	const double TuningStartSeconds = FPlatformTime::Seconds();
	LoadOrCreateLoginSceneTuningAsset(true);
	const double TuningElapsedMs = (FPlatformTime::Seconds() - TuningStartSeconds) * 1000.0;

	const double SpawnStartSeconds = FPlatformTime::Seconds();
	SpawnLoginScene();
	const double SpawnElapsedMs = (FPlatformTime::Seconds() - SpawnStartSeconds) * 1000.0;

	const double CameraStartSeconds = FPlatformTime::Seconds();
	SetupLoginCamera();
	const double CameraElapsedMs = (FPlatformTime::Seconds() - CameraStartSeconds) * 1000.0;

	PresentLoginSceneVisuals();
	const double TotalElapsedMs = (FPlatformTime::Seconds() - ReloadStartSeconds) * 1000.0;

	const FLoginSceneDefinition* SceneDefinition = GetActiveLoginSceneDefinition(this);
	UE_LOG(LogTemp, Display, TEXT("Glue背景重载耗时: scene=%s total=%.2fms destroy=%.2fms tuning=%.2fms spawn=%.2fms camera=%.2fms cached=%d"),
		SceneDefinition ? *SceneDefinition->DisplayName : TEXT("<none>"),
		TotalElapsedMs,
		DestroyElapsedMs,
		TuningElapsedMs,
		SpawnElapsedMs,
		CameraElapsedMs,
		LoginSceneRuntimeCaches.Contains(ActiveLoginSceneIndex) ? 1 : 0);
}

void AWoWLoginGameMode::PrewarmLoginSceneRuntimeCaches()
{
	const int32 OriginalActiveIndex = ActiveLoginSceneIndex;
	const double StartSeconds = FPlatformTime::Seconds();
	int32 LoadedCount = 0;

	for (int32 SceneIndex = 0; SceneIndex < LoginSceneDefinitions.Num(); ++SceneIndex)
	{
		BuildLoginSceneRuntimeCache(SceneIndex);
		if (const FLoginSceneRuntimeCache* Cache = LoginSceneRuntimeCaches.Find(SceneIndex);
			Cache && (Cache->bCameraLoaded || Cache->bRuntimeLoaded))
		{
			++LoadedCount;
		}
	}

	ActiveLoginSceneIndex = OriginalActiveIndex;
	ApplyLoginSceneRuntimeCache(ActiveLoginSceneIndex);
	UE_LOG(LogTemp, Display, TEXT("Glue背景运行时缓存预热完成: scenes=%d loaded=%d elapsed=%.2fms"),
		LoginSceneDefinitions.Num(),
		LoadedCount,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
}

void AWoWLoginGameMode::BuildLoginSceneRuntimeCache(int32 SceneIndex)
{
	if (!LoginSceneDefinitions.IsValidIndex(SceneIndex))
	{
		return;
	}

	const int32 OriginalActiveIndex = ActiveLoginSceneIndex;
	const int32 OriginalBaseCameraId = BaseCameraId;
	FLoginSceneRuntimeCache OriginalRuntimeState;
	OriginalRuntimeState.bRuntimeLoaded = true;
	OriginalRuntimeState.TextureTransformTracks = TextureTransformTracks;
	OriginalRuntimeState.RenderSections = RenderSections;
	OriginalRuntimeState.ColorTracks = ColorTracks;
	OriginalRuntimeState.LateAdditiveOverlaySlots = LoginSceneLateAdditiveOverlaySlots;
	OriginalRuntimeState.CameraPositionSamples = CameraPositionSamples;
	OriginalRuntimeState.CameraTargetSamples = CameraTargetSamples;
	OriginalRuntimeState.CameraRollSamples = CameraRollSamples;
	OriginalRuntimeState.BonePivots = BonePivots;
	OriginalRuntimeState.LoginBones = LoginBones;
	OriginalRuntimeState.BoneAnimationSamples = BoneAnimationSamples;
	OriginalRuntimeState.LoginSceneLights = LoginSceneLights;
	OriginalRuntimeState.ParticleEmitters = ParticleEmitters;
	OriginalRuntimeState.CameraTrackDurationMs = CameraTrackDurationMs;

	ActiveLoginSceneIndex = SceneIndex;

	FVector CameraPosition = FVector::ZeroVector;
	FVector CameraTarget = FVector::ZeroVector;
	float CameraFovDegrees = 0.0f;
	float NearClip = 0.0f;
	float FarClip = 0.0f;
	const bool bCameraLoaded = LoadLoginM2Camera(CameraPosition, CameraTarget, CameraFovDegrees, NearClip, FarClip);
	const int32 CachedCameraId = BaseCameraId;
	const bool bRuntimeLoaded = LoadLoginM2RuntimeTracks();

	FLoginSceneRuntimeCache& Cache = LoginSceneRuntimeCaches.FindOrAdd(SceneIndex);
	Cache.bCameraLoaded = bCameraLoaded;
	Cache.CameraId = CachedCameraId;
	Cache.CameraPosition = CameraPosition;
	Cache.CameraTarget = CameraTarget;
	Cache.CameraFovDegrees = CameraFovDegrees;
	Cache.CameraNearClip = NearClip;
	Cache.CameraFarClip = FarClip;
	Cache.bRuntimeLoaded = bRuntimeLoaded;
	Cache.TextureTransformTracks = TextureTransformTracks;
	Cache.RenderSections = RenderSections;
	Cache.ColorTracks = ColorTracks;
	Cache.LateAdditiveOverlaySlots = LoginSceneLateAdditiveOverlaySlots;
	Cache.CameraPositionSamples = CameraPositionSamples;
	Cache.CameraTargetSamples = CameraTargetSamples;
	Cache.CameraRollSamples = CameraRollSamples;
	Cache.BonePivots = BonePivots;
	Cache.LoginBones = LoginBones;
	Cache.BoneAnimationSamples = BoneAnimationSamples;
	Cache.LoginSceneLights = LoginSceneLights;
	Cache.ParticleEmitters = ParticleEmitters;
	Cache.CameraTrackDurationMs = CameraTrackDurationMs;

	ActiveLoginSceneIndex = OriginalActiveIndex;
	BaseCameraId = OriginalBaseCameraId;
	TextureTransformTracks = OriginalRuntimeState.TextureTransformTracks;
	RenderSections = OriginalRuntimeState.RenderSections;
	ColorTracks = OriginalRuntimeState.ColorTracks;
	LoginSceneLateAdditiveOverlaySlots = OriginalRuntimeState.LateAdditiveOverlaySlots;
	CameraPositionSamples = OriginalRuntimeState.CameraPositionSamples;
	CameraTargetSamples = OriginalRuntimeState.CameraTargetSamples;
	CameraRollSamples = OriginalRuntimeState.CameraRollSamples;
	BonePivots = OriginalRuntimeState.BonePivots;
	LoginBones = OriginalRuntimeState.LoginBones;
	BoneAnimationSamples = OriginalRuntimeState.BoneAnimationSamples;
	LoginSceneLights = OriginalRuntimeState.LoginSceneLights;
	ParticleEmitters = OriginalRuntimeState.ParticleEmitters;
	CameraTrackDurationMs = OriginalRuntimeState.CameraTrackDurationMs;
}

void AWoWLoginGameMode::ProcessLoginSceneRuntimeCachePrewarmQueue()
{
	if (PendingLoginSceneRuntimeCachePrewarmIndices.IsEmpty())
	{
		return;
	}
	if (!LoginSceneActor.IsValid() || !LoginCameraActor.IsValid())
	{
		return;
	}
	if (ActiveLoginSceneStreamableHandle.IsValid() &&
		ActiveLoginSceneStreamableHandle->IsLoadingInProgress())
	{
		return;
	}

	const int32 SceneIndex = PendingLoginSceneRuntimeCachePrewarmIndices[0];
	PendingLoginSceneRuntimeCachePrewarmIndices.RemoveAt(0, 1, EAllowShrinking::No);
	if (!LoginSceneDefinitions.IsValidIndex(SceneIndex) || LoginSceneRuntimeCaches.Contains(SceneIndex))
	{
		return;
	}

	const int32 VisibleSceneIndex = ActiveLoginSceneIndex;
	const double StartSeconds = FPlatformTime::Seconds();
	BuildLoginSceneRuntimeCache(SceneIndex);
	UE_LOG(LogTemp, Verbose, TEXT("Glue背景运行时缓存后台预热: scene=%d visible=%d remaining=%d elapsed=%.2fms"),
		SceneIndex,
		VisibleSceneIndex,
		PendingLoginSceneRuntimeCachePrewarmIndices.Num(),
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
}

void AWoWLoginGameMode::PrewarmLoginSceneAssets()
{
	const double StartSeconds = FPlatformTime::Seconds();
	int32 MeshCount = 0;
	int32 AnimationCount = 0;
	int32 MaterialCount = 0;
	int32 TextureCount = 0;

	for (int32 SceneIndex = 0; SceneIndex < LoginSceneDefinitions.Num(); ++SceneIndex)
	{
		const FLoginSceneDefinition& Definition = LoginSceneDefinitions[SceneIndex];
		if (LoadObject<USkeletalMesh>(nullptr, *Definition.MeshPath))
		{
			++MeshCount;
		}
		if (LoadObject<UAnimSequence>(nullptr, *Definition.AnimationPath))
		{
			++AnimationCount;
		}
		if (const FLoginSceneRuntimeCache* Cache = LoginSceneRuntimeCaches.Find(SceneIndex))
		{
			for (const FSectionTextureTransformTrack& Section : Cache->RenderSections)
			{
				const FString MaterialPath = FString::Printf(
					TEXT("%s/MI_M2_Section_%03d.MI_M2_Section_%03d"),
					*Definition.MaterialPackagePath,
					Section.SlotIndex,
					Section.SlotIndex);
				if (LoadObject<UMaterialInterface>(nullptr, *MaterialPath))
				{
					++MaterialCount;
				}
				if (SelectTextureOverrideParentMaterial(
					EWoWLoginTextureRenderOverride::M2Original,
					Section.RenderMode,
					Section.MaterialFlags,
					false))
				{
					++MaterialCount;
				}
				if (LoadGeneratedTextureFromPreviewPng(Section.PreviewPng))
				{
					++TextureCount;
				}
			}
			for (const FLoginParticleEmitter& Emitter : Cache->ParticleEmitters)
			{
				if (LoadGeneratedTextureFromPreviewPng(Emitter.PreviewPng))
				{
					++TextureCount;
				}
				if (SelectParticleParentMaterial(Emitter.Blend))
				{
					++MaterialCount;
				}
			}
		}
	}

	UE_LOG(LogTemp, Display, TEXT("Glue背景资源预热完成: scenes=%d meshes=%d anims=%d materials=%d textures=%d elapsed=%.2fms"),
		LoginSceneDefinitions.Num(),
		MeshCount,
		AnimationCount,
		MaterialCount,
		TextureCount,
		(FPlatformTime::Seconds() - StartSeconds) * 1000.0);
}

void AWoWLoginGameMode::LoadOrCreateLoginSceneTuningAsset(bool bAllowSyncLoadIfMissing)
{
	const FLoginSceneDefinition* SceneDefinition = GetActiveLoginSceneDefinition(this);
	if (!SceneDefinition)
	{
		LoginSceneTuningAsset = nullptr;
		LoginScenePresetAsset = nullptr;
		LoginSceneTuningState = FWoWLoginSceneTuningState();
		return;
	}

	LoginSceneTuningAsset = ResolveLoadedObject<UWoWLoginSceneTuningAsset>(SceneDefinition->TuningAssetPath);
	if (!LoginSceneTuningAsset && bAllowSyncLoadIfMissing)
	{
		LoginSceneTuningAsset = LoadObject<UWoWLoginSceneTuningAsset>(nullptr, *SceneDefinition->TuningAssetPath);
	}
	if (!LoginSceneTuningAsset && !DoesGeneratedAssetExist(SceneDefinition->TuningAssetPath))
	{
		LoginSceneTuningAsset = LoadOrCreateObjectAsset<UWoWLoginSceneTuningAsset>(*SceneDefinition->TuningAssetPath, *SceneDefinition->TuningAssetPackagePath);
	}
	if (LoginSceneTuningAsset)
	{
		LoginSceneTuningState = LoginSceneTuningAsset->Tuning;
	}
	else
	{
		LoginSceneTuningState = FWoWLoginSceneTuningState();
		UE_LOG(LogTemp, Warning, TEXT("登录场景调参资产未预加载，使用默认调参且跳过同步加载: %s"), *SceneDefinition->TuningAssetPath);
	}

	LoginScenePresetAsset = ResolveLoginScenePresetAsset(ActiveLoginSceneIndex, bAllowSyncLoadIfMissing);
	if (LoginScenePresetAsset)
	{
		LoginSceneTuningState = LoginScenePresetAsset->Tuning;
		UE_LOG(LogTemp, Verbose, TEXT("登录场景调参来自Preset资产: %s"), *SceneDefinition->PresetAssetPath);
	}
}

UWoWLoginScenePresetAsset* AWoWLoginGameMode::ResolveLoginScenePresetAsset(int32 SceneIndex, bool bAllowSyncLoadIfMissing) const
{
	if (!LoginSceneDefinitions.IsValidIndex(SceneIndex))
	{
		return nullptr;
	}

	const FLoginSceneDefinition& SceneDefinition = LoginSceneDefinitions[SceneIndex];
	UWoWLoginScenePresetAsset* PresetAsset = ResolveLoadedObject<UWoWLoginScenePresetAsset>(SceneDefinition.PresetAssetPath);
	if (!PresetAsset && bAllowSyncLoadIfMissing && DoesGeneratedAssetExist(SceneDefinition.PresetAssetPath))
	{
		PresetAsset = LoadObject<UWoWLoginScenePresetAsset>(nullptr, *SceneDefinition.PresetAssetPath);
	}
	if (!PresetAsset)
	{
		return nullptr;
	}
	if (!PresetAsset->SceneKey.IsEmpty() && !PresetAsset->SceneKey.Equals(SceneDefinition.Key, ESearchCase::IgnoreCase))
	{
		UE_LOG(LogTemp, Warning, TEXT("登录场景Preset与定义不匹配，跳过: scene=%s presetKey=%s expectedKey=%s"),
			*SceneDefinition.DisplayName,
			*PresetAsset->SceneKey,
			*SceneDefinition.Key);
		return nullptr;
	}
	return PresetAsset;
}

bool AWoWLoginGameMode::ApplyLoginScenePresetRuntimeCache(int32 SceneIndex, const UWoWLoginScenePresetAsset* PresetAsset)
{
	if (!LoginSceneDefinitions.IsValidIndex(SceneIndex) || !PresetAsset)
	{
		return false;
	}

	const FWoWLoginSceneRuntimeCacheSnapshot& Snapshot = PresetAsset->RuntimeCache;
	if (!Snapshot.bCameraLoaded && !Snapshot.bRuntimeLoaded)
	{
		return false;
	}

	FLoginSceneRuntimeCache& Cache = LoginSceneRuntimeCaches.FindOrAdd(SceneIndex);
	Cache = FLoginSceneRuntimeCache();
	Cache.bCameraLoaded = Snapshot.bCameraLoaded;
	Cache.CameraId = Snapshot.CameraId;
	Cache.CameraPosition = Snapshot.CameraPosition;
	Cache.CameraTarget = Snapshot.CameraTarget;
	Cache.CameraFovDegrees = Snapshot.CameraFovDegrees;
	Cache.CameraNearClip = Snapshot.CameraNearClip;
	Cache.CameraFarClip = Snapshot.CameraFarClip;
	Cache.bRuntimeLoaded = Snapshot.bRuntimeLoaded;
	Cache.LateAdditiveOverlaySlots = Snapshot.LateAdditiveOverlaySlots;
	Cache.BonePivots = Snapshot.BonePivots;
	Cache.CameraTrackDurationMs = Snapshot.CameraTrackDurationMs;

	Cache.TextureTransformTracks.Reserve(Snapshot.TextureTransformTracks.Num());
	for (const FWoWLoginSceneTextureSectionTrack& Track : Snapshot.TextureTransformTracks)
	{
		Cache.TextureTransformTracks.Add(MakeRuntimeTextureTrack(Track));
	}

	Cache.RenderSections.Reserve(Snapshot.RenderSections.Num());
	for (const FWoWLoginSceneTextureSectionTrack& Track : Snapshot.RenderSections)
	{
		Cache.RenderSections.Add(MakeRuntimeTextureTrack(Track));
	}

	Cache.ColorTracks.Reserve(Snapshot.ColorTracks.Num());
	for (const FWoWLoginSceneSectionColorTrack& Track : Snapshot.ColorTracks)
	{
		Cache.ColorTracks.Add(MakeRuntimeColorTrack(Track));
	}

	Cache.CameraPositionSamples.Reserve(Snapshot.CameraPositionSamples.Num());
	for (const FWoWLoginSceneCameraVectorSample& Sample : Snapshot.CameraPositionSamples)
	{
		FCameraVectorSample RuntimeSample;
		RuntimeSample.TimeMs = Sample.TimeMs;
		RuntimeSample.Value = Sample.Value;
		Cache.CameraPositionSamples.Add(RuntimeSample);
	}

	Cache.CameraTargetSamples.Reserve(Snapshot.CameraTargetSamples.Num());
	for (const FWoWLoginSceneCameraVectorSample& Sample : Snapshot.CameraTargetSamples)
	{
		FCameraVectorSample RuntimeSample;
		RuntimeSample.TimeMs = Sample.TimeMs;
		RuntimeSample.Value = Sample.Value;
		Cache.CameraTargetSamples.Add(RuntimeSample);
	}

	Cache.CameraRollSamples.Reserve(Snapshot.CameraRollSamples.Num());
	for (const FWoWLoginSceneCameraRollSample& Sample : Snapshot.CameraRollSamples)
	{
		FCameraRollSample RuntimeSample;
		RuntimeSample.TimeMs = Sample.TimeMs;
		RuntimeSample.Degrees = Sample.Degrees;
		Cache.CameraRollSamples.Add(RuntimeSample);
	}

	Cache.LoginBones.Reserve(Snapshot.LoginBones.Num());
	for (const FWoWLoginSceneBoneInfo& Bone : Snapshot.LoginBones)
	{
		FLoginBoneInfo RuntimeBone;
		RuntimeBone.Index = Bone.Index;
		RuntimeBone.KeyBoneId = Bone.KeyBoneId;
		RuntimeBone.Flags = Bone.Flags;
		RuntimeBone.Parent = Bone.Parent;
		RuntimeBone.GeosetId = Bone.GeosetId;
		RuntimeBone.Pivot = Bone.Pivot;
		Cache.LoginBones.Add(RuntimeBone);
	}

	for (const FWoWLoginSceneBoneAnimationTrack& Track : Snapshot.BoneAnimationTracks)
	{
		TArray<FLoginBoneFrameSample>& RuntimeSamples = Cache.BoneAnimationSamples.FindOrAdd(Track.BoneIndex);
		RuntimeSamples.Reserve(Track.Samples.Num());
		for (const FWoWLoginSceneBoneFrameSample& Sample : Track.Samples)
		{
			FLoginBoneFrameSample RuntimeSample;
			RuntimeSample.FrameIndex = Sample.FrameIndex;
			RuntimeSample.TimeMs = Sample.TimeMs;
			RuntimeSample.Transform = Sample.Transform;
			RuntimeSamples.Add(RuntimeSample);
		}
	}

	Cache.LoginSceneLights.Reserve(Snapshot.LoginSceneLights.Num());
	for (const FWoWLoginSceneLightSnapshot& Light : Snapshot.LoginSceneLights)
	{
		FLoginSceneLight RuntimeLight;
		RuntimeLight.Index = Light.Index;
		RuntimeLight.LightType = Light.LightType;
		RuntimeLight.BoneIndex = Light.BoneIndex;
		RuntimeLight.Position = Light.Position;
		RuntimeLight.AmbientColor = Light.AmbientColor;
		RuntimeLight.AmbientIntensity = Light.AmbientIntensity;
		RuntimeLight.DiffuseColor = Light.DiffuseColor;
		RuntimeLight.DiffuseIntensity = Light.DiffuseIntensity;
		RuntimeLight.AttenuationStart = Light.AttenuationStart;
		RuntimeLight.AttenuationEnd = Light.AttenuationEnd;
		Cache.LoginSceneLights.Add(RuntimeLight);
	}

	Cache.ParticleEmitters.Reserve(Snapshot.ParticleEmitters.Num());
	for (const FWoWLoginSceneParticleEmitterSnapshot& Emitter : Snapshot.ParticleEmitters)
	{
		Cache.ParticleEmitters.Add(MakeRuntimeParticleEmitter(Emitter));
	}

	if (SceneIndex == ActiveLoginSceneIndex && Cache.bRuntimeLoaded)
	{
		ApplyLoginSceneRuntimeCache(SceneIndex);
	}
	return true;
}

void AWoWLoginGameMode::ApplyLoginSceneTuning()
{
	ApplyLoginSceneTuningToSceneActor();
	const float TimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	ApplyLoginSceneTuningToCamera(TimeSeconds);
}

void AWoWLoginGameMode::ApplyLoginSceneTuningToSceneActor()
{
	if (!LoginSceneActor.IsValid())
	{
		return;
	}

	ASkeletalMeshActor* SceneActor = LoginSceneActor.Get();
	SceneActor->SetActorLocation(LoginSceneTuningState.SceneLocationOffset);
	SceneActor->SetActorRotation(LoginSceneTuningState.SceneRotationOffset);
	SceneActor->SetActorScale3D(LoginSceneTuningState.SceneScaleMultiplier);
}

void AWoWLoginGameMode::ApplyLoginSceneTuningToCamera(float TimeSeconds)
{
	if (!LoginCameraActor.IsValid())
	{
		return;
	}

	auto SampleVectorTrack = [TimeSeconds](const TArray<FCameraVectorSample>& Samples, float DurationMs)
	{
		const float TimeMs = ResolveLoopTimeMs(TimeSeconds, DurationMs);
		const int32 SegmentIndex = FindSampleSegmentByTime(TimeMs, Samples.Num(), [&Samples](int32 Index)
		{
			return Samples[Index].TimeMs;
		});
		const FCameraVectorSample& A = Samples[SegmentIndex];
		const FCameraVectorSample& B = Samples[FMath::Min(SegmentIndex + 1, Samples.Num() - 1)];
		const float Ratio = B.TimeMs > A.TimeMs ? (TimeMs - A.TimeMs) / (B.TimeMs - A.TimeMs) : 0.0f;
		return FMath::Lerp(A.Value, B.Value, Ratio);
	};

	FVector Position = BaseCameraPosition;
	FVector Target = BaseCameraTarget;
	float RollDegrees = 0.0f;

	if (CameraPositionSamples.Num() >= 2 && CameraTargetSamples.Num() >= 2 && CameraTrackDurationMs > 0.0f)
	{
		Position = SampleVectorTrack(CameraPositionSamples, CameraTrackDurationMs);
		Target = SampleVectorTrack(CameraTargetSamples, CameraTrackDurationMs);

		if (CameraRollSamples.Num() == 1)
		{
			RollDegrees = CameraRollSamples[0].Degrees;
		}
		else if (CameraRollSamples.Num() >= 2)
		{
			const float TimeMs = ResolveLoopTimeMs(TimeSeconds, CameraTrackDurationMs);
			const int32 SegmentIndex = FindSampleSegmentByTime(TimeMs, CameraRollSamples.Num(), [this](int32 Index)
			{
				return CameraRollSamples[Index].TimeMs;
			});
			const FCameraRollSample& A = CameraRollSamples[SegmentIndex];
			const FCameraRollSample& B = CameraRollSamples[FMath::Min(SegmentIndex + 1, CameraRollSamples.Num() - 1)];
			const float Ratio = B.TimeMs > A.TimeMs ? (TimeMs - A.TimeMs) / (B.TimeMs - A.TimeMs) : 0.0f;
			RollDegrees = FMath::Lerp(A.Degrees, B.Degrees, Ratio);
		}
	}

	Position += LoginSceneTuningState.CameraPositionOffset;
	Target += LoginSceneTuningState.CameraTargetOffset;

	FRotator Rotation = (Target - Position).Rotation();
	Rotation.Roll = RollDegrees + LoginSceneTuningState.CameraRollOffsetDegrees;
	LoginCameraActor->SetActorLocationAndRotation(Position, Rotation);

	if (UCameraComponent* CameraComponent = LoginCameraActor->GetCameraComponent())
	{
		ApplyGlueCameraProjection(
			CameraComponent,
			BaseCameraProjectionFovDegrees + LoginSceneTuningState.CameraVerticalFovOffsetDegrees * M2CameraFovToProjectionDegrees,
			BaseCameraNearClip + LoginSceneTuningState.CameraNearClipOffsetWowUnits * WowScalarToUE,
			BaseCameraFarClip + LoginSceneTuningState.CameraFarClipOffsetWowUnits * WowScalarToUE);
	}

	PositionCharacterSelectPreviewActor();
}

void AWoWLoginGameMode::MarkLoginSceneTuningAssetDirty()
{
	if (!LoginSceneTuningAsset)
	{
		return;
	}

	LoginSceneTuningAsset->MarkPackageDirty();
	if (UPackage* Package = LoginSceneTuningAsset->GetPackage())
	{
		Package->MarkPackageDirty();
	}
}

void AWoWLoginGameMode::StartPlay()
{
	Super::StartPlay();

	if (UGameViewportClient* ViewportClient = GetWorld() ? GetWorld()->GetGameViewport() : nullptr)
	{
		AWoWPlayerController::ConfigureWoWHardwareCursors(ViewportClient);
	}

	BuildLoginSceneDefinitions();
	LoadPlayerCharacterPreviewDefinitions();
	WoWRuntimeCharacterAppearance::WarmupRuntimeDbc();
	GetCharacterAppearanceDbcCache();
	PendingLoginSceneRuntimeCachePrewarmIndices.Reset();
	LoadOrCreateLoginSceneTuningAsset();
	ClearTemplateLevelActors();
	BuildLoginOverlay();
	RequestActiveLoginSceneIndex(ActiveLoginSceneIndex);
	StartCommandLineGlueAutoLoginIfRequested();
}

void AWoWLoginGameMode::ClearTemplateLevelActors()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	TArray<AActor*> ActorsToDestroy;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || Actor->IsA<APlayerController>() || Actor->IsA<ALevelScriptActor>())
		{
			continue;
		}

		if (Actor->IsA<ADefaultPawn>() ||
			Actor->IsA<APlayerStart>() ||
			Actor->IsA<AStaticMeshActor>() ||
			Actor->IsA<APostProcessVolume>() ||
			Actor->IsA<ASkyLight>() ||
			Actor->FindComponentByClass<ULightComponent>() ||
			Actor->IsA<ABrush>())
		{
			ActorsToDestroy.Add(Actor);
		}
	}

	for (AActor* Actor : ActorsToDestroy)
	{
		Actor->Destroy();
	}

	UE_LOG(LogTemp, Display, TEXT("WLK登录模式已清理模板关卡Actor: %d"), ActorsToDestroy.Num());
}

void AWoWLoginGameMode::SpawnLoginScene()
{
	const double SpawnStartSeconds = FPlatformTime::Seconds();
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const FLoginSceneDefinition* SceneDefinition = GetActiveLoginSceneDefinition(this);
	if (!SceneDefinition)
	{
		UE_LOG(LogTemp, Error, TEXT("没有可用登录界面资产。"));
		return;
	}

	const double MeshLoadStartSeconds = FPlatformTime::Seconds();
	USkeletalMesh* LoginMesh = ResolveLoadedObject<USkeletalMesh>(SceneDefinition->MeshPath);
	if (!LoginMesh && !ActiveLoginSceneStreamableHandle.IsValid())
	{
		LoginMesh = LoadObject<USkeletalMesh>(nullptr, *SceneDefinition->MeshPath);
	}
	const double MeshLoadElapsedMs = (FPlatformTime::Seconds() - MeshLoadStartSeconds) * 1000.0;
	if (!LoginMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("登录界面M2生成网格未预加载，拒绝在 apply 阶段同步加载: %s"), *SceneDefinition->MeshPath);
		return;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	ASkeletalMeshActor* SceneActor = World->SpawnActor<ASkeletalMeshActor>(
		ASkeletalMeshActor::StaticClass(),
		FVector::ZeroVector,
		FRotator::ZeroRotator,
		SpawnParameters);
	if (!SceneActor)
	{
		return;
	}

	LoginSceneActor = SceneActor;
	USkeletalMeshComponent* MeshComponent = SceneActor->GetSkeletalMeshComponent();
	MeshComponent->SetSkeletalMesh(LoginMesh);
	MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	MeshComponent->SetMobility(EComponentMobility::Movable);
	// M2顶点和M2 camera 已经在导出阶段一起转换为UE单位；这里不能再缩放，否则相机距离会错100倍。
	MeshComponent->SetRelativeScale3D(FVector::OneVector);
	MeshComponent->SetVisibility(false, true);
	MeshComponent->SetHiddenInGame(true, true);
	const double MaterialStartSeconds = FPlatformTime::Seconds();
	ApplyLoginSceneMaterials(MeshComponent, *SceneDefinition);
	const double MaterialElapsedMs = (FPlatformTime::Seconds() - MaterialStartSeconds) * 1000.0;
	LoginSceneDynamicMaterials.Reset();
	const int32 MaterialSlotCount = MeshComponent->GetNumMaterials();
	const double DynamicMaterialStartSeconds = FPlatformTime::Seconds();
	LoginSceneDynamicMaterials.SetNum(MaterialSlotCount);
	for (int32 SlotIndex = 0; SlotIndex < MaterialSlotCount; ++SlotIndex)
	{
		UMaterialInstanceDynamic* DynamicMaterial = MeshComponent->CreateDynamicMaterialInstance(SlotIndex);
		LoginSceneDynamicMaterials[SlotIndex] = DynamicMaterial;
		if (DynamicMaterial)
		{
			DynamicMaterial->SetVectorParameterValue(TEXT("UVScale"), FLinearColor(1.0f, 1.0f, 0.0f, 0.0f));
			DynamicMaterial->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor::Black);
			DynamicMaterial->SetScalarParameterValue(TEXT("UVRotationDegrees"), 0.0f);
		}
	}
	const double DynamicMaterialElapsedMs = (FPlatformTime::Seconds() - DynamicMaterialStartSeconds) * 1000.0;
	LoadLoginM2RuntimeTracks();
	CreateLoginSceneLateAdditiveOverlay(MeshComponent);
	int32 InitializedTextureSections = 0;
	const double TextureOverrideStartSeconds = FPlatformTime::Seconds();
	for (const FSectionTextureTransformTrack& Track : RenderSections)
	{
		ApplyTextureSectionRenderOverride(Track.SlotIndex);
		ApplyTextureSectionVisibility(Track.SlotIndex);
		++InitializedTextureSections;
	}
	const double TextureOverrideElapsedMs = (FPlatformTime::Seconds() - TextureOverrideStartSeconds) * 1000.0;
	UE_LOG(LogTemp, Display, TEXT("WLK登录M2渲染section材质已初始化: %d"), InitializedTextureSections);
	SpawnLoginSceneLights();
	const double ParticleStartSeconds = FPlatformTime::Seconds();
	SpawnLoginParticleSystems();
	const double ParticleElapsedMs = (FPlatformTime::Seconds() - ParticleStartSeconds) * 1000.0;

	const double AnimationStartSeconds = FPlatformTime::Seconds();
	if (UAnimSequence* LoginAnimation = ResolveLoadedObject<UAnimSequence>(SceneDefinition->AnimationPath))
	{
		MeshComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
		MeshComponent->SetAnimation(LoginAnimation);
		MeshComponent->Play(true);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("找不到登录界面循环动画: %s"), *SceneDefinition->AnimationPath);
	}
	const double AnimationElapsedMs = (FPlatformTime::Seconds() - AnimationStartSeconds) * 1000.0;

	ApplyLoginSceneTuningToSceneActor();
	UE_LOG(LogTemp, Display, TEXT("Glue SpawnLoginScene profile: scene=%s total=%.2fms mesh=%.2fms materials=%.2fms dynamic=%.2fms textureOverrides=%.2fms particles=%.2fms animation=%.2fms slots=%d renderTracks=%d"),
		*SceneDefinition->DisplayName,
		(FPlatformTime::Seconds() - SpawnStartSeconds) * 1000.0,
		MeshLoadElapsedMs,
		MaterialElapsedMs,
		DynamicMaterialElapsedMs,
		TextureOverrideElapsedMs,
		ParticleElapsedMs,
		AnimationElapsedMs,
		MaterialSlotCount,
		RenderSections.Num());
}

void AWoWLoginGameMode::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	UWorld* World = GetWorld();
	const float TimeSeconds = World ? World->GetTimeSeconds() : 0.0f;
	bool bRunVisualUpdate = true;
	const int32 VisualUpdateHz = CVarWoWLoginSceneVisualUpdateHz.GetValueOnGameThread();
	if (VisualUpdateHz > 0)
	{
		LoginSceneVisualUpdateAccumulatorSeconds += DeltaSeconds;
		const float VisualUpdateIntervalSeconds = 1.0f / FMath::Clamp(VisualUpdateHz, 1, 240);
		bRunVisualUpdate = LoginSceneVisualUpdateAccumulatorSeconds >= VisualUpdateIntervalSeconds;
		if (bRunVisualUpdate)
		{
			LoginSceneVisualUpdateAccumulatorSeconds = 0.0f;
		}
	}
	if (bRunVisualUpdate)
	{
		ApplyLoginSceneTextureAnimation(TimeSeconds);
		ApplyLoginSceneColorAnimation(TimeSeconds);
		ApplyLoginSceneCameraAnimation(TimeSeconds);
		UpdateLoginSceneLights();
	}

	if (CVarWoWLoginParticles.GetValueOnGameThread() != 0)
	{
		float ParticleUpdateDeltaSeconds = DeltaSeconds;
		bool bRunParticleUpdate = true;
		const int32 ParticleUpdateHz = CVarWoWLoginParticleUpdateHz.GetValueOnGameThread();
		if (ParticleUpdateHz > 0)
		{
			LoginParticleUpdateAccumulatorSeconds += DeltaSeconds;
			const float ParticleUpdateIntervalSeconds = 1.0f / FMath::Clamp(ParticleUpdateHz, 1, 240);
			bRunParticleUpdate = LoginParticleUpdateAccumulatorSeconds >= ParticleUpdateIntervalSeconds;
			if (bRunParticleUpdate)
			{
				ParticleUpdateDeltaSeconds = LoginParticleUpdateAccumulatorSeconds;
				LoginParticleUpdateAccumulatorSeconds = 0.0f;
			}
		}
		if (bRunParticleUpdate)
		{
			UpdateLoginParticleSystems(ParticleUpdateDeltaSeconds, TimeSeconds);
		}
	}
	else
	{
		auto ClearParticleSystems = [](TArray<FLoginParticleSystem>& Systems)
		{
			for (FLoginParticleSystem& System : Systems)
			{
				System.Particles.Reset();
				System.SpawnRemainder = 0.0f;
				if (System.MeshComponent)
				{
					System.MeshComponent->SetVisibility(false, true);
					if (System.bMeshSectionInitialized)
					{
						System.MeshComponent->ClearAllMeshSections();
						System.bMeshSectionInitialized = false;
					}
				}
			}
		};
		ClearParticleSystems(ParticleSystems);
		ClearParticleSystems(CharacterSelectPreviewAttachmentParticleSystems);
	}
	if (LoginGlueLuaRuntime.IsValid())
	{
		LoginGlueLuaRuntime->Tick(DeltaSeconds);
	}
	ProcessGlueAsyncResults();
	ProcessPendingGlueTransition(DeltaSeconds);
	ProcessLoginSceneRuntimeCachePrewarmQueue();
	ProcessCharacterPreviewPrewarmQueue();
	ProcessDeferredCharacterPreviewRuntimeMaterials(DeltaSeconds);
	ProcessDeferredCharacterPreviewM2Attachments(DeltaSeconds);
	ProcessCharacterPreviewAnimationCatalogBuild(DeltaSeconds);
	ProcessCharacterPreviewBuildCoordinator();
	ProcessCommandLineGlueAutoCycle(DeltaSeconds);
	ProcessCommandLineGlueAutoExit(DeltaSeconds);
}

void AWoWLoginGameMode::SetupLoginCamera()
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	FActorSpawnParameters SpawnParameters;
	SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	// ============================================
	// === 登录界面相机默认参数 (仅 M2 cameras[] 为空时使用) ===
	// CameraPosition    : 相机世界位置 (X=前后, Y=左右, Z=上下)
	// CameraTarget      : 相机看向的目标点
	// CameraFovDegrees  : 视野角度 (默认~72°, 调大=广角/拉远感, 调小=望远/贴近感)
	// 如果 M2 JSON 中有 cameras[] 数据, 将自动覆盖这些默认值
	// ============================================
	FVector CameraPosition(1111.7126f, 3.5033333f, 244.14883f);
	FVector CameraTarget(-1028.4618f, 3.5033333f, 244.14883f);
	float CameraFovDegrees = 2.094395f;
	float CameraProjectionFovDegrees = CameraFovDegrees * M2CameraFovToProjectionDegrees;
	float NearClip = 22.222223f;
	float FarClip = 277777.78f;
	BaseCameraId = INDEX_NONE;
	const bool bLoadedM2Camera = LoadLoginM2Camera(CameraPosition, CameraTarget, CameraFovDegrees, NearClip, FarClip);
	CameraProjectionFovDegrees = CameraFovDegrees * M2CameraFovToProjectionDegrees;
	BaseCameraPosition = CameraPosition;
	BaseCameraTarget = CameraTarget;
	BaseCameraFovDegrees = CameraFovDegrees;
	BaseCameraProjectionFovDegrees = CameraProjectionFovDegrees;
	BaseCameraNearClip = NearClip;
	BaseCameraFarClip = FarClip;

	// 中文说明：登录界面优先使用 M2 cameras[] 的位置、目标点和FOV。没有数据时才退回上面的本地默认值。
	ACameraActor* Camera = World->SpawnActor<ACameraActor>(
		ACameraActor::StaticClass(),
		CameraPosition,
		(CameraTarget - CameraPosition).Rotation(),
		SpawnParameters);
	if (!Camera)
	{
		return;
	}

	LoginCameraActor = Camera;
	UCameraComponent* CameraComponent = Camera->GetCameraComponent();
	const float ViewportAspectRatio = GetCurrentViewportAspectRatio();
	ApplyGlueCameraProjection(
		CameraComponent,
		BaseCameraProjectionFovDegrees + LoginSceneTuningState.CameraVerticalFovOffsetDegrees * M2CameraFovToProjectionDegrees,
		BaseCameraNearClip + LoginSceneTuningState.CameraNearClipOffsetWowUnits * WowScalarToUE,
		BaseCameraFarClip + LoginSceneTuningState.CameraFarClipOffsetWowUnits * WowScalarToUE);
	// 中文说明：Glue 登录 M2 更接近旧客户端固定管线画面，不应被 UE 默认自动曝光、景深和运动模糊二次改色。
	CameraComponent->PostProcessSettings.bOverride_MotionBlurAmount = true;
	CameraComponent->PostProcessSettings.MotionBlurAmount = 0.0f;
	CameraComponent->PostProcessSettings.bOverride_DepthOfFieldFstop = true;
	CameraComponent->PostProcessSettings.DepthOfFieldFstop = 64.0f;
	CameraComponent->PostProcessSettings.bOverride_AutoExposureBias = true;
	CameraComponent->PostProcessSettings.AutoExposureBias = 0.0f;
	CameraComponent->PostProcessSettings.bOverride_BloomIntensity = true;
	CameraComponent->PostProcessSettings.BloomIntensity = 0.0f;
	UE_LOG(LogTemp, Display, TEXT("WLK登录M2相机: loaded=%d pos=%s target=%s m2ProjectionFov=%.2f viewportAspect=%.4f axis=GlueHorizontalFOV near=%.2f far=%.2f"),
		bLoadedM2Camera ? 1 : 0,
		*BaseCameraPosition.ToString(),
		*BaseCameraTarget.ToString(),
		BaseCameraProjectionFovDegrees,
		ViewportAspectRatio,
		BaseCameraNearClip,
		BaseCameraFarClip);

	if (APlayerController* PlayerController = World->GetFirstPlayerController())
	{
		PlayerController->SetViewTarget(Camera);
		PlayerController->bShowMouseCursor = true;
		FInputModeUIOnly InputMode;
		InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::DoNotLock);
		PlayerController->SetInputMode(InputMode);
	}

	ApplyLoginSceneTuningToCamera(GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f);
}

void AWoWLoginGameMode::PresentLoginSceneVisuals()
{
	const float TimeSeconds = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0f;
	ApplyLoginSceneTextureAnimation(TimeSeconds);
	ApplyLoginSceneColorAnimation(TimeSeconds);
	UpdateLoginSceneLights();
	UpdateLoginParticleSystems(0.0f, TimeSeconds);

	if (LoginSceneActor.IsValid())
	{
		if (USkeletalMeshComponent* MeshComponent = LoginSceneActor->GetSkeletalMeshComponent())
		{
			MeshComponent->SetVisibility(true, true);
			MeshComponent->SetHiddenInGame(false, true);
		}
	}

	if (LoginSceneLateAdditiveOverlayComponent.IsValid())
	{
		LoginSceneLateAdditiveOverlayComponent->SetVisibility(true, true);
		LoginSceneLateAdditiveOverlayComponent->SetHiddenInGame(false, true);
	}

	for (FLoginParticleSystem& System : ParticleSystems)
	{
		if (System.MeshComponent)
		{
			System.MeshComponent->SetVisibility(true, true);
			System.MeshComponent->SetHiddenInGame(false, true);
		}
	}
}

bool AWoWLoginGameMode::LoadLoginM2Camera(FVector& OutPosition, FVector& OutTarget, float& OutFovDegrees, float& OutNearClip, float& OutFarClip)
{
	if (!bBypassLoginScenePresetCacheForBake && !LoginSceneRuntimeCaches.Contains(ActiveLoginSceneIndex))
	{
		if (const UWoWLoginScenePresetAsset* PresetAsset = ResolveLoginScenePresetAsset(ActiveLoginSceneIndex))
		{
			ApplyLoginScenePresetRuntimeCache(ActiveLoginSceneIndex, PresetAsset);
		}
	}

	if (const FLoginSceneRuntimeCache* Cache = LoginSceneRuntimeCaches.Find(ActiveLoginSceneIndex))
	{
		if (Cache->bCameraLoaded)
		{
			BaseCameraId = Cache->CameraId;
			OutPosition = Cache->CameraPosition;
			OutTarget = Cache->CameraTarget;
			OutFovDegrees = Cache->CameraFovDegrees;
			OutNearClip = Cache->CameraNearClip;
			OutFarClip = Cache->CameraFarClip;
			return true;
		}
	}

	const FLoginSceneDefinition* SceneDefinition = GetActiveLoginSceneDefinition(this);
	const FString JsonPath = SceneDefinition ? MakeSavedJsonPathFromRelativePath(SceneDefinition->JsonRelativePath) : FString();
	FString JsonText;
	if (JsonPath.IsEmpty() || !FFileHelper::LoadFileToString(JsonText, *JsonPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("找不到登录M2 JSON，使用默认相机: jsonRel=%s jsonPath=%s"),
			SceneDefinition ? *SceneDefinition->JsonRelativePath : TEXT("<none>"),
			*JsonPath);
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("无法解析登录M2 JSON，使用默认相机: %s"), *JsonPath);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Cameras = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("cameras"), Cameras) || !Cameras || Cameras->IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("登录M2 JSON没有 cameras[]，使用默认相机。"));
		return false;
	}

	const TSharedPtr<FJsonObject> CameraObject = TryReadJsonObject((*Cameras)[0]);
	if (!CameraObject.IsValid())
	{
		return false;
	}

	double CameraIdValue = -1.0;
	if (CameraObject->TryGetNumberField(TEXT("id"), CameraIdValue))
	{
		BaseCameraId = FMath::RoundToInt(CameraIdValue);
	}

	auto ReadVector = [](const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FVector& OutValue) -> bool
	{
		const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
		if (!Object->TryGetArrayField(FieldName, Values) || !Values || Values->Num() < 3)
		{
			return false;
		}
		OutValue = FVector(
			static_cast<float>((*Values)[0]->AsNumber()),
			static_cast<float>((*Values)[1]->AsNumber()),
			static_cast<float>((*Values)[2]->AsNumber()));
		return true;
	};

	if (!ReadVector(CameraObject, TEXT("position"), OutPosition) ||
		!ReadVector(CameraObject, TEXT("target"), OutTarget))
	{
		return false;
	}

	double FovRadiansValue = 0.0;
	if (CameraObject->TryGetNumberField(TEXT("fov_radians"), FovRadiansValue))
	{
		OutFovDegrees = static_cast<float>(FovRadiansValue);
	}
	else
	{
		double ProjectionFovValue = 0.0;
		if (CameraObject->TryGetNumberField(TEXT("projection_fov_degrees"), ProjectionFovValue))
		{
			OutFovDegrees = static_cast<float>(ProjectionFovValue) / M2CameraFovToProjectionDegrees;
		}
		else
		{
			OutFovDegrees = static_cast<float>(CameraObject->GetNumberField(TEXT("fov_degrees"))) / M2CameraFovToProjectionDegrees;
		}
	}
	OutNearClip = static_cast<float>(CameraObject->GetNumberField(TEXT("near_clip")));
	OutFarClip = static_cast<float>(CameraObject->GetNumberField(TEXT("far_clip")));
	return true;
}

bool AWoWLoginGameMode::LoadLoginM2RuntimeTracks()
{
	if (!bBypassLoginScenePresetCacheForBake && !LoginSceneRuntimeCaches.Contains(ActiveLoginSceneIndex))
	{
		if (const UWoWLoginScenePresetAsset* PresetAsset = ResolveLoginScenePresetAsset(ActiveLoginSceneIndex))
		{
			ApplyLoginScenePresetRuntimeCache(ActiveLoginSceneIndex, PresetAsset);
		}
	}

	if (ApplyLoginSceneRuntimeCache(ActiveLoginSceneIndex))
	{
		return true;
	}

	TextureTransformTracks.Reset();
	RenderSections.Reset();
	ColorTracks.Reset();
	LoginSceneLateAdditiveOverlaySlots.Reset();
	CameraPositionSamples.Reset();
	CameraTargetSamples.Reset();
	CameraRollSamples.Reset();
	BonePivots.Reset();
	LoginBones.Reset();
	BoneAnimationSamples.Reset();
	LoginSceneLights.Reset();
	ParticleEmitters.Reset();
	CameraTrackDurationMs = 0.0f;

	const FLoginSceneDefinition* SceneDefinition = GetActiveLoginSceneDefinition(this);
	const FString JsonPath = SceneDefinition ? MakeSavedJsonPathFromRelativePath(SceneDefinition->JsonRelativePath) : FString();
	FString JsonText;
	if (JsonPath.IsEmpty() || !FFileHelper::LoadFileToString(JsonText, *JsonPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("找不到登录M2 JSON，无法读取运行时轨道: jsonRel=%s jsonPath=%s"),
			SceneDefinition ? *SceneDefinition->JsonRelativePath : TEXT("<none>"),
			*JsonPath);
		return false;
	}
	UE_LOG(LogTemp, Display, TEXT("登录M2 JSON已加载: scene=%s jsonRel=%s jsonPath=%s"),
		SceneDefinition ? *SceneDefinition->DisplayName : TEXT("<none>"),
		SceneDefinition ? *SceneDefinition->JsonRelativePath : TEXT("<none>"),
		*JsonPath);

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("无法解析登录M2 JSON，无法读取运行时轨道: %s"), *JsonPath);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
	if (RootObject->TryGetArrayField(TEXT("sections"), Sections) && Sections)
	{
		for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
		{
			const TSharedPtr<FJsonObject> SectionObject = TryReadJsonObject(SectionValue);
			if (!SectionObject.IsValid())
			{
				continue;
			}

			const int32 SlotIndex = static_cast<int32>(SectionObject->GetIntegerField(TEXT("section_index")));
			double Number = 0.0;
			FSectionTextureTransformTrack Section;
			Section.SlotIndex = SlotIndex;
			Section.SubmeshIndex = SectionObject->TryGetNumberField(TEXT("submesh_index"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.SubmeshId = SectionObject->TryGetNumberField(TEXT("submesh_id"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.BatchFlags = SectionObject->TryGetNumberField(TEXT("batch_flags"), Number) ? FMath::RoundToInt(Number) : 0;
			const int32 RenderMode = SectionObject->TryGetNumberField(TEXT("render_mode"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			const int32 MaterialIndex = SectionObject->TryGetNumberField(TEXT("material_index"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			const int32 MaterialFlags = SectionObject->TryGetNumberField(TEXT("material_flags"), Number) ? FMath::RoundToInt(Number) : 0;
			const int32 TextureCount = SectionObject->TryGetNumberField(TEXT("texture_count"), Number) ? FMath::RoundToInt(Number) : 0;
			const int32 PriorityPlane = SectionObject->TryGetNumberField(TEXT("priority_plane"), Number) ? FMath::RoundToInt(Number) : 0;
			const int32 TextureTransformIndex = SectionObject->TryGetNumberField(TEXT("texture_transform_index"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.MaterialIndex = MaterialIndex;
			Section.MaterialFlags = MaterialFlags;
			Section.PriorityPlane = PriorityPlane;
			Section.ShaderId = SectionObject->TryGetNumberField(TEXT("shader_id"), Number) ? FMath::RoundToInt(Number) : 0;
			Section.RenderMode = RenderMode;
			Section.TextureCount = TextureCount;
			Section.TextureComboIndex = SectionObject->TryGetNumberField(TEXT("texture_combo_index"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.TextureCoordComboIndex = SectionObject->TryGetNumberField(TEXT("texture_coord_combo_index"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.TextureWeightComboIndex = SectionObject->TryGetNumberField(TEXT("texture_weight_combo_index"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.TextureTransformComboIndex = SectionObject->TryGetNumberField(TEXT("texture_transform_combo_index"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.TextureCombinerIndex = SectionObject->TryGetNumberField(TEXT("texture_combiner_index"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.TextureCombinerRaw0 = SectionObject->TryGetNumberField(TEXT("texture_combiner_raw0"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.TextureCombinerRaw1 = SectionObject->TryGetNumberField(TEXT("texture_combiner_raw1"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.SkinTexUnitFlags = SectionObject->TryGetNumberField(TEXT("skin_texunit_flags"), Number) ? FMath::RoundToInt(Number) : 0;
			Section.SkinTexUnitFlags2 = SectionObject->TryGetNumberField(TEXT("skin_texunit_flags2"), Number) ? FMath::RoundToInt(Number) : 0;
			Section.SkinTexUnitOrder = SectionObject->TryGetNumberField(TEXT("skin_texunit_order"), Number) ? FMath::RoundToInt(Number) : 0;
			Section.SkinSubmesh = SectionObject->TryGetNumberField(TEXT("skin_submesh"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.SkinSubmesh2 = SectionObject->TryGetNumberField(TEXT("skin_submesh2"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.SkinRenderFlagsIndex = SectionObject->TryGetNumberField(TEXT("skin_render_flags_index"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.SkinTexUnit = SectionObject->TryGetNumberField(TEXT("skin_texunit"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.SkinTexUnitMode = SectionObject->TryGetNumberField(TEXT("skin_texunit_mode"), Number) ? FMath::RoundToInt(Number) : 0;
			Section.SkinTextureLookupIndex = SectionObject->TryGetNumberField(TEXT("skin_texture_lookup_index"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.SkinTexUnit2 = SectionObject->TryGetNumberField(TEXT("skin_texunit2"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.SkinTransparencyLookupIndex = SectionObject->TryGetNumberField(TEXT("skin_transparency_lookup_index"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.SkinTextureAnimLookupIndex = SectionObject->TryGetNumberField(TEXT("skin_texture_anim_lookup_index"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.TextureUnit = SectionObject->TryGetNumberField(TEXT("texture_unit"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.ColorIndex = SectionObject->TryGetNumberField(TEXT("color_index"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.TransparencyIndex = SectionObject->TryGetNumberField(TEXT("transparency_index"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.TextureTransformIndex = TextureTransformIndex;
			bool bNoDepthWrite = false;
			SectionObject->TryGetBoolField(TEXT("no_depth_write"), bNoDepthWrite);
			bool bTwoSided = false;
			SectionObject->TryGetBoolField(TEXT("two_sided"), bTwoSided);
			bool bUseEnvMap = false;
			SectionObject->TryGetBoolField(TEXT("use_env_map"), bUseEnvMap);
			bool bUnlit = false;
			SectionObject->TryGetBoolField(TEXT("unlit"), bUnlit);
			bool bBillboard = false;
			SectionObject->TryGetBoolField(TEXT("billboard"), bBillboard);
			Section.bNoDepthWrite = bNoDepthWrite;
			Section.bTwoSided = bTwoSided;
			Section.bUseEnvMap = bUseEnvMap;
			Section.bUnlit = bUnlit;
			Section.bBillboard = bBillboard;
			Section.TextureIndex = SectionObject->TryGetNumberField(TEXT("texture_index"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.TextureType = SectionObject->TryGetNumberField(TEXT("texture_type"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			Section.TextureFlags = SectionObject->TryGetNumberField(TEXT("texture_flags"), Number) ? FMath::RoundToInt(Number) : 0;
			FString PreviewPng;
			SectionObject->TryGetStringField(TEXT("preview_png"), PreviewPng);
			Section.PreviewPng = PreviewPng;
			SectionObject->TryGetStringField(TEXT("source_blp"), Section.SourceBlp);
			RenderSections.Add(Section);
			const FString PreviewName = FPaths::GetBaseFilename(PreviewPng).ToUpper();
			const bool bSparseSnowTexture =
				PreviewName.Contains(TEXT("SNOW_SHIELD")) ||
				PreviewName.Contains(TEXT("SNOWFLAKE"));
			if (RenderMode == 4 && bNoDepthWrite && bTwoSided && TextureCount == 1 && bSparseSnowTexture)
			{
				LoginSceneLateAdditiveOverlaySlots.AddUnique(SlotIndex);
			}

			const TArray<TSharedPtr<FJsonValue>>* Samples = nullptr;
			if (SectionObject->TryGetArrayField(TEXT("texture_transform_samples"), Samples) && Samples && Samples->Num() >= 2)
			{
				FSectionTextureTransformTrack Track = Section;
				Track.Samples.Reserve(Samples->Num());
				for (const TSharedPtr<FJsonValue>& SampleValue : *Samples)
				{
					const TSharedPtr<FJsonObject> SampleObject = TryReadJsonObject(SampleValue);
					if (!SampleObject.IsValid())
					{
						continue;
					}

					FTextureTransformSample Sample;
					Sample.TimeMs = static_cast<float>(SampleObject->GetNumberField(TEXT("time_ms")));
					ReadJsonVector2(SampleObject, TEXT("translation"), Sample.Translation);
					ReadJsonVector2(SampleObject, TEXT("scaling"), Sample.Scaling);
					double RotationDegrees = 0.0;
					if (SampleObject->TryGetNumberField(TEXT("rotation_degrees"), RotationDegrees))
					{
						Sample.RotationDegrees = static_cast<float>(RotationDegrees);
					}
					Track.DurationMs = FMath::Max(Track.DurationMs, Sample.TimeMs);
					Track.Samples.Add(Sample);
				}

				if (Track.Samples.Num() >= 2 && Track.DurationMs > 0.0f)
				{
					TextureTransformTracks.Add(MoveTemp(Track));
				}
			}

			const TArray<TSharedPtr<FJsonValue>>* ColorSamples = nullptr;
			if (SectionObject->TryGetArrayField(TEXT("color_samples"), ColorSamples) && ColorSamples && ColorSamples->Num() >= 1)
			{
				FSectionColorTrack Track;
				Track.SlotIndex = SlotIndex;
				Track.Samples.Reserve(ColorSamples->Num());
				for (const TSharedPtr<FJsonValue>& SampleValue : *ColorSamples)
				{
					const TSharedPtr<FJsonObject> SampleObject = TryReadJsonObject(SampleValue);
					if (!SampleObject.IsValid())
					{
						continue;
					}

					FColorSample Sample;
					Sample.TimeMs = static_cast<float>(SampleObject->GetNumberField(TEXT("time_ms")));
					ReadJsonLinearColor(SampleObject, TEXT("color"), Sample.Color);
					Track.DurationMs = FMath::Max(Track.DurationMs, Sample.TimeMs);
					Track.Samples.Add(Sample);
				}
				if (Track.Samples.Num() > 0)
				{
					Track.BaseColor = Track.Samples[0].Color;
					ColorTracks.Add(MoveTemp(Track));
				}
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Bones = nullptr;
	if (RootObject->TryGetArrayField(TEXT("bones"), Bones) && Bones)
	{
		BonePivots.SetNum(Bones->Num());
		for (const TSharedPtr<FJsonValue>& BoneValue : *Bones)
		{
			const TSharedPtr<FJsonObject> BoneObject = TryReadJsonObject(BoneValue);
			if (!BoneObject.IsValid())
			{
				continue;
			}

			const int32 BoneIndex = BoneObject->GetIntegerField(TEXT("index"));
			FLoginBoneInfo BoneInfo;
			double Number = 0.0;
			BoneInfo.Index = BoneIndex;
			BoneInfo.KeyBoneId = BoneObject->TryGetNumberField(TEXT("key_bone_id"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			BoneInfo.Flags = BoneObject->TryGetNumberField(TEXT("flags"), Number) ? FMath::RoundToInt(Number) : 0;
			BoneInfo.Parent = BoneObject->TryGetNumberField(TEXT("parent"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			BoneInfo.GeosetId = BoneObject->TryGetNumberField(TEXT("geoset_id"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			ReadJsonVector3(BoneObject, TEXT("pivot"), BoneInfo.Pivot);
			LoginBones.Add(BoneInfo);
			if (BonePivots.IsValidIndex(BoneIndex))
			{
				BonePivots[BoneIndex] = BoneInfo.Pivot;
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Animations = nullptr;
	if (RootObject->TryGetArrayField(TEXT("animations"), Animations) && Animations && !Animations->IsEmpty())
	{
		const TSharedPtr<FJsonObject> AnimationObject = TryReadJsonObject((*Animations)[0]);
		const TArray<TSharedPtr<FJsonValue>>* Frames = nullptr;
		double SampleRateValue = 0.0;
		const float SampleRate = AnimationObject.IsValid() && AnimationObject->TryGetNumberField(TEXT("sample_rate"), SampleRateValue)
			? static_cast<float>(SampleRateValue)
			: 0.0f;
		const float FrameStepMs = SampleRate > 0.0f ? 1000.0f / SampleRate : 0.0f;
		if (AnimationObject.IsValid() && AnimationObject->TryGetArrayField(TEXT("frames"), Frames) && Frames)
		{
			for (int32 FrameIndex = 0; FrameIndex < Frames->Num(); ++FrameIndex)
			{
				const TSharedPtr<FJsonObject> FrameObject = TryReadJsonObject((*Frames)[FrameIndex]);
				if (!FrameObject.IsValid())
				{
					continue;
				}

				for (const FLoginBoneInfo& BoneInfo : LoginBones)
				{
					FTransform BoneTransform = FTransform::Identity;
					if (ReadJsonMatrixTransform(FrameObject, FString::FromInt(BoneInfo.Index), BoneTransform))
					{
						FLoginBoneFrameSample Sample;
						Sample.FrameIndex = FrameIndex;
						Sample.TimeMs = FrameStepMs > 0.0f ? FrameIndex * FrameStepMs : static_cast<float>(FrameIndex);
						Sample.Transform = BoneTransform;
						BoneAnimationSamples.FindOrAdd(BoneInfo.Index).Add(Sample);
					}
				}
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Lights = nullptr;
	if (RootObject->TryGetArrayField(TEXT("lights"), Lights) && Lights)
	{
		LoginSceneLights.Reserve(Lights->Num());
		for (const TSharedPtr<FJsonValue>& LightValue : *Lights)
		{
			const TSharedPtr<FJsonObject> LightObject = TryReadJsonObject(LightValue);
			if (!LightObject.IsValid())
			{
				continue;
			}

			double Number = 0.0;
			FLoginSceneLight Light;
			Light.Index = LightObject->TryGetNumberField(TEXT("index"), Number) ? FMath::RoundToInt(Number) : LoginSceneLights.Num();
			Light.LightType = LightObject->TryGetNumberField(TEXT("light_type"), Number) ? FMath::RoundToInt(Number) : 0;
			Light.BoneIndex = LightObject->TryGetNumberField(TEXT("bone"), Number) ? FMath::RoundToInt(Number) : INDEX_NONE;
			ReadJsonVector3(LightObject, TEXT("position"), Light.Position);
			ReadJsonLinearColor(LightObject, TEXT("ambient_color"), Light.AmbientColor);
			Light.AmbientColor.A = 1.0f;
			Light.AmbientIntensity = LightObject->TryGetNumberField(TEXT("ambient_intensity"), Number) ? static_cast<float>(Number) : 0.0f;
			ReadJsonLinearColor(LightObject, TEXT("diffuse_color"), Light.DiffuseColor);
			Light.DiffuseColor.A = 1.0f;
			Light.DiffuseIntensity = LightObject->TryGetNumberField(TEXT("diffuse_intensity"), Number) ? static_cast<float>(Number) : 0.0f;
			Light.AttenuationStart = LightObject->TryGetNumberField(TEXT("attenuation_start"), Number) ? static_cast<float>(Number) : 0.0f;
			Light.AttenuationEnd = LightObject->TryGetNumberField(TEXT("attenuation_end"), Number) ? static_cast<float>(Number) : 0.0f;
			LoginSceneLights.Add(Light);
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* ParticleEmitterValues = nullptr;
	if (RootObject->TryGetArrayField(TEXT("particle_emitters"), ParticleEmitterValues) && ParticleEmitterValues)
	{
		for (const TSharedPtr<FJsonValue>& EmitterValue : *ParticleEmitterValues)
		{
			const TSharedPtr<FJsonObject> EmitterObject = TryReadJsonObject(EmitterValue);
			if (!EmitterObject.IsValid())
			{
				continue;
			}

			FLoginParticleEmitter Emitter;
			double Number = 0.0;
			if (EmitterObject->TryGetNumberField(TEXT("index"), Number))
			{
				Emitter.Index = FMath::RoundToInt(Number);
			}
			if (EmitterObject->TryGetNumberField(TEXT("flags"), Number))
			{
				Emitter.Flags = FMath::RoundToInt(Number);
			}
			if (EmitterObject->TryGetNumberField(TEXT("bone"), Number))
			{
				Emitter.BoneIndex = FMath::RoundToInt(Number);
			}
			ReadJsonVector3(EmitterObject, TEXT("position"), Emitter.Position);
			EmitterObject->TryGetStringField(TEXT("preview_png"), Emitter.PreviewPng);
			if (EmitterObject->TryGetNumberField(TEXT("blend"), Number))
			{
				Emitter.Blend = FMath::RoundToInt(Number);
			}
			if (EmitterObject->TryGetNumberField(TEXT("emitter_type"), Number))
			{
				Emitter.EmitterType = FMath::RoundToInt(Number);
			}
			if (EmitterObject->TryGetNumberField(TEXT("particle_type"), Number))
			{
				Emitter.ParticleType = FMath::RoundToInt(Number);
			}
			if (EmitterObject->TryGetNumberField(TEXT("head_or_tail"), Number))
			{
				Emitter.HeadOrTail = FMath::RoundToInt(Number);
			}
			if (EmitterObject->TryGetNumberField(TEXT("texture_tile_rotation"), Number))
			{
				Emitter.TextureTileRotation = FMath::RoundToInt(Number);
			}
			if (EmitterObject->TryGetNumberField(TEXT("texture_cols"), Number))
			{
				Emitter.TextureColumns = FMath::Max(FMath::RoundToInt(Number), 1);
			}
			if (EmitterObject->TryGetNumberField(TEXT("texture_rows"), Number))
			{
				Emitter.TextureRows = FMath::Max(FMath::RoundToInt(Number), 1);
			}
			ReadJsonLinearColorArray3(EmitterObject, TEXT("lifecycle_colors"), Emitter.LifecycleColors);
			ReadJsonFloatArray3(EmitterObject, TEXT("lifecycle_sizes"), Emitter.LifecycleSizes);
			const FString EmitterTextureName = FPaths::GetBaseFilename(Emitter.PreviewPng).ToLower();
			Emitter.bSnow = EmitterTextureName.Contains(TEXT("snow"));
			Emitter.bSmoke = EmitterTextureName.Contains(TEXT("smoke"));
			Emitter.bFire =
				EmitterTextureName.Contains(TEXT("flame")) ||
				EmitterTextureName.Contains(TEXT("fire"));
			if (EmitterObject->TryGetNumberField(TEXT("mid_point"), Number))
			{
				Emitter.MidPoint = static_cast<float>(Number);
			}
			if (EmitterObject->TryGetNumberField(TEXT("slowdown"), Number))
			{
				Emitter.Slowdown = static_cast<float>(Number);
			}
			if (EmitterObject->TryGetNumberField(TEXT("rotation"), Number))
			{
				Emitter.Rotation = static_cast<float>(Number);
			}

			ReadParticleFloatSamples(EmitterObject, TEXT("emission_speed_samples"), Emitter.EmissionSpeedSamples);
			ReadParticleFloatSamples(EmitterObject, TEXT("speed_variation_samples"), Emitter.SpeedVariationSamples);
			ReadParticleFloatSamples(EmitterObject, TEXT("lifespan_samples"), Emitter.LifespanSamples);
			ReadParticleFloatSamples(EmitterObject, TEXT("emission_rate_samples"), Emitter.EmissionRateSamples);
			ReadParticleFloatSamples(EmitterObject, TEXT("emission_area_length_samples"), Emitter.EmissionAreaLengthSamples);
			ReadParticleFloatSamples(EmitterObject, TEXT("emission_area_width_samples"), Emitter.EmissionAreaWidthSamples);
			ReadParticleFloatSamples(EmitterObject, TEXT("gravity_samples"), Emitter.GravitySamples);
			ReadParticleFloatSamples(EmitterObject, TEXT("gravity2_samples"), Emitter.Gravity2Samples);
			ReadParticleFloatSamples(EmitterObject, TEXT("vertical_range_samples"), Emitter.VerticalRangeSamples);
			ReadParticleFloatSamples(EmitterObject, TEXT("horizontal_range_samples"), Emitter.HorizontalRangeSamples);
			ReadParticleFloatSamples(EmitterObject, TEXT("z_source_samples"), Emitter.ZSourceSamples);

			if (!Emitter.PreviewPng.IsEmpty())
			{
				ParticleEmitters.Add(MoveTemp(Emitter));
			}
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Cameras = nullptr;
	if (RootObject->TryGetArrayField(TEXT("cameras"), Cameras) && Cameras && !Cameras->IsEmpty())
	{
		const TSharedPtr<FJsonObject> CameraObject = TryReadJsonObject((*Cameras)[0]);
		auto ReadCameraVectorSamples = [this](const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FCameraVectorSample>& OutSamples)
		{
			const TArray<TSharedPtr<FJsonValue>>* Samples = nullptr;
			if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Samples) || !Samples)
			{
				return;
			}

			for (const TSharedPtr<FJsonValue>& SampleValue : *Samples)
			{
				const TSharedPtr<FJsonObject> SampleObject = TryReadJsonObject(SampleValue);
				if (!SampleObject.IsValid())
				{
					continue;
				}

				FCameraVectorSample Sample;
				Sample.TimeMs = static_cast<float>(SampleObject->GetNumberField(TEXT("time_ms")));
				if (ReadJsonVector3(SampleObject, TEXT("value"), Sample.Value))
				{
					OutSamples.Add(Sample);
					CameraTrackDurationMs = FMath::Max(CameraTrackDurationMs, Sample.TimeMs);
				}
			}
		};

		ReadCameraVectorSamples(CameraObject, TEXT("position_samples"), CameraPositionSamples);
		ReadCameraVectorSamples(CameraObject, TEXT("target_samples"), CameraTargetSamples);

		const TArray<TSharedPtr<FJsonValue>>* RollSamples = nullptr;
		if (CameraObject.IsValid() && CameraObject->TryGetArrayField(TEXT("roll_samples"), RollSamples) && RollSamples)
		{
			for (const TSharedPtr<FJsonValue>& SampleValue : *RollSamples)
			{
				const TSharedPtr<FJsonObject> SampleObject = TryReadJsonObject(SampleValue);
				if (!SampleObject.IsValid())
				{
					continue;
				}

				FCameraRollSample Sample;
				Sample.TimeMs = static_cast<float>(SampleObject->GetNumberField(TEXT("time_ms")));
				Sample.Degrees = static_cast<float>(SampleObject->GetNumberField(TEXT("degrees")));
				CameraRollSamples.Add(Sample);
				CameraTrackDurationMs = FMath::Max(CameraTrackDurationMs, Sample.TimeMs);
			}
		}
	}

	UE_LOG(LogTemp, Display, TEXT("WLK登录M2运行时轨道: textureTracks=%d colorTracks=%d lights=%d particles=%d cameraPos=%d cameraTarget=%d cameraRoll=%d"),
		TextureTransformTracks.Num(),
		ColorTracks.Num(),
		LoginSceneLights.Num(),
		ParticleEmitters.Num(),
		CameraPositionSamples.Num(),
		CameraTargetSamples.Num(),
		CameraRollSamples.Num());
	if (!LoginSceneLateAdditiveOverlaySlots.IsEmpty())
	{
		FString SlotList;
		for (const int32 SlotIndex : LoginSceneLateAdditiveOverlaySlots)
		{
			if (!SlotList.IsEmpty())
			{
				SlotList += TEXT(",");
			}
			SlotList += FString::FromInt(SlotIndex);
		}
		UE_LOG(LogTemp, Display, TEXT("WLK登录M2后绘制加色雪层 slots=%s"), *SlotList);
	}
	for (const FSectionColorTrack& Track : ColorTracks)
	{
		if (LoginSceneLateAdditiveOverlaySlots.Contains(Track.SlotIndex))
		{
			UE_LOG(LogTemp, Display, TEXT("WLK登录M2雪层颜色轨道: slot=%d baseRGBA=(%.4f,%.4f,%.4f,%.4f) samples=%d"),
				Track.SlotIndex,
				Track.BaseColor.R,
				Track.BaseColor.G,
				Track.BaseColor.B,
				Track.BaseColor.A,
				Track.Samples.Num());
		}
	}
	return true;
}

bool AWoWLoginGameMode::ApplyLoginSceneRuntimeCache(int32 SceneIndex)
{
	const FLoginSceneRuntimeCache* Cache = LoginSceneRuntimeCaches.Find(SceneIndex);
	if (!Cache || !Cache->bRuntimeLoaded)
	{
		return false;
	}

	TextureTransformTracks = Cache->TextureTransformTracks;
	RenderSections = Cache->RenderSections;
	ColorTracks = Cache->ColorTracks;
	LoginSceneLateAdditiveOverlaySlots = Cache->LateAdditiveOverlaySlots;
	CameraPositionSamples = Cache->CameraPositionSamples;
	CameraTargetSamples = Cache->CameraTargetSamples;
	CameraRollSamples = Cache->CameraRollSamples;
	BonePivots = Cache->BonePivots;
	LoginBones = Cache->LoginBones;
	BoneAnimationSamples = Cache->BoneAnimationSamples;
	LoginSceneLights = Cache->LoginSceneLights;
	ParticleEmitters = Cache->ParticleEmitters;
	CameraTrackDurationMs = Cache->CameraTrackDurationMs;
	return true;
}

void AWoWLoginGameMode::CreateLoginSceneLateAdditiveOverlay(USkeletalMeshComponent* SourceMeshComponent)
{
	LoginSceneLateAdditiveOverlayComponent.Reset();
	LoginSceneLateAdditiveOverlayDynamicMaterials.Reset();
	if (!SourceMeshComponent || !LoginSceneActor.IsValid() || LoginSceneLateAdditiveOverlaySlots.IsEmpty())
	{
		return;
	}

	USkeletalMeshComponent* OverlayComponent = NewObject<USkeletalMeshComponent>(LoginSceneActor.Get(), TEXT("WoW_Login_LateAdditiveOverlay"));
	if (!OverlayComponent)
	{
		return;
	}

	OverlayComponent->SetupAttachment(SourceMeshComponent);
	OverlayComponent->SetSkeletalMesh(SourceMeshComponent->GetSkeletalMeshAsset());
	OverlayComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	OverlayComponent->SetMobility(EComponentMobility::Movable);
	OverlayComponent->SetRelativeTransform(FTransform::Identity);
	OverlayComponent->SetVisibility(false, true);
	OverlayComponent->SetHiddenInGame(true, true);
	OverlayComponent->SetLeaderPoseComponent(SourceMeshComponent);
	OverlayComponent->TranslucencySortPriority = 1000;
	OverlayComponent->RegisterComponent();

	if (const FLoginSceneDefinition* SceneDefinition = GetActiveLoginSceneDefinition(this))
	{
		ApplyLoginSceneMaterials(OverlayComponent, *SceneDefinition);
	}

	const int32 MaterialSlotCount = SourceMeshComponent->GetNumMaterials();
	LoginSceneLateAdditiveOverlayDynamicMaterials.SetNum(MaterialSlotCount);
	for (int32 SlotIndex = 0; SlotIndex < MaterialSlotCount; ++SlotIndex)
	{
		OverlayComponent->ShowMaterialSection(SlotIndex, SlotIndex, false, 0);
	}

	int32 VisibleOverlaySlots = 0;
	for (const int32 SlotIndex : LoginSceneLateAdditiveOverlaySlots)
	{
		if (SlotIndex < 0 || SlotIndex >= MaterialSlotCount)
		{
			continue;
		}

		SourceMeshComponent->ShowMaterialSection(SlotIndex, SlotIndex, false, 0);
		OverlayComponent->ShowMaterialSection(SlotIndex, SlotIndex, true, 0);
		UMaterialInstanceDynamic* DynamicMaterial = OverlayComponent->CreateDynamicMaterialInstance(SlotIndex);
		LoginSceneLateAdditiveOverlayDynamicMaterials[SlotIndex] = DynamicMaterial;
		if (DynamicMaterial)
		{
			DynamicMaterial->SetVectorParameterValue(TEXT("UVScale"), FLinearColor(1.0f, 1.0f, 0.0f, 0.0f));
			DynamicMaterial->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor::Black);
			DynamicMaterial->SetScalarParameterValue(TEXT("UVRotationDegrees"), 0.0f);
		}
		++VisibleOverlaySlots;
	}

	LoginSceneLateAdditiveOverlayComponent = OverlayComponent;
	UE_LOG(LogTemp, Display, TEXT("WLK登录M2后绘制加色雪层已创建: slots=%d sortPriority=%d"), VisibleOverlaySlots, OverlayComponent->TranslucencySortPriority);
}

void AWoWLoginGameMode::CreateLoginSceneDebugHighlightOverlay(USkeletalMeshComponent* SourceMeshComponent)
{
	LoginSceneDebugHighlightComponent.Reset();
	LoginSceneDebugHighlightDynamicMaterials.Reset();
	if (!SourceMeshComponent || !LoginSceneActor.IsValid())
	{
		return;
	}

	USkeletalMeshComponent* HighlightComponent = NewObject<USkeletalMeshComponent>(LoginSceneActor.Get(), TEXT("WoW_Login_DebugSectionHighlight"));
	if (!HighlightComponent)
	{
		return;
	}

	HighlightComponent->SetupAttachment(SourceMeshComponent);
	HighlightComponent->SetSkeletalMesh(SourceMeshComponent->GetSkeletalMeshAsset());
	HighlightComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	HighlightComponent->SetMobility(EComponentMobility::Movable);
	HighlightComponent->SetRelativeTransform(FTransform::Identity);
	HighlightComponent->SetVisibility(true, true);
	HighlightComponent->SetHiddenInGame(false, true);
	HighlightComponent->SetLeaderPoseComponent(SourceMeshComponent);
	HighlightComponent->TranslucencySortPriority = 2000;
	HighlightComponent->RegisterComponent();

	UMaterialInterface* HighlightParent = ResolveLoadedObject<UMaterialInterface>(TEXT("/Game/WoW/Materials/M_WoWM2AdditiveAlphaUnlit.M_WoWM2AdditiveAlphaUnlit"));
	if (!HighlightParent)
	{
		HighlightParent = ResolveLoadedObject<UMaterialInterface>(TEXT("/Game/WoW/Materials/M_WoWM2AlphaBlendUnlit.M_WoWM2AlphaBlendUnlit"));
	}

	const int32 MaterialSlotCount = SourceMeshComponent->GetNumMaterials();
	LoginSceneDebugHighlightDynamicMaterials.SetNum(MaterialSlotCount);
	for (int32 SlotIndex = 0; SlotIndex < MaterialSlotCount; ++SlotIndex)
	{
		HighlightComponent->ShowMaterialSection(SlotIndex, SlotIndex, false, 0);
		if (HighlightParent)
		{
			HighlightComponent->SetMaterial(SlotIndex, HighlightParent);
			UMaterialInstanceDynamic* DynamicMaterial = HighlightComponent->CreateDynamicMaterialInstance(SlotIndex, HighlightParent);
			LoginSceneDebugHighlightDynamicMaterials[SlotIndex] = DynamicMaterial;
			if (DynamicMaterial)
			{
				DynamicMaterial->SetVectorParameterValue(TEXT("TintColor"), FLinearColor(0.15f, 0.95f, 1.0f, 1.0f));
				DynamicMaterial->SetScalarParameterValue(TEXT("Brightness"), 6.0f);
				DynamicMaterial->SetScalarParameterValue(TEXT("LayerAlphaWeight"), 0.0f);
				DynamicMaterial->SetScalarParameterValue(TEXT("LayerColorWeight"), 0.0f);
				DynamicMaterial->SetVectorParameterValue(TEXT("UVScale"), FLinearColor(1.0f, 1.0f, 0.0f, 0.0f));
				DynamicMaterial->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor::Black);
				DynamicMaterial->SetScalarParameterValue(TEXT("UVRotationDegrees"), 0.0f);
			}
		}
	}

	LoginSceneDebugHighlightComponent = HighlightComponent;
	SetLoginTextureDebugHighlightSection(INDEX_NONE);
	UE_LOG(LogTemp, Verbose, TEXT("WLK登录M2纹理section调试高亮层已创建: slots=%d sortPriority=%d"), MaterialSlotCount, HighlightComponent->TranslucencySortPriority);
}

void AWoWLoginGameMode::ApplyTextureSectionRenderOverride(int32 SectionIndex)
{
	const FSectionTextureTransformTrack* Track = GetLoginRenderSectionByIndex(SectionIndex);
	if (!Track)
	{
		return;
	}

	const FWoWLoginTextureSectionTuning Tuning = GetLoginTextureSectionTuning(SectionIndex);
	const int32 EffectiveRenderFlags = Tuning.RenderFlagOverride == INDEX_NONE ? Track->MaterialFlags : Tuning.RenderFlagOverride;
	const int32 EffectiveRenderMode = Tuning.BlendModeOverride == INDEX_NONE ? Track->RenderMode : Tuning.BlendModeOverride;
	UMaterialInterface* ParentMaterial = SelectTextureOverrideParentMaterial(
		Tuning.RenderOverride,
		EffectiveRenderMode,
		EffectiveRenderFlags,
		LoginSceneTuningState.bUseM2PhysicalLightBridge);
	UTexture2D* Texture = LoadGeneratedTextureFromPreviewPng(Track->PreviewPng);
	if (!ParentMaterial || !Texture)
	{
		return;
	}

	auto RebuildDynamicMaterial = [SectionIndex, ParentMaterial, Texture](USkeletalMeshComponent* MeshComponent, TArray<TObjectPtr<UMaterialInstanceDynamic>>& DynamicMaterials)
	{
		if (!MeshComponent || !DynamicMaterials.IsValidIndex(SectionIndex))
		{
			return;
		}

		MeshComponent->SetMaterial(SectionIndex, ParentMaterial);
		UMaterialInstanceDynamic* DynamicMaterial = MeshComponent->CreateDynamicMaterialInstance(SectionIndex, ParentMaterial);
		DynamicMaterials[SectionIndex] = DynamicMaterial;
		if (DynamicMaterial)
		{
			DynamicMaterial->SetTextureParameterValue(TEXT("DiffuseTexture"), Texture);
			DynamicMaterial->SetVectorParameterValue(TEXT("UVScale"), FLinearColor(1.0f, 1.0f, 0.0f, 0.0f));
			DynamicMaterial->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor::Black);
			DynamicMaterial->SetScalarParameterValue(TEXT("UVRotationDegrees"), 0.0f);
			DynamicMaterial->SetVectorParameterValue(TEXT("TintColor"), FLinearColor::White);
			DynamicMaterial->SetScalarParameterValue(TEXT("Brightness"), 1.0f);
			DynamicMaterial->SetScalarParameterValue(TEXT("LayerAlphaWeight"), 0.0f);
			DynamicMaterial->SetScalarParameterValue(TEXT("LayerColorWeight"), 0.0f);
		}
	};

	if (LoginSceneActor.IsValid())
	{
		RebuildDynamicMaterial(LoginSceneActor->GetSkeletalMeshComponent(), LoginSceneDynamicMaterials);
	}
	if (LoginSceneLateAdditiveOverlayComponent.IsValid())
	{
		RebuildDynamicMaterial(LoginSceneLateAdditiveOverlayComponent.Get(), LoginSceneLateAdditiveOverlayDynamicMaterials);
	}

	UE_LOG(LogTemp, Verbose, TEXT("WLK登录M2纹理section渲染覆盖: section=%d m2Flag=%d effectiveFlag=%d m2Blend=%d effectiveBlend=%d override=%s texture=%s"),
		SectionIndex,
		Track->MaterialFlags,
		EffectiveRenderFlags,
		Track->RenderMode,
		EffectiveRenderMode,
		*TextureRenderOverrideToString(Tuning.RenderOverride),
		*FPaths::GetCleanFilename(Track->PreviewPng));
}

void AWoWLoginGameMode::ReapplyLoginSceneRenderOverrides()
{
	for (const FSectionTextureTransformTrack& Section : RenderSections)
	{
		ApplyTextureSectionRenderOverride(Section.SlotIndex);
		ApplyTextureSectionVisibility(Section.SlotIndex);
	}
}

void AWoWLoginGameMode::ApplyTextureSectionVisibility(int32 SectionIndex)
{
	const FSectionTextureTransformTrack* Track = GetLoginRenderSectionByIndex(SectionIndex);
	if (!Track)
	{
		return;
	}

	const FWoWLoginTextureSectionTuning Tuning = GetLoginTextureSectionTuning(SectionIndex);
	const bool bShow = Tuning.bEnabled;
	const bool bOverlaySlot = LoginSceneLateAdditiveOverlaySlots.Contains(SectionIndex);
	if (LoginSceneActor.IsValid())
	{
		if (USkeletalMeshComponent* MeshComponent = LoginSceneActor->GetSkeletalMeshComponent())
		{
			MeshComponent->ShowMaterialSection(SectionIndex, SectionIndex, bShow && !bOverlaySlot, 0);
		}
	}
	if (LoginSceneLateAdditiveOverlayComponent.IsValid())
	{
		LoginSceneLateAdditiveOverlayComponent->ShowMaterialSection(SectionIndex, SectionIndex, bShow && bOverlaySlot, 0);
	}
	if (LoginSceneDebugHighlightComponent.IsValid() && SectionIndex == HighlightedTextureSectionIndex)
	{
		LoginSceneDebugHighlightComponent->ShowMaterialSection(SectionIndex, SectionIndex, bShow, 0);
	}

	UE_LOG(LogTemp, Verbose, TEXT("WLK登录M2纹理section显隐: section=%d material=%d enabled=%s texture=%s"),
		SectionIndex,
		Track->MaterialIndex,
		bShow ? TEXT("true") : TEXT("false"),
		*FPaths::GetCleanFilename(Track->PreviewPng));
}

void AWoWLoginGameMode::ApplyLoginSceneTextureAnimation(float TimeSeconds)
{
	const float GlobalBrightnessScale = FMath::Max(LoginSceneTuningState.GlobalBrightnessScale, 0.0f);
	const FLinearColor MaterialLightTint = ComputeLoginSceneMaterialLightTint();
	const float MaterialLightBrightness = ComputeLoginSceneMaterialLightBrightness();
	for (const FSectionTextureTransformTrack& Section : RenderSections)
	{
		const FWoWLoginTextureSectionTuning Tuning = GetLoginTextureSectionTuning(Section.SlotIndex);
		const float EffectiveBrightness = (Tuning.bForceVisibleDebug ? FMath::Max(12.0f, Tuning.BrightnessScale) : Tuning.BrightnessScale) *
			GlobalBrightnessScale *
			MaterialLightBrightness;
		FLinearColor EffectiveTint = Tuning.bForceVisibleDebug ? FLinearColor::White : MaterialLightTint;
		EffectiveTint.A = Tuning.bForceVisibleDebug ? FMath::Max(8.0f, Tuning.AlphaScale) : FMath::Max(Tuning.AlphaScale, 0.0f);
		if (LoginSceneDynamicMaterials.IsValidIndex(Section.SlotIndex) && LoginSceneDynamicMaterials[Section.SlotIndex])
		{
			LoginSceneDynamicMaterials[Section.SlotIndex]->SetScalarParameterValue(TEXT("Brightness"), EffectiveBrightness);
			LoginSceneDynamicMaterials[Section.SlotIndex]->SetVectorParameterValue(TEXT("TintColor"), EffectiveTint);
		}
		if (LoginSceneLateAdditiveOverlayDynamicMaterials.IsValidIndex(Section.SlotIndex) &&
			LoginSceneLateAdditiveOverlayDynamicMaterials[Section.SlotIndex])
		{
			LoginSceneLateAdditiveOverlayDynamicMaterials[Section.SlotIndex]->SetScalarParameterValue(TEXT("Brightness"), EffectiveBrightness);
			LoginSceneLateAdditiveOverlayDynamicMaterials[Section.SlotIndex]->SetVectorParameterValue(TEXT("TintColor"), EffectiveTint);
		}
	}

	for (const FSectionTextureTransformTrack& Track : TextureTransformTracks)
	{
		if (!LoginSceneDynamicMaterials.IsValidIndex(Track.SlotIndex) || !LoginSceneDynamicMaterials[Track.SlotIndex])
		{
			continue;
		}
		if (Track.Samples.Num() == 0)
		{
			continue;
		}

		const FWoWLoginTextureSectionTuning Tuning = GetLoginTextureSectionTuning(Track.SlotIndex);
		FVector2D Translation = FVector2D::ZeroVector;
		FVector2D Scaling = FVector2D::UnitVector;
		float RotationDegrees = 0.0f;
		if (Tuning.bUseM2TextureAnimation)
		{
			const float TimeMs = ResolveLoopTimeMs(TimeSeconds, Track.DurationMs);
			const FTextureTransformSample* A = nullptr;
			const FTextureTransformSample* B = nullptr;
			float Ratio = 0.0f;
			ResolveLoopingSamples<FTextureTransformSample>(
				Track.Samples,
				TimeMs,
				Track.DurationMs,
				[](const FTextureTransformSample& Sample)
				{
					return Sample.TimeMs;
				},
				A,
				B,
				Ratio);
			if (A && B)
			{
				Translation = FMath::Lerp(A->Translation, B->Translation, Ratio);
				Scaling = FMath::Lerp(A->Scaling, B->Scaling, Ratio);
				RotationDegrees = FMath::Lerp(A->RotationDegrees, B->RotationDegrees, Ratio);
			}
		}
		Translation += Tuning.UVOffset;
		Scaling *= Tuning.UVScale;
		RotationDegrees += Tuning.UVRotationDegrees;

		UMaterialInstanceDynamic* DynamicMaterial = LoginSceneDynamicMaterials[Track.SlotIndex];
		DynamicMaterial->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor(Translation.X, Translation.Y, 0.0f, 0.0f));
		DynamicMaterial->SetVectorParameterValue(TEXT("UVScale"), FLinearColor(Scaling.X, Scaling.Y, 0.0f, 0.0f));
		DynamicMaterial->SetScalarParameterValue(TEXT("UVRotationDegrees"), RotationDegrees);
		if (LoginSceneLateAdditiveOverlayDynamicMaterials.IsValidIndex(Track.SlotIndex) &&
			LoginSceneLateAdditiveOverlayDynamicMaterials[Track.SlotIndex])
		{
			UMaterialInstanceDynamic* OverlayDynamicMaterial = LoginSceneLateAdditiveOverlayDynamicMaterials[Track.SlotIndex];
			OverlayDynamicMaterial->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor(Translation.X, Translation.Y, 0.0f, 0.0f));
			OverlayDynamicMaterial->SetVectorParameterValue(TEXT("UVScale"), FLinearColor(Scaling.X, Scaling.Y, 0.0f, 0.0f));
			OverlayDynamicMaterial->SetScalarParameterValue(TEXT("UVRotationDegrees"), RotationDegrees);
		}
		if (Track.SlotIndex == HighlightedTextureSectionIndex &&
			LoginSceneDebugHighlightDynamicMaterials.IsValidIndex(Track.SlotIndex) &&
			LoginSceneDebugHighlightDynamicMaterials[Track.SlotIndex])
		{
			UMaterialInstanceDynamic* HighlightDynamicMaterial = LoginSceneDebugHighlightDynamicMaterials[Track.SlotIndex];
			HighlightDynamicMaterial->SetVectorParameterValue(TEXT("UVOffset"), FLinearColor(Translation.X, Translation.Y, 0.0f, 0.0f));
			HighlightDynamicMaterial->SetVectorParameterValue(TEXT("UVScale"), FLinearColor(Scaling.X, Scaling.Y, 0.0f, 0.0f));
			HighlightDynamicMaterial->SetScalarParameterValue(TEXT("UVRotationDegrees"), RotationDegrees);
		}
	}
}

FLinearColor AWoWLoginGameMode::ComputeLoginSceneMaterialLightTint() const
{
	if (LoginSceneLights.IsEmpty())
	{
		return FLinearColor::White;
	}

	FLinearColor Accumulated = FLinearColor::Black;
	float WeightSum = 0.0f;
	for (const FLoginSceneLight& Light : LoginSceneLights)
	{
		const FWoWLoginLightTuning Tuning = GetLoginSceneLightTuning(Light.Index);
		if (!Tuning.bEnabled)
		{
			continue;
		}

		const float AmbientWeight = FMath::Max(Light.AmbientIntensity * Tuning.AmbientIntensityScale, 0.0f);
		const float DiffuseWeight = FMath::Max(Light.DiffuseIntensity * Tuning.DiffuseIntensityScale, 0.0f);
		Accumulated += Light.AmbientColor * Tuning.AmbientColorScale * AmbientWeight;
		Accumulated += Light.DiffuseColor * Tuning.DiffuseColorScale * DiffuseWeight;
		WeightSum += AmbientWeight + DiffuseWeight;
	}

	if (WeightSum <= UE_SMALL_NUMBER)
	{
		return FLinearColor::White;
	}

	FLinearColor Tint = Accumulated / WeightSum;
	const float MaxChannel = FMath::Max3(Tint.R, Tint.G, Tint.B);
	if (MaxChannel > 1.0f)
	{
		Tint.R /= MaxChannel;
		Tint.G /= MaxChannel;
		Tint.B /= MaxChannel;
	}
	Tint.A = 1.0f;
	return Tint;
}

float AWoWLoginGameMode::ComputeLoginSceneMaterialLightBrightness() const
{
	if (LoginSceneLights.IsEmpty())
	{
		return 1.0f;
	}

	float Brightness = 0.35f;
	for (const FLoginSceneLight& Light : LoginSceneLights)
	{
		const FWoWLoginLightTuning Tuning = GetLoginSceneLightTuning(Light.Index);
		if (!Tuning.bEnabled)
		{
			continue;
		}

		const float Ambient = FMath::Max(Light.AmbientIntensity * Tuning.AmbientIntensityScale, 0.0f);
		const float Diffuse = FMath::Max(Light.DiffuseIntensity * Tuning.DiffuseIntensityScale, 0.0f);
		Brightness += (Ambient * 0.35f + Diffuse * 0.65f) * FMath::Max(Tuning.AttenuationRadiusScale, 0.01f);
	}

	return FMath::Clamp(Brightness, 0.15f, 4.0f);
}

void AWoWLoginGameMode::ApplyLoginSceneColorAnimation(float TimeSeconds)
{
	const FLinearColor MaterialLightTint = ComputeLoginSceneMaterialLightTint();
	for (const FSectionColorTrack& Track : ColorTracks)
	{
		if (!LoginSceneDynamicMaterials.IsValidIndex(Track.SlotIndex) || !LoginSceneDynamicMaterials[Track.SlotIndex])
		{
			continue;
		}
		if (Track.Samples.Num() == 0)
		{
			continue;
		}

		const float TimeMs = ResolveLoopTimeMs(TimeSeconds, Track.DurationMs);
		const FColorSample* A = nullptr;
		const FColorSample* B = nullptr;
		float Ratio = 0.0f;
		ResolveLoopingSamples<FColorSample>(
			Track.Samples,
			TimeMs,
			Track.DurationMs,
			[](const FColorSample& Sample)
			{
				return Sample.TimeMs;
			},
			A,
			B,
			Ratio);
		if (!A || !B)
		{
			continue;
		}
		const FLinearColor AbsoluteColor = FMath::Lerp(A->Color, B->Color, Ratio);
		FLinearColor Color = FLinearColor::White;
		Color.R = Track.BaseColor.R > UE_SMALL_NUMBER ? AbsoluteColor.R / Track.BaseColor.R : AbsoluteColor.R;
		Color.G = Track.BaseColor.G > UE_SMALL_NUMBER ? AbsoluteColor.G / Track.BaseColor.G : AbsoluteColor.G;
		Color.B = Track.BaseColor.B > UE_SMALL_NUMBER ? AbsoluteColor.B / Track.BaseColor.B : AbsoluteColor.B;
		Color.A = Track.BaseColor.A > UE_SMALL_NUMBER ? AbsoluteColor.A / Track.BaseColor.A : AbsoluteColor.A;
		const FWoWLoginTextureSectionTuning Tuning = GetLoginTextureSectionTuning(Track.SlotIndex);
		Color.A *= Tuning.bForceVisibleDebug ? FMath::Max(8.0f, Tuning.AlphaScale) : Tuning.AlphaScale;
		if (Tuning.bForceVisibleDebug)
		{
			Color.R = 1.0f;
			Color.G = 1.0f;
			Color.B = 1.0f;
		}
		else
		{
			Color.R *= MaterialLightTint.R;
			Color.G *= MaterialLightTint.G;
			Color.B *= MaterialLightTint.B;
		}
		LoginSceneDynamicMaterials[Track.SlotIndex]->SetVectorParameterValue(TEXT("TintColor"), Color);
		if (LoginSceneLateAdditiveOverlayDynamicMaterials.IsValidIndex(Track.SlotIndex) &&
			LoginSceneLateAdditiveOverlayDynamicMaterials[Track.SlotIndex])
		{
			LoginSceneLateAdditiveOverlayDynamicMaterials[Track.SlotIndex]->SetVectorParameterValue(TEXT("TintColor"), Color);
		}
	}
}

void AWoWLoginGameMode::SpawnLoginParticleSystems()
{
	ParticleSystems.Reset();
	if (!LoginSceneActor.IsValid())
	{
		return;
	}

	AActor* SceneOwner = LoginSceneActor.Get();
	int32 CreatedCount = 0;
	for (const FLoginParticleEmitter& Emitter : ParticleEmitters)
	{
		const FWoWLoginParticleEmitterTuning Tuning = GetLoginParticleEmitterTuning(Emitter.Index);
		UTexture2D* Texture = LoadGeneratedTextureFromPreviewPng(Emitter.PreviewPng);
		UMaterialInterface* ParentMaterial = SelectParticleParentMaterial(Emitter.Blend);
		if (!Texture || !ParentMaterial)
		{
			UE_LOG(LogTemp, Warning, TEXT("跳过WLK登录M2粒子，缺少贴图或材质: emitter=%d png=%s"), Emitter.Index, *Emitter.PreviewPng);
			continue;
		}

		UProceduralMeshComponent* MeshComponent = NewObject<UProceduralMeshComponent>(SceneOwner);
		if (!MeshComponent)
		{
			continue;
		}

		MeshComponent->SetMobility(EComponentMobility::Movable);
		MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		MeshComponent->SetTranslucentSortPriority(1000 + Emitter.Index);
		MeshComponent->bUseAsyncCooking = false;
		MeshComponent->RegisterComponent();
		MeshComponent->AttachToComponent(SceneOwner->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);
		MeshComponent->SetVisibility(false, true);
		MeshComponent->SetHiddenInGame(true, true);

		UMaterialInstanceDynamic* DynamicMaterial = UMaterialInstanceDynamic::Create(ParentMaterial, MeshComponent);
		if (DynamicMaterial)
		{
			DynamicMaterial->SetTextureParameterValue(TEXT("DiffuseTexture"), Texture);
			DynamicMaterial->SetVectorParameterValue(TEXT("TintColor"), FLinearColor::White);
			DynamicMaterial->SetScalarParameterValue(TEXT("Brightness"), GetLoginParticleBaseBrightness(Emitter) * FMath::Max(Tuning.BrightnessScale, 0.0f));
			DynamicMaterial->SetScalarParameterValue(TEXT("AlphaCutoff"), FMath::Clamp(Tuning.AlphaCutoff, 0.0f, 0.95f));
			DynamicMaterial->SetScalarParameterValue(TEXT("LegacyGammaWeight"), GetLoginParticleLegacyGammaWeight(Emitter));
			MeshComponent->SetMaterial(0, DynamicMaterial);
		}

		FLoginParticleSystem System;
		System.Emitter = Emitter;
		System.MeshComponent = MeshComponent;
		System.DynamicMaterial = DynamicMaterial;
		System.RandomSeed = Emitter.Index * 7919;
		System.MaxParticles = FMath::Clamp(CVarWoWLoginMaxParticlesPerEmitter.GetValueOnGameThread(), 1, MaxParticlesPerEmitter);
		System.BrightnessScale = FMath::Max(Tuning.BrightnessScale, 0.0f);
		System.AlphaCutoff = FMath::Clamp(Tuning.AlphaCutoff, 0.0f, 0.95f);
		System.LegacyGammaWeight = GetLoginParticleLegacyGammaWeight(Emitter);
		System.bUseLoginSceneTuning = true;
		ParticleSystems.Add(MoveTemp(System));
		++CreatedCount;
	}

	UE_LOG(LogTemp, Display, TEXT("WLK登录M2粒子桥接: emitters=%d created=%d"), ParticleEmitters.Num(), CreatedCount);
}

void AWoWLoginGameMode::DestroyCharacterSelectPreviewAttachmentParticleSystems()
{
	for (FLoginParticleSystem& System : CharacterSelectPreviewAttachmentParticleSystems)
	{
		if (System.MeshComponent)
		{
			System.MeshComponent->DestroyComponent();
		}
	}
	CharacterSelectPreviewAttachmentParticleSystems.Reset();
}

void AWoWLoginGameMode::SpawnCharacterPreviewAttachmentParticleSystems(
	USkeletalMeshComponent* AttachmentComponent,
	const FString& AttachmentName,
	const FString& AttachmentMeshPath,
	const FString& AttachmentMeshJsonPath,
	const FString& AttachmentDescriptorPath)
{
	if (!AttachmentComponent)
	{
		return;
	}

	const FString MeshJsonPath = !AttachmentMeshJsonPath.IsEmpty()
		? AttachmentMeshJsonPath
		: BuildCharacterPreviewAttachmentMeshJsonPath(AttachmentMeshPath, AttachmentDescriptorPath);
	if (MeshJsonPath.IsEmpty() || !FPaths::FileExists(MeshJsonPath))
	{
		return;
	}

	TArray<FLoginParticleEmitter> AttachmentEmitters;
	TArray<FVector> AttachmentBonePivots;
	if (!ReadCharacterPreviewAttachmentParticleData(MeshJsonPath, AttachmentEmitters, AttachmentBonePivots) || AttachmentEmitters.IsEmpty())
	{
		return;
	}

	AActor* PreviewOwner = AttachmentComponent->GetOwner();
	if (!PreviewOwner)
	{
		return;
	}

	int32 CreatedCount = 0;
	for (const FLoginParticleEmitter& Emitter : AttachmentEmitters)
	{
		UTexture2D* Texture = LoadGeneratedTextureFromPreviewPng(Emitter.PreviewPng);
		UMaterialInterface* ParentMaterial = SelectParticleParentMaterial(Emitter.Blend);
		if (!Texture || !ParentMaterial)
		{
			UE_LOG(LogTemp, Warning, TEXT("角色预览附件粒子跳过，缺少贴图或材质: name=%s emitter=%d png=%s"),
				*AttachmentName,
				Emitter.Index,
				*Emitter.PreviewPng);
			continue;
		}

		UProceduralMeshComponent* MeshComponent = NewObject<UProceduralMeshComponent>(PreviewOwner);
		if (!MeshComponent)
		{
			continue;
		}

		MeshComponent->SetMobility(EComponentMobility::Movable);
		MeshComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		MeshComponent->SetCastShadow(false);
		MeshComponent->SetTranslucentSortPriority(1200 + CharacterSelectPreviewAttachmentParticleSystems.Num());
		MeshComponent->bUseAsyncCooking = false;
		PreviewOwner->AddInstanceComponent(MeshComponent);
		MeshComponent->RegisterComponent();
		// Particle vertices are emitted in world space. Keep this component unparented so
		// existing particle history is not re-transformed when the preview actor is rotated.
		MeshComponent->SetVisibility(true, true);
		MeshComponent->SetHiddenInGame(false, true);

		UMaterialInstanceDynamic* DynamicMaterial = UMaterialInstanceDynamic::Create(ParentMaterial, MeshComponent);
		if (DynamicMaterial)
		{
			DynamicMaterial->SetTextureParameterValue(TEXT("DiffuseTexture"), Texture);
			DynamicMaterial->SetVectorParameterValue(TEXT("TintColor"), FLinearColor::White);
			DynamicMaterial->SetScalarParameterValue(TEXT("Brightness"), 1.0f);
			DynamicMaterial->SetScalarParameterValue(TEXT("AlphaCutoff"), 0.0f);
			DynamicMaterial->SetScalarParameterValue(TEXT("LegacyGammaWeight"), 0.0f);
			MeshComponent->SetMaterial(0, DynamicMaterial);
		}

		FLoginParticleSystem System;
		System.Emitter = Emitter;
		System.MeshComponent = MeshComponent;
		System.DynamicMaterial = DynamicMaterial;
		System.SourceMeshComponent = AttachmentComponent;
		System.SourceBonePivots = AttachmentBonePivots;
		System.RandomSeed = HashCombine(GetTypeHash(AttachmentName), GetTypeHash(Emitter.Index));
		System.MaxParticles = FMath::Clamp(CVarWoWLoginMaxAttachmentParticlesPerEmitter.GetValueOnGameThread(), 1, MaxAttachmentParticlesPerEmitter);
		System.BrightnessScale = 1.0f;
		System.AlphaCutoff = 0.0f;
		System.LegacyGammaWeight = 0.0f;
		System.bUseLoginSceneTuning = false;
		CharacterSelectPreviewAttachmentParticleSystems.Add(MoveTemp(System));
		++CreatedCount;
	}

	UE_LOG(LogTemp, Display, TEXT("角色预览附件粒子已挂载: name=%s emitters=%d created=%d json=%s maxPerEmitter=%d"),
		*AttachmentName,
		AttachmentEmitters.Num(),
		CreatedCount,
		*MeshJsonPath,
		MaxAttachmentParticlesPerEmitter);
}

void AWoWLoginGameMode::SpawnLoginSceneLights()
{
	for (FLoginSceneLightComponent& LightComponent : LoginSceneLightComponents)
	{
		if (LightComponent.Component.IsValid())
		{
			LightComponent.Component->DestroyComponent();
		}
	}
	LoginSceneLightComponents.Reset();
	if (LoginSceneAmbientLightComponent.IsValid())
	{
		LoginSceneAmbientLightComponent->DestroyComponent();
	}
	LoginSceneAmbientLightComponent.Reset();
	if (!LoginSceneActor.IsValid() || LoginSceneLights.IsEmpty())
	{
		return;
	}

	if (!LoginSceneTuningState.bUseM2PhysicalLightBridge)
	{
		UE_LOG(LogTemp, Display, TEXT("WLK登录M2灯光数据已读取但不生成UE物理灯: lights=%d bridge=Off"), LoginSceneLights.Num());
		return;
	}

	AActor* SceneOwner = LoginSceneActor.Get();
	int32 PointLightCount = 0;
	for (const FLoginSceneLight& Light : LoginSceneLights)
	{
		FWoWLoginLightTuning Tuning = GetLoginSceneLightTuning(Light.Index);
		if (!Tuning.bEnabled || Light.DiffuseIntensity <= KINDA_SMALL_NUMBER)
		{
			continue;
		}

		UPointLightComponent* PointLight = NewObject<UPointLightComponent>(SceneOwner);
		if (!PointLight)
		{
			continue;
		}

		const FLinearColor LightColor = Light.DiffuseColor * Tuning.DiffuseColorScale;
		PointLight->SetMobility(EComponentMobility::Movable);
		PointLight->SetLightColor(LightColor);
		PointLight->SetIntensity(Light.DiffuseIntensity * Tuning.DiffuseIntensityScale * LoginSceneTuningState.M2PhysicalLightIntensityScale);
		PointLight->SetAttenuationRadius(FMath::Max(Light.AttenuationEnd * WowScalarToUE * FMath::Max(Tuning.AttenuationRadiusScale, 0.01f), 10.0f));
		PointLight->bUseInverseSquaredFalloff = false;
		PointLight->LightFalloffExponent = 2.0f;
		PointLight->CastShadows = false;
		PointLight->RegisterComponent();
		PointLight->AttachToComponent(SceneOwner->GetRootComponent(), FAttachmentTransformRules::KeepWorldTransform);

		FLoginSceneLightComponent LightComponent;
		LightComponent.Light = Light;
		LightComponent.Component = PointLight;
		LoginSceneLightComponents.Add(LightComponent);
		++PointLightCount;
	}

	UpdateLoginSceneLights();
	UE_LOG(LogTemp, Display, TEXT("WLK登录M2物理灯调试桥接: source=%d point=%d globalScale=%.3f"),
		LoginSceneLights.Num(),
		PointLightCount,
		LoginSceneTuningState.M2PhysicalLightIntensityScale);
}

void AWoWLoginGameMode::UpdateLoginSceneLights()
{
	for (FLoginSceneLightComponent& LightComponent : LoginSceneLightComponents)
	{
		if (!LightComponent.Component.IsValid())
		{
			continue;
		}

		const FVector WorldPosition = GetM2BoneWorldPosition(LightComponent.Light.BoneIndex, LightComponent.Light.Position);
		LightComponent.Component->SetWorldLocation(WorldPosition);
	}
}

FVector AWoWLoginGameMode::GetM2BoneWorldPosition(int32 BoneIndex, const FVector& M2ModelPosition) const
{
	if (!LoginSceneActor.IsValid())
	{
		return M2ModelPosition;
	}

	const USkeletalMeshComponent* MeshComponent = LoginSceneActor->GetSkeletalMeshComponent();
	if (!MeshComponent || BoneIndex < 0)
	{
		return LoginSceneActor->GetActorTransform().TransformPosition(M2ModelPosition);
	}

	const FName BoneName(*FString::Printf(TEXT("M2_Bone_%03d"), BoneIndex));
	const int32 UEBoneIndex = MeshComponent->GetBoneIndex(BoneName);
	if (UEBoneIndex == INDEX_NONE)
	{
		return MeshComponent->GetComponentTransform().TransformPosition(M2ModelPosition);
	}

	const FTransform BoneTransform = MeshComponent->GetBoneTransform(UEBoneIndex);
	const FVector BoneReferencePosition = BonePivots.IsValidIndex(BoneIndex) ? BonePivots[BoneIndex] : FVector::ZeroVector;
	return BoneTransform.TransformPosition(M2ModelPosition - BoneReferencePosition);
}

FQuat AWoWLoginGameMode::GetM2BoneWorldRotation(int32 BoneIndex) const
{
	if (!LoginSceneActor.IsValid())
	{
		return FQuat::Identity;
	}

	const USkeletalMeshComponent* MeshComponent = LoginSceneActor->GetSkeletalMeshComponent();
	if (!MeshComponent || BoneIndex < 0)
	{
		return LoginSceneActor->GetActorTransform().GetRotation();
	}

	const FName BoneName(*FString::Printf(TEXT("M2_Bone_%03d"), BoneIndex));
	const int32 UEBoneIndex = MeshComponent->GetBoneIndex(BoneName);
	if (UEBoneIndex == INDEX_NONE)
	{
		return MeshComponent->GetComponentTransform().GetRotation();
	}

	const FTransform BoneTransform = MeshComponent->GetBoneTransform(UEBoneIndex);
	return BoneTransform.GetRotation();
}

void AWoWLoginGameMode::UpdateLoginParticleSystems(float DeltaSeconds, float TimeSeconds)
{
	UpdateParticleSystems(ParticleSystems, DeltaSeconds, TimeSeconds);
	UpdateParticleSystems(CharacterSelectPreviewAttachmentParticleSystems, DeltaSeconds, TimeSeconds);
}

void AWoWLoginGameMode::UpdateParticleSystems(TArray<FLoginParticleSystem>& Systems, float DeltaSeconds, float TimeSeconds)
{
	if (Systems.IsEmpty() || !LoginCameraActor.IsValid())
	{
		return;
	}

	const float SafeDeltaSeconds = FMath::Clamp(DeltaSeconds, 0.0f, 1.0f / 20.0f);
	const FVector CameraLocation = LoginCameraActor->GetActorLocation();
	const FRotator CameraRotation = LoginCameraActor->GetActorRotation();
	const FVector CameraRight = CameraRotation.RotateVector(FVector::RightVector);
	const FVector CameraUp = CameraRotation.RotateVector(FVector::UpVector);

	for (FLoginParticleSystem& System : Systems)
	{
		if (!System.MeshComponent)
		{
			continue;
		}

		const FLoginParticleEmitter& Emitter = System.Emitter;
		FWoWLoginParticleEmitterTuning Tuning;
		if (System.bUseLoginSceneTuning)
		{
			Tuning = GetLoginParticleEmitterTuning(Emitter.Index);
		}
		else
		{
			Tuning.EmitterIndex = Emitter.Index;
			Tuning.BrightnessScale = System.BrightnessScale;
			Tuning.AlphaCutoff = System.AlphaCutoff;
		}
		System.MeshComponent->SetVisibility(Tuning.bEnabled, true);
		if (!Tuning.bEnabled)
		{
			System.Particles.Reset();
			System.SpawnRemainder = 0.0f;
			if (System.bMeshSectionInitialized)
			{
				System.MeshComponent->ClearAllMeshSections();
				System.bMeshSectionInitialized = false;
			}
			continue;
		}

		const float Lifespan = FMath::Max(
			SampleParticleFloatTrack(Emitter.LifespanSamples, TimeSeconds, 0.0f) * FMath::Max(Tuning.LifespanScale, 0.01f),
			0.1f);
		const float RawEmissionRate = FMath::Max(
			SampleParticleFloatTrack(Emitter.EmissionRateSamples, TimeSeconds, 0.0f) * FMath::Max(Tuning.EmissionRateScale, 0.0f),
			0.0f);
		const float LifespanForSpawn = Lifespan;
		// WMV: ftospawn = (dt * frate / flife) + rem
		const float SpawnUnits = (SafeDeltaSeconds * RawEmissionRate / LifespanForSpawn) + System.SpawnRemainder;
		const int32 ParticleLimit = FMath::Max(System.MaxParticles, 1);
		const int32 SpawnCount = FMath::Min(FMath::FloorToInt(SpawnUnits), ParticleLimit - System.Particles.Num());
		System.SpawnRemainder = FMath::Max(0.0f, SpawnUnits - static_cast<float>(SpawnCount));
		const float RawSpeed = SampleParticleFloatTrack(Emitter.EmissionSpeedSamples, TimeSeconds, 0.0f) * FMath::Max(Tuning.EmissionSpeedScale, 0.0f);
		const float Variation = SampleParticleFloatTrack(Emitter.SpeedVariationSamples, TimeSeconds, 0.0f);
		const float VerticalSpread = SampleParticleFloatTrack(Emitter.VerticalRangeSamples, TimeSeconds, 0.0f) * FMath::Max(Tuning.VerticalRangeScale, 0.0f);
		const float HorizontalSpread = SampleParticleFloatTrack(Emitter.HorizontalRangeSamples, TimeSeconds, 0.0f) * FMath::Max(Tuning.HorizontalRangeScale, 0.0f);
		const bool bModelSpaceParticle = ShouldKeepParticleInModelSpace(Emitter.Flags, Emitter.Index);
		const float ParticleWorldScale = GetParticleSystemWorldScale(System);


		if (SpawnCount > 0)
		{
			FRandomStream Random(System.RandomSeed + FMath::FloorToInt(TimeSeconds * 1000.0f));
			const FVector EmitterWorldPosition = GetParticleSystemBoneWorldPosition(System, Emitter.BoneIndex, Emitter.Position + Tuning.PositionOffset);
			const FQuat BoneWorldRotation = GetParticleSystemBoneWorldRotation(System, Emitter.BoneIndex);
			const float AreaLengthM2 = SampleParticleFloatTrack(Emitter.EmissionAreaLengthSamples, TimeSeconds, 1.0f) * FMath::Max(Tuning.AreaLengthScale, 0.0f) * 0.5f;
			const float AreaWidthM2 = SampleParticleFloatTrack(Emitter.EmissionAreaWidthSamples, TimeSeconds, 1.0f) * FMath::Max(Tuning.AreaWidthScale, 0.0f) * 0.5f;
			const int32 TileCount = FMath::Max(Emitter.TextureColumns * Emitter.TextureRows, 1);
			const FVector BaseDirM2(0.0f, 1.0f, 0.0f);
			const FVector BaseDirUE = ConvertWMVParticleVectorToUE(BaseDirM2);

			for (int32 Index = 0; Index < SpawnCount; ++Index)
			{
				FLoginParticleInstance Particle;
				FVector Direction = BoneWorldRotation.RotateVector(BaseDirUE).GetSafeNormal(UE_SMALL_NUMBER, BaseDirUE);
				bool bUseZeroVelocity = false;

				if (Emitter.EmitterType == 2)
				{
					// WMV SphereParticleEmitter::newParticle 默认分支:
					// 1. 用 vertical/horizontal range 构造 spread matrix
					// 2. parent bone rotation 后再做 M2 轴交换, 以球形偏移决定出生位置
					// 3. 非 0x100 标志时, 速度方向跟随球面偏移; 0x100 时沿骨骼正前方
					const float Radius = Random.FRandRange(0.0f, 1.0f);
					const FVector SphereSpreadM2 = BuildWMVSphereEmitterSpreadM2(
						VerticalSpread,
						HorizontalSpread,
						AreaLengthM2,
						AreaWidthM2,
						Radius,
						Random);
					const FVector SphereOffsetModelUE = ConvertM2PositionToUE(SphereSpreadM2) * ParticleWorldScale;
					const FVector SphereOffsetUE = BoneWorldRotation.RotateVector(SphereOffsetModelUE);
					const FVector SphereLocalOffsetUE = SphereOffsetModelUE;
					Particle.Position = bModelSpaceParticle ? SphereLocalOffsetUE : EmitterWorldPosition + SphereOffsetUE;

					const FVector DirectionSource = bModelSpaceParticle ? SphereLocalOffsetUE : SphereOffsetUE;
					if (DirectionSource.IsNearlyZero(UE_SMALL_NUMBER) && (Emitter.Flags & 0x100) == 0)
					{
						bUseZeroVelocity = true;
					}
					else if ((Emitter.Flags & 0x100) == 0x100)
					{
						Direction = bModelSpaceParticle ? BaseDirUE : BoneWorldRotation.RotateVector(BaseDirUE).GetSafeNormal(UE_SMALL_NUMBER, BaseDirUE);
					}
					else
					{
						Direction = DirectionSource.GetSafeNormal(UE_SMALL_NUMBER, bModelSpaceParticle ? BaseDirUE : BoneWorldRotation.RotateVector(BaseDirUE).GetSafeNormal(UE_SMALL_NUMBER, BaseDirUE));
					}
				}
				else
				{
					const FVector AreaOffsetModelUE = ConvertM2PositionToUE(FVector(
						Random.FRandRange(-AreaLengthM2, AreaLengthM2),
						0.0f,
						Random.FRandRange(-AreaWidthM2, AreaWidthM2))) * ParticleWorldScale;
					const FVector AreaOffsetUE = BoneWorldRotation.RotateVector(AreaOffsetModelUE);
					Particle.Position = bModelSpaceParticle ? AreaOffsetModelUE : EmitterWorldPosition + AreaOffsetUE;
				}
				Particle.Origin = Particle.Position;

				// ============================================
				// 粒子速度 (WMV: speed = normalize(dir) * spd * (1 + rand(-var, var)))
				// ============================================
				Particle.Direction = Direction;
				if (bUseZeroVelocity)
				{
					Particle.Velocity = FVector::ZeroVector;
				}
				else
				{
					Particle.Velocity = Direction * (RawSpeed * WowScalarToUE * ParticleWorldScale) * (1.0f + Random.FRandRange(-Variation, Variation));
				}
				Particle.Lifespan = Lifespan;
				Particle.Down = ConvertWMVParticleVectorToUE(FVector(0.0f, -1.0f, 0.0f));
				Particle.SizeScale = 1.0f;
				Particle.InitialTileIndex = Random.RandRange(0, TileCount - 1);
				Particle.TileIndex = Particle.InitialTileIndex;
				System.Particles.Add(Particle);
			}
		}

		// ============================================
		// 粒子物理更新 (基于 WMV particle.cpp::update)
		// 公式:
		//   1. speed += down * gravity * dt
		//   2. if slowdown>0: mspeed = exp(-slowdown * particle_life)
		//   3. pos += speed * mspeed * dt
		//   4. life += dt
		// ============================================
		const float Gravity = SampleParticleFloatTrack(Emitter.GravitySamples, TimeSeconds, 0.0f) * FMath::Max(Tuning.GravityScale, 0.0f) * WowScalarToUE * ParticleWorldScale;
		const float Deaccel = SampleParticleFloatTrack(Emitter.Gravity2Samples, TimeSeconds, 0.0f) * FMath::Max(Tuning.Gravity2Scale, 0.0f) * WowScalarToUE * ParticleWorldScale;

		for (int32 ParticleIndex = System.Particles.Num() - 1; ParticleIndex >= 0; --ParticleIndex)
		{
			FLoginParticleInstance& Particle = System.Particles[ParticleIndex];

			// WMV: p.speed += p.down * grav * dt - p.dir * deaccel * dt
			Particle.Velocity += Particle.Down * Gravity * SafeDeltaSeconds;
			Particle.Velocity -= Particle.Direction * Deaccel * SafeDeltaSeconds;

			float MoveSpeed = 1.0f;
			if (Emitter.Slowdown > 0.0f)
			{
				MoveSpeed = FMath::Exp(-Emitter.Slowdown * Particle.Age);
			}
			Particle.Position += Particle.Velocity * MoveSpeed * SafeDeltaSeconds;

			Particle.Age += SafeDeltaSeconds;
			if (Particle.Age >= Particle.Lifespan)
			{
				System.Particles.RemoveAtSwap(ParticleIndex, 1, EAllowShrinking::No);
			}
		}

		EnsureParticleRenderBuffers(System);
		const int32 RenderParticleCapacity = FMath::Max(System.MaxParticles, 1);

		const FQuat ParticleEmitterWorldRotation = GetParticleSystemBoneWorldRotation(System, Emitter.BoneIndex);
		const FVector EmitterRenderWorldPosition = bModelSpaceParticle ? GetParticleSystemBoneWorldPosition(System, Emitter.BoneIndex, Emitter.Position + Tuning.PositionOffset) : FVector::ZeroVector;
		const FQuat EmitterRenderWorldRotation = bModelSpaceParticle ? ParticleEmitterWorldRotation : FQuat::Identity;
		const FVector HiddenParticlePosition = EmitterRenderWorldPosition;
		FVector2D HiddenTileUVs[4];
		BuildParticleTileUVs(0, Emitter.TextureColumns, Emitter.TextureRows, HiddenTileUVs);
		ApplyParticleTileRotation(HiddenTileUVs, Emitter.TextureTileRotation);
		const bool bBillboard = (Emitter.Flags & 0x1000) == 0;
		const FVector NonBillboardRight = ParticleEmitterWorldRotation.RotateVector(FVector::RightVector);
		const FVector NonBillboardUp = ParticleEmitterWorldRotation.RotateVector(FVector::UpVector);

		for (int32 RenderParticleIndex = 0; RenderParticleIndex < RenderParticleCapacity; ++RenderParticleIndex)
		{
			const int32 BaseVertex = RenderParticleIndex * 4;
			if (!System.Particles.IsValidIndex(RenderParticleIndex))
			{
				for (int32 Corner = 0; Corner < 4; ++Corner)
				{
					System.RenderVertices[BaseVertex + Corner] = HiddenParticlePosition;
					System.RenderNormals[BaseVertex + Corner] = FVector::ForwardVector;
					System.RenderUVs[BaseVertex + Corner] = HiddenTileUVs[Corner];
					System.RenderVertexColors[BaseVertex + Corner] = FLinearColor::Transparent;
				}
				continue;
			}

			const FLoginParticleInstance& Particle = System.Particles[RenderParticleIndex];
			const FVector ParticleWorldPosition = bModelSpaceParticle
				? EmitterRenderWorldPosition + EmitterRenderWorldRotation.RotateVector(Particle.Position)
				: Particle.Position;
			const float LifeRatio = FMath::Clamp(Particle.Age / FMath::Max(Particle.Lifespan, KINDA_SMALL_NUMBER), 0.0f, 1.0f);
			const float LifecycleSizes[3] =
			{
				Emitter.LifecycleSizes[0] * FMath::Max(Tuning.LifecycleSizeStartScale, 0.0f),
				Emitter.LifecycleSizes[1] * FMath::Max(Tuning.LifecycleSizeMidScale, 0.0f),
				Emitter.LifecycleSizes[2] * FMath::Max(Tuning.LifecycleSizeEndScale, 0.0f)
			};
			FLinearColor LifecycleColors[3] =
			{
				Emitter.LifecycleColors[0],
				Emitter.LifecycleColors[1],
				Emitter.LifecycleColors[2]
			};
			LifecycleColors[0].A *= FMath::Max(Tuning.LifecycleAlphaStartScale, 0.0f);
			LifecycleColors[1].A *= FMath::Max(Tuning.LifecycleAlphaMidScale, 0.0f);
			LifecycleColors[2].A *= FMath::Max(Tuning.LifecycleAlphaEndScale, 0.0f);
			const float M2Size = ParticleLifeRamp<float>(
				LifeRatio,
				Emitter.MidPoint,
				LifecycleSizes[0],
				LifecycleSizes[1],
				LifecycleSizes[2]);
			// M2 lifecycle_sizes: Wow单位, 统一转换为UE厘米
			const float SizeToUE = WowScalarToUE * ParticleWorldScale;
			FLinearColor M2Color = ParticleLifeRamp<FLinearColor>(
				LifeRatio,
				Emitter.MidPoint,
				LifecycleColors[0],
				LifecycleColors[1],
				LifecycleColors[2]);
			M2Color.A *= FMath::Max(Tuning.AlphaScale, 0.0f);
			const float HalfSize = M2Size * SizeToUE * Particle.SizeScale * FMath::Max(Tuning.SizeScale, 0.0f);
			FVector2D TileUVs[4];
			const int32 TileCount = FMath::Max(Emitter.TextureColumns * Emitter.TextureRows, 1);
			int32 TileIndex = Particle.InitialTileIndex;
			if (TileCount > 1)
			{
				const int32 FrameOffset = FMath::Clamp(FMath::FloorToInt(LifeRatio * static_cast<float>(TileCount)), 0, TileCount - 1);
				TileIndex = (Particle.InitialTileIndex + FrameOffset) % TileCount;
			}
			BuildParticleTileUVs(TileIndex, Emitter.TextureColumns, Emitter.TextureRows, TileUVs);
			ApplyParticleTileRotation(TileUVs, Emitter.TextureTileRotation);

			FVector QuadRight = bBillboard ? CameraRight : NonBillboardRight;
			FVector QuadUp = bBillboard ? CameraUp : NonBillboardUp;
			float HalfWidth = HalfSize;
			float HalfHeight = HalfSize;
			if (Emitter.ParticleType == 1)
			{
				const FVector ParticleOriginWorldPosition = bModelSpaceParticle
					? EmitterRenderWorldPosition + EmitterRenderWorldRotation.RotateVector(Particle.Origin)
					: Particle.Origin;
				System.RenderVertices[BaseVertex + 0] = ParticleWorldPosition - QuadRight * HalfWidth + QuadUp * HalfHeight;
				System.RenderVertices[BaseVertex + 1] = ParticleWorldPosition + QuadRight * HalfWidth + QuadUp * HalfHeight;
				System.RenderVertices[BaseVertex + 2] = ParticleOriginWorldPosition + QuadRight * HalfWidth + QuadUp * HalfHeight;
				System.RenderVertices[BaseVertex + 3] = ParticleOriginWorldPosition - QuadRight * HalfWidth + QuadUp * HalfHeight;
			}
			else
			{
				System.RenderVertices[BaseVertex + 0] = ParticleWorldPosition - QuadRight * HalfWidth - QuadUp * HalfHeight;
				System.RenderVertices[BaseVertex + 1] = ParticleWorldPosition + QuadRight * HalfWidth - QuadUp * HalfHeight;
				System.RenderVertices[BaseVertex + 2] = ParticleWorldPosition + QuadRight * HalfWidth + QuadUp * HalfHeight;
				System.RenderVertices[BaseVertex + 3] = ParticleWorldPosition - QuadRight * HalfWidth + QuadUp * HalfHeight;
			}
			const FVector ParticleNormal = (CameraLocation - ParticleWorldPosition).GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
			for (int32 Corner = 0; Corner < 4; ++Corner)
			{
				System.RenderNormals[BaseVertex + Corner] = ParticleNormal;
				System.RenderUVs[BaseVertex + Corner] = TileUVs[Corner];
				System.RenderVertexColors[BaseVertex + Corner] = M2Color;
			}
		}

		if (!System.bMeshSectionInitialized)
		{
			System.MeshComponent->CreateMeshSection_LinearColor(0, System.RenderVertices, System.RenderTriangles, System.RenderNormals, System.RenderUVs, System.RenderVertexColors, TArray<FProcMeshTangent>(), false);
			System.bMeshSectionInitialized = true;
		}
		else
		{
			System.MeshComponent->UpdateMeshSection_LinearColor(0, System.RenderVertices, System.RenderNormals, System.RenderUVs, System.RenderVertexColors, TArray<FProcMeshTangent>());
		}
	}
}

float AWoWLoginGameMode::GetParticleSystemWorldScale(const FLoginParticleSystem& System) const
{
	if (const USkeletalMeshComponent* SourceMeshComponent = System.SourceMeshComponent.Get())
	{
		const FVector Scale = SourceMeshComponent->GetComponentTransform().GetScale3D();
		return FMath::Max(Scale.GetAbsMax(), KINDA_SMALL_NUMBER);
	}
	return 1.0f;
}

FVector AWoWLoginGameMode::GetParticleSystemBoneWorldPosition(const FLoginParticleSystem& System, int32 BoneIndex, const FVector& M2ModelPosition) const
{
	if (const USkeletalMeshComponent* SourceMeshComponent = System.SourceMeshComponent.Get())
	{
		if (BoneIndex >= 0)
		{
			const FName BoneName(*FString::Printf(TEXT("M2_Bone_%03d"), BoneIndex));
			const int32 UEBoneIndex = SourceMeshComponent->GetBoneIndex(BoneName);
			if (UEBoneIndex != INDEX_NONE)
			{
				const FTransform BoneTransform = SourceMeshComponent->GetBoneTransform(UEBoneIndex);
				const FVector BoneReferencePosition = System.SourceBonePivots.IsValidIndex(BoneIndex) ? System.SourceBonePivots[BoneIndex] : FVector::ZeroVector;
				return BoneTransform.TransformPosition(M2ModelPosition - BoneReferencePosition);
			}
		}
		return SourceMeshComponent->GetComponentTransform().TransformPosition(M2ModelPosition);
	}

	return GetM2BoneWorldPosition(BoneIndex, M2ModelPosition);
}

FQuat AWoWLoginGameMode::GetParticleSystemBoneWorldRotation(const FLoginParticleSystem& System, int32 BoneIndex) const
{
	if (const USkeletalMeshComponent* SourceMeshComponent = System.SourceMeshComponent.Get())
	{
		if (BoneIndex >= 0)
		{
			const FName BoneName(*FString::Printf(TEXT("M2_Bone_%03d"), BoneIndex));
			const int32 UEBoneIndex = SourceMeshComponent->GetBoneIndex(BoneName);
			if (UEBoneIndex != INDEX_NONE)
			{
				return SourceMeshComponent->GetBoneTransform(UEBoneIndex).GetRotation();
			}
		}
		return SourceMeshComponent->GetComponentTransform().GetRotation();
	}

	return GetM2BoneWorldRotation(BoneIndex);
}

void AWoWLoginGameMode::ApplyLoginSceneCameraAnimation(float TimeSeconds)
{
	if (!LoginCameraActor.IsValid())
	{
		return;
	}

	ApplyLoginSceneTuningToCamera(TimeSeconds);
}

void AWoWLoginGameMode::BuildLoginOverlay()
{
	if (LoginOverlayWidget.IsValid())
	{
		return;
	}

	UGameViewportClient* ViewportClient = GetWorld() && GetWorld()->GetGameViewport()
		? GetWorld()->GetGameViewport()
		: nullptr;
	if (!ViewportClient)
	{
		return;
	}

	TSharedRef<SWoWLoginSceneDebugPanel> DebugPanel =
		SNew(SWoWLoginSceneDebugPanel)
		.LoginGameMode(this)
		.OnCloseRequested(FSimpleDelegate::CreateUObject(this, &AWoWLoginGameMode::SetLoginDebugPanelVisible, false));
	DebugPanel->SetVisibility(EVisibility::Collapsed);
	LoginDebugPanelWidget = DebugPanel;
	FString GlueError;
	ReloadLoginGlueUI(&GlueError);

	LoginOverlayWidget = SNew(SOverlay)
		+ SOverlay::Slot()
		.HAlign(HAlign_Fill)
		.VAlign(VAlign_Fill)
		[
			SAssignNew(LoginGlueRootWidget, SWoWGlueRoot)
			.GlueManager(LoginGlueUI)
			.OnRootMouseButtonDown(SWoWGlueRoot::FPointerHandler::CreateUObject(this, &AWoWLoginGameMode::HandleGlueRootMouseButtonDown))
			.OnRootMouseButtonUp(SWoWGlueRoot::FPointerHandler::CreateUObject(this, &AWoWLoginGameMode::HandleGlueRootMouseButtonUp))
			.OnRootMouseMove(SWoWGlueRoot::FPointerHandler::CreateUObject(this, &AWoWLoginGameMode::HandleGlueRootMouseMove))
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		.Padding(FMargin(16.0f))
		[
			DebugPanel
		]
		+ SOverlay::Slot()
		.HAlign(HAlign_Left)
		.VAlign(VAlign_Top)
		.Padding(FMargin(16.0f, 16.0f, 0.0f, 0.0f))
		[
			SNew(SButton)
			.IsFocusable(false)
			.Text_Lambda([this]()
			{
				const bool bVisible = LoginDebugPanelWidget.IsValid() && LoginDebugPanelWidget->GetVisibility().IsVisible();
				return FText::FromString(bVisible ? TEXT("关闭调试") : TEXT("打开调试"));
			})
			.OnClicked(FOnClicked::CreateUObject(this, &AWoWLoginGameMode::HandleShowDebugPanelClicked))
		]
	+ SOverlay::Slot()
	.HAlign(HAlign_Left)
	.VAlign(VAlign_Top)
	.Padding(FMargin(116.0f, 16.0f, 0.0f, 0.0f))
	[
		SNew(SButton)
		.IsFocusable(false)
		.Text(FText::FromString(TEXT("重载Lua")))
		.OnClicked(FOnClicked::CreateUObject(this, &AWoWLoginGameMode::HandleReloadGlueLuaClicked))
	]
	+ SOverlay::Slot()
	.HAlign(HAlign_Center)
	.VAlign(VAlign_Center)
	[
		SAssignNew(LoginGlueErrorWidget, SBox)
		.WidthOverride(850.0f)
		.HeightOverride(325.0f)
		.Visibility_Lambda([this]()
		{
			return LoginGlueLastError.IsEmpty() ? EVisibility::Collapsed : EVisibility::Visible;
		})
		[
			BuildLoginGlueErrorDialog()
		]
	]
	;

	ViewportClient->AddViewportWidgetContent(LoginOverlayWidget.ToSharedRef(), 20);
	SetLoginDebugPanelVisible(false);
}

bool AWoWLoginGameMode::ReloadLoginGlueUI(FString* OutError)
{
	const FString PreservedGlueScreen = LoginGlueLuaRuntime.IsValid()
		? LoginGlueLuaRuntime->GetCurrentGlueScreen()
		: TEXT("login");

	LoginGlueUI = MakeShared<FWoWGlueUIManager>();
	LoginGlueLuaRuntime = MakeShared<FWoWGlueLuaRuntime>();
	LoginGlueLuaRuntime->Initialize(LoginGlueUI.ToSharedRef());
	LoginGlueLuaRuntime->SetGlueLoginHandler([this](const FString& AccountName, const FString& Password)
	{
		HandleGlueLogin(AccountName, Password);
	});
	LoginGlueLuaRuntime->SetGlueScreenHandler([this](const FString& ScreenName)
	{
		HandleGlueScreenChanged(ScreenName);
	});
	LoginGlueLuaRuntime->SetCharacterSelectHandler([this](int32 CharacterGuid)
	{
		HandleGlueCharacterSelected(CharacterGuid);
	});
	LoginGlueLuaRuntime->SetEnterWorldHandler([this]()
	{
		HandleGlueEnterWorld();
	});
	LoginGlueLuaRuntime->SetCharacterCreateHandler([this](const FString& CharacterName, int32 Race, int32 ClassId, int32 Gender, int32 Skin, int32 Face, int32 HairStyle, int32 HairColor, int32 FacialStyle)
	{
		HandleGlueCharacterCreate(CharacterName, Race, ClassId, Gender, Skin, Face, HairStyle, HairColor, FacialStyle);
	});
	LoginGlueLuaRuntime->SetRealmSelectHandler([this](int32 RealmIndex)
	{
		HandleGlueRealmSelected(RealmIndex);
	});
	LoginGlueLuaRuntime->SetRealmCancelHandler([this]()
	{
		HandleGlueRealmCancelled();
	});
	LoginGlueLuaRuntime->SetErrorHandler([this](const FString& ErrorText)
	{
		ShowLoginGlueError(ErrorText);
	});
	LoginGlueLuaRuntime->SetBackgroundModelHandler([this](const FString& ModelPath, int32 Race, int32 Gender)
	{
		HandleGlueBackgroundModelChanged(ModelPath, Race, Gender);
	});
	LoginGlueLuaRuntime->SetDebugLogHandler([this](const FString& Message)
	{
		AddGlueDebugLogLine(Message);
	});

	FString LoadError;
	FString LoadedGlueEntry;
	bool bLoaded = false;

	const FString DataGlueTocPath = FPaths::ProjectDir() / TEXT("Data/Interface/GlueXML/GlueXML.toc");
	const FString ContentGlueTocPath = FPaths::ProjectContentDir() / TEXT("GlueXML/GlueXML.toc");
	if (FPaths::FileExists(DataGlueTocPath) || FPaths::FileExists(ContentGlueTocPath))
	{
		LoadedGlueEntry = FPaths::FileExists(DataGlueTocPath) ? DataGlueTocPath : ContentGlueTocPath;
		bLoaded = LoginGlueLuaRuntime->LoadTOC(LoadedGlueEntry, &LoadError);
	}
	else
	{
		const FString DataLoginLuaPath = FPaths::ProjectDir() / TEXT("Data/Interface/GlueXML/AccountLogin.lua");
		const FString ContentLoginLuaPath = FPaths::ProjectContentDir() / TEXT("GlueXML/AccountLogin.lua");
		LoadedGlueEntry = FPaths::FileExists(DataLoginLuaPath) ? DataLoginLuaPath : ContentLoginLuaPath;
		bLoaded = LoginGlueLuaRuntime->LoadFile(LoadedGlueEntry, &LoadError);
	}

	if (!bLoaded)
	{
		UE_LOG(LogTemp, Error, TEXT("Glue reload failed: %s"), *LoadError);
		if (OutError)
		{
			*OutError = LoadError;
		}
		ShowLoginGlueError(LoadError);
	}
	else
	{
		UE_LOG(LogTemp, Display, TEXT("Glue reloaded: %s"), *LoadedGlueEntry);
		AddGlueDebugLogLine(FString::Printf(TEXT("Glue reloaded: %s"), *LoadedGlueEntry));
		ClearLoginGlueError();
		PushGlueStateToLua();
		CallGlueGlobal(TEXT("SetGlueScreen"), { PreservedGlueScreen });
	}

	if (LoginGlueRootWidget.IsValid())
	{
		LoginGlueRootWidget->SetGlueManager(LoginGlueUI);
	}

	return bLoaded;
}

TSharedRef<SWidget> AWoWLoginGameMode::BuildLoginGlueErrorDialog()
{
	return SNew(SBorder)
		.BorderImage(FCoreStyle::Get().GetBrush("ToolPanel.GroupBorder"))
		.BorderBackgroundColor(FLinearColor(0.04f, 0.035f, 0.03f, 0.92f))
		.Padding(FMargin(16.0f, 14.0f))
		[
			SNew(SVerticalBox)
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(FMargin(0.0f, 0.0f, 0.0f, 8.0f))
			[
				SNew(STextBlock)
				.Text(FText::FromString(TEXT("Glue Lua 错误")))
				.ColorAndOpacity(FLinearColor(1.0f, 0.82f, 0.18f, 1.0f))
			]
			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SNew(SMultiLineEditableTextBox)
				.IsReadOnly(true)
				.AllowMultiLine(true)
				.AutoWrapText(false)
				.AlwaysShowScrollbars(true)
				.SelectAllTextWhenFocused(false)
				.ClearTextSelectionOnFocusLoss(false)
				.AllowContextMenu(true)
				.Padding(FMargin(10.0f))
				.BackgroundColor(FLinearColor(0.015f, 0.012f, 0.01f, 0.76f))
				.ForegroundColor(FLinearColor(1.0f, 0.12f, 0.08f, 1.0f))
				.ReadOnlyForegroundColor(FLinearColor(1.0f, 0.12f, 0.08f, 1.0f))
				.Text_Lambda([this]()
				{
					return FText::FromString(LoginGlueLastError);
				})
			]
			+ SVerticalBox::Slot()
			.AutoHeight()
			.HAlign(HAlign_Right)
			.Padding(FMargin(0.0f, 10.0f, 0.0f, 0.0f))
			[
				SNew(SButton)
				.Text(FText::FromString(TEXT("关闭")))
				.OnClicked_Lambda([this]()
				{
					ClearLoginGlueError();
					return FReply::Handled();
				})
			]
		];
}

void AWoWLoginGameMode::ShowLoginGlueError(const FString& ErrorText)
{
	LoginGlueLastError = FString::Printf(TEXT("Glue Lua 加载/运行失败:\n%s"), *ErrorText);

	if (LoginGlueUI.IsValid())
	{
		const TSharedRef<FWoWGlueFrame> UIParent = LoginGlueUI->GetUIParent();
		TSharedPtr<FWoWGlueFrame> ErrorFrame = LoginGlueUI->FindFrame(TEXT("GlueLuaErrorDialog"));
		if (!ErrorFrame.IsValid())
		{
			ErrorFrame = LoginGlueUI->CreateFrame(TEXT("Frame"), TEXT("GlueLuaErrorDialog"), UIParent);
		}

		ErrorFrame->SetPoint(TEXT("CENTER"), UIParent, TEXT("CENTER"), 0.0f, 0.0f)
			.SetSize(920.0f, 385.0f)
			.SetDrawLayer(220)
			.SetBackdrop(
				LoginGlueUI->LoadTexture(TEXT("Interface\\Tooltips\\UI-Tooltip-Background")),
				TEXT("Interface\\Tooltips\\UI-Tooltip-Background"),
				LoginGlueUI->LoadTexture(TEXT("Interface\\Glues\\Common\\Glue-Tooltip-Border")),
				TEXT("Interface\\Glues\\Common\\Glue-Tooltip-Border"),
				24.0f,
				FMargin(12.0f))
			.Show();

		TSharedPtr<FWoWGlueFrame> ErrorTextBackdrop = LoginGlueUI->FindFrame(TEXT("GlueLuaErrorTextBackdrop"));
		if (!ErrorTextBackdrop.IsValid())
		{
			ErrorTextBackdrop = LoginGlueUI->CreateFrame(TEXT("Frame"), TEXT("GlueLuaErrorTextBackdrop"), UIParent);
		}

		ErrorTextBackdrop->SetPoint(TEXT("CENTER"), UIParent, TEXT("CENTER"), 0.0f, 8.0f)
			.SetSize(820.0f, 230.0f)
			.SetDrawLayer(221)
			.SetBackdrop(
				LoginGlueUI->LoadTexture(TEXT("Interface\\Tooltips\\UI-Tooltip-Background")),
				TEXT("Interface\\Tooltips\\UI-Tooltip-Background"),
				nullptr,
				FString(),
				0.0f,
				FMargin(0.0f))
			.Show();

		if (LoginGlueRootWidget.IsValid())
		{
			LoginGlueRootWidget->Refresh();
		}
	}
}

void AWoWLoginGameMode::ClearLoginGlueError()
{
	LoginGlueLastError.Empty();

	if (LoginGlueUI.IsValid())
	{
		if (TSharedPtr<FWoWGlueFrame> ErrorFrame = LoginGlueUI->FindFrame(TEXT("GlueLuaErrorDialog")))
		{
			ErrorFrame->Hide();
		}
		if (TSharedPtr<FWoWGlueFrame> ErrorTextBackdrop = LoginGlueUI->FindFrame(TEXT("GlueLuaErrorTextBackdrop")))
		{
			ErrorTextBackdrop->Hide();
		}
		if (LoginGlueRootWidget.IsValid())
		{
			LoginGlueRootWidget->Refresh();
		}
	}
}

FReply AWoWLoginGameMode::HandleGlueRootMouseButtonDown(const FGeometry&, const FPointerEvent& MouseEvent)
{
	if (!LoginGlueLuaRuntime.IsValid() ||
		!LoginGlueLuaRuntime->GetCurrentGlueScreen().Equals(TEXT("charselect"), ESearchCase::IgnoreCase) ||
		MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton)
	{
		return FReply::Unhandled();
	}

	bCharacterSelectPreviewDragging = CharacterSelectPreviewActor.IsValid();
	return bCharacterSelectPreviewDragging && LoginGlueRootWidget.IsValid()
		? FReply::Handled().CaptureMouse(LoginGlueRootWidget.ToSharedRef())
		: FReply::Unhandled();
}

FReply AWoWLoginGameMode::HandleGlueRootMouseButtonUp(const FGeometry&, const FPointerEvent& MouseEvent)
{
	if (MouseEvent.GetEffectingButton() != EKeys::LeftMouseButton || !bCharacterSelectPreviewDragging)
	{
		return FReply::Unhandled();
	}

	bCharacterSelectPreviewDragging = false;
	return FReply::Handled().ReleaseMouseCapture();
}

FReply AWoWLoginGameMode::HandleGlueRootMouseMove(const FGeometry&, const FPointerEvent& MouseEvent)
{
	if (!bCharacterSelectPreviewDragging || !MouseEvent.IsMouseButtonDown(EKeys::LeftMouseButton))
	{
		return FReply::Unhandled();
	}

	const float DeltaX = MouseEvent.GetCursorDelta().X;
	if (!FMath::IsNearlyZero(DeltaX))
	{
		RotateCharacterSelectPreview(-DeltaX * 0.35f);
	}
	return FReply::Handled();
}

void AWoWLoginGameMode::RotateCharacterSelectPreview(float YawDeltaDegrees)
{
	if (!CharacterSelectPreviewActor.IsValid())
	{
		return;
	}

	CharacterSelectPreviewUserYawOffset = FMath::UnwindDegrees(CharacterSelectPreviewUserYawOffset + YawDeltaDegrees);
	PositionCharacterSelectPreviewActor();
}

void AWoWLoginGameMode::AddGlueDebugLogLine(const FString& Message)
{
	const FString Timestamp = FDateTime::Now().ToString(TEXT("%H:%M:%S"));
	GlueDebugLogLines.Add(FString::Printf(TEXT("[%s] %s"), *Timestamp, *Message));
	while (GlueDebugLogLines.Num() > MaxGlueDebugLogLines)
	{
		GlueDebugLogLines.RemoveAt(0, 1, EAllowShrinking::No);
	}
}

void AWoWLoginGameMode::ClearGlueDebugLog()
{
	GlueDebugLogLines.Reset();
}

FString AWoWLoginGameMode::GetGlueDebugLogText() const
{
	if (GlueDebugLogLines.IsEmpty())
	{
		return TEXT("暂无 Glue Lua 日志。Lua 中调用 print(...) 后会显示在这里。");
	}

	return FString::Join(GlueDebugLogLines, TEXT("\n"));
}

FString AWoWLoginGameMode::GetGlueCharacterInfoDebugText() const
{
	if (!LoginGlueLuaRuntime.IsValid())
	{
		return TEXT("Glue Lua Runtime 尚未初始化。");
	}

	return LoginGlueLuaRuntime->BuildCharacterInfoDebugDump();
}

void AWoWLoginGameMode::SetLoginDebugPanelVisible(bool bVisible)
{
	if (LoginDebugPanelWidget.IsValid())
	{
		LoginDebugPanelWidget->SetVisibility(bVisible ? EVisibility::Visible : EVisibility::Collapsed);
	}
	if (!bVisible)
	{
		SetLoginTextureDebugHighlightSection(INDEX_NONE);
	}
}

FReply AWoWLoginGameMode::HandleShowDebugPanelClicked()
{
	const bool bCurrentlyVisible = LoginDebugPanelWidget.IsValid() && LoginDebugPanelWidget->GetVisibility().IsVisible();
	SetLoginDebugPanelVisible(!bCurrentlyVisible);
	return FReply::Handled();
}

FReply AWoWLoginGameMode::HandleReloadGlueLuaClicked()
{
	FString Error;
	const bool bLoaded = ReloadLoginGlueUI(&Error);
	UE_LOG(LogTemp, Display, TEXT("Glue Lua manual reload: loaded=%d error=%s"), bLoaded ? 1 : 0, *Error);
	AddGlueDebugLogLine(FString::Printf(TEXT("手动重载Lua: loaded=%d error=%s"), bLoaded ? 1 : 0, *Error));
	return FReply::Handled();
}

FReply AWoWLoginGameMode::HandleEnterTestWorldClicked()
{
	if (UGameViewportClient* ViewportClient = GetWorld() ? GetWorld()->GetGameViewport() : nullptr)
	{
		if (LoginOverlayWidget.IsValid())
		{
			ViewportClient->RemoveViewportWidgetContent(LoginOverlayWidget.ToSharedRef());
			LoginOverlayWidget.Reset();
		}
	}

	UGameplayStatics::OpenLevel(this, FName(TestWorldMapPath), true, TEXT("game=/Script/wow.WoWGameMode"));
	return FReply::Handled();
}

void AWoWLoginGameMode::HandleGlueLogin(const FString& AccountName, const FString& Password)
{
	const double StartSeconds = FPlatformTime::Seconds();
	const uint32 GameThreadId = FPlatformTLS::GetCurrentThreadId();
	UE_LOG(LogTemp, Display, TEXT("Glue 登录请求: account=%s passwordLength=%d"),
		*AccountName,
		Password.Len());

	++GlueLoginRequestSerial;
	const int32 RequestSerial = GlueLoginRequestSerial;
	if (LoginGlueLuaRuntime.IsValid())
	{
		LoginGlueLuaRuntime->SetAccountErrorText(FString());
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(
			this,
			[this, RequestSerial, AccountName, Password]()
			{
				StartGlueLoginRequest(RequestSerial, AccountName, Password);
			}));
	}
	UE_LOG(LogTemp, Display, TEXT("Glue login click returned: serial=%d thread=%u elapsed=%.3f"),
		RequestSerial,
		GameThreadId,
		FPlatformTime::Seconds() - StartSeconds);
}

void AWoWLoginGameMode::StartGlueLoginRequest(int32 RequestSerial, FString AccountName, FString Password)
{
	if (GlueLoginRequestSerial != RequestSerial)
	{
		return;
	}

	const double StartSeconds = FPlatformTime::Seconds();
	if (!GThreadPool || !GlueLoginResultQueue.IsValid())
	{
		FGlueLoginAsyncResult Result;
		Result.RequestSerial = RequestSerial;
		Result.AccountName = AccountName;
		Result.ErrorText = TEXT("后台线程池不可用。");
		GlueLoginResultQueue->Enqueue(MoveTemp(Result));
		return;
	}
	GThreadPool->AddQueuedWork(new FGlueLoginQueuedWork(RequestSerial, MoveTemp(AccountName), MoveTemp(Password), GlueLoginResultQueue));
	UE_LOG(LogTemp, Display, TEXT("Glue login queued: serial=%d thread=%u elapsed=%.3f"),
		RequestSerial,
		FPlatformTLS::GetCurrentThreadId(),
		FPlatformTime::Seconds() - StartSeconds);
}

void AWoWLoginGameMode::FinishGlueLogin(const FGlueLoginAsyncResult& LoginResult)
{
	PendingGlueScreen.Empty();
	PendingGlueScreenDelaySeconds = 0.0f;
	bPendingGlueCloseStatusDialog = false;

	if (!LoginResult.bAuthenticated)
	{
		UE_LOG(LogTemp, Warning, TEXT("Glue 登录失败: %s"), *LoginResult.ErrorText);
		LoggedInAccountId = 0;
		LoggedInAccountName.Empty();
		CachedGlueCharacterRows.Reset();
		CachedGlueRealmRows.Reset();
		if (LoginGlueLuaRuntime.IsValid())
		{
			LoginGlueLuaRuntime->SetAccountErrorText(GetGlueLoginFailureText(LoginResult.ErrorText));
			LoginGlueLuaRuntime->SetCurrentGlueScreen(TEXT("login"));
			PushGlueStateToLua();
		}
		return;
	}

	LoggedInAccountId = LoginResult.AccountRecord.AccountId;
	LoggedInAccountName = LoginResult.AccountRecord.Username;
	LoginScreenSceneIndex = ActiveLoginSceneIndex;
	LastCreatedRace = 1;
	LastCreatedClass = 1;
	LastCreatedGender = 0;
	if (LoginGlueLuaRuntime.IsValid())
	{
		LoginGlueLuaRuntime->SetAccountErrorText(FString());
	}

	SetGlueRealmListFromSummaries(LoginResult.Realms);
	if (LoginGlueLuaRuntime.IsValid())
	{
		ApplyGlueSceneForScreen(TEXT("login"));
		PushGlueStateToLua();
		if (LoginResult.bWorldConnected)
		{
			if (!LoginResult.bCharactersLoaded)
			{
				UE_LOG(LogTemp, Warning, TEXT("Glue 登录成功但角色列表读取失败: %s"), *LoginResult.CharacterErrorText);
				LoginGlueLuaRuntime->SetAccountErrorText(LoginResult.CharacterErrorText);
			}
			SetGlueCharacterListFromSummaries(LoginResult.Characters, true);
			const FString TargetScreen = CachedGlueCharacterRows.Num() > 0 ? TEXT("charselect") : TEXT("charcreate");
			PushGlueStateToLua();
			UE_LOG(LogTemp, Display, TEXT("Glue 登录成功并已自动连接 WorldServer，跳过服务器列表: account=%s characters=%d target=%s"),
				*LoginResult.AccountName,
				CachedGlueCharacterRows.Num(),
				*TargetScreen);
			QueueGlueStatusThenScreen(
				GetGlueString(TEXT("LOGIN_STATE_AUTHENTICATED"), TEXT("连接成功！")),
				GetGlueString(TEXT("OKAY"), TEXT("确定")),
				TargetScreen,
				0.45f);
			return;
		}

		UE_LOG(LogTemp, Display, TEXT("Glue 登录成功但未连接 WorldServer，显示服务器列表: account=%s realms=%d"),
			*LoginResult.AccountName,
			CachedGlueRealmRows.Num());
		QueueGlueStatusThenScreen(
			GetGlueString(TEXT("LOGIN_STATE_AUTHENTICATED"), TEXT("连接成功！")),
			GetGlueString(TEXT("OKAY"), TEXT("确定")),
			TEXT("realmlist"),
			0.45f);
	}
}

void AWoWLoginGameMode::HandleGlueRealmSelected(int32 RealmIndex)
{
	const double StartSeconds = FPlatformTime::Seconds();
	const uint32 GameThreadId = FPlatformTLS::GetCurrentThreadId();
	UE_LOG(LogTemp, Display, TEXT("Glue 选择服务器: realmIndex=%d account=%d"), RealmIndex, LoggedInAccountId);
	++GlueRealmRequestSerial;
	const int32 RequestSerial = GlueRealmRequestSerial;
	if (LoginGlueLuaRuntime.IsValid())
	{
		LoginGlueLuaRuntime->SetAccountErrorText(FString());
		LoginGlueLuaRuntime->SetCurrentGlueScreen(TEXT("realmconnect"));
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(
			this,
			[this, RequestSerial, RealmIndex]()
			{
				StartGlueRealmConnectRequest(RequestSerial, RealmIndex);
			}));
	}
	UE_LOG(LogTemp, Display, TEXT("Glue realm click returned: serial=%d thread=%u elapsed=%.3f"),
		RequestSerial,
		GameThreadId,
		FPlatformTime::Seconds() - StartSeconds);
}

void AWoWLoginGameMode::StartGlueRealmConnectRequest(int32 RequestSerial, int32 RealmIndex)
{
	if (GlueRealmRequestSerial != RequestSerial)
	{
		return;
	}

	const double StartSeconds = FPlatformTime::Seconds();
	if (!GThreadPool || !GlueRealmResultQueue.IsValid())
	{
		FGlueRealmAsyncResult Result;
		Result.RequestSerial = RequestSerial;
		Result.RealmIndex = RealmIndex;
		Result.ErrorText = TEXT("后台线程池不可用。");
		GlueRealmResultQueue->Enqueue(MoveTemp(Result));
		return;
	}
	GThreadPool->AddQueuedWork(new FGlueRealmQueuedWork(RequestSerial, RealmIndex, GlueRealmResultQueue));
	UE_LOG(LogTemp, Display, TEXT("Glue realm queued: serial=%d thread=%u elapsed=%.3f"),
		RequestSerial,
		FPlatformTLS::GetCurrentThreadId(),
		FPlatformTime::Seconds() - StartSeconds);
}

void AWoWLoginGameMode::FinishGlueRealmSelected(const FGlueRealmAsyncResult& RealmResult)
{
	PendingGlueScreen.Empty();
	PendingGlueScreenDelaySeconds = 0.0f;
	bPendingGlueCloseStatusDialog = false;

	if (!RealmResult.bConnected)
	{
		UE_LOG(LogTemp, Warning, TEXT("Glue 服务器连接失败: realmIndex=%d error=%s"), RealmResult.RealmIndex, *RealmResult.ErrorText);
		if (LoginGlueLuaRuntime.IsValid())
		{
			LoginGlueLuaRuntime->SetAccountErrorText(FString());
			PushGlueStateToLua();
			QueueGlueStatusThenScreen(
				GetGlueString(TEXT("CHAR_LOGIN_NO_WORLD"), GetGlueString(TEXT("SERVER_DOWN"), TEXT("服务器无法连接"))),
				GetGlueString(TEXT("OKAY"), TEXT("确定")),
				TEXT("realmlist"),
				0.65f);
		}
		return;
	}

	if (!RealmResult.bCharactersLoaded)
	{
		UE_LOG(LogTemp, Warning, TEXT("Glue 服务器连接成功但角色列表读取失败: realmIndex=%d error=%s"), RealmResult.RealmIndex, *RealmResult.CharacterErrorText);
		if (LoginGlueLuaRuntime.IsValid())
		{
			LoginGlueLuaRuntime->SetAccountErrorText(RealmResult.CharacterErrorText);
		}
	}
	SetGlueCharacterListFromSummaries(RealmResult.Characters, true);
	if (LoginGlueLuaRuntime.IsValid())
	{
		const FString TargetScreen = CachedGlueCharacterRows.Num() > 0 ? TEXT("charselect") : TEXT("charcreate");
		bPendingGlueCloseStatusDialog = true;
		if (TryPrepareGlueScreenScene(TargetScreen) && TryPrepareGlueScreenCharacterPreview(TargetScreen))
		{
			CompletePendingGlueScreenTransition(TargetScreen);
		}
	}
}

void AWoWLoginGameMode::QueueGlueScreenTransition(const FString& TargetScreen, float DelaySeconds)
{
	PendingGlueScreen = TargetScreen;
	PendingGlueScreenDelaySeconds = FMath::Max(0.0f, DelaySeconds);
	bPendingGlueCloseStatusDialog = true;
}

void AWoWLoginGameMode::QueueGlueStatusThenScreen(const FString& StatusText, const FString& ButtonText, const FString& TargetScreen, float DelaySeconds)
{
	CallGlueGlobal(TEXT("UpdateStatusDialog"), { StatusText, ButtonText });
	QueueGlueScreenTransition(TargetScreen, DelaySeconds);
}

void AWoWLoginGameMode::ProcessPendingGlueTransition(float DeltaSeconds)
{
	if (PendingGlueScreen.IsEmpty())
	{
		return;
	}

	PendingGlueScreenDelaySeconds -= DeltaSeconds;
	if (PendingGlueScreenDelaySeconds > 0.0f)
	{
		return;
	}

	const FString TargetScreen = PendingGlueScreen;
	PendingGlueScreen.Empty();
	PendingGlueScreenDelaySeconds = 0.0f;

	if (!TryPrepareGlueScreenScene(TargetScreen))
	{
		return;
	}
	if (!TryPrepareGlueScreenCharacterPreview(TargetScreen))
	{
		return;
	}

	CompletePendingGlueScreenTransition(TargetScreen);
}

void AWoWLoginGameMode::CompletePendingGlueScreenTransition(const FString& TargetScreen)
{
	if (!IsGlueSceneVisualReadyForScreen(TargetScreen))
	{
		bPendingGlueWaitingForScene = true;
		PendingGlueSceneScreen = TargetScreen;
		if (PendingGlueSceneWaitStartSeconds <= 0.0)
		{
			PendingGlueSceneWaitStartSeconds = FPlatformTime::Seconds();
		}
		UE_LOG(LogTemp, Display, TEXT("Glue transition held until scene visual-ready: target=%s activeScene=%d actor=%d"),
			*TargetScreen,
			ActiveLoginSceneIndex,
			LoginSceneActor.IsValid() ? 1 : 0);
		return;
	}

	if (TargetScreen.Equals(TEXT("charselect"), ESearchCase::IgnoreCase))
	{
		if (!IsSelectedCharacterPreviewPreparedReady())
		{
			bPendingGlueWaitingForCharacterPreview = true;
			PendingGlueCharacterPreviewScreen = TargetScreen;
			if (PendingGlueCharacterPreviewWaitStartSeconds <= 0.0)
			{
				PendingGlueCharacterPreviewWaitStartSeconds = FPlatformTime::Seconds();
			}
			UE_LOG(LogTemp, Display, TEXT("Glue charselect transition held until selected character preview prepared-ready: guid=%d preparedKey=%s activeKey=%s"),
				SelectedGlueCharacterGuid,
				*PreparedCharacterPreviewLoadKey,
				*ActiveCharacterPreviewLoadKey);
			return;
		}
		if (!CommitPreparedCharacterVisualKit(PreparedCharacterPreviewLoadKey))
		{
			bPendingGlueWaitingForCharacterPreview = true;
			PendingGlueCharacterPreviewScreen = TargetScreen;
			if (PendingGlueCharacterPreviewWaitStartSeconds <= 0.0)
			{
				PendingGlueCharacterPreviewWaitStartSeconds = FPlatformTime::Seconds();
			}
			UE_LOG(LogTemp, Display, TEXT("Glue charselect transition held until prepared character visual kit can commit: guid=%d preparedKey=%s activeMesh=%s activeAppearance=%s"),
				SelectedGlueCharacterGuid,
				*PreparedCharacterPreviewLoadKey,
				*SelectedGlueCharacterModelKey,
				*SelectedGlueCharacterAppearanceKey);
			return;
		}
	}

	if (bPendingGlueCloseStatusDialog)
	{
		CallGlueGlobal(TEXT("CloseStatusDialog"));
		bPendingGlueCloseStatusDialog = false;
	}

	if (LoginGlueLuaRuntime.IsValid())
	{
		const bool bAlreadyOnTargetScreen = LoginGlueLuaRuntime->GetCurrentGlueScreen().Equals(TargetScreen, ESearchCase::IgnoreCase);
		if (!bAlreadyOnTargetScreen)
		{
			LoginGlueLuaRuntime->SetCurrentGlueScreen(TargetScreen);
			PushGlueStateToLua();
			CallGlueGlobal(TEXT("SetGlueScreen"), { TargetScreen });
		}
		else
		{
			PushGlueStateToLua();
			UE_LOG(LogTemp, Verbose, TEXT("Glue transition target already active, skip SetGlueScreen: target=%s"), *TargetScreen);
		}
	}

	if (bCommandLineGlueAutoExitRequested &&
		CommandLineGlueAutoExitElapsedSeconds < 0.0f &&
		TargetScreen.Equals(TEXT("charselect"), ESearchCase::IgnoreCase))
	{
		CommandLineGlueAutoExitElapsedSeconds = 0.0f;
		UE_LOG(LogTemp, Display, TEXT("Glue command-line auto exit armed after charselect: delay=%.2fs"), CommandLineGlueAutoExitDelaySeconds);
	}
	if (bCommandLineGlueAutoCycleRequested &&
		!bCommandLineGlueAutoCycleActive &&
		!bCommandLineGlueAutoCycleCompleted &&
		TargetScreen.Equals(TEXT("charselect"), ESearchCase::IgnoreCase))
	{
		bCommandLineGlueAutoCycleActive = true;
		CommandLineGlueAutoCycleElapsedSeconds = 0.0f;
		CommandLineGlueAutoCycleNextIndex = 0;
		if (CommandLineGlueAutoCycleRemaining <= 0)
		{
			CommandLineGlueAutoCycleRemaining = CachedGlueCharacterRows.Num();
		}
		UE_LOG(LogTemp, Display, TEXT("Glue command-line auto character cycle armed: rows=%d remaining=%d delay=%.2fs"),
			CachedGlueCharacterRows.Num(),
			CommandLineGlueAutoCycleRemaining,
			CommandLineGlueAutoCycleDelaySeconds);
	}
}

void AWoWLoginGameMode::StartCommandLineGlueAutoLoginIfRequested()
{
	if (bCommandLineGlueAutoLoginStarted)
	{
		return;
	}

	FString LoginValue;
	if (!FParse::Value(FCommandLine::Get(), TEXT("WoWGlueAutoLogin="), LoginValue) || LoginValue.IsEmpty())
	{
		return;
	}

	FString AccountName;
	FString Password;
	if (!LoginValue.Split(TEXT(":"), &AccountName, &Password, ESearchCase::CaseSensitive, ESearchDir::FromStart))
	{
		UE_LOG(LogTemp, Error, TEXT("Glue command-line auto login expects -WoWGlueAutoLogin=account:password"));
		return;
	}

	AccountName = AccountName.TrimStartAndEnd();
	Password = Password.TrimStartAndEnd();
	if (AccountName.IsEmpty() || Password.IsEmpty())
	{
		UE_LOG(LogTemp, Error, TEXT("Glue command-line auto login has empty account or password"));
		return;
	}

	bCommandLineGlueAutoLoginStarted = true;
	bCommandLineGlueAutoCycleRequested = FParse::Param(FCommandLine::Get(), TEXT("WoWGlueAutoCycleCharacters"));
	FParse::Value(FCommandLine::Get(), TEXT("WoWGlueAutoCycleCount="), CommandLineGlueAutoCycleRemaining);
	const FString AutoCycleDelayMarker = TEXT("WoWGlueAutoCycleDelay=");
	const FString FullCommandLine = FCommandLine::Get();
	const int32 AutoCycleDelayIndex = FullCommandLine.Find(AutoCycleDelayMarker, ESearchCase::IgnoreCase);
	if (AutoCycleDelayIndex != INDEX_NONE)
	{
		FString AutoCycleDelayText = FullCommandLine.Mid(AutoCycleDelayIndex + AutoCycleDelayMarker.Len()).TrimStartAndEnd();
		int32 TokenEndIndex = INDEX_NONE;
		if (AutoCycleDelayText.FindChar(TEXT(' '), TokenEndIndex))
		{
			AutoCycleDelayText.LeftInline(TokenEndIndex, EAllowShrinking::No);
		}
		AutoCycleDelayText.TrimQuotesInline();
		if (!AutoCycleDelayText.IsEmpty())
		{
			CommandLineGlueAutoCycleDelaySeconds = FCString::Atof(*AutoCycleDelayText);
		}
	}
	CommandLineGlueAutoCycleDelaySeconds = FMath::Max(CommandLineGlueAutoCycleDelaySeconds, 0.05f);
	bCommandLineGlueAutoExitRequested = FParse::Param(FCommandLine::Get(), TEXT("WoWGlueAutoExitOnCharselect"));
	FParse::Value(FCommandLine::Get(), TEXT("WoWGlueAutoExitDelay="), CommandLineGlueAutoExitDelaySeconds);
	CommandLineGlueAutoExitDelaySeconds = FMath::Max(CommandLineGlueAutoExitDelaySeconds, 0.0f);
	CommandLineGlueAutoExitElapsedSeconds = -1.0f;

	UE_LOG(LogTemp, Display, TEXT("Glue command-line auto login started: account=%s autoCycle=%d cycleCount=%d cycleDelay=%.2fs autoExit=%d exitDelay=%.2fs"),
		*AccountName,
		bCommandLineGlueAutoCycleRequested ? 1 : 0,
		CommandLineGlueAutoCycleRemaining,
		CommandLineGlueAutoCycleDelaySeconds,
		bCommandLineGlueAutoExitRequested ? 1 : 0,
		CommandLineGlueAutoExitDelaySeconds);
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimerForNextTick(FTimerDelegate::CreateWeakLambda(
			this,
			[this, AccountName, Password]()
			{
				HandleGlueLogin(AccountName, Password);
			}));
	}
}

void AWoWLoginGameMode::ProcessCommandLineGlueAutoCycle(float DeltaSeconds)
{
	if (!bCommandLineGlueAutoCycleRequested ||
		!bCommandLineGlueAutoCycleActive ||
		!LoginGlueLuaRuntime.IsValid() ||
		!LoginGlueLuaRuntime->GetCurrentGlueScreen().Equals(TEXT("charselect"), ESearchCase::IgnoreCase))
	{
		return;
	}
	if (CachedGlueCharacterRows.IsEmpty())
	{
		bCommandLineGlueAutoCycleActive = false;
		bCommandLineGlueAutoCycleCompleted = true;
		return;
	}
	if (CommandLineGlueAutoCycleRemaining == 0)
	{
		bCommandLineGlueAutoCycleActive = false;
		bCommandLineGlueAutoCycleCompleted = true;
		UE_LOG(LogTemp, Display, TEXT("Glue command-line auto character cycle completed"));
		return;
	}

	CommandLineGlueAutoCycleElapsedSeconds += DeltaSeconds;
	if (CommandLineGlueAutoCycleElapsedSeconds < CommandLineGlueAutoCycleDelaySeconds)
	{
		return;
	}
	CommandLineGlueAutoCycleElapsedSeconds = 0.0f;

	for (int32 Attempts = 0; Attempts < CachedGlueCharacterRows.Num(); ++Attempts)
	{
		const int32 RowIndex = CommandLineGlueAutoCycleNextIndex % CachedGlueCharacterRows.Num();
		++CommandLineGlueAutoCycleNextIndex;

		TArray<FString> Columns;
		CachedGlueCharacterRows[RowIndex].ParseIntoArray(Columns, TEXT("\t"), false);
		if (Columns.Num() < 1)
		{
			continue;
		}

		const int32 CharacterGuid = FCString::Atoi(*Columns[0]);
		if (CharacterGuid <= 0)
		{
			continue;
		}

		if (CommandLineGlueAutoCycleRemaining > 0)
		{
			--CommandLineGlueAutoCycleRemaining;
		}
		UE_LOG(LogTemp, Display, TEXT("Glue command-line auto character cycle selects: index=%d guid=%d remaining=%d"),
			RowIndex,
			CharacterGuid,
			CommandLineGlueAutoCycleRemaining);
		HandleGlueCharacterSelected(CharacterGuid);
		return;
	}

	bCommandLineGlueAutoCycleActive = false;
	bCommandLineGlueAutoCycleCompleted = true;
}

void AWoWLoginGameMode::ProcessCommandLineGlueAutoExit(float DeltaSeconds)
{
	if (!bCommandLineGlueAutoExitRequested ||
		!LoginGlueLuaRuntime.IsValid() ||
		!LoginGlueLuaRuntime->GetCurrentGlueScreen().Equals(TEXT("charselect"), ESearchCase::IgnoreCase))
	{
		return;
	}

	if (CommandLineGlueAutoExitElapsedSeconds < 0.0f)
	{
		return;
	}

	CommandLineGlueAutoExitElapsedSeconds += DeltaSeconds;
	if (CommandLineGlueAutoExitElapsedSeconds < CommandLineGlueAutoExitDelaySeconds)
	{
		return;
	}

	bCommandLineGlueAutoExitRequested = false;
	UE_LOG(LogTemp, Display, TEXT("Glue command-line auto exit requested after charselect: elapsed=%.2fs"), CommandLineGlueAutoExitElapsedSeconds);
	FGenericPlatformMisc::RequestExit(false);
}

void AWoWLoginGameMode::CallGlueGlobal(const FString& FunctionName, const TArray<FString>& Args)
{
	if (!LoginGlueLuaRuntime.IsValid())
	{
		return;
	}

	FString Error;
	if (!LoginGlueLuaRuntime->CallGlobalFunction(FunctionName, Args, &Error))
	{
		UE_LOG(LogTemp, Warning, TEXT("Glue Lua 调用失败: %s: %s"), *FunctionName, *Error);
	}
}

FString AWoWLoginGameMode::GetGlueString(const FString& Name, const FString& Fallback) const
{
	if (LoginGlueLuaRuntime.IsValid())
	{
		FString Value;
		if (LoginGlueLuaRuntime->GetGlobalString(Name, Value))
		{
			return Value;
		}
	}
	return Fallback;
}

FString AWoWLoginGameMode::GetGlueLoginFailureText(const FString& ErrorText) const
{
	if (ErrorText.Contains(TEXT("请输入账号和密码")) || ErrorText.Contains(TEXT("请输入账号")))
	{
		return GetGlueString(TEXT("LOGIN_ENTER_NAME"), ErrorText);
	}
	if (ErrorText.Contains(TEXT("请输入密码")))
	{
		return GetGlueString(TEXT("LOGIN_ENTER_PASSWORD"), ErrorText);
	}
	if (ErrorText.Contains(TEXT("账号")) || ErrorText.Contains(TEXT("ACCOUNT")))
	{
		return GetGlueString(TEXT("LOGIN_UNKNOWN_ACCOUNT"), ErrorText);
	}
	if (ErrorText.Contains(TEXT("密码")) || ErrorText.Contains(TEXT("PASSWORD")))
	{
		return GetGlueString(TEXT("LOGIN_INCORRECT_PASSWORD"), ErrorText);
	}
	return GetGlueString(TEXT("LOGIN_FAILED"), ErrorText);
}

void AWoWLoginGameMode::HandleGlueRealmCancelled()
{
	ResetGlueLoginSessionState();
	if (LoginGlueLuaRuntime.IsValid())
	{
		LoginGlueLuaRuntime->SetAccountErrorText(FString());
		LoginGlueLuaRuntime->SetCurrentGlueScreen(TEXT("login"));
		PushGlueStateToLua();
	}
}

void AWoWLoginGameMode::ResetGlueLoginSessionState()
{
	++GlueRealmRequestSerial;
	++GlueLoginRequestSerial;

	FWoWAzerothCoreService::Disconnect();

	PendingGlueScreen.Empty();
	PendingGlueScreenDelaySeconds = 0.0f;
	bPendingGlueCloseStatusDialog = false;

	bPendingGlueWaitingForScene = false;
	PendingGlueSceneScreen.Empty();
	PendingGlueSceneWaitStartSeconds = 0.0;

	bPendingGlueWaitingForCharacterPreview = false;
	PendingGlueCharacterPreviewScreen.Empty();
	PendingGlueCharacterPreviewWaitStartSeconds = 0.0;

	ActiveCharacterPreviewLoadKey.Empty();
	PreparedCharacterPreviewLoadKey.Empty();
	if (ActiveCharacterPreviewStreamableHandle.IsValid())
	{
		ActiveCharacterPreviewStreamableHandle->CancelHandle();
		ActiveCharacterPreviewStreamableHandle.Reset();
	}
	if (ActiveLoginSceneStreamableHandle.IsValid())
	{
		ActiveLoginSceneStreamableHandle->CancelHandle();
		ActiveLoginSceneStreamableHandle.Reset();
	}

	LoggedInAccountId = 0;
	LoggedInAccountName.Empty();
	SelectedGlueCharacterGuid = 0;
	SelectedGlueCharacterModelKey.Empty();
	SelectedGlueCharacterAnimationKey.Empty();
	SelectedGlueCharacterAppearanceKey.Empty();
	CachedGlueRealmRows.Reset();
	CachedGlueCharacterRows.Reset();

	DestroyCharacterSelectPreviewActor();
}

void AWoWLoginGameMode::HandleGlueScreenChanged(const FString& ScreenName)
{
	if (LoginGlueLuaRuntime.IsValid())
	{
		LoginGlueLuaRuntime->SetCurrentGlueScreen(ScreenName);
		PushGlueStateToLua();
	}
	ApplyGlueSceneForScreen(ScreenName);
	if (ScreenName.Equals(TEXT("charselect"), ESearchCase::IgnoreCase))
	{
		QueueCharacterPreviewPrewarm();
	}
}

void AWoWLoginGameMode::HandleGlueBackgroundModelChanged(const FString& ModelPath, int32 Race, int32 Gender)
{
	if (ModelPath.IsEmpty())
	{
		return;
	}

	const int32 SceneIndex = FindLoginSceneIndexByModelPath(ModelPath);
	if (LoginSceneDefinitions.IsValidIndex(SceneIndex))
	{
		if (SceneIndex == ActiveLoginSceneIndex)
		{
			UE_LOG(LogTemp, Verbose, TEXT("Glue背景模型已是当前场景，忽略重复切换: path=%s race=%d gender=%d scene=%s"),
				*ModelPath,
				Race,
				Gender,
				*LoginSceneDefinitions[SceneIndex].DisplayName);
			return;
		}

		UE_LOG(LogTemp, Display, TEXT("Glue背景模型切换: path=%s race=%d gender=%d scene=%s"),
			*ModelPath,
			Race,
			Gender,
			*LoginSceneDefinitions[SceneIndex].DisplayName);
		RequestActiveLoginSceneIndex(SceneIndex);
		return;
	}

	UE_LOG(LogTemp, Warning, TEXT("Glue背景模型未找到生成资产: path=%s race=%d gender=%d"), *ModelPath, Race, Gender);
}

int32 AWoWLoginGameMode::FindLoginSceneIndexByKeyFragment(const FString& KeyFragment) const
{
	for (int32 Index = 0; Index < LoginSceneDefinitions.Num(); ++Index)
	{
		const FLoginSceneDefinition& Definition = LoginSceneDefinitions[Index];
		if (Definition.Key.Contains(KeyFragment, ESearchCase::IgnoreCase) ||
			Definition.DisplayName.Contains(KeyFragment, ESearchCase::IgnoreCase) ||
			Definition.JsonRelativePath.Contains(KeyFragment, ESearchCase::IgnoreCase))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

int32 AWoWLoginGameMode::FindLoginSceneIndexByModelPath(const FString& ModelPath) const
{
	FString Normalized = ModelPath;
	Normalized.ReplaceInline(TEXT("\\"), TEXT("/"));
	Normalized.RemoveFromEnd(TEXT(".m2"), ESearchCase::IgnoreCase);
	Normalized.RemoveFromStart(TEXT("M2Preview/"), ESearchCase::IgnoreCase);
	Normalized.RemoveFromStart(TEXT("Saved/M2Preview/"), ESearchCase::IgnoreCase);

	for (int32 Index = 0; Index < LoginSceneDefinitions.Num(); ++Index)
	{
		FString Key = LoginSceneDefinitions[Index].Key;
		Key.ReplaceInline(TEXT("\\"), TEXT("/"));
		Key.RemoveFromEnd(TEXT(".mesh.json"), ESearchCase::IgnoreCase);
		Key.RemoveFromEnd(TEXT(".mesh"), ESearchCase::IgnoreCase);
		if (Key.Equals(Normalized, ESearchCase::IgnoreCase) ||
			Key.EndsWith(Normalized, ESearchCase::IgnoreCase) ||
			Normalized.EndsWith(Key, ESearchCase::IgnoreCase))
		{
			return Index;
		}
	}

	return INDEX_NONE;
}

void AWoWLoginGameMode::LoadPlayerCharacterPreviewDefinitions()
{
	PlayerCharacterPreviewDefinitions.Reset();

	const FString ManifestPath = FPaths::ProjectConfigDir() / TEXT("WoWAssetManifests/PlayerCharacters.json");
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *ManifestPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览 manifest 不存在: %s"), *ManifestPath);
		return;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览 manifest 解析失败: %s"), *ManifestPath);
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("entries"), Entries) || !Entries)
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览 manifest 缺少 entries: %s"), *ManifestPath);
		return;
	}

	for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
	{
		const TSharedPtr<FJsonObject> EntryObject = TryReadJsonObject(EntryValue);
		if (!EntryObject.IsValid())
		{
			continue;
		}

		double RaceNumber = 0.0;
		double GenderNumber = 0.0;
		FString PackageRoot;
		if (!EntryObject->TryGetNumberField(TEXT("race_id"), RaceNumber) ||
			!EntryObject->TryGetNumberField(TEXT("gender"), GenderNumber) ||
			!EntryObject->TryGetStringField(TEXT("generated_package_root"), PackageRoot) ||
			PackageRoot.IsEmpty())
		{
			continue;
		}

		FString Id;
		EntryObject->TryGetStringField(TEXT("id"), Id);
		FString DisplayName;
		EntryObject->TryGetStringField(TEXT("display_name"), DisplayName);
		FString ModelPath;
		EntryObject->TryGetStringField(TEXT("model_path"), ModelPath);
		FString GeneratedAssetStem;
		EntryObject->TryGetStringField(TEXT("generated_asset_stem"), GeneratedAssetStem);
		const FString ModelStem = SanitizePackageSegment(FPaths::GetBaseFilename(ModelPath));
		const FString AssetStem = SanitizePackageSegment(!GeneratedAssetStem.IsEmpty() ? GeneratedAssetStem : (ModelStem.IsEmpty() ? Id : ModelStem));
		const FString AnimationStem = ModelStem.IsEmpty() ? AssetStem : ModelStem;
		if (AssetStem.IsEmpty())
		{
			continue;
		}

		FString AnimationPackagePathStr;
		EntryObject->TryGetStringField(TEXT("animation_package_path"), AnimationPackagePathStr);
		FString MaterialPackagePathStr;
		EntryObject->TryGetStringField(TEXT("material_package_path"), MaterialPackagePathStr);

		FPlayerCharacterPreviewDefinition Definition;
		Definition.Race = FMath::RoundToInt(RaceNumber);
		Definition.Gender = FMath::RoundToInt(GenderNumber);
		Definition.Id = Id;
		Definition.DisplayName = DisplayName;
		Definition.SourceModelPath = ModelPath;
		Definition.GeneratedAssetStem = AssetStem;
		Definition.MeshJsonPath = BuildPreviewMetadataPath(ModelPath, AssetStem, TEXT(".mesh.json"));
		PackageRoot.RemoveFromEnd(TEXT("/"));
		Definition.MeshPath = MakeObjectPathFromPackagePath(PackageRoot / (AssetStem + TEXT("_M2Mesh")));
		Definition.AnimationPath = PackageRoot / (AssetStem + TEXT("_M2Skeleton"));
		Definition.AnimationPackagePath = AnimationPackagePathStr.IsEmpty()
			? MakeObjectPathFromPackagePath(PackageRoot / TEXT("Animations"))
			: AnimationPackagePathStr;
		Definition.MaterialPackagePath = MaterialPackagePathStr.IsEmpty()
			? MakeObjectPathFromPackagePath(PackageRoot / (AssetStem + TEXT("_Materials")))
			: MaterialPackagePathStr;
		Definition.StandAnimationPath = MakeObjectPathFromPackagePath(PackageRoot / TEXT("Animations") / (TEXT("A_") + AnimationStem + TEXT("_000_Stand_00")));
		Definition.LookupAnimationPath = MakeObjectPathFromPackagePath(PackageRoot / TEXT("Animations_Lookup") / (TEXT("A_") + AnimationStem + TEXT("_000_Stand_00")));
		Definition.SectionMetadataPath = BuildPreviewMetadataPath(ModelPath, AssetStem, TEXT(".sections.json"));

		const int64 Key = (static_cast<int64>(Definition.Race) << 32) | static_cast<uint32>(Definition.Gender);
		PlayerCharacterPreviewDefinitions.Add(Key, MoveTemp(Definition));
	}

	UE_LOG(LogTemp, Display, TEXT("角色预览 manifest 已加载: entries=%d path=%s"), PlayerCharacterPreviewDefinitions.Num(), *ManifestPath);
}

const AWoWLoginGameMode::FPlayerCharacterPreviewDefinition* AWoWLoginGameMode::FindPlayerCharacterPreviewDefinition(int32 Race, int32 Gender) const
{
	const int64 Key = (static_cast<int64>(Race) << 32) | static_cast<uint32>(Gender);
	return PlayerCharacterPreviewDefinitions.Find(Key);
}

void AWoWLoginGameMode::QueueCharacterPreviewPrewarm()
{
	PendingCharacterPreviewPrewarmRows.Reset();
	PrewarmedCharacterPreviewGuids.Reset();
	ActiveCharacterPreviewPrewarmKey.Empty();
	ActiveCharacterPreviewPrewarmStreamableHandle.Reset();
	++CharacterPreviewPrewarmRequestSerial;

	if (CachedGlueCharacterRows.IsEmpty())
	{
		return;
	}

	auto TryQueueRow = [this](const FString& Row)
	{
		TArray<FString> Columns;
		Row.ParseIntoArray(Columns, TEXT("\t"), false);
		if (Columns.Num() < 1)
		{
			return;
		}
		const int32 Guid = FCString::Atoi(*Columns[0]);
		if (Guid <= 0 || PrewarmedCharacterPreviewGuids.Contains(Guid))
		{
			return;
		}
		PendingCharacterPreviewPrewarmRows.Add(Row);
		PrewarmedCharacterPreviewGuids.Add(Guid);
	};

	if (SelectedGlueCharacterGuid > 0)
	{
		for (const FString& Row : CachedGlueCharacterRows)
		{
			TArray<FString> Columns;
			Row.ParseIntoArray(Columns, TEXT("\t"), false);
			if (Columns.Num() >= 1 && FCString::Atoi(*Columns[0]) == SelectedGlueCharacterGuid)
			{
				TryQueueRow(Row);
				break;
			}
		}
	}

	for (const FString& Row : CachedGlueCharacterRows)
	{
		TryQueueRow(Row);
	}
}

void AWoWLoginGameMode::ProcessCharacterPreviewPrewarmQueue()
{
	if (PendingSelectedCharacterPreviewRefreshGuid > 0)
	{
		return;
	}
	if (!PendingCharacterSelectPreviewM2Attachments.IsEmpty())
	{
		return;
	}
	if (!PendingCharacterPreviewRuntimeMaterialKey.IsEmpty())
	{
		return;
	}
	if (ActiveCharacterPreviewPrewarmStreamableHandle.IsValid() &&
		ActiveCharacterPreviewPrewarmStreamableHandle->IsLoadingInProgress())
	{
		return;
	}
	if (PendingCharacterPreviewPrewarmRows.IsEmpty())
	{
		return;
	}

	const FString Row = PendingCharacterPreviewPrewarmRows[0];
	PendingCharacterPreviewPrewarmRows.RemoveAt(0, 1, EAllowShrinking::No);
	PrewarmCharacterPreviewRow(Row);
}

void AWoWLoginGameMode::PrewarmCharacterPreviewRow(const FString& Row)
{
	TArray<FString> Columns;
	Row.ParseIntoArray(Columns, TEXT("\t"), false);
	if (Columns.Num() < 17)
	{
		return;
	}

	const int32 Guid = FCString::Atoi(*Columns[0]);
	const int32 Race = FCString::Atoi(*Columns[2]);
	const int32 Gender = FCString::Atoi(*Columns[4]);
	const int32 Skin = FCString::Atoi(*Columns[6]);
	const int32 Face = FCString::Atoi(*Columns[7]);
	const int32 HairStyle = FCString::Atoi(*Columns[8]);
	const int32 HairColor = FCString::Atoi(*Columns[9]);
	const int32 FacialStyle = FCString::Atoi(*Columns[10]);
	const FString EquipmentSummary = GetColumnOrEmpty(Columns, 16);
	const FPlayerCharacterPreviewDefinition* Definition = FindPlayerCharacterPreviewDefinition(Race, Gender);
	if (!Definition)
	{
		return;
	}

	const FString AppearanceSignature = BuildCharacterAppearanceSignature(Race, Gender, Skin, Face, HairStyle, HairColor, FacialStyle, EquipmentSummary);
	const FString CharacterPreviewStem = BuildGeneratedCharacterPreviewStemForSignature(Definition->SourceModelPath, Definition->GeneratedAssetStem, AppearanceSignature);
	const FString CharacterPreviewAnimationStem = BuildGeneratedCharacterPreviewAnimationStem(Definition->SourceModelPath);
	const FString CharacterPreviewPackageRoot = FString::Printf(TEXT("/Game/WoW/Generated/Cache/CharacterPreview/Appearance/%s"), *AppearanceSignature);
	const FString LegacyCharacterPreviewPackageRoot = FString::Printf(TEXT("/Game/WoW/Generated/CharacterPreview/Appearance/%s"), *AppearanceSignature);
	const FString AppearanceMetadataRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("M2Preview/CharacterPreview/Appearance") / AppearanceSignature);

	FString MeshPath = Definition->MeshPath;

	FCharacterSelectPreviewAppearance Appearance;
	const FPlayerCharacterPreviewDefinition* ResolvedDefinition = nullptr;
	FString ResolvedAppearanceSignature;
	FString ResolvedCharacterPreviewStem;
	FString ResolvedCharacterPreviewPackageRoot;
	FString ResolvedCharacterPreviewLookupAnimationPath;
	if (ResolveCharacterPreviewRuntimeAssets(
		Columns,
		Appearance,
		ResolvedDefinition,
		ResolvedAppearanceSignature,
		ResolvedCharacterPreviewStem,
		ResolvedCharacterPreviewPackageRoot,
		ResolvedCharacterPreviewLookupAnimationPath))
	{
		if (!EnsureCharacterPreviewCacheAssetReady(
			Appearance,
			ResolvedDefinition,
			ResolvedAppearanceSignature,
			ResolvedCharacterPreviewStem,
			ResolvedCharacterPreviewPackageRoot,
			TEXT("Character enum warmup builds complete equipped preview cache before row selection")))
		{
			UE_LOG(LogTemp, Display, TEXT("角色预览枚举阶段已排队完整装备缓存，运行态基础外观继续可用: guid=%d signature=%s equipmentEntries=%d baseRuntime=%d"),
				Guid,
				*ResolvedAppearanceSignature,
				Appearance.EquipmentEntryCount,
				Appearance.bUsesBaseRuntimeComposition ? 1 : 0);
			if (!Appearance.bUsesBaseRuntimeComposition)
			{
				return;
			}
		}

		MeshPath = Appearance.CharacterPreviewMeshPath;
		TArray<FSoftObjectPath> AssetsToLoad;
		CollectCharacterPreviewAsyncAssets(Appearance, AssetsToLoad);
		const FString RequestedAnimationPath = ResolveCharacterPreviewAnimationObjectPath(Appearance.CharacterPreviewAnimationPath, Appearance.EquipmentSummary);
		const FString RequestKey = WoWCharacterVisualKit::BuildPreviewRequestKey(
			Appearance.Guid,
			Appearance.CacheKey,
			Appearance.CharacterPreviewMeshPath,
			RequestedAnimationPath,
			Appearance.CharacterPreviewSectionMetadataPath,
			Appearance.CharacterPreviewAttachmentDescriptorPath);
		if (!AssetsToLoad.IsEmpty())
		{
			const int32 PrewarmSerial = ++CharacterPreviewPrewarmRequestSerial;
			ActiveCharacterPreviewPrewarmKey = RequestKey;
			const double PrewarmStartSeconds = FPlatformTime::Seconds();
			FStreamableManager& StreamableManager = UAssetManager::GetStreamableManager();
			ActiveCharacterPreviewPrewarmStreamableHandle = StreamableManager.RequestAsyncLoad(
				AssetsToLoad,
				FStreamableDelegate::CreateWeakLambda(this, [this, PrewarmSerial, RequestKey, Appearance, PrewarmStartSeconds]()
				{
					if (PrewarmSerial != CharacterPreviewPrewarmRequestSerial ||
						!ActiveCharacterPreviewPrewarmKey.Equals(RequestKey, ESearchCase::CaseSensitive))
					{
						return;
					}

					PrewarmCharacterVisualKit(Appearance);
					if (ActiveCharacterPreviewPrewarmStreamableHandle.IsValid())
					{
						RetainedCharacterPreviewPrewarmHandles.Add(ActiveCharacterPreviewPrewarmStreamableHandle);
						constexpr int32 MaxRetainedPrewarmHandles = 12;
						while (RetainedCharacterPreviewPrewarmHandles.Num() > MaxRetainedPrewarmHandles)
						{
							RetainedCharacterPreviewPrewarmHandles.RemoveAt(0, 1, EAllowShrinking::No);
						}
					}
					UE_LOG(LogTemp, Display, TEXT("角色预览后台 VisualKit 预热完成: guid=%d assetsRetained=%d elapsed=%.2fms key=%s"),
						Appearance.Guid,
						RetainedCharacterPreviewPrewarmHandles.Num(),
						(FPlatformTime::Seconds() - PrewarmStartSeconds) * 1000.0,
						*RequestKey);
					ActiveCharacterPreviewPrewarmKey.Empty();
					ActiveCharacterPreviewPrewarmStreamableHandle.Reset();
				}));
			UE_LOG(LogTemp, Display, TEXT("角色预览后台 VisualKit 预热请求: guid=%d assets=%d baseRuntime=%d key=%s"),
				Appearance.Guid,
				AssetsToLoad.Num(),
				Appearance.bUsesBaseRuntimeComposition ? 1 : 0,
				*RequestKey);
		}
		else
		{
			PrewarmCharacterVisualKit(Appearance);
			UE_LOG(LogTemp, Display, TEXT("角色预览后台 VisualKit 无 streamable 资产，仅预热运行态描述: guid=%d baseRuntime=%d key=%s"),
				Appearance.Guid,
				Appearance.bUsesBaseRuntimeComposition ? 1 : 0,
				*RequestKey);
		}

		TArray<FCharacterPreviewAttachmentDefinition> AttachmentDefinitions;
		if (FPaths::FileExists(Appearance.CharacterPreviewAttachmentDescriptorPath))
		{
			ReadCharacterPreviewAttachmentDefinitions(Appearance.CharacterPreviewAttachmentDescriptorPath, AttachmentDefinitions);
		}
		if (AttachmentDefinitions.IsEmpty())
		{
			BuildSharedItemObjectAttachmentDefinitions(Appearance, AttachmentDefinitions);
		}
		for (const FCharacterPreviewAttachmentDefinition& AttachmentDefinition : AttachmentDefinitions)
		{
			const FString AttachmentMeshJsonPath = !AttachmentDefinition.MeshJsonPath.IsEmpty()
				? AttachmentDefinition.MeshJsonPath
				: BuildCharacterPreviewAttachmentMeshJsonPath(AttachmentDefinition.MeshPath, Appearance.CharacterPreviewAttachmentDescriptorPath);
			if (!AttachmentMeshJsonPath.IsEmpty() &&
				DoesEquipmentSummaryContainAttachment(Appearance.EquipmentSummary, AttachmentDefinition.Slot, AttachmentDefinition.DisplayId) &&
				ShouldUseCharacterPreviewM2RuntimeAttachment(AttachmentDefinition, AttachmentMeshJsonPath))
			{
				UWoWM2ModelComponent::PrewarmMeshJson(AttachmentMeshJsonPath, false);
			}
		}
	}

	UE_LOG(LogTemp, Verbose, TEXT("角色预览轻量预热完成: guid=%d race=%d gender=%d equipmentEntries=%d mesh=%s"),
		Guid,
		Race,
		Gender,
		CountEquipmentDisplayEntries(EquipmentSummary),
		*MeshPath);
}

void AWoWLoginGameMode::RequestSelectedCharacterPreviewRefresh()
{
	if (SelectedGlueCharacterGuid <= 0)
	{
		return;
	}
	CancelPreparedCharacterSelectPreviewStaging(TEXT("new_selection_request"));
	PendingSelectedCharacterPreviewRefreshGuid = SelectedGlueCharacterGuid;
	PendingCharacterSelectPreviewM2Attachments.Reset();
	PendingCharacterSelectPreviewM2AttachmentKey.Empty();
	PendingCharacterSelectPreviewM2AttachmentDescriptorPath.Empty();
	PendingCharacterSelectPreviewM2AttachmentDefinitionCount = 0;
	PendingCharacterSelectPreviewM2AttachmentAttachedCount = 0;
	PendingCharacterSelectPreviewM2AttachmentStartSeconds = 0.0;
	PendingCharacterSelectPreviewRevealGuid = 0;
	PendingCharacterSelectPreviewRevealAppearanceKey.Empty();

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(SelectedCharacterPreviewRefreshTimerHandle);
	}

	FString SelectedRow;
	for (const FString& Row : CachedGlueCharacterRows)
	{
		TArray<FString> Columns;
		Row.ParseIntoArray(Columns, TEXT("\t"), false);
		if (Columns.Num() >= 5 && FCString::Atoi(*Columns[0]) == SelectedGlueCharacterGuid)
		{
			SelectedRow = Row;
			break;
		}
	}
	if (SelectedRow.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览异步请求找不到选中角色: guid=%d rows=%d"), SelectedGlueCharacterGuid, CachedGlueCharacterRows.Num());
		PendingSelectedCharacterPreviewRefreshGuid = 0;
		return;
	}

	TArray<FString> Columns;
	SelectedRow.ParseIntoArray(Columns, TEXT("\t"), false);
	FCharacterSelectPreviewAppearance Appearance;
	const FPlayerCharacterPreviewDefinition* Definition = nullptr;
	FString AppearanceSignature;
	FString CharacterPreviewStem;
	FString CharacterPreviewPackageRoot;
	FString CharacterPreviewLookupAnimationPath;
	if (!ResolveCharacterPreviewRuntimeAssets(Columns, Appearance, Definition, AppearanceSignature, CharacterPreviewStem, CharacterPreviewPackageRoot, CharacterPreviewLookupAnimationPath))
	{
		PendingSelectedCharacterPreviewRefreshGuid = 0;
		return;
	}

	StoreSelectedCharacterRuntimeAppearance(Appearance);
	if (!EnsureCharacterPreviewCacheAssetReady(
		Appearance,
		Definition,
		AppearanceSignature,
		CharacterPreviewStem,
		CharacterPreviewPackageRoot,
		TEXT("Selected character refresh requires a complete equipped preview cache")))
	{
		UE_LOG(LogTemp, Warning, TEXT("Selected character preview full cache is unavailable; using runtime visual kit path when possible: guid=%d signature=%s baseRuntime=%d"),
			Appearance.Guid,
			*AppearanceSignature,
			Appearance.bUsesBaseRuntimeComposition ? 1 : 0);
		if (!Appearance.bUsesBaseRuntimeComposition)
		{
			bPendingGlueWaitingForCharacterPreview = true;
			PendingGlueCharacterPreviewScreen = TEXT("charselect");
			if (PendingGlueCharacterPreviewWaitStartSeconds <= 0.0)
			{
				PendingGlueCharacterPreviewWaitStartSeconds = FPlatformTime::Seconds();
			}
			return;
		}
	}
	const FString RequestedAnimationPath = ResolveCharacterPreviewAnimationObjectPath(Appearance.CharacterPreviewAnimationPath, Appearance.EquipmentSummary);
	const FString RequestKey = WoWCharacterVisualKit::BuildPreviewRequestKey(
		SelectedGlueCharacterGuid,
		Appearance.CacheKey,
		Appearance.CharacterPreviewMeshPath,
		RequestedAnimationPath,
		Appearance.CharacterPreviewSectionMetadataPath,
		Appearance.CharacterPreviewAttachmentDescriptorPath);
	if (ActiveCharacterPreviewLoadKey.Equals(RequestKey, ESearchCase::CaseSensitive))
	{
		if (ActiveCharacterPreviewStreamableHandle.IsValid() && ActiveCharacterPreviewStreamableHandle->IsLoadingInProgress())
		{
			UE_LOG(LogTemp, Verbose, TEXT("Character preview async load deduped while pending: guid=%d key=%s"),
				SelectedGlueCharacterGuid,
				*RequestKey);
			return;
		}
		if (SelectedGlueCharacterModelKey.Equals(Appearance.CharacterPreviewMeshPath, ESearchCase::IgnoreCase) &&
			SelectedGlueCharacterAppearanceKey.Equals(Appearance.CacheKey, ESearchCase::CaseSensitive) &&
			CharacterSelectPreviewActor.IsValid())
		{
			UE_LOG(LogTemp, Verbose, TEXT("Character preview refresh deduped because selected appearance is already active: guid=%d key=%s"),
				SelectedGlueCharacterGuid,
				*RequestKey);
			PositionCharacterSelectPreviewActor();
			PendingSelectedCharacterPreviewRefreshGuid = 0;
			return;
		}
	}
	if (PreparedCharacterPreviewLoadKey.Equals(RequestKey, ESearchCase::CaseSensitive) &&
		(!ActiveCharacterPreviewStreamableHandle.IsValid() || !ActiveCharacterPreviewStreamableHandle->IsLoadingInProgress()))
	{
		RefreshSelectedCharacterPreview();
		PendingSelectedCharacterPreviewRefreshGuid = 0;
		return;
	}

	StartSelectedCharacterPreviewAsyncLoad(++CharacterPreviewAsyncRequestSerial, SelectedGlueCharacterGuid, RequestKey, Appearance, true);
}

bool AWoWLoginGameMode::ResolveCharacterPreviewRuntimeAssets(
	const TArray<FString>& Columns,
	FCharacterSelectPreviewAppearance& OutAppearance,
	const FPlayerCharacterPreviewDefinition*& OutDefinition,
	FString& OutAppearanceSignature,
	FString& OutCharacterPreviewStem,
	FString& OutCharacterPreviewPackageRoot,
	FString& OutCharacterPreviewLookupAnimationPath) const
{
	OutDefinition = nullptr;
	OutAppearanceSignature.Empty();
	OutCharacterPreviewStem.Empty();
	OutCharacterPreviewPackageRoot.Empty();
	OutCharacterPreviewLookupAnimationPath.Empty();

	if (!BuildCharacterPreviewAppearanceFromRow(Columns, OutAppearance))
	{
		return false;
	}

	OutDefinition = FindPlayerCharacterPreviewDefinition(OutAppearance.Race, OutAppearance.Gender);
	if (!OutDefinition)
	{
		return false;
	}

	FWoWCharacterVisualInput VisualInput;
	VisualInput.Guid = OutAppearance.Guid;
	VisualInput.CharacterName = OutAppearance.CharacterName;
	VisualInput.ClassId = OutAppearance.ClassId;
	VisualInput.Race = OutAppearance.Race;
	VisualInput.Gender = OutAppearance.Gender;
	VisualInput.Skin = OutAppearance.Skin;
	VisualInput.Face = OutAppearance.Face;
	VisualInput.HairStyle = OutAppearance.HairStyle;
	VisualInput.HairColor = OutAppearance.HairColor;
	VisualInput.FacialStyle = OutAppearance.FacialStyle;
	VisualInput.EquipmentSummary = OutAppearance.EquipmentSummary;
	const FWoWCharacterVisualKit VisualKit = WoWCharacterVisualKit::Build(
		VisualInput,
		MakeCharacterVisualBaseDefinition(*OutDefinition),
		FPaths::ProjectSavedDir());

	OutAppearanceSignature = VisualKit.AppearanceSignature;
	OutCharacterPreviewStem = VisualKit.CharacterPreviewStem;
	OutCharacterPreviewPackageRoot = VisualKit.CharacterPreviewPackageRoot;
	OutCharacterPreviewLookupAnimationPath = VisualKit.LookupAnimationPath;
	OutAppearance.CacheKey = VisualKit.CacheKey;
	OutAppearance.EquipmentEntryCount = VisualKit.EquipmentEntryCount;
	OutAppearance.CharacterPreviewAttachmentDescriptorPath = VisualKit.AttachmentDescriptorPath;
	OutAppearance.CharacterPreviewMeshJsonPath = VisualKit.MeshJsonPath;
	OutAppearance.CharacterPreviewAnimationStem = VisualKit.AnimationStem;
	OutAppearance.CharacterPreviewMeshPath = VisualKit.MeshPath;
	OutAppearance.CharacterPreviewAnimationPath = VisualKit.AnimationPath;
	OutAppearance.CharacterPreviewLookupAnimationPath = VisualKit.LookupAnimationPath;
	OutAppearance.CharacterPreviewAnimationPackagePath = VisualKit.AnimationPackagePath;
	OutAppearance.CharacterPreviewMaterialPackagePath = VisualKit.MaterialPackagePath;
	OutAppearance.CharacterPreviewSectionMetadataPath = VisualKit.SectionMetadataPath;
	OutAppearance.bHasCharacterPreviewAsset = false;
	OutAppearance.bUsesBaseRuntimeComposition = true;

	for (const FWoWCharacterVisualCacheCandidate& Candidate : VisualKit.CacheCandidates)
	{
		if (!DoesGeneratedAssetExist(Candidate.MeshPath))
		{
			continue;
		}
		OutAppearance.CharacterPreviewMeshPath = Candidate.MeshPath;
		OutAppearance.CharacterPreviewMeshJsonPath = Candidate.MeshJsonPath;
		OutAppearance.CharacterPreviewAnimationPath = Candidate.AnimationPath;
		OutAppearance.CharacterPreviewLookupAnimationPath = Candidate.LookupAnimationPath;
		OutAppearance.CharacterPreviewAnimationPackagePath = Candidate.AnimationPackagePath;
		OutAppearance.CharacterPreviewMaterialPackagePath = Candidate.MaterialPackagePath;
		OutAppearance.CharacterPreviewSectionMetadataPath = Candidate.SectionMetadataPath;
		OutCharacterPreviewLookupAnimationPath = Candidate.LookupAnimationPath;
		OutAppearance.bHasCharacterPreviewAsset = true;
		OutAppearance.bUsesBaseRuntimeComposition = false;
		break;
	}

	return true;
}

bool AWoWLoginGameMode::EnsureCharacterPreviewCacheAssetReady(
	const FCharacterSelectPreviewAppearance& Appearance,
	const FPlayerCharacterPreviewDefinition* Definition,
	const FString& AppearanceSignature,
	const FString& CharacterPreviewStem,
	const FString& CharacterPreviewPackageRoot,
	const FString& Reason)
{
	if (Appearance.bHasCharacterPreviewAsset)
	{
		return true;
	}

	WriteCharacterPreviewBuildRequest(
		Appearance,
		Definition,
		AppearanceSignature,
		CharacterPreviewStem,
		CharacterPreviewPackageRoot,
		Reason);
	QueueCharacterPreviewBuild(Appearance, AppearanceSignature, Reason);
	CharacterSelectPreviewDebugState.SelectedGuid = Appearance.Guid;
	CharacterSelectPreviewDebugState.Race = Appearance.Race;
	CharacterSelectPreviewDebugState.ClassId = Appearance.ClassId;
	CharacterSelectPreviewDebugState.Gender = Appearance.Gender;
	CharacterSelectPreviewDebugState.Skin = Appearance.Skin;
	CharacterSelectPreviewDebugState.Face = Appearance.Face;
	CharacterSelectPreviewDebugState.HairStyle = Appearance.HairStyle;
	CharacterSelectPreviewDebugState.HairColor = Appearance.HairColor;
	CharacterSelectPreviewDebugState.FacialStyle = Appearance.FacialStyle;
	CharacterSelectPreviewDebugState.EquipmentSummary = Appearance.EquipmentSummary;
	CharacterSelectPreviewDebugState.AppearanceDescriptorPath = Appearance.AppearanceDescriptorPath;
	CharacterSelectPreviewDebugState.CharacterPreviewMeshPath = Appearance.CharacterPreviewMeshPath;
	CharacterSelectPreviewDebugState.CharacterPreviewAnimationPath = Appearance.CharacterPreviewAnimationPath;
	CharacterSelectPreviewDebugState.CharacterPreviewSectionMetadataPath = Appearance.CharacterPreviewSectionMetadataPath;
	CharacterSelectPreviewDebugState.AppearanceCacheKey = Appearance.CacheKey;
	CharacterSelectPreviewDebugState.bHasAppearanceDescriptor = Appearance.bHasAppearanceDescriptor;
	CharacterSelectPreviewDebugState.bHasCharacterPreviewAsset = false;
	CharacterSelectPreviewDebugState.EquipmentEntryCount = Appearance.EquipmentEntryCount;
	CharacterSelectPreviewDebugState.CharacterPreviewBuildStatus = Appearance.bUsesBaseRuntimeComposition ? TEXT("RuntimeVisualKit") : TEXT("Queued");
	CharacterSelectPreviewDebugState.CharacterPreviewBuildReason = Appearance.bUsesBaseRuntimeComposition
		? TEXT("using lightweight runtime visual kit while full cache is queued offline")
		: Reason;
	if (Definition)
	{
		CharacterSelectPreviewDebugState.DefinitionId = Definition->Id;
		CharacterSelectPreviewDebugState.DisplayName = Definition->DisplayName;
	}

	UE_LOG(LogTemp, Display, TEXT("Glue character preview cache required before commit: guid=%d signature=%s equipmentEntries=%d reason=%s"),
		Appearance.Guid,
		*AppearanceSignature,
		Appearance.EquipmentEntryCount,
		*Reason);
	AddGlueDebugLogLine(FString::Printf(
		TEXT("角色预览完整缓存缺失，已排队离线构建: guid=%d signature=%s equipment=%d"),
		Appearance.Guid,
		*AppearanceSignature,
		Appearance.EquipmentEntryCount));
	if (Appearance.bUsesBaseRuntimeComposition)
	{
		UE_LOG(LogTemp, Display, TEXT("Glue character preview uses lightweight runtime visual kit instead of waiting for UE import: guid=%d signature=%s mesh=%s"),
			Appearance.Guid,
			*AppearanceSignature,
			*Appearance.CharacterPreviewMeshPath);
		return true;
	}
	return false;
}

void AWoWLoginGameMode::CollectCharacterPreviewAsyncAssets(const FCharacterSelectPreviewAppearance& Appearance, TArray<FSoftObjectPath>& InOutAssetsToLoad) const
{
	auto AddAssetPath = [&InOutAssetsToLoad](const FString& ObjectPath)
	{
		const FString TrimmedObjectPath = ObjectPath.TrimStartAndEnd();
		if (!TrimmedObjectPath.IsEmpty())
		{
			InOutAssetsToLoad.AddUnique(FSoftObjectPath(TrimmedObjectPath));
		}
	};

	const int32 PreviewMaterialSlotsToPreload = 128;
	TArray<FString> CoreAssetObjectPaths;
	WoWCharacterVisualKit::CollectCoreAsyncAssetObjectPaths(
		Appearance.CharacterPreviewMeshPath,
		ResolveCharacterPreviewAnimationObjectPath(Appearance.CharacterPreviewAnimationPath, Appearance.EquipmentSummary),
		Appearance.CharacterPreviewMaterialPackagePath,
		PreviewMaterialSlotsToPreload,
		[](const FString& ObjectPath)
		{
			return DoesGeneratedAssetExist(ObjectPath);
		},
		CoreAssetObjectPaths);
	for (const FString& ObjectPath : CoreAssetObjectPaths)
	{
		AddAssetPath(ObjectPath);
	}
	TArray<FString> IdleFallbackAnimationPaths;
	CollectCharacterPreviewIdleFallbackAnimationPaths(
		Appearance.CharacterPreviewAnimationPath,
		Appearance.EquipmentSummary,
		IdleFallbackAnimationPaths);
	CollectCharacterPreviewIdleFallbackAnimationPaths(
		Appearance.CharacterPreviewLookupAnimationPath,
		Appearance.EquipmentSummary,
		IdleFallbackAnimationPaths);
	for (const FString& ObjectPath : IdleFallbackAnimationPaths)
	{
		AddAssetPath(ObjectPath);
	}

	if (!Appearance.bHasCharacterPreviewAsset && !Appearance.bUsesBaseRuntimeComposition)
	{
		return;
	}

	TArray<FCharacterPreviewAttachmentDefinition> AttachmentDefinitions;
	if (FPaths::FileExists(Appearance.CharacterPreviewAttachmentDescriptorPath))
	{
		ReadCharacterPreviewAttachmentDefinitions(Appearance.CharacterPreviewAttachmentDescriptorPath, AttachmentDefinitions);
	}
	if (AttachmentDefinitions.IsEmpty())
	{
		BuildSharedItemObjectAttachmentDefinitions(Appearance, AttachmentDefinitions);
	}
	TArray<FString> AttachmentAssetObjectPaths;
	WoWCharacterVisualKit::CollectAttachmentAsyncAssetObjectPaths(
		Appearance.EquipmentSummary,
		Appearance.CharacterPreviewAttachmentDescriptorPath,
		AttachmentDefinitions,
		PreviewMaterialSlotsToPreload,
		[](const FString& ObjectPath)
		{
			return DoesGeneratedAssetExist(ObjectPath);
		},
		AttachmentAssetObjectPaths);
	for (const FString& ObjectPath : AttachmentAssetObjectPaths)
	{
		AddAssetPath(ObjectPath);
	}
}

bool AWoWLoginGameMode::PrewarmCharacterVisualKit(const FCharacterSelectPreviewAppearance& Appearance)
{
	if (Appearance.Guid <= 0)
	{
		return false;
	}

	const double PrewarmStartSeconds = FPlatformTime::Seconds();
	bool bBodyAtlasReady = true;
	if (Appearance.bUsesBaseRuntimeComposition)
	{
		FWoWRuntimeCharacterAppearanceDescriptor RuntimeDescriptor;
		RuntimeDescriptor.Guid = Appearance.Guid;
		RuntimeDescriptor.Race = Appearance.Race;
		RuntimeDescriptor.Gender = Appearance.Gender;
		RuntimeDescriptor.Skin = Appearance.Skin;
		RuntimeDescriptor.Face = Appearance.Face;
		RuntimeDescriptor.HairStyle = Appearance.HairStyle;
		RuntimeDescriptor.HairColor = Appearance.HairColor;
		RuntimeDescriptor.FacialStyle = Appearance.FacialStyle;
		RuntimeDescriptor.EquipmentSummary = Appearance.EquipmentSummary;
		int32 AppliedLayers = 0;
		int32 MissingLayers = 0;
		bBodyAtlasReady = WoWRuntimeCharacterAppearance::ComposeBodyAtlas(RuntimeDescriptor, AppliedLayers, MissingLayers) != nullptr;
	}

	TArray<FCharacterPreviewAttachmentDefinition> AttachmentDefinitions;
	bool bUsingSharedItemsManifest = false;
	if (FPaths::FileExists(Appearance.CharacterPreviewAttachmentDescriptorPath))
	{
		ReadCharacterPreviewAttachmentDefinitions(Appearance.CharacterPreviewAttachmentDescriptorPath, AttachmentDefinitions);
	}
	if (AttachmentDefinitions.IsEmpty())
	{
		bUsingSharedItemsManifest = BuildSharedItemObjectAttachmentDefinitions(Appearance, AttachmentDefinitions);
	}

	int32 RuntimeM2AttachmentCount = 0;
	int32 RuntimeM2AttachmentReadyCount = 0;
	for (const FCharacterPreviewAttachmentDefinition& AttachmentDefinition : AttachmentDefinitions)
	{
		if (!DoesEquipmentSummaryContainAttachment(Appearance.EquipmentSummary, AttachmentDefinition.Slot, AttachmentDefinition.DisplayId))
		{
			continue;
		}

		const FString AttachmentMeshJsonPath = !AttachmentDefinition.MeshJsonPath.IsEmpty()
			? AttachmentDefinition.MeshJsonPath
			: BuildCharacterPreviewAttachmentMeshJsonPath(AttachmentDefinition.MeshPath, Appearance.CharacterPreviewAttachmentDescriptorPath);
		if (!ShouldUseCharacterPreviewM2RuntimeAttachment(AttachmentDefinition, AttachmentMeshJsonPath))
		{
			continue;
		}

		++RuntimeM2AttachmentCount;
		if (UWoWM2ModelComponent::PrewarmMeshJson(AttachmentMeshJsonPath, true))
		{
			++RuntimeM2AttachmentReadyCount;
		}
	}

	const bool bStrictReady = bBodyAtlasReady && RuntimeM2AttachmentReadyCount == RuntimeM2AttachmentCount;
	UE_LOG(LogTemp, Display, TEXT("Character visual kit prepared: guid=%d bodyAtlas=%d m2Attachments=%d/%d sharedManifest=%d strictReady=%d elapsed=%.2fms"),
		Appearance.Guid,
		bBodyAtlasReady ? 1 : 0,
		RuntimeM2AttachmentReadyCount,
		RuntimeM2AttachmentCount,
		bUsingSharedItemsManifest ? 1 : 0,
		bStrictReady ? 1 : 0,
		(FPlatformTime::Seconds() - PrewarmStartSeconds) * 1000.0);
	if (!bStrictReady)
	{
		UE_LOG(LogTemp, Warning, TEXT("Character visual kit prewarm is incomplete but non-blocking: guid=%d baseRuntime=%d bodyAtlas=%d m2Attachments=%d/%d"),
			Appearance.Guid,
			Appearance.bUsesBaseRuntimeComposition ? 1 : 0,
			bBodyAtlasReady ? 1 : 0,
			RuntimeM2AttachmentReadyCount,
			RuntimeM2AttachmentCount);
	}
	return true;
}

bool AWoWLoginGameMode::CommitPreparedCharacterVisualKit(const FString& RequestKey)
{
	if (RequestKey.IsEmpty() || !PreparedCharacterPreviewLoadKey.Equals(RequestKey, ESearchCase::CaseSensitive))
	{
		return false;
	}
	if (CommittedCharacterPreviewLoadKey.Equals(RequestKey, ESearchCase::CaseSensitive) &&
		CharacterSelectPreviewActor.IsValid())
	{
		PositionCharacterSelectPreviewActor();
		return true;
	}
	if (IsSelectedCharacterPreviewVisualReady())
	{
		CommittedCharacterPreviewLoadKey = RequestKey;
		PositionCharacterSelectPreviewActor();
		UE_LOG(LogTemp, Display, TEXT("Character visual kit committed from already visible preview: guid=%d key=%s"),
			SelectedGlueCharacterGuid,
			*RequestKey);
		return true;
	}

	const double CommitStartSeconds = FPlatformTime::Seconds();
	RefreshSelectedCharacterPreview();
	bool bCommitted = IsSelectedCharacterPreviewVisualReady();
	if (!bCommitted && CharacterSelectPreviewActor.IsValid())
	{
		const ASkeletalMeshActor* PreviewActor = CharacterSelectPreviewActor.Get();
		const USkeletalMeshComponent* MeshComponent = PreviewActor ? PreviewActor->GetSkeletalMeshComponent() : nullptr;
		const bool bVisibleMesh = MeshComponent &&
			MeshComponent->GetSkeletalMeshAsset() &&
			MeshComponent->IsVisible() &&
			!MeshComponent->bHiddenInGame;
		const FString ExpectedGuidPrefix = FString::Printf(TEXT("guid=%d|"), SelectedGlueCharacterGuid);
		if (bVisibleMesh && RequestKey.StartsWith(ExpectedGuidPrefix, ESearchCase::CaseSensitive))
		{
			FString SelectedRow;
			for (const FString& Row : CachedGlueCharacterRows)
			{
				TArray<FString> Columns;
				Row.ParseIntoArray(Columns, TEXT("\t"), false);
				if (Columns.Num() >= 5 && FCString::Atoi(*Columns[0]) == SelectedGlueCharacterGuid)
				{
					SelectedRow = Row;
					break;
				}
			}

			TArray<FString> Columns;
			SelectedRow.ParseIntoArray(Columns, TEXT("\t"), false);
			FCharacterSelectPreviewAppearance Appearance;
			const FPlayerCharacterPreviewDefinition* Definition = nullptr;
			FString AppearanceSignature;
			FString CharacterPreviewStem;
			FString CharacterPreviewPackageRoot;
			FString CharacterPreviewLookupAnimationPath;
			if (!SelectedRow.IsEmpty() &&
				ResolveCharacterPreviewRuntimeAssets(Columns, Appearance, Definition, AppearanceSignature, CharacterPreviewStem, CharacterPreviewPackageRoot, CharacterPreviewLookupAnimationPath) &&
				SelectedGlueCharacterAppearanceKey.Equals(Appearance.CacheKey, ESearchCase::CaseSensitive) &&
				RequestKey.Contains(Appearance.CacheKey, ESearchCase::CaseSensitive))
			{
				bCommitted = true;
				UE_LOG(LogTemp, Warning, TEXT("Character visual kit commit accepted visible runtime preview despite strict ready lag: guid=%d mesh=%s appearance=%s key=%s"),
					SelectedGlueCharacterGuid,
					*SelectedGlueCharacterModelKey,
					*SelectedGlueCharacterAppearanceKey,
					*RequestKey);
			}
		}
	}
	if (bCommitted)
	{
		CommittedCharacterPreviewLoadKey = RequestKey;
		UE_LOG(LogTemp, Display, TEXT("Character visual kit committed: guid=%d elapsed=%.2fms key=%s"),
			SelectedGlueCharacterGuid,
			(FPlatformTime::Seconds() - CommitStartSeconds) * 1000.0,
			*RequestKey);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("Character visual kit commit did not become visual-ready: guid=%d elapsed=%.2fms key=%s"),
			SelectedGlueCharacterGuid,
			(FPlatformTime::Seconds() - CommitStartSeconds) * 1000.0,
			*RequestKey);
	}
	return bCommitted;
}

void AWoWLoginGameMode::StartSelectedCharacterPreviewAsyncLoad(int32 RequestSerial, int32 CharacterGuid, const FString& RequestKey, const FCharacterSelectPreviewAppearance& Appearance, bool bRefreshOnComplete)
{
	TArray<FSoftObjectPath> AssetsToLoad;
	CollectCharacterPreviewAsyncAssets(Appearance, AssetsToLoad);
	if (AssetsToLoad.IsEmpty())
	{
		if (!PrewarmCharacterVisualKit(Appearance))
		{
			UE_LOG(LogTemp, Warning, TEXT("Character visual kit prewarm failed without streamable assets: serial=%d guid=%d key=%s"),
				RequestSerial,
				CharacterGuid,
				*RequestKey);
			return;
		}
		PreparedCharacterPreviewLoadKey = RequestKey;
		if (bPendingGlueWaitingForCharacterPreview && PendingGlueCharacterPreviewScreen.Equals(TEXT("charselect"), ESearchCase::IgnoreCase))
		{
			const FString TargetScreen = PendingGlueCharacterPreviewScreen;
			bPendingGlueWaitingForCharacterPreview = false;
			PendingGlueCharacterPreviewScreen.Empty();
			PendingGlueCharacterPreviewWaitStartSeconds = 0.0;
			if (TryPrepareGlueScreenScene(TargetScreen))
			{
				CompletePendingGlueScreenTransition(TargetScreen);
			}
			return;
		}
		if (bRefreshOnComplete)
		{
			CommitPreparedCharacterVisualKit(RequestKey);
		}
		if (PendingSelectedCharacterPreviewRefreshGuid == CharacterGuid)
		{
			PendingSelectedCharacterPreviewRefreshGuid = 0;
		}
		return;
	}

	ActiveCharacterPreviewLoadKey = RequestKey;
	const double AsyncStartSeconds = FPlatformTime::Seconds();
	UE_LOG(LogTemp, Display, TEXT("Character preview async load requested: serial=%d guid=%d assets=%d mesh=%s metadata=%s equipmentEntries=%d baseRuntime=%d"),
		RequestSerial,
		CharacterGuid,
		AssetsToLoad.Num(),
		*Appearance.CharacterPreviewMeshPath,
		*Appearance.CharacterPreviewSectionMetadataPath,
		Appearance.EquipmentEntryCount,
		Appearance.bUsesBaseRuntimeComposition ? 1 : 0);
	FStreamableManager& StreamableManager = UAssetManager::GetStreamableManager();
	ActiveCharacterPreviewStreamableHandle = StreamableManager.RequestAsyncLoad(
		AssetsToLoad,
		FStreamableDelegate::CreateWeakLambda(this, [this, RequestSerial, CharacterGuid, RequestKey, Appearance, bRefreshOnComplete, AsyncStartSeconds]()
		{
			if (RequestSerial != CharacterPreviewAsyncRequestSerial || CharacterGuid != SelectedGlueCharacterGuid)
			{
				return;
			}
			UE_LOG(LogTemp, Display, TEXT("Character preview async load completed: serial=%d guid=%d elapsed=%.2fms"),
				RequestSerial,
				CharacterGuid,
				(FPlatformTime::Seconds() - AsyncStartSeconds) * 1000.0);
			if (!PrewarmCharacterVisualKit(Appearance))
			{
				UE_LOG(LogTemp, Warning, TEXT("Character visual kit prewarm failed after async load: serial=%d guid=%d totalSinceAsyncStart=%.2fms key=%s"),
					RequestSerial,
					CharacterGuid,
					(FPlatformTime::Seconds() - AsyncStartSeconds) * 1000.0,
					*RequestKey);
				return;
			}
			PreparedCharacterPreviewLoadKey = RequestKey;
			if (bRefreshOnComplete || bPendingGlueWaitingForCharacterPreview)
			{
				if (bRefreshOnComplete && !bPendingGlueWaitingForCharacterPreview)
				{
					CommitPreparedCharacterVisualKit(RequestKey);
				}
			}
			if (bPendingGlueWaitingForCharacterPreview && PendingGlueCharacterPreviewScreen.Equals(TEXT("charselect"), ESearchCase::IgnoreCase))
			{
				const FString TargetScreen = PendingGlueCharacterPreviewScreen;
				bPendingGlueWaitingForCharacterPreview = false;
				PendingGlueCharacterPreviewScreen.Empty();
				const double WaitElapsedMs = PendingGlueCharacterPreviewWaitStartSeconds > 0.0
					? (FPlatformTime::Seconds() - PendingGlueCharacterPreviewWaitStartSeconds) * 1000.0
					: 0.0;
				PendingGlueCharacterPreviewWaitStartSeconds = 0.0;
				UE_LOG(LogTemp, Display, TEXT("Glue pending selected character preview is ready: target=%s guid=%d wait=%.2fms"),
					*TargetScreen,
					CharacterGuid,
					WaitElapsedMs);
				if (TryPrepareGlueScreenScene(TargetScreen))
				{
					CompletePendingGlueScreenTransition(TargetScreen);
				}
				return;
			}
			if (PendingSelectedCharacterPreviewRefreshGuid == CharacterGuid)
			{
				PendingSelectedCharacterPreviewRefreshGuid = 0;
			}
		}),
		FStreamableManager::AsyncLoadHighPriority);
}

bool AWoWLoginGameMode::WriteCharacterPreviewAppearanceDescriptorFromRow(const TArray<FString>& Columns, const FString& EquipmentSummary, FString& OutPath) const
{
	FCharacterSelectPreviewAppearance Appearance;
	if (!BuildCharacterPreviewAppearanceFromRow(Columns, Appearance))
	{
		return false;
	}
	Appearance.EquipmentSummary = EquipmentSummary;

	OutPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("WoWData/CharacterAppearance") / FString::Printf(TEXT("%d.appearance.json"), Appearance.Guid));
	TSharedRef<FJsonObject> Root = BuildCharacterPreviewAppearanceDescriptorObject(Appearance, LoggedInAccountId);
	Root->SetStringField(TEXT("source"), TEXT("worldserver character enum debug snapshot"));
	WriteJsonNumber(Root, TEXT("class"), FCString::Atoi(*GetColumnOrEmpty(Columns, 3)));
	WriteJsonNumber(Root, TEXT("level"), FCString::Atoi(*GetColumnOrEmpty(Columns, 5)));
	WriteJsonNumber(Root, TEXT("zone"), FCString::Atoi(*GetColumnOrEmpty(Columns, 12)));
	WriteJsonNumber(Root, TEXT("map"), FCString::Atoi(*GetColumnOrEmpty(Columns, 13)));

	FString JsonText;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		return false;
	}

	IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutPath), true);
	return FFileHelper::SaveStringToFile(JsonText + LINE_TERMINATOR, *OutPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
}

bool AWoWLoginGameMode::BuildCharacterPreviewAppearanceFromRow(const TArray<FString>& Columns, FCharacterSelectPreviewAppearance& OutAppearance) const
{
	if (Columns.Num() < 17)
	{
		return false;
	}

	const int32 Guid = FCString::Atoi(*Columns[0]);
	if (Guid <= 0)
	{
		return false;
	}

	OutAppearance = FCharacterSelectPreviewAppearance();
	OutAppearance.Guid = Guid;
	OutAppearance.CharacterName = GetColumnOrEmpty(Columns, 1);
	OutAppearance.Race = FCString::Atoi(*Columns[2]);
	OutAppearance.ClassId = FCString::Atoi(*Columns[3]);
	OutAppearance.Gender = FCString::Atoi(*Columns[4]);
	OutAppearance.Skin = FCString::Atoi(*Columns[6]);
	OutAppearance.Face = FCString::Atoi(*Columns[7]);
	OutAppearance.HairStyle = FCString::Atoi(*Columns[8]);
	OutAppearance.HairColor = FCString::Atoi(*Columns[9]);
	OutAppearance.FacialStyle = FCString::Atoi(*Columns[10]);
	OutAppearance.EquipmentSummary = GetColumnOrEmpty(Columns, 16);
	OutAppearance.EquipmentEntryCount = CountEquipmentDisplayEntries(OutAppearance.EquipmentSummary);
	OutAppearance.AppearanceDescriptorPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("WoWData/CharacterAppearance") / FString::Printf(TEXT("%d.appearance.json"), OutAppearance.Guid));
	OutAppearance.bHasAppearanceDescriptor = true;
	OutAppearance.CacheKey = FString::Printf(
		TEXT("guid=%d|race=%d|class=%d|gender=%d|skin=%d|face=%d|hairStyle=%d|hairColor=%d|facial=%d|equipment=%s"),
		OutAppearance.Guid,
		OutAppearance.Race,
		OutAppearance.ClassId,
		OutAppearance.Gender,
		OutAppearance.Skin,
		OutAppearance.Face,
		OutAppearance.HairStyle,
		OutAppearance.HairColor,
		OutAppearance.FacialStyle,
		*OutAppearance.EquipmentSummary);
	return true;
}

void AWoWLoginGameMode::WriteCharacterPreviewBuildRequest(
	const FCharacterSelectPreviewAppearance& Appearance,
	const FPlayerCharacterPreviewDefinition* Definition,
	const FString& AppearanceSignature,
	const FString& CharacterPreviewStem,
	const FString& CharacterPreviewPackageRoot,
	const FString& Reason) const
{
	if (AppearanceSignature.IsEmpty() || Appearance.AppearanceDescriptorPath.IsEmpty())
	{
		return;
	}

	const FString RequestDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("WoWData/CharacterPreviewBuild/Requests"));
	IFileManager::Get().MakeDirectory(*RequestDirectory, true);
	const FString RequestPath = RequestDirectory / FString::Printf(TEXT("%s.request.json"), *AppearanceSignature);

	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetStringField(TEXT("format"), TEXT("wow_character_preview_build_request_v1"));
	Root->SetStringField(TEXT("status"), TEXT("queued"));
	Root->SetStringField(TEXT("reason"), Reason);
	Root->SetStringField(TEXT("createdAt"), FDateTime::UtcNow().ToIso8601());
	WriteJsonNumber(Root, TEXT("account"), LoggedInAccountId);
	WriteJsonNumber(Root, TEXT("guid"), Appearance.Guid);
	Root->SetStringField(TEXT("name"), Appearance.CharacterName);
	WriteJsonNumber(Root, TEXT("race"), Appearance.Race);
	WriteJsonNumber(Root, TEXT("gender"), Appearance.Gender);
	Root->SetStringField(TEXT("appearanceSignature"), AppearanceSignature);
	Root->SetStringField(TEXT("characterPreviewStem"), CharacterPreviewStem);
	Root->SetStringField(TEXT("characterPreviewPackageRoot"), CharacterPreviewPackageRoot);
	Root->SetStringField(TEXT("appearanceDescriptorPath"), Appearance.AppearanceDescriptorPath);
	Root->SetObjectField(TEXT("appearanceDescriptor"), BuildCharacterPreviewAppearanceDescriptorObject(Appearance, LoggedInAccountId));
	const FString ExpectedImportStem = CharacterPreviewStem + TEXT("_EquipFixed6");
	const FString ExpectedAnimationStem = Definition
		? BuildGeneratedCharacterPreviewAnimationStem(Definition->SourceModelPath)
		: CharacterPreviewStem;
	const FString ExpectedMetadataRoot = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("M2Preview/CharacterPreview/Appearance") / AppearanceSignature);
	Root->SetStringField(TEXT("expectedMesh"), MakeObjectPathFromPackagePath(CharacterPreviewPackageRoot / (ExpectedImportStem + TEXT("_M2Mesh"))));
	Root->SetStringField(TEXT("expectedAnimation"), MakeObjectPathFromPackagePath(CharacterPreviewPackageRoot / TEXT("Animations_EquipFixed6") / (TEXT("A_") + ExpectedAnimationStem + TEXT("_000_Stand_00"))));
	Root->SetStringField(TEXT("expectedSections"), ExpectedMetadataRoot / FString::Printf(TEXT("%s.sections.json"), *CharacterPreviewStem));
	Root->SetStringField(TEXT("expectedAttachments"), ExpectedMetadataRoot / FString::Printf(TEXT("%s.attachments.json"), *CharacterPreviewStem));
	Root->SetStringField(TEXT("expectedMaterialPackage"), CharacterPreviewPackageRoot / (CharacterPreviewStem + TEXT("_Materials")));
	Root->SetStringField(TEXT("runtimeResolvedMesh"), Appearance.CharacterPreviewMeshPath);
	Root->SetStringField(TEXT("runtimeResolvedAnimation"), Appearance.CharacterPreviewAnimationPath);
	Root->SetStringField(TEXT("runtimeResolvedSections"), Appearance.CharacterPreviewSectionMetadataPath);
	Root->SetBoolField(TEXT("runtimeUsesBaseComposition"), Appearance.bUsesBaseRuntimeComposition);
	Root->SetStringField(TEXT("equipmentSummary"), Appearance.EquipmentSummary);
	if (Definition)
	{
		Root->SetStringField(TEXT("definitionId"), Definition->Id);
		Root->SetStringField(TEXT("sourceModelPath"), Definition->SourceModelPath);
		Root->SetStringField(TEXT("baseMesh"), Definition->MeshPath);
	}

	FString JsonText;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&JsonText);
	if (FJsonSerializer::Serialize(Root, Writer))
	{
		FFileHelper::SaveStringToFile(JsonText + LINE_TERMINATOR, *RequestPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
	}
}

void AWoWLoginGameMode::QueueCharacterPreviewBuild(
	const FCharacterSelectPreviewAppearance& Appearance,
	const FString& AppearanceSignature,
	const FString& Reason)
{
	if (AppearanceSignature.IsEmpty() || Appearance.AppearanceDescriptorPath.IsEmpty())
	{
		return;
	}
	if (FailedCharacterPreviewBuildSignatures.Contains(AppearanceSignature))
	{
		FailedCharacterPreviewBuildSignatures.Remove(AppearanceSignature);
	}

	QueuedCharacterPreviewBuildSignatures.Add(AppearanceSignature);
	QueuedCharacterPreviewBuildReasons.Add(AppearanceSignature, Reason);
	QueuedCharacterPreviewBuildGuids.Add(AppearanceSignature, FString::FromInt(Appearance.Guid));
	QueuedCharacterPreviewBuildNames.Add(AppearanceSignature, Appearance.CharacterName);
	if (ActiveCharacterPreviewBuildSignature.Equals(AppearanceSignature, ESearchCase::IgnoreCase))
	{
		CharacterSelectPreviewDebugState.CharacterPreviewBuildStatus = TEXT("Building");
		CharacterSelectPreviewDebugState.CharacterPreviewBuildReason = Reason;
		return;
	}

	CharacterSelectPreviewDebugState.CharacterPreviewBuildStatus = TEXT("Queued");
	CharacterSelectPreviewDebugState.CharacterPreviewBuildReason = Reason;
	UE_LOG(LogTemp, Display, TEXT("角色预览完整缓存已排队离线构建，运行时不启动 UE 导入: guid=%d signature=%s reason=%s"),
		Appearance.Guid,
		*AppearanceSignature,
		*Reason);
}

bool AWoWLoginGameMode::StartCharacterPreviewBuildProcess(const FString& AppearanceSignature, const FString& GuidText, const FString& Reason)
{
	if (AppearanceSignature.IsEmpty())
	{
		return false;
	}
	if (ActiveCharacterPreviewBuildProcess.IsValid())
	{
		return false;
	}

	const FString PythonPath = FPaths::ConvertRelativePathToFull(FPaths::EngineDir() / TEXT("Binaries/ThirdParty/Python3/Win64/python.exe"));
	const FString BuilderPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectDir() / TEXT("Tools/ProcessCharacterPreviewBuildRequests.py"));
	if (!FPaths::FileExists(PythonPath) || !FPaths::FileExists(BuilderPath))
	{
		FailedCharacterPreviewBuildSignatures.Add(AppearanceSignature);
		CharacterSelectPreviewDebugState.CharacterPreviewBuildStatus = CharacterPreviewBuildStatusToString(ECharacterPreviewBuildStatus::MissingCriticalSource);
		CharacterSelectPreviewDebugState.CharacterPreviewBuildReason = FString::Printf(TEXT("build coordinator missing python/tool: python=%s tool=%s"), *PythonPath, *BuilderPath);
		UE_LOG(LogTemp, Warning, TEXT("角色预览自动构建器缺失: signature=%s python=%s tool=%s"),
			*AppearanceSignature,
			*PythonPath,
			*BuilderPath);
		return false;
	}

	const FString Args = FString::Printf(
		TEXT("%s --signature %s --limit 1"),
		*QuoteCommandArgument(BuilderPath),
		*QuoteCommandArgument(AppearanceSignature));
	ActiveCharacterPreviewBuildProcess = FPlatformProcess::CreateProc(
		*PythonPath,
		*Args,
		false,
		true,
		true,
		&ActiveCharacterPreviewBuildProcessId,
		0,
		*FPaths::ProjectDir(),
		nullptr,
		nullptr);
	if (!ActiveCharacterPreviewBuildProcess.IsValid())
	{
		FailedCharacterPreviewBuildSignatures.Add(AppearanceSignature);
		QueuedCharacterPreviewBuildSignatures.Remove(AppearanceSignature);
		CharacterSelectPreviewDebugState.CharacterPreviewBuildStatus = CharacterPreviewBuildStatusToString(ECharacterPreviewBuildStatus::FailedPreviously);
		CharacterSelectPreviewDebugState.CharacterPreviewBuildReason = FString::Printf(TEXT("failed to launch build coordinator: %s %s"), *PythonPath, *Args);
		UE_LOG(LogTemp, Warning, TEXT("角色预览自动构建启动失败: signature=%s command=%s %s"),
			*AppearanceSignature,
			*PythonPath,
			*Args);
		return false;
	}

	ActiveCharacterPreviewBuildSignature = AppearanceSignature;
	ActiveCharacterPreviewBuildGuid = GuidText;
	ActiveCharacterPreviewBuildReason = Reason;
	ActiveCharacterPreviewBuildStartSeconds = FPlatformTime::Seconds();
	const FString CharacterName = QueuedCharacterPreviewBuildNames.FindRef(AppearanceSignature);
	CharacterSelectPreviewDebugState.CharacterPreviewBuildStatus = TEXT("Building");
	CharacterSelectPreviewDebugState.CharacterPreviewBuildReason = Reason;
	UE_LOG(LogTemp, Display, TEXT("角色预览自动构建已启动: pid=%u guid=%d name=%s signature=%s command=%s %s"),
		ActiveCharacterPreviewBuildProcessId,
		FCString::Atoi(*GuidText),
		*CharacterName,
		*AppearanceSignature,
		*PythonPath,
		*Args);
	AddGlueDebugLogLine(FString::Printf(TEXT("角色预览自动构建开始: pid=%u guid=%s name=%s signature=%s"), ActiveCharacterPreviewBuildProcessId, *GuidText, *CharacterName, *AppearanceSignature));
	return true;
}

bool AWoWLoginGameMode::StartNextQueuedCharacterPreviewBuild()
{
	if (ActiveCharacterPreviewBuildProcess.IsValid())
	{
		return false;
	}

	const FString RequestDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("WoWData/CharacterPreviewBuild/Requests"));
	TArray<FString> RequestFiles;
	IFileManager::Get().FindFiles(RequestFiles, *(RequestDirectory / TEXT("*.request.json")), true, false);
	RequestFiles.Sort();
	for (const FString& RequestFile : RequestFiles)
	{
		const FString Signature = RequestFile.LeftChop(FString(TEXT(".request.json")).Len());
		if (Signature.IsEmpty() || FailedCharacterPreviewBuildSignatures.Contains(Signature))
		{
			continue;
		}
		const FString* Reason = QueuedCharacterPreviewBuildReasons.Find(Signature);
		const FString* Guid = QueuedCharacterPreviewBuildGuids.Find(Signature);
		if (StartCharacterPreviewBuildProcess(
			Signature,
			Guid ? *Guid : TEXT("0"),
			Reason ? *Reason : TEXT("queued character preview build request")))
		{
			return true;
		}
	}
	return false;
}

bool AWoWLoginGameMode::IsCharacterPreviewBuildRequestComplete(const FString& AppearanceSignature) const
{
	if (AppearanceSignature.IsEmpty())
	{
		return false;
	}

	const FString CompletedRequestPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("WoWData/CharacterPreviewBuild/Completed") / FString::Printf(TEXT("%s.request.json"), *AppearanceSignature));
	return FPaths::FileExists(CompletedRequestPath);
}

void AWoWLoginGameMode::ResetCharacterPreviewBuildProcess()
{
	if (ActiveCharacterPreviewBuildProcess.IsValid())
	{
		FPlatformProcess::CloseProc(ActiveCharacterPreviewBuildProcess);
	}
	ActiveCharacterPreviewBuildProcess.Reset();
	ActiveCharacterPreviewBuildProcessId = 0;
	ActiveCharacterPreviewBuildSignature.Empty();
	ActiveCharacterPreviewBuildGuid.Empty();
	ActiveCharacterPreviewBuildReason.Empty();
	ActiveCharacterPreviewBuildStartSeconds = 0.0;
}

void AWoWLoginGameMode::ProcessCharacterPreviewBuildCoordinator()
{
	if (!ActiveCharacterPreviewBuildProcess.IsValid())
	{
		return;
	}
	if (FPlatformProcess::IsProcRunning(ActiveCharacterPreviewBuildProcess))
	{
		if (SelectedGlueCharacterGuid > 0 && !ActiveCharacterPreviewBuildGuid.IsEmpty() && ActiveCharacterPreviewBuildGuid == FString::FromInt(SelectedGlueCharacterGuid))
		{
			const double ElapsedSeconds = FPlatformTime::Seconds() - ActiveCharacterPreviewBuildStartSeconds;
			FString StageText;
			const FString RequestPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("WoWData/CharacterPreviewBuild/Requests") / FString::Printf(TEXT("%s.request.json"), *ActiveCharacterPreviewBuildSignature));
			FString RequestJsonText;
			if (FFileHelper::LoadFileToString(RequestJsonText, *RequestPath))
			{
				TSharedPtr<FJsonObject> RequestJson;
				const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RequestJsonText);
				if (FJsonSerializer::Deserialize(Reader, RequestJson) && RequestJson.IsValid())
				{
					RequestJson->TryGetStringField(TEXT("buildStageText"), StageText);
				}
			}
			if (StageText.IsEmpty())
			{
				StageText = ActiveCharacterPreviewBuildReason;
			}
			CharacterSelectPreviewDebugState.CharacterPreviewBuildStatus = TEXT("Building");
			CharacterSelectPreviewDebugState.CharacterPreviewBuildReason = FString::Printf(
				TEXT("%s pid=%u signature=%s elapsed=%.1fs"),
				*StageText,
				ActiveCharacterPreviewBuildProcessId,
				*ActiveCharacterPreviewBuildSignature,
				ElapsedSeconds);
		}
		return;
	}

	int32 ReturnCode = INDEX_NONE;
	FPlatformProcess::GetProcReturnCode(ActiveCharacterPreviewBuildProcess, &ReturnCode);
	const FString FinishedSignature = ActiveCharacterPreviewBuildSignature;
	const FString FinishedGuid = ActiveCharacterPreviewBuildGuid;
	const FString FinishedName = QueuedCharacterPreviewBuildNames.FindRef(FinishedSignature);
	const double ElapsedSeconds = FPlatformTime::Seconds() - ActiveCharacterPreviewBuildStartSeconds;
	QueuedCharacterPreviewBuildSignatures.Remove(FinishedSignature);
	QueuedCharacterPreviewBuildReasons.Remove(FinishedSignature);
	QueuedCharacterPreviewBuildGuids.Remove(FinishedSignature);
	QueuedCharacterPreviewBuildNames.Remove(FinishedSignature);
	ResetCharacterPreviewBuildProcess();

	bool bFinishedCurrentSelectedAppearance = false;
	if (SelectedGlueCharacterGuid > 0)
	{
		for (const FString& Row : CachedGlueCharacterRows)
		{
			TArray<FString> Columns;
			Row.ParseIntoArray(Columns, TEXT("\t"), false);
			if (Columns.Num() >= 17 && FCString::Atoi(*Columns[0]) == SelectedGlueCharacterGuid)
			{
				FCharacterSelectPreviewAppearance CurrentAppearance;
				if (BuildCharacterPreviewAppearanceFromRow(Columns, CurrentAppearance))
				{
					const FString CurrentSignature = BuildCharacterAppearanceSignature(
						CurrentAppearance.Race,
						CurrentAppearance.Gender,
						CurrentAppearance.Skin,
						CurrentAppearance.Face,
						CurrentAppearance.HairStyle,
						CurrentAppearance.HairColor,
						CurrentAppearance.FacialStyle,
						CurrentAppearance.EquipmentSummary);
					bFinishedCurrentSelectedAppearance = CurrentSignature.Equals(FinishedSignature, ESearchCase::IgnoreCase);
				}
				break;
			}
		}
	}

	if (ReturnCode == 0 && IsCharacterPreviewBuildRequestComplete(FinishedSignature))
	{
		UE_LOG(LogTemp, Display, TEXT("角色预览自动构建完成: guid=%s name=%s signature=%s elapsed=%.2f"),
			*FinishedGuid,
			*FinishedName,
			*FinishedSignature,
			ElapsedSeconds);
		AddGlueDebugLogLine(FString::Printf(TEXT("角色预览自动构建完成: guid=%s name=%s signature=%s elapsed=%.2fs"), *FinishedGuid, *FinishedName, *FinishedSignature, ElapsedSeconds));
		if (bFinishedCurrentSelectedAppearance)
		{
			SelectedGlueCharacterModelKey.Empty();
			SelectedGlueCharacterAnimationKey.Empty();
			SelectedGlueCharacterAppearanceKey.Empty();
			RequestSelectedCharacterPreviewRefresh();
		}
		StartNextQueuedCharacterPreviewBuild();
		return;
	}

	FailedCharacterPreviewBuildSignatures.Add(FinishedSignature);
	UE_LOG(LogTemp, Warning, TEXT("角色预览自动构建失败: guid=%s name=%s signature=%s exit=%d elapsed=%.2f"),
		*FinishedGuid,
		*FinishedName,
		*FinishedSignature,
		ReturnCode,
		ElapsedSeconds);
	AddGlueDebugLogLine(FString::Printf(TEXT("角色预览自动构建失败: guid=%s name=%s signature=%s exit=%d elapsed=%.2fs"), *FinishedGuid, *FinishedName, *FinishedSignature, ReturnCode, ElapsedSeconds));
	if (SelectedGlueCharacterGuid > 0 && FinishedGuid == FString::FromInt(SelectedGlueCharacterGuid))
	{
		CharacterSelectPreviewDebugState.CharacterPreviewBuildStatus = CharacterPreviewBuildStatusToString(ECharacterPreviewBuildStatus::FailedPreviously);
		CharacterSelectPreviewDebugState.CharacterPreviewBuildReason = FString::Printf(TEXT("build coordinator failed: exit=%d signature=%s"), ReturnCode, *FinishedSignature);
	}
	StartNextQueuedCharacterPreviewBuild();
}

void AWoWLoginGameMode::ReportCharacterPreviewBuildIssue(
	ECharacterPreviewBuildStatus Status,
	const FString& Stage,
	const FCharacterSelectPreviewAppearance& Appearance,
	const FPlayerCharacterPreviewDefinition* Definition,
	const FString& AppearanceSignature,
	const FString& CharacterPreviewStem,
	const FString& CharacterPreviewPackageRoot,
	const FString& Reason)
{
	const FString StatusText = CharacterPreviewBuildStatusToString(Status);
	const FString DateSegment = FDateTime::Now().ToString(TEXT("%Y%m%d"));
	const FString LogDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("Logs/WoWRuntime/CharacterPreviewBuild") / DateSegment);
	IFileManager::Get().MakeDirectory(*LogDirectory, true);

	const FString FileName = FString::Printf(
		TEXT("CharacterPreviewBuild_%s_guid%d_race%d_gender%d.log"),
		AppearanceSignature.IsEmpty() ? TEXT("nosignature") : *AppearanceSignature,
		Appearance.Guid,
		Appearance.Race,
		Appearance.Gender);
	const FString BuildLogPath = LogDirectory / FileName;

	FString Text;
	Text += TEXT("CharacterPreviewBuildStatus\n");
	Text += FString::Printf(TEXT("status=%s\n"), *StatusText);
	Text += FString::Printf(TEXT("stage=%s\n"), *Stage);
	Text += FString::Printf(TEXT("reason=%s\n"), *Reason);
	Text += FString::Printf(TEXT("account=%d\n"), LoggedInAccountId);
	Text += FString::Printf(TEXT("guid=%d\n"), Appearance.Guid);
	Text += FString::Printf(TEXT("name=%s\n"), *Appearance.CharacterName);
	Text += FString::Printf(TEXT("race=%d\n"), Appearance.Race);
	Text += FString::Printf(TEXT("gender=%d\n"), Appearance.Gender);
	Text += FString::Printf(TEXT("skin=%d\n"), Appearance.Skin);
	Text += FString::Printf(TEXT("face=%d\n"), Appearance.Face);
	Text += FString::Printf(TEXT("hairStyle=%d\n"), Appearance.HairStyle);
	Text += FString::Printf(TEXT("hairColor=%d\n"), Appearance.HairColor);
	Text += FString::Printf(TEXT("facialStyle=%d\n"), Appearance.FacialStyle);
	Text += FString::Printf(TEXT("equipmentEntries=%d\n"), Appearance.EquipmentEntryCount);
	Text += FString::Printf(TEXT("equipmentSummary=%s\n"), *Appearance.EquipmentSummary);
	Text += FString::Printf(TEXT("appearanceSignature=%s\n"), *AppearanceSignature);
	Text += FString::Printf(TEXT("characterPreviewStem=%s\n"), *CharacterPreviewStem);
	Text += FString::Printf(TEXT("characterPreviewPackageRoot=%s\n"), *CharacterPreviewPackageRoot);
	Text += FString::Printf(TEXT("appearanceDescriptorPath=%s\n"), *Appearance.AppearanceDescriptorPath);
	Text += FString::Printf(TEXT("hasAppearanceDescriptor=%d\n"), Appearance.bHasAppearanceDescriptor ? 1 : 0);
	Text += FString::Printf(TEXT("expectedMesh=%s\n"), *Appearance.CharacterPreviewMeshPath);
	Text += FString::Printf(TEXT("expectedAnimation=%s\n"), *Appearance.CharacterPreviewAnimationPath);
	Text += FString::Printf(TEXT("expectedSections=%s\n"), *Appearance.CharacterPreviewSectionMetadataPath);
	Text += FString::Printf(TEXT("expectedAttachments=%s\n"), *Appearance.CharacterPreviewAttachmentDescriptorPath);
	if (Definition)
	{
		Text += FString::Printf(TEXT("definitionId=%s\n"), *Definition->Id);
		Text += FString::Printf(TEXT("displayName=%s\n"), *Definition->DisplayName);
		Text += FString::Printf(TEXT("sourceModelPath=%s\n"), *Definition->SourceModelPath);
		Text += FString::Printf(TEXT("baseMesh=%s\n"), *Definition->MeshPath);
		Text += FString::Printf(TEXT("baseStandAnimation=%s\n"), *Definition->StandAnimationPath);
		Text += FString::Printf(TEXT("baseSections=%s\n"), *Definition->SectionMetadataPath);
	}

	if (FFileHelper::SaveStringToFile(Text, *BuildLogPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		CharacterSelectPreviewDebugState.CharacterPreviewBuildLogPath = BuildLogPath;
	}
	CharacterSelectPreviewDebugState.SelectedGuid = Appearance.Guid;
	CharacterSelectPreviewDebugState.Race = Appearance.Race;
	CharacterSelectPreviewDebugState.ClassId = Appearance.ClassId;
	CharacterSelectPreviewDebugState.Gender = Appearance.Gender;
	CharacterSelectPreviewDebugState.Skin = Appearance.Skin;
	CharacterSelectPreviewDebugState.Face = Appearance.Face;
	CharacterSelectPreviewDebugState.HairStyle = Appearance.HairStyle;
	CharacterSelectPreviewDebugState.HairColor = Appearance.HairColor;
	CharacterSelectPreviewDebugState.FacialStyle = Appearance.FacialStyle;
	CharacterSelectPreviewDebugState.EquipmentSummary = Appearance.EquipmentSummary;
	CharacterSelectPreviewDebugState.AppearanceDescriptorPath = Appearance.AppearanceDescriptorPath;
	CharacterSelectPreviewDebugState.CharacterPreviewMeshPath = Appearance.CharacterPreviewMeshPath;
	CharacterSelectPreviewDebugState.CharacterPreviewAnimationPath = Appearance.CharacterPreviewAnimationPath;
	CharacterSelectPreviewDebugState.CharacterPreviewSectionMetadataPath = Appearance.CharacterPreviewSectionMetadataPath;
	CharacterSelectPreviewDebugState.AppearanceCacheKey = Appearance.CacheKey;
	CharacterSelectPreviewDebugState.bHasAppearanceDescriptor = Appearance.bHasAppearanceDescriptor;
	CharacterSelectPreviewDebugState.bHasCharacterPreviewAsset = Appearance.bHasCharacterPreviewAsset;
	CharacterSelectPreviewDebugState.EquipmentEntryCount = Appearance.EquipmentEntryCount;
	if (Definition)
	{
		CharacterSelectPreviewDebugState.DefinitionId = Definition->Id;
		CharacterSelectPreviewDebugState.DisplayName = Definition->DisplayName;
	}
	CharacterSelectPreviewDebugState.CharacterPreviewBuildStatus = StatusText;
	CharacterSelectPreviewDebugState.CharacterPreviewBuildReason = FString::Printf(TEXT("%s: %s"), *Stage, *Reason);

	UE_LOG(LogTemp, Warning, TEXT("角色预览构建状态: status=%s stage=%s guid=%d signature=%s reason=%s log=%s"),
		*StatusText,
		*Stage,
		Appearance.Guid,
		*AppearanceSignature,
		*Reason,
		*BuildLogPath);
}

void AWoWLoginGameMode::ApplyGlueSceneForScreen(const FString& ScreenName)
{
	if (ScreenName.Equals(TEXT("login"), ESearchCase::IgnoreCase))
	{
		DestroyCharacterSelectPreviewActor();
		if (LoginSceneDefinitions.IsValidIndex(LoginScreenSceneIndex))
		{
			RequestActiveLoginSceneIndex(LoginScreenSceneIndex);
		}
		return;
	}

	if (!ScreenName.Equals(TEXT("charselect"), ESearchCase::IgnoreCase) &&
		!ScreenName.Equals(TEXT("charcreate"), ESearchCase::IgnoreCase))
	{
		DestroyCharacterSelectPreviewActor();
		return;
	}

	// 角色选择/创建背景由 Glue Lua 的 GetSelectBackgroundModel/SetBackgroundModel 按种族决定。
	// 不在 C++ 层沿用旧的 WLK/旧世互换逻辑，否则会覆盖角色列表的种族背景。
	if (ScreenName.Equals(TEXT("charselect"), ESearchCase::IgnoreCase))
	{
		CallGlueGlobal(TEXT("CharacterSelect_RefreshBackground"));
		if (!CharacterSelectPreviewActor.IsValid())
		{
			RequestSelectedCharacterPreviewRefresh();
		}
	}
	else if (ScreenName.Equals(TEXT("charcreate"), ESearchCase::IgnoreCase) && CharacterSelectPreviewActor.IsValid())
	{
		DestroyCharacterSelectPreviewActor();
	}
}

void AWoWLoginGameMode::HandleGlueCharacterSelected(int32 CharacterGuid)
{
	UE_LOG(LogTemp, Display, TEXT("Glue 选择角色: guid=%d account=%d"), CharacterGuid, LoggedInAccountId);
	if (SelectedGlueCharacterGuid == CharacterGuid && CharacterSelectPreviewActor.IsValid())
	{
		return;
	}

	CharacterSelectPreviewUserYawOffset = 0.0f;
	bCharacterSelectPreviewDragging = false;
	SelectedGlueCharacterGuid = CharacterGuid;
	// 官方 Glue 中 SelectCharacter 只改变当前选中角色并刷新角色模型，EnterWorld 才进入游戏。
	// 后续真实角色模型预览会从这里读取选中角色的 race/gender/appearance/equipment 描述。
	const FString TargetScreen = TEXT("charselect");
	const bool bPreviewReady = TryPrepareGlueScreenCharacterPreview(TargetScreen);
	if (bPreviewReady)
	{
		CompletePendingGlueScreenTransition(TargetScreen);
	}
}

void AWoWLoginGameMode::HandleGlueEnterWorld()
{
	UE_LOG(LogTemp, Display, TEXT("Glue 进入世界: selectedGuid=%d account=%d"), SelectedGlueCharacterGuid, LoggedInAccountId);
	const UWoWGameInstance* WoWGameInstance = GetGameInstance<UWoWGameInstance>();
	const FWoWSelectedCharacterRuntimeAppearance* SelectedRuntimeAppearance = WoWGameInstance
		? &WoWGameInstance->GetSelectedCharacterRuntimeAppearance()
		: nullptr;
	if (!SelectedRuntimeAppearance || !SelectedRuntimeAppearance->IsValid() || SelectedRuntimeAppearance->Guid != SelectedGlueCharacterGuid)
	{
		RequestSelectedCharacterPreviewRefresh();
		UE_LOG(LogTemp, Error, TEXT("进入世界失败：没有有效的选中角色运行态外观。selectedGuid=%d hasGameInstance=%d"),
			SelectedGlueCharacterGuid,
			WoWGameInstance ? 1 : 0);
		if (LoginGlueLuaRuntime.IsValid())
		{
			LoginGlueLuaRuntime->SetAccountErrorText(TEXT("当前选中角色外观尚未准备完成，请重新选择角色后再进入世界。"));
			PushGlueStateToLua();
		}
		return;
	}
	HandleEnterTestWorldClicked();
}

void AWoWLoginGameMode::HandleGlueCharacterCreate(const FString& CharacterName, int32 Race, int32 ClassId, int32 Gender, int32 Skin, int32 Face, int32 HairStyle, int32 HairColor, int32 FacialStyle)
{
	if (LoggedInAccountId <= 0)
	{
		if (LoginGlueLuaRuntime.IsValid())
		{
			LoginGlueLuaRuntime->SetAccountErrorText(GetGlueString(TEXT("LOGIN_UNKNOWN_ACCOUNT"), TEXT("请先登录账号。")));
			LoginGlueLuaRuntime->SetCurrentGlueScreen(TEXT("login"));
			PushGlueStateToLua();
		}
		return;
	}

	FWoWGlueCharacterCreateRequest Request;
	Request.Name = CharacterName;
	Request.Race = static_cast<uint8>(Race);
	Request.ClassId = static_cast<uint8>(ClassId);
	Request.Gender = static_cast<uint8>(Gender);
	Request.Skin = static_cast<uint8>(Skin);
	Request.Face = static_cast<uint8>(Face);
	Request.HairStyle = static_cast<uint8>(HairStyle);
	Request.HairColor = static_cast<uint8>(HairColor);
	Request.FacialStyle = static_cast<uint8>(FacialStyle);

	LastCreatedRace = Race;
	LastCreatedClass = ClassId;
	LastCreatedGender = Gender;

	FWoWGlueCharacterSummary CreatedCharacter;
	FString ErrorText;
	if (!FWoWAzerothCoreService::CreateCharacter(LoggedInAccountId, Request, CreatedCharacter, ErrorText))
	{
		UE_LOG(LogTemp, Warning, TEXT("Glue 创建角色失败: %s"), *ErrorText);
		if (LoginGlueLuaRuntime.IsValid())
		{
			LoginGlueLuaRuntime->SetAccountErrorText(ErrorText);
			LoginGlueLuaRuntime->SetCurrentGlueScreen(TEXT("charcreate"));
			PushGlueStateToLua();
		}
		return;
	}

	if (LoginGlueLuaRuntime.IsValid())
	{
		LoginGlueLuaRuntime->SetAccountErrorText(FString());
	}
	RefreshGlueCharacterList();
	if (LoginGlueLuaRuntime.IsValid())
	{
		SelectedGlueCharacterGuid = CreatedCharacter.Guid;
		const FString TargetScreen = TEXT("charselect");
		bPendingGlueCloseStatusDialog = false;
		if (TryPrepareGlueScreenScene(TargetScreen) && TryPrepareGlueScreenCharacterPreview(TargetScreen))
		{
			CompletePendingGlueScreenTransition(TargetScreen);
		}
	}
}

void AWoWLoginGameMode::RefreshSelectedCharacterPreview()
{
	const double RefreshStartSeconds = FPlatformTime::Seconds();
	if (SelectedGlueCharacterGuid <= 0)
	{
		return;
	}

	FString SelectedRow;
	for (const FString& Row : CachedGlueCharacterRows)
	{
		TArray<FString> Columns;
		Row.ParseIntoArray(Columns, TEXT("\t"), false);
		if (Columns.Num() >= 5 && FCString::Atoi(*Columns[0]) == SelectedGlueCharacterGuid)
		{
			SelectedRow = Row;
			break;
		}
	}

	if (SelectedRow.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览找不到选中角色: guid=%d rows=%d"), SelectedGlueCharacterGuid, CachedGlueCharacterRows.Num());
		return;
	}

	TArray<FString> Columns;
	SelectedRow.ParseIntoArray(Columns, TEXT("\t"), false);
	FCharacterSelectPreviewAppearance Appearance;
	const FPlayerCharacterPreviewDefinition* Definition = nullptr;
	FString AppearanceSignature;
	FString CharacterPreviewStem;
	FString CharacterPreviewPackageRoot;
	FString CharacterPreviewLookupAnimationPath;
	if (!ResolveCharacterPreviewRuntimeAssets(Columns, Appearance, Definition, AppearanceSignature, CharacterPreviewStem, CharacterPreviewPackageRoot, CharacterPreviewLookupAnimationPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览无法解析选中角色运行态外观: guid=%d row=%s"),
			SelectedGlueCharacterGuid,
			*SelectedRow);
		DestroyCharacterSelectPreviewActor();
		return;
	}
	const double ResolveElapsedMs = (FPlatformTime::Seconds() - RefreshStartSeconds) * 1000.0;

	CharacterSelectPreviewDebugState.CharacterPreviewBuildStatus = CharacterPreviewBuildStatusToString(ECharacterPreviewBuildStatus::Unknown);
	CharacterSelectPreviewDebugState.CharacterPreviewBuildReason.Empty();
	CharacterSelectPreviewDebugState.CharacterPreviewBuildLogPath.Empty();
	StoreSelectedCharacterRuntimeAppearance(Appearance);
	if (!EnsureCharacterPreviewCacheAssetReady(
		Appearance,
		Definition,
		AppearanceSignature,
		CharacterPreviewStem,
		CharacterPreviewPackageRoot,
		TEXT("Visible character select preview refuses base-runtime fallback; waiting for complete equipped cache")))
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览完整缓存缺失，改用运行态 VisualKit 提交: guid=%d signature=%s baseRuntime=%d"),
			Appearance.Guid,
			*AppearanceSignature,
			Appearance.bUsesBaseRuntimeComposition ? 1 : 0);
		if (!Appearance.bUsesBaseRuntimeComposition)
		{
			UE_LOG(LogTemp, Display, TEXT("角色预览保持当前可见模型，等待完整装备缓存: guid=%d signature=%s"),
				Appearance.Guid,
				*AppearanceSignature);
			return;
		}
	}

	const FString MeshObjectPath = Appearance.CharacterPreviewMeshPath;
	const FString DefaultAnimationObjectPath = Appearance.CharacterPreviewAnimationPath;
	const FString EquipmentAnimationObjectPath = ResolveCharacterPreviewAnimationObjectPath(DefaultAnimationObjectPath, Appearance.EquipmentSummary);
	USkeletalMesh* PreviewMesh = ResolveLoadedObject<USkeletalMesh>(MeshObjectPath);
	if (!PreviewMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览 mesh 未预加载，拒绝在 apply 阶段同步加载: guid=%d mesh=%s"),
			SelectedGlueCharacterGuid,
			*MeshObjectPath);
	}
	const double MeshLoadedElapsedMs = (FPlatformTime::Seconds() - RefreshStartSeconds) * 1000.0;
	int32 OverrideAnimationId = LoginSceneTuningState.CharacterPreviewAnimationOverrideId;
	if (OverrideAnimationId == INDEX_NONE && !LoginSceneTuningState.CharacterPreviewAnimationOverridePath.IsEmpty())
	{
		TryParseCharacterPreviewAnimationIdFromObjectPath(LoginSceneTuningState.CharacterPreviewAnimationOverridePath, OverrideAnimationId);
	}
	FString AnimationObjectPath = EquipmentAnimationObjectPath;
	if (OverrideAnimationId != INDEX_NONE && PreviewMesh)
	{
		FString CompatibleOverridePath;
		if (TryGetResolvedCharacterPreviewAnimationPath(MeshObjectPath, BuildCharacterPreviewAnimationOptionsCacheKey(), OverrideAnimationId, CompatibleOverridePath))
		{
			AnimationObjectPath = CompatibleOverridePath;
		}
		else
		{
			UE_LOG(LogTemp, Display, TEXT("角色预览动作覆盖等待分帧目录解析，先使用自动动作: guid=%d overrideId=%d rawOverride=%s fallback=%s mesh=%s"),
				SelectedGlueCharacterGuid,
				OverrideAnimationId,
				*LoginSceneTuningState.CharacterPreviewAnimationOverridePath,
				*EquipmentAnimationObjectPath,
				*MeshObjectPath);
		}
	}
	UE_LOG(LogTemp, Display, TEXT("角色预览动作解析: guid=%d mesh=%s finalAnim=%s overrideId=%d rawOverride=%s hasAppearanceCache=%d appearanceStand=%s appearanceLookup=%s baseStand=%s baseLookup=%s equipmentAnim=%s"),
		SelectedGlueCharacterGuid,
		*MeshObjectPath,
		*AnimationObjectPath,
		OverrideAnimationId,
		*LoginSceneTuningState.CharacterPreviewAnimationOverridePath,
		Appearance.bHasCharacterPreviewAsset ? 1 : 0,
		*Appearance.CharacterPreviewAnimationPath,
		*CharacterPreviewLookupAnimationPath,
		*Definition->StandAnimationPath,
		*Definition->LookupAnimationPath,
		*EquipmentAnimationObjectPath);
	const FString PreviewSourceId = Definition->Id;
	const FString PreviewSourceModelPath = Definition->SourceModelPath;
	const int32 Race = Appearance.Race;
	const int32 Gender = Appearance.Gender;
	if (Appearance.bHasCharacterPreviewAsset)
	{
		UE_LOG(LogTemp, Display, TEXT("角色预览使用当前角色外观缓存资产: guid=%d race=%d gender=%d mesh=%s animation=%s metadata=%s"),
			SelectedGlueCharacterGuid,
			Race,
			Gender,
			*Appearance.CharacterPreviewMeshPath,
			*Appearance.CharacterPreviewAnimationPath,
			*Appearance.CharacterPreviewSectionMetadataPath);
	}
	else if (Appearance.bUsesBaseRuntimeComposition)
	{
		UE_LOG(LogTemp, Display, TEXT("角色预览使用 BaseRuntimeComposed 正式运行态外观: guid=%d race=%d gender=%d mesh=%s animation=%s metadata=%s"),
			SelectedGlueCharacterGuid,
			Race,
			Gender,
			*Appearance.CharacterPreviewMeshPath,
			*Appearance.CharacterPreviewAnimationPath,
			*Appearance.CharacterPreviewSectionMetadataPath);
	}
	else if (Appearance.bHasAppearanceDescriptor)
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览已有内存外观描述但缺少可用运行态基础资产: guid=%d signature=%s expectedMesh=%s debugDescriptor=%s"),
			SelectedGlueCharacterGuid,
			*AppearanceSignature,
			*Appearance.CharacterPreviewMeshPath,
			*Appearance.AppearanceDescriptorPath);
	}
	if (Appearance.bHasCharacterPreviewAsset)
	{
		CharacterSelectPreviewDebugState.CharacterPreviewBuildStatus = CharacterPreviewBuildStatusToString(ECharacterPreviewBuildStatus::Complete);
		CharacterSelectPreviewDebugState.CharacterPreviewBuildReason = TEXT("signature appearance cache asset loaded");
	}
	else if (Appearance.bUsesBaseRuntimeComposition)
	{
		CharacterSelectPreviewDebugState.CharacterPreviewBuildStatus = CharacterPreviewBuildStatusToString(ECharacterPreviewBuildStatus::Complete);
		CharacterSelectPreviewDebugState.CharacterPreviewBuildReason = TEXT("runtime CharacterAppearance texture library applied to base PlayerCharacters asset");
		UE_LOG(LogTemp, Display, TEXT("角色预览使用正式运行态外观解释器，跳过即时签名烘焙检查: guid=%d signature=%s baseMesh=%s equipment=%s"),
			Appearance.Guid,
			*AppearanceSignature,
			*Appearance.CharacterPreviewMeshPath,
			*Appearance.EquipmentSummary);
	}
	else
	{
		CharacterSelectPreviewDebugState.CharacterPreviewBuildStatus = CharacterPreviewBuildStatusToString(ECharacterPreviewBuildStatus::NeedsRebuild);
		CharacterSelectPreviewDebugState.CharacterPreviewBuildReason = TEXT("runtime appearance has no baked cache and no base runtime composition");
	}
	if (SelectedGlueCharacterModelKey.Equals(MeshObjectPath, ESearchCase::IgnoreCase) &&
		SelectedGlueCharacterAnimationKey.Equals(AnimationObjectPath, ESearchCase::IgnoreCase) &&
		SelectedGlueCharacterAppearanceKey.Equals(Appearance.CacheKey, ESearchCase::CaseSensitive) &&
		CharacterSelectPreviewActor.IsValid())
	{
		PositionCharacterSelectPreviewActor();
		return;
	}

	if (!PreviewMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览缺少生成资产: guid=%d race=%d gender=%d id=%s source=%s mesh=%s。请先运行 Config/WoWAssetManifests/PlayerCharacters.json。"),
			SelectedGlueCharacterGuid,
			Race,
			Gender,
			*PreviewSourceId,
			*PreviewSourceModelPath,
			*MeshObjectPath);
		DestroyCharacterSelectPreviewActor();
		ReportCharacterPreviewBuildIssue(
			ECharacterPreviewBuildStatus::MissingCriticalSource,
			TEXT("runtime_mesh_load"),
			Appearance,
			Definition,
			AppearanceSignature,
			CharacterPreviewStem,
			CharacterPreviewPackageRoot,
			TEXT("runtime could not load preview skeletal mesh"));
		return;
	}

	const bool bHoldVisibilityUntilRuntimeVisualReady = Appearance.bUsesBaseRuntimeComposition && !Appearance.bHasCharacterPreviewAsset;
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	const bool bUseStagingActor = bHoldVisibilityUntilRuntimeVisualReady && CharacterSelectPreviewActor.IsValid();
	if (bUseStagingActor)
	{
		CancelPreparedCharacterSelectPreviewStaging(TEXT("replace_staging_request"));

		CharacterSelectPreviewPreviousActorDuringStaging = CharacterSelectPreviewActor;
		ASkeletalMeshActor* StagingActor = World->SpawnActor<ASkeletalMeshActor>(
			ASkeletalMeshActor::StaticClass(),
			FVector::ZeroVector,
			FRotator::ZeroRotator);
		if (!StagingActor)
		{
			return;
		}
#if WITH_EDITOR
		StagingActor->SetActorLabel(TEXT("WoW_CharacterSelect_PlayerPreview_Staging"));
#endif
		StagingActor->SetActorHiddenInGame(true);
		CharacterSelectPreviewActor = StagingActor;
		UE_LOG(LogTemp, Display, TEXT("角色预览创建隐藏 staging actor，旧角色保持可见: oldGuid=%d newGuid=%d"),
			CharacterSelectPreviewDebugState.SelectedGuid,
			Appearance.Guid);
	}
	else if (!CharacterSelectPreviewActor.IsValid())
	{
		ASkeletalMeshActor* PreviewActor = World->SpawnActor<ASkeletalMeshActor>(
			ASkeletalMeshActor::StaticClass(),
			FVector::ZeroVector,
			FRotator::ZeroRotator);
		if (!PreviewActor)
		{
			return;
		}
#if WITH_EDITOR
		PreviewActor->SetActorLabel(TEXT("WoW_CharacterSelect_PlayerPreview"));
#endif
		CharacterSelectPreviewActor = PreviewActor;
	}

	ASkeletalMeshActor* PreviewActor = CharacterSelectPreviewActor.Get();
	USkeletalMeshComponent* MeshComponent = PreviewActor ? PreviewActor->GetSkeletalMeshComponent() : nullptr;
	if (!MeshComponent)
	{
		return;
	}

	const double ApplyStartSeconds = FPlatformTime::Seconds();
	if (bHoldVisibilityUntilRuntimeVisualReady)
	{
		MeshComponent->SetVisibility(false, true);
		MeshComponent->SetHiddenInGame(true, true);
	}
	const double ConfigureStartSeconds = FPlatformTime::Seconds();
	ConfigureCharacterSelectPreviewSkeletalComponent(MeshComponent);
	const double ConfigureElapsedMs = (FPlatformTime::Seconds() - ConfigureStartSeconds) * 1000.0;
	const double DestroyAttachmentsStartSeconds = FPlatformTime::Seconds();
	bPreparingCharacterSelectPreviewStaging = bUseStagingActor;
	DestroyCharacterSelectPreviewAttachments();
	bPreparingCharacterSelectPreviewStaging = false;
	const double DestroyAttachmentsElapsedMs = (FPlatformTime::Seconds() - DestroyAttachmentsStartSeconds) * 1000.0;
	const double ResetAnimationStartSeconds = FPlatformTime::Seconds();
	MeshComponent->SetAnimation(nullptr);
	const double ResetAnimationElapsedMs = (FPlatformTime::Seconds() - ResetAnimationStartSeconds) * 1000.0;
	const double ClearMeshStartSeconds = FPlatformTime::Seconds();
	MeshComponent->SetSkeletalMesh(nullptr);
	const double ClearMeshElapsedMs = (FPlatformTime::Seconds() - ClearMeshStartSeconds) * 1000.0;
	const double SetMeshStartSeconds = FPlatformTime::Seconds();
	MeshComponent->SetSkeletalMesh(PreviewMesh);
	const double SetMeshElapsedMs = (FPlatformTime::Seconds() - SetMeshStartSeconds) * 1000.0;
	const double ApplyMaterialsStartSeconds = FPlatformTime::Seconds();
	ApplyCharacterPreviewMaterials(MeshComponent, *Definition, Appearance, CharacterPreviewPackageRoot, CharacterPreviewStem, MeshObjectPath, Appearance.bHasCharacterPreviewAsset);
	SelectedGlueCharacterAppearanceKey = Appearance.CacheKey;
	QueueDeferredCharacterPreviewRuntimeMaterials(MeshComponent, *Definition, Appearance);
	const double ApplyMaterialsElapsedMs = (FPlatformTime::Seconds() - ApplyMaterialsStartSeconds) * 1000.0;
	const double SectionVisibilityStartSeconds = FPlatformTime::Seconds();
	ApplyCharacterPreviewSectionVisibility(MeshComponent, *Definition, Appearance);
	const double SectionVisibilityElapsedMs = (FPlatformTime::Seconds() - SectionVisibilityStartSeconds) * 1000.0;
	const double AttachmentsStartSeconds = FPlatformTime::Seconds();
	ApplyCharacterPreviewAttachments(MeshComponent, Appearance);
	const double AttachmentsElapsedMs = (FPlatformTime::Seconds() - AttachmentsStartSeconds) * 1000.0;
	const double ApplyElapsedMs = (FPlatformTime::Seconds() - ApplyStartSeconds) * 1000.0;
	const double SetAnimationModeStartSeconds = FPlatformTime::Seconds();
	MeshComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	const double SetAnimationModeElapsedMs = (FPlatformTime::Seconds() - SetAnimationModeStartSeconds) * 1000.0;
	const double AnimationLoadStartSeconds = FPlatformTime::Seconds();
	UAnimSequence* PreviewAnimation = ResolveLoadedObject<UAnimSequence>(AnimationObjectPath);
	const bool bInitialAnimationPlayable = IsCharacterPreviewAnimationPlayable(PreviewAnimation);
	const bool bInitialAnimationCompatible = PreviewAnimation &&
		IsCharacterPreviewAnimationCompatibleWithMesh(PreviewMesh, PreviewAnimation, MeshObjectPath, AnimationObjectPath, PreviewSourceId, false);
	if (PreviewAnimation && (!bInitialAnimationPlayable || !bInitialAnimationCompatible))
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览默认动作不可用，尝试切换 WoW idle fallback: guid=%d playable=%d compatible=%d frames=%d length=%.3f anim=%s mesh=%s"),
			SelectedGlueCharacterGuid,
			bInitialAnimationPlayable ? 1 : 0,
			bInitialAnimationCompatible ? 1 : 0,
			PreviewAnimation->GetNumberOfSampledKeys(),
			PreviewAnimation->GetPlayLength(),
			*AnimationObjectPath,
			*MeshObjectPath);
	}
	if (!PreviewAnimation || !bInitialAnimationPlayable || !bInitialAnimationCompatible)
	{
		TArray<FString> FallbackAnimationPaths;
		CollectCharacterPreviewIdleFallbackAnimationPaths(AnimationObjectPath, Appearance.EquipmentSummary, FallbackAnimationPaths);
		CollectCharacterPreviewIdleFallbackAnimationPaths(DefaultAnimationObjectPath, Appearance.EquipmentSummary, FallbackAnimationPaths);
		CollectCharacterPreviewIdleFallbackAnimationPaths(CharacterPreviewLookupAnimationPath, Appearance.EquipmentSummary, FallbackAnimationPaths);
		CollectCharacterPreviewIdleFallbackAnimationPaths(Definition->LookupAnimationPath, Appearance.EquipmentSummary, FallbackAnimationPaths);
		CollectCharacterPreviewIdleFallbackAnimationPaths(Definition->StandAnimationPath, Appearance.EquipmentSummary, FallbackAnimationPaths);
		for (const FString& FallbackAnimationPath : FallbackAnimationPaths)
		{
			if (TryResolvePlayableCharacterPreviewAnimation(
				PreviewMesh,
				MeshObjectPath,
				PreviewSourceId,
				FallbackAnimationPath,
				PreviewAnimation,
				AnimationObjectPath))
			{
				UE_LOG(LogTemp, Display, TEXT("角色预览 idle fallback 动作已选择: guid=%d anim=%s mesh=%s"),
					SelectedGlueCharacterGuid,
					*AnimationObjectPath,
					*MeshObjectPath);
				break;
			}
		}
	}
	if (!PreviewAnimation && OverrideAnimationId != INDEX_NONE)
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览默认动作覆盖加载失败，回退 Stand: override=%s fallback=%s"),
			*AnimationObjectPath,
			*DefaultAnimationObjectPath);
		PreviewAnimation = ResolveLoadedObject<UAnimSequence>(DefaultAnimationObjectPath);
		AnimationObjectPath = DefaultAnimationObjectPath;
	}
	const double AnimationLoadElapsedMs = (FPlatformTime::Seconds() - AnimationLoadStartSeconds) * 1000.0;
	bool bPreviewAnimationApplied = false;
	if (PreviewAnimation)
	{
		bPreviewAnimationApplied = PlayCharacterSelectPreviewAnimation(MeshComponent, PreviewAnimation, PreviewSourceId, AnimationObjectPath, MeshObjectPath);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览 Stand 动画加载失败: guid=%d race=%d gender=%d id=%s animation=%s mesh=%s"),
		SelectedGlueCharacterGuid,
		Race,
		Gender,
		*PreviewSourceId,
		*AnimationObjectPath,
		*MeshObjectPath);
	}
	if (!bPreviewAnimationApplied)
	{
		const bool bHadPreviousPreviewActor = CharacterSelectPreviewPreviousActorDuringStaging.IsValid();
		UE_LOG(LogTemp, Warning, TEXT("角色预览动作未就绪，拒绝提交静止/T-pose 半成品: guid=%d race=%d gender=%d animation=%s mesh=%s stagingPrevious=%d"),
			SelectedGlueCharacterGuid,
			Race,
			Gender,
			*AnimationObjectPath,
			*MeshObjectPath,
			bHadPreviousPreviewActor ? 1 : 0);
		CancelPreparedCharacterSelectPreviewStaging(TEXT("animation_not_ready"));
		if (!bHadPreviousPreviewActor && CharacterSelectPreviewActor.IsValid())
		{
			if (USkeletalMeshComponent* CurrentMeshComponent = CharacterSelectPreviewActor->GetSkeletalMeshComponent())
			{
				CurrentMeshComponent->SetVisibility(false, true);
				CurrentMeshComponent->SetHiddenInGame(true, true);
			}
			CharacterSelectPreviewActor->SetActorHiddenInGame(true);
		}
		return;
	}
	SelectedGlueCharacterModelKey = MeshObjectPath;
	SelectedGlueCharacterAnimationKey = AnimationObjectPath;
	SelectedGlueCharacterAppearanceKey = Appearance.CacheKey;
	if (bHoldVisibilityUntilRuntimeVisualReady)
	{
		PendingCharacterSelectPreviewRevealGuid = Appearance.Guid;
		PendingCharacterSelectPreviewRevealAppearanceKey = Appearance.CacheKey;
		UE_LOG(LogTemp, Display, TEXT("角色预览等待运行态视觉完成后统一显示: guid=%d baseRuntime=%d materialPending=%d attachmentPending=%d key=%s"),
			Appearance.Guid,
			Appearance.bUsesBaseRuntimeComposition ? 1 : 0,
			PendingCharacterPreviewRuntimeMaterialKey.IsEmpty() ? 0 : 1,
			PendingCharacterSelectPreviewM2AttachmentKey.IsEmpty() ? 0 : 1,
			*Appearance.CacheKey);
	}
	else
	{
		PendingCharacterSelectPreviewRevealGuid = 0;
		PendingCharacterSelectPreviewRevealAppearanceKey.Empty();
		MeshComponent->SetVisibility(true, true);
		MeshComponent->SetHiddenInGame(false, true);
	}
	CharacterSelectPreviewDebugState.SelectedGuid = SelectedGlueCharacterGuid;
	CharacterSelectPreviewDebugState.Race = Race;
	CharacterSelectPreviewDebugState.ClassId = Appearance.ClassId;
	CharacterSelectPreviewDebugState.Gender = Gender;
	CharacterSelectPreviewDebugState.Skin = Appearance.Skin;
	CharacterSelectPreviewDebugState.Face = Appearance.Face;
	CharacterSelectPreviewDebugState.HairStyle = Appearance.HairStyle;
	CharacterSelectPreviewDebugState.HairColor = Appearance.HairColor;
	CharacterSelectPreviewDebugState.FacialStyle = Appearance.FacialStyle;
	CharacterSelectPreviewDebugState.DefinitionId = Definition->Id;
	CharacterSelectPreviewDebugState.DisplayName = Definition->DisplayName;
	CharacterSelectPreviewDebugState.MeshPath = MeshObjectPath;
	CharacterSelectPreviewDebugState.StandAnimationPath = DefaultAnimationObjectPath;
	CharacterSelectPreviewDebugState.ActiveAnimationPath = AnimationObjectPath;
	CharacterSelectPreviewDebugState.EquipmentSummary = Appearance.EquipmentSummary;
	CharacterSelectPreviewDebugState.AppearanceDescriptorPath = Appearance.AppearanceDescriptorPath;
	CharacterSelectPreviewDebugState.CharacterPreviewMeshPath = Appearance.CharacterPreviewMeshPath;
	CharacterSelectPreviewDebugState.CharacterPreviewAnimationPath = Appearance.CharacterPreviewAnimationPath;
	CharacterSelectPreviewDebugState.CharacterPreviewLookupAnimationPath = CharacterPreviewLookupAnimationPath;
	CharacterSelectPreviewDebugState.BaseLookupAnimationPath = Definition->LookupAnimationPath;
	CharacterSelectPreviewDebugState.CharacterPreviewSectionMetadataPath = Appearance.CharacterPreviewSectionMetadataPath;
	CharacterSelectPreviewDebugState.AppearanceCacheKey = Appearance.CacheKey;
	if (Appearance.bHasCharacterPreviewAsset)
	{
		CharacterSelectPreviewDebugState.CharacterPreviewBuildStatus = CharacterPreviewBuildStatusToString(ECharacterPreviewBuildStatus::Complete);
		CharacterSelectPreviewDebugState.CharacterPreviewBuildReason = TEXT("signature appearance cache asset loaded");
		CharacterSelectPreviewDebugState.CharacterPreviewBuildLogPath.Empty();
	}
	CachedCharacterPreviewAnimationOptionsKey.Empty();
	CachedCharacterPreviewAnimationOptions.Reset();
	CachedCharacterPreviewAnimationPaths.Reset();
	CachedCharacterPreviewAnimationPathById.Reset();
	CachedCharacterPreviewImportedAnimationIds.Reset();
	CachedCharacterPreviewAnimationCatalogState = FCharacterPreviewAnimationCatalogState();
	++CharacterPreviewAnimationCatalogRequestSerial;
	CharacterSelectPreviewDebugState.bHasAppearanceDescriptor = Appearance.bHasAppearanceDescriptor;
	CharacterSelectPreviewDebugState.bHasCharacterPreviewAsset = Appearance.bHasCharacterPreviewAsset;
	CharacterSelectPreviewDebugState.EquipmentEntryCount = Appearance.EquipmentEntryCount;
	PositionCharacterSelectPreviewActor();
	if (bHoldVisibilityUntilRuntimeVisualReady)
	{
		TryRevealPreparedCharacterSelectPreview(TEXT("refresh_end"));
	}
	QueueCurrentCharacterPreviewAnimationOptionsPrewarm();

	UE_LOG(LogTemp, Display, TEXT("角色选择预览模型已切换: guid=%d race=%d gender=%d skin=%d face=%d hairStyle=%d hairColor=%d facial=%d equipmentEntries=%d descriptor=%d appearanceCache=%d descriptorPath=%s equipment=%s id=%s source=%s mesh=%s"),
		SelectedGlueCharacterGuid,
		Race,
		Gender,
		Appearance.Skin,
		Appearance.Face,
		Appearance.HairStyle,
		Appearance.HairColor,
		Appearance.FacialStyle,
		Appearance.EquipmentEntryCount,
		Appearance.bHasAppearanceDescriptor ? 1 : 0,
		Appearance.bHasCharacterPreviewAsset ? 1 : 0,
		*Appearance.AppearanceDescriptorPath,
		*Appearance.EquipmentSummary,
		*PreviewSourceId,
		*PreviewSourceModelPath,
		*MeshObjectPath);
	UE_LOG(LogTemp, Display, TEXT("Character preview refresh completed: guid=%d elapsed=%.2fms mesh=%s animation=%s"),
		SelectedGlueCharacterGuid,
		(FPlatformTime::Seconds() - RefreshStartSeconds) * 1000.0,
		*MeshObjectPath,
		*AnimationObjectPath);
	UE_LOG(LogTemp, Display, TEXT("Character preview refresh profile: guid=%d resolve=%.2fms meshLoadAt=%.2fms apply=%.2fms animLoad=%.2fms total=%.2fms baseRuntime=%d appearanceCache=%d"),
		SelectedGlueCharacterGuid,
		ResolveElapsedMs,
		MeshLoadedElapsedMs,
		ApplyElapsedMs,
		AnimationLoadElapsedMs,
		(FPlatformTime::Seconds() - RefreshStartSeconds) * 1000.0,
		Appearance.bUsesBaseRuntimeComposition ? 1 : 0,
		Appearance.bHasCharacterPreviewAsset ? 1 : 0);
	UE_LOG(LogTemp, Display, TEXT("Character preview apply profile: guid=%d configure=%.2fms destroyAttachments=%.2fms resetAnim=%.2fms clearMesh=%.2fms setMesh=%.2fms materials=%.2fms sectionVisibility=%.2fms attachments=%.2fms setAnimMode=%.2fms applyTotal=%.2fms"),
		SelectedGlueCharacterGuid,
		ConfigureElapsedMs,
		DestroyAttachmentsElapsedMs,
		ResetAnimationElapsedMs,
		ClearMeshElapsedMs,
		SetMeshElapsedMs,
		ApplyMaterialsElapsedMs,
		SectionVisibilityElapsedMs,
		AttachmentsElapsedMs,
		SetAnimationModeElapsedMs,
		ApplyElapsedMs);
}

bool AWoWLoginGameMode::IsGlueSceneVisualReadyForScreen(const FString& TargetScreen) const
{
	int32 ExpectedSceneIndex = INDEX_NONE;
	if (TargetScreen.Equals(TEXT("login"), ESearchCase::IgnoreCase))
	{
		ExpectedSceneIndex = LoginScreenSceneIndex;
	}
	else if (TargetScreen.Equals(TEXT("charselect"), ESearchCase::IgnoreCase) ||
		TargetScreen.Equals(TEXT("charcreate"), ESearchCase::IgnoreCase))
	{
		TryResolveCharacterSelectSceneIndex(ExpectedSceneIndex);
	}
	else
	{
		return true;
	}

	if (LoginSceneDefinitions.IsValidIndex(ExpectedSceneIndex) && ExpectedSceneIndex != ActiveLoginSceneIndex)
	{
		return false;
	}
	if (!LoginSceneActor.IsValid())
	{
		return false;
	}
	if (!LoginCameraActor.IsValid())
	{
		return false;
	}

	const ASkeletalMeshActor* SceneActor = LoginSceneActor.Get();
	const USkeletalMeshComponent* MeshComponent = SceneActor ? SceneActor->GetSkeletalMeshComponent() : nullptr;
	return MeshComponent &&
		MeshComponent->GetSkeletalMeshAsset() &&
		MeshComponent->IsVisible() &&
		!MeshComponent->bHiddenInGame;
}

bool AWoWLoginGameMode::IsSelectedCharacterPreviewPreparedReady() const
{
	if (SelectedGlueCharacterGuid <= 0)
	{
		return false;
	}
	const FString ExpectedGuidPrefix = FString::Printf(TEXT("guid=%d|"), SelectedGlueCharacterGuid);
	if (!PreparedCharacterPreviewLoadKey.IsEmpty() &&
		PreparedCharacterPreviewLoadKey.Equals(ActiveCharacterPreviewLoadKey, ESearchCase::CaseSensitive) &&
		PreparedCharacterPreviewLoadKey.StartsWith(ExpectedGuidPrefix, ESearchCase::CaseSensitive) &&
		(!ActiveCharacterPreviewStreamableHandle.IsValid() || !ActiveCharacterPreviewStreamableHandle->IsLoadingInProgress()))
	{
		return true;
	}

	FString SelectedRow;
	for (const FString& Row : CachedGlueCharacterRows)
	{
		TArray<FString> Columns;
		Row.ParseIntoArray(Columns, TEXT("\t"), false);
		if (Columns.Num() >= 5 && FCString::Atoi(*Columns[0]) == SelectedGlueCharacterGuid)
		{
			SelectedRow = Row;
			break;
		}
	}
	if (SelectedRow.IsEmpty())
	{
		return false;
	}

	TArray<FString> Columns;
	SelectedRow.ParseIntoArray(Columns, TEXT("\t"), false);
	FCharacterSelectPreviewAppearance Appearance;
	const FPlayerCharacterPreviewDefinition* Definition = nullptr;
	FString AppearanceSignature;
	FString CharacterPreviewStem;
	FString CharacterPreviewPackageRoot;
	FString CharacterPreviewLookupAnimationPath;
	if (!ResolveCharacterPreviewRuntimeAssets(Columns, Appearance, Definition, AppearanceSignature, CharacterPreviewStem, CharacterPreviewPackageRoot, CharacterPreviewLookupAnimationPath))
	{
		return false;
	}

	const FString RequestedAnimationPath = ResolveCharacterPreviewAnimationObjectPath(Appearance.CharacterPreviewAnimationPath, Appearance.EquipmentSummary);
	const FString RequestKey = WoWCharacterVisualKit::BuildPreviewRequestKey(
		SelectedGlueCharacterGuid,
		Appearance.CacheKey,
		Appearance.CharacterPreviewMeshPath,
		RequestedAnimationPath,
		Appearance.CharacterPreviewSectionMetadataPath,
		Appearance.CharacterPreviewAttachmentDescriptorPath);

	return PreparedCharacterPreviewLoadKey.Equals(RequestKey, ESearchCase::CaseSensitive);
}

bool AWoWLoginGameMode::IsSelectedCharacterPreviewVisualReady() const
{
	if (!IsSelectedCharacterPreviewPreparedReady() || !CharacterSelectPreviewActor.IsValid())
	{
		return false;
	}
	if (PendingCharacterSelectPreviewRevealGuid == SelectedGlueCharacterGuid &&
		!PendingCharacterSelectPreviewRevealAppearanceKey.IsEmpty())
	{
		return false;
	}

	const ASkeletalMeshActor* PreviewActor = CharacterSelectPreviewActor.Get();
	const USkeletalMeshComponent* MeshComponent = PreviewActor ? PreviewActor->GetSkeletalMeshComponent() : nullptr;
	if (!MeshComponent || !MeshComponent->GetSkeletalMeshAsset() || !MeshComponent->IsVisible() || MeshComponent->bHiddenInGame)
	{
		return false;
	}

	FString SelectedRow;
	for (const FString& Row : CachedGlueCharacterRows)
	{
		TArray<FString> Columns;
		Row.ParseIntoArray(Columns, TEXT("\t"), false);
		if (Columns.Num() >= 5 && FCString::Atoi(*Columns[0]) == SelectedGlueCharacterGuid)
		{
			SelectedRow = Row;
			break;
		}
	}
	if (SelectedRow.IsEmpty())
	{
		return false;
	}

	TArray<FString> Columns;
	SelectedRow.ParseIntoArray(Columns, TEXT("\t"), false);
	FCharacterSelectPreviewAppearance Appearance;
	const FPlayerCharacterPreviewDefinition* Definition = nullptr;
	FString AppearanceSignature;
	FString CharacterPreviewStem;
	FString CharacterPreviewPackageRoot;
	FString CharacterPreviewLookupAnimationPath;
	if (!ResolveCharacterPreviewRuntimeAssets(Columns, Appearance, Definition, AppearanceSignature, CharacterPreviewStem, CharacterPreviewPackageRoot, CharacterPreviewLookupAnimationPath))
	{
		return false;
	}

	return SelectedGlueCharacterModelKey.Equals(Appearance.CharacterPreviewMeshPath, ESearchCase::IgnoreCase) &&
		SelectedGlueCharacterAppearanceKey.Equals(Appearance.CacheKey, ESearchCase::CaseSensitive);
}

void AWoWLoginGameMode::StoreSelectedCharacterRuntimeAppearance(const FCharacterSelectPreviewAppearance& Appearance)
{
	UWoWGameInstance* WoWGameInstance = GetGameInstance<UWoWGameInstance>();
	if (!WoWGameInstance || Appearance.Guid <= 0)
	{
		return;
	}

	FWoWSelectedCharacterRuntimeAppearance RuntimeAppearance;
	RuntimeAppearance.Guid = Appearance.Guid;
	RuntimeAppearance.CharacterName = Appearance.CharacterName;
	RuntimeAppearance.Race = Appearance.Race;
	RuntimeAppearance.ClassId = Appearance.ClassId;
	RuntimeAppearance.Gender = Appearance.Gender;
	RuntimeAppearance.Skin = Appearance.Skin;
	RuntimeAppearance.Face = Appearance.Face;
	RuntimeAppearance.HairStyle = Appearance.HairStyle;
	RuntimeAppearance.HairColor = Appearance.HairColor;
	RuntimeAppearance.FacialStyle = Appearance.FacialStyle;
	RuntimeAppearance.EquipmentSummary = Appearance.EquipmentSummary;
	RuntimeAppearance.AppearanceKey = Appearance.CacheKey;
	RuntimeAppearance.CharacterPreviewMeshPath = Appearance.CharacterPreviewMeshPath;
	RuntimeAppearance.CharacterPreviewMeshJsonPath = Appearance.CharacterPreviewMeshJsonPath;
	RuntimeAppearance.CharacterPreviewMaterialPackagePath = Appearance.CharacterPreviewMaterialPackagePath;
	RuntimeAppearance.CharacterPreviewAnimationPath = Appearance.CharacterPreviewAnimationPath;
	RuntimeAppearance.CharacterPreviewAnimationPackagePath = Appearance.CharacterPreviewAnimationPackagePath;
	RuntimeAppearance.CharacterPreviewAnimationStem = Appearance.CharacterPreviewAnimationStem;
	RuntimeAppearance.CharacterPreviewSectionMetadataPath = Appearance.CharacterPreviewSectionMetadataPath;
	RuntimeAppearance.CharacterPreviewAttachmentDescriptorPath = Appearance.CharacterPreviewAttachmentDescriptorPath;
	RuntimeAppearance.bHasCharacterPreviewAsset = Appearance.bHasCharacterPreviewAsset;
	RuntimeAppearance.bUsesBaseRuntimeComposition = Appearance.bUsesBaseRuntimeComposition;
	WoWGameInstance->SetSelectedCharacterRuntimeAppearance(RuntimeAppearance);
}

bool AWoWLoginGameMode::ApplyCharacterPreviewAnimationOnly(const FString& AnimationObjectPath)
{
	int32 AnimationId = INDEX_NONE;
	if (!AnimationObjectPath.TrimStartAndEnd().IsEmpty())
	{
		TryParseCharacterPreviewAnimationIdFromObjectPath(AnimationObjectPath, AnimationId);
	}
	return ApplyCharacterPreviewAnimationOnlyById(AnimationId);
}

bool AWoWLoginGameMode::ApplyCharacterPreviewAnimationOnlyById(int32 AnimationId)
{
	if (!CharacterSelectPreviewActor.IsValid())
	{
		return false;
	}

	ASkeletalMeshActor* PreviewActor = CharacterSelectPreviewActor.Get();
	USkeletalMeshComponent* MeshComponent = PreviewActor ? PreviewActor->GetSkeletalMeshComponent() : nullptr;
	if (!MeshComponent || !MeshComponent->GetSkeletalMeshAsset() || SelectedGlueCharacterModelKey.IsEmpty())
	{
		return false;
	}

	FString ResolvedAnimationPath;
	if (AnimationId == INDEX_NONE)
	{
		ResolvedAnimationPath = CharacterSelectPreviewDebugState.StandAnimationPath;
		TryParseCharacterPreviewAnimationIdFromObjectPath(ResolvedAnimationPath, AnimationId);
	}
	if (AnimationId == INDEX_NONE)
	{
		return false;
	}

	if (!TryGetResolvedCharacterPreviewAnimationPath(SelectedGlueCharacterModelKey, BuildCharacterPreviewAnimationOptionsCacheKey(), AnimationId, ResolvedAnimationPath))
	{
		return false;
	}

	UAnimSequence* PreviewAnimation = ResolveLoadedObject<UAnimSequence>(ResolvedAnimationPath);
	if (!PreviewAnimation)
	{
		MissingCharacterSelectPreviewAnimationPaths.Add(ResolvedAnimationPath);
		CacheMissingResolvedCharacterPreviewAnimation(SelectedGlueCharacterModelKey, BuildCharacterPreviewAnimationOptionsCacheKey(), AnimationId);
		return false;
	}

	MeshComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
	PlayCharacterSelectPreviewAnimation(
		MeshComponent,
		PreviewAnimation,
		CharacterSelectPreviewDebugState.DefinitionId,
		ResolvedAnimationPath,
		SelectedGlueCharacterModelKey,
		false,
		false);

	for (TWeakObjectPtr<USkeletalMeshComponent>& AttachmentComponentPtr : CharacterSelectPreviewAttachmentComponents)
	{
		USkeletalMeshComponent* AttachmentComponent = AttachmentComponentPtr.Get();
		if (!AttachmentComponent || !AttachmentComponent->GetSkeletalMeshAsset())
		{
			continue;
		}

		const FString CurrentAttachmentAnimationPath = CharacterSelectPreviewAttachmentAnimationKeys.FindRef(AttachmentComponentPtr);
		if (CurrentAttachmentAnimationPath.IsEmpty())
		{
			continue;
		}

		FString AttachmentAnimationPath = CurrentAttachmentAnimationPath;
		int32 AttachmentAnimationId = INDEX_NONE;
		if (TryParseCharacterPreviewAnimationIdFromObjectPath(CurrentAttachmentAnimationPath, AttachmentAnimationId) && AttachmentAnimationId != AnimationId)
		{
			FString PackagePath;
			FString AssetName;
			if (CurrentAttachmentAnimationPath.Split(TEXT("."), &PackagePath, &AssetName, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
			{
				const FString PaddedAnimationId = FString::Printf(TEXT("_%03d_"), AnimationId);
				FString CandidateAssetName = AssetName;
				CandidateAssetName = CandidateAssetName.Replace(*FString::Printf(TEXT("_%03d_"), AttachmentAnimationId), *PaddedAnimationId, ESearchCase::CaseSensitive);
				const FString CandidateAnimationPath = PackagePath + TEXT(".") + CandidateAssetName;
				if (!MissingCharacterSelectPreviewAnimationPaths.Contains(CandidateAnimationPath))
				{
					if (ResolveLoadedObject<UAnimSequence>(CandidateAnimationPath))
					{
						AttachmentAnimationPath = CandidateAnimationPath;
					}
					else
					{
						MissingCharacterSelectPreviewAnimationPaths.Add(CandidateAnimationPath);
					}
				}
			}
		}

		if (AttachmentAnimationPath.Equals(CurrentAttachmentAnimationPath, ESearchCase::IgnoreCase) && SelectedGlueCharacterAnimationKey.Equals(ResolvedAnimationPath, ESearchCase::IgnoreCase))
		{
			continue;
		}

		if (UAnimSequence* AttachmentAnimation = ResolveLoadedObject<UAnimSequence>(AttachmentAnimationPath))
		{
			AttachmentComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
			PlayCharacterSelectPreviewAnimation(
				AttachmentComponent,
				AttachmentAnimation,
				AttachmentComponent->GetName(),
				AttachmentAnimationPath,
				AttachmentComponent->GetSkeletalMeshAsset()->GetPathName(),
				false,
				false);
			CharacterSelectPreviewAttachmentAnimationKeys.FindOrAdd(AttachmentComponentPtr) = AttachmentAnimationPath;
		}
		else
		{
			MissingCharacterSelectPreviewAnimationPaths.Add(AttachmentAnimationPath);
		}
	}

	SelectedGlueCharacterAnimationKey = ResolvedAnimationPath;
	CharacterSelectPreviewDebugState.ActiveAnimationPath = ResolvedAnimationPath;
	PositionCharacterSelectPreviewActor();
	return true;
}

void AWoWLoginGameMode::PositionCharacterSelectPreviewActor()
{
	if (!CharacterSelectPreviewActor.IsValid() || !LoginCameraActor.IsValid())
	{
		return;
	}

	ASkeletalMeshActor* PreviewActor = CharacterSelectPreviewActor.Get();
	USkeletalMeshComponent* MeshComponent = PreviewActor ? PreviewActor->GetSkeletalMeshComponent() : nullptr;
	if (!PreviewActor || !MeshComponent || !MeshComponent->GetSkeletalMeshAsset())
	{
		return;
	}

	const FVector CameraLocation = LoginCameraActor->GetActorLocation();
	const FRotator CameraRotation = LoginCameraActor->GetActorRotation();
	const FVector CameraForward = CameraRotation.Vector();
	const FVector CameraRight = CameraRotation.RotateVector(FVector::RightVector);
	const FVector CameraUp = CameraRotation.RotateVector(FVector::UpVector);
	const FBoxSphereBounds Bounds = MeshComponent->GetSkeletalMeshAsset()->GetBounds();
	const float MeshHeight = FMath::Max(Bounds.BoxExtent.Z * 2.0f, 1.0f);
	const float MeshRadius = FMath::Max(Bounds.SphereRadius, 1.0f);

	// 角色选择人物属于 Glue 前景预览层：位置跟随当前 M2 相机，而不是写死到某个种族背景世界坐标。
	const float DesiredHeight = 185.0f;
	const float PreviewScale = FMath::Clamp(DesiredHeight / MeshHeight, 0.25f, 6.0f);
	const float Distance = FMath::Max(MeshRadius * PreviewScale * 3.0f, 360.0f);
	const FVector ActorLocation =
		CameraLocation +
		CameraForward * Distance -
		CameraRight * 75.0f -
		CameraUp * 115.0f;

	FRotator ActorRotation = (CameraLocation - ActorLocation).Rotation();
	ActorRotation.Pitch = 0.0f;
	ActorRotation.Roll = 0.0f;

	const FVector FinalLocation = ActorLocation + LoginSceneTuningState.CharacterPreviewLocationOffset;
	const FRotator FinalRotation = ActorRotation + LoginSceneTuningState.CharacterPreviewRotationOffset + FRotator(0.0f, CharacterSelectPreviewUserYawOffset, 0.0f);
	const FVector FinalScale = FVector(PreviewScale * LoginSceneTuningState.CharacterPreviewUniformScale) * LoginSceneTuningState.CharacterPreviewScaleMultiplier;
	PreviewActor->SetActorLocationAndRotation(FinalLocation, FinalRotation);
	PreviewActor->SetActorScale3D(FinalScale);

	CharacterSelectPreviewDebugState.bHasPreviewActor = true;
	CharacterSelectPreviewDebugState.BaseLocation = ActorLocation;
	CharacterSelectPreviewDebugState.BaseRotation = ActorRotation;
	CharacterSelectPreviewDebugState.BaseScale = PreviewScale;
	CharacterSelectPreviewDebugState.FinalLocation = FinalLocation;
	CharacterSelectPreviewDebugState.FinalRotation = FinalRotation;
	CharacterSelectPreviewDebugState.FinalScale = FinalScale;
}

void AWoWLoginGameMode::CancelPreparedCharacterSelectPreviewStaging(const TCHAR* Reason)
{
	ASkeletalMeshActor* PreviousActor = CharacterSelectPreviewPreviousActorDuringStaging.Get();
	ASkeletalMeshActor* CurrentActor = CharacterSelectPreviewActor.Get();
	if (!PreviousActor)
	{
		return;
	}

	AActor* AbandonedStagingActor = (CurrentActor && CurrentActor != PreviousActor) ? CurrentActor : nullptr;
	if (AbandonedStagingActor)
	{
		for (int32 SystemIndex = CharacterSelectPreviewAttachmentParticleSystems.Num() - 1; SystemIndex >= 0; --SystemIndex)
		{
			FLoginParticleSystem& System = CharacterSelectPreviewAttachmentParticleSystems[SystemIndex];
			USkeletalMeshComponent* SourceComponent = System.SourceMeshComponent.Get();
			if (SourceComponent && SourceComponent->GetOwner() == AbandonedStagingActor)
			{
				if (System.MeshComponent)
				{
					System.MeshComponent->DestroyComponent();
				}
				CharacterSelectPreviewAttachmentParticleSystems.RemoveAtSwap(SystemIndex, 1, EAllowShrinking::No);
			}
		}
		for (int32 ComponentIndex = CharacterSelectPreviewAttachmentComponents.Num() - 1; ComponentIndex >= 0; --ComponentIndex)
		{
			USkeletalMeshComponent* AttachmentComponent = CharacterSelectPreviewAttachmentComponents[ComponentIndex].Get();
			if (!AttachmentComponent || AttachmentComponent->GetOwner() == AbandonedStagingActor)
			{
				if (AttachmentComponent)
				{
					AttachmentComponent->DestroyComponent();
				}
				CharacterSelectPreviewAttachmentComponents.RemoveAtSwap(ComponentIndex, 1, EAllowShrinking::No);
			}
		}
		for (int32 ComponentIndex = CharacterSelectPreviewM2AttachmentComponents.Num() - 1; ComponentIndex >= 0; --ComponentIndex)
		{
			UWoWM2ModelComponent* AttachmentComponent = CharacterSelectPreviewM2AttachmentComponents[ComponentIndex].Get();
			if (!AttachmentComponent || AttachmentComponent->GetOwner() == AbandonedStagingActor)
			{
				if (AttachmentComponent)
				{
					AttachmentComponent->DestroyComponent();
				}
				CharacterSelectPreviewM2AttachmentComponents.RemoveAtSwap(ComponentIndex, 1, EAllowShrinking::No);
			}
		}
		AbandonedStagingActor->Destroy();
	}

	CharacterSelectPreviewActor = PreviousActor;
	CharacterSelectPreviewPreviousActorDuringStaging.Reset();
	PendingCharacterPreviewRuntimeMaterialKey.Empty();
	PendingCharacterPreviewRuntimeMaterialTask = FPendingCharacterPreviewRuntimeMaterialTask();
	PendingCharacterSelectPreviewM2AttachmentKey.Empty();
	PendingCharacterSelectPreviewM2Attachments.Reset();
	PendingCharacterSelectPreviewM2AttachmentDescriptorPath.Empty();
	PendingCharacterSelectPreviewM2AttachmentDefinitionCount = 0;
	PendingCharacterSelectPreviewM2AttachmentAttachedCount = 0;
	PendingCharacterSelectPreviewM2AttachmentStartSeconds = 0.0;
	PendingCharacterSelectPreviewRevealGuid = 0;
	PendingCharacterSelectPreviewRevealAppearanceKey.Empty();
	CharacterSelectPreviewAttachmentKey.Empty();
	bPreparingCharacterSelectPreviewStaging = false;

	PreviousActor->SetActorHiddenInGame(false);
	if (USkeletalMeshComponent* PreviousMeshComponent = PreviousActor->GetSkeletalMeshComponent())
	{
		PreviousMeshComponent->SetVisibility(true, true);
		PreviousMeshComponent->SetHiddenInGame(false, true);
	}
	PositionCharacterSelectPreviewActor();
	UE_LOG(LogTemp, Display, TEXT("角色预览取消未完成 staging，恢复旧可见角色: selectedGuid=%d reason=%s abandoned=%d"),
		SelectedGlueCharacterGuid,
		Reason ? Reason : TEXT("<none>"),
		AbandonedStagingActor ? 1 : 0);
}

void AWoWLoginGameMode::DestroyCharacterSelectPreviewActor()
{
	PendingCharacterPreviewRuntimeMaterialKey.Empty();
	PendingCharacterPreviewRuntimeMaterialTask = FPendingCharacterPreviewRuntimeMaterialTask();
	DestroyCharacterSelectPreviewAttachments();
	bCharacterSelectPreviewDragging = false;
	CharacterSelectPreviewUserYawOffset = 0.0f;
	PendingCharacterPreviewAnimationOptionsPrewarmKey.Empty();
	ResetCharacterPreviewAnimationCatalogBuild();
	CachedCharacterPreviewAnimationOptionsKey.Empty();
	CachedCharacterPreviewAnimationOptions.Reset();
	CachedCharacterPreviewAnimationPaths.Reset();
	CachedCharacterPreviewAnimationPathById.Reset();
	CachedCharacterPreviewImportedAnimationIds.Reset();
	CachedCharacterPreviewAnimationCatalogState = FCharacterPreviewAnimationCatalogState();
	CachedCharacterPreviewResolvedAnimationPaths.Reset();
	MissingCharacterPreviewResolvedAnimationKeys.Reset();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(CharacterPreviewAnimationOptionsPrewarmTimerHandle);
	}
	if (CharacterSelectPreviewActor.IsValid())
	{
		CharacterSelectPreviewActor->Destroy();
		CharacterSelectPreviewActor.Reset();
	}
	if (CharacterSelectPreviewPreviousActorDuringStaging.IsValid())
	{
		CharacterSelectPreviewPreviousActorDuringStaging->Destroy();
		CharacterSelectPreviewPreviousActorDuringStaging.Reset();
	}
	SelectedGlueCharacterModelKey.Empty();
	SelectedGlueCharacterAnimationKey.Empty();
	SelectedGlueCharacterAppearanceKey.Empty();
	PendingCharacterSelectPreviewRevealGuid = 0;
	PendingCharacterSelectPreviewRevealAppearanceKey.Empty();
	bPreparingCharacterSelectPreviewStaging = false;
	MissingCharacterSelectPreviewAnimationPaths.Reset();
	CharacterSelectPreviewDebugState = FCharacterSelectPreviewDebugState();
}

void AWoWLoginGameMode::QueueDeferredCharacterPreviewRuntimeMaterials(USkeletalMeshComponent* MeshComponent, const FPlayerCharacterPreviewDefinition& Definition, const FCharacterSelectPreviewAppearance& Appearance)
{
	PendingCharacterPreviewRuntimeMaterialKey.Empty();
	PendingCharacterPreviewRuntimeMaterialTask = FPendingCharacterPreviewRuntimeMaterialTask();
	if (!MeshComponent || Appearance.Guid <= 0 || Appearance.bHasCharacterPreviewAsset || !Appearance.bUsesBaseRuntimeComposition)
	{
		return;
	}

	const FString MaterialKey = FString::Printf(
		TEXT("%s|mesh=%s|runtimeMaterials"),
		*Appearance.CacheKey,
		MeshComponent->GetSkeletalMeshAsset() ? *MeshComponent->GetSkeletalMeshAsset()->GetPathName() : TEXT("<null>"));
	PendingCharacterPreviewRuntimeMaterialKey = MaterialKey;
	PendingCharacterPreviewRuntimeMaterialTask.Guid = Appearance.Guid;
	PendingCharacterPreviewRuntimeMaterialTask.MaterialKey = MaterialKey;
	PendingCharacterPreviewRuntimeMaterialTask.Appearance = Appearance;
	PendingCharacterPreviewRuntimeMaterialTask.SectionMetadataPath = (!Appearance.CharacterPreviewSectionMetadataPath.IsEmpty() && FPaths::FileExists(Appearance.CharacterPreviewSectionMetadataPath))
		? Appearance.CharacterPreviewSectionMetadataPath
		: Definition.SectionMetadataPath;
	PendingCharacterPreviewRuntimeMaterialTask.StartSeconds = FPlatformTime::Seconds();
	UE_LOG(LogTemp, Display, TEXT("角色预览运行时外观纹理已进入分帧队列: guid=%d key=%s metadata=%s"),
		Appearance.Guid,
		*MaterialKey,
		*PendingCharacterPreviewRuntimeMaterialTask.SectionMetadataPath);
}

void AWoWLoginGameMode::ProcessDeferredCharacterPreviewRuntimeMaterials(float DeltaSeconds)
{
	(void)DeltaSeconds;
	if (PendingCharacterPreviewRuntimeMaterialKey.IsEmpty())
	{
		return;
	}
	if (!CharacterSelectPreviewActor.IsValid() ||
		PendingCharacterPreviewRuntimeMaterialTask.Guid != SelectedGlueCharacterGuid ||
		!PendingCharacterPreviewRuntimeMaterialTask.Appearance.CacheKey.Equals(SelectedGlueCharacterAppearanceKey, ESearchCase::CaseSensitive) ||
		!PendingCharacterPreviewRuntimeMaterialTask.MaterialKey.Equals(PendingCharacterPreviewRuntimeMaterialKey, ESearchCase::CaseSensitive))
	{
		PendingCharacterPreviewRuntimeMaterialKey.Empty();
		PendingCharacterPreviewRuntimeMaterialTask = FPendingCharacterPreviewRuntimeMaterialTask();
		return;
	}

	ASkeletalMeshActor* PreviewActor = CharacterSelectPreviewActor.Get();
	USkeletalMeshComponent* MeshComponent = PreviewActor ? PreviewActor->GetSkeletalMeshComponent() : nullptr;
	if (!MeshComponent || !MeshComponent->GetSkeletalMeshAsset())
	{
		return;
	}

	FPendingCharacterPreviewRuntimeMaterialTask& Task = PendingCharacterPreviewRuntimeMaterialTask;
	auto AbortRuntimeMaterialStaging = [this, MeshComponent, &Task](const TCHAR* AbortReason)
	{
		const bool bHadPreviousPreviewActor = CharacterSelectPreviewPreviousActorDuringStaging.IsValid();
		PendingCharacterPreviewRuntimeMaterialKey.Empty();
		Task = FPendingCharacterPreviewRuntimeMaterialTask();
		if (bHadPreviousPreviewActor)
		{
			CancelPreparedCharacterSelectPreviewStaging(AbortReason);
			return;
		}

		PendingCharacterSelectPreviewRevealGuid = 0;
		PendingCharacterSelectPreviewRevealAppearanceKey.Empty();
		if (MeshComponent)
		{
			MeshComponent->SetVisibility(false, true);
			MeshComponent->SetHiddenInGame(true, true);
		}
		if (CharacterSelectPreviewActor.IsValid())
		{
			CharacterSelectPreviewActor->SetActorHiddenInGame(true);
		}
		UE_LOG(LogTemp, Warning, TEXT("角色预览运行态材质未完成，隐藏当前半成品而不是显示默认/裸体模型: selectedGuid=%d reason=%s"),
			SelectedGlueCharacterGuid,
			AbortReason ? AbortReason : TEXT("<none>"));

		if (bPendingGlueWaitingForCharacterPreview &&
			PendingGlueCharacterPreviewScreen.Equals(TEXT("charselect"), ESearchCase::IgnoreCase))
		{
			const FString TargetScreen = PendingGlueCharacterPreviewScreen;
			bPendingGlueWaitingForCharacterPreview = false;
			PendingGlueCharacterPreviewScreen.Empty();
			PendingGlueCharacterPreviewWaitStartSeconds = 0.0;
			CompletePendingGlueScreenTransition(TargetScreen);
		}
	};
	if (!Task.bInitialized)
	{
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *Task.SectionMetadataPath))
		{
			UE_LOG(LogTemp, Warning, TEXT("角色预览基础 mesh 缺少 section metadata，取消分帧纹理 pass: guid=%d path=%s"),
				Task.Guid,
				*Task.SectionMetadataPath);
			AbortRuntimeMaterialStaging(TEXT("runtime_material_metadata_missing"));
			return;
		}

		TSharedPtr<FJsonObject> RootObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
		if (!FJsonSerializer::Deserialize(Reader, RootObject) ||
			!RootObject.IsValid() ||
			!RootObject->TryGetArrayField(TEXT("sections"), Sections) ||
			!Sections)
		{
			UE_LOG(LogTemp, Warning, TEXT("角色预览基础 mesh section metadata 解析失败，取消分帧纹理 pass: guid=%d path=%s"),
				Task.Guid,
				*Task.SectionMetadataPath);
			AbortRuntimeMaterialStaging(TEXT("runtime_material_metadata_invalid"));
			return;
		}

		Task.Sections = *Sections;
		Task.ComposedBodyAtlas = FindCachedRuntimeCharacterBodyAtlas(Task.Appearance, Task.ComposedItemLayerCount, Task.MissingItemLayerCount);
		if (!Task.ComposedBodyAtlas)
		{
			UE_LOG(LogTemp, Warning, TEXT("角色预览运行时身体 atlas 不可用，拒绝使用 section metadata 旧预览图兜底: guid=%d race=%d gender=%d"),
				Task.Guid,
				Task.Appearance.Race,
				Task.Appearance.Gender);
			AbortRuntimeMaterialStaging(TEXT("runtime_body_atlas_missing"));
			return;
		}
		Task.bInitialized = true;
		UE_LOG(LogTemp, Display, TEXT("角色预览运行时外观纹理分帧 pass 已初始化: guid=%d sections=%d metadata=%s"),
			Task.Guid,
			Task.Sections.Num(),
			*Task.SectionMetadataPath);
	}

	const bool bWaitingForAtomicReveal =
		PendingCharacterSelectPreviewRevealGuid == Task.Guid &&
		PendingCharacterSelectPreviewRevealAppearanceKey.Equals(Task.Appearance.CacheKey, ESearchCase::CaseSensitive);
	const int32 SectionsPerTick = bWaitingForAtomicReveal ? 64 : 8;
	const double MaxTickBudgetMs = bWaitingForAtomicReveal ? 4.0 : 2.0;
	const double TickStartSeconds = FPlatformTime::Seconds();
	int32 ProcessedThisTick = 0;
	while (Task.Sections.IsValidIndex(Task.NextSectionIndex) && ProcessedThisTick < SectionsPerTick)
	{
		const TSharedPtr<FJsonObject> SectionObject = TryReadJsonObject(Task.Sections[Task.NextSectionIndex]);
		++Task.NextSectionIndex;
		++ProcessedThisTick;
		if (!SectionObject.IsValid())
		{
			continue;
		}

		double SectionNumber = 0.0;
		double SubmeshNumber = 0.0;
		double TextureTypeNumber = -1.0;
		if (!SectionObject->TryGetNumberField(TEXT("section_index"), SectionNumber) ||
			!SectionObject->TryGetNumberField(TEXT("texture_type"), TextureTypeNumber))
		{
			continue;
		}

		const int32 SectionIndex = FMath::RoundToInt(SectionNumber);
		const int32 TextureType = FMath::RoundToInt(TextureTypeNumber);
		const int32 MaterialSlotIndex = ResolveMaterialSlotForM2Section(MeshComponent, SectionIndex);
		if (MaterialSlotIndex == INDEX_NONE)
		{
			++Task.MissingCount;
			continue;
		}
		FString SectionPreviewPng;
		SectionObject->TryGetStringField(TEXT("preview_png"), SectionPreviewPng);
		const int32 SubmeshId = SectionObject->TryGetNumberField(TEXT("submesh_id"), SubmeshNumber)
			? FMath::RoundToInt(SubmeshNumber)
			: INDEX_NONE;
		const bool bFaceSection = FPaths::GetBaseFilename(SectionPreviewPng).ToLower().Contains(TEXT("faceupper")) ||
			FPaths::GetBaseFilename(SectionPreviewPng).ToLower().Contains(TEXT("facelower"));

		UTexture2D* Texture = nullptr;
		FString ResolvedRuntimeTexturePng;
		const bool bUseComposedBodyAtlas = WoWRuntimeCharacterAppearance::ShouldUseComposedBodyAtlasForSection(TextureType, SubmeshId, SectionPreviewPng);
		if (bUseComposedBodyAtlas && Task.ComposedBodyAtlas)
		{
			Texture = Task.ComposedBodyAtlas;
			ResolvedRuntimeTexturePng = TEXT("<body-atlas>");
		}
		else
		{
			FWoWRuntimeCharacterAppearanceDescriptor RuntimeDescriptor;
			RuntimeDescriptor.Guid = Task.Appearance.Guid;
			RuntimeDescriptor.Race = Task.Appearance.Race;
			RuntimeDescriptor.Gender = Task.Appearance.Gender;
			RuntimeDescriptor.Skin = Task.Appearance.Skin;
			RuntimeDescriptor.Face = Task.Appearance.Face;
			RuntimeDescriptor.HairStyle = Task.Appearance.HairStyle;
			RuntimeDescriptor.HairColor = Task.Appearance.HairColor;
			RuntimeDescriptor.FacialStyle = Task.Appearance.FacialStyle;
			RuntimeDescriptor.EquipmentSummary = Task.Appearance.EquipmentSummary;
			FString RuntimePreviewPng;
			if (WoWRuntimeCharacterAppearance::ResolveRuntimeSectionTexturePng(
				RuntimeDescriptor,
				TextureType,
				SubmeshId,
				SectionPreviewPng,
				RuntimePreviewPng))
			{
				ResolvedRuntimeTexturePng = RuntimePreviewPng;
				Texture = LoadGeneratedTextureFromPreviewPng(RuntimePreviewPng);
			}
		}
		if (!Texture)
		{
			++Task.MissingCount;
			continue;
		}

		UMaterialInterface* CurrentMaterial = MeshComponent->GetMaterial(MaterialSlotIndex);
		UMaterialInstanceDynamic* DynamicMaterial = Cast<UMaterialInstanceDynamic>(CurrentMaterial);
		if (!DynamicMaterial)
		{
			DynamicMaterial = MeshComponent->CreateAndSetMaterialInstanceDynamic(MaterialSlotIndex);
		}
		if (!DynamicMaterial)
		{
			continue;
		}

		DynamicMaterial->SetTextureParameterValue(TEXT("DiffuseTexture"), Texture);
		if (SubmeshId == 0 || (SubmeshId >= 1500 && SubmeshId < 1800))
		{
			UE_LOG(LogTemp, Display, TEXT("角色预览运行时关键 section 贴图: guid=%d section=%d slot=%d submesh=%d textureType=%d useAtlas=%d texture=%s exportPreview=%s"),
				Task.Guid,
				SectionIndex,
				MaterialSlotIndex,
				SubmeshId,
				TextureType,
				bUseComposedBodyAtlas ? 1 : 0,
				*ResolvedRuntimeTexturePng,
				*SectionPreviewPng);
		}
		if (MaterialSlotIndex != SectionIndex && (SubmeshId == 0 || SubmeshId >= 1500))
		{
			UE_LOG(LogTemp, Display, TEXT("角色预览运行时材质槽映射: guid=%d section=%d slot=%d submesh=%d textureType=%d texture=%s"),
				Task.Guid,
				SectionIndex,
				MaterialSlotIndex,
				SubmeshId,
				TextureType,
				Texture ? *Texture->GetName() : TEXT("<null>"));
		}
		if (bUseComposedBodyAtlas)
		{
			++Task.BodyApplied;
		}
		else if (bFaceSection)
		{
			++Task.FaceApplied;
		}
		else if (TextureType == 6)
		{
			++Task.HairApplied;
		}
		else if (TextureType == 8)
		{
			++Task.FurApplied;
		}

		if ((FPlatformTime::Seconds() - TickStartSeconds) * 1000.0 >= MaxTickBudgetMs)
		{
			break;
		}
	}

	if (Task.NextSectionIndex >= Task.Sections.Num())
	{
		UE_LOG(LogTemp, Display, TEXT("角色预览运行时外观纹理分帧 pass 完成: guid=%d race=%d gender=%d skin=%d face=%d hair=%d/%d body=%d faceSlots=%d hairSlots=%d fur=%d itemLayers=%d missingItemLayers=%d missing=%d elapsed=%.2fms metadata=%s"),
			Task.Guid,
			Task.Appearance.Race,
			Task.Appearance.Gender,
			Task.Appearance.Skin,
			Task.Appearance.Face,
			Task.Appearance.HairStyle,
			Task.Appearance.HairColor,
			Task.BodyApplied,
			Task.FaceApplied,
			Task.HairApplied,
			Task.FurApplied,
			Task.ComposedItemLayerCount,
			Task.MissingItemLayerCount,
			Task.MissingCount,
			(Task.StartSeconds > 0.0 ? (FPlatformTime::Seconds() - Task.StartSeconds) * 1000.0 : 0.0),
			*Task.SectionMetadataPath);
		PendingCharacterPreviewRuntimeMaterialKey.Empty();
		Task = FPendingCharacterPreviewRuntimeMaterialTask();
		TryRevealPreparedCharacterSelectPreview(TEXT("runtime_material_complete"));
	}
}

void AWoWLoginGameMode::DestroyCharacterSelectPreviewAttachments()
{
	PendingCharacterSelectPreviewM2AttachmentKey.Empty();
	PendingCharacterSelectPreviewM2Attachments.Reset();
	PendingCharacterSelectPreviewM2AttachmentDescriptorPath.Empty();
	PendingCharacterSelectPreviewM2AttachmentDefinitionCount = 0;
	PendingCharacterSelectPreviewM2AttachmentAttachedCount = 0;
	PendingCharacterSelectPreviewM2AttachmentStartSeconds = 0.0;
	if (bPreparingCharacterSelectPreviewStaging)
	{
		CharacterSelectPreviewAttachmentKey.Empty();
		UE_LOG(LogTemp, Display, TEXT("角色预览 staging 准备中，保留旧角色附件直到原子替换"));
		return;
	}
	DestroyCharacterSelectPreviewAttachmentParticleSystems();
	for (TWeakObjectPtr<USkeletalMeshComponent>& AttachmentComponentPtr : CharacterSelectPreviewAttachmentComponents)
	{
		if (USkeletalMeshComponent* AttachmentComponent = AttachmentComponentPtr.Get())
		{
			AttachmentComponent->DestroyComponent();
		}
	}
	for (TWeakObjectPtr<UWoWM2ModelComponent>& AttachmentComponentPtr : CharacterSelectPreviewM2AttachmentComponents)
	{
		if (UWoWM2ModelComponent* AttachmentComponent = AttachmentComponentPtr.Get())
		{
			AttachmentComponent->DestroyComponent();
		}
	}
	CharacterSelectPreviewAttachmentComponents.Reset();
	CharacterSelectPreviewM2AttachmentComponents.Reset();
	CharacterSelectPreviewAttachmentAnimationKeys.Reset();
	CharacterSelectPreviewAttachmentKey.Empty();
}

void AWoWLoginGameMode::ApplyCharacterPreviewAttachments(USkeletalMeshComponent* MeshComponent, const FCharacterSelectPreviewAppearance& Appearance)
{
	if (!MeshComponent || Appearance.Guid <= 0)
	{
		return;
	}

	const FString NewAttachmentKey = FString::Printf(
		TEXT("%s|mesh=%s|equipment=%s"),
		*Appearance.CacheKey,
		MeshComponent->GetSkeletalMeshAsset() ? *MeshComponent->GetSkeletalMeshAsset()->GetPathName() : TEXT("<null>"),
		*Appearance.EquipmentSummary);
	if (CharacterSelectPreviewAttachmentKey.Equals(NewAttachmentKey, ESearchCase::CaseSensitive) &&
		(CharacterSelectPreviewM2AttachmentComponents.Num() > 0 || CharacterSelectPreviewAttachmentComponents.Num() > 0))
	{
		return;
	}

	DestroyCharacterSelectPreviewAttachments();

	TArray<FCharacterPreviewAttachmentDefinition> AttachmentDefinitions;
	bool bUsingSharedItemsManifest = false;
	if (FPaths::FileExists(Appearance.CharacterPreviewAttachmentDescriptorPath))
	{
		ReadCharacterPreviewAttachmentDefinitions(Appearance.CharacterPreviewAttachmentDescriptorPath, AttachmentDefinitions);
	}
	if (AttachmentDefinitions.IsEmpty())
	{
		bUsingSharedItemsManifest = BuildSharedItemObjectAttachmentDefinitions(Appearance, AttachmentDefinitions);
	}
	if (AttachmentDefinitions.IsEmpty())
	{
		UE_LOG(LogTemp, Display, TEXT("角色预览无附件定义: guid=%d descriptor=%s sharedItems=%d equipment=%s"),
			Appearance.Guid,
			*Appearance.CharacterPreviewAttachmentDescriptorPath,
			bUsingSharedItemsManifest ? 1 : 0,
			*Appearance.EquipmentSummary);
		return;
	}

	if (bUsingSharedItemsManifest)
	{
		UE_LOG(LogTemp, Display, TEXT("角色预览使用共享 Items ObjectComponents 附件: guid=%d definitions=%d manifest=%s"),
			Appearance.Guid,
			AttachmentDefinitions.Num(),
			*GetItemObjectComponentLibrary().ManifestPath);
	}

	int32 AttachedCount = 0;
	int32 DeferredM2AttachmentCount = 0;
	PendingCharacterSelectPreviewM2Attachments.Reset();
	for (const FCharacterPreviewAttachmentDefinition& AttachmentDefinition : AttachmentDefinitions)
	{
		if (!DoesEquipmentSummaryContainAttachment(Appearance.EquipmentSummary, AttachmentDefinition.Slot, AttachmentDefinition.DisplayId))
		{
			UE_LOG(LogTemp, Verbose, TEXT("角色预览跳过未装备附件: guid=%d name=%s slot=%d displayId=%d equipment=%s"),
				Appearance.Guid,
				*AttachmentDefinition.Name,
				AttachmentDefinition.Slot,
				AttachmentDefinition.DisplayId,
				*Appearance.EquipmentSummary);
			continue;
		}

		FName ResolvedSocketName;
		if (!TryResolveAttachmentSocketById(MeshComponent, AttachmentDefinition.SocketName, ResolvedSocketName))
		{
			UE_LOG(LogTemp, Warning, TEXT("角色预览附件 socket 不存在: guid=%d name=%s socket=%s mesh=%s"),
				Appearance.Guid,
				*AttachmentDefinition.Name,
				*AttachmentDefinition.SocketName.ToString(),
				MeshComponent->GetSkeletalMeshAsset() ? *MeshComponent->GetSkeletalMeshAsset()->GetPathName() : TEXT("<null>"));
			continue;
		}
		if (ResolvedSocketName != AttachmentDefinition.SocketName)
		{
			UE_LOG(LogTemp, Verbose, TEXT("角色预览附件 socket 按 M2 attachment id 解析: guid=%d name=%s requested=%s resolved=%s mesh=%s"),
				Appearance.Guid,
				*AttachmentDefinition.Name,
				*AttachmentDefinition.SocketName.ToString(),
				*ResolvedSocketName.ToString(),
				MeshComponent->GetSkeletalMeshAsset() ? *MeshComponent->GetSkeletalMeshAsset()->GetPathName() : TEXT("<null>"));
		}

		const FString AttachmentMeshJsonPath = !AttachmentDefinition.MeshJsonPath.IsEmpty()
			? AttachmentDefinition.MeshJsonPath
			: BuildCharacterPreviewAttachmentMeshJsonPath(AttachmentDefinition.MeshPath, Appearance.CharacterPreviewAttachmentDescriptorPath);
		FPendingCharacterPreviewM2Attachment PendingAttachment;
		PendingAttachment.Guid = Appearance.Guid;
		PendingAttachment.AttachmentKey = NewAttachmentKey;
		PendingAttachment.Name = AttachmentDefinition.Name;
		PendingAttachment.Slot = AttachmentDefinition.Slot;
		PendingAttachment.DisplayId = AttachmentDefinition.DisplayId;
		PendingAttachment.SocketName = ResolvedSocketName;
		PendingAttachment.MeshPath = AttachmentDefinition.MeshPath;
		PendingAttachment.MeshJsonPath = AttachmentMeshJsonPath;
		PendingAttachment.AnimationPath = AttachmentDefinition.AnimationPath.IsEmpty()
			? BuildCharacterPreviewAttachmentStandAnimationPath(AttachmentDefinition.MeshPath)
			: AttachmentDefinition.AnimationPath;
		PendingAttachment.DescriptorPath = bUsingSharedItemsManifest ? FString() : Appearance.CharacterPreviewAttachmentDescriptorPath;
		PendingAttachment.RelativeLocation = AttachmentDefinition.RelativeLocation;
		PendingAttachment.RelativeRotation = AttachmentDefinition.RelativeRotation;
		PendingAttachment.RelativeScale = AttachmentDefinition.RelativeScale;
		PendingAttachment.bUseM2Runtime = ShouldUseCharacterPreviewM2RuntimeAttachment(AttachmentDefinition, AttachmentMeshJsonPath);
		PendingCharacterSelectPreviewM2Attachments.Add(MoveTemp(PendingAttachment));
		++DeferredM2AttachmentCount;
		continue;
	}

	UE_LOG(LogTemp, Display, TEXT("角色预览附件应用完成: guid=%d descriptor=%s definitions=%d attached=%d equipment=%s"),
		Appearance.Guid,
		bUsingSharedItemsManifest ? TEXT("<shared Items ObjectComponents>") : *Appearance.CharacterPreviewAttachmentDescriptorPath,
		AttachmentDefinitions.Num(),
		AttachedCount,
		*Appearance.EquipmentSummary);
	CharacterSelectPreviewAttachmentKey = NewAttachmentKey;
	if (DeferredM2AttachmentCount > 0 && !PendingCharacterSelectPreviewM2Attachments.IsEmpty())
	{
		PendingCharacterSelectPreviewM2AttachmentKey = NewAttachmentKey;
		PendingCharacterSelectPreviewM2AttachmentDescriptorPath = bUsingSharedItemsManifest ? TEXT("<shared Items ObjectComponents>") : Appearance.CharacterPreviewAttachmentDescriptorPath;
		PendingCharacterSelectPreviewM2AttachmentDefinitionCount = AttachmentDefinitions.Num();
		PendingCharacterSelectPreviewM2AttachmentAttachedCount = 0;
		PendingCharacterSelectPreviewM2AttachmentStartSeconds = FPlatformTime::Seconds();
		UE_LOG(LogTemp, Display, TEXT("角色预览附件延迟到分帧 visual kit pass: guid=%d count=%d key=%s"),
			Appearance.Guid,
			PendingCharacterSelectPreviewM2Attachments.Num(),
			*NewAttachmentKey);
	}
}

void AWoWLoginGameMode::ProcessDeferredCharacterPreviewM2Attachments(float DeltaSeconds)
{
	(void)DeltaSeconds;
	if (PendingCharacterSelectPreviewM2AttachmentKey.IsEmpty() ||
		!PendingCharacterSelectPreviewM2AttachmentKey.Equals(CharacterSelectPreviewAttachmentKey, ESearchCase::CaseSensitive) ||
		!CharacterSelectPreviewActor.IsValid() ||
		PendingCharacterSelectPreviewM2Attachments.IsEmpty())
	{
		PendingCharacterSelectPreviewM2AttachmentKey.Empty();
		PendingCharacterSelectPreviewM2Attachments.Reset();
		return;
	}

	ASkeletalMeshActor* PreviewActor = CharacterSelectPreviewActor.Get();
	USkeletalMeshComponent* MeshComponent = PreviewActor ? PreviewActor->GetSkeletalMeshComponent() : nullptr;
	if (!MeshComponent || !MeshComponent->GetSkeletalMeshAsset() || SelectedGlueCharacterGuid <= 0)
	{
		return;
	}

	const bool bWaitingForAtomicReveal =
		PendingCharacterSelectPreviewRevealGuid == SelectedGlueCharacterGuid &&
		!PendingCharacterSelectPreviewRevealAppearanceKey.IsEmpty();
	const double BatchStartSeconds = FPlatformTime::Seconds();
	const double MaxBatchBudgetMs = bWaitingForAtomicReveal ? 4.0 : 0.0;
	int32 LastProcessedAttachmentGuid = SelectedGlueCharacterGuid;
	do
	{
	FPendingCharacterPreviewM2Attachment PendingAttachment = PendingCharacterSelectPreviewM2Attachments[0];
	PendingCharacterSelectPreviewM2Attachments.RemoveAt(0, 1, EAllowShrinking::No);
	if (PendingAttachment.Guid != SelectedGlueCharacterGuid ||
		!PendingAttachment.AttachmentKey.Equals(PendingCharacterSelectPreviewM2AttachmentKey, ESearchCase::CaseSensitive))
	{
		PendingCharacterSelectPreviewM2AttachmentKey.Empty();
		PendingCharacterSelectPreviewM2Attachments.Reset();
		return;
	}
	LastProcessedAttachmentGuid = PendingAttachment.Guid;

	const double DeferredStartSeconds = FPlatformTime::Seconds();
	const bool bRequiresObjectSkin =
		PendingAttachment.DisplayId > 0 &&
		(PendingAttachment.MeshPath.Contains(TEXT("Shoulder"), ESearchCase::IgnoreCase) ||
		 PendingAttachment.MeshPath.Contains(TEXT("Helm"), ESearchCase::IgnoreCase) ||
		 PendingAttachment.MeshPath.Contains(TEXT("Head"), ESearchCase::IgnoreCase) ||
		 PendingAttachment.Name.Contains(TEXT("shoulder"), ESearchCase::IgnoreCase) ||
		 PendingAttachment.Name.Contains(TEXT("helm"), ESearchCase::IgnoreCase));
	if (PendingAttachment.bUseM2Runtime)
	{
		UWoWM2ModelComponent* M2AttachmentComponent = NewObject<UWoWM2ModelComponent>(MeshComponent->GetOwner());
		if (M2AttachmentComponent)
		{
			const FTransform RelativeTransform(
				PendingAttachment.RelativeRotation,
				PendingAttachment.RelativeLocation,
				PendingAttachment.RelativeScale);
			if (M2AttachmentComponent->LoadFromMeshJson(MeshComponent, PendingAttachment.SocketName, PendingAttachment.MeshJsonPath, RelativeTransform))
			{
				FString ObjectSkinPng;
				bool bAppliedObjectSkin = false;
				if (WoWRuntimeCharacterAppearance::ResolveItemDisplayObjectSkinPng(PendingAttachment.DisplayId, PendingAttachment.MeshPath, ObjectSkinPng))
				{
					const int32 SkinAppliedCount = M2AttachmentComponent->OverrideModelDiffuseTexture(ObjectSkinPng);
					bAppliedObjectSkin = SkinAppliedCount > 0;
					UE_LOG(LogTemp, Verbose, TEXT("角色预览 M2 附件 DBC skin 已应用: guid=%d displayId=%d mesh=%s skin=%s sections=%d"),
						PendingAttachment.Guid,
						PendingAttachment.DisplayId,
						*PendingAttachment.MeshPath,
						*ObjectSkinPng,
						SkinAppliedCount);
				}
				if (bRequiresObjectSkin && !bAppliedObjectSkin)
				{
					UE_LOG(LogTemp, Warning, TEXT("角色预览附件跳过显示：装备 DBC skin 缺失或无法绑定，避免显示错误默认纹理: guid=%d name=%s slot=%d displayId=%d mesh=%s"),
						PendingAttachment.Guid,
						*PendingAttachment.Name,
						PendingAttachment.Slot,
						PendingAttachment.DisplayId,
						*PendingAttachment.MeshPath);
					M2AttachmentComponent->DestroyComponent();
					continue;
				}
				if (AActor* PreviewOwner = MeshComponent->GetOwner())
				{
					PreviewOwner->AddInstanceComponent(M2AttachmentComponent);
				}
				M2AttachmentComponent->RegisterComponent();
				CharacterSelectPreviewM2AttachmentComponents.Add(M2AttachmentComponent);
				++PendingCharacterSelectPreviewM2AttachmentAttachedCount;
				UE_LOG(LogTemp, Verbose, TEXT("角色预览 M2 runtime 附件分帧挂载: guid=%d name=%s slot=%d displayId=%d elapsed=%.2fms remaining=%d"),
					PendingAttachment.Guid,
					*PendingAttachment.Name,
					PendingAttachment.Slot,
					PendingAttachment.DisplayId,
					(FPlatformTime::Seconds() - DeferredStartSeconds) * 1000.0,
					PendingCharacterSelectPreviewM2Attachments.Num());
			}
			else
			{
				M2AttachmentComponent->DestroyComponent();
			}
		}
	}
	else
	{
		USkeletalMesh* AttachmentMesh = ResolveLoadedObject<USkeletalMesh>(PendingAttachment.MeshPath);
		if (!AttachmentMesh)
		{
			UE_LOG(LogTemp, Verbose, TEXT("角色预览 UE 附件未预加载，分帧 pass 跳过同步加载: guid=%d name=%s mesh=%s remaining=%d"),
				PendingAttachment.Guid,
				*PendingAttachment.Name,
				*PendingAttachment.MeshPath,
				PendingCharacterSelectPreviewM2Attachments.Num());
		}
		else if (USkeletalMeshComponent* AttachmentComponent = NewObject<USkeletalMeshComponent>(MeshComponent->GetOwner()))
		{
			AttachmentComponent->SetSkeletalMesh(AttachmentMesh);
			ConfigureCharacterSelectPreviewSkeletalComponent(AttachmentComponent);
			AttachmentComponent->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			AttachmentComponent->SetCastShadow(false);
			AttachmentComponent->AttachToComponent(MeshComponent, FAttachmentTransformRules::SnapToTargetNotIncludingScale, PendingAttachment.SocketName);
			if (AActor* PreviewOwner = MeshComponent->GetOwner())
			{
				PreviewOwner->AddInstanceComponent(AttachmentComponent);
			}
			AttachmentComponent->RegisterComponent();
			const FString AttachmentMaterialPackagePath = BuildCharacterPreviewAttachmentMaterialPackagePath(PendingAttachment.MeshPath);
			const int32 AttachmentAppliedMaterials = ApplyGeneratedSectionMaterials(AttachmentComponent, AttachmentMaterialPackagePath);
			FString ObjectSkinPng;
			bool bAppliedObjectSkin = false;
			if (WoWRuntimeCharacterAppearance::ResolveItemDisplayObjectSkinPng(PendingAttachment.DisplayId, PendingAttachment.MeshPath, ObjectSkinPng))
			{
				if (UTexture2D* SkinTexture = LoadGeneratedTextureFromPreviewPng(ObjectSkinPng))
				{
					for (int32 SlotIndex = 0; SlotIndex < AttachmentComponent->GetNumMaterials(); ++SlotIndex)
					{
						UMaterialInterface* CurrentMaterial = AttachmentComponent->GetMaterial(SlotIndex);
						UMaterialInstanceDynamic* DynamicMaterial = Cast<UMaterialInstanceDynamic>(CurrentMaterial);
						if (!DynamicMaterial)
						{
							DynamicMaterial = AttachmentComponent->CreateDynamicMaterialInstance(SlotIndex, CurrentMaterial);
						}
						if (DynamicMaterial)
						{
							DynamicMaterial->SetTextureParameterValue(TEXT("DiffuseTexture"), SkinTexture);
							bAppliedObjectSkin = true;
						}
					}
				}
			}
			if (bRequiresObjectSkin && !bAppliedObjectSkin)
			{
				UE_LOG(LogTemp, Warning, TEXT("角色预览 UE 附件跳过显示：装备 DBC skin 缺失或无法绑定，避免显示错误默认纹理: guid=%d name=%s slot=%d displayId=%d mesh=%s materials=%d"),
					PendingAttachment.Guid,
					*PendingAttachment.Name,
					PendingAttachment.Slot,
					PendingAttachment.DisplayId,
					*PendingAttachment.MeshPath,
					AttachmentAppliedMaterials);
				AttachmentComponent->DestroyComponent();
				continue;
			}
			AttachmentComponent->SetRelativeLocation(PendingAttachment.RelativeLocation);
			AttachmentComponent->SetRelativeRotation(PendingAttachment.RelativeRotation);
			AttachmentComponent->SetRelativeScale3D(PendingAttachment.RelativeScale);
			AttachmentComponent->SetVisibility(true, true);
			AttachmentComponent->SetHiddenInGame(false, true);
			AttachmentComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
			if (!PendingAttachment.AnimationPath.IsEmpty())
			{
				if (UAnimSequence* AttachmentAnimation = ResolveLoadedObject<UAnimSequence>(PendingAttachment.AnimationPath))
				{
					PlayCharacterSelectPreviewAnimation(
						AttachmentComponent,
						AttachmentAnimation,
						PendingAttachment.Name,
						PendingAttachment.AnimationPath,
						PendingAttachment.MeshPath);
					CharacterSelectPreviewAttachmentAnimationKeys.Add(AttachmentComponent, PendingAttachment.AnimationPath);
				}
			}
			CharacterSelectPreviewAttachmentComponents.Add(AttachmentComponent);
			SpawnCharacterPreviewAttachmentParticleSystems(
				AttachmentComponent,
				PendingAttachment.Name,
				PendingAttachment.MeshPath,
				PendingAttachment.MeshJsonPath,
				PendingAttachment.DescriptorPath);
			++PendingCharacterSelectPreviewM2AttachmentAttachedCount;
			UE_LOG(LogTemp, Verbose, TEXT("角色预览 UE 附件分帧挂载: guid=%d name=%s slot=%d displayId=%d materials=%d elapsed=%.2fms remaining=%d"),
				PendingAttachment.Guid,
				*PendingAttachment.Name,
				PendingAttachment.Slot,
				PendingAttachment.DisplayId,
				AttachmentAppliedMaterials,
				(FPlatformTime::Seconds() - DeferredStartSeconds) * 1000.0,
				PendingCharacterSelectPreviewM2Attachments.Num());
		}
	}
		if (!bWaitingForAtomicReveal ||
			PendingCharacterSelectPreviewM2Attachments.IsEmpty() ||
			(FPlatformTime::Seconds() - BatchStartSeconds) * 1000.0 >= MaxBatchBudgetMs)
		{
			break;
		}
	} while (!PendingCharacterSelectPreviewM2Attachments.IsEmpty());

	if (PendingCharacterSelectPreviewM2Attachments.IsEmpty())
	{
		UE_LOG(LogTemp, Display, TEXT("角色预览附件分帧 visual kit pass 完成: guid=%d descriptor=%s definitions=%d attached=%d elapsed=%.2fms key=%s"),
			LastProcessedAttachmentGuid,
			*PendingCharacterSelectPreviewM2AttachmentDescriptorPath,
			PendingCharacterSelectPreviewM2AttachmentDefinitionCount,
			PendingCharacterSelectPreviewM2AttachmentAttachedCount,
			PendingCharacterSelectPreviewM2AttachmentStartSeconds > 0.0
				? (FPlatformTime::Seconds() - PendingCharacterSelectPreviewM2AttachmentStartSeconds) * 1000.0
				: 0.0,
			*PendingCharacterSelectPreviewM2AttachmentKey);
		PendingCharacterSelectPreviewM2AttachmentKey.Empty();
		PendingCharacterSelectPreviewM2AttachmentDescriptorPath.Empty();
		PendingCharacterSelectPreviewM2AttachmentDefinitionCount = 0;
		PendingCharacterSelectPreviewM2AttachmentAttachedCount = 0;
		PendingCharacterSelectPreviewM2AttachmentStartSeconds = 0.0;
		TryRevealPreparedCharacterSelectPreview(TEXT("attachment_complete"));
	}
}

void AWoWLoginGameMode::TryRevealPreparedCharacterSelectPreview(const TCHAR* Reason)
{
	if (PendingCharacterSelectPreviewRevealGuid <= 0 ||
		PendingCharacterSelectPreviewRevealAppearanceKey.IsEmpty())
	{
		return;
	}
	if (PendingCharacterSelectPreviewRevealGuid != SelectedGlueCharacterGuid ||
		!PendingCharacterSelectPreviewRevealAppearanceKey.Equals(SelectedGlueCharacterAppearanceKey, ESearchCase::CaseSensitive))
	{
		PendingCharacterSelectPreviewRevealGuid = 0;
		PendingCharacterSelectPreviewRevealAppearanceKey.Empty();
		return;
	}
	if (!PendingCharacterPreviewRuntimeMaterialKey.IsEmpty() ||
		!PendingCharacterSelectPreviewM2AttachmentKey.IsEmpty())
	{
		return;
	}
	if (!CharacterSelectPreviewActor.IsValid())
	{
		return;
	}

	ASkeletalMeshActor* PreviewActor = CharacterSelectPreviewActor.Get();
	USkeletalMeshComponent* MeshComponent = PreviewActor ? PreviewActor->GetSkeletalMeshComponent() : nullptr;
	if (!MeshComponent || !MeshComponent->GetSkeletalMeshAsset())
	{
		return;
	}

	MeshComponent->SetVisibility(true, true);
	MeshComponent->SetHiddenInGame(false, true);
	PreviewActor->SetActorHiddenInGame(false);
	PositionCharacterSelectPreviewActor();
	if (CharacterSelectPreviewPreviousActorDuringStaging.IsValid() &&
		CharacterSelectPreviewPreviousActorDuringStaging.Get() != PreviewActor)
	{
		AActor* PreviousActor = CharacterSelectPreviewPreviousActorDuringStaging.Get();
		for (int32 SystemIndex = CharacterSelectPreviewAttachmentParticleSystems.Num() - 1; SystemIndex >= 0; --SystemIndex)
		{
			FLoginParticleSystem& System = CharacterSelectPreviewAttachmentParticleSystems[SystemIndex];
			USkeletalMeshComponent* SourceComponent = System.SourceMeshComponent.Get();
			if (SourceComponent && SourceComponent->GetOwner() == PreviousActor)
			{
				if (System.MeshComponent)
				{
					System.MeshComponent->DestroyComponent();
				}
				CharacterSelectPreviewAttachmentParticleSystems.RemoveAtSwap(SystemIndex, 1, EAllowShrinking::No);
			}
		}
		CharacterSelectPreviewAttachmentComponents.RemoveAll([PreviousActor](const TWeakObjectPtr<USkeletalMeshComponent>& ComponentPtr)
		{
			const USkeletalMeshComponent* Component = ComponentPtr.Get();
			return !Component || Component->GetOwner() == PreviousActor;
		});
		CharacterSelectPreviewM2AttachmentComponents.RemoveAll([PreviousActor](const TWeakObjectPtr<UWoWM2ModelComponent>& ComponentPtr)
		{
			const UWoWM2ModelComponent* Component = ComponentPtr.Get();
			return !Component || Component->GetOwner() == PreviousActor;
		});
		CharacterSelectPreviewPreviousActorDuringStaging->Destroy();
		CharacterSelectPreviewPreviousActorDuringStaging.Reset();
		UE_LOG(LogTemp, Display, TEXT("角色预览 staging 已替换旧 actor: guid=%d"), SelectedGlueCharacterGuid);
	}
	UE_LOG(LogTemp, Display, TEXT("角色预览运行态视觉完成，统一显示: guid=%d reason=%s key=%s"),
		SelectedGlueCharacterGuid,
		Reason ? Reason : TEXT("<none>"),
		*PendingCharacterSelectPreviewRevealAppearanceKey);
	PendingCharacterSelectPreviewRevealGuid = 0;
	PendingCharacterSelectPreviewRevealAppearanceKey.Empty();

	if (bPendingGlueWaitingForCharacterPreview &&
		PendingGlueCharacterPreviewScreen.Equals(TEXT("charselect"), ESearchCase::IgnoreCase))
	{
		const FString TargetScreen = PendingGlueCharacterPreviewScreen;
		bPendingGlueWaitingForCharacterPreview = false;
		PendingGlueCharacterPreviewScreen.Empty();
		const double WaitElapsedMs = PendingGlueCharacterPreviewWaitStartSeconds > 0.0
			? (FPlatformTime::Seconds() - PendingGlueCharacterPreviewWaitStartSeconds) * 1000.0
			: 0.0;
		PendingGlueCharacterPreviewWaitStartSeconds = 0.0;
		UE_LOG(LogTemp, Display, TEXT("Glue pending selected character preview woke after visual reveal: target=%s guid=%d wait=%.2fms reason=%s"),
			*TargetScreen,
			SelectedGlueCharacterGuid,
			WaitElapsedMs,
			Reason ? Reason : TEXT("<none>"));
		CompletePendingGlueScreenTransition(TargetScreen);
	}
}

void AWoWLoginGameMode::ApplyCharacterPreviewSectionVisibility(USkeletalMeshComponent* MeshComponent, const FPlayerCharacterPreviewDefinition& Definition, const FCharacterSelectPreviewAppearance& Appearance)
{
	if (!MeshComponent)
	{
		return;
	}

	const FString SectionMetadataPath = (!Appearance.CharacterPreviewSectionMetadataPath.IsEmpty() && FPaths::FileExists(Appearance.CharacterPreviewSectionMetadataPath))
		? Appearance.CharacterPreviewSectionMetadataPath
		: Definition.SectionMetadataPath;

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *SectionMetadataPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览缺少 section metadata，无法按 geoset 控制可见性: %s。请重新运行 PlayerCharacters 导出生成 *.sections.json。"),
			*SectionMetadataPath);
		return;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览 section metadata 解析失败: %s"), *SectionMetadataPath);
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
	{
		UE_LOG(LogTemp, Warning, TEXT("角色预览 section metadata 缺少 sections: %s"), *SectionMetadataPath);
		return;
	}

	if (Appearance.bHasCharacterPreviewAsset)
	{
		const int32 SectionCount = MeshComponent->GetNumMaterials();
		UE_LOG(LogTemp, Display, TEXT("角色预览已烘焙 section 诊断: guid=%d meshSections=%d metadataSections=%d mesh=%s metadata=%s"),
			Appearance.Guid,
			SectionCount,
			Sections->Num(),
			*Appearance.CharacterPreviewMeshPath,
			*SectionMetadataPath);
		if (SectionCount != Sections->Num())
		{
			UE_LOG(LogTemp, Warning, TEXT("角色预览已烘焙 mesh section 数量与 metadata 不匹配，继续按 M2_Section 槽名应用显隐: guid=%d meshSections=%d metadataSections=%d metadata=%s"),
				Appearance.Guid,
				SectionCount,
				Sections->Num(),
				*SectionMetadataPath);
		}
	}

	FVector MeshBoundsMin = FVector::ZeroVector;
	FVector MeshBoundsMax = FVector::ZeroVector;
	if (!TryReadJsonVector3Field(RootObject, TEXT("bounds_min"), MeshBoundsMin) ||
		!TryReadJsonVector3Field(RootObject, TEXT("bounds_max"), MeshBoundsMax))
	{
		MeshBoundsMax.Z = 235.0f;
	}

	TSet<int32> AvailableSubmeshIds;
	TArray<FCharacterPreviewSectionGeosetInfo> SectionGeosetInfos;
	for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
	{
		const TSharedPtr<FJsonObject> SectionObject = TryReadJsonObject(SectionValue);
		if (!SectionObject.IsValid())
		{
			continue;
		}

		double SubmeshNumber = 0.0;
		if (SectionObject->TryGetNumberField(TEXT("submesh_id"), SubmeshNumber))
		{
			FCharacterPreviewSectionGeosetInfo SectionInfo;
			SectionInfo.SubmeshId = FMath::RoundToInt(SubmeshNumber);
			double TextureTypeNumber = -1.0;
			SectionInfo.TextureType = SectionObject->TryGetNumberField(TEXT("texture_type"), TextureTypeNumber)
				? FMath::RoundToInt(TextureTypeNumber)
				: INDEX_NONE;
			SectionInfo.RenderMode = SectionObject->TryGetNumberField(TEXT("render_mode"), TextureTypeNumber)
				? FMath::RoundToInt(TextureTypeNumber)
				: INDEX_NONE;
			SectionInfo.MaterialFlags = SectionObject->TryGetNumberField(TEXT("material_flags"), TextureTypeNumber)
				? FMath::RoundToInt(TextureTypeNumber)
				: 0;
			SectionObject->TryGetBoolField(TEXT("unlit"), SectionInfo.bUnlit);
			SectionObject->TryGetBoolField(TEXT("no_depth_write"), SectionInfo.bNoDepthWrite);
			SectionObject->TryGetStringField(TEXT("preview_png"), SectionInfo.PreviewPng);
			AvailableSubmeshIds.Add(SectionInfo.SubmeshId);
			SectionGeosetInfos.Add(MoveTemp(SectionInfo));
		}
	}

	TMap<int32, int32> GeosetGroups;
	const FCharacterAppearanceDbcCache& AppearanceDbcCache = GetCharacterAppearanceDbcCache();
	BuildWmvDefaultPreviewGeosets(AvailableSubmeshIds, GeosetGroups);
	if (AppearanceDbcCache.bLoaded)
	{
		ApplyWmvEquipmentGeosetRules(AppearanceDbcCache, Appearance.EquipmentSummary, AvailableSubmeshIds, GeosetGroups);
	}
	const int32 SelectedHairGeosetId = AppearanceDbcCache.bLoaded
		? ResolveSelectedHairGeosetId(AppearanceDbcCache, Appearance.Race, Appearance.Gender, Appearance.HairStyle)
		: 0;
	const FCharacterAppearanceDbcFacialHairStyle* FacialHairStyleRecord = AppearanceDbcCache.bLoaded
		? ResolveSelectedFacialHairStyle(AppearanceDbcCache, Appearance.Race, Appearance.Gender, Appearance.FacialStyle)
		: nullptr;
	int32 HelmetDisplayId = 0;
	int32 HelmetVisId = 0;
	const FCharacterAppearanceDbcHelmetVis* HelmetVisRecord = AppearanceDbcCache.bLoaded
		? ResolveHelmetVisForEquippedHelmet(AppearanceDbcCache, Appearance.EquipmentSummary, Appearance.Gender, HelmetDisplayId, HelmetVisId)
		: nullptr;
	const bool bHideHairForHelmet = HelmetVisRecord && DoHelmetGeosetFlagsHideRace(HelmetVisRecord->HairFlags, Appearance.Race);
	const bool bHideFacial1ForHelmet = HelmetVisRecord && DoHelmetGeosetFlagsHideRace(HelmetVisRecord->Facial1Flags, Appearance.Race);
	const bool bHideFacial2ForHelmet = HelmetVisRecord && DoHelmetGeosetFlagsHideRace(HelmetVisRecord->Facial2Flags, Appearance.Race);
	const bool bHideFacial3ForHelmet = HelmetVisRecord && DoHelmetGeosetFlagsHideRace(HelmetVisRecord->Facial3Flags, Appearance.Race);
	const bool bHideEarsForHelmet = HelmetVisRecord && DoHelmetGeosetFlagsHideRace(HelmetVisRecord->EarsFlags, Appearance.Race);
	const int32 EyeGlowGeosetValue = ResolveCharacterEyeGlowGeosetValue(AvailableSubmeshIds, SectionGeosetInfos, Appearance.ClassId);
	if (!bHideHairForHelmet && SelectedHairGeosetId > 0 && AvailableSubmeshIds.Contains(SelectedHairGeosetId))
	{
		GeosetGroups.Add(0, SelectedHairGeosetId);
	}
	if (FacialHairStyleRecord)
	{
		SetPreviewGeosetGroup(GeosetGroups, ECharacterPreviewGeosetGroup::Facial1, FacialHairStyleRecord->Geoset100);
		SetPreviewGeosetGroup(GeosetGroups, ECharacterPreviewGeosetGroup::Facial2, FacialHairStyleRecord->Geoset200);
		SetPreviewGeosetGroup(GeosetGroups, ECharacterPreviewGeosetGroup::Facial3, FacialHairStyleRecord->Geoset300);
	}
	SetPreviewGeosetGroup(GeosetGroups, ECharacterPreviewGeosetGroup::EyeGlow, EyeGlowGeosetValue);

	int32 ExplicitGeosetCount = 0;
	const bool bUseLegacyMetadataGeosets = !Appearance.bHasAppearanceDescriptor || !AppearanceDbcCache.bLoaded;
	const TArray<TSharedPtr<FJsonValue>>* EnabledGeosets = nullptr;
	if (bUseLegacyMetadataGeosets && RootObject->TryGetArrayField(TEXT("enabled_geosets"), EnabledGeosets) && EnabledGeosets)
	{
		for (const TSharedPtr<FJsonValue>& GeosetValue : *EnabledGeosets)
		{
			if (!GeosetValue.IsValid())
			{
				continue;
			}

			const int32 SubmeshId = FMath::RoundToInt(GeosetValue->AsNumber());
			if (AvailableSubmeshIds.Contains(SubmeshId))
			{
				OverridePreviewGeosetGroupFromSubmesh(SubmeshId, GeosetGroups);
				++ExplicitGeosetCount;
			}
		}
	}

	if (bHideHairForHelmet)
	{
		GeosetGroups.Add(0, 0);
	}
	if (bHideFacial1ForHelmet)
	{
		GeosetGroups.Add(1, 0);
	}
	if (bHideFacial2ForHelmet)
	{
		GeosetGroups.Add(2, 0);
	}
	if (bHideFacial3ForHelmet)
	{
		GeosetGroups.Add(3, 0);
	}
	if (bHideEarsForHelmet)
	{
		GeosetGroups.Add(7, 0);
	}

	int32 AppliedCount = 0;
	int32 VisibleCount = 0;
	int32 HelmetHiddenHeadOverlayCount = 0;
	TMap<int32, bool> VisibilityByM2SectionIndex;
	for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
	{
		const TSharedPtr<FJsonObject> SectionObject = TryReadJsonObject(SectionValue);
		if (!SectionObject.IsValid())
		{
			continue;
		}

		double SectionNumber = 0.0;
		double SubmeshNumber = 0.0;
		if (!SectionObject->TryGetNumberField(TEXT("section_index"), SectionNumber) ||
			!SectionObject->TryGetNumberField(TEXT("submesh_id"), SubmeshNumber))
		{
			continue;
		}

		const int32 SectionIndex = FMath::RoundToInt(SectionNumber);
		const int32 SubmeshId = FMath::RoundToInt(SubmeshNumber);
		if (SectionIndex < 0)
		{
			continue;
		}

		double TextureTypeNumber = -1.0;
		const int32 TextureType = SectionObject->TryGetNumberField(TEXT("texture_type"), TextureTypeNumber)
			? FMath::RoundToInt(TextureTypeNumber)
			: INDEX_NONE;
		const int32 RenderMode = SectionObject->TryGetNumberField(TEXT("render_mode"), TextureTypeNumber)
			? FMath::RoundToInt(TextureTypeNumber)
			: INDEX_NONE;
		const int32 MaterialFlags = SectionObject->TryGetNumberField(TEXT("material_flags"), TextureTypeNumber)
			? FMath::RoundToInt(TextureTypeNumber)
			: 0;
		bool bUnlit = false;
		bool bNoDepthWrite = false;
		SectionObject->TryGetBoolField(TEXT("unlit"), bUnlit);
		SectionObject->TryGetBoolField(TEXT("no_depth_write"), bNoDepthWrite);
		FString SectionPreviewPng;
		SectionObject->TryGetStringField(TEXT("preview_png"), SectionPreviewPng);
		FVector BoundsMin = FVector::ZeroVector;
		FVector BoundsMax = FVector::ZeroVector;
		const bool bHasBounds =
			TryReadJsonVector3Field(SectionObject, TEXT("bounds_min"), BoundsMin) &&
			TryReadJsonVector3Field(SectionObject, TEXT("bounds_max"), BoundsMax);
		const bool bHideHelmetHeadOverlay =
			bHideHairForHelmet &&
			bHasBounds &&
			IsHelmetHiddenHeadOverlaySubmesh(SubmeshId, TextureType, BoundsMin, BoundsMax, MeshBoundsMin, MeshBoundsMax);
		const bool bFaceSection = IsPreviewFaceSectionSubmesh(SubmeshId, TextureType, SectionPreviewPng);
		const bool bVisible = IsPreviewSectionVisible(SubmeshId, TextureType, GeosetGroups) && !bHideHelmetHeadOverlay;
		VisibilityByM2SectionIndex.Add(SectionIndex, bVisible);
		if (SubmeshId == 0 || (SubmeshId >= 1500 && SubmeshId < 1800) || bFaceSection)
		{
			UE_LOG(LogTemp, Display, TEXT("角色预览关键 section 诊断: guid=%d baked=%d section=%d submesh=%d textureType=%d renderMode=%d materialFlags=0x%X unlit=%d noDepthWrite=%d face=%d visible=%d hideHelmetOverlay=%d preview=%s"),
				Appearance.Guid,
				Appearance.bHasCharacterPreviewAsset ? 1 : 0,
				SectionIndex,
				SubmeshId,
				TextureType,
				RenderMode,
				MaterialFlags,
				bUnlit ? 1 : 0,
				bNoDepthWrite ? 1 : 0,
				bFaceSection ? 1 : 0,
				bVisible ? 1 : 0,
				bHideHelmetHeadOverlay ? 1 : 0,
				*SectionPreviewPng);
		}
		if (bVisible)
		{
			++VisibleCount;
		}
		else if (bHideHelmetHeadOverlay)
		{
			++HelmetHiddenHeadOverlayCount;
		}
		++AppliedCount;
	}
	ApplyLoginPreviewSkeletalM2SectionVisibility(MeshComponent, VisibilityByM2SectionIndex, AppliedCount, VisibleCount);

	UE_LOG(LogTemp, Display, TEXT("角色预览 geoset 可见性已应用: guid=%d id=%s class=%d skin=%d face=%d hairStyle=%d hairColor=%d facial=%d eyeGlow=%d equipmentEntries=%d descriptor=%d sections=%d visible=%d groups=%d explicit=%d helmetHeadOverlaysHidden=%d metadata=%s"),
		Appearance.Guid,
		*Definition.Id,
		Appearance.ClassId,
		Appearance.Skin,
		Appearance.Face,
		Appearance.HairStyle,
		Appearance.HairColor,
		Appearance.FacialStyle,
		EyeGlowGeosetValue,
		Appearance.EquipmentEntryCount,
		Appearance.bHasAppearanceDescriptor ? 1 : 0,
		AppliedCount,
		VisibleCount,
		GeosetGroups.Num(),
		ExplicitGeosetCount,
		HelmetHiddenHeadOverlayCount,
		*SectionMetadataPath);

	if (bHideHairForHelmet || SelectedHairGeosetId > 0 || FacialHairStyleRecord)
	{
		UE_LOG(LogTemp, Display, TEXT("角色预览头盔/头发geoset规则: guid=%d race=%d gender=%d hairStyle=%d hairGeoset=%d facialStyle=%d facialGeosets=%d/%d/%d helmetDisplayId=%d helmetVisId=%d hideHair=%d hideFacial=%d/%d/%d hideEars=%d flags=0x%08X/0x%08X/0x%08X/0x%08X/0x%08X dbcLoaded=%d"),
			Appearance.Guid,
			Appearance.Race,
			Appearance.Gender,
			Appearance.HairStyle,
			SelectedHairGeosetId,
			Appearance.FacialStyle,
			FacialHairStyleRecord ? FacialHairStyleRecord->Geoset100 : 0,
			FacialHairStyleRecord ? FacialHairStyleRecord->Geoset200 : 0,
			FacialHairStyleRecord ? FacialHairStyleRecord->Geoset300 : 0,
			HelmetDisplayId,
			HelmetVisId,
			bHideHairForHelmet ? 1 : 0,
			bHideFacial1ForHelmet ? 1 : 0,
			bHideFacial2ForHelmet ? 1 : 0,
			bHideFacial3ForHelmet ? 1 : 0,
			bHideEarsForHelmet ? 1 : 0,
			HelmetVisRecord ? HelmetVisRecord->HairFlags : 0,
			HelmetVisRecord ? HelmetVisRecord->Facial1Flags : 0,
			HelmetVisRecord ? HelmetVisRecord->Facial2Flags : 0,
			HelmetVisRecord ? HelmetVisRecord->Facial3Flags : 0,
			HelmetVisRecord ? HelmetVisRecord->EarsFlags : 0,
			AppearanceDbcCache.bLoaded ? 1 : 0);
	}
}

void AWoWLoginGameMode::ProcessGlueAsyncResults()
{
	FGlueLoginAsyncResult LoginResult;
	while (GlueLoginResultQueue.IsValid() && GlueLoginResultQueue->Dequeue(LoginResult))
	{
		if (GlueLoginRequestSerial == LoginResult.RequestSerial)
		{
			UE_LOG(LogTemp, Display, TEXT("Glue login result consumed: serial=%d gameThread=%u workerThread=%u elapsed=%.3f authenticated=%d worldConnected=%d"),
				LoginResult.RequestSerial,
				FPlatformTLS::GetCurrentThreadId(),
				LoginResult.WorkerThreadId,
				LoginResult.ElapsedSeconds,
				LoginResult.bAuthenticated ? 1 : 0,
				LoginResult.bWorldConnected ? 1 : 0);
			FinishGlueLogin(
				LoginResult);
		}
	}

	FGlueRealmAsyncResult RealmResult;
	while (GlueRealmResultQueue.IsValid() && GlueRealmResultQueue->Dequeue(RealmResult))
	{
		if (GlueRealmRequestSerial == RealmResult.RequestSerial)
		{
			UE_LOG(LogTemp, Display, TEXT("Glue realm result consumed: serial=%d gameThread=%u workerThread=%u elapsed=%.3f connected=%d"),
				RealmResult.RequestSerial,
				FPlatformTLS::GetCurrentThreadId(),
				RealmResult.WorkerThreadId,
				RealmResult.ElapsedSeconds,
				RealmResult.bConnected ? 1 : 0);
			FinishGlueRealmSelected(
				RealmResult);
		}
	}
}

void AWoWLoginGameMode::RefreshGlueCharacterList()
{
	CachedGlueCharacterRows.Reset();

	if (LoggedInAccountId <= 0)
	{
		return;
	}

	TArray<FWoWGlueCharacterSummary> Characters;
	FString ErrorText;
	if (!FWoWAzerothCoreService::LoadCharactersForAccount(LoggedInAccountId, Characters, ErrorText))
	{
		UE_LOG(LogTemp, Warning, TEXT("读取角色列表失败: %s"), *ErrorText);
		if (LoginGlueLuaRuntime.IsValid())
		{
			LoginGlueLuaRuntime->SetAccountErrorText(ErrorText);
		}
		return;
	}

	SetGlueCharacterListFromSummaries(Characters, true);
}

void AWoWLoginGameMode::ReconcileGlueCharacterCacheForAccount(const TArray<FWoWGlueCharacterSummary>& Characters)
{
	if (LoggedInAccountId <= 0)
	{
		return;
	}

	const FString AccountCacheDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("WoWData/Accounts"));
	const FString AccountCacheKey = SanitizePackageSegment(
		LoggedInAccountName.IsEmpty() ? FString::Printf(TEXT("account_%d"), LoggedInAccountId) : LoggedInAccountName);
	const FString AccountCharacterIndexPath = AccountCacheDirectory / (AccountCacheKey + TEXT(".characters.json"));
	IFileManager::Get().MakeDirectory(*AccountCacheDirectory, true);

	TSet<int32> CurrentGuids;
	TSet<FString> CurrentSignatures;
	TArray<TSharedPtr<FJsonValue>> CurrentCharacterValues;
	CurrentCharacterValues.Reserve(Characters.Num());

	for (const FWoWGlueCharacterSummary& Character : Characters)
	{
		if (Character.Guid <= 0)
		{
			continue;
		}

		const FString EquipmentSummary = BuildEquipmentDisplaySummary(Character.Equipment);
		const FString AppearanceSignature = BuildCharacterAppearanceSignature(
			Character.Race,
			Character.Gender,
			Character.Skin,
			Character.Face,
			Character.HairStyle,
			Character.HairColor,
			Character.FacialStyle,
			EquipmentSummary);

		CurrentGuids.Add(Character.Guid);
		CurrentSignatures.Add(AppearanceSignature);

		TSharedRef<FJsonObject> CharacterObject = MakeShared<FJsonObject>();
		CharacterObject->SetNumberField(TEXT("guid"), Character.Guid);
		CharacterObject->SetStringField(TEXT("name"), Character.Name);
		CharacterObject->SetNumberField(TEXT("class"), Character.ClassId);
		CharacterObject->SetStringField(TEXT("appearanceSignature"), AppearanceSignature);
		CharacterObject->SetStringField(TEXT("equipment"), EquipmentSummary);
		CurrentCharacterValues.Add(MakeShared<FJsonValueObject>(CharacterObject));
	}

	int32 RemovedGuidCount = 0;
	int32 RemovedFileCount = 0;
	FString PreviousIndexText;
	if (FFileHelper::LoadFileToString(PreviousIndexText, *AccountCharacterIndexPath))
	{
		TSharedPtr<FJsonObject> PreviousRootObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(PreviousIndexText);
		if (FJsonSerializer::Deserialize(Reader, PreviousRootObject) && PreviousRootObject.IsValid())
		{
			const TArray<TSharedPtr<FJsonValue>>* PreviousCharacterValues = nullptr;
			if (PreviousRootObject->TryGetArrayField(TEXT("characters"), PreviousCharacterValues) && PreviousCharacterValues)
			{
				for (const TSharedPtr<FJsonValue>& PreviousCharacterValue : *PreviousCharacterValues)
				{
					const TSharedPtr<FJsonObject> PreviousCharacterObject = TryReadJsonObject(PreviousCharacterValue);
					if (!PreviousCharacterObject.IsValid())
					{
						continue;
					}

					const int32 PreviousGuid = PreviousCharacterObject->GetIntegerField(TEXT("guid"));
					if (PreviousGuid <= 0 || CurrentGuids.Contains(PreviousGuid))
					{
						continue;
					}

					++RemovedGuidCount;
					const FString GuidText = FString::FromInt(PreviousGuid);
					const FString AppearanceDescriptorPath = FPaths::ConvertRelativePathToFull(
						FPaths::ProjectSavedDir() / TEXT("WoWData/CharacterAppearance") / FString::Printf(TEXT("%d.appearance.json"), PreviousGuid));
					if (FPaths::FileExists(AppearanceDescriptorPath) && IFileManager::Get().Delete(*AppearanceDescriptorPath, false, true))
					{
						++RemovedFileCount;
					}

					const FString DescriptorDirectory = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("WoWData/CharacterPreviewBuild/Descriptors"));
					TArray<FString> DescriptorFiles;
					IFileManager::Get().FindFiles(DescriptorFiles, *(DescriptorDirectory / FString::Printf(TEXT("*_guid%s.appearance.json"), *GuidText)), true, false);
					for (const FString& DescriptorFile : DescriptorFiles)
					{
						const FString DescriptorPath = DescriptorDirectory / DescriptorFile;
						if (IFileManager::Get().Delete(*DescriptorPath, false, true))
						{
							++RemovedFileCount;
						}
					}
				}
			}
		}
	}

	TSharedRef<FJsonObject> RootObject = MakeShared<FJsonObject>();
	RootObject->SetNumberField(TEXT("accountId"), LoggedInAccountId);
	RootObject->SetStringField(TEXT("accountName"), LoggedInAccountName);
	RootObject->SetStringField(TEXT("accountCacheKey"), AccountCacheKey);
	RootObject->SetStringField(TEXT("updatedAt"), FDateTime::UtcNow().ToIso8601());
	RootObject->SetArrayField(TEXT("characters"), MoveTemp(CurrentCharacterValues));

	FString IndexText;
	const TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&IndexText);
	if (FJsonSerializer::Serialize(RootObject, Writer))
	{
		FFileHelper::SaveStringToFile(IndexText, *AccountCharacterIndexPath);
	}

	if (RemovedGuidCount > 0 || RemovedFileCount > 0)
	{
		UE_LOG(LogTemp, Display, TEXT("账号角色索引已按服务器列表同步: account=%d current=%d staleGuids=%d removedGuidFiles=%d index=%s"),
			LoggedInAccountId,
			CurrentGuids.Num(),
			RemovedGuidCount,
			RemovedFileCount,
			*AccountCharacterIndexPath);
	}
}

void AWoWLoginGameMode::SetGlueCharacterListFromSummaries(const TArray<FWoWGlueCharacterSummary>& Characters, bool bQueueResourceWork)
{
	CachedGlueCharacterRows.Reset();
	ReconcileGlueCharacterCacheForAccount(Characters);

	for (const FWoWGlueCharacterSummary& Character : Characters)
	{
		const FString EquipmentSummary = BuildEquipmentDisplaySummary(Character.Equipment);
		const FString CharacterRow = FString::Printf(
			TEXT("%d\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%d\t%s"),
			Character.Guid,
			*Character.Name,
			Character.Race,
			Character.ClassId,
			Character.Gender,
			Character.Level,
			Character.Skin,
			Character.Face,
			Character.HairStyle,
			Character.HairColor,
			Character.FacialStyle,
			Character.bHardcore ? 1 : 0,
			Character.Zone,
			Character.Map,
			Character.Flags,
			Character.AtLoginFlags,
			*EquipmentSummary);
		CachedGlueCharacterRows.Add(CharacterRow);

	}

	if (bQueueResourceWork)
	{
		QueueCharacterPreviewPrewarm();
	}
}

void AWoWLoginGameMode::RefreshGlueRealmList()
{
	CachedGlueRealmRows.Reset();

	TArray<FWoWGlueRealmSummary> Realms;
	FString ErrorText;
	if (!FWoWAzerothCoreService::LoadRealmList(Realms, ErrorText))
	{
		UE_LOG(LogTemp, Warning, TEXT("读取服务器列表失败: %s"), *ErrorText);
		if (LoginGlueLuaRuntime.IsValid())
		{
			LoginGlueLuaRuntime->SetAccountErrorText(ErrorText);
		}
		return;
	}

	SetGlueRealmListFromSummaries(Realms);
}

void AWoWLoginGameMode::SetGlueRealmListFromSummaries(const TArray<FWoWGlueRealmSummary>& Realms)
{
	CachedGlueRealmRows.Reset();

	for (const FWoWGlueRealmSummary& Realm : Realms)
	{
		CachedGlueRealmRows.Add(FString::Printf(
			TEXT("%d\t%s\t%d\t%d\t%.3f\t%d\t%s\t%d\t%d\t%d\t%d\t%d\t%d\t%d"),
			Realm.Index,
			*Realm.Name,
			static_cast<int32>(Realm.Icon),
			static_cast<int32>(Realm.CharacterCount),
			Realm.Population,
			static_cast<int32>(Realm.Lock),
			*Realm.Address,
			static_cast<int32>(Realm.RealmId),
			static_cast<int32>(Realm.Flags),
			static_cast<int32>(Realm.Timezone),
			static_cast<int32>(Realm.MajorVersion),
			static_cast<int32>(Realm.MinorVersion),
			static_cast<int32>(Realm.PatchVersion),
			static_cast<int32>(Realm.Build)));
	}
}

void AWoWLoginGameMode::PushGlueStateToLua()
{
	if (!LoginGlueLuaRuntime.IsValid())
	{
		return;
	}

	LoginGlueLuaRuntime->SetCharacterList(CachedGlueCharacterRows);
	LoginGlueLuaRuntime->SetRealmList(CachedGlueRealmRows);
	LoginGlueLuaRuntime->SetCharacterCreateDefaults(LastCreatedRace, LastCreatedClass, LastCreatedGender);
}
