#include "WoWCharacterPreviewComponent.h"
#include "WoWCharacterEquipment.h"
#include "WoWGameInstance.h"
#include "WoWM2ModelComponent.h"
#include "WoWM2PreviewAnimInstance.h"
#include "WoWRuntimeCharacterAppearance.h"

#include "Components/CapsuleComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/World.h"
#include "Engine/Texture2D.h"
#include "Camera/PlayerCameraManager.h"
#include "Animation/AnimSequence.h"
#include "Animation/AnimationAsset.h"
#include "GameFramework/Character.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "KismetProceduralMeshLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "SkeletalRenderPublic.h"
#include "UObject/SoftObjectPath.h"

static TAutoConsoleVariable<int32> CVarWoWCharacterPreviewUpdateHz(
	TEXT("wow.CharacterPreviewUpdateHz"),
	0,
	TEXT("Caps expensive CPU-side character preview updates. 0 or lower updates every frame."));

static TAutoConsoleVariable<int32> CVarWoWCharacterPreviewLegacyBodyTick(
	TEXT("wow.CharacterPreviewLegacyBodyTick"),
	0,
	TEXT("Updates the hidden legacy procedural character body even when generated SkeletalMesh is the primary visual."));

static TAutoConsoleVariable<int32> CVarWoWCharacterPreviewRibbons(
	TEXT("wow.CharacterPreviewRibbons"),
	1,
	TEXT("Enables legacy attached M2 ribbon updates in the character preview component."));

namespace
{
constexpr const TCHAR* PreviewComponentName = TEXT("WoWM2AttachedPreview");
constexpr const TCHAR* GeneratedSkeletalComponentName = TEXT("WoWGeneratedSkeletalPreview");
constexpr const TCHAR* GeneratedGripPoseSourceComponentName = TEXT("WoWGeneratedGripPoseSource");
constexpr const TCHAR* GeneratedPoseableComponentName = TEXT("WoWGeneratedPoseablePreview");
constexpr const TCHAR* AttachedWeaponComponentName = TEXT("WoWM2AttachedWeaponPreview");
constexpr const TCHAR* AttachedRibbonComponentName = TEXT("WoWM2AttachedRibbonPreview");
constexpr const TCHAR* ItemObjectComponentManifestRelativePath = TEXT("WoWData/AssetLibraryBuild/ItemObjectComponents.all.manifest.json");
constexpr const TCHAR* ItemObjectComponentContentManifestRelativePath = TEXT("WoW/Generated/Manifests/ItemObjectComponents.all.manifest.json");
constexpr const TCHAR* PreviewTextureParameterName = TEXT("DiffuseTexture");
constexpr const TCHAR* PreviewTintParameterName = TEXT("TintColor");
constexpr bool bForceTwoSidedOpaquePreview = true;
constexpr float BackpedalAnimationPlaybackRate = 1.18f;
constexpr int32 M2BoneFlagBillboard = 0x08;
// Ribbon 发射点几乎没移动时不要新增历史边；否则会把整张 ZAP1B 压成短白条堆在光团周围。
constexpr float RibbonMinEdgeDistanceWorldUnits = 1.25f;
// 低透明度的 M2 additive pass 在 UE HDR/Additive 下会比 D3D9/WoW 显得更硬、更白；只压低 alpha<0.25 的弱光层。
constexpr float M2LowAlphaAdditiveTintScale = 0.25f;
// 使用已生成的 UE 原生 SkeletalMesh/UAnimSequence 作为主角色显示路径。
// 程序化M2预览仍在后台计算挂点/武器/Ribbon，等原生骨骼路径覆盖装备与特效后再完全移除。
constexpr bool bUseGeneratedSkeletalPreviewAsPrimaryVisual = true;
constexpr const TCHAR* PlayerCharacterManifestRelativePath = TEXT("WoWAssetManifests/PlayerCharacters.json");

// 原地跳落地时允许播完整 JumpEnd。具体时长来自当前角色 M2/AnimationData。
constexpr float JumpLandMaxDurationSeconds = 1.00f;

// 移动中落地使用 JumpLandRun(187) 作为连接覆盖层，不抢占跑步主动画；时间越长，落地后继续往前带的惯性越明显。
constexpr int32 MovingJumpLandRunAnimationId = 187;
constexpr float MovingJumpLandOverlayDurationSeconds = 0.42f;

// 移动落地覆盖层最大权重；数值过大会重新出现明显深蹲，数值过低会像没有落地缓冲。
constexpr float MovingJumpLandOverlayMaxAlpha = 1.00f;

// JumpLandRun 本身就是跑动落地连接动作，从起始帧播放能保留触地后的前冲衔接。
constexpr float MovingJumpLandOverlayStartTimeSeconds = 0.0f;

// X键坐下相关M2动画ID，来自本地WotLK AnimationData.dbc。
constexpr int32 SitGroundDownAnimationId = 96;
constexpr int32 SitGroundLoopAnimationId = 97;
constexpr int32 SitGroundUpAnimationId = 98;
constexpr int32 FallbackWeaponSheatheAnimationId = 89;
constexpr const TCHAR* WeaponSheatheAnimationName = TEXT("Sheath");
// M2 events 缺失时才使用这个兜底比例。正常角色模型应优先从 $SHL/$SHR 等事件得到切换时间。
constexpr float WeaponSheatheAttachmentSwitchFallbackAlpha = 0.50f;
constexpr float WeaponSheatheOverlayDefaultBlendSeconds = 0.15f;

// WMV / M2 POSITION_SLOTS：1=ATT_RIGHT_PALM，2=ATT_LEFT_PALM。手持武器必须走 attachment_lookup，不走 KeyBoneLookup。
constexpr int32 RightPalmAttachmentId = 1;
constexpr int32 LeftPalmAttachmentId = 2;
constexpr int32 GeneratedGripVariantNone = 0;
constexpr int32 GeneratedGripVariantRight = 1;
constexpr int32 GeneratedGripVariantLeft = 2;
// 武器收纳说明：
// 1. M2 自身只提供 attachment_lookup / attachments，即“可挂在哪里”的骨骼挂点。
// 2. 真正装备收纳时选哪个挂点，来自物品的 SheatheType/装备槽逻辑；WMV 源码也证明会在逻辑层额外套模型旋转。
// 3. 当前还没有完整 Item.dbc 装备系统，所以这里先把常见背部挂点和 WMV 旋转列成调试候选。
// 4. 你自己微调时，先在 UE 中用 C 切换 DebugSheathedAttachmentIds，用 V 切换 DebugSheathedRotations，
//    屏幕提示会显示 attach 和 rotIndex；找到最接近 WoW 的组合后，再把默认值固定到下面两处。
// WotLK M2 attachment_lookup：26=AT_SheathMainHand。正式收鞘先使用角色M2自己的主手收鞘挂点。
// Item.dbc SheatheType 后续只负责选择“哪个收鞘语义ID”，不应该替代 M2 attachment 的位置数据。
constexpr int32 LargeWeaponSheathedAttachmentId = 26;

// C 键循环的“背部/收纳挂点”候选：
// 30=ATT_LEFT_BACK，31=ATT_RIGHT_BACK，26/27=左右背部剑鞘挂点，28=盾牌中背，12=普通背部挂点。
// 如果你发现某个 attach 最像 WoW，可以把 LargeWeaponSheathedAttachmentId 改成那个数字作为默认值。
constexpr int32 DebugSheathedAttachmentIds[] = { 30, 31, 26, 27, 28, 12 };

// V 键循环的“收纳武器额外旋转”候选：
// 这些不是 M2 attachment 自带值，而是 WMV/客户端装备逻辑层会叠加的模型旋转。
// 如果某个 rotIndex 最接近 WoW，就把 ActiveWeaponSheathedRotationIndex 的默认值改成对应索引。
const FRotator DebugSheathedRotations[] = {
	FRotator(135.0f, 90.0f, 90.0f),
	FRotator(225.0f, 90.0f, 90.0f),
	FRotator(0.0f, 90.0f, 90.0f)
};

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

struct FGeneratedPlayerCharacterRuntimeDefinition
{
	int32 Race = 0;
	int32 Gender = 0;
	FString Id;
	FString SourceModelPath;
	FString MeshJsonPath;
	FString MeshObjectPath;
	FString MaterialPackagePath;
	FString AnimationPackagePath;
	FString AnimationStem;
};

using FRuntimeCharacterEquipmentDisplay = FWoWCharacterEquipmentDisplay;

struct FRuntimeCharacterItemDisplay
{
	int32 DisplayId = 0;
	int32 GeosetGroups[3] = {};
	int32 HelmetGeosetVisMale = 0;
	int32 HelmetGeosetVisFemale = 0;
};

struct FRuntimeCharacterHelmetVis
{
	int32 Id = 0;
	uint32 HairFlags = 0;
	uint32 Facial1Flags = 0;
	uint32 Facial2Flags = 0;
	uint32 Facial3Flags = 0;
	uint32 EarsFlags = 0;
};

struct FRuntimeCharacterHairGeoset
{
	int32 Race = 0;
	int32 Gender = 0;
	int32 HairStyle = 0;
	int32 GeosetId = 0;
	bool bBald = false;
};

struct FRuntimeCharacterFacialHairStyle
{
	int32 Race = 0;
	int32 Gender = 0;
	int32 Style = 0;
	int32 Geoset100 = 0;
	int32 Geoset200 = 0;
	int32 Geoset300 = 0;
};

struct FRuntimeCharacterAppearanceDbcCache
{
	bool bLoaded = false;
	TMap<int32, FRuntimeCharacterItemDisplay> ItemDisplayInfo;
	TMap<int32, FRuntimeCharacterHelmetVis> HelmetGeosetVisData;
	TArray<FRuntimeCharacterHairGeoset> HairGeosets;
	TArray<FRuntimeCharacterFacialHairStyle> FacialHairStyles;
};

const TCHAR* GetAttachmentDebugName(int32 AttachmentId)
{
	switch (AttachmentId)
	{
	case 12:
		return TEXT("ATT_BACK");
	case 26:
		return TEXT("ATT_RIGHT_BACK_SHEATH");
	case 27:
		return TEXT("ATT_LEFT_BACK_SHEATH");
	case 28:
		return TEXT("ATT_MIDDLE_BACK_SHEATH");
	case 30:
		return TEXT("ATT_LEFT_BACK");
	case 31:
		return TEXT("ATT_RIGHT_BACK");
	case 32:
		return TEXT("ATT_LEFT_HIP_SHEATH");
	case 33:
		return TEXT("ATT_RIGHT_HIP_SHEATH");
	default:
		return TEXT("ATT_UNKNOWN");
	}
}

const TCHAR* GetM2RuntimeAnimationName(int32 AnimationId)
{
	switch (AnimationId)
	{
	case 0: return TEXT("Stand");
	case 1: return TEXT("Death");
	case 3: return TEXT("Stop");
	case 4: return TEXT("Walk");
	case 5: return TEXT("Run");
	case 8: return TEXT("StandWound");
	case 9: return TEXT("CombatWound");
	case 10: return TEXT("CombatCritical");
	case 11: return TEXT("ShuffleLeft");
	case 12: return TEXT("ShuffleRight");
	case 13: return TEXT("WalkBackwards");
	case 14: return TEXT("Stun");
	case 15: return TEXT("HandsClosed");
	case 16: return TEXT("AttackUnarmed");
	case 17: return TEXT("Attack1H");
	case 18: return TEXT("Attack2H");
	case 19: return TEXT("Attack2HL");
	case 20: return TEXT("ParryUnarmed");
	case 21: return TEXT("Parry1H");
	case 22: return TEXT("Parry2H");
	case 23: return TEXT("Parry2HL");
	case 24: return TEXT("ShieldBlock");
	case 25: return TEXT("ReadyUnarmed");
	case 26: return TEXT("Ready1H");
	case 27: return TEXT("Ready2H");
	case 28: return TEXT("Ready2HL");
	case 29: return TEXT("ReadyBow");
	case 30: return TEXT("Dodge");
	case 37: return TEXT("JumpStart");
	case 38: return TEXT("Jump");
	case 39: return TEXT("JumpEnd");
	case 40: return TEXT("Fall");
	case 41: return TEXT("SwimIdle");
	case 42: return TEXT("Swim");
	case 50: return TEXT("Loot");
	case 55: return TEXT("BattleRoar");
	case 60: return TEXT("EmoteTalk");
	case 66: return TEXT("EmoteBow");
	case 67: return TEXT("EmoteWave");
	case 68: return TEXT("EmoteCheer");
	case 69: return TEXT("EmoteDance");
	case 70: return TEXT("EmoteLaugh");
	case 72: return TEXT("EmoteSitGround");
	case 80: return TEXT("EmoteApplaud");
	case 82: return TEXT("EmoteFlex");
	case 89: return TEXT("Sheath");
	case 90: return TEXT("HipSheath");
	case 91: return TEXT("Mount");
	case 95: return TEXT("Kick");
	case 96: return TEXT("SitGroundDown");
	case 97: return TEXT("SitGround");
	case 98: return TEXT("SitGroundUp");
	case 99: return TEXT("SleepDown");
	case 100: return TEXT("Sleep");
	case 101: return TEXT("SleepUp");
	case 124: return TEXT("ChannelCastDirected");
	case 125: return TEXT("ChannelCastOmni");
	case 126: return TEXT("Whirlwind");
	case 131: return TEXT("Drown");
	case 132: return TEXT("Drowned");
	case 133: return TEXT("FishingCast");
	case 134: return TEXT("FishingLoop");
	case 137: return TEXT("EmoteStunNoSheathe");
	case 139: return TEXT("SpellSleepDown");
	case 143: return TEXT("Sprint");
	case 181: return TEXT("DragonStomp");
	case 185: return TEXT("EmoteYes");
	case 186: return TEXT("EmoteNo");
	case 187: return TEXT("JumpLandRun");
	case 193: return TEXT("Hover");
	case 195: return TEXT("EmoteTrain");
	case 199: return TEXT("EmoteEatNoSheathe");
	case 211: return TEXT("EmoteDanceSpecial");
	case 212: return TEXT("Mutilate");
	case 223: return TEXT("StealthRun");
	case 225: return TEXT("Cower");
	case 474: return TEXT("Strangulate");
	case 476: return TEXT("ReadyJoust");
	case 477: return TEXT("LoadJoust");
	case 478: return TEXT("HoldJoust");
	case 482: return TEXT("AttackJoust");
	case 484: return TEXT("ReclinedMount");
	case 500: return TEXT("ReclinedMountPassenger");
	default: return nullptr;
	}
}

void ApplyGeneratedPlayerCharacterMaterials(USkinnedMeshComponent* SkeletalPreview, const FString& MaterialPackagePathOverride)
{
	if (!SkeletalPreview)
	{
		return;
	}

	const USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(SkeletalPreview->GetSkinnedAsset());
	if (!SkeletalMesh)
	{
		return;
	}

	const TArray<FSkeletalMaterial>& MeshMaterials = SkeletalMesh->GetMaterials();
	const int32 MaterialCount = MeshMaterials.Num();
	int32 AppliedMaterialCount = 0;
	for (int32 MaterialIndex = 0; MaterialIndex < MaterialCount; ++MaterialIndex)
	{
		const FName SlotName = MeshMaterials[MaterialIndex].MaterialSlotName;
		const FString SlotText = SlotName.ToString();
		int32 SectionId = INDEX_NONE;
		if (SlotText.StartsWith(TEXT("M2_Section_")))
		{
			LexFromString(SectionId, *SlotText.RightChop(11));
		}
		if (SectionId == INDEX_NONE)
		{
			continue;
		}

		if (MaterialPackagePathOverride.IsEmpty())
		{
			UE_LOG(LogTemp, Error, TEXT("Generated player material package is empty for mesh=%s"), *SkeletalMesh->GetPathName());
			return;
		}
		const FString MaterialPath = FString::Printf(
			TEXT("%s/MI_M2_Section_%03d.MI_M2_Section_%03d"),
			*MaterialPackagePathOverride,
			SectionId,
			SectionId);
		UMaterialInterface* Material = ResolveLoadedObject<UMaterialInterface>(MaterialPath);
		if (Material)
		{
			// 中文说明：UE原生 SkeletalMesh 资产桥接阶段显式绑定每个 M2 section 的材质实例，
			// 避免编辑器缓存/旧资产导致运行时仍显示默认灰黑材质。
			SkeletalPreview->SetMaterial(MaterialIndex, Material);
			++AppliedMaterialCount;
		}
		else
		{
			UE_LOG(LogTemp, Verbose, TEXT("Generated player material not preloaded; skip sync load: mesh=%s slot=%d material=%s"),
				*SkeletalMesh->GetPathName(),
				MaterialIndex,
				*MaterialPath);
		}
	}

	UE_LOG(LogTemp, Verbose, TEXT("Generated player material binding: slots=%d applied=%d component=%s mesh=%s package=%s"),
		MaterialCount,
		AppliedMaterialCount,
		*SkeletalPreview->GetName(),
		*SkeletalMesh->GetPathName(),
		*MaterialPackagePathOverride);
	for (int32 MaterialIndex = 0; MaterialIndex < SkeletalPreview->GetNumMaterials(); ++MaterialIndex)
	{
		UMaterialInterface* Material = SkeletalPreview->GetMaterial(MaterialIndex);
		UE_LOG(LogTemp, Verbose, TEXT("Generated player material slot[%d]=%s"),
			MaterialIndex,
			Material ? *Material->GetPathName() : TEXT("None"));
	}
}

USkeletalMesh* LoadGeneratedPlayerCharacterMesh(const FString& PreferredObjectPath, FString& OutObjectPath)
{
	if (!PreferredObjectPath.IsEmpty())
	{
		OutObjectPath = PreferredObjectPath;
		if (USkeletalMesh* PreferredMesh = ResolveLoadedObject<USkeletalMesh>(PreferredObjectPath))
		{
			return PreferredMesh;
		}
		return nullptr;
	}
	OutObjectPath.Empty();
	return nullptr;
}

FString BuildRuntimeAppearanceMaterialPackagePath(const FString& MeshObjectPath)
{
	FString MeshPackagePath = MeshObjectPath;
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

	const FString PackageRoot = FPaths::GetPath(MeshPackagePath);
	return PackageRoot.IsEmpty() || MeshAssetName.IsEmpty()
		? FString()
		: PackageRoot / (MeshAssetName + TEXT("_Materials"));
}

void HideNativeCharacterMesh(ACharacter* Character)
{
	if (!Character)
	{
		return;
	}

	if (USkeletalMeshComponent* CharacterMesh = Character->GetMesh())
	{
		// 中文说明：Character 默认 SkeletalMesh 不能参与渲染；否则会和 M2 原生预览叠在一起，
		// 画面会像黑色高光塑料壳，误判成 M2 材质/贴图失败。
		CharacterMesh->SetVisibility(false, true);
		CharacterMesh->SetHiddenInGame(true, true);
		CharacterMesh->SetRenderInMainPass(false);
		CharacterMesh->SetRenderInDepthPass(false);
		CharacterMesh->SetCastShadow(false);
	}
}

// 武器挂点微调：当前程序化预览还不是完整装备系统，这些值只负责让测试物品贴近角色 M2 attachment。
const FVector AttachedWeaponLocationOffset = FVector(0.0f, 0.0f, 0.0f);
const FRotator AttachedWeaponRotationOffset = FRotator(0.0f, 0.0f, 0.0f);
// 收纳到背部时不能复用手持偏移。
// 这两个值就是你后面“慢慢微调”的地方：
// - LocationOffset：X=前后，Y=左右，Z=上下。
// - RotationOffset：Pitch/Yaw/Roll 额外角度。
// 先用 attachment + WMV 预设旋转把方向找近，再只动这里做最后修正。
const FVector SheathedBackWeaponLocationOffset = FVector(0.0f, 0.0f, 0.0f);
const FRotator SheathedBackWeaponRotationOffset = FRotator(0.0f, 0.0f, 0.0f);
const FVector DefaultDebugSheathedBackWeaponLocationOffset = FVector(9.088f, -7.008f, -50.697f);
const FRotator DefaultDebugSheathedBackWeaponRotationOffset = FRotator(26.138878f, 46.833817f, 171.520889f);

void SetLegacyPreviewBodyRendered(UProceduralMeshComponent* PreviewMesh, bool bRendered)
{
	if (!PreviewMesh)
	{
		return;
	}

	// 中文说明：旧 ProceduralMesh 现在只负责挂点/武器/Ribbon 的运行时计算。
	// 当 UE 原生 SkeletalMesh 作为主显示时，必须彻底关闭它的主渲染/深度渲染，避免黑色旧身体叠在新资产前面。
	PreviewMesh->SetVisibility(bRendered, false);
	PreviewMesh->SetHiddenInGame(!bRendered, false);
	PreviewMesh->SetRenderInMainPass(bRendered);
	PreviewMesh->SetRenderInDepthPass(bRendered);
	PreviewMesh->SetCastShadow(false);
}

int32 ResolveStrafeDirection(bool bLeftHeld, bool bRightHeld, float StrafeInput)
{
	if (StrafeInput < -0.1f || (bLeftHeld && !bRightHeld))
	{
		return -1;
	}
	if (StrafeInput > 0.1f || (bRightHeld && !bLeftHeld))
	{
		return 1;
	}
	return 0;
}

struct FWoWPreviewSubmesh
{
	int32 Id = 0;
	int32 TriangleStart = 0;
	int32 TriangleCount = 0;
};

struct FWoWPreviewSection
{
	int32 SubmeshId = 0;
	int32 MaterialFlags = 0;
	int32 RenderMode = 0;
	int32 TextureUnit = 0;
	int32 ColorIndex = INDEX_NONE;
	int32 TransparencyIndex = INDEX_NONE;
	int32 TextureTransformIndex = INDEX_NONE;
	bool bUseEnvMap = false;
	bool bTextureTransformStatic = true;
	bool bTwoSided = false;
	bool bUnlit = false;
	bool bBillboard = false;
	bool bNoDepthTest = false;
	bool bNoDepthWrite = false;
	int32 TextureIndex = -1;
	int32 TextureType = INDEX_NONE;
	FString PreviewPng;
	TArray<FVector> Positions;
	TArray<FIntVector4> BoneWeights;
	TArray<FIntVector4> BoneIndices;
	TArray<FVector> Normals;
	TArray<FVector2D> UV0;
	TArray<FWoWTextureTransformSample> TextureTransformSamples;
	TArray<FWoWSectionColorSample> ColorSamples;
	TArray<FLinearColor> VertexColors;
	TArray<int32> Indices;
};

struct FRuntimeSectionGeosetInfo
{
	int32 SubmeshId = 0;
	int32 TextureType = INDEX_NONE;
	int32 RenderMode = INDEX_NONE;
	int32 MaterialFlags = 0;
	bool bUnlit = false;
	bool bNoDepthWrite = false;
	FString PreviewPng;
};

bool ReadRuntimeSectionGeosetInfoFromMetadata(const FString& MetadataPath, TArray<FRuntimeSectionGeosetInfo>& OutSections)
{
	OutSections.Reset();
	if (MetadataPath.IsEmpty())
	{
		return false;
	}

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *MetadataPath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* SectionValues = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("sections"), SectionValues) || !SectionValues)
	{
		return false;
	}

	OutSections.Reserve(SectionValues->Num());
	for (const TSharedPtr<FJsonValue>& Value : *SectionValues)
	{
		const TSharedPtr<FJsonObject>* SectionObject = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(SectionObject) || !SectionObject || !SectionObject->IsValid())
		{
			continue;
		}
		double Number = 0.0;
		FRuntimeSectionGeosetInfo Section;
		Section.SubmeshId = (*SectionObject)->TryGetNumberField(TEXT("submesh_id"), Number)
			? FMath::RoundToInt(Number)
			: 0;
		Section.TextureType = (*SectionObject)->TryGetNumberField(TEXT("texture_type"), Number)
			? FMath::RoundToInt(Number)
			: INDEX_NONE;
		Section.RenderMode = (*SectionObject)->TryGetNumberField(TEXT("render_mode"), Number)
			? FMath::RoundToInt(Number)
			: INDEX_NONE;
		Section.MaterialFlags = (*SectionObject)->TryGetNumberField(TEXT("material_flags"), Number)
			? FMath::RoundToInt(Number)
			: 0;
		(*SectionObject)->TryGetBoolField(TEXT("unlit"), Section.bUnlit);
		(*SectionObject)->TryGetBoolField(TEXT("no_depth_write"), Section.bNoDepthWrite);
		(*SectionObject)->TryGetStringField(TEXT("preview_png"), Section.PreviewPng);
		OutSections.Add(MoveTemp(Section));
	}
	return !OutSections.IsEmpty();
}

bool ReadRuntimeSectionSubmeshIdsFromMetadata(const FString& MetadataPath, TArray<int32>& OutSubmeshIds)
{
	OutSubmeshIds.Reset();

	TArray<FRuntimeSectionGeosetInfo> Sections;
	if (!ReadRuntimeSectionGeosetInfoFromMetadata(MetadataPath, Sections))
	{
		return false;
	}

	OutSubmeshIds.Reserve(Sections.Num());
	for (const FRuntimeSectionGeosetInfo& Section : Sections)
	{
		OutSubmeshIds.Add(Section.SubmeshId);
	}
	return !OutSubmeshIds.IsEmpty();
}

int32 ResolveM2SectionIndexFromMaterialSlot(const USkeletalMesh* SkeletalMesh, int32 MaterialIndex)
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

void ApplySkeletalM2SectionVisibility(
	USkeletalMeshComponent* SkeletalComponent,
	const TArray<int32>& SectionSubmeshIds,
	const TFunctionRef<bool(int32, int32)> IsSubmeshVisibleFunc,
	int32& OutApplied,
	int32& OutVisible)
{
	OutApplied = 0;
	OutVisible = 0;
	if (!SkeletalComponent)
	{
		return;
	}

	const USkeletalMesh* SkeletalMesh = SkeletalComponent->GetSkeletalMeshAsset();
	const FSkeletalMeshRenderData* RenderData = SkeletalMesh ? SkeletalMesh->GetResourceForRendering() : nullptr;
	if (!SkeletalMesh || !RenderData || !RenderData->LODRenderData.IsValidIndex(0))
	{
		return;
	}

	const FSkeletalMeshLODRenderData& LODData = RenderData->LODRenderData[0];
	for (int32 RenderSectionIndex = 0; RenderSectionIndex < LODData.RenderSections.Num(); ++RenderSectionIndex)
	{
		const FSkelMeshRenderSection& RenderSection = LODData.RenderSections[RenderSectionIndex];
		const int32 MaterialIndex = RenderSection.MaterialIndex;
		const int32 M2SectionIndex = ResolveM2SectionIndexFromMaterialSlot(SkeletalMesh, MaterialIndex);
		if (!SectionSubmeshIds.IsValidIndex(M2SectionIndex))
		{
			UE_LOG(LogTemp, Warning, TEXT("Runtime geoset section mapping missing: component=%s renderSection=%d material=%d m2Section=%d mappings=%d mesh=%s"),
				*SkeletalComponent->GetName(),
				RenderSectionIndex,
				MaterialIndex,
				M2SectionIndex,
				SectionSubmeshIds.Num(),
				SkeletalMesh ? *SkeletalMesh->GetPathName() : TEXT("<null>"));
			SkeletalComponent->ShowMaterialSection(MaterialIndex, RenderSectionIndex, false, 0);
			++OutApplied;
			continue;
		}
		const int32 SubmeshId = SectionSubmeshIds[M2SectionIndex];
		const bool bVisible = IsSubmeshVisibleFunc(SubmeshId, M2SectionIndex);
		SkeletalComponent->ShowMaterialSection(MaterialIndex, RenderSectionIndex, bVisible, 0);
		if (RenderSectionIndex < 80)
		{
			const FString SlotName = SkeletalMesh->GetMaterials().IsValidIndex(MaterialIndex)
				? SkeletalMesh->GetMaterials()[MaterialIndex].MaterialSlotName.ToString()
				: FString(TEXT("<invalid>"));
			UE_LOG(LogTemp, Verbose, TEXT("Runtime geoset section map: component=%s renderSection=%d material=%d slot=%s m2Section=%d submesh=%d visible=%d"),
				*SkeletalComponent->GetName(),
				RenderSectionIndex,
				MaterialIndex,
				*SlotName,
				M2SectionIndex,
				SubmeshId,
				bVisible ? 1 : 0);
		}
		++OutApplied;
		OutVisible += bVisible ? 1 : 0;
	}
}

struct FWoWPreviewM2Material
{
	int32 Index = INDEX_NONE;
	int32 MaterialFlags = 0;
	int32 RenderMode = 0;
	bool bTwoSided = false;
	bool bUnlit = false;
	bool bUnfogged = false;
	bool bBillboard = false;
	bool bNoDepthTest = false;
	bool bNoDepthWrite = false;
};

struct FWoWPreviewMaterials
{
	TObjectPtr<UMaterialInterface> OpaqueOneSided = nullptr;
	TObjectPtr<UMaterialInterface> OpaqueTwoSided = nullptr;
	TObjectPtr<UMaterialInterface> MaskedTwoSided = nullptr;
	TObjectPtr<UMaterialInterface> AdditiveAlphaUnlit = nullptr;
	TObjectPtr<UMaterialInterface> AdditiveUnlit = nullptr;
	TObjectPtr<UMaterialInterface> Fallback = nullptr;
};

bool ReadVectorArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FVector>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(FieldName, Values))
	{
		return false;
	}

	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
		if (!Value->TryGetArray(Tuple) || Tuple->Num() != 3)
		{
			return false;
		}
		OutValues.Emplace((*Tuple)[0]->AsNumber(), (*Tuple)[1]->AsNumber(), (*Tuple)[2]->AsNumber());
	}
	return true;
}

bool ReadVectorField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FVector& OutValue)
{
	const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
	if (!Object->TryGetArrayField(FieldName, Tuple) || Tuple->Num() != 3)
	{
		return false;
	}

	OutValue = FVector((*Tuple)[0]->AsNumber(), (*Tuple)[1]->AsNumber(), (*Tuple)[2]->AsNumber());
	return true;
}

bool ReadUVArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FVector2D>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(FieldName, Values))
	{
		return false;
	}

	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
		if (!Value->TryGetArray(Tuple) || Tuple->Num() != 2)
		{
			return false;
		}
		OutValues.Emplace((*Tuple)[0]->AsNumber(), (*Tuple)[1]->AsNumber());
	}
	return true;
}

bool ReadColorArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FLinearColor>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(FieldName, Values))
	{
		return false;
	}

	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
		if (!Value->TryGetArray(Tuple) || Tuple->Num() != 4)
		{
			return false;
		}
		OutValues.Emplace((*Tuple)[0]->AsNumber(), (*Tuple)[1]->AsNumber(), (*Tuple)[2]->AsNumber(), (*Tuple)[3]->AsNumber());
	}
	return true;
}

bool ReadTextureTransformSampleArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FWoWTextureTransformSample>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(FieldName, Values))
	{
		return true;
	}

	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* SampleObject = nullptr;
		if (!Value->TryGetObject(SampleObject) || !SampleObject || !SampleObject->IsValid())
		{
			return false;
		}

		double Number = 0.0;
		FWoWTextureTransformSample Sample;
		if ((*SampleObject)->TryGetNumberField(TEXT("time_ms"), Number))
		{
			Sample.TimeSeconds = static_cast<float>(Number / 1000.0);
		}
		if ((*SampleObject)->TryGetNumberField(TEXT("rotation_degrees"), Number))
		{
			Sample.RotationDegrees = static_cast<float>(Number);
		}

		const TArray<TSharedPtr<FJsonValue>>* Translation = nullptr;
		if ((*SampleObject)->TryGetArrayField(TEXT("translation"), Translation) && Translation->Num() >= 2)
		{
			Sample.Translation = FVector2D((*Translation)[0]->AsNumber(), (*Translation)[1]->AsNumber());
		}
		const TArray<TSharedPtr<FJsonValue>>* Scaling = nullptr;
		if ((*SampleObject)->TryGetArrayField(TEXT("scaling"), Scaling) && Scaling->Num() >= 2)
		{
			Sample.Scaling = FVector2D((*Scaling)[0]->AsNumber(), (*Scaling)[1]->AsNumber());
		}
		OutValues.Add(Sample);
	}
	return true;
}

bool ReadSectionColorSampleArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FWoWSectionColorSample>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(FieldName, Values))
	{
		return true;
	}

	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* SampleObject = nullptr;
		if (!Value->TryGetObject(SampleObject) || !SampleObject || !SampleObject->IsValid())
		{
			return false;
		}

		double Number = 0.0;
		FWoWSectionColorSample Sample;
		if ((*SampleObject)->TryGetNumberField(TEXT("time_ms"), Number))
		{
			Sample.TimeSeconds = static_cast<float>(Number / 1000.0);
		}

		const TArray<TSharedPtr<FJsonValue>>* ColorValues = nullptr;
		if ((*SampleObject)->TryGetArrayField(TEXT("color"), ColorValues) && ColorValues->Num() == 4)
		{
			Sample.Color.R = static_cast<float>((*ColorValues)[0]->AsNumber());
			Sample.Color.G = static_cast<float>((*ColorValues)[1]->AsNumber());
			Sample.Color.B = static_cast<float>((*ColorValues)[2]->AsNumber());
			Sample.Color.A = static_cast<float>((*ColorValues)[3]->AsNumber());
		}
		OutValues.Add(Sample);
	}
	return true;
}

bool ReadIndexArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<int32>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(FieldName, Values))
	{
		return false;
	}

	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		OutValues.Add(static_cast<int32>(Value->AsNumber()));
	}
	return true;
}

bool ReadIntVector4Array(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FIntVector4>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(FieldName, Values))
	{
		return false;
	}

	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
		if (!Value->TryGetArray(Tuple) || Tuple->Num() != 4)
		{
			return false;
		}
		OutValues.Emplace(
			static_cast<int32>((*Tuple)[0]->AsNumber()),
			static_cast<int32>((*Tuple)[1]->AsNumber()),
			static_cast<int32>((*Tuple)[2]->AsNumber()),
			static_cast<int32>((*Tuple)[3]->AsNumber()));
	}
	return true;
}

bool ReadMatrixFrameArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<TArray<FMatrix>>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* JsonFrames = nullptr;
	if (!Object->TryGetArrayField(FieldName, JsonFrames))
	{
		return false;
	}

	OutValues.Reserve(JsonFrames->Num());
	for (const TSharedPtr<FJsonValue>& FrameValue : *JsonFrames)
	{
		const TArray<TSharedPtr<FJsonValue>>* JsonBones = nullptr;
		if (!FrameValue->TryGetArray(JsonBones))
		{
			return false;
		}

		TArray<FMatrix> BoneMatrices;
		BoneMatrices.Reserve(JsonBones->Num());
		for (const TSharedPtr<FJsonValue>& BoneValue : *JsonBones)
		{
			const TArray<TSharedPtr<FJsonValue>>* JsonRows = nullptr;
			if (!BoneValue->TryGetArray(JsonRows) || JsonRows->Num() != 4)
			{
				return false;
			}

			FMatrix Matrix = FMatrix::Identity;
			for (int32 Row = 0; Row < 4; ++Row)
			{
				const TArray<TSharedPtr<FJsonValue>>* JsonColumns = nullptr;
				if (!(*JsonRows)[Row]->TryGetArray(JsonColumns) || JsonColumns->Num() != 4)
				{
					return false;
				}

				for (int32 Column = 0; Column < 4; ++Column)
				{
					Matrix.M[Row][Column] = static_cast<float>((*JsonColumns)[Column]->AsNumber());
				}
			}

			BoneMatrices.Add(Matrix);
		}
		OutValues.Add(MoveTemp(BoneMatrices));
	}
	return true;
}

bool ReadAnimationClipArray(const TSharedPtr<FJsonObject>& Object, TArray<FWoWCharacterPreviewAnimationClip>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(TEXT("animations"), Values))
	{
		return false;
	}

	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* ClipObject = nullptr;
		if (!Value->TryGetObject(ClipObject) || !ClipObject || !ClipObject->IsValid())
		{
			return false;
		}

		FWoWCharacterPreviewAnimationClip Clip;
		(*ClipObject)->TryGetStringField(TEXT("key"), Clip.Key);
		Clip.SequenceIndex = static_cast<int32>((*ClipObject)->GetNumberField(TEXT("sequence_index")));
		const TArray<TSharedPtr<FJsonValue>>* LookupIdValues = nullptr;
		if ((*ClipObject)->TryGetArrayField(TEXT("lookup_ids"), LookupIdValues))
		{
			Clip.LookupIds.Reserve(LookupIdValues->Num());
			for (const TSharedPtr<FJsonValue>& LookupIdValue : *LookupIdValues)
			{
				Clip.LookupIds.Add(static_cast<int32>(LookupIdValue->AsNumber()));
			}
		}
		Clip.AnimationId = static_cast<int32>((*ClipObject)->GetNumberField(TEXT("animation_id")));
		Clip.SubAnimationId = static_cast<int32>((*ClipObject)->GetNumberField(TEXT("sub_animation_id")));
		double LengthMs = 0.0;
		(*ClipObject)->TryGetNumberField(TEXT("length_ms"), LengthMs);
		Clip.LengthSeconds = static_cast<float>(FMath::Max(LengthMs / 1000.0, 0.0));
		double MovingSpeed = 0.0;
		(*ClipObject)->TryGetNumberField(TEXT("moving_speed"), MovingSpeed);
		Clip.MovingSpeed = static_cast<float>(MovingSpeed);
		double BlendTimeMs = 0.0;
		(*ClipObject)->TryGetNumberField(TEXT("blend_time_ms"), BlendTimeMs);
		Clip.BlendTimeSeconds = static_cast<float>(FMath::Max(BlendTimeMs / 1000.0, 0.0));
		double Frequency = 0.0;
		(*ClipObject)->TryGetNumberField(TEXT("frequency"), Frequency);
		Clip.Frequency = static_cast<int32>(Frequency);
		double NextAnimation = -1.0;
		(*ClipObject)->TryGetNumberField(TEXT("next_animation"), NextAnimation);
		Clip.NextAnimation = static_cast<int32>(NextAnimation);
		double AliasNext = 0.0;
		(*ClipObject)->TryGetNumberField(TEXT("alias_next"), AliasNext);
		Clip.AliasNext = static_cast<int32>(AliasNext);
		double SampleRate = 20.0;
		(*ClipObject)->TryGetNumberField(TEXT("sample_rate"), SampleRate);
		Clip.SampleRate = static_cast<float>(SampleRate > 0.0 ? SampleRate : 20.0);
		(*ClipObject)->TryGetBoolField(TEXT("loop_end_frame"), Clip.bLoopEndFrame);
		(*ClipObject)->TryGetBoolField(TEXT("external_anim"), Clip.bExternalAnim);
		if (!ReadMatrixFrameArray(*ClipObject, TEXT("frames"), Clip.Frames) || Clip.Frames.Num() == 0)
		{
			continue;
		}
		if (Clip.Key.IsEmpty())
		{
			Clip.Key = FString::Printf(TEXT("%d-%d"), Clip.AnimationId, Clip.SubAnimationId);
		}
		OutValues.Add(MoveTemp(Clip));
	}
	return true;
}

bool ReadIntArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<int32>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(FieldName, Values))
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		OutValues.Add(static_cast<int32>(Value->AsNumber()));
	}
	return true;
}

bool ReadBoneParentArray(const TSharedPtr<FJsonObject>& Object, TArray<int32>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(TEXT("bones"), Values))
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* BoneObject = nullptr;
		if (!Value->TryGetObject(BoneObject) || !BoneObject || !BoneObject->IsValid())
		{
			return false;
		}

		double Parent = -1.0;
		(*BoneObject)->TryGetNumberField(TEXT("parent"), Parent);
		OutValues.Add(static_cast<int32>(Parent));
	}
	return true;
}

bool ReadAttachmentArray(const TSharedPtr<FJsonObject>& Object, TArray<FWoWCharacterAttachment>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(TEXT("attachments"), Values))
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* AttachmentObject = nullptr;
		if (!Value->TryGetObject(AttachmentObject) || !AttachmentObject || !AttachmentObject->IsValid())
		{
			return false;
		}

		FWoWCharacterAttachment Attachment;
		double Number = 0.0;
		if ((*AttachmentObject)->TryGetNumberField(TEXT("index"), Number))
		{
			Attachment.Index = FMath::RoundToInt(Number);
		}
		if ((*AttachmentObject)->TryGetNumberField(TEXT("id"), Number))
		{
			Attachment.Id = FMath::RoundToInt(Number);
		}
		if ((*AttachmentObject)->TryGetNumberField(TEXT("bone"), Number))
		{
			Attachment.BoneIndex = FMath::RoundToInt(Number);
		}
		ReadVectorField(*AttachmentObject, TEXT("position"), Attachment.Position);

		if (Attachment.Index >= 0 && Attachment.Id >= 0 && Attachment.BoneIndex >= 0)
		{
			OutValues.Add(Attachment);
		}
	}
	return true;
}

bool ReadM2EventArray(const TSharedPtr<FJsonObject>& Object, TArray<FWoWM2EventRecord>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(TEXT("events"), Values))
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* EventObject = nullptr;
		if (!Value->TryGetObject(EventObject) || !EventObject || !EventObject->IsValid())
		{
			continue;
		}

		FWoWM2EventRecord Event;
		double Number = 0.0;
		if ((*EventObject)->TryGetNumberField(TEXT("index"), Number))
		{
			Event.Index = FMath::RoundToInt(Number);
		}
		(*EventObject)->TryGetStringField(TEXT("identifier"), Event.Identifier);
		if ((*EventObject)->TryGetNumberField(TEXT("bone"), Number))
		{
			Event.BoneIndex = FMath::RoundToInt(Number);
		}

		const TArray<TSharedPtr<FJsonValue>>* TrackValues = nullptr;
		if ((*EventObject)->TryGetArrayField(TEXT("timestamps"), TrackValues))
		{
			Event.TimestampTracks.Reserve(TrackValues->Num());
			for (const TSharedPtr<FJsonValue>& TrackValue : *TrackValues)
			{
				const TSharedPtr<FJsonObject>* TrackObject = nullptr;
				if (!TrackValue->TryGetObject(TrackObject) || !TrackObject || !TrackObject->IsValid())
				{
					continue;
				}

				FWoWM2EventTimestampTrack Track;
				if ((*TrackObject)->TryGetNumberField(TEXT("sequence_index"), Number))
				{
					Track.SequenceIndex = FMath::RoundToInt(Number);
				}
				if ((*TrackObject)->TryGetNumberField(TEXT("animation_id"), Number))
				{
					Track.AnimationId = FMath::RoundToInt(Number);
				}
				if ((*TrackObject)->TryGetNumberField(TEXT("sub_animation_id"), Number))
				{
					Track.SubAnimationId = FMath::RoundToInt(Number);
				}

				const TArray<TSharedPtr<FJsonValue>>* TimestampValues = nullptr;
				if ((*TrackObject)->TryGetArrayField(TEXT("timestamps_ms"), TimestampValues))
				{
					Track.TimestampsMs.Reserve(TimestampValues->Num());
					for (const TSharedPtr<FJsonValue>& TimestampValue : *TimestampValues)
					{
						Track.TimestampsMs.Add(FMath::RoundToInt(TimestampValue->AsNumber()));
					}
				}

				if (Track.AnimationId != INDEX_NONE && Track.TimestampsMs.Num() > 0)
				{
					Event.TimestampTracks.Add(MoveTemp(Track));
				}
			}
		}

		if (!Event.Identifier.IsEmpty())
		{
			OutValues.Add(MoveTemp(Event));
		}
	}
	return true;
}

bool ReadBoneMetadataArray(const TSharedPtr<FJsonObject>& Object, TArray<FWoWBoneMetadata>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(TEXT("bones"), Values))
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* BoneObject = nullptr;
		if (!Value->TryGetObject(BoneObject) || !BoneObject || !BoneObject->IsValid())
		{
			return false;
		}

		FWoWBoneMetadata Bone;
		double Number = 0.0;
		if ((*BoneObject)->TryGetNumberField(TEXT("index"), Number))
		{
			Bone.Index = FMath::RoundToInt(Number);
		}
		if ((*BoneObject)->TryGetNumberField(TEXT("flags"), Number))
		{
			Bone.Flags = FMath::RoundToInt(Number);
		}
		if ((*BoneObject)->TryGetNumberField(TEXT("parent"), Number))
		{
			Bone.Parent = FMath::RoundToInt(Number);
		}
		ReadVectorField(*BoneObject, TEXT("pivot"), Bone.Pivot);
		if (Bone.Index >= 0)
		{
			if (OutValues.Num() <= Bone.Index)
			{
				OutValues.SetNum(Bone.Index + 1);
			}
			OutValues[Bone.Index] = Bone;
		}
	}

	return true;
}

bool ReadRibbonEmitterArray(const TSharedPtr<FJsonObject>& Object, TArray<FWoWRibbonEmitter>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(TEXT("ribbons"), Values))
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* RibbonObject = nullptr;
		if (!Value->TryGetObject(RibbonObject) || !RibbonObject || !RibbonObject->IsValid())
		{
			return false;
		}

		FWoWRibbonEmitter Ribbon;
		double Number = 0.0;
		if ((*RibbonObject)->TryGetNumberField(TEXT("index"), Number))
		{
			Ribbon.Index = FMath::RoundToInt(Number);
		}
		if ((*RibbonObject)->TryGetNumberField(TEXT("id"), Number))
		{
			Ribbon.Id = FMath::RoundToInt(Number);
		}
		if ((*RibbonObject)->TryGetNumberField(TEXT("bone"), Number))
		{
			Ribbon.BoneIndex = FMath::RoundToInt(Number);
		}
		ReadVectorField(*RibbonObject, TEXT("position"), Ribbon.Position);
		(*RibbonObject)->TryGetStringField(TEXT("preview_png"), Ribbon.PreviewPng);
		ReadIntArray(*RibbonObject, TEXT("material_indices"), Ribbon.MaterialIndices);
		if ((*RibbonObject)->TryGetNumberField(TEXT("resolution"), Number))
		{
			Ribbon.EdgesPerSecond = FMath::Max(static_cast<float>(Number), 1.0f);
		}
		if ((*RibbonObject)->TryGetNumberField(TEXT("length"), Number))
		{
			// 010 / M2: length 是 EdgeLifeSpanInSec，单位是秒；不能乘模型单位或按空间长度裁剪。
			Ribbon.EdgeLifetimeSeconds = FMath::Max(static_cast<float>(Number), 1.0f / Ribbon.EdgesPerSecond);
		}
		if ((*RibbonObject)->TryGetNumberField(TEXT("gravity"), Number))
		{
			Ribbon.Gravity = static_cast<float>(Number);
		}
		if ((*RibbonObject)->TryGetNumberField(TEXT("texture_rows"), Number))
		{
			Ribbon.TextureRows = FMath::Max(FMath::RoundToInt(Number), 1);
		}
		if ((*RibbonObject)->TryGetNumberField(TEXT("texture_cols"), Number))
		{
			Ribbon.TextureCols = FMath::Max(FMath::RoundToInt(Number), 1);
		}

		const TArray<TSharedPtr<FJsonValue>>* SampleValues = nullptr;
		if ((*RibbonObject)->TryGetArrayField(TEXT("samples"), SampleValues))
		{
			Ribbon.Samples.Reserve(SampleValues->Num());
			for (const TSharedPtr<FJsonValue>& SampleValue : *SampleValues)
			{
				const TSharedPtr<FJsonObject>* SampleObject = nullptr;
				if (!SampleValue->TryGetObject(SampleObject) || !SampleObject || !SampleObject->IsValid())
				{
					continue;
				}

				FWoWRibbonSample Sample;
				if ((*SampleObject)->TryGetNumberField(TEXT("time_ms"), Number))
				{
					Sample.TimeSeconds = static_cast<float>(Number / 1000.0);
				}
				const TArray<TSharedPtr<FJsonValue>>* ColorValues = nullptr;
				if ((*SampleObject)->TryGetArrayField(TEXT("color"), ColorValues) && ColorValues->Num() == 3)
				{
					Sample.Color.R = static_cast<float>((*ColorValues)[0]->AsNumber());
					Sample.Color.G = static_cast<float>((*ColorValues)[1]->AsNumber());
					Sample.Color.B = static_cast<float>((*ColorValues)[2]->AsNumber());
				}
				if ((*SampleObject)->TryGetNumberField(TEXT("opacity"), Number))
				{
					Sample.Color.A = static_cast<float>(Number);
				}
				if ((*SampleObject)->TryGetNumberField(TEXT("above"), Number))
				{
					Sample.Above = static_cast<float>(Number);
				}
				if ((*SampleObject)->TryGetNumberField(TEXT("below"), Number))
				{
					Sample.Below = static_cast<float>(Number);
				}
				Ribbon.Samples.Add(Sample);
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* TexSlotValues = nullptr;
		if ((*RibbonObject)->TryGetArrayField(TEXT("tex_slot_samples"), TexSlotValues))
		{
			Ribbon.TexSlotSamples.Reserve(TexSlotValues->Num());
			for (const TSharedPtr<FJsonValue>& SlotValue : *TexSlotValues)
			{
				const TSharedPtr<FJsonObject>* SlotObject = nullptr;
				if (!SlotValue->TryGetObject(SlotObject) || !SlotObject || !SlotObject->IsValid())
				{
					continue;
				}

				FWoWRibbonTexSlotSample SlotSample;
				if ((*SlotObject)->TryGetNumberField(TEXT("time_ms"), Number))
				{
					SlotSample.TimeSeconds = static_cast<float>(Number / 1000.0);
				}
				if ((*SlotObject)->TryGetNumberField(TEXT("slot"), Number))
				{
					SlotSample.Slot = FMath::RoundToInt(Number);
				}
				Ribbon.TexSlotSamples.Add(SlotSample);
			}
		}
		const TArray<TSharedPtr<FJsonValue>>* VisibilityValues = nullptr;
		if ((*RibbonObject)->TryGetArrayField(TEXT("visibility_samples"), VisibilityValues))
		{
			Ribbon.VisibilitySamples.Reserve(VisibilityValues->Num());
			for (const TSharedPtr<FJsonValue>& VisibilityValue : *VisibilityValues)
			{
				const TSharedPtr<FJsonObject>* VisibilityObject = nullptr;
				if (!VisibilityValue->TryGetObject(VisibilityObject) || !VisibilityObject || !VisibilityObject->IsValid())
				{
					continue;
				}

				FWoWRibbonVisibilitySample VisibilitySample;
				if ((*VisibilityObject)->TryGetNumberField(TEXT("time_ms"), Number))
				{
					VisibilitySample.TimeSeconds = static_cast<float>(Number / 1000.0);
				}
				(*VisibilityObject)->TryGetBoolField(TEXT("visible"), VisibilitySample.bVisible);
				Ribbon.VisibilitySamples.Add(VisibilitySample);
			}
		}
		if (Ribbon.Samples.Num() > 0)
		{
			Ribbon.LengthSeconds = FMath::Max(Ribbon.Samples.Last().TimeSeconds, KINDA_SMALL_NUMBER);
		}

		if (Ribbon.BoneIndex >= 0)
		{
			OutValues.Add(MoveTemp(Ribbon));
		}
	}
	return true;
}

bool ReadM2MaterialArray(const TSharedPtr<FJsonObject>& Object, TArray<FWoWPreviewM2Material>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(TEXT("materials"), Values))
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* MaterialObject = nullptr;
		if (!Value->TryGetObject(MaterialObject) || !MaterialObject || !MaterialObject->IsValid())
		{
			return false;
		}

		FWoWPreviewM2Material Material;
		double Number = 0.0;
		if ((*MaterialObject)->TryGetNumberField(TEXT("index"), Number))
		{
			Material.Index = FMath::RoundToInt(Number);
		}
		if ((*MaterialObject)->TryGetNumberField(TEXT("flags"), Number))
		{
			Material.MaterialFlags = FMath::RoundToInt(Number);
		}
		if ((*MaterialObject)->TryGetNumberField(TEXT("render_mode"), Number))
		{
			Material.RenderMode = FMath::RoundToInt(Number);
		}
		(*MaterialObject)->TryGetBoolField(TEXT("two_sided"), Material.bTwoSided);
		(*MaterialObject)->TryGetBoolField(TEXT("unlit"), Material.bUnlit);
		(*MaterialObject)->TryGetBoolField(TEXT("unfogged"), Material.bUnfogged);
		(*MaterialObject)->TryGetBoolField(TEXT("billboard"), Material.bBillboard);
		(*MaterialObject)->TryGetBoolField(TEXT("no_depth_test"), Material.bNoDepthTest);
		(*MaterialObject)->TryGetBoolField(TEXT("no_depth_write"), Material.bNoDepthWrite);
		OutValues.Add(Material);
	}
	return true;
}

bool TryApplyM2MaterialToPreviewSection(const TArray<FWoWPreviewM2Material>& Materials, int32 MaterialIndex, FWoWPreviewSection& Section)
{
	const FWoWPreviewM2Material* Material = Materials.FindByPredicate([MaterialIndex](const FWoWPreviewM2Material& Candidate)
	{
		return Candidate.Index == MaterialIndex;
	});
	if (!Material)
	{
		return false;
	}

	Section.MaterialFlags = Material->MaterialFlags;
	Section.RenderMode = Material->RenderMode;
	Section.bTwoSided = Material->bTwoSided;
	Section.bUnlit = Material->bUnlit;
	Section.bBillboard = Material->bBillboard;
	Section.bNoDepthTest = Material->bNoDepthTest;
	Section.bNoDepthWrite = Material->bNoDepthWrite;
	return true;
}

bool ReadSectionArray(const TSharedPtr<FJsonObject>& Object, TArray<FWoWPreviewSection>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(TEXT("sections"), Values))
	{
		return false;
	}

	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* SectionObject = nullptr;
		if (!Value->TryGetObject(SectionObject) || !SectionObject || !SectionObject->IsValid())
		{
			return false;
		}

		FWoWPreviewSection Section;
		Section.SubmeshId = static_cast<int32>((*SectionObject)->GetNumberField(TEXT("submesh_id")));
		(*SectionObject)->TryGetNumberField(TEXT("material_flags"), Section.MaterialFlags);
		(*SectionObject)->TryGetNumberField(TEXT("render_mode"), Section.RenderMode);
		(*SectionObject)->TryGetNumberField(TEXT("texture_unit"), Section.TextureUnit);
		(*SectionObject)->TryGetNumberField(TEXT("color_index"), Section.ColorIndex);
		(*SectionObject)->TryGetNumberField(TEXT("transparency_index"), Section.TransparencyIndex);
		(*SectionObject)->TryGetNumberField(TEXT("texture_transform_index"), Section.TextureTransformIndex);
		(*SectionObject)->TryGetBoolField(TEXT("use_env_map"), Section.bUseEnvMap);
		(*SectionObject)->TryGetBoolField(TEXT("texture_transform_static"), Section.bTextureTransformStatic);
		(*SectionObject)->TryGetBoolField(TEXT("two_sided"), Section.bTwoSided);
		(*SectionObject)->TryGetBoolField(TEXT("unlit"), Section.bUnlit);
		(*SectionObject)->TryGetBoolField(TEXT("billboard"), Section.bBillboard);
		(*SectionObject)->TryGetBoolField(TEXT("no_depth_test"), Section.bNoDepthTest);
		(*SectionObject)->TryGetBoolField(TEXT("no_depth_write"), Section.bNoDepthWrite);
		Section.TextureIndex = static_cast<int32>((*SectionObject)->GetNumberField(TEXT("texture_index")));
		double Number = 0.0;
		Section.TextureType = (*SectionObject)->TryGetNumberField(TEXT("texture_type"), Number)
			? FMath::RoundToInt(Number)
			: INDEX_NONE;
		(*SectionObject)->TryGetStringField(TEXT("preview_png"), Section.PreviewPng);

		if (!ReadVectorArray(*SectionObject, TEXT("positions"), Section.Positions) ||
			!ReadIntVector4Array(*SectionObject, TEXT("bone_weights"), Section.BoneWeights) ||
			!ReadIntVector4Array(*SectionObject, TEXT("bone_indices"), Section.BoneIndices) ||
			!ReadVectorArray(*SectionObject, TEXT("normals"), Section.Normals) ||
			!ReadUVArray(*SectionObject, TEXT("uvs"), Section.UV0) ||
			!ReadTextureTransformSampleArray(*SectionObject, TEXT("texture_transform_samples"), Section.TextureTransformSamples) ||
			!ReadSectionColorSampleArray(*SectionObject, TEXT("color_samples"), Section.ColorSamples) ||
			!ReadColorArray(*SectionObject, TEXT("vertex_colors"), Section.VertexColors) ||
			!ReadIndexArray(*SectionObject, TEXT("indices"), Section.Indices))
		{
			return false;
		}
		OutValues.Add(MoveTemp(Section));
	}
	return true;
}

bool ReadSubmeshArray(const TSharedPtr<FJsonObject>& Object, TArray<FWoWPreviewSubmesh>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object->TryGetArrayField(TEXT("submeshes"), Values))
	{
		return false;
	}

	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* SubmeshObject = nullptr;
		if (!Value->TryGetObject(SubmeshObject) || !SubmeshObject || !SubmeshObject->IsValid())
		{
			return false;
		}
		FWoWPreviewSubmesh Submesh;
		Submesh.Id = static_cast<int32>((*SubmeshObject)->GetNumberField(TEXT("id")));
		Submesh.TriangleStart = static_cast<int32>((*SubmeshObject)->GetNumberField(TEXT("triangle_start")));
		Submesh.TriangleCount = static_cast<int32>((*SubmeshObject)->GetNumberField(TEXT("triangle_count")));
		OutValues.Add(Submesh);
	}
	return true;
}

TArray<FRuntimeCharacterEquipmentDisplay> ParseRuntimeEquipmentSummary(const FString& EquipmentSummary)
{
	return WoWCharacterEquipment::ParseEquipmentSummary(EquipmentSummary);
}

bool ReadRuntimeDbcUInt32(const TArray<uint8>& Data, int32 Offset, uint32& OutValue)
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

bool ReadRuntimeWdbcHeader(
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
	if (!ReadRuntimeDbcUInt32(OutData, 4, OutRecordCount) ||
		!ReadRuntimeDbcUInt32(OutData, 8, OutFieldCount) ||
		!ReadRuntimeDbcUInt32(OutData, 12, OutRecordSize) ||
		!ReadRuntimeDbcUInt32(OutData, 16, StringBlockSize))
	{
		return false;
	}

	const int64 ExpectedMinSize = 20ll + static_cast<int64>(OutRecordCount) * static_cast<int64>(OutRecordSize);
	return ExpectedMinSize <= OutData.Num();
}

bool ReadRuntimeDbcRecordUInt32(const TArray<uint8>& Data, uint32 RecordIndex, uint32 RecordSize, int32 FieldIndex, uint32& OutValue)
{
	const int32 Offset = 20 + static_cast<int32>(RecordIndex * RecordSize) + FieldIndex * 4;
	return ReadRuntimeDbcUInt32(Data, Offset, OutValue);
}

bool TryReadRuntimeAppearanceDbcFiles(const FString& DbcRoot, FRuntimeCharacterAppearanceDbcCache& OutCache)
{
	OutCache = FRuntimeCharacterAppearanceDbcCache();

	{
		const FString Path = DbcRoot / TEXT("ItemDisplayInfo.dbc");
		TArray<uint8> Data;
		uint32 RecordCount = 0;
		uint32 FieldCount = 0;
		uint32 RecordSize = 0;
		if (!ReadRuntimeWdbcHeader(Path, Data, RecordCount, FieldCount, RecordSize) || FieldCount != 25 || RecordSize != 100)
		{
			UE_LOG(LogTemp, Warning, TEXT("Runtime character appearance DBC read failed: %s"), *Path);
			return false;
		}

		for (uint32 RecordIndex = 0; RecordIndex < RecordCount; ++RecordIndex)
		{
			uint32 Value = 0;
			FRuntimeCharacterItemDisplay Record;

			ReadRuntimeDbcRecordUInt32(Data, RecordIndex, RecordSize, 0, Value);
			Record.DisplayId = static_cast<int32>(Value);
			ReadRuntimeDbcRecordUInt32(Data, RecordIndex, RecordSize, 7, Value);
			Record.GeosetGroups[0] = static_cast<int32>(Value);
			ReadRuntimeDbcRecordUInt32(Data, RecordIndex, RecordSize, 8, Value);
			Record.GeosetGroups[1] = static_cast<int32>(Value);
			ReadRuntimeDbcRecordUInt32(Data, RecordIndex, RecordSize, 9, Value);
			Record.GeosetGroups[2] = static_cast<int32>(Value);
			ReadRuntimeDbcRecordUInt32(Data, RecordIndex, RecordSize, 13, Value);
			Record.HelmetGeosetVisMale = static_cast<int32>(Value);
			ReadRuntimeDbcRecordUInt32(Data, RecordIndex, RecordSize, 14, Value);
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
		if (!ReadRuntimeWdbcHeader(Path, Data, RecordCount, FieldCount, RecordSize) || FieldCount != 8 || RecordSize != 32)
		{
			UE_LOG(LogTemp, Warning, TEXT("Runtime helmet geoset visibility DBC read failed: %s"), *Path);
			return false;
		}

		for (uint32 RecordIndex = 0; RecordIndex < RecordCount; ++RecordIndex)
		{
			uint32 Values[8] = {};
			for (int32 FieldIndex = 0; FieldIndex < 8; ++FieldIndex)
			{
				ReadRuntimeDbcRecordUInt32(Data, RecordIndex, RecordSize, FieldIndex, Values[FieldIndex]);
			}

			FRuntimeCharacterHelmetVis Record;
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
		if (!ReadRuntimeWdbcHeader(Path, Data, RecordCount, FieldCount, RecordSize) || FieldCount != 6 || RecordSize != 24)
		{
			UE_LOG(LogTemp, Warning, TEXT("Runtime hair geoset DBC read failed: %s"), *Path);
			return false;
		}

		for (uint32 RecordIndex = 0; RecordIndex < RecordCount; ++RecordIndex)
		{
			uint32 Value = 0;
			FRuntimeCharacterHairGeoset Record;
			ReadRuntimeDbcRecordUInt32(Data, RecordIndex, RecordSize, 1, Value);
			Record.Race = static_cast<int32>(Value);
			ReadRuntimeDbcRecordUInt32(Data, RecordIndex, RecordSize, 2, Value);
			Record.Gender = static_cast<int32>(Value);
			ReadRuntimeDbcRecordUInt32(Data, RecordIndex, RecordSize, 3, Value);
			Record.HairStyle = static_cast<int32>(Value);
			ReadRuntimeDbcRecordUInt32(Data, RecordIndex, RecordSize, 4, Value);
			Record.GeosetId = static_cast<int32>(Value);
			ReadRuntimeDbcRecordUInt32(Data, RecordIndex, RecordSize, 5, Value);
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
		if (!ReadRuntimeWdbcHeader(Path, Data, RecordCount, FieldCount, RecordSize) || FieldCount != 8 || RecordSize != 32)
		{
			UE_LOG(LogTemp, Warning, TEXT("Runtime facial hair geoset DBC read failed: %s"), *Path);
			return false;
		}

		for (uint32 RecordIndex = 0; RecordIndex < RecordCount; ++RecordIndex)
		{
			uint32 Values[8] = {};
			for (int32 FieldIndex = 0; FieldIndex < 8; ++FieldIndex)
			{
				ReadRuntimeDbcRecordUInt32(Data, RecordIndex, RecordSize, FieldIndex, Values[FieldIndex]);
			}

			FRuntimeCharacterFacialHairStyle Record;
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

const FRuntimeCharacterAppearanceDbcCache& GetRuntimeAppearanceDbcCache()
{
	static FRuntimeCharacterAppearanceDbcCache Cache;
	static bool bAttemptedLoad = false;
	if (!bAttemptedLoad)
	{
		bAttemptedLoad = true;
		const FString DbcRoot = FPaths::ProjectDir() / TEXT("Data/DBFilesClient");
		if (!TryReadRuntimeAppearanceDbcFiles(DbcRoot, Cache))
		{
			UE_LOG(LogTemp, Warning, TEXT("Runtime character appearance DBC files missing or invalid: %s"), *DbcRoot);
		}
		else
		{
			UE_LOG(LogTemp, Display, TEXT("Runtime character appearance DBC loaded: itemDisplay=%d helmetVis=%d hairGeosets=%d facialHair=%d source=%s"),
				Cache.ItemDisplayInfo.Num(),
				Cache.HelmetGeosetVisData.Num(),
				Cache.HairGeosets.Num(),
				Cache.FacialHairStyles.Num(),
				*DbcRoot);
		}
	}
	return Cache;
}

bool IsSubmeshVisible(int32 SubmeshId, const TMap<int32, int32>& GeosetGroups)
{
	if (SubmeshId == 0)
	{
		return true;
	}
	const int32 Group = SubmeshId / 100;
	const int32 Value = SubmeshId % 100;
	const int32* SelectedValue = GeosetGroups.Find(Group);
	return SelectedValue && Value == *SelectedValue;
}

bool IsRuntimeSectionVisible(int32 SubmeshId, int32 TextureType, const TMap<int32, int32>& GeosetGroups)
{
	if (SubmeshId == 0)
	{
		return true;
	}
	return IsSubmeshVisible(SubmeshId, GeosetGroups);
}

enum class ERuntimeCharacterGeosetGroup : int32
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
	Tabard = 12,
	Trousers = 13,
	Cape = 15,
	EyeGlow = 17,
};

void SetRuntimeGeosetGroup(TMap<int32, int32>& InOutGeosets, ERuntimeCharacterGeosetGroup Group, int32 Value)
{
	InOutGeosets.Add(static_cast<int32>(Group), Value);
}

bool RuntimeGeosetGroupHasSection(const TSet<int32>& AvailableSubmeshIds, ERuntimeCharacterGeosetGroup Group, int32 Value)
{
	if (Value <= 0)
	{
		return true;
	}

	const int32 WantedGroup = static_cast<int32>(Group);
	for (const int32 SubmeshId : AvailableSubmeshIds)
	{
		const int32 CandidateGroup = SubmeshId / 100;
		const int32 CandidateValue = SubmeshId % 100;
		if (CandidateGroup == WantedGroup && CandidateValue == Value)
		{
			return true;
		}
	}
	return false;
}

bool RuntimeEyeGlowCandidateHasOrdinaryTextureType(
	const TArray<FRuntimeSectionGeosetInfo>& Sections,
	const TSet<int32>& AvailableSubmeshIds,
	int32 CandidateValue)
{
	const int32 EyeGlowGroup = static_cast<int32>(ERuntimeCharacterGeosetGroup::EyeGlow);
	const int32 WantedSubmeshId = EyeGlowGroup * 100 + CandidateValue;
	if (CandidateValue <= 0 || !AvailableSubmeshIds.Contains(WantedSubmeshId))
	{
		return false;
	}

	for (const FRuntimeSectionGeosetInfo& Section : Sections)
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

bool RuntimeEyeGlowCandidateExists(
	const TSet<int32>& AvailableSubmeshIds,
	int32 CandidateValue)
{
	const int32 EyeGlowGroup = static_cast<int32>(ERuntimeCharacterGeosetGroup::EyeGlow);
	return CandidateValue > 0 && AvailableSubmeshIds.Contains(EyeGlowGroup * 100 + CandidateValue);
}

int32 ResolveRuntimeDefaultEyeGeosetValue(
	const TSet<int32>& AvailableSubmeshIds,
	const TArray<FRuntimeSectionGeosetInfo>& Sections,
	int32 ClassId)
{
	const int32 EyeGlowGroup = static_cast<int32>(ERuntimeCharacterGeosetGroup::EyeGlow);
	if (ClassId == 6)
	{
		for (const int32 Candidate : { 3, 2, 1 })
		{
			if (AvailableSubmeshIds.Contains(EyeGlowGroup * 100 + Candidate))
			{
				UE_LOG(LogTemp, Verbose, TEXT("Runtime character EyeGlow geoset selected: class=%d value=%d reason=death_knight_prefer_highest"), ClassId, Candidate);
				return Candidate;
			}
		}
		return 0;
	}

	for (const int32 Candidate : { 2, 1 })
	{
		if (RuntimeEyeGlowCandidateExists(AvailableSubmeshIds, Candidate))
		{
			UE_LOG(LogTemp, Verbose, TEXT("Runtime character EyeGlow geoset selected: class=%d value=%d reason=non_dk_standard_eye_value"), ClassId, Candidate);
			return Candidate;
		}
	}
	for (const int32 Candidate : { 3 })
	{
		if (RuntimeEyeGlowCandidateHasOrdinaryTextureType(Sections, AvailableSubmeshIds, Candidate))
		{
			UE_LOG(LogTemp, Verbose, TEXT("Runtime character EyeGlow geoset selected: class=%d value=%d reason=ordinary_eye_texture_type"), ClassId, Candidate);
			return Candidate;
		}
	}
	for (const int32 Candidate : { 3, 2, 1 })
	{
		if (RuntimeEyeGlowCandidateExists(AvailableSubmeshIds, Candidate))
		{
			UE_LOG(LogTemp, Display, TEXT("Runtime character EyeGlow geoset skipped: class=%d value=%d reason=non_dk_no_standard_or_ordinary_eye"), ClassId, Candidate);
		}
	}
	return 0;
}

void BuildRuntimeDefaultGeosets(
	const TSet<int32>& AvailableSubmeshIds,
	const TArray<FRuntimeSectionGeosetInfo>& Sections,
	int32 ClassId,
	TMap<int32, int32>& OutGeosets)
{
	OutGeosets.Reset();
	for (const int32 SubmeshId : AvailableSubmeshIds)
	{
		if (SubmeshId >= 100)
		{
			OutGeosets.FindOrAdd(SubmeshId / 100, 1);
		}
	}

	SetRuntimeGeosetGroup(OutGeosets, ERuntimeCharacterGeosetGroup::HairStyle, 0);
	SetRuntimeGeosetGroup(OutGeosets, ERuntimeCharacterGeosetGroup::Facial1, 0);
	SetRuntimeGeosetGroup(OutGeosets, ERuntimeCharacterGeosetGroup::Facial2, 0);
	SetRuntimeGeosetGroup(OutGeosets, ERuntimeCharacterGeosetGroup::Facial3, 0);
	SetRuntimeGeosetGroup(OutGeosets, ERuntimeCharacterGeosetGroup::Ears, 2);
	SetRuntimeGeosetGroup(OutGeosets, ERuntimeCharacterGeosetGroup::EyeGlow, ResolveRuntimeDefaultEyeGeosetValue(AvailableSubmeshIds, Sections, ClassId));
}

void ApplyRuntimeEquipmentGeosetRules(
	const FRuntimeCharacterAppearanceDbcCache& Cache,
	const FString& EquipmentSummary,
	const TSet<int32>& AvailableSubmeshIds,
	TMap<int32, int32>& InOutGeosets)
{
	const TArray<FRuntimeCharacterEquipmentDisplay> Equipment = ParseRuntimeEquipmentSummary(EquipmentSummary);
	auto FindEquipmentBySlot = [&Equipment](int32 Slot) -> const FRuntimeCharacterEquipmentDisplay*
	{
		return Equipment.FindByPredicate([Slot](const FRuntimeCharacterEquipmentDisplay& Entry)
		{
			return Entry.Slot == Slot && Entry.DisplayId > 0;
		});
	};
	auto FindDisplay = [&Cache](int32 DisplayId) -> const FRuntimeCharacterItemDisplay*
	{
		return DisplayId > 0 ? Cache.ItemDisplayInfo.Find(DisplayId) : nullptr;
	};

	bool bHadRobe = false;
	for (const int32 Slot : {4, 6})
	{
		const FRuntimeCharacterEquipmentDisplay* Entry = FindEquipmentBySlot(Slot);
		const FRuntimeCharacterItemDisplay* Display = Entry ? FindDisplay(Entry->DisplayId) : nullptr;
		if (Display && (Entry->InventoryType == 20 || Display->GeosetGroups[2] == 1))
		{
			bHadRobe = true;
			break;
		}
	}

	auto ApplySlot = [&](int32 Slot)
	{
		const FRuntimeCharacterEquipmentDisplay* Entry = FindEquipmentBySlot(Slot);
		const FRuntimeCharacterItemDisplay* Display = Entry ? FindDisplay(Entry->DisplayId) : nullptr;
		if (!Display)
		{
			return;
		}

		switch (Slot)
		{
		case 3:
		case 4:
			SetRuntimeGeosetGroup(InOutGeosets, ERuntimeCharacterGeosetGroup::Wristbands, 1 + Display->GeosetGroups[0]);
			break;
		case 6:
			if (const int32* Trousers = InOutGeosets.Find(static_cast<int32>(ERuntimeCharacterGeosetGroup::Trousers)); Trousers && *Trousers == 2)
			{
				return;
			}
			SetRuntimeGeosetGroup(InOutGeosets, ERuntimeCharacterGeosetGroup::Kneepads, 1 + Display->GeosetGroups[1]);
			break;
		case 7:
			SetRuntimeGeosetGroup(InOutGeosets, ERuntimeCharacterGeosetGroup::Boots, 1 + Display->GeosetGroups[0]);
			break;
		case 9:
			SetRuntimeGeosetGroup(InOutGeosets, ERuntimeCharacterGeosetGroup::Gloves, 1 + Display->GeosetGroups[0]);
			break;
		case 14:
			SetRuntimeGeosetGroup(InOutGeosets, ERuntimeCharacterGeosetGroup::Cape, 1 + Display->GeosetGroups[0]);
			break;
		case 18:
			SetRuntimeGeosetGroup(InOutGeosets, ERuntimeCharacterGeosetGroup::Tabard, 2);
			break;
		default:
			break;
		}

		if (const int32* Trousers = InOutGeosets.Find(static_cast<int32>(ERuntimeCharacterGeosetGroup::Trousers)); Trousers && *Trousers == 1)
		{
			SetRuntimeGeosetGroup(InOutGeosets, ERuntimeCharacterGeosetGroup::Trousers, 1 + Display->GeosetGroups[2]);
		}
		if (const int32* Trousers = InOutGeosets.Find(static_cast<int32>(ERuntimeCharacterGeosetGroup::Trousers)); Trousers && *Trousers == 2)
		{
			SetRuntimeGeosetGroup(InOutGeosets, ERuntimeCharacterGeosetGroup::Boots, 0);
			SetRuntimeGeosetGroup(InOutGeosets, ERuntimeCharacterGeosetGroup::Tabard, 0);
		}
		if (const int32* Gloves = InOutGeosets.Find(static_cast<int32>(ERuntimeCharacterGeosetGroup::Gloves)); Gloves && *Gloves > 1)
		{
			SetRuntimeGeosetGroup(InOutGeosets, ERuntimeCharacterGeosetGroup::Wristbands, 0);
		}
	};

	const int32 SlotOrder[] = {3, 0, 1, 2, 6, 7, 4, 18, 5, 8, 9, 15, 16, 14, 17};
	const int32 SlotOrderWithRobe[] = {3, 0, 1, 2, 6, 7, 5, 4, 18, 8, 9, 15, 16, 14, 17};
	const int32* EffectiveOrder = bHadRobe ? SlotOrderWithRobe : SlotOrder;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(SlotOrder); ++Index)
	{
		ApplySlot(EffectiveOrder[Index]);
	}

	for (auto It = InOutGeosets.CreateIterator(); It; ++It)
	{
		if (It.Value() > 0 && !RuntimeGeosetGroupHasSection(AvailableSubmeshIds, static_cast<ERuntimeCharacterGeosetGroup>(It.Key()), It.Value()))
		{
			It.Value() = 0;
		}
	}
}

int32 ResolveRuntimeHairGeosetId(const FRuntimeCharacterAppearanceDbcCache& Cache, int32 Race, int32 Gender, int32 HairStyle)
{
	for (const FRuntimeCharacterHairGeoset& Record : Cache.HairGeosets)
	{
		if (Record.Race == Race && Record.Gender == Gender && Record.HairStyle == HairStyle)
		{
			return Record.bBald ? 0 : Record.GeosetId;
		}
	}
	return 0;
}

const FRuntimeCharacterFacialHairStyle* ResolveRuntimeFacialHairStyle(const FRuntimeCharacterAppearanceDbcCache& Cache, int32 Race, int32 Gender, int32 FacialStyle)
{
	for (const FRuntimeCharacterFacialHairStyle& Record : Cache.FacialHairStyles)
	{
		if (Record.Race == Race && Record.Gender == Gender && Record.Style == FacialStyle)
		{
			return &Record;
		}
	}
	return nullptr;
}

bool RuntimeHelmetFlagsHideRace(uint32 Flags, int32 Race)
{
	if (Flags == 0 || Race <= 0)
	{
		return false;
	}
	const int32 BitIndex = Race - 1;
	return BitIndex >= 0 && BitIndex < 32 && (Flags & (1u << BitIndex)) != 0;
}

const FRuntimeCharacterHelmetVis* ResolveRuntimeHelmetVis(const FRuntimeCharacterAppearanceDbcCache& Cache, const FString& EquipmentSummary, int32 Gender)
{
	for (const FRuntimeCharacterEquipmentDisplay& Entry : ParseRuntimeEquipmentSummary(EquipmentSummary))
	{
		if (Entry.Slot != 0 || Entry.DisplayId <= 0)
		{
			continue;
		}
		const FRuntimeCharacterItemDisplay* Display = Cache.ItemDisplayInfo.Find(Entry.DisplayId);
		if (!Display)
		{
			return nullptr;
		}
		const int32 VisId = Gender == 1 ? Display->HelmetGeosetVisFemale : Display->HelmetGeosetVisMale;
		return Cache.HelmetGeosetVisData.Find(VisId);
	}
	return nullptr;
}

TMap<int32, int32> LoadConfiguredGeosets()
{
	return TMap<int32, int32>();
}

void BuildDefaultGeosetSelection(const TArray<FWoWPreviewSubmesh>& Submeshes, TMap<int32, int32>& InOutGeosetGroups)
{
	if (InOutGeosetGroups.Num() > 0)
	{
		return;
	}

	for (const FWoWPreviewSubmesh& Submesh : Submeshes)
	{
		if (Submesh.Id >= 100)
		{
			InOutGeosetGroups.Add(Submesh.Id / 100, 1);
		}
	}

	InOutGeosetGroups.Add(0, 7);
	InOutGeosetGroups.Add(7, 2);
	InOutGeosetGroups.Add(17, 0);
}

FString ResolvePreviewTexturePath(const FString& PreviewPng)
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

FString SanitizePackageSegment(FString Value)
{
	Value.ReplaceInline(TEXT("\\"), TEXT("/"));
	Value = FPaths::GetBaseFilename(Value);
	const TCHAR* InvalidCharacters = TEXT(" .:-");
	for (const TCHAR* Cursor = InvalidCharacters; *Cursor; ++Cursor)
	{
		Value.ReplaceCharInline(*Cursor, TEXT('_'));
	}
	return Value;
}

FString MakeObjectPathFromPackagePath(const FString& PackagePath)
{
	const FString AssetName = FPaths::GetCleanFilename(PackagePath);
	return AssetName.IsEmpty() ? FString() : PackagePath + TEXT(".") + AssetName;
}

FString BuildGeneratedPlayerAnimationStem(const FString& ModelPath)
{
	FString Stem = FPaths::GetBaseFilename(ModelPath);
	Stem.ReplaceCharInline(TEXT('-'), TEXT('_'));
	return Stem;
}

FString BuildGeneratedPlayerMeshObjectPath(const FString& GeneratedPackageRoot, const FString& ModelPath)
{
	FString PackageRoot = GeneratedPackageRoot;
	PackageRoot.RemoveFromEnd(TEXT("/"));
	const FString AssetStem = SanitizePackageSegment(ModelPath);
	return AssetStem.IsEmpty() || PackageRoot.IsEmpty()
		? FString()
		: MakeObjectPathFromPackagePath(PackageRoot / (AssetStem + TEXT("_M2Mesh")));
}

FString ResolveGeneratedPlayerMeshJsonPath(const FString& ModelPath)
{
	FString NormalizedModelPath = ModelPath.Replace(TEXT("\\"), TEXT("/"));
	if (NormalizedModelPath.IsEmpty())
	{
		return FString();
	}
	NormalizedModelPath.RemoveFromEnd(TEXT(".mdx"), ESearchCase::IgnoreCase);
	NormalizedModelPath.RemoveFromEnd(TEXT(".m2"), ESearchCase::IgnoreCase);
	const FString CandidatePath = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("M2Preview") / (NormalizedModelPath + TEXT(".mesh.json")));
	return FPaths::FileExists(CandidatePath) ? CandidatePath : FString();
}

const FGeneratedPlayerCharacterRuntimeDefinition* FindGeneratedPlayerCharacterDefinition(int32 Race, int32 Gender)
{
	static TMap<int64, FGeneratedPlayerCharacterRuntimeDefinition> Definitions;
	static bool bAttemptedLoad = false;
	if (!bAttemptedLoad)
	{
		bAttemptedLoad = true;
		const FString ManifestPath = FPaths::ProjectConfigDir() / PlayerCharacterManifestRelativePath;
		FString JsonText;
		if (!FFileHelper::LoadFileToString(JsonText, *ManifestPath))
		{
			UE_LOG(LogTemp, Warning, TEXT("PlayerCharacters manifest was not found: %s"), *ManifestPath);
			return nullptr;
		}

		TSharedPtr<FJsonObject> RootObject;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
		if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
		{
			UE_LOG(LogTemp, Warning, TEXT("PlayerCharacters manifest parse failed: %s"), *ManifestPath);
			return nullptr;
		}

		const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
		if (!RootObject->TryGetArrayField(TEXT("entries"), Entries) || !Entries)
		{
			return nullptr;
		}

		for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
		{
			const TSharedPtr<FJsonObject>* EntryObject = nullptr;
			if (!EntryValue.IsValid() || !EntryValue->TryGetObject(EntryObject) || !EntryObject || !EntryObject->IsValid())
			{
				continue;
			}

			FGeneratedPlayerCharacterRuntimeDefinition Definition;
			double Number = 0.0;
			(*EntryObject)->TryGetStringField(TEXT("id"), Definition.Id);
			if ((*EntryObject)->TryGetNumberField(TEXT("race_id"), Number))
			{
				Definition.Race = FMath::RoundToInt(Number);
			}
			if ((*EntryObject)->TryGetNumberField(TEXT("gender"), Number))
			{
				Definition.Gender = FMath::RoundToInt(Number);
			}

			FString ModelPath;
			FString GeneratedPackageRoot;
			(*EntryObject)->TryGetStringField(TEXT("model_path"), ModelPath);
			Definition.SourceModelPath = ModelPath;
			(*EntryObject)->TryGetStringField(TEXT("generated_package_root"), GeneratedPackageRoot);
			(*EntryObject)->TryGetStringField(TEXT("material_package_path"), Definition.MaterialPackagePath);
			(*EntryObject)->TryGetStringField(TEXT("animation_package_path"), Definition.AnimationPackagePath);
			Definition.MeshJsonPath = ResolveGeneratedPlayerMeshJsonPath(ModelPath);
			Definition.MeshObjectPath = BuildGeneratedPlayerMeshObjectPath(GeneratedPackageRoot, ModelPath);
			Definition.AnimationStem = BuildGeneratedPlayerAnimationStem(ModelPath);
			if (Definition.Race > 0 && Definition.Gender >= 0 && !Definition.MeshObjectPath.IsEmpty())
			{
				const int64 Key = (static_cast<int64>(Definition.Race) << 32) | static_cast<uint32>(Definition.Gender);
				Definitions.Add(Key, MoveTemp(Definition));
			}
		}
	}

	const int64 Key = (static_cast<int64>(Race) << 32) | static_cast<uint32>(Gender);
	return Definitions.Find(Key);
}

UTexture2D* LoadPngFileAsTransientTexture(const FString& TexturePath)
{
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

	UTexture2D* Texture = UTexture2D::CreateTransient(ImageWrapper->GetWidth(), ImageWrapper->GetHeight(), PF_B8G8R8A8);
	if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.Num() == 0)
	{
		return nullptr;
	}

	void* TextureData = Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(TextureData, RawData.GetData(), RawData.Num());
	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
	return Texture;
}

UTexture2D* LoadPreviewPngTexture(const FString& PreviewPng, TMap<FString, TObjectPtr<UTexture2D>>& TextureCache)
{
	const FString TexturePath = ResolvePreviewTexturePath(PreviewPng);
	if (TexturePath.IsEmpty())
	{
		return nullptr;
	}
	if (TObjectPtr<UTexture2D>* CachedTexture = TextureCache.Find(TexturePath))
	{
		return CachedTexture->Get();
	}

	UTexture2D* Texture = LoadPngFileAsTransientTexture(TexturePath);
	if (!Texture)
	{
		return nullptr;
	}

	Texture->SRGB = true;
	Texture->CompressionSettings = TC_Default;
	// WoW/D3D9 的 M2 特效贴图放大时应走双线性过滤；最近邻会把 32x32/64x64 的发光贴图放成明显马赛克。
	Texture->Filter = TF_Bilinear;
	// M2 发光层的导出 UV 已经在 0..1；这里用 Clamp 避免边界采样把贴图另一侧卷回来形成白条/硬边。
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
	Texture->UpdateResource();
	TextureCache.Add(TexturePath, Texture);
	return Texture;
}

FWoWPreviewMaterials LoadPreviewMaterials()
{
	FWoWPreviewMaterials Materials;
	Materials.OpaqueOneSided = ResolveLoadedObject<UMaterialInterface>(TEXT("/Game/WoW/Materials/M_WoWRuntimeTexture_OneSided.M_WoWRuntimeTexture_OneSided"));
	Materials.OpaqueTwoSided = ResolveLoadedObject<UMaterialInterface>(TEXT("/Game/WoW/Materials/M_WoWRuntimeTexture.M_WoWRuntimeTexture"));
	Materials.MaskedTwoSided = ResolveLoadedObject<UMaterialInterface>(TEXT("/Game/WoW/Materials/M_WoWRuntimeTexture_MaskedTwoSided.M_WoWRuntimeTexture_MaskedTwoSided"));
	// 对应 WMV / WoW 的 BM_ADDITIVE_ALPHA：glBlendFunc(GL_SRC_ALPHA, GL_ONE)。
	Materials.AdditiveAlphaUnlit = ResolveLoadedObject<UMaterialInterface>(TEXT("/Game/WoW/Materials/M_WoWM2AdditiveAlphaUnlit.M_WoWM2AdditiveAlphaUnlit"));
	Materials.AdditiveUnlit = ResolveLoadedObject<UMaterialInterface>(TEXT("/Game/WoW/Materials/M_WoWRuntimeTexture_AdditiveUnlit.M_WoWRuntimeTexture_AdditiveUnlit"));
	Materials.Fallback = ResolveLoadedObject<UMaterialInterface>(TEXT("/Engine/EngineDebugMaterials/VertexColorMaterial.VertexColorMaterial"));
	if (!Materials.Fallback)
	{
		Materials.Fallback = UMaterial::GetDefaultMaterial(MD_Surface);
	}
	if (!Materials.OpaqueOneSided || !Materials.OpaqueTwoSided || !Materials.MaskedTwoSided || !Materials.AdditiveAlphaUnlit || !Materials.AdditiveUnlit)
	{
		UE_LOG(LogTemp, Verbose, TEXT("Preview parent materials were not fully preloaded; using loaded/default materials without sync load"));
	}
	return Materials;
}

UMaterialInterface* SelectPreviewMaterial(const FWoWPreviewSection& Section, const FWoWPreviewMaterials& Materials)
{
	if (Section.RenderMode == 4)
	{
		return Materials.AdditiveAlphaUnlit
			? Materials.AdditiveAlphaUnlit.Get()
			: (Materials.AdditiveUnlit ? Materials.AdditiveUnlit.Get() : Materials.OpaqueTwoSided.Get());
	}
	if (Section.RenderMode == 3 || Section.RenderMode == 5 || Section.RenderMode == 6 || Section.bUnlit || Section.bNoDepthTest)
	{
		return Materials.AdditiveUnlit ? Materials.AdditiveUnlit.Get() : Materials.OpaqueTwoSided.Get();
	}
	if (Section.RenderMode == 1)
	{
		return Materials.MaskedTwoSided ? Materials.MaskedTwoSided.Get() : Materials.OpaqueTwoSided.Get();
	}
	if (Section.bTwoSided || bForceTwoSidedOpaquePreview)
	{
		return Materials.OpaqueTwoSided ? Materials.OpaqueTwoSided.Get() : Materials.OpaqueOneSided.Get();
	}
	return Materials.OpaqueOneSided ? Materials.OpaqueOneSided.Get() : Materials.OpaqueTwoSided.Get();
}

UMaterialInterface* CreatePreviewSectionMaterial(const FWoWPreviewSection& Section, const FWoWPreviewMaterials& Materials, UObject* Outer, TMap<FString, TObjectPtr<UTexture2D>>& TextureCache)
{
	UMaterialInterface* TextureMaterial = SelectPreviewMaterial(Section, Materials);
	UMaterialInterface* FallbackMaterial = Materials.Fallback.Get();
	if (!TextureMaterial)
	{
		return FallbackMaterial;
	}

	UTexture2D* Texture = LoadPreviewPngTexture(Section.PreviewPng, TextureCache);
	if (!Texture)
	{
		return FallbackMaterial ? FallbackMaterial : TextureMaterial;
	}

	UMaterialInstanceDynamic* DynamicMaterial = UMaterialInstanceDynamic::Create(TextureMaterial, Outer);
	if (!DynamicMaterial)
	{
		return FallbackMaterial ? FallbackMaterial : TextureMaterial;
	}
	DynamicMaterial->SetTextureParameterValue(PreviewTextureParameterName, Texture);
	if (Section.VertexColors.Num() > 0)
	{
		// M2 TextureUnit 的 Color/Transparency 轨道已经被导出为顶点色；这里保留同名 TintColor 参数给材质资产使用。
		FLinearColor TintColor = Section.VertexColors[0];
		if (Section.RenderMode == 4 && TintColor.A > 0.0f && TintColor.A < 0.25f)
		{
			TintColor.A *= M2LowAlphaAdditiveTintScale;
		}
		DynamicMaterial->SetVectorParameterValue(PreviewTintParameterName, TintColor);
	}
	else if (Section.bUseEnvMap)
	{
		// WoW 的 ArmorReflect 走 sphere-map 环境反射；强度已在导出顶点色中预衰减，这里只给支持 TintColor 的材质保留同名参数。
		DynamicMaterial->SetVectorParameterValue(PreviewTintParameterName, FLinearColor::White);
	}
	return DynamicMaterial;
}

FString ResolveItemObjectComponentMeshJsonByDisplayId(int32 DisplayId, const FString& ComponentName)
{
	if (DisplayId <= 0 || ComponentName.IsEmpty())
	{
		return FString();
	}

	const FString ContentManifestPath = FPaths::Combine(FPaths::ProjectContentDir(), ItemObjectComponentContentManifestRelativePath);
	const FString SavedManifestPath = FPaths::Combine(FPaths::ProjectSavedDir(), ItemObjectComponentManifestRelativePath);
	FString ManifestPath = ContentManifestPath;
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *ManifestPath))
	{
		ManifestPath = SavedManifestPath;
		if (!FFileHelper::LoadFileToString(JsonText, *ManifestPath))
		{
			UE_LOG(LogTemp, Warning, TEXT("Item object-component manifest was not found: content=%s saved=%s"),
				*ContentManifestPath,
				*SavedManifestPath);
			return FString();
		}
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to parse item object-component manifest: %s"), *ManifestPath);
		return FString();
	}

	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("entries"), Entries) || !Entries)
	{
		UE_LOG(LogTemp, Warning, TEXT("Item object-component manifest has no entries: %s"), *ManifestPath);
		return FString();
	}

	for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
	{
		const TSharedPtr<FJsonObject>* EntryObject = nullptr;
		if (!EntryValue.IsValid() || !EntryValue->TryGetObject(EntryObject) || !EntryObject || !EntryObject->IsValid())
		{
			continue;
		}

		FString EntryComponent;
		(*EntryObject)->TryGetStringField(TEXT("component"), EntryComponent);
		if (!EntryComponent.Equals(ComponentName, ESearchCase::IgnoreCase))
		{
			continue;
		}

		const TArray<TSharedPtr<FJsonValue>>* DisplayIds = nullptr;
		if (!(*EntryObject)->TryGetArrayField(TEXT("display_ids"), DisplayIds) || !DisplayIds)
		{
			continue;
		}

		bool bDisplayMatches = false;
		for (const TSharedPtr<FJsonValue>& DisplayValue : *DisplayIds)
		{
			if (DisplayValue.IsValid() && FMath::RoundToInt(DisplayValue->AsNumber()) == DisplayId)
			{
				bDisplayMatches = true;
				break;
			}
		}
		if (!bDisplayMatches)
		{
			continue;
		}

		FString MeshJsonPath;
		(*EntryObject)->TryGetStringField(TEXT("mesh_json_path"), MeshJsonPath);
		if (!MeshJsonPath.IsEmpty() && FPaths::FileExists(MeshJsonPath))
		{
			return MeshJsonPath;
		}
		UE_LOG(LogTemp, Warning, TEXT("Item object-component mesh json is missing: displayId=%d component=%s path=%s"),
			DisplayId,
			*ComponentName,
			*MeshJsonPath);
		return FString();
	}

	UE_LOG(LogTemp, Warning, TEXT("Item object-component entry was not found: displayId=%d component=%s manifest=%s"),
		DisplayId,
		*ComponentName,
		*ManifestPath);
	return FString();
}

FVector TransformPointByExportMatrix(const FMatrix& Matrix, const FVector& Point)
{
	return FVector(
		Matrix.M[0][0] * Point.X + Matrix.M[0][1] * Point.Y + Matrix.M[0][2] * Point.Z + Matrix.M[0][3],
		Matrix.M[1][0] * Point.X + Matrix.M[1][1] * Point.Y + Matrix.M[1][2] * Point.Z + Matrix.M[1][3],
		Matrix.M[2][0] * Point.X + Matrix.M[2][1] * Point.Y + Matrix.M[2][2] * Point.Z + Matrix.M[2][3]);
}

FVector InverseTransformPointByExportMatrix(const FMatrix& Matrix, const FVector& Point)
{
	return TransformPointByExportMatrix(Matrix.Inverse(), Point);
}

int32 GetIntVector4Component(const FIntVector4& Value, int32 Index)
{
	switch (Index)
	{
	case 0: return Value.X;
	case 1: return Value.Y;
	case 2: return Value.Z;
	default: return Value.W;
	}
}

void ResolveLoopingClipFrame(const FWoWCharacterPreviewAnimationClip& Clip, float TimeSeconds, int32& OutFrameIndex, int32& OutNextFrameIndex, float& OutFrameAlpha)
{
	OutFrameIndex = 0;
	OutNextFrameIndex = 0;
	OutFrameAlpha = 0.0f;
	if (Clip.Frames.Num() <= 1 || Clip.SampleRate <= 0.0f)
	{
		return;
	}

	// 只有导出器明确标记 loop_end_frame 的循环动作才把最后一帧当闭合帧。
	// JumpEnd / JumpLandRun 这类连接动作不能借用最后帧做循环闭合，否则落地会出现一帧跳变。
	const int32 PlaybackFrameCount = Clip.bLoopEndFrame
		? FMath::Max(Clip.Frames.Num() - 1, 1)
		: FMath::Max(Clip.Frames.Num(), 1);
	const float SampledDurationSeconds = static_cast<float>(PlaybackFrameCount) / Clip.SampleRate;
	const float LoopDurationSeconds = Clip.bLoopEndFrame && Clip.LengthSeconds > KINDA_SMALL_NUMBER
		? Clip.LengthSeconds
		: FMath::Max(Clip.LengthSeconds, SampledDurationSeconds);
	const float WrappedTimeSeconds = FMath::Fmod(FMath::Max(TimeSeconds, 0.0f), LoopDurationSeconds);
	const float ExactFrame = WrappedTimeSeconds * Clip.SampleRate;
	OutFrameIndex = FMath::Clamp(FMath::FloorToInt(ExactFrame), 0, PlaybackFrameCount - 1);
	OutNextFrameIndex = Clip.bLoopEndFrame
		? OutFrameIndex + 1
		: (OutFrameIndex + 1) % Clip.Frames.Num();
	OutFrameAlpha = FMath::Frac(ExactFrame);
}

void ResolveOneShotClipFrame(const FWoWCharacterPreviewAnimationClip& Clip, float TimeSeconds, int32& OutFrameIndex, int32& OutNextFrameIndex, float& OutFrameAlpha)
{
	OutFrameIndex = 0;
	OutNextFrameIndex = 0;
	OutFrameAlpha = 0.0f;
	if (Clip.Frames.Num() <= 1 || Clip.SampleRate <= 0.0f)
	{
		return;
	}

	const float MaxFrame = static_cast<float>(Clip.Frames.Num() - 1);
	const float ExactFrame = FMath::Clamp(TimeSeconds * Clip.SampleRate, 0.0f, MaxFrame);
	OutFrameIndex = FMath::Clamp(FMath::FloorToInt(ExactFrame), 0, Clip.Frames.Num() - 1);
	OutNextFrameIndex = FMath::Min(OutFrameIndex + 1, Clip.Frames.Num() - 1);
	OutFrameAlpha = FMath::Frac(ExactFrame);
}

bool SampleBoneMatrixAtTime(const FWoWCharacterPreviewAnimationClip& Clip, float TimeSeconds, int32 BoneIndex, FMatrix& OutMatrix)
{
	if (BoneIndex < 0 || Clip.Frames.Num() == 0 || Clip.SampleRate <= 0.0f)
	{
		return false;
	}

	int32 FrameIndex = 0;
	int32 NextFrameIndex = 0;
	float FrameAlpha = 0.0f;
	if (Clip.bLoopEndFrame)
	{
		ResolveLoopingClipFrame(Clip, TimeSeconds, FrameIndex, NextFrameIndex, FrameAlpha);
	}
	else
	{
		ResolveOneShotClipFrame(Clip, TimeSeconds, FrameIndex, NextFrameIndex, FrameAlpha);
	}

	if (!Clip.Frames.IsValidIndex(FrameIndex) || !Clip.Frames.IsValidIndex(NextFrameIndex))
	{
		return false;
	}

	const TArray<FMatrix>& BoneMatrices = Clip.Frames[FrameIndex];
	const TArray<FMatrix>& NextBoneMatrices = Clip.Frames[NextFrameIndex];
	if (!BoneMatrices.IsValidIndex(BoneIndex) || !NextBoneMatrices.IsValidIndex(BoneIndex))
	{
		return false;
	}

	OutMatrix = FMatrix::Identity;
	for (int32 Row = 0; Row < 4; ++Row)
	{
		for (int32 Column = 0; Column < 4; ++Column)
		{
			OutMatrix.M[Row][Column] = FMath::Lerp(BoneMatrices[BoneIndex].M[Row][Column], NextBoneMatrices[BoneIndex].M[Row][Column], FrameAlpha);
		}
	}
	return true;
}

FWoWTextureTransformSample SampleTextureTransform(const TArray<FWoWTextureTransformSample>& Samples, float TimeSeconds)
{
	if (Samples.Num() == 0)
	{
		return FWoWTextureTransformSample();
	}

	const float DurationSeconds = FMath::Max(Samples.Last().TimeSeconds, KINDA_SMALL_NUMBER);
	const float WrappedTime = FMath::Fmod(FMath::Max(TimeSeconds, 0.0f), DurationSeconds);
	for (int32 Index = 1; Index < Samples.Num(); ++Index)
	{
		if (WrappedTime <= Samples[Index].TimeSeconds)
		{
			const FWoWTextureTransformSample& Previous = Samples[Index - 1];
			const FWoWTextureTransformSample& Next = Samples[Index];
			const float Duration = FMath::Max(Next.TimeSeconds - Previous.TimeSeconds, KINDA_SMALL_NUMBER);
			const float Alpha = FMath::Clamp((WrappedTime - Previous.TimeSeconds) / Duration, 0.0f, 1.0f);

			FWoWTextureTransformSample Result;
			Result.TimeSeconds = WrappedTime;
			Result.Translation = FMath::Lerp(Previous.Translation, Next.Translation, Alpha);
			Result.RotationDegrees = FMath::Lerp(Previous.RotationDegrees, Next.RotationDegrees, Alpha);
			Result.Scaling = FMath::Lerp(Previous.Scaling, Next.Scaling, Alpha);
			return Result;
		}
	}

	return Samples.Last();
}

FWoWSectionColorSample SampleSectionColor(const TArray<FWoWSectionColorSample>& Samples, float TimeSeconds)
{
	if (Samples.Num() == 0)
	{
		return FWoWSectionColorSample();
	}

	const float LengthSeconds = Samples.Last().TimeSeconds;
	const float WrappedTime = LengthSeconds > KINDA_SMALL_NUMBER
		? FMath::Fmod(FMath::Max(TimeSeconds, 0.0f), LengthSeconds)
		: 0.0f;
	for (int32 Index = 1; Index < Samples.Num(); ++Index)
	{
		if (WrappedTime <= Samples[Index].TimeSeconds)
		{
			const FWoWSectionColorSample& Previous = Samples[Index - 1];
			const FWoWSectionColorSample& Next = Samples[Index];
			const float Duration = FMath::Max(Next.TimeSeconds - Previous.TimeSeconds, KINDA_SMALL_NUMBER);
			const float Alpha = FMath::Clamp((WrappedTime - Previous.TimeSeconds) / Duration, 0.0f, 1.0f);

			FWoWSectionColorSample Result;
			Result.TimeSeconds = WrappedTime;
			Result.Color = FMath::Lerp(Previous.Color, Next.Color, Alpha);
			return Result;
		}
	}

	return Samples.Last();
}

void ApplySectionColorSamples(const TArray<FWoWSectionColorSample>& Samples, float TimeSeconds, bool bScaleLowAlphaAdditive, TArray<FLinearColor>& InOutVertexColors)
{
	if (Samples.Num() == 0 || InOutVertexColors.Num() == 0)
	{
		return;
	}

	// M2 Color/Transparency 是 render pass 的 glColor/alpha 轨道；整段 section 使用同一个采样色。
	FLinearColor Color = SampleSectionColor(Samples, TimeSeconds).Color;
	if (bScaleLowAlphaAdditive && Color.A > 0.0f && Color.A < 0.25f)
	{
		Color.A *= M2LowAlphaAdditiveTintScale;
	}
	for (FLinearColor& VertexColor : InOutVertexColors)
	{
		VertexColor = Color;
	}
}

void ApplyTextureTransformToUVs(const TArray<FVector2D>& BaseUVs, const TArray<FWoWTextureTransformSample>& Samples, float TimeSeconds, TArray<FVector2D>& OutUVs)
{
	if (Samples.Num() == 0)
	{
		return;
	}

	if (OutUVs.Num() != BaseUVs.Num())
	{
		OutUVs = BaseUVs;
	}

	const FWoWTextureTransformSample Sample = SampleTextureTransform(Samples, TimeSeconds);
	const float Radians = FMath::DegreesToRadians(Sample.RotationDegrees);
	const float Cos = FMath::Cos(Radians);
	const float Sin = FMath::Sin(Radians);
	for (int32 Index = 0; Index < BaseUVs.Num(); ++Index)
	{
		const FVector2D Scaled(BaseUVs[Index].X * Sample.Scaling.X, BaseUVs[Index].Y * Sample.Scaling.Y);
		const FVector2D Rotated(Scaled.X * Cos - Scaled.Y * Sin, Scaled.X * Sin + Scaled.Y * Cos);
		// M2 TextureAnim 对应 OpenGL texture matrix：Scale/Rotate 后再加 Translation，所有值直接来自 M2Track。
		OutUVs[Index] = Rotated + Sample.Translation;
	}
}
}

UWoWCharacterPreviewComponent::UWoWCharacterPreviewComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

void UWoWCharacterPreviewComponent::BeginPlay()
{
	Super::BeginPlay();
	if (ACharacter* Character = Cast<ACharacter>(GetOwner()))
	{
		InitializeForCharacter(Character);
	}
}

void UWoWCharacterPreviewComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	float UpdateDeltaTime = DeltaTime;
	const int32 UpdateHz = CVarWoWCharacterPreviewUpdateHz.GetValueOnGameThread();
	if (UpdateHz > 0)
	{
		RuntimeUpdateAccumulatorSeconds += DeltaTime;
		const float UpdateIntervalSeconds = 1.0f / FMath::Clamp(UpdateHz, 1, 240);
		if (RuntimeUpdateAccumulatorSeconds < UpdateIntervalSeconds)
		{
			return;
		}
		UpdateDeltaTime = RuntimeUpdateAccumulatorSeconds;
		RuntimeUpdateAccumulatorSeconds = 0.0f;
	}

	UpdateAnimation(UpdateDeltaTime);
	UpdateAttachedM2Animation(UpdateDeltaTime);
	if (CVarWoWCharacterPreviewRibbons.GetValueOnGameThread() != 0)
	{
		UpdateAttachedM2Ribbons(UpdateDeltaTime);
	}
}

void UWoWCharacterPreviewComponent::InitializeForCharacter(ACharacter* InCharacter)
{
	if (!InCharacter)
	{
		return;
	}
	PreviewCharacter = InCharacter;
	if (!bHasRuntimeAppearance)
	{
		HideNativeCharacterMesh(InCharacter);
		UE_LOG(LogTemp, Error, TEXT("WoW character preview initialized without selected runtime appearance. This is a flow error; no fallback character will be rendered."));
		return;
	}
	LoadAnimationData();
	if (bPreviewMeshInitialized && PreviewMeshComponent.IsValid())
	{
		ApplyGeneratedSkeletalPreviewToCharacter(InCharacter);
		if (InCharacter->IsPlayerControlled())
		{
			ApplyPreviewEquipmentToCharacter(InCharacter);
		}
		return;
	}

	ApplyM2PreviewMeshToCharacter(InCharacter);
	bPreviewMeshInitialized = PreviewMeshComponent.IsValid();
	if (bPreviewMeshInitialized && InCharacter->IsPlayerControlled())
	{
		ApplyPreviewEquipmentToCharacter(InCharacter);
	}
}

void UWoWCharacterPreviewComponent::SetRuntimeAppearance(const FWoWSelectedCharacterRuntimeAppearance& InAppearance)
{
	if (!InAppearance.IsValid())
	{
		return;
	}

	RuntimeAppearanceKey = InAppearance.AppearanceKey;
	RuntimeEquipmentSummary = InAppearance.EquipmentSummary;
	RuntimeGuid = InAppearance.Guid;
	RuntimeRace = InAppearance.Race;
	RuntimeClassId = InAppearance.ClassId;
	RuntimeGender = InAppearance.Gender;
	RuntimeSkin = InAppearance.Skin;
	RuntimeFace = InAppearance.Face;
	RuntimeHairStyle = InAppearance.HairStyle;
	RuntimeHairColor = InAppearance.HairColor;
	RuntimeFacialStyle = InAppearance.FacialStyle;
	RuntimeSectionMetadataPath = InAppearance.CharacterPreviewSectionMetadataPath;
	RuntimeAttachmentDescriptorPath = InAppearance.CharacterPreviewAttachmentDescriptorPath;
	RuntimeAttachmentDefinitions.Reset();
	RuntimeSectionSubmeshIds.Reset();
	AppliedGeneratedMaterialKey.Empty();
	AppliedRuntimeBodyTextureKey.Empty();
	GeneratedAnimationCache.Reset();
	MissingGeneratedAnimationPaths.Reset();
	DestroyRuntimeEquipmentAttachments();
	bHasRuntimeAppearance = true;
	bUsingBakedRuntimeAppearanceAsset = InAppearance.bHasCharacterPreviewAsset && !InAppearance.CharacterPreviewMeshPath.IsEmpty();
	if (bUsingBakedRuntimeAppearanceAsset)
	{
		GeneratedCharacterMeshJsonPath = InAppearance.CharacterPreviewMeshJsonPath;
		GeneratedCharacterMeshPath = InAppearance.CharacterPreviewMeshPath;
		GeneratedCharacterMaterialPackagePath = !InAppearance.CharacterPreviewMaterialPackagePath.IsEmpty()
			? InAppearance.CharacterPreviewMaterialPackagePath
			: BuildRuntimeAppearanceMaterialPackagePath(InAppearance.CharacterPreviewMeshPath);
		GeneratedCharacterAnimationPackagePath = InAppearance.CharacterPreviewAnimationPackagePath;
		GeneratedCharacterAnimationStem = InAppearance.CharacterPreviewAnimationStem;
	}
	else if (InAppearance.bUsesBaseRuntimeComposition && !InAppearance.CharacterPreviewMeshPath.IsEmpty())
	{
		GeneratedCharacterMeshJsonPath = InAppearance.CharacterPreviewMeshJsonPath;
		GeneratedCharacterMeshPath = InAppearance.CharacterPreviewMeshPath;
		GeneratedCharacterMaterialPackagePath = InAppearance.CharacterPreviewMaterialPackagePath;
		GeneratedCharacterAnimationPackagePath = InAppearance.CharacterPreviewAnimationPackagePath;
		GeneratedCharacterAnimationStem = InAppearance.CharacterPreviewAnimationStem;
	}
	else if (const FGeneratedPlayerCharacterRuntimeDefinition* Definition = FindGeneratedPlayerCharacterDefinition(InAppearance.Race, InAppearance.Gender))
	{
		GeneratedCharacterMeshJsonPath = Definition->MeshJsonPath;
		GeneratedCharacterMeshPath = Definition->MeshObjectPath;
		GeneratedCharacterMaterialPackagePath = Definition->MaterialPackagePath;
		GeneratedCharacterAnimationPackagePath = Definition->AnimationPackagePath;
		GeneratedCharacterAnimationStem = Definition->AnimationStem;
	}
	else
	{
		GeneratedCharacterMeshJsonPath.Empty();
		GeneratedCharacterMeshPath.Empty();
		GeneratedCharacterMaterialPackagePath.Empty();
		GeneratedCharacterAnimationPackagePath.Empty();
		GeneratedCharacterAnimationStem.Empty();
	}

	if (USkeletalMeshComponent* SkeletalPreview = GeneratedSkeletalMeshComponent.Get())
	{
		FString LoadedMeshPath;
		if (USkeletalMesh* GeneratedMesh = LoadGeneratedPlayerCharacterMesh(GeneratedCharacterMeshPath, LoadedMeshPath))
		{
			if (SkeletalPreview->GetSkeletalMeshAsset() != GeneratedMesh)
			{
				SkeletalPreview->SetSkeletalMesh(GeneratedMesh);
				GeneratedSkeletalAnimationId = INDEX_NONE;
				GeneratedSkeletalSubAnimationId = INDEX_NONE;
				GeneratedSkeletalGripVariant = GeneratedGripVariantNone;
			}
			ApplyGeneratedRuntimeAppearanceIfNeeded(SkeletalPreview);
			ApplyRuntimeGeosetVisibility();
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("Selected player character mesh is missing: race=%d gender=%d mesh=%s"),
				InAppearance.Race,
				InAppearance.Gender,
				*GeneratedCharacterMeshPath);
		}
	}

	int32 MainHandDisplayId = 0;
	TArray<FString> EquipmentParts;
	RuntimeEquipmentSummary.ParseIntoArray(EquipmentParts, TEXT(";"), true);
	for (const FString& Part : EquipmentParts)
	{
		TArray<FString> Fields;
		Part.ParseIntoArray(Fields, TEXT(":"), false);
		if (Fields.Num() < 3)
		{
			continue;
		}

		const int32 Slot = FCString::Atoi(*Fields[0]);
		const int32 DisplayId = FCString::Atoi(*Fields[1]);
		if ((Slot == 15 || Slot == 16) && DisplayId > 0)
		{
			MainHandDisplayId = DisplayId;
			break;
		}
	}

	if (MainHandDisplayId > 0 && MainHandDisplayId != PreviewMainHandItemDisplayId)
	{
		PreviewMainHandItemDisplayId = MainHandDisplayId;
		if (UWoWM2ModelComponent* ExistingModelComponent = AttachedM2ModelComponent.Get())
		{
			ExistingModelComponent->DestroyComponent();
			AttachedM2ModelComponent.Reset();
			AttachedWeaponMeshComponent.Reset();
			AttachedRibbonMeshComponent.Reset();
		}
		if (ACharacter* Character = PreviewCharacter.Get())
		{
			ApplyPreviewEquipmentToCharacter(Character);
		}
	}
}

void UWoWCharacterPreviewComponent::SetMovementInputState(bool bInMoveForward, bool bInMoveBackward, bool bInStrafeLeft, bool bInStrafeRight, bool bInTurnLeft, bool bInTurnRight, bool bInMouseTurnLeft, bool bInMouseTurnRight, bool bInLeftMouseHeld, bool bInRightMouseHeld, float InForwardInput, float InStrafeInput)
{
	const int32 PreviousStrafeDirection = ResolveStrafeDirection(bStrafeLeftHeld, bStrafeRightHeld, StrafeInputAmount);
	const bool bPreviousPureStrafe = PreviousStrafeDirection != 0 && FMath::Abs(ForwardInputAmount) <= 0.1f && !bMoveForwardHeld && !bMoveBackwardHeld;

	bMoveForwardHeld = bInMoveForward;
	bMoveBackwardHeld = bInMoveBackward;
	bStrafeLeftHeld = bInStrafeLeft;
	bStrafeRightHeld = bInStrafeRight;
	bTurnLeftHeld = bInTurnLeft;
	bTurnRightHeld = bInTurnRight;
	bMouseTurnLeft = bInMouseTurnLeft;
	bMouseTurnRight = bInMouseTurnRight;
	bLeftMouseHeld = bInLeftMouseHeld;
	bRightMouseHeld = bInRightMouseHeld;
	ForwardInputAmount = FMath::Clamp(InForwardInput, -1.0f, 1.0f);
	StrafeInputAmount = FMath::Clamp(InStrafeInput, -1.0f, 1.0f);
	if (bJumpStartedWithGroundMovement && OneShotAnimationClipIndex != INDEX_NONE && AnimationClips.IsValidIndex(OneShotAnimationClipIndex) && IsClipForAnimationId(AnimationClips[OneShotAnimationClipIndex], 39) && HasActiveMovementInput())
	{
		StartMovingLandingOverlay(0.0f);
		OneShotAnimationClipIndex = INDEX_NONE;
		OneShotRemainingSeconds = 0.0f;
		OneShotPlaybackRate = 1.0f;
		AnimationTimeSeconds = 0.0f;
	}
	else if (OneShotAnimationClipIndex != INDEX_NONE && AnimationClips.IsValidIndex(OneShotAnimationClipIndex) && IsClipForAnimationId(AnimationClips[OneShotAnimationClipIndex], 39) && HasActiveMovementInput())
	{
		// 原地跳落地后的蹲下缓冲可以完整播放；但玩家按W/S/Q/E时必须立刻交回移动姿态，不能滑着走。
		OneShotAnimationClipIndex = INDEX_NONE;
		OneShotRemainingSeconds = 0.0f;
		OneShotPlaybackRate = 1.0f;
		AnimationTimeSeconds = 0.0f;
	}
	const int32 CurrentStrafeDirection = ResolveStrafeDirection(bStrafeLeftHeld, bStrafeRightHeld, StrafeInputAmount);
	if (CurrentStrafeDirection != 0)
	{
		StrafeReleaseShuffleRemainingSeconds = 0.0f;
		StrafeReleaseShuffleDirection = 0;
	}
	else if (bPreviousPureStrafe && PreviousStrafeDirection != 0)
	{
		StrafeReleaseShuffleRemainingSeconds = StrafeReleaseShuffleDurationSeconds;
		StrafeReleaseShuffleDirection = PreviousStrafeDirection;
		StrafeOverlayAmount = 1.0f;
		StrafeOverlayTimeSeconds = 0.0f;
		CurrentStrafeOverlayClipIndex = INDEX_NONE;
	}

	const bool bAnyMovementInput = bMoveForwardHeld || bMoveBackwardHeld || bStrafeLeftHeld || bStrafeRightHeld || bTurnLeftHeld || bTurnRightHeld || bMouseTurnLeft || bMouseTurnRight || (bLeftMouseHeld && bRightMouseHeld);
	if (bAnyMovementInput && !bCombatReady)
	{
		CombatReadyLingerSeconds = 0.0f;
	}
}

void UWoWCharacterPreviewComponent::SetCombatReady(bool bInCombatReady, float LingerSeconds)
{
	bCombatReady = bInCombatReady;
	if (bCombatReady)
	{
		CombatReadyLingerSeconds = 0.0f;
		return;
	}

	CombatReadyLingerSeconds = FMath::Max(LingerSeconds, 0.0f);
}

void UWoWCharacterPreviewComponent::TriggerAttack()
{
	StartOneShotAnimation(16, 0);
}

void UWoWCharacterPreviewComponent::TriggerJumpStart()
{
	StartOneShotAnimation(37, 0);
}

void UWoWCharacterPreviewComponent::SetJumpStartedWithGroundMovement(bool bInJumpStartedWithGroundMovement)
{
	bJumpStartedWithGroundMovement = bInJumpStartedWithGroundMovement;
}

void UWoWCharacterPreviewComponent::SetSitting(bool bInSitting, bool bPlayTransition)
{
	if (bSitting == bInSitting)
	{
		return;
	}

	bSitting = bInSitting;
	bWeaponSheatheTransitionActive = false;
	WeaponSheatheTransitionElapsedSeconds = 0.0f;
	WeaponSheatheTransitionSwitchSeconds = 0.0f;
	WeaponSheatheTransitionClipSeconds = 0.0f;
	WeaponSheatheTransitionBlendOutSeconds = 0.0f;
	WeaponSheatheTransitionDurationSeconds = 0.0f;
	WeaponSheatheTransitionAnimationId = INDEX_NONE;
	// 坐下/起立使用M2自带过渡；过渡结束后自动保持SitGround或回到普通站立/移动选择。
	if (bPlayTransition)
	{
		StartOneShotAnimation(bSitting ? SitGroundDownAnimationId : SitGroundUpAnimationId, 0);
	}
	else if (OneShotAnimationClipIndex != INDEX_NONE &&
		AnimationClips.IsValidIndex(OneShotAnimationClipIndex) &&
		(IsClipForAnimationId(AnimationClips[OneShotAnimationClipIndex], SitGroundDownAnimationId) ||
			IsClipForAnimationId(AnimationClips[OneShotAnimationClipIndex], SitGroundUpAnimationId)))
	{
		OneShotAnimationClipIndex = INDEX_NONE;
		OneShotRemainingSeconds = 0.0f;
		OneShotPlaybackRate = 1.0f;
		AnimationTimeSeconds = 0.0f;
	}

	UpdateAttachedWeaponTransform();
}

bool UWoWCharacterPreviewComponent::IsSitting() const
{
	return bSitting;
}

bool UWoWCharacterPreviewComponent::IsSittingTransitionActive() const
{
	return OneShotAnimationClipIndex != INDEX_NONE &&
		AnimationClips.IsValidIndex(OneShotAnimationClipIndex) &&
		(IsClipForAnimationId(AnimationClips[OneShotAnimationClipIndex], SitGroundDownAnimationId) ||
			IsClipForAnimationId(AnimationClips[OneShotAnimationClipIndex], SitGroundUpAnimationId));
}

void UWoWCharacterPreviewComponent::ToggleWeaponSheathed()
{
	SetWeaponSheathed(!bWeaponSheathed, true);
}

void UWoWCharacterPreviewComponent::SetWeaponSheathed(bool bInSheathed, bool bPlayTransition)
{
	if (bWeaponSheathed == bInSheathed)
	{
		return;
	}

	const bool bPreviousVisualSheathed = IsWeaponVisuallySheathed();
	bWeaponSheathed = bInSheathed;
	bDebugWeaponSheathedPreview = false;
	ActiveWeaponSheathedAttachmentId = LargeWeaponSheathedAttachmentId;
	bWeaponSheatheTransitionActive = false;
	WeaponSheatheTransitionElapsedSeconds = 0.0f;
	WeaponSheatheTransitionSwitchSeconds = 0.0f;
	WeaponSheatheTransitionClipSeconds = 0.0f;
	WeaponSheatheTransitionBlendOutSeconds = 0.0f;
	WeaponSheatheTransitionDurationSeconds = 0.0f;
	WeaponSheatheTransitionAnimationId = INDEX_NONE;
	bWeaponSheatheTransitionStartVisualSheathed = bPreviousVisualSheathed;

	if (bCombatReady || CombatReadyLingerSeconds > 0.0f ||
		(OneShotAnimationClipIndex != INDEX_NONE &&
			AnimationClips.IsValidIndex(OneShotAnimationClipIndex) &&
			IsClipForAnimationId(AnimationClips[OneShotAnimationClipIndex], 16)))
	{
		// WoW 中进入攻击/战斗状态后不能自由收武器；这里把状态还原，避免按键改变装备视觉。
		bWeaponSheathed = !bInSheathed;
		UE_LOG(LogTemp, Display, TEXT("Weapon sheathe toggle ignored while combat/attack is active"));
		return;
	}

	if (bPlayTransition && bPreviewEquipmentVisible && !bSitting && !IsSittingTransitionActive())
	{
		const int32 SheatheAnimationId = GetWeaponSheatheAnimationId();
		const int32 SheatheClipIndex = FindAnimationClipIndex(SheatheAnimationId, 0);
		if (AnimationClips.IsValidIndex(SheatheClipIndex))
		{
			const FWoWCharacterPreviewAnimationClip& SheathClip = AnimationClips[SheatheClipIndex];
			const float SheathDurationSeconds = SheathClip.LengthSeconds > 0.0f
				? SheathClip.LengthSeconds
				: static_cast<float>(SheathClip.Frames.Num()) / FMath::Max(SheathClip.SampleRate, 1.0f);
			bWeaponSheatheTransitionActive = true;
			WeaponSheatheTransitionAnimationId = SheatheAnimationId;
			WeaponSheatheTransitionElapsedSeconds = 0.0f;
			WeaponSheatheTransitionSwitchSeconds = FMath::Max(GetWeaponSheatheAttachmentSwitchSeconds(SheatheAnimationId, 0, SheathDurationSeconds), KINDA_SMALL_NUMBER);
			WeaponSheatheTransitionClipSeconds = FMath::Max(SheathDurationSeconds, WeaponSheatheTransitionSwitchSeconds);
			WeaponSheatheTransitionBlendOutSeconds = FMath::Max(SheathClip.BlendTimeSeconds, WeaponSheatheOverlayDefaultBlendSeconds);
			WeaponSheatheTransitionDurationSeconds = WeaponSheatheTransitionClipSeconds + WeaponSheatheTransitionBlendOutSeconds;
			bWeaponSheatheTransitionStartVisualSheathed = bPreviousVisualSheathed;
		}
	}

	UpdateAttachedWeaponTransform();
	UE_LOG(LogTemp, Display, TEXT("Weapon sheathe toggled: sheathed=%s visual_attachment=%d"),
		bWeaponSheathed ? TEXT("true") : TEXT("false"),
		ResolveVisualWeaponAttachmentId());
}

int32 UWoWCharacterPreviewComponent::ResolvePreviewMainHandDisplayId() const
{
	if (PreviewMainHandItemDisplayId > 0)
	{
		return PreviewMainHandItemDisplayId;
	}

	return DebugAttachedWeaponDisplayId_DEPRECATED > 0 ? DebugAttachedWeaponDisplayId_DEPRECATED : INDEX_NONE;
}

UAnimSequence* UWoWCharacterPreviewComponent::LoadGeneratedCharacterAnimation(int32 AnimationId, int32 SubAnimationId, int32 GripVariant) const
{
	const TCHAR* AnimationName = GetM2RuntimeAnimationName(AnimationId);
	if (!AnimationName)
	{
		return nullptr;
	}

	if (!GeneratedCharacterAnimationPackagePath.IsEmpty() && !GeneratedCharacterAnimationStem.IsEmpty())
	{
		const TCHAR* GripSuffix = TEXT("");
		if (GripVariant == GeneratedGripVariantRight)
		{
			GripSuffix = TEXT("_GripR");
		}
		else if (GripVariant == GeneratedGripVariantLeft)
		{
			GripSuffix = TEXT("_GripL");
		}

		const FString ObjectPath = FString::Printf(
			TEXT("%s/A_%s_%03d_%s_%02d%s.A_%s_%03d_%s_%02d%s"),
			*GeneratedCharacterAnimationPackagePath,
			*GeneratedCharacterAnimationStem,
			AnimationId,
			AnimationName,
			SubAnimationId,
			GripSuffix,
			*GeneratedCharacterAnimationStem,
			AnimationId,
			AnimationName,
			SubAnimationId,
			GripSuffix);
		if (TWeakObjectPtr<UAnimSequence>* CachedSequence = GeneratedAnimationCache.Find(ObjectPath))
		{
			return CachedSequence->Get();
		}
		if (UAnimSequence* Sequence = ResolveLoadedObject<UAnimSequence>(ObjectPath))
		{
			GeneratedAnimationCache.Add(ObjectPath, Sequence);
			return Sequence;
		}
		if (!MissingGeneratedAnimationPaths.Contains(ObjectPath))
		{
			MissingGeneratedAnimationPaths.Add(ObjectPath);
			UE_LOG(LogTemp, Verbose, TEXT("Generated character animation not preloaded; skip sync load: anim=%s"), *ObjectPath);
		}
	}

	return nullptr;
}

bool UWoWCharacterPreviewComponent::IsWeaponSheathed() const
{
	return bWeaponSheathed;
}

bool UWoWCharacterPreviewComponent::IsWeaponVisuallySheathed() const
{
	if (bDebugWeaponSheathedPreview || bSitting || IsSittingTransitionActive())
	{
		return true;
	}
	if (bWeaponSheatheTransitionActive && WeaponSheatheTransitionElapsedSeconds < WeaponSheatheTransitionSwitchSeconds)
	{
		return bWeaponSheatheTransitionStartVisualSheathed;
	}
	return bWeaponSheathed;
}

bool UWoWCharacterPreviewComponent::IsDebugSheathedTransformActive() const
{
	return bDebugWeaponSheathedPreview;
}

int32 UWoWCharacterPreviewComponent::ResolveVisualWeaponAttachmentId() const
{
	return IsWeaponVisuallySheathed() ? ActiveWeaponSheathedAttachmentId : ActiveWeaponAttachmentId;
}

void UWoWCharacterPreviewComponent::DebugPreviousAnimation()
{
	if (AnimationClips.Num() == 0)
	{
		return;
	}
	DebugAnimationClipIndex = DebugAnimationClipIndex == INDEX_NONE
		? 0
		: (DebugAnimationClipIndex - 1 + AnimationClips.Num()) % AnimationClips.Num();
	AnimationTimeSeconds = 0.0f;
}

void UWoWCharacterPreviewComponent::DebugNextAnimation()
{
	if (AnimationClips.Num() == 0)
	{
		return;
	}
	DebugAnimationClipIndex = DebugAnimationClipIndex == INDEX_NONE
		? 0
		: (DebugAnimationClipIndex + 1) % AnimationClips.Num();
	AnimationTimeSeconds = 0.0f;
}

void UWoWCharacterPreviewComponent::DebugClearAnimation()
{
	DebugAnimationClipIndex = INDEX_NONE;
	AnimationTimeSeconds = 0.0f;
}

void UWoWCharacterPreviewComponent::DebugEquipWeaponRightHand()
{
	SetDebugAttachedWeaponSlot(RightPalmAttachmentId);
}

void UWoWCharacterPreviewComponent::DebugEquipWeaponLeftHand()
{
	SetDebugAttachedWeaponSlot(LeftPalmAttachmentId);
}

void UWoWCharacterPreviewComponent::DebugUnequipWeapon()
{
	SetDebugAttachedWeaponSlot(INDEX_NONE);
}

void UWoWCharacterPreviewComponent::DebugCycleSheathedAttachment()
{
	CycleDebugSheathedAttachment();
}

void UWoWCharacterPreviewComponent::LoadAnimationData()
{
	if (AnimationDataRecords.Num() > 0)
	{
		return;
	}

	const FString AnimationDataPath = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("WoWData/AnimationData.json"));
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *AnimationDataPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("WoW AnimationData json was not found: %s"), *AnimationDataPath);
		return;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to parse WoW AnimationData json: %s"), *AnimationDataPath);
		return;
	}

	const TArray<TSharedPtr<FJsonValue>>* RecordValues = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("records"), RecordValues))
	{
		UE_LOG(LogTemp, Error, TEXT("WoW AnimationData json has no records array: %s"), *AnimationDataPath);
		return;
	}

	AnimationDataRecords.Reserve(RecordValues->Num());
	for (const TSharedPtr<FJsonValue>& RecordValue : *RecordValues)
	{
		const TSharedPtr<FJsonObject>* RecordObject = nullptr;
		if (!RecordValue->TryGetObject(RecordObject) || !RecordObject || !RecordObject->IsValid())
		{
			continue;
		}

		FWoWAnimationDataRecord Record;
		double NumberValue = 0.0;
		if ((*RecordObject)->TryGetNumberField(TEXT("id"), NumberValue))
		{
			Record.Id = static_cast<int32>(NumberValue);
		}
		(*RecordObject)->TryGetStringField(TEXT("name"), Record.Name);
		if ((*RecordObject)->TryGetNumberField(TEXT("weapon_flags"), NumberValue))
		{
			Record.WeaponFlags = static_cast<int32>(NumberValue);
		}
		if ((*RecordObject)->TryGetNumberField(TEXT("body_flags"), NumberValue))
		{
			Record.BodyFlags = static_cast<int32>(NumberValue);
		}
		if ((*RecordObject)->TryGetNumberField(TEXT("flags"), NumberValue))
		{
			Record.Flags = static_cast<int32>(NumberValue);
		}
		if ((*RecordObject)->TryGetNumberField(TEXT("fallback"), NumberValue))
		{
			Record.Fallback = static_cast<int32>(NumberValue);
		}
		if ((*RecordObject)->TryGetNumberField(TEXT("behavior_id"), NumberValue))
		{
			Record.BehaviorId = static_cast<int32>(NumberValue);
		}
		if ((*RecordObject)->TryGetNumberField(TEXT("behavior_tier"), NumberValue))
		{
			Record.BehaviorTier = static_cast<int32>(NumberValue);
		}

		if (Record.Id == INDEX_NONE || Record.Name.IsEmpty())
		{
			continue;
		}

		const int32 NewIndex = AnimationDataRecords.Add(MoveTemp(Record));
		AnimationDataIndexById.Add(AnimationDataRecords[NewIndex].Id, NewIndex);
		AnimationDataIndexByName.Add(AnimationDataRecords[NewIndex].Name.ToLower(), NewIndex);
	}

	UE_LOG(LogTemp, Display, TEXT("Loaded WoW AnimationData records: count=%d source=%s"), AnimationDataRecords.Num(), *AnimationDataPath);
}

const FWoWAnimationDataRecord* UWoWCharacterPreviewComponent::FindAnimationDataById(int32 AnimationId) const
{
	if (const int32* Index = AnimationDataIndexById.Find(AnimationId))
	{
		if (AnimationDataRecords.IsValidIndex(*Index))
		{
			return &AnimationDataRecords[*Index];
		}
	}
	return nullptr;
}

const FWoWAnimationDataRecord* UWoWCharacterPreviewComponent::FindAnimationDataByName(const FString& AnimationName) const
{
	if (const int32* Index = AnimationDataIndexByName.Find(AnimationName.ToLower()))
	{
		if (AnimationDataRecords.IsValidIndex(*Index))
		{
			return &AnimationDataRecords[*Index];
		}
	}
	return nullptr;
}

int32 UWoWCharacterPreviewComponent::ResolveAnimationDataIdByName(const FString& AnimationName, int32 FallbackAnimationId) const
{
	if (const FWoWAnimationDataRecord* Record = FindAnimationDataByName(AnimationName))
	{
		return Record->Id;
	}
	return FallbackAnimationId;
}

int32 UWoWCharacterPreviewComponent::GetWeaponSheatheAnimationId() const
{
	return ResolveAnimationDataIdByName(WeaponSheatheAnimationName, FallbackWeaponSheatheAnimationId);
}

float UWoWCharacterPreviewComponent::GetWeaponSheatheAttachmentSwitchSeconds(int32 AnimationId, int32 SubAnimationId, float FallbackDurationSeconds) const
{
	for (const FWoWM2EventRecord& Event : M2Events)
	{
		if (Event.Identifier != TEXT("$SHL") && Event.Identifier != TEXT("$SHR"))
		{
			continue;
		}
		for (const FWoWM2EventTimestampTrack& Track : Event.TimestampTracks)
		{
			if (Track.AnimationId == AnimationId &&
				Track.SubAnimationId == SubAnimationId &&
				Track.TimestampsMs.Num() > 0)
			{
				return FMath::Max(static_cast<float>(Track.TimestampsMs[0]) / 1000.0f, KINDA_SMALL_NUMBER);
			}
		}
	}

	UE_LOG(LogTemp, Warning, TEXT("M2 sheathe switch event $SHL/$SHR missing for anim=%d sub=%d; fallback ratio %.2f"),
		AnimationId,
		SubAnimationId,
		WeaponSheatheAttachmentSwitchFallbackAlpha);
	return FMath::Max(FallbackDurationSeconds * WeaponSheatheAttachmentSwitchFallbackAlpha, KINDA_SMALL_NUMBER);
}

void UWoWCharacterPreviewComponent::ApplyM2PreviewMeshToCharacter(ACharacter* Character)
{
	if (PreviewMeshComponent.IsValid())
	{
		return;
	}

	for (UActorComponent* Component : Character->GetComponents())
	{
		if (Component && Component->GetFName() == PreviewComponentName)
		{
			// 预览网格是运行时从 JSON 重建的临时组件；复用旧组件会导致 SectionStates/材质实例没有同步重建。
			Component->DestroyComponent();
		}
	}

	const FString MeshPath = GeneratedCharacterMeshJsonPath;
	FString JsonText;
	if (MeshPath.IsEmpty() || !FFileHelper::LoadFileToString(JsonText, *MeshPath))
	{
		UE_LOG(LogTemp, Error, TEXT("Selected player character M2 preview mesh json was not found: %s"), *MeshPath);
		return;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to parse WoW M2 preview mesh json: %s"), *MeshPath);
		return;
	}

	TArray<FWoWPreviewSubmesh> Submeshes;
	TArray<FWoWPreviewSection> Sections;
	ReadSubmeshArray(RootObject, Submeshes);
	if (!ReadSectionArray(RootObject, Sections))
	{
		UE_LOG(LogTemp, Error, TEXT("WoW M2 preview mesh json has invalid sections: %s"), *MeshPath);
		return;
	}

	AnimationClips.Reset();
	AnimationLookup.Reset();
	KeyBoneLookup.Reset();
	Attachments.Reset();
	AttachmentLookup.Reset();
	M2Events.Reset();
	BoneParents.Reset();
	LocomotionTorsoBoneIndices.Reset();
	LocomotionHeadBoneIndices.Reset();
	LocomotionLowerBodyBoneIndices.Reset();
	WeaponGripBoneIndices.Reset();
	ReadVectorField(RootObject, TEXT("bounds_min"), PreviewBoundsMin);
	ReadVectorField(RootObject, TEXT("bounds_max"), PreviewBoundsMax);
	ReadIntArray(RootObject, TEXT("animation_lookup"), AnimationLookup);
	ReadIntArray(RootObject, TEXT("key_bone_lookup"), KeyBoneLookup);
	ReadAttachmentArray(RootObject, Attachments);
	ReadIntArray(RootObject, TEXT("attachment_lookup"), AttachmentLookup);
	ReadM2EventArray(RootObject, M2Events);
	ReadBoneParentArray(RootObject, BoneParents);
	ReadAnimationClipArray(RootObject, AnimationClips);
	RebuildLocomotionPoseLayers();
	RebuildWeaponGripPoseLayer();
	RebuildWeaponSheatheUpperBodyPoseLayer();
	AnimationSampleRate = AnimationClips.Num() > 0 ? AnimationClips[0].SampleRate : 20.0f;
	AnimationTimeSeconds = 0.0f;
	VisualYawOffsetDegrees = 0.0f;
	StrafePoseAmount = 0.0f;
	StrafeOverlayAmount = 0.0f;
	StrafeOverlayTimeSeconds = 0.0f;
	StrafeReleaseShuffleRemainingSeconds = 0.0f;
	StrafeReleaseShuffleDirection = 0;
	CurrentAnimationClipIndex = INDEX_NONE;
	CurrentStrafeOverlayClipIndex = INDEX_NONE;
	LastLoggedAnimationClipIndex = INDEX_NONE;
	OneShotAnimationClipIndex = INDEX_NONE;
	OneShotRemainingSeconds = 0.0f;
	OneShotPlaybackRate = 1.0f;
	bWasFallingLastTick = false;
	bJumpStartedWithGroundMovement = false;
	bSitting = false;
	bDebugWeaponSheathedPreview = false;
	MovingLandingOverlayClipIndex = INDEX_NONE;
	MovingLandingOverlayTimeSeconds = 0.0f;
	MovingLandingOverlayRemainingSeconds = 0.0f;
	DebugAnimationClipIndex = INDEX_NONE;
	SectionStates.Reset();
	RuntimeSectionSubmeshIds.Reset();

	HideNativeCharacterMesh(Character);

	UProceduralMeshComponent* PreviewMesh = NewObject<UProceduralMeshComponent>(Character, PreviewComponentName);
	PreviewMesh->bUseAsyncCooking = true;
	PreviewMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	PreviewMesh->SetCastShadow(false);
	PreviewMesh->AttachToComponent(Character->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
	const float CapsuleHalfHeight = Character->GetCapsuleComponent()
		? Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()
		: 88.0f;
	PreviewMesh->SetRelativeLocation(FVector(0.0f, 0.0f, -CapsuleHalfHeight));
	PreviewMesh->SetRelativeRotation(FRotator::ZeroRotator);
	Character->AddInstanceComponent(PreviewMesh);
	PreviewMesh->RegisterComponent();

	TMap<int32, int32> GeosetGroups = LoadConfiguredGeosets();
	BuildDefaultGeosetSelection(Submeshes, GeosetGroups);
	const FWoWPreviewMaterials PreviewMaterials = LoadPreviewMaterials();
	TMap<FString, TObjectPtr<UTexture2D>> TextureCache;
	int32 CreatedSectionCount = 0;
	for (const FWoWPreviewSection& Section : Sections)
	{
		if (!IsRuntimeSectionVisible(Section.SubmeshId, Section.TextureType, GeosetGroups) ||
			Section.Positions.Num() == 0 ||
			Section.Indices.Num() == 0 ||
			Section.Indices.Num() % 3 != 0)
		{
			continue;
		}

		TArray<FVector> SectionNormals = Section.Normals;
		TArray<FVector2D> SectionUV0 = Section.UV0;
		TArray<FLinearColor> SectionVertexColors = Section.VertexColors;
		if (SectionNormals.Num() != Section.Positions.Num())
		{
			SectionNormals.SetNumZeroed(Section.Positions.Num());
		}
		if (SectionUV0.Num() != Section.Positions.Num())
		{
			SectionUV0.SetNumZeroed(Section.Positions.Num());
		}
		if (SectionVertexColors.Num() != Section.Positions.Num())
		{
			SectionVertexColors.Init(FLinearColor::White, Section.Positions.Num());
		}
		TArray<FProcMeshTangent> Tangents;
		UKismetProceduralMeshLibrary::CalculateTangentsForMesh(Section.Positions, Section.Indices, SectionUV0, SectionNormals, Tangents);
		PreviewMesh->CreateMeshSection_LinearColor(CreatedSectionCount, Section.Positions, Section.Indices, SectionNormals, SectionUV0, SectionVertexColors, Tangents, false);
		PreviewMesh->SetMaterial(CreatedSectionCount, CreatePreviewSectionMaterial(Section, PreviewMaterials, PreviewMesh, TextureCache));

		FWoWCharacterPreviewSectionState SectionState;
		SectionState.BasePositions = Section.Positions;
		SectionState.BoneWeights = Section.BoneWeights;
		SectionState.BoneIndices = Section.BoneIndices;
		SectionState.SkinnedPositions = Section.Positions;
		SectionState.Normals = MoveTemp(SectionNormals);
		SectionState.UV0 = SectionUV0;
		SectionState.BaseUV0 = MoveTemp(SectionUV0);
		SectionState.VertexColors = MoveTemp(SectionVertexColors);
		SectionState.Tangents = MoveTemp(Tangents);
		SectionState.TextureTransformSamples = Section.TextureTransformSamples;
		SectionState.ColorSamples = Section.ColorSamples;
		SectionState.bScaleLowAlphaAdditive = Section.RenderMode == 4;
		SectionStates.Add(MoveTemp(SectionState));
		RuntimeSectionSubmeshIds.Add(Section.SubmeshId);
		++CreatedSectionCount;
	}

	PreviewMeshComponent = PreviewMesh;
	ApplyRuntimeGeosetVisibility();
	ApplyGeneratedSkeletalPreviewToCharacter(Character);
	UE_LOG(LogTemp, Display, TEXT("Attached WoW character preview component: sections=%d clips=%d textures=%d"), CreatedSectionCount, AnimationClips.Num(), TextureCache.Num());
}

void UWoWCharacterPreviewComponent::ApplyGeneratedSkeletalPreviewToCharacter(ACharacter* Character)
{
	if (!Character)
	{
		return;
	}
	HideNativeCharacterMesh(Character);
	if (GeneratedSkeletalMeshComponent.IsValid())
	{
		ApplyGeneratedRuntimeAppearanceIfNeeded(GeneratedSkeletalMeshComponent.Get());
		ApplyRuntimeGeosetVisibility();
		GeneratedSkeletalMeshComponent->SetAnimInstanceClass(UWoWM2PreviewAnimInstance::StaticClass());
		GeneratedSkeletalMeshComponent->SetVisibility(true, true);
		GeneratedSkeletalMeshComponent->SetHiddenInGame(false, true);
		if (bUseGeneratedSkeletalPreviewAsPrimaryVisual)
		{
			if (UProceduralMeshComponent* PreviewMesh = PreviewMeshComponent.Get())
			{
				SetLegacyPreviewBodyRendered(PreviewMesh, false);
			}
		}
		return;
	}

	for (UActorComponent* Component : Character->GetComponents())
	{
		if (Component &&
			(Component->GetFName() == GeneratedSkeletalComponentName ||
				Component->GetFName() == GeneratedGripPoseSourceComponentName ||
				Component->GetFName() == GeneratedPoseableComponentName))
		{
			// 原生骨骼预览组件由命令行生成的资产驱动；重新进入关卡时销毁旧组件，避免拿到过期动画状态。
			Component->DestroyComponent();
		}
	}

	FString GeneratedMeshPath;
	USkeletalMesh* GeneratedMesh = LoadGeneratedPlayerCharacterMesh(GeneratedCharacterMeshPath, GeneratedMeshPath);
	if (!GeneratedMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("Generated WoW skeletal mesh was not found for selected runtime appearance: requested=%s"),
			*GeneratedCharacterMeshPath);
		if (UProceduralMeshComponent* PreviewMesh = PreviewMeshComponent.Get())
		{
			SetLegacyPreviewBodyRendered(PreviewMesh, true);
		}
		return;
	}

	USkeletalMeshComponent* SkeletalPreview = NewObject<USkeletalMeshComponent>(Character, GeneratedSkeletalComponentName);
	SkeletalPreview->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	SkeletalPreview->SetCastShadow(false);
	SkeletalPreview->AttachToComponent(Character->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
	const float CapsuleHalfHeight = Character->GetCapsuleComponent()
		? Character->GetCapsuleComponent()->GetScaledCapsuleHalfHeight()
		: 88.0f;
	SkeletalPreview->SetRelativeLocation(FVector(0.0f, 0.0f, -CapsuleHalfHeight));
	SkeletalPreview->SetRelativeRotation(FRotator::ZeroRotator);
	SkeletalPreview->SetSkeletalMesh(GeneratedMesh);
	SkeletalPreview->SetAnimInstanceClass(UWoWM2PreviewAnimInstance::StaticClass());
	ApplyGeneratedRuntimeAppearanceIfNeeded(SkeletalPreview);
	SkeletalPreview->SetVisibility(true, true);
	SkeletalPreview->SetHiddenInGame(false, true);
	Character->AddInstanceComponent(SkeletalPreview);
	SkeletalPreview->RegisterComponent();

	GeneratedSkeletalMeshComponent = SkeletalPreview;
	GeneratedSkeletalAnimationId = INDEX_NONE;
	GeneratedSkeletalSubAnimationId = INDEX_NONE;
	GeneratedSkeletalGripVariant = GeneratedGripVariantNone;
	GeneratedSkeletalAnimationTimeSeconds = 0.0f;
	ApplyRuntimeGeosetVisibility();

	if (bUseGeneratedSkeletalPreviewAsPrimaryVisual)
	{
		if (UProceduralMeshComponent* PreviewMesh = PreviewMeshComponent.Get())
		{
			// 暂时只隐藏旧角色身体，不向子组件传播；武器/Ribbon 仍依赖旧组件作为挂点计算参照。
			SetLegacyPreviewBodyRendered(PreviewMesh, false);
		}
	}

	UE_LOG(LogTemp, Display, TEXT("Attached generated WoW skeletal preview: %s"), *GeneratedMeshPath);
}

void UWoWCharacterPreviewComponent::ApplyGeneratedRuntimeAppearanceIfNeeded(USkeletalMeshComponent* SkeletalPreview)
{
	if (!SkeletalPreview)
	{
		return;
	}

	const FString MeshPath = SkeletalPreview->GetSkeletalMeshAsset()
		? SkeletalPreview->GetSkeletalMeshAsset()->GetPathName()
		: FString();
	const FString MaterialKey = MeshPath + TEXT("|") + GeneratedCharacterMaterialPackagePath;
	if (!AppliedGeneratedMaterialKey.Equals(MaterialKey, ESearchCase::CaseSensitive))
	{
		ApplyGeneratedPlayerCharacterMaterials(SkeletalPreview, GeneratedCharacterMaterialPackagePath);
		AppliedGeneratedMaterialKey = MaterialKey;
		AppliedRuntimeBodyTextureKey.Empty();
	}

	if (!bHasRuntimeAppearance || bUsingBakedRuntimeAppearanceAsset)
	{
		return;
	}

	FWoWRuntimeCharacterAppearanceDescriptor RuntimeDescriptor;
	RuntimeDescriptor.Guid = RuntimeGuid;
	RuntimeDescriptor.Race = RuntimeRace;
	RuntimeDescriptor.Gender = RuntimeGender;
	RuntimeDescriptor.Skin = RuntimeSkin;
	RuntimeDescriptor.Face = RuntimeFace;
	RuntimeDescriptor.HairStyle = RuntimeHairStyle;
	RuntimeDescriptor.HairColor = RuntimeHairColor;
	RuntimeDescriptor.FacialStyle = RuntimeFacialStyle;
	RuntimeDescriptor.EquipmentSummary = RuntimeEquipmentSummary;

	const FString BodyTextureKey = MeshPath + TEXT("|") + RuntimeSectionMetadataPath + TEXT("|") + WoWRuntimeCharacterAppearance::BuildBodyAtlasSignature(RuntimeDescriptor);
	if (AppliedRuntimeBodyTextureKey.Equals(BodyTextureKey, ESearchCase::CaseSensitive))
	{
		return;
	}

	WoWRuntimeCharacterAppearance::ApplyRuntimeBodyTextures(SkeletalPreview, RuntimeDescriptor, RuntimeSectionMetadataPath);
	AppliedRuntimeBodyTextureKey = BodyTextureKey;
}

void UWoWCharacterPreviewComponent::ApplyRuntimeGeosetVisibility()
{
	if (!bHasRuntimeAppearance || GeneratedCharacterMeshJsonPath.IsEmpty())
	{
		return;
	}
	if (bUsingBakedRuntimeAppearanceAsset)
	{
		UE_LOG(LogTemp, Display, TEXT("Runtime character uses baked signature appearance mesh; applying metadata geoset visibility when slots expose M2 sections: race=%d class=%d gender=%d mesh=%s metadata=%s"),
			RuntimeRace,
			RuntimeClassId,
			RuntimeGender,
			*GeneratedCharacterMeshPath,
			*RuntimeSectionMetadataPath);
	}

	TSet<int32> AvailableSubmeshIds;
	TArray<int32> SkeletalSectionSubmeshIds;
	TArray<FRuntimeSectionGeosetInfo> SectionGeosetInfos;
	if (ReadRuntimeSectionGeosetInfoFromMetadata(RuntimeSectionMetadataPath, SectionGeosetInfos))
	{
		SkeletalSectionSubmeshIds.Reserve(SectionGeosetInfos.Num());
		for (const FRuntimeSectionGeosetInfo& Section : SectionGeosetInfos)
		{
			SkeletalSectionSubmeshIds.Add(Section.SubmeshId);
			AvailableSubmeshIds.Add(Section.SubmeshId);
		}
	}
	else
	{
		for (const int32 SubmeshId : RuntimeSectionSubmeshIds)
		{
			AvailableSubmeshIds.Add(SubmeshId);
		}
		if (AvailableSubmeshIds.IsEmpty())
		{
			UE_LOG(LogTemp, Warning, TEXT("Runtime character section metadata missing or invalid; using mesh json section order: race=%d gender=%d metadata=%s meshJson=%s"),
				RuntimeRace,
				RuntimeGender,
				*RuntimeSectionMetadataPath,
				*GeneratedCharacterMeshJsonPath);
			FString JsonText;
			if (!FFileHelper::LoadFileToString(JsonText, *GeneratedCharacterMeshJsonPath))
			{
				return;
			}

			TSharedPtr<FJsonObject> RootObject;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
			if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
			{
				return;
			}

			const TArray<TSharedPtr<FJsonValue>>* SectionValues = nullptr;
			if (!RootObject->TryGetArrayField(TEXT("sections"), SectionValues) || !SectionValues)
			{
				return;
			}

			RuntimeSectionSubmeshIds.Reset();
			RuntimeSectionSubmeshIds.Reserve(SectionValues->Num());
			for (const TSharedPtr<FJsonValue>& Value : *SectionValues)
			{
				const TSharedPtr<FJsonObject>* SectionObject = nullptr;
				if (!Value.IsValid() || !Value->TryGetObject(SectionObject) || !SectionObject || !SectionObject->IsValid())
				{
					continue;
				}
				double Number = 0.0;
				const int32 SubmeshId = (*SectionObject)->TryGetNumberField(TEXT("submesh_id"), Number)
					? FMath::RoundToInt(Number)
					: 0;
				FRuntimeSectionGeosetInfo SectionInfo;
				SectionInfo.SubmeshId = SubmeshId;
				SectionInfo.TextureType = (*SectionObject)->TryGetNumberField(TEXT("texture_type"), Number)
					? FMath::RoundToInt(Number)
					: INDEX_NONE;
				SectionInfo.RenderMode = (*SectionObject)->TryGetNumberField(TEXT("render_mode"), Number)
					? FMath::RoundToInt(Number)
					: INDEX_NONE;
				SectionInfo.MaterialFlags = (*SectionObject)->TryGetNumberField(TEXT("material_flags"), Number)
					? FMath::RoundToInt(Number)
					: 0;
				(*SectionObject)->TryGetBoolField(TEXT("unlit"), SectionInfo.bUnlit);
				(*SectionObject)->TryGetBoolField(TEXT("no_depth_write"), SectionInfo.bNoDepthWrite);
				(*SectionObject)->TryGetStringField(TEXT("preview_png"), SectionInfo.PreviewPng);
				RuntimeSectionSubmeshIds.Add(SubmeshId);
				AvailableSubmeshIds.Add(SubmeshId);
				SectionGeosetInfos.Add(MoveTemp(SectionInfo));
			}
		}
	}

	TMap<int32, int32> Geosets;
	BuildRuntimeDefaultGeosets(AvailableSubmeshIds, SectionGeosetInfos, RuntimeClassId, Geosets);
	const FRuntimeCharacterAppearanceDbcCache& Cache = GetRuntimeAppearanceDbcCache();
	if (Cache.bLoaded)
	{
		ApplyRuntimeEquipmentGeosetRules(Cache, RuntimeEquipmentSummary, AvailableSubmeshIds, Geosets);

		const FRuntimeCharacterHelmetVis* HelmetVis = ResolveRuntimeHelmetVis(Cache, RuntimeEquipmentSummary, RuntimeGender);
		const bool bHideHair = HelmetVis && RuntimeHelmetFlagsHideRace(HelmetVis->HairFlags, RuntimeRace);
		const bool bHideFacial1 = HelmetVis && RuntimeHelmetFlagsHideRace(HelmetVis->Facial1Flags, RuntimeRace);
		const bool bHideFacial2 = HelmetVis && RuntimeHelmetFlagsHideRace(HelmetVis->Facial2Flags, RuntimeRace);
		const bool bHideFacial3 = HelmetVis && RuntimeHelmetFlagsHideRace(HelmetVis->Facial3Flags, RuntimeRace);
		const bool bHideEars = HelmetVis && RuntimeHelmetFlagsHideRace(HelmetVis->EarsFlags, RuntimeRace);

		const int32 HairGeosetId = ResolveRuntimeHairGeosetId(Cache, RuntimeRace, RuntimeGender, RuntimeHairStyle);
		if (!bHideHair && HairGeosetId > 0 && AvailableSubmeshIds.Contains(HairGeosetId))
		{
			Geosets.Add(0, HairGeosetId);
		}
		else
		{
			Geosets.Add(0, 0);
		}

		const FRuntimeCharacterFacialHairStyle* Facial = ResolveRuntimeFacialHairStyle(Cache, RuntimeRace, RuntimeGender, RuntimeFacialStyle);
		Geosets.Add(static_cast<int32>(ERuntimeCharacterGeosetGroup::Facial1), (!bHideFacial1 && Facial) ? Facial->Geoset100 : 0);
		Geosets.Add(static_cast<int32>(ERuntimeCharacterGeosetGroup::Facial2), (!bHideFacial2 && Facial) ? Facial->Geoset200 : 0);
		Geosets.Add(static_cast<int32>(ERuntimeCharacterGeosetGroup::Facial3), (!bHideFacial3 && Facial) ? Facial->Geoset300 : 0);
		if (bHideEars)
		{
			Geosets.Add(static_cast<int32>(ERuntimeCharacterGeosetGroup::Ears), 0);
		}
	}

	int32 Applied = 0;
	int32 Visible = 0;
	if (USkeletalMeshComponent* SkeletalPreview = GeneratedSkeletalMeshComponent.Get())
	{
		const USkeletalMesh* SkeletalMesh = SkeletalPreview->GetSkeletalMeshAsset();
		const FSkeletalMeshRenderData* RenderData = SkeletalMesh ? SkeletalMesh->GetResourceForRendering() : nullptr;
		const int32 LOD0RenderSectionCount = (RenderData && RenderData->LODRenderData.IsValidIndex(0))
			? RenderData->LODRenderData[0].RenderSections.Num()
			: 0;
		const TArray<int32>& SectionMappingForSkeletalMesh = SkeletalSectionSubmeshIds.IsEmpty()
			? RuntimeSectionSubmeshIds
			: SkeletalSectionSubmeshIds;
		ApplySkeletalM2SectionVisibility(
			SkeletalPreview,
			SectionMappingForSkeletalMesh,
			[&Geosets, &SectionGeosetInfos](int32 SubmeshId, int32 M2SectionIndex)
			{
				int32 TextureType = INDEX_NONE;
				FString PreviewPng;
				if (SectionGeosetInfos.IsValidIndex(M2SectionIndex))
				{
					const FRuntimeSectionGeosetInfo& Section = SectionGeosetInfos[M2SectionIndex];
					TextureType = Section.TextureType;
					PreviewPng = Section.PreviewPng;
				}
				const bool bVisible = IsRuntimeSectionVisible(SubmeshId, TextureType, Geosets);
				if (SubmeshId == 0 || (SubmeshId >= 1500 && SubmeshId < 1800))
				{
					const FRuntimeSectionGeosetInfo* Section = SectionGeosetInfos.IsValidIndex(M2SectionIndex) ? &SectionGeosetInfos[M2SectionIndex] : nullptr;
					UE_LOG(LogTemp, Display, TEXT("Runtime character key section visibility: m2Section=%d submesh=%d textureType=%d renderMode=%d materialFlags=0x%X unlit=%d noDepthWrite=%d visible=%d preview=%s"),
						M2SectionIndex,
						SubmeshId,
						TextureType,
						Section ? Section->RenderMode : INDEX_NONE,
						Section ? Section->MaterialFlags : 0,
						Section && Section->bUnlit ? 1 : 0,
						Section && Section->bNoDepthWrite ? 1 : 0,
						bVisible ? 1 : 0,
						*PreviewPng);
				}
				return bVisible;
			},
			Applied,
			Visible);
		UE_LOG(LogTemp, Display, TEXT("Runtime character skeletal section audit: mesh=%s metadata=%s m2Sections=%d lod0RenderSections=%d applied=%d visible=%d"),
			SkeletalMesh ? *SkeletalMesh->GetPathName() : TEXT("<null>"),
			*RuntimeSectionMetadataPath,
			SectionMappingForSkeletalMesh.Num(),
			LOD0RenderSectionCount,
			Applied,
			Visible);
	}

	if (UProceduralMeshComponent* PreviewMesh = PreviewMeshComponent.Get())
	{
		const int32 SectionCount = FMath::Min(PreviewMesh->GetNumSections(), RuntimeSectionSubmeshIds.Num());
		for (int32 SectionIndex = 0; SectionIndex < SectionCount; ++SectionIndex)
		{
			const int32 TextureType = SectionGeosetInfos.IsValidIndex(SectionIndex) ? SectionGeosetInfos[SectionIndex].TextureType : INDEX_NONE;
			const bool bVisible = IsRuntimeSectionVisible(RuntimeSectionSubmeshIds[SectionIndex], TextureType, Geosets);
			PreviewMesh->SetMeshSectionVisible(SectionIndex, bVisible);
		}
	}

	UE_LOG(LogTemp, Display, TEXT("Runtime character geoset visibility applied: race=%d gender=%d hair=%d facial=%d equipment=%s availableSubmeshes=%d selectedGroups=%d sections=%d visible=%d"),
		RuntimeRace,
		RuntimeGender,
		RuntimeHairStyle,
		RuntimeFacialStyle,
		*RuntimeEquipmentSummary,
		AvailableSubmeshIds.Num(),
		Geosets.Num(),
		Applied,
		Visible);
}

void UWoWCharacterPreviewComponent::DestroyRuntimeEquipmentAttachments()
{
	for (TObjectPtr<UWoWM2ModelComponent>& AttachmentComponent : RuntimeEquipmentAttachmentComponents)
	{
		if (AttachmentComponent)
		{
			AttachmentComponent->DestroyComponent();
		}
	}
	RuntimeEquipmentAttachmentComponents.Reset();
}

bool UWoWCharacterPreviewComponent::LoadRuntimeAttachmentDefinitions()
{
	if (!RuntimeAttachmentDefinitions.IsEmpty())
	{
		return true;
	}
	if (RuntimeAttachmentDescriptorPath.IsEmpty() || !FPaths::FileExists(RuntimeAttachmentDescriptorPath))
	{
		return false;
	}

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *RuntimeAttachmentDescriptorPath))
	{
		return false;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("Runtime character attachment descriptor parse failed: %s"), *RuntimeAttachmentDescriptorPath);
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* AttachmentValues = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("attachments"), AttachmentValues) || !AttachmentValues)
	{
		return false;
	}

	for (const TSharedPtr<FJsonValue>& AttachmentValue : *AttachmentValues)
	{
		const TSharedPtr<FJsonObject>* AttachmentObject = nullptr;
		if (!AttachmentValue.IsValid() || !AttachmentValue->TryGetObject(AttachmentObject) || !AttachmentObject || !AttachmentObject->IsValid())
		{
			continue;
		}

		FWoWRuntimeCharacterAttachmentDefinition Definition;
		double Number = 0.0;
		(*AttachmentObject)->TryGetStringField(TEXT("name"), Definition.Name);
		(*AttachmentObject)->TryGetStringField(TEXT("component"), Definition.ComponentType);
		if ((*AttachmentObject)->TryGetNumberField(TEXT("slot"), Number))
		{
			Definition.Slot = FMath::RoundToInt(Number);
		}
		if ((*AttachmentObject)->TryGetNumberField(TEXT("display_id"), Number))
		{
			Definition.DisplayId = FMath::RoundToInt(Number);
		}
		if ((*AttachmentObject)->TryGetNumberField(TEXT("inventory_type"), Number))
		{
			Definition.InventoryType = FMath::RoundToInt(Number);
		}
		FString SocketText;
		(*AttachmentObject)->TryGetStringField(TEXT("socket"), SocketText);
		Definition.SocketName = SocketText.IsEmpty() ? NAME_None : FName(*SocketText);
		(*AttachmentObject)->TryGetStringField(TEXT("mesh_json"), Definition.MeshJsonPath);
		if (!Definition.MeshJsonPath.IsEmpty())
		{
			Definition.MeshJsonPath = FPaths::ConvertRelativePathToFull(Definition.MeshJsonPath);
		}
		if (!Definition.MeshJsonPath.IsEmpty() && FPaths::FileExists(Definition.MeshJsonPath) && !Definition.SocketName.IsNone())
		{
			RuntimeAttachmentDefinitions.Add(MoveTemp(Definition));
		}
	}

	UE_LOG(LogTemp, Display, TEXT("Runtime character attachment descriptor loaded: definitions=%d path=%s"),
		RuntimeAttachmentDefinitions.Num(),
		*RuntimeAttachmentDescriptorPath);
	return !RuntimeAttachmentDefinitions.IsEmpty();
}

void UWoWCharacterPreviewComponent::ApplyPreviewEquipmentToCharacter(ACharacter* Character)
{
	if (!bEnableDefaultPreviewEquipment)
	{
		bPreviewEquipmentVisible = false;
		DestroyRuntimeEquipmentAttachments();
		if (AttachedWeaponMeshComponent.IsValid())
		{
			AttachedWeaponMeshComponent->SetVisibility(false, true);
		}
		if (AttachedRibbonMeshComponent.IsValid())
		{
			AttachedRibbonMeshComponent->SetVisibility(false, true);
		}
		return;
	}

	if (AttachedM2ModelComponent.IsValid() && AttachedWeaponMeshComponent.IsValid())
	{
		AttachedWeaponMeshComponent->SetVisibility(bPreviewEquipmentVisible, true);
		if (AttachedRibbonMeshComponent.IsValid())
		{
			AttachedRibbonMeshComponent->SetVisibility(bPreviewEquipmentVisible, true);
		}
		UpdateAttachedWeaponTransform();
		return;
	}
	if (AttachedWeaponMeshComponent.IsValid() && AttachedM2SectionStates.Num() > 0)
	{
		AttachedWeaponMeshComponent->SetVisibility(bPreviewEquipmentVisible, true);
		if (AttachedRibbonMeshComponent.IsValid())
		{
			AttachedRibbonMeshComponent->SetVisibility(bPreviewEquipmentVisible, true);
		}
		return;
	}

	for (UActorComponent* Component : Character->GetComponents())
	{
		if (!Component)
		{
			continue;
		}

		if (Component->GetFName() == AttachedWeaponComponentName || Component->GetFName() == AttachedRibbonComponentName)
		{
			// 武器/Ribbon 同样必须每次从 M2 JSON 重建。只拿到旧 ProceduralMeshComponent 会让丝带运行状态为空，
			// 画面上看得到剑身，但中心光效和拖尾不会再更新。
			Component->DestroyComponent();
		}
	}

	AttachedWeaponMeshComponent.Reset();
	AttachedRibbonMeshComponent.Reset();
	if (UWoWM2ModelComponent* ExistingModelComponent = AttachedM2ModelComponent.Get())
	{
		ExistingModelComponent->DestroyComponent();
	}
	AttachedM2ModelComponent.Reset();
	AttachedM2SectionStates.Reset();
	AttachedM2AnimationClips.Reset();
	AttachedM2CurrentBoneMatrices.Reset();
	AttachedM2Bones.Reset();
	AttachedRibbonStates.Reset();
	AttachedM2AnimationTimeSeconds = 0.0f;
	AttachedM2CurrentAnimationClipIndex = INDEX_NONE;

	USkeletalMeshComponent* SkeletalPreview = GeneratedSkeletalMeshComponent.Get();
	if (SkeletalPreview && LoadRuntimeAttachmentDefinitions())
	{
		int32 AttachedCount = 0;
		for (const FWoWRuntimeCharacterAttachmentDefinition& Definition : RuntimeAttachmentDefinitions)
		{
			const bool bIsMainHandWeapon = Definition.ComponentType.Equals(TEXT("Weapon"), ESearchCase::IgnoreCase) && Definition.Slot != 16;
			if (bIsMainHandWeapon)
			{
				continue;
			}

			if (!SkeletalPreview->DoesSocketExist(Definition.SocketName))
			{
				UE_LOG(LogTemp, Warning, TEXT("Runtime character attachment socket missing: name=%s socket=%s mesh=%s"),
					*Definition.Name,
					*Definition.SocketName.ToString(),
					*GeneratedCharacterMeshPath);
				continue;
			}

			UWoWM2ModelComponent* AttachmentComponent = NewObject<UWoWM2ModelComponent>(Character);
			const FTransform RelativeTransform(Definition.RelativeRotation, Definition.RelativeLocation, Definition.RelativeScale);
			if (AttachmentComponent && AttachmentComponent->LoadFromMeshJson(SkeletalPreview, Definition.SocketName, Definition.MeshJsonPath, RelativeTransform))
			{
				Character->AddInstanceComponent(AttachmentComponent);
				AttachmentComponent->RegisterComponent();
				RuntimeEquipmentAttachmentComponents.Add(AttachmentComponent);
				++AttachedCount;
			}
			else if (AttachmentComponent)
			{
				AttachmentComponent->DestroyComponent();
			}
		}
		UE_LOG(LogTemp, Display, TEXT("Runtime character equipment attachments applied: descriptor=%s definitions=%d attached=%d"),
			*RuntimeAttachmentDescriptorPath,
			RuntimeAttachmentDefinitions.Num(),
			AttachedCount);
	}

	int32 MainHandDisplayId = ResolvePreviewMainHandDisplayId();
	FString MeshPath;
	if (LoadRuntimeAttachmentDefinitions())
	{
		for (const FWoWRuntimeCharacterAttachmentDefinition& Definition : RuntimeAttachmentDefinitions)
		{
			if (Definition.ComponentType.Equals(TEXT("Weapon"), ESearchCase::IgnoreCase) && Definition.Slot != 16 && !Definition.MeshJsonPath.IsEmpty())
			{
				MainHandDisplayId = Definition.DisplayId > 0 ? Definition.DisplayId : MainHandDisplayId;
				MeshPath = Definition.MeshJsonPath;
				break;
			}
		}
	}
	if (MeshPath.IsEmpty())
	{
		MeshPath = ResolveItemObjectComponentMeshJsonByDisplayId(MainHandDisplayId, TEXT("Weapon"));
	}
	if (MeshPath.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("WoW preview equipment displayId could not be resolved: displayId=%d"), MainHandDisplayId);
		return;
	}

	USceneComponent* AttachParent = PreviewMeshComponent.IsValid()
		? Cast<USceneComponent>(PreviewMeshComponent.Get())
		: Character->GetRootComponent();
	if (!AttachParent)
	{
		return;
	}

	UWoWM2ModelComponent* M2ModelComponent = NewObject<UWoWM2ModelComponent>(Character);
	if (M2ModelComponent && M2ModelComponent->LoadFromMeshJson(AttachParent, NAME_None, MeshPath, FTransform::Identity))
	{
		Character->AddInstanceComponent(M2ModelComponent);
		M2ModelComponent->RegisterComponent();
		AttachedM2ModelComponent = M2ModelComponent;
		AttachedWeaponMeshComponent = M2ModelComponent->GetModelMeshComponent();
		AttachedRibbonMeshComponent = M2ModelComponent->GetRibbonMeshComponent();
		UpdateAttachedWeaponTransform();
		UE_LOG(LogTemp, Display, TEXT("Attached WoW preview item display through shared M2 component: displayId=%d meshJson=%s"), MainHandDisplayId, *MeshPath);
		return;
	}
	if (M2ModelComponent)
	{
		M2ModelComponent->DestroyComponent();
	}

	UE_LOG(LogTemp, Warning, TEXT("WoW preview item failed to load through shared M2 component: displayId=%d meshJson=%s"), MainHandDisplayId, *MeshPath);
}

void UWoWCharacterPreviewComponent::SetDebugAttachedWeaponSlot(int32 AttachmentId)
{
	const bool bValidAttachment = AttachmentId == RightPalmAttachmentId || AttachmentId == LeftPalmAttachmentId;
	bPreviewEquipmentVisible = bValidAttachment && bEnableDefaultPreviewEquipment;
	bDebugWeaponSheathedPreview = false;
	bWeaponSheathed = false;
	bWeaponSheatheTransitionActive = false;
	WeaponSheatheTransitionElapsedSeconds = 0.0f;
	WeaponSheatheTransitionSwitchSeconds = 0.0f;
	WeaponSheatheTransitionClipSeconds = 0.0f;
	WeaponSheatheTransitionBlendOutSeconds = 0.0f;
	WeaponSheatheTransitionDurationSeconds = 0.0f;
	WeaponSheatheTransitionAnimationId = INDEX_NONE;
	ActiveWeaponAttachmentId = bValidAttachment ? AttachmentId : INDEX_NONE;
	ActiveWeaponSheathedAttachmentId = LargeWeaponSheathedAttachmentId;
	RebuildWeaponGripPoseLayer();

	if (AttachedWeaponMeshComponent.IsValid())
	{
		AttachedWeaponMeshComponent->SetVisibility(bPreviewEquipmentVisible, true);
	}
	if (AttachedRibbonMeshComponent.IsValid())
	{
		AttachedRibbonMeshComponent->SetVisibility(bPreviewEquipmentVisible, true);
	}

	if (bPreviewEquipmentVisible)
	{
		if (ACharacter* Character = PreviewCharacter.Get())
		{
			ApplyPreviewEquipmentToCharacter(Character);
			UpdateAttachedWeaponTransform();
		}
	}

	const TCHAR* SlotName = AttachmentId == RightPalmAttachmentId
		? TEXT("right_hand")
		: (AttachmentId == LeftPalmAttachmentId ? TEXT("left_hand") : TEXT("none"));
	UE_LOG(LogTemp, Display, TEXT("Preview equipment slot switched: %s attachment_id=%d"), SlotName, ActiveWeaponAttachmentId);
}

void UWoWCharacterPreviewComponent::CycleDebugSheathedAttachment()
{
	bDebugWeaponSheathedPreview = true;

	int32 CurrentIndex = INDEX_NONE;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(DebugSheathedAttachmentIds); ++Index)
	{
		if (DebugSheathedAttachmentIds[Index] == ActiveWeaponSheathedAttachmentId)
		{
			CurrentIndex = Index;
			break;
		}
	}
	const int32 NextIndex = CurrentIndex == INDEX_NONE ? 0 : (CurrentIndex + 1) % UE_ARRAY_COUNT(DebugSheathedAttachmentIds);
	ActiveWeaponSheathedAttachmentId = DebugSheathedAttachmentIds[NextIndex];
	if (ACharacter* Character = PreviewCharacter.Get())
	{
		UpdateAttachedWeaponTransform();
	}
	ShowDebugSheathedMessage();
}

void UWoWCharacterPreviewComponent::DebugCycleSheathedRotation()
{
	CycleDebugSheathedRotation();
}

void UWoWCharacterPreviewComponent::DebugCycleSheathedNudgeMode()
{
	CycleDebugSheathedNudgeMode();
}

void UWoWCharacterPreviewComponent::DebugAdjustSheathedNudge(float Direction)
{
	AdjustDebugSheathedNudge(Direction);
}

FWoWSheathedWeaponDebugState UWoWCharacterPreviewComponent::GetSheathedWeaponDebugState() const
{
	FWoWSheathedWeaponDebugState State;
	State.AttachmentId = ActiveWeaponSheathedAttachmentId;
	State.AttachmentName = GetAttachmentDebugName(ActiveWeaponSheathedAttachmentId);
	State.RotationIndex = ActiveWeaponSheathedRotationIndex;
	State.BaseRotation = DebugSheathedRotations[FMath::Clamp(ActiveWeaponSheathedRotationIndex, 0, UE_ARRAY_COUNT(DebugSheathedRotations) - 1)];
	State.LocationOffset = DebugSheathedBackWeaponLocationOffset;
	State.RotationOffset = DebugSheathedBackWeaponRotationOffset;
	State.ExportText = FString::Printf(
		TEXT("attach=%d(%s) rotIndex=%d baseRot=%s loc=%s rotNudge=%s"),
		State.AttachmentId,
		*State.AttachmentName,
		State.RotationIndex,
		*State.BaseRotation.ToString(),
		*State.LocationOffset.ToString(),
		*State.RotationOffset.ToString());
	return State;
}

void UWoWCharacterPreviewComponent::SetDebugSheathedLocationOffset(const FVector& InOffset)
{
	DebugSheathedBackWeaponLocationOffset = InOffset;
	bDebugWeaponSheathedPreview = true;
	UpdateAttachedWeaponTransform();
}

void UWoWCharacterPreviewComponent::SetDebugSheathedRotationOffset(const FRotator& InOffset)
{
	DebugSheathedBackWeaponRotationOffset = InOffset;
	bDebugWeaponSheathedPreview = true;
	UpdateAttachedWeaponTransform();
}

void UWoWCharacterPreviewComponent::SetDebugWeaponSheathedPreview(bool bInPreview)
{
	bDebugWeaponSheathedPreview = bInPreview;
	bWeaponSheatheTransitionActive = false;
	WeaponSheatheTransitionElapsedSeconds = 0.0f;
	WeaponSheatheTransitionSwitchSeconds = 0.0f;
	WeaponSheatheTransitionClipSeconds = 0.0f;
	WeaponSheatheTransitionBlendOutSeconds = 0.0f;
	WeaponSheatheTransitionDurationSeconds = 0.0f;
	WeaponSheatheTransitionAnimationId = INDEX_NONE;
	UpdateAttachedWeaponTransform();
}

void UWoWCharacterPreviewComponent::ResetDebugSheathedOffsetsToDefault()
{
	DebugSheathedBackWeaponLocationOffset = DefaultDebugSheathedBackWeaponLocationOffset;
	DebugSheathedBackWeaponRotationOffset = DefaultDebugSheathedBackWeaponRotationOffset;
	bDebugWeaponSheathedPreview = true;
	UpdateAttachedWeaponTransform();
}

void UWoWCharacterPreviewComponent::CycleDebugSheathedRotation()
{
	bDebugWeaponSheathedPreview = true;
	ActiveWeaponSheathedRotationIndex = (ActiveWeaponSheathedRotationIndex + 1) % UE_ARRAY_COUNT(DebugSheathedRotations);
	UpdateAttachedWeaponTransform();
	ShowDebugSheathedMessage();
}

void UWoWCharacterPreviewComponent::CycleDebugSheathedNudgeMode()
{
	DebugSheathedNudgeMode = (DebugSheathedNudgeMode + 1) % 6;
	bDebugWeaponSheathedPreview = true;
	ShowDebugSheathedMessage();
}

void UWoWCharacterPreviewComponent::AdjustDebugSheathedNudge(float Direction)
{
	const float Step = 1.0f;
	switch (DebugSheathedNudgeMode)
	{
	case 0: DebugSheathedBackWeaponLocationOffset.X += Step * Direction; break;
	case 1: DebugSheathedBackWeaponLocationOffset.Y += Step * Direction; break;
	case 2: DebugSheathedBackWeaponLocationOffset.Z += Step * Direction; break;
	case 3: DebugSheathedBackWeaponRotationOffset.Pitch += 1.0f * Direction; break;
	case 4: DebugSheathedBackWeaponRotationOffset.Yaw += 1.0f * Direction; break;
	case 5: DebugSheathedBackWeaponRotationOffset.Roll += 1.0f * Direction; break;
	default: break;
	}
	bDebugWeaponSheathedPreview = true;
	UpdateAttachedWeaponTransform();
	ShowDebugSheathedMessage();
}

void UWoWCharacterPreviewComponent::ShowDebugSheathedMessage() const
{
	const FRotator CurrentRotation = DebugSheathedRotations[FMath::Clamp(ActiveWeaponSheathedRotationIndex, 0, UE_ARRAY_COUNT(DebugSheathedRotations) - 1)];
	const FString Message = FString::Printf(
		TEXT("Sheath attach=%d(%s) rotIndex=%d rot=%s nudgeMode=%d loc=%s rotNudge=%s standing-preview=%s"),
		ActiveWeaponSheathedAttachmentId,
		GetAttachmentDebugName(ActiveWeaponSheathedAttachmentId),
		ActiveWeaponSheathedRotationIndex,
		*CurrentRotation.ToString(),
		DebugSheathedNudgeMode,
		*DebugSheathedBackWeaponLocationOffset.ToString(),
		*DebugSheathedBackWeaponRotationOffset.ToString(),
		IsSitting() ? TEXT("false") : TEXT("true"));
	UE_LOG(LogTemp, Display, TEXT("%s"), *Message);
	if (GEngine)
	{
		GEngine->AddOnScreenDebugMessage(INDEX_NONE, 2.0f, FColor::Yellow, Message);
	}
}

void UWoWCharacterPreviewComponent::UpdateAnimation(float DeltaTime)
{
	UProceduralMeshComponent* PreviewMesh = PreviewMeshComponent.Get();
	const bool bGeneratedPrimaryVisual = bUseGeneratedSkeletalPreviewAsPrimaryVisual && GeneratedSkeletalMeshComponent.IsValid();
	if ((!PreviewMesh && !bGeneratedPrimaryVisual) || AnimationClips.Num() == 0)
	{
		return;
	}

	if (!bCombatReady && CombatReadyLingerSeconds > 0.0f)
	{
		CombatReadyLingerSeconds = FMath::Max(CombatReadyLingerSeconds - DeltaTime, 0.0f);
	}

	bool bIsFallingNow = false;
	if (const ACharacter* Character = PreviewCharacter.Get())
	{
		if (const UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
		{
			bIsFallingNow = Movement->IsFalling();
		}
	}
	if (bWasFallingLastTick && !bIsFallingNow && DebugAnimationClipIndex == INDEX_NONE)
	{
		if (bJumpStartedWithGroundMovement && HasActiveMovementInput())
		{
			StartMovingLandingOverlay(MovingJumpLandOverlayStartTimeSeconds);
		}
		else
		{
			StartOneShotAnimation(39, 0, JumpLandMaxDurationSeconds);
		}
		bJumpStartedWithGroundMovement = false;
	}
	bWasFallingLastTick = bIsFallingNow;

	if (OneShotAnimationClipIndex != INDEX_NONE)
	{
		OneShotRemainingSeconds -= DeltaTime;
		if (OneShotRemainingSeconds <= 0.0f)
		{
			OneShotAnimationClipIndex = INDEX_NONE;
			OneShotRemainingSeconds = 0.0f;
			OneShotPlaybackRate = 1.0f;
			AnimationTimeSeconds = 0.0f;
		}
	}
	if (bWeaponSheatheTransitionActive)
	{
		const float PreviousElapsedSeconds = WeaponSheatheTransitionElapsedSeconds;
		WeaponSheatheTransitionElapsedSeconds += DeltaTime;
		if (PreviousElapsedSeconds < WeaponSheatheTransitionSwitchSeconds &&
			WeaponSheatheTransitionElapsedSeconds >= WeaponSheatheTransitionSwitchSeconds)
		{
			UpdateAttachedWeaponTransform();
		}
		const bool bDifferentOneShotInterrupted =
			OneShotAnimationClipIndex != INDEX_NONE &&
			AnimationClips.IsValidIndex(OneShotAnimationClipIndex) &&
			WeaponSheatheTransitionAnimationId != INDEX_NONE &&
			!IsClipForAnimationId(AnimationClips[OneShotAnimationClipIndex], WeaponSheatheTransitionAnimationId);
		const bool bSheatheTransitionFinished = WeaponSheatheTransitionElapsedSeconds >= FMath::Max(WeaponSheatheTransitionDurationSeconds, WeaponSheatheTransitionSwitchSeconds);
		if (bDifferentOneShotInterrupted || bSheatheTransitionFinished)
		{
			bWeaponSheatheTransitionActive = false;
			WeaponSheatheTransitionElapsedSeconds = 0.0f;
			WeaponSheatheTransitionSwitchSeconds = 0.0f;
			WeaponSheatheTransitionClipSeconds = 0.0f;
			WeaponSheatheTransitionBlendOutSeconds = 0.0f;
			WeaponSheatheTransitionDurationSeconds = 0.0f;
			WeaponSheatheTransitionAnimationId = INDEX_NONE;
		}
	}
	if (MovingLandingOverlayClipIndex != INDEX_NONE)
	{
		MovingLandingOverlayTimeSeconds += DeltaTime;
		MovingLandingOverlayRemainingSeconds -= DeltaTime;
		if (MovingLandingOverlayRemainingSeconds <= 0.0f)
		{
			MovingLandingOverlayClipIndex = INDEX_NONE;
			MovingLandingOverlayTimeSeconds = 0.0f;
			MovingLandingOverlayRemainingSeconds = 0.0f;
		}
	}
	UpdateVisualMovementFacing(DeltaTime);

	const int32 ClipIndex = ResolveAnimationClipIndex();
	if (!AnimationClips.IsValidIndex(ClipIndex))
	{
		return;
	}

	if (CurrentAnimationClipIndex != ClipIndex)
	{
		CurrentAnimationClipIndex = ClipIndex;
		AnimationTimeSeconds = 0.0f;
	}

	const FWoWCharacterPreviewAnimationClip& Clip = AnimationClips[ClipIndex];
	const int32 StrafeOverlayClipIndex = ResolveStrafeOverlayClipIndex();
	const FWoWCharacterPreviewAnimationClip* StrafeOverlayClip = AnimationClips.IsValidIndex(StrafeOverlayClipIndex) ? &AnimationClips[StrafeOverlayClipIndex] : nullptr;
	const FWoWCharacterPreviewAnimationClip* LandingOverlayClip = AnimationClips.IsValidIndex(MovingLandingOverlayClipIndex) ? &AnimationClips[MovingLandingOverlayClipIndex] : nullptr;
	const float LandingOverlayFade = MovingLandingOverlayRemainingSeconds > 0.0f
		? FMath::Clamp(MovingLandingOverlayRemainingSeconds / FMath::Max(MovingJumpLandOverlayDurationSeconds, KINDA_SMALL_NUMBER), 0.0f, 1.0f)
		: 0.0f;
	const float LandingOverlayAlpha = LandingOverlayClip
		? MovingJumpLandOverlayMaxAlpha * LandingOverlayFade * LandingOverlayFade * (3.0f - 2.0f * LandingOverlayFade)
		: 0.0f;
	const int32 ReferenceStandClipIndex = ResolveReferenceStandClipIndex();
	const FWoWCharacterPreviewAnimationClip* ReferenceStandClip = AnimationClips.IsValidIndex(ReferenceStandClipIndex) ? &AnimationClips[ReferenceStandClipIndex] : nullptr;
	const int32 WeaponGripClipIndex = AttachedWeaponMeshComponent.IsValid() && !IsWeaponVisuallySheathed() ? FindAnimationClipIndex(15, 0) : INDEX_NONE;
	const FWoWCharacterPreviewAnimationClip* WeaponGripClip = AnimationClips.IsValidIndex(WeaponGripClipIndex) ? &AnimationClips[WeaponGripClipIndex] : nullptr;
	if (CurrentStrafeOverlayClipIndex != StrafeOverlayClipIndex)
	{
		CurrentStrafeOverlayClipIndex = StrafeOverlayClipIndex;
		StrafeOverlayTimeSeconds = 0.0f;
	}
	if (LastLoggedAnimationClipIndex != ClipIndex)
	{
		LastLoggedAnimationClipIndex = ClipIndex;
		UE_LOG(LogTemp, Display, TEXT("WoW preview animation: clip=%d sequence=%d anim=%d sub=%d overlay=%d lookups=%s"),
			ClipIndex,
			Clip.SequenceIndex,
			Clip.AnimationId,
			Clip.SubAnimationId,
			StrafeOverlayClipIndex,
			*FString::JoinBy(Clip.LookupIds, TEXT(","), [](int32 LookupId) { return FString::FromInt(LookupId); }));
	}
	if (Clip.Frames.Num() == 0 || Clip.SampleRate <= 0.0f)
	{
		return;
	}

	const bool bOneShotActive = OneShotAnimationClipIndex != INDEX_NONE;
	const float PlaybackRate = bOneShotActive ? OneShotPlaybackRate : GetAnimationPlaybackRate(Clip);
	AnimationTimeSeconds += DeltaTime * PlaybackRate;
	UpdateGeneratedSkeletalPreviewAnimation(ClipIndex, PlaybackRate, !bOneShotActive);
	int32 FrameIndex = 0;
	int32 NextFrameIndex = 0;
	float FrameAlpha = 0.0f;
	if (bOneShotActive)
	{
		ResolveOneShotClipFrame(Clip, AnimationTimeSeconds, FrameIndex, NextFrameIndex, FrameAlpha);
	}
	else
	{
		ResolveLoopingClipFrame(Clip, AnimationTimeSeconds, FrameIndex, NextFrameIndex, FrameAlpha);
	}
	UpdateCurrentBoneMatrices(Clip, FrameIndex, NextFrameIndex, FrameAlpha);

	int32 OverlayFrameIndex = 0;
	int32 OverlayNextFrameIndex = 0;
	float OverlayFrameAlpha = 0.0f;
	if (StrafeOverlayClip && StrafeOverlayClip->Frames.Num() > 0 && StrafeOverlayClip->SampleRate > 0.0f)
	{
		StrafeOverlayTimeSeconds += DeltaTime * GetAnimationPlaybackRate(*StrafeOverlayClip);
		ResolveLoopingClipFrame(*StrafeOverlayClip, StrafeOverlayTimeSeconds, OverlayFrameIndex, OverlayNextFrameIndex, OverlayFrameAlpha);
	}

	const bool bUpdateLegacyBody = PreviewMesh &&
		SectionStates.Num() > 0 &&
		(!bGeneratedPrimaryVisual || CVarWoWCharacterPreviewLegacyBodyTick.GetValueOnGameThread() != 0);
	if (bUpdateLegacyBody)
	{
		for (int32 SectionIndex = 0; SectionIndex < SectionStates.Num(); ++SectionIndex)
		{
			FWoWCharacterPreviewSectionState& SectionState = SectionStates[SectionIndex];
			SkinSection(Clip, StrafeOverlayClip, ReferenceStandClip, FrameIndex, NextFrameIndex, FrameAlpha, OverlayFrameIndex, OverlayNextFrameIndex, OverlayFrameAlpha, LandingOverlayClip, MovingLandingOverlayTimeSeconds, LandingOverlayAlpha, WeaponGripClip, SectionState);
			ApplySectionColorSamples(SectionState.ColorSamples, AnimationTimeSeconds, SectionState.bScaleLowAlphaAdditive, SectionState.VertexColors);
			PreviewMesh->UpdateMeshSection_LinearColor(
				SectionIndex,
				SectionState.SkinnedPositions,
				SectionState.Normals,
				SectionState.UV0,
				SectionState.VertexColors,
				SectionState.Tangents);
		}
	}
	UpdateAttachedWeaponTransform(LandingOverlayClip, MovingLandingOverlayTimeSeconds, LandingOverlayAlpha);
}

void UWoWCharacterPreviewComponent::UpdateGeneratedSkeletalPreviewAnimation(int32 ClipIndex, float PlaybackRate, bool bLooping)
{
	USkeletalMeshComponent* SkeletalPreview = GeneratedSkeletalMeshComponent.Get();
	if (!SkeletalPreview || !AnimationClips.IsValidIndex(ClipIndex))
	{
		return;
	}

	const FWoWCharacterPreviewAnimationClip& Clip = AnimationClips[ClipIndex];
	int32 DesiredGripVariant = GeneratedGripVariantNone;
	const bool bBaseVisualWeaponSheathed = bWeaponSheatheTransitionActive
		? bWeaponSheatheTransitionStartVisualSheathed
		: IsWeaponVisuallySheathed();
	if (bPreviewEquipmentVisible && AttachedWeaponMeshComponent.IsValid() && !bBaseVisualWeaponSheathed)
	{
		DesiredGripVariant = ActiveWeaponAttachmentId == LeftPalmAttachmentId
			? GeneratedGripVariantLeft
			: GeneratedGripVariantRight;
	}
	UAnimSequence* AnimSequence = LoadGeneratedCharacterAnimation(Clip.AnimationId, Clip.SubAnimationId, DesiredGripVariant);
	if (!AnimSequence && DesiredGripVariant != GeneratedGripVariantNone)
	{
		AnimSequence = LoadGeneratedCharacterAnimation(Clip.AnimationId, Clip.SubAnimationId);
		DesiredGripVariant = GeneratedGripVariantNone;
	}
	if (!AnimSequence)
	{
		return;
	}

	const float SequenceLength = FMath::Max(AnimSequence->GetPlayLength(), KINDA_SMALL_NUMBER);
	const float DesiredPosition = bLooping
		? FMath::Fmod(FMath::Max(AnimationTimeSeconds, 0.0f), SequenceLength)
		: FMath::Clamp(AnimationTimeSeconds, 0.0f, SequenceLength);

	UAnimSequence* UpperBodySequence = nullptr;
	float UpperBodyTimeSeconds = 0.0f;
	float UpperBodyWeight = 0.0f;
	if (bWeaponSheatheTransitionActive && WeaponSheatheTransitionAnimationId != INDEX_NONE)
	{
		UpperBodySequence = LoadGeneratedCharacterAnimation(WeaponSheatheTransitionAnimationId, 0);
		if (UpperBodySequence)
		{
			const float ClipSeconds = WeaponSheatheTransitionClipSeconds > 0.0f
				? WeaponSheatheTransitionClipSeconds
				: UpperBodySequence->GetPlayLength();
			const float BlendSeconds = WeaponSheatheTransitionBlendOutSeconds > 0.0f
				? WeaponSheatheTransitionBlendOutSeconds
				: WeaponSheatheOverlayDefaultBlendSeconds;
			UpperBodyTimeSeconds = FMath::Clamp(WeaponSheatheTransitionElapsedSeconds, 0.0f, ClipSeconds);
			const float BlendIn = BlendSeconds > 0.0f
				? FMath::Clamp(WeaponSheatheTransitionElapsedSeconds / BlendSeconds, 0.0f, 1.0f)
				: 1.0f;
			const float BlendOut = WeaponSheatheTransitionElapsedSeconds > ClipSeconds && BlendSeconds > 0.0f
				? FMath::Clamp((WeaponSheatheTransitionDurationSeconds - WeaponSheatheTransitionElapsedSeconds) / BlendSeconds, 0.0f, 1.0f)
				: 1.0f;
			const float LinearWeight = FMath::Min(BlendIn, BlendOut);
			UpperBodyWeight = LinearWeight * LinearWeight * (3.0f - 2.0f * LinearWeight);
		}
	}

	if (!SkeletalPreview->GetAnimInstance() || !SkeletalPreview->GetAnimInstance()->IsA<UWoWM2PreviewAnimInstance>())
	{
		SkeletalPreview->SetAnimInstanceClass(UWoWM2PreviewAnimInstance::StaticClass());
	}
	if (UWoWM2PreviewAnimInstance* PreviewAnimInstance = Cast<UWoWM2PreviewAnimInstance>(SkeletalPreview->GetAnimInstance()))
	{
		PreviewAnimInstance->ConfigureM2PreviewPose(
			AnimSequence,
			DesiredPosition,
			bLooping,
			UpperBodySequence,
			UpperBodyTimeSeconds,
			UpperBodyWeight,
			WeaponSheatheUpperBodyBoneIndices);
		SkeletalPreview->TickAnimation(0.0f, false);
		SkeletalPreview->RefreshBoneTransforms();
	}

	GeneratedSkeletalAnimationId = Clip.AnimationId;
	GeneratedSkeletalSubAnimationId = Clip.SubAnimationId;
	GeneratedSkeletalGripVariant = DesiredGripVariant;
	GeneratedSkeletalAnimationTimeSeconds = AnimationTimeSeconds;
	(void)PlaybackRate;
}

void UWoWCharacterPreviewComponent::UpdateCurrentBoneMatrices(const FWoWCharacterPreviewAnimationClip& Clip, int32 FrameIndex, int32 NextFrameIndex, float FrameAlpha)
{
	CurrentBoneMatrices.Reset();
	if (!Clip.Frames.IsValidIndex(FrameIndex) || !Clip.Frames.IsValidIndex(NextFrameIndex))
	{
		return;
	}

	const TArray<FMatrix>& BoneMatrices = Clip.Frames[FrameIndex];
	const TArray<FMatrix>& NextBoneMatrices = Clip.Frames[NextFrameIndex];
	const int32 BoneCount = FMath::Min(BoneMatrices.Num(), NextBoneMatrices.Num());
	CurrentBoneMatrices.SetNum(BoneCount);
	for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
	{
		FMatrix Interpolated = FMatrix::Identity;
		for (int32 Row = 0; Row < 4; ++Row)
		{
			for (int32 Column = 0; Column < 4; ++Column)
			{
				Interpolated.M[Row][Column] = FMath::Lerp(BoneMatrices[BoneIndex].M[Row][Column], NextBoneMatrices[BoneIndex].M[Row][Column], FrameAlpha);
			}
		}
		CurrentBoneMatrices[BoneIndex] = Interpolated;
	}
}

void UWoWCharacterPreviewComponent::UpdateAttachedWeaponTransform()
{
	UpdateAttachedWeaponTransform(nullptr, 0.0f, 0.0f);
}

FName UWoWCharacterPreviewComponent::MakeGeneratedAttachmentSocketName(const FWoWCharacterAttachment& Attachment) const
{
	return FName(*FString::Printf(TEXT("ATT_%03d_ID_%03d"), Attachment.Index, Attachment.Id));
}

bool UWoWCharacterPreviewComponent::TryUpdateAttachedWeaponTransformFromGeneratedSkeleton(const FWoWCharacterAttachment& Attachment)
{
	UProceduralMeshComponent* WeaponMesh = AttachedWeaponMeshComponent.Get();
	USkeletalMeshComponent* SkeletalPreview = GeneratedSkeletalMeshComponent.Get();
	if (!WeaponMesh || !SkeletalPreview || !bUseGeneratedSkeletalPreviewAsPrimaryVisual)
	{
		return false;
	}

	const FName SocketName = MakeGeneratedAttachmentSocketName(Attachment);
	if (!SkeletalPreview->DoesSocketExist(SocketName))
	{
		return false;
	}

	const FTransform SocketWorldTransform = SkeletalPreview->GetSocketTransform(SocketName, RTS_World);
	FTransform WeaponWorldTransform = SocketWorldTransform;
	if (IsWeaponVisuallySheathed())
	{
		if (IsDebugSheathedTransformActive())
		{
			const FRotator SheathedRotation = DebugSheathedRotations[FMath::Clamp(ActiveWeaponSheathedRotationIndex, 0, UE_ARRAY_COUNT(DebugSheathedRotations) - 1)];
			WeaponWorldTransform.SetLocation(SocketWorldTransform.TransformPosition(DebugSheathedBackWeaponLocationOffset));
			WeaponWorldTransform.SetRotation((SocketWorldTransform.GetRotation() * SheathedRotation.Quaternion() * DebugSheathedBackWeaponRotationOffset.Quaternion()).GetNormalized());
		}
	}
	else
	{
		WeaponWorldTransform.SetLocation(SocketWorldTransform.TransformPosition(AttachedWeaponLocationOffset));
		WeaponWorldTransform.SetRotation((SocketWorldTransform.GetRotation() * AttachedWeaponRotationOffset.Quaternion()).GetNormalized());
	}

	const USceneComponent* AttachParent = WeaponMesh->GetAttachParent();
	const FTransform ParentWorldTransform = AttachParent ? AttachParent->GetComponentTransform() : FTransform::Identity;
	const FTransform WeaponRelativeTransform = WeaponWorldTransform.GetRelativeTransform(ParentWorldTransform);
	WeaponMesh->SetRelativeLocationAndRotation(WeaponRelativeTransform.GetLocation(), WeaponRelativeTransform.GetRotation().Rotator());
	return true;
}

void UWoWCharacterPreviewComponent::UpdateAttachedWeaponTransform(
	const FWoWCharacterPreviewAnimationClip* LandingOverlayClip,
	float LandingOverlayTimeSeconds,
	float LandingOverlayAlpha)
{
	UProceduralMeshComponent* WeaponMesh = AttachedWeaponMeshComponent.Get();
	if (!WeaponMesh || !bPreviewEquipmentVisible)
	{
		return;
	}

	FWoWCharacterAttachment Attachment;
	const int32 VisualAttachmentId = ResolveVisualWeaponAttachmentId();
	if (!ResolveAttachmentById(VisualAttachmentId, Attachment))
	{
		return;
	}

	if (TryUpdateAttachedWeaponTransformFromGeneratedSkeleton(Attachment))
	{
		return;
	}

	if (!CurrentBoneMatrices.IsValidIndex(Attachment.BoneIndex))
	{
		return;
	}

	// 武器挂点必须跟随角色最终视觉姿态，而不是只读基础骨骼矩阵。
	// 跑动落地时角色顶点会叠加 JumpLandRun 覆盖层；这里用同一套采样算挂点三点，避免手和武器分离。
	constexpr float AttachmentAxisSampleLength = 10.0f;
	const FVector BoneLocation = SampleAttachmentPointFromCurrentPose(Attachment, Attachment.Position, LandingOverlayClip, LandingOverlayTimeSeconds, LandingOverlayAlpha);
	const FVector BoneForwardPoint = SampleAttachmentPointFromCurrentPose(Attachment, Attachment.Position + FVector::ForwardVector * AttachmentAxisSampleLength, LandingOverlayClip, LandingOverlayTimeSeconds, LandingOverlayAlpha);
	const FVector BoneUpPoint = SampleAttachmentPointFromCurrentPose(Attachment, Attachment.Position + FVector::UpVector * AttachmentAxisSampleLength, LandingOverlayClip, LandingOverlayTimeSeconds, LandingOverlayAlpha);
	const FVector BoneXAxis = (BoneForwardPoint - BoneLocation).GetSafeNormal();
	const FVector BoneZAxis = (BoneUpPoint - BoneLocation).GetSafeNormal();
	const FRotator BoneRotation = (!BoneXAxis.IsNearlyZero() && !BoneZAxis.IsNearlyZero())
		? FRotationMatrix::MakeFromXZ(BoneXAxis, BoneZAxis).Rotator()
		: FRotator::ZeroRotator;

	if (IsWeaponVisuallySheathed())
	{
		if (IsDebugSheathedTransformActive())
		{
			const FRotator SheathedRotation = DebugSheathedRotations[FMath::Clamp(ActiveWeaponSheathedRotationIndex, 0, UE_ARRAY_COUNT(DebugSheathedRotations) - 1)];
			const FQuat BoneQuat = BoneRotation.Quaternion();
			const FVector LocalOffset = BoneQuat.RotateVector(DebugSheathedBackWeaponLocationOffset);
			WeaponMesh->SetRelativeLocation(BoneLocation + LocalOffset);
			WeaponMesh->SetRelativeRotation((BoneQuat * SheathedRotation.Quaternion() * DebugSheathedBackWeaponRotationOffset.Quaternion()).Rotator());
		}
		else
		{
			WeaponMesh->SetRelativeLocation(BoneLocation);
			WeaponMesh->SetRelativeRotation(BoneRotation);
		}
	}
	else
	{
		WeaponMesh->SetRelativeLocation(BoneLocation + AttachedWeaponLocationOffset);
		WeaponMesh->SetRelativeRotation((BoneRotation.Quaternion() * AttachedWeaponRotationOffset.Quaternion()).Rotator());
	}
}

FVector UWoWCharacterPreviewComponent::SampleAttachmentPointFromCurrentPose(
	const FWoWCharacterAttachment& Attachment,
	const FVector& LocalPoint,
	const FWoWCharacterPreviewAnimationClip* LandingOverlayClip,
	float LandingOverlayTimeSeconds,
	float LandingOverlayAlpha) const
{
	if (!CurrentBoneMatrices.IsValidIndex(Attachment.BoneIndex))
	{
		return LocalPoint;
	}

	FVector Result = TransformPointByExportMatrix(CurrentBoneMatrices[Attachment.BoneIndex], LocalPoint);
	if (LandingOverlayClip && LandingOverlayAlpha > 0.01f)
	{
		FMatrix OverlayBoneMatrix = FMatrix::Identity;
		if (SampleBoneMatrixAtTime(*LandingOverlayClip, LandingOverlayTimeSeconds, Attachment.BoneIndex, OverlayBoneMatrix))
		{
			const FVector OverlayPoint = TransformPointByExportMatrix(OverlayBoneMatrix, LocalPoint);
			Result = FMath::Lerp(Result, OverlayPoint, FMath::Clamp(LandingOverlayAlpha, 0.0f, 1.0f));
		}
	}
	const FIntVector4 Weights(255, 0, 0, 0);
	const FIntVector4 BoneIndices(Attachment.BoneIndex, 0, 0, 0);
	return ApplyLocomotionPoseToSkinnedPosition(Result, Weights, BoneIndices);
}

void UWoWCharacterPreviewComponent::UpdateAttachedM2Animation(float DeltaTime)
{
	if (AttachedM2ModelComponent.IsValid())
	{
		return;
	}

	UProceduralMeshComponent* Mesh = AttachedWeaponMeshComponent.Get();
	if (!Mesh || AttachedM2SectionStates.Num() == 0 || AttachedM2AnimationClips.Num() == 0)
	{
		return;
	}

	const int32 ClipIndex = ResolveAttachedM2AnimationClipIndex();
	if (!AttachedM2AnimationClips.IsValidIndex(ClipIndex))
	{
		return;
	}

	if (AttachedM2CurrentAnimationClipIndex != ClipIndex)
	{
		AttachedM2CurrentAnimationClipIndex = ClipIndex;
		AttachedM2AnimationTimeSeconds = 0.0f;
	}

	const FWoWCharacterPreviewAnimationClip& Clip = AttachedM2AnimationClips[ClipIndex];
	if (Clip.Frames.Num() == 0 || Clip.SampleRate <= 0.0f)
	{
		return;
	}

	// 附加M2先走通用默认循环：武器、物件、未来生物都按自身M2骨骼动画更新；状态选择以后再接装备/技能系统。
	AttachedM2AnimationTimeSeconds += DeltaTime * GetAnimationPlaybackRate(Clip);
	int32 FrameIndex = 0;
	int32 NextFrameIndex = 0;
	float FrameAlpha = 0.0f;
	ResolveLoopingClipFrame(Clip, AttachedM2AnimationTimeSeconds, FrameIndex, NextFrameIndex, FrameAlpha);

	AttachedM2CurrentBoneMatrices.Reset();
	if (Clip.Frames.IsValidIndex(FrameIndex) && Clip.Frames.IsValidIndex(NextFrameIndex))
	{
		const TArray<FMatrix>& BoneMatrices = Clip.Frames[FrameIndex];
		const TArray<FMatrix>& NextBoneMatrices = Clip.Frames[NextFrameIndex];
		const int32 BoneCount = FMath::Min(BoneMatrices.Num(), NextBoneMatrices.Num());
		AttachedM2CurrentBoneMatrices.SetNum(BoneCount);
		for (int32 BoneIndex = 0; BoneIndex < BoneCount; ++BoneIndex)
		{
			FMatrix Interpolated = FMatrix::Identity;
			for (int32 Row = 0; Row < 4; ++Row)
			{
				for (int32 Column = 0; Column < 4; ++Column)
				{
					Interpolated.M[Row][Column] = FMath::Lerp(BoneMatrices[BoneIndex].M[Row][Column], NextBoneMatrices[BoneIndex].M[Row][Column], FrameAlpha);
				}
			}
			AttachedM2CurrentBoneMatrices[BoneIndex] = Interpolated;
		}
	}

	ApplyAttachedM2BillboardBones(AttachedM2CurrentBoneMatrices);

	for (int32 SectionIndex = 0; SectionIndex < AttachedM2SectionStates.Num(); ++SectionIndex)
	{
		FWoWCharacterPreviewSectionState& SectionState = AttachedM2SectionStates[SectionIndex];
		const int32 VertexCount = SectionState.BasePositions.Num();
		if (SectionState.SkinnedPositions.Num() != VertexCount)
		{
			SectionState.SkinnedPositions.SetNum(VertexCount);
		}

		for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
		{
			const FVector BasePosition = SectionState.BasePositions[VertexIndex];
			const FIntVector4 Weights = SectionState.BoneWeights.IsValidIndex(VertexIndex) ? SectionState.BoneWeights[VertexIndex] : FIntVector4(255, 0, 0, 0);
			const FIntVector4 BoneIndices = SectionState.BoneIndices.IsValidIndex(VertexIndex) ? SectionState.BoneIndices[VertexIndex] : FIntVector4(0, 0, 0, 0);

			// 附加M2的骨骼矩阵已经在当前帧做过插值，并且已应用 M2 billboard bone(0x08)。
			// 这里必须直接使用 AttachedM2CurrentBoneMatrices；再去采样 Clip.Frames 会绕开 billboard，中心发光片就会固定成面片。
			FVector SkinnedPosition = FVector::ZeroVector;
			float TotalWeight = 0.0f;
			for (int32 InfluenceIndex = 0; InfluenceIndex < 4; ++InfluenceIndex)
			{
				const int32 WeightByte = GetIntVector4Component(Weights, InfluenceIndex);
				if (WeightByte <= 0)
				{
					continue;
				}

				const int32 BoneIndex = GetIntVector4Component(BoneIndices, InfluenceIndex);
				if (!AttachedM2CurrentBoneMatrices.IsValidIndex(BoneIndex))
				{
					continue;
				}

				const float Weight = static_cast<float>(WeightByte) / 255.0f;
				SkinnedPosition += TransformPointByExportMatrix(AttachedM2CurrentBoneMatrices[BoneIndex], BasePosition) * Weight;
				TotalWeight += Weight;
			}
			SectionState.SkinnedPositions[VertexIndex] = TotalWeight > KINDA_SMALL_NUMBER ? SkinnedPosition / TotalWeight : BasePosition;
		}
		// M2 TextureTransform 是贴图矩阵动画，不是骨骼动画；每帧从BaseUV重新计算，避免UV累积漂移。
		ApplyTextureTransformToUVs(SectionState.BaseUV0, SectionState.TextureTransformSamples, AttachedM2AnimationTimeSeconds, SectionState.UV0);
		ApplySectionColorSamples(SectionState.ColorSamples, AttachedM2AnimationTimeSeconds, SectionState.bScaleLowAlphaAdditive, SectionState.VertexColors);

		Mesh->UpdateMeshSection_LinearColor(
			SectionIndex,
			SectionState.SkinnedPositions,
			SectionState.Normals,
			SectionState.UV0,
			SectionState.VertexColors,
			SectionState.Tangents);
	}
}

int32 UWoWCharacterPreviewComponent::ResolveAttachedM2AnimationClipIndex() const
{
	for (int32 Index = 0; Index < AttachedM2AnimationClips.Num(); ++Index)
	{
		const FWoWCharacterPreviewAnimationClip& Clip = AttachedM2AnimationClips[Index];
		if (Clip.AnimationId == 0 || Clip.LookupIds.Contains(0))
		{
			return Index;
		}
	}

	return AttachedM2AnimationClips.Num() > 0 ? 0 : INDEX_NONE;
}

void UWoWCharacterPreviewComponent::ApplyAttachedM2BillboardBones(TArray<FMatrix>& InOutBoneMatrices) const
{
	const UProceduralMeshComponent* WeaponMesh = AttachedWeaponMeshComponent.Get();
	const UWorld* World = GetWorld();
	const APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
	const APlayerCameraManager* CameraManager = PlayerController ? PlayerController->PlayerCameraManager : nullptr;
	if (!WeaponMesh || !CameraManager || AttachedM2Bones.Num() == 0)
	{
		return;
	}

	const FRotationMatrix CameraRotation(CameraManager->GetCameraRotation());
	const FTransform WeaponToWorld = WeaponMesh->GetComponentTransform();
	const FVector CameraRightLocal = WeaponToWorld.InverseTransformVectorNoScale(CameraRotation.GetScaledAxis(EAxis::Y)).GetSafeNormal();
	const FVector CameraUpLocal = WeaponToWorld.InverseTransformVectorNoScale(CameraRotation.GetScaledAxis(EAxis::Z)).GetSafeNormal();
	if (CameraRightLocal.IsNearlyZero() || CameraUpLocal.IsNearlyZero())
	{
		return;
	}

	// M2 bone flag 0x08 是 spherical billboard。带球状发光层的物品会把平面顶点挂在这类骨骼上，
	// WoW/WMV 会用相机 Right/Up 替换骨骼朝向轴，让多层发光片始终叠成球状亮团，而不是露出固定面片。
	const FVector BillboardAxisZ = -CameraRightLocal;
	const FVector BillboardAxisY = CameraUpLocal;
	const FVector BillboardAxisX = FVector::CrossProduct(BillboardAxisY, BillboardAxisZ).GetSafeNormal();
	if (BillboardAxisX.IsNearlyZero())
	{
		return;
	}

	auto GetMatrixColumn = [](const FMatrix& Matrix, int32 Column) -> FVector
	{
		return FVector(Matrix.M[0][Column], Matrix.M[1][Column], Matrix.M[2][Column]);
	};

	auto SetMatrixColumn = [](FMatrix& Matrix, int32 Column, const FVector& Value)
	{
		Matrix.M[0][Column] = Value.X;
		Matrix.M[1][Column] = Value.Y;
		Matrix.M[2][Column] = Value.Z;
	};

	for (int32 BoneIndex = 0; BoneIndex < InOutBoneMatrices.Num(); ++BoneIndex)
	{
		if (!AttachedM2Bones.IsValidIndex(BoneIndex) || (AttachedM2Bones[BoneIndex].Flags & M2BoneFlagBillboard) == 0)
		{
			continue;
		}

		const FWoWBoneMetadata& Bone = AttachedM2Bones[BoneIndex];
		FMatrix& Matrix = InOutBoneMatrices[BoneIndex];
		const FVector PivotPosition = TransformPointByExportMatrix(Matrix, Bone.Pivot);
		const float ScaleX = FMath::Max(GetMatrixColumn(Matrix, 0).Length(), KINDA_SMALL_NUMBER);
		const float ScaleY = FMath::Max(GetMatrixColumn(Matrix, 1).Length(), KINDA_SMALL_NUMBER);
		const float ScaleZ = FMath::Max(GetMatrixColumn(Matrix, 2).Length(), KINDA_SMALL_NUMBER);
		const FVector AxisX = BillboardAxisX * ScaleX;
		const FVector AxisY = BillboardAxisY * ScaleY;
		const FVector AxisZ = BillboardAxisZ * ScaleZ;

		SetMatrixColumn(Matrix, 0, AxisX);
		SetMatrixColumn(Matrix, 1, AxisY);
		SetMatrixColumn(Matrix, 2, AxisZ);
		const FVector Translation = PivotPosition - AxisX * Bone.Pivot.X - AxisY * Bone.Pivot.Y - AxisZ * Bone.Pivot.Z;
		Matrix.M[0][3] = Translation.X;
		Matrix.M[1][3] = Translation.Y;
		Matrix.M[2][3] = Translation.Z;
	}
}

void UWoWCharacterPreviewComponent::UpdateAttachedM2Ribbons(float DeltaTime)
{
	UProceduralMeshComponent* RibbonMesh = AttachedRibbonMeshComponent.Get();
	if (!RibbonMesh || AttachedRibbonStates.Num() == 0 || AttachedM2CurrentBoneMatrices.Num() == 0)
	{
		return;
	}

	for (int32 RibbonIndex = 0; RibbonIndex < AttachedRibbonStates.Num(); ++RibbonIndex)
	{
		FWoWRibbonRuntimeState& State = AttachedRibbonStates[RibbonIndex];
		const FWoWRibbonEmitter& Emitter = State.Emitter;
		if (!AttachedM2CurrentBoneMatrices.IsValidIndex(Emitter.BoneIndex))
		{
			continue;
		}

		const FMatrix& BoneMatrix = AttachedM2CurrentBoneMatrices[Emitter.BoneIndex];
		const FVector WeaponLocalPosition = TransformPointByExportMatrix(BoneMatrix, Emitter.Position);
		const FVector WeaponLocalUpPosition = TransformPointByExportMatrix(BoneMatrix, Emitter.Position + FVector(0.0f, 0.0f, 1.0f));
		const UProceduralMeshComponent* WeaponMesh = AttachedWeaponMeshComponent.Get();
		if (!WeaponMesh)
		{
			continue;
		}

		// M2 Ribbon 发射点跟随武器骨骼，但拖尾历史必须记录为世界空间点，角色跑动时旧段才会真正留在身后。
		const FTransform WeaponToWorld = WeaponMesh->GetComponentTransform();
		const FTransform RibbonToWorld = RibbonMesh->GetComponentTransform();
		const FVector CurrentWorldPosition = WeaponToWorld.TransformPosition(WeaponLocalPosition);
		const FVector UpWorldPosition = WeaponToWorld.TransformPosition(WeaponLocalUpPosition);
		const FVector CurrentPosition = CurrentWorldPosition;
		FVector CurrentUp = (UpWorldPosition - CurrentWorldPosition).GetSafeNormal();
		if (CurrentUp.IsNearlyZero())
		{
			CurrentUp = FVector::UpVector;
		}

		if (!State.bHasLastPosition || State.Segments.Num() == 0)
		{
			State.Segments.Reset();
			FWoWRibbonSegment InitialSegment;
			InitialSegment.Position = CurrentPosition;
			InitialSegment.Up = CurrentUp;
			State.Segments.Add(InitialSegment);
			State.LastPosition = CurrentPosition;
			State.TimeSinceLastEdgeSeconds = 0.0f;
			State.bHasLastPosition = true;
		}

		const float MovementLength = FVector::Distance(CurrentPosition, State.LastPosition);
		for (FWoWRibbonSegment& Segment : State.Segments)
		{
			Segment.AgeSeconds += DeltaTime;
		}

		State.TimeSinceLastEdgeSeconds += DeltaTime;
		FWoWRibbonSegment& FirstSegment = State.Segments[0];
		const float EdgeIntervalSeconds = 1.0f / FMath::Max(Emitter.EdgesPerSecond, 1.0f);
		const bool bMovedEnoughForNewEdge = MovementLength >= RibbonMinEdgeDistanceWorldUnits;
		const int32 EdgeSpawnCount = bMovedEnoughForNewEdge
			? FMath::Clamp(FMath::FloorToInt(State.TimeSinceLastEdgeSeconds / EdgeIntervalSeconds), 0, 8)
			: 0;
		if (EdgeSpawnCount > 0)
		{
			FirstSegment.Back = (State.LastPosition - CurrentPosition).GetSafeNormal();
			FirstSegment.OriginalLength = FMath::Max(FirstSegment.Length, 1.0f);
			// EdgesPerSec 是每秒生成边数；帧率低于该频率时，一帧内要补齐多个边，不能只插一个。
			const FVector PreviousPosition = State.LastPosition;
			for (int32 EdgeIndex = 1; EdgeIndex <= EdgeSpawnCount; ++EdgeIndex)
			{
				const float Alpha = static_cast<float>(EdgeIndex) / static_cast<float>(EdgeSpawnCount);
				const FVector EdgePosition = FMath::Lerp(PreviousPosition, CurrentPosition, Alpha);
				FWoWRibbonSegment NewSegment;
				NewSegment.Position = EdgePosition;
				NewSegment.Up = CurrentUp;
				NewSegment.Length = FVector::Distance(EdgePosition, State.Segments[0].Position);
				State.Segments.Insert(NewSegment, 0);
			}
			State.TimeSinceLastEdgeSeconds = FMath::Fmod(State.TimeSinceLastEdgeSeconds, EdgeIntervalSeconds);
		}
		else
		{
			FirstSegment.Position = CurrentPosition;
			FirstSegment.Up = CurrentUp;
			FirstSegment.Length += MovementLength;
			if (!bMovedEnoughForNewEdge)
			{
				// 保留最多一个边间隔，避免静止很久后第一次移动时一次性补出一串重叠短段。
				State.TimeSinceLastEdgeSeconds = FMath::Min(State.TimeSinceLastEdgeSeconds, EdgeIntervalSeconds);
			}
		}

		// M2 Ribbon.length 是 EdgeLifeSpanInSec：旧边按时间寿命消失，跑动时不会把中心能量整团拖成长线。
		for (int32 SegmentIndex = State.Segments.Num() - 1; SegmentIndex >= 0; --SegmentIndex)
		{
			if (State.Segments[SegmentIndex].AgeSeconds <= Emitter.EdgeLifetimeSeconds)
			{
				break;
			}
			State.Segments.RemoveAt(SegmentIndex);
		}

		State.LastPosition = CurrentPosition;
		const FWoWRibbonSample RibbonSample = SampleRibbonEmitter(Emitter, AttachedM2AnimationTimeSeconds);
		const bool bRibbonVisible = SampleRibbonVisibility(Emitter, AttachedM2AnimationTimeSeconds);
		const int32 RibbonTexSlot = SampleRibbonTexSlot(Emitter, AttachedM2AnimationTimeSeconds);
		const int32 TextureRows = FMath::Max(Emitter.TextureRows, 1);
		const int32 TextureCols = FMath::Max(Emitter.TextureCols, 1);
		const int32 ClampedTexSlot = FMath::Clamp(RibbonTexSlot, 0, TextureRows * TextureCols - 1);
		const int32 TileColumn = ClampedTexSlot % TextureCols;
		const int32 TileRow = ClampedTexSlot / TextureCols;

		TArray<FVector> Positions;
		TArray<int32> Indices;
		TArray<FVector> Normals;
		TArray<FVector2D> UV0;
		TArray<FLinearColor> VertexColors;
		TArray<FProcMeshTangent> Tangents;
		const int32 SegmentCount = State.Segments.Num();
		if (bRibbonVisible && SegmentCount >= 2)
		{
			Positions.Reserve(SegmentCount * 2);
			Normals.Init(FVector::UpVector, SegmentCount * 2);
			UV0.Reserve(SegmentCount * 2);
			VertexColors.Init(RibbonSample.Color, SegmentCount * 2);
			Tangents.Init(FProcMeshTangent(1.0f, 0.0f, 0.0f), SegmentCount * 2);

			float RibbonLength = 0.0f;
			for (const FWoWRibbonSegment& Segment : State.Segments)
			{
				RibbonLength += Segment.Length;
			}
			float U = 0.0f;
			for (int32 SegmentIndex = 0; SegmentIndex < SegmentCount; ++SegmentIndex)
			{
				const FWoWRibbonSegment& Segment = State.Segments[SegmentIndex];
				const float UCoord = RibbonLength > KINDA_SMALL_NUMBER ? U / RibbonLength : 0.0f;
				Positions.Add(RibbonToWorld.InverseTransformPosition(Segment.Position + RibbonSample.Above * Segment.Up));
				Positions.Add(RibbonToWorld.InverseTransformPosition(Segment.Position - RibbonSample.Below * Segment.Up));
				// Ribbon 的 texSlotTrack + textureRows/textureCols 是 M2 自带的贴图格子选择，不能固定使用整张贴图第0格。
				UV0.Add(FVector2D((TileColumn + UCoord) / TextureCols, static_cast<float>(TileRow) / TextureRows));
				UV0.Add(FVector2D((TileColumn + UCoord) / TextureCols, static_cast<float>(TileRow + 1) / TextureRows));
				U += Segment.Length;
			}
			const FWoWRibbonSegment& LastSegment = State.Segments.Last();
			if (SegmentCount > 1 && LastSegment.OriginalLength > KINDA_SMALL_NUMBER)
			{
				const float TailOffset = LastSegment.Length / LastSegment.OriginalLength;
				const FVector TailPosition = LastSegment.Position + TailOffset * LastSegment.Back;
				Positions.Add(RibbonToWorld.InverseTransformPosition(TailPosition + RibbonSample.Above * LastSegment.Up));
				Positions.Add(RibbonToWorld.InverseTransformPosition(TailPosition - RibbonSample.Below * LastSegment.Up));
				UV0.Add(FVector2D(static_cast<float>(TileColumn + 1) / TextureCols, static_cast<float>(TileRow) / TextureRows));
				UV0.Add(FVector2D(static_cast<float>(TileColumn + 1) / TextureCols, static_cast<float>(TileRow + 1) / TextureRows));
				Normals.Add(FVector::UpVector);
				Normals.Add(FVector::UpVector);
				VertexColors.Add(RibbonSample.Color);
				VertexColors.Add(RibbonSample.Color);
				Tangents.Add(FProcMeshTangent(1.0f, 0.0f, 0.0f));
				Tangents.Add(FProcMeshTangent(1.0f, 0.0f, 0.0f));
			}
			const int32 QuadPairCount = Positions.Num() / 2;
			for (int32 SegmentIndex = 0; SegmentIndex < QuadPairCount - 1; ++SegmentIndex)
			{
				const int32 Base = SegmentIndex * 2;
				Indices.Append({ Base, Base + 2, Base + 1, Base + 1, Base + 2, Base + 3 });
			}
		}

		RibbonMesh->ClearMeshSection(RibbonIndex);
		if (Positions.Num() > 0 && Indices.Num() > 0)
		{
			RibbonMesh->CreateMeshSection_LinearColor(RibbonIndex, Positions, Indices, Normals, UV0, VertexColors, Tangents, false);
			if (State.Material)
			{
				RibbonMesh->SetMaterial(RibbonIndex, State.Material.Get());
			}
		}
	}
}

FWoWRibbonSample UWoWCharacterPreviewComponent::SampleRibbonEmitter(const FWoWRibbonEmitter& Emitter, float TimeSeconds) const
{
	if (Emitter.Samples.Num() == 0)
	{
		return FWoWRibbonSample();
	}

	const float WrappedTime = Emitter.LengthSeconds > KINDA_SMALL_NUMBER
		? FMath::Fmod(FMath::Max(TimeSeconds, 0.0f), Emitter.LengthSeconds)
		: 0.0f;
	for (int32 Index = 1; Index < Emitter.Samples.Num(); ++Index)
	{
		if (WrappedTime <= Emitter.Samples[Index].TimeSeconds)
		{
			const FWoWRibbonSample& Previous = Emitter.Samples[Index - 1];
			const FWoWRibbonSample& Next = Emitter.Samples[Index];
			const float Duration = FMath::Max(Next.TimeSeconds - Previous.TimeSeconds, KINDA_SMALL_NUMBER);
			const float Alpha = FMath::Clamp((WrappedTime - Previous.TimeSeconds) / Duration, 0.0f, 1.0f);
			FWoWRibbonSample Result;
			Result.TimeSeconds = WrappedTime;
			Result.Color = FMath::Lerp(Previous.Color, Next.Color, Alpha);
			Result.Above = FMath::Lerp(Previous.Above, Next.Above, Alpha);
			Result.Below = FMath::Lerp(Previous.Below, Next.Below, Alpha);
			return Result;
		}
	}

	return Emitter.Samples.Last();
}

int32 UWoWCharacterPreviewComponent::SampleRibbonTexSlot(const FWoWRibbonEmitter& Emitter, float TimeSeconds) const
{
	if (Emitter.TexSlotSamples.Num() == 0)
	{
		return 0;
	}

	const float WrappedTime = Emitter.LengthSeconds > KINDA_SMALL_NUMBER
		? FMath::Fmod(FMath::Max(TimeSeconds, 0.0f), Emitter.LengthSeconds)
		: 0.0f;
	for (int32 Index = 1; Index < Emitter.TexSlotSamples.Num(); ++Index)
	{
		if (WrappedTime <= Emitter.TexSlotSamples[Index].TimeSeconds)
		{
			return Emitter.TexSlotSamples[Index - 1].Slot;
		}
	}

	return Emitter.TexSlotSamples.Last().Slot;
}

bool UWoWCharacterPreviewComponent::SampleRibbonVisibility(const FWoWRibbonEmitter& Emitter, float TimeSeconds) const
{
	if (Emitter.VisibilitySamples.Num() == 0)
	{
		return true;
	}

	const float WrappedTime = Emitter.LengthSeconds > KINDA_SMALL_NUMBER
		? FMath::Fmod(FMath::Max(TimeSeconds, 0.0f), Emitter.LengthSeconds)
		: 0.0f;
	for (int32 Index = 1; Index < Emitter.VisibilitySamples.Num(); ++Index)
	{
		if (WrappedTime <= Emitter.VisibilitySamples[Index].TimeSeconds)
		{
			return Emitter.VisibilitySamples[Index - 1].bVisible;
		}
	}

	return Emitter.VisibilitySamples.Last().bVisible;
}

bool UWoWCharacterPreviewComponent::ResolveAttachmentById(int32 AttachmentId, FWoWCharacterAttachment& OutAttachment) const
{
	// attachment_lookup 的下标是 ATT_* 语义ID，值才是 attachments 数组索引；这和 KeyBoneLookup 是两套完全不同的数据。
	if (AttachmentLookup.IsValidIndex(AttachmentId))
	{
		const int32 AttachmentIndex = AttachmentLookup[AttachmentId];
		if (Attachments.IsValidIndex(AttachmentIndex))
		{
			OutAttachment = Attachments[AttachmentIndex];
			return true;
		}
	}

	for (const FWoWCharacterAttachment& Attachment : Attachments)
	{
		if (Attachment.Id == AttachmentId)
		{
			OutAttachment = Attachment;
			return true;
		}
	}
	return false;
}

int32 UWoWCharacterPreviewComponent::ResolveAnimationClipIndex() const
{
	if (DebugAnimationClipIndex != INDEX_NONE)
	{
		return DebugAnimationClipIndex;
	}
	if (OneShotAnimationClipIndex != INDEX_NONE)
	{
		return OneShotAnimationClipIndex;
	}
	if (bSitting)
	{
		const int32 SitIndex = FindAnimationClipIndex(SitGroundLoopAnimationId, 0);
		if (SitIndex != INDEX_NONE)
		{
			return SitIndex;
		}
	}
	if (const ACharacter* Character = PreviewCharacter.Get())
	{
		if (const UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
		{
			if (Movement->IsFalling())
			{
				const int32 JumpLoopIndex = FindAnimationClipIndex(38, 0);
				if (JumpLoopIndex != INDEX_NONE)
				{
					return JumpLoopIndex;
				}
			}
		}
	}
	if (bMoveBackwardHeld)
	{
		const int32 Index = FindAnimationClipIndex(13, 0);
		if (Index != INDEX_NONE)
		{
			return Index;
		}
	}
	if (ResolveStrafeDirection(bStrafeLeftHeld, bStrafeRightHeld, StrafeInputAmount) < 0)
	{
		const int32 Index = FindAnimationClipIndex(5, 0);
		if (Index != INDEX_NONE)
		{
			return Index;
		}
	}
	if (ResolveStrafeDirection(bStrafeLeftHeld, bStrafeRightHeld, StrafeInputAmount) > 0)
	{
		const int32 Index = FindAnimationClipIndex(5, 0);
		if (Index != INDEX_NONE)
		{
			return Index;
		}
	}
	const bool bMovingForward = bMoveForwardHeld || (bLeftMouseHeld && bRightMouseHeld) || ForwardInputAmount > 0.1f;
	if (bMovingForward)
	{
		const int32 Index = FindAnimationClipIndex(5, 0);
		if (Index != INDEX_NONE)
		{
			return Index;
		}
	}
	const int32 TurnShuffleDirection = ResolveKeyboardTurnShuffleDirection();
	if (TurnShuffleDirection < 0)
	{
		const int32 Index = FindAnimationClipIndex(11, 0);
		if (Index != INDEX_NONE)
		{
			return Index;
		}
	}
	if (TurnShuffleDirection > 0)
	{
		const int32 Index = FindAnimationClipIndex(12, 0);
		if (Index != INDEX_NONE)
		{
			return Index;
		}
	}
	if (bCombatReady || CombatReadyLingerSeconds > 0.0f)
	{
		const int32 ReadyUnarmedIndex = FindAnimationClipIndex(25, 0);
		if (ReadyUnarmedIndex != INDEX_NONE)
		{
			return ReadyUnarmedIndex;
		}
	}
	const int32 StandIndex = FindAnimationClipIndex(0, 0);
	return StandIndex != INDEX_NONE ? StandIndex : 0;
}

int32 UWoWCharacterPreviewComponent::ResolveStrafeOverlayClipIndex() const
{
	if (DebugAnimationClipIndex != INDEX_NONE || OneShotAnimationClipIndex != INDEX_NONE || bMoveBackwardHeld)
	{
		return INDEX_NONE;
	}

	if (StrafeOverlayAmount <= 0.01f)
	{
		return INDEX_NONE;
	}

	if (StrafeReleaseShuffleDirection < 0)
	{
		return FindAnimationClipIndex(11, 0);
	}

	if (StrafeReleaseShuffleDirection > 0)
	{
		return FindAnimationClipIndex(12, 0);
	}

	return INDEX_NONE;
}

int32 UWoWCharacterPreviewComponent::ResolveKeyboardTurnShuffleDirection() const
{
	if (bRightMouseHeld ||
		bMoveForwardHeld ||
		bMoveBackwardHeld ||
		bStrafeLeftHeld ||
		bStrafeRightHeld ||
		FMath::Abs(ForwardInputAmount) > 0.1f ||
		FMath::Abs(StrafeInputAmount) > 0.1f ||
		bTurnLeftHeld == bTurnRightHeld)
	{
		return 0;
	}

	if (const ACharacter* Character = PreviewCharacter.Get())
	{
		if (const UCharacterMovementComponent* Movement = Character->GetCharacterMovement())
		{
			if (Movement->IsFalling())
			{
				return 0;
			}
		}
	}

	// A/D原地转身使用M2的ShuffleLeft/ShuffleRight作为主动画；空中只旋转角色朝向，不叠加脚步。
	return bTurnLeftHeld ? -1 : 1;
}

int32 UWoWCharacterPreviewComponent::ResolveReferenceStandClipIndex() const
{
	const int32 StandIndex = FindAnimationClipIndex(0, 0);
	return StandIndex != INDEX_NONE ? StandIndex : 0;
}

int32 UWoWCharacterPreviewComponent::FindAnimationClipIndex(int32 AnimationId, int32 SubAnimationId) const
{
	if (AnimationLookup.IsValidIndex(AnimationId))
	{
		const int32 SequenceIndex = AnimationLookup[AnimationId];
		if (SequenceIndex >= 0)
		{
			for (int32 Index = 0; Index < AnimationClips.Num(); ++Index)
			{
				const FWoWCharacterPreviewAnimationClip& Clip = AnimationClips[Index];
				if (Clip.SequenceIndex == SequenceIndex && (SubAnimationId == 0 || Clip.SubAnimationId == SubAnimationId))
				{
					return Index;
				}
			}
		}
	}

	for (int32 Index = 0; Index < AnimationClips.Num(); ++Index)
	{
		const FWoWCharacterPreviewAnimationClip& Clip = AnimationClips[Index];
		if ((Clip.AnimationId == AnimationId || Clip.LookupIds.Contains(AnimationId)) && Clip.SubAnimationId == SubAnimationId)
		{
			return Index;
		}
	}
	for (int32 Index = 0; Index < AnimationClips.Num(); ++Index)
	{
		if (AnimationClips[Index].AnimationId == AnimationId || AnimationClips[Index].LookupIds.Contains(AnimationId))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

bool UWoWCharacterPreviewComponent::IsClipForAnimationId(const FWoWCharacterPreviewAnimationClip& Clip, int32 AnimationId) const
{
	return Clip.AnimationId == AnimationId || Clip.LookupIds.Contains(AnimationId);
}

bool UWoWCharacterPreviewComponent::HasActiveMovementInput() const
{
	return bMoveForwardHeld ||
		bMoveBackwardHeld ||
		bStrafeLeftHeld ||
		bStrafeRightHeld ||
		(bLeftMouseHeld && bRightMouseHeld) ||
		FMath::Abs(ForwardInputAmount) > 0.1f ||
		FMath::Abs(StrafeInputAmount) > 0.1f;
}

void UWoWCharacterPreviewComponent::StartMovingLandingOverlay(float InitialTimeSeconds)
{
	const int32 ClipIndex = FindAnimationClipIndex(MovingJumpLandRunAnimationId, 0);
	if (!AnimationClips.IsValidIndex(ClipIndex))
	{
		return;
	}

	const FWoWCharacterPreviewAnimationClip& Clip = AnimationClips[ClipIndex];
	const float ClipDurationSeconds = Clip.LengthSeconds > 0.0f
		? Clip.LengthSeconds
		: static_cast<float>(Clip.Frames.Num()) / FMath::Max(Clip.SampleRate, 1.0f);

	MovingLandingOverlayClipIndex = ClipIndex;
	MovingLandingOverlayTimeSeconds = FMath::Clamp(
		FMath::Max(InitialTimeSeconds, MovingJumpLandOverlayStartTimeSeconds),
		0.0f,
		FMath::Max(ClipDurationSeconds - KINDA_SMALL_NUMBER, 0.0f));
	MovingLandingOverlayRemainingSeconds = MovingJumpLandOverlayDurationSeconds;
}

void UWoWCharacterPreviewComponent::StartOneShotAnimation(int32 AnimationId, int32 SubAnimationId, float MaxDurationSeconds)
{
	const int32 ClipIndex = FindAnimationClipIndex(AnimationId, SubAnimationId);
	if (!AnimationClips.IsValidIndex(ClipIndex))
	{
		return;
	}
	const FWoWCharacterPreviewAnimationClip& Clip = AnimationClips[ClipIndex];
	const float RawDurationSeconds = Clip.LengthSeconds > 0.0f
		? Clip.LengthSeconds
		: static_cast<float>(Clip.Frames.Num()) / FMath::Max(Clip.SampleRate, 1.0f);
	const float TargetDurationSeconds = MaxDurationSeconds > 0.0f
		? FMath::Min(RawDurationSeconds, MaxDurationSeconds)
		: RawDurationSeconds;
	OneShotAnimationClipIndex = ClipIndex;
	OneShotRemainingSeconds = TargetDurationSeconds;
	OneShotPlaybackRate = TargetDurationSeconds > KINDA_SMALL_NUMBER ? RawDurationSeconds / TargetDurationSeconds : 1.0f;
	AnimationTimeSeconds = 0.0f;
}

float UWoWCharacterPreviewComponent::GetAnimationPlaybackRate(const FWoWCharacterPreviewAnimationClip& Clip) const
{
	if (bMoveBackwardHeld && IsClipForAnimationId(Clip, 13))
	{
		return BackpedalAnimationPlaybackRate;
	}

	return 1.0f;
}

void UWoWCharacterPreviewComponent::UpdateVisualMovementFacing(float DeltaTime)
{
	float TargetYawOffset = 0.0f;
	float TargetPoseAmount = 0.0f;
	const int32 CurrentStrafeDirection = ResolveStrafeDirection(bStrafeLeftHeld, bStrafeRightHeld, StrafeInputAmount);
	const bool bDirectionalStrafe = CurrentStrafeDirection != 0 && !bMoveBackwardHeld;
	if (bDirectionalStrafe)
	{
		const float ForwardForDirection = FMath::Max(ForwardInputAmount, 0.01f);
		const float DesiredYaw = FMath::RadiansToDegrees(FMath::Atan2(StrafeInputAmount, ForwardForDirection));
		TargetYawOffset = FMath::Clamp(DesiredYaw, -StrafeVisualYawDegrees, StrafeVisualYawDegrees);
		TargetPoseAmount = bEnableStrafePoseLayer
			? FMath::Clamp(TargetYawOffset / FMath::Max(StrafeUpperBodyCounterYawDegrees, 1.0f), -1.0f, 1.0f)
			: 0.0f;
	}

	if (StrafeReleaseShuffleRemainingSeconds > 0.0f)
	{
		StrafeReleaseShuffleRemainingSeconds = FMath::Max(StrafeReleaseShuffleRemainingSeconds - DeltaTime, 0.0f);
		if (StrafeReleaseShuffleRemainingSeconds <= 0.0f)
		{
			StrafeReleaseShuffleDirection = 0;
		}
	}

	VisualYawOffsetDegrees = FMath::FInterpTo(VisualYawOffsetDegrees, TargetYawOffset, DeltaTime, StrafeVisualYawInterpSpeed);
	StrafePoseAmount = FMath::FInterpTo(StrafePoseAmount, TargetPoseAmount, DeltaTime, StrafePoseInterpSpeed);
	const float ReleaseFade = StrafeReleaseShuffleRemainingSeconds > 0.0f
		? FMath::Clamp(StrafeReleaseShuffleRemainingSeconds / FMath::Max(StrafeReleaseShuffleBlendOutSeconds, KINDA_SMALL_NUMBER), 0.0f, 1.0f)
		: 0.0f;
	const float TargetOverlayAmount = CurrentStrafeDirection == 0 && !bMoveBackwardHeld ? ReleaseFade : 0.0f;
	StrafeOverlayAmount = FMath::FInterpTo(StrafeOverlayAmount, TargetOverlayAmount, DeltaTime, StrafeOverlayInterpSpeed);
	if (UProceduralMeshComponent* PreviewMesh = PreviewMeshComponent.Get())
	{
		PreviewMesh->SetRelativeRotation(FRotator(0.0f, VisualYawOffsetDegrees, 0.0f));
	}
}

void UWoWCharacterPreviewComponent::RebuildLocomotionPoseLayers()
{
	LocomotionTorsoBoneIndices.Reset();
	LocomotionHeadBoneIndices.Reset();

	if (KeyBoneLookup.Num() == 0 || BoneParents.Num() == 0)
	{
		return;
	}

	// M2 key-bone lookup provides semantic anchors, so this stays model-generic for humanoids.
	const int32 TorsoKeyBones[] = {
		4, // SpineLow / stomach
		5, // Waist
		2, // ShoulderL
		3, // ShoulderR
	};
	for (const int32 KeyBoneIndex : TorsoKeyBones)
	{
		if (KeyBoneLookup.IsValidIndex(KeyBoneIndex) && KeyBoneLookup[KeyBoneIndex] >= 0)
		{
			AddBoneSubtreeToSet(KeyBoneLookup[KeyBoneIndex], LocomotionTorsoBoneIndices);
		}
	}

	const int32 HeadKeyBones[] = {
		6, // Head
		7, // Jaw
		18, // BTH
	};
	for (const int32 KeyBoneIndex : HeadKeyBones)
	{
		if (KeyBoneLookup.IsValidIndex(KeyBoneIndex) && KeyBoneLookup[KeyBoneIndex] >= 0)
		{
			AddBoneSubtreeToSet(KeyBoneLookup[KeyBoneIndex], LocomotionHeadBoneIndices);
		}
	}

	for (const int32 HeadBoneIndex : LocomotionHeadBoneIndices)
	{
		LocomotionTorsoBoneIndices.Remove(HeadBoneIndex);
	}

	const int32 LowerBodyKeyBones[] = {
		51, // LowerBodyParent
		55, // LegR
		56, // LegL
		57, // KneeR
		58, // KneeL
		59, // FootL
		60, // FootR
	};
	for (const int32 KeyBoneIndex : LowerBodyKeyBones)
	{
		if (KeyBoneLookup.IsValidIndex(KeyBoneIndex) && KeyBoneLookup[KeyBoneIndex] >= 0)
		{
			AddBoneSubtreeToSet(KeyBoneLookup[KeyBoneIndex], LocomotionLowerBodyBoneIndices);
		}
	}

	UE_LOG(LogTemp, Display, TEXT("Built WoW locomotion pose layers: torso_bones=%d head_bones=%d lower_bones=%d key_bones=%d"),
		LocomotionTorsoBoneIndices.Num(),
		LocomotionHeadBoneIndices.Num(),
		LocomotionLowerBodyBoneIndices.Num(),
		KeyBoneLookup.Num());
}

void UWoWCharacterPreviewComponent::RebuildWeaponGripPoseLayer()
{
	WeaponGripBoneIndices.Reset();
	if (!bPreviewEquipmentVisible || KeyBoneLookup.Num() == 0 || BoneParents.Num() == 0)
	{
		return;
	}

	// key_bone_lookup 提供左右手指子树；这里按当前调试装备槽只闭合持武器的那只手。
	const int32 FirstGripKeyBone = ActiveWeaponAttachmentId == LeftPalmAttachmentId ? 13 : 8;
	const int32 LastGripKeyBone = ActiveWeaponAttachmentId == LeftPalmAttachmentId ? 17 : 12;
	for (int32 KeyBoneIndex = FirstGripKeyBone; KeyBoneIndex <= LastGripKeyBone; ++KeyBoneIndex)
	{
		if (KeyBoneLookup.IsValidIndex(KeyBoneIndex) && KeyBoneLookup[KeyBoneIndex] >= 0)
		{
			AddBoneSubtreeToSet(KeyBoneLookup[KeyBoneIndex], WeaponGripBoneIndices);
		}
	}

	UE_LOG(LogTemp, Display, TEXT("Built WoW weapon grip pose layer: grip_bones=%d"), WeaponGripBoneIndices.Num());
}

void UWoWCharacterPreviewComponent::RebuildWeaponSheatheUpperBodyPoseLayer()
{
	WeaponSheatheUpperBodyBoneIndices.Reset();
	if (KeyBoneLookup.Num() == 0 || BoneParents.Num() == 0)
	{
		return;
	}

	TSet<int32> BoneSet;
	const int32 UpperBodyKeyBones[] = {
		2, // ShoulderL
		3, // ShoulderR
	};
	for (const int32 KeyBoneIndex : UpperBodyKeyBones)
	{
		if (KeyBoneLookup.IsValidIndex(KeyBoneIndex) && KeyBoneLookup[KeyBoneIndex] >= 0)
		{
			AddBoneSubtreeToSet(KeyBoneLookup[KeyBoneIndex], BoneSet);
		}
	}

	// If key-bone data is absent on a future model, fall back to the event bones used by $SHL/$SHR.
	if (BoneSet.Num() == 0)
	{
		for (const FWoWM2EventRecord& Event : M2Events)
		{
			if ((Event.Identifier == TEXT("$SHL") || Event.Identifier == TEXT("$SHR")) && Event.BoneIndex >= 0)
			{
				AddBoneSubtreeToSet(Event.BoneIndex, BoneSet);
			}
		}
	}

	WeaponSheatheUpperBodyBoneIndices = BoneSet.Array();
	WeaponSheatheUpperBodyBoneIndices.Sort();
	UE_LOG(LogTemp, Display, TEXT("Built WoW sheathe upper-body layer: bones=%d events=%d"),
		WeaponSheatheUpperBodyBoneIndices.Num(),
		M2Events.Num());
}

void UWoWCharacterPreviewComponent::AddBoneSubtreeToSet(int32 RootBoneIndex, TSet<int32>& OutBoneSet) const
{
	if (!BoneParents.IsValidIndex(RootBoneIndex))
	{
		return;
	}

	TArray<int32> Stack;
	Stack.Add(RootBoneIndex);
	while (Stack.Num() > 0)
	{
		const int32 BoneIndex = Stack.Pop(EAllowShrinking::No);
		if (!BoneParents.IsValidIndex(BoneIndex) || OutBoneSet.Contains(BoneIndex))
		{
			continue;
		}

		OutBoneSet.Add(BoneIndex);
		for (int32 ChildIndex = 0; ChildIndex < BoneParents.Num(); ++ChildIndex)
		{
			if (BoneParents[ChildIndex] == BoneIndex)
			{
				Stack.Add(ChildIndex);
			}
		}
	}
}

float UWoWCharacterPreviewComponent::GetBoneSetInfluence(const FIntVector4& Weights, const FIntVector4& BoneIndices, const TSet<int32>& BoneSet) const
{
	if (BoneSet.Num() == 0)
	{
		return 0.0f;
	}

	float Influence = 0.0f;
	for (int32 InfluenceIndex = 0; InfluenceIndex < 4; ++InfluenceIndex)
	{
		const int32 WeightByte = GetIntVector4Component(Weights, InfluenceIndex);
		if (WeightByte <= 0)
		{
			continue;
		}

		const int32 BoneIndex = GetIntVector4Component(BoneIndices, InfluenceIndex);
		if (BoneSet.Contains(BoneIndex))
		{
			Influence += static_cast<float>(WeightByte) / 255.0f;
		}
	}

	return FMath::Clamp(Influence, 0.0f, 1.0f);
}

FVector UWoWCharacterPreviewComponent::ApplyLocomotionPoseToSkinnedPosition(const FVector& SkinnedPosition, const FIntVector4& Weights, const FIntVector4& BoneIndices) const
{
	if (FMath::Abs(StrafePoseAmount) < 0.01f)
	{
		return SkinnedPosition;
	}

	const float TorsoInfluence = GetBoneSetInfluence(Weights, BoneIndices, LocomotionTorsoBoneIndices);
	const float HeadInfluence = GetBoneSetInfluence(Weights, BoneIndices, LocomotionHeadBoneIndices);
	if (TorsoInfluence <= KINDA_SMALL_NUMBER && HeadInfluence <= KINDA_SMALL_NUMBER)
	{
		return SkinnedPosition;
	}

	const float ModelHeight = FMath::Max(PreviewBoundsMax.Z - PreviewBoundsMin.Z, 1.0f);
	const float UpperStart = PreviewBoundsMin.Z + ModelHeight * StrafeUpperBodyStartHeightRatio;
	const float UpperEnd = PreviewBoundsMin.Z + ModelHeight * StrafeUpperBodyEndHeightRatio;
	const float HeightAlphaRaw = FMath::Clamp((SkinnedPosition.Z - UpperStart) / FMath::Max(UpperEnd - UpperStart, 1.0f), 0.0f, 1.0f);
	const float HeightAlpha = HeightAlphaRaw * HeightAlphaRaw * (3.0f - 2.0f * HeightAlphaRaw);
	const FVector Pivot(0.0f, 0.0f, PreviewBoundsMin.Z + ModelHeight * StrafePosePivotHeightRatio);
	const FVector HeadPivot(0.0f, 0.0f, PreviewBoundsMin.Z + ModelHeight * StrafeHeadPivotHeightRatio);

	FVector Result = SkinnedPosition;
	const float UpperBodyAlpha = FMath::Clamp((TorsoInfluence * StrafeTorsoInfluenceScale + HeadInfluence * StrafeHeadInfluenceScale) * HeightAlpha, 0.0f, 1.0f);
	if (UpperBodyAlpha > KINDA_SMALL_NUMBER)
	{
		const float CounterYaw = -StrafePoseAmount * StrafeUpperBodyCounterYawDegrees * UpperBodyAlpha;
		Result = FQuat(FVector::UpVector, FMath::DegreesToRadians(CounterYaw)).RotateVector(Result - Pivot) + Pivot;
	}

	if (HeadInfluence > KINDA_SMALL_NUMBER)
	{
		const float ScaledHeadInfluence = FMath::Clamp(HeadInfluence * StrafeHeadInfluenceScale, 0.0f, 1.0f);
		const float HeadYaw = -StrafePoseAmount * StrafeHeadExtraCounterYawDegrees * ScaledHeadInfluence;
		const float HeadRoll = -StrafePoseAmount * StrafeHeadTiltDegrees * ScaledHeadInfluence;
		Result = FQuat(FVector::UpVector, FMath::DegreesToRadians(HeadYaw)).RotateVector(Result - HeadPivot) + HeadPivot;
		Result = FQuat(FVector::ForwardVector, FMath::DegreesToRadians(HeadRoll)).RotateVector(Result - HeadPivot) + HeadPivot;
	}

	return Result;
}

FVector UWoWCharacterPreviewComponent::SampleSkinnedPosition(
	const FWoWCharacterPreviewAnimationClip& Clip,
	int32 FrameIndex,
	int32 NextFrameIndex,
	float FrameAlpha,
	const FVector& BasePosition,
	const FIntVector4& Weights,
	const FIntVector4& BoneIndices,
	const FWoWCharacterPreviewAnimationClip* ReferenceStandClip,
	const FWoWCharacterPreviewAnimationClip* WeaponGripClip) const
{
	if (!Clip.Frames.IsValidIndex(FrameIndex) || !Clip.Frames.IsValidIndex(NextFrameIndex))
	{
		return BasePosition;
	}

	const TArray<FMatrix>& BoneMatrices = Clip.Frames[FrameIndex];
	const TArray<FMatrix>& NextBoneMatrices = Clip.Frames[NextFrameIndex];
	FVector SkinnedPosition = FVector::ZeroVector;
	float TotalWeight = 0.0f;
	for (int32 InfluenceIndex = 0; InfluenceIndex < 4; ++InfluenceIndex)
	{
		const int32 WeightByte = GetIntVector4Component(Weights, InfluenceIndex);
		if (WeightByte <= 0)
		{
			continue;
		}

		const int32 BoneIndex = GetIntVector4Component(BoneIndices, InfluenceIndex);
		if (!BoneMatrices.IsValidIndex(BoneIndex) || !NextBoneMatrices.IsValidIndex(BoneIndex))
		{
			continue;
		}

		const float Weight = static_cast<float>(WeightByte) / 255.0f;
		const FMatrix* CurrentMatrix = &BoneMatrices[BoneIndex];
		const FMatrix* NextMatrix = &NextBoneMatrices[BoneIndex];
		FVector CurrentFramePosition = TransformPointByExportMatrix(*CurrentMatrix, BasePosition);
		FVector NextFramePosition = TransformPointByExportMatrix(*NextMatrix, BasePosition);
		if (ReferenceStandClip && WeaponGripClip && WeaponGripBoneIndices.Contains(BoneIndex) && BoneParents.IsValidIndex(BoneIndex))
		{
			const int32 ParentBoneIndex = BoneParents[BoneIndex];
			FMatrix StandBoneMatrix = FMatrix::Identity;
			FMatrix StandParentMatrix = FMatrix::Identity;
			FMatrix GripBoneMatrix = FMatrix::Identity;
			if (SampleBoneMatrixAtTime(*ReferenceStandClip, 0.0f, BoneIndex, StandBoneMatrix) &&
				SampleBoneMatrixAtTime(*WeaponGripClip, 0.0f, BoneIndex, GripBoneMatrix) &&
				ParentBoneIndex >= 0 &&
				CurrentMatrix &&
				NextMatrix &&
				BoneMatrices.IsValidIndex(ParentBoneIndex) &&
				NextBoneMatrices.IsValidIndex(ParentBoneIndex) &&
				SampleBoneMatrixAtTime(*ReferenceStandClip, 0.0f, ParentBoneIndex, StandParentMatrix))
			{
				// HandsClosed 是绝对矩阵；先把它转回 Stand 父骨骼空间，再挂到当前跑跳动作的父骨骼上。
				const FVector GripWorldPoint = TransformPointByExportMatrix(GripBoneMatrix, BasePosition);
				const FVector GripPointInStandParent = InverseTransformPointByExportMatrix(StandParentMatrix, GripWorldPoint);
				CurrentFramePosition = TransformPointByExportMatrix(BoneMatrices[ParentBoneIndex], GripPointInStandParent);
				NextFramePosition = TransformPointByExportMatrix(NextBoneMatrices[ParentBoneIndex], GripPointInStandParent);
			}
		}
		SkinnedPosition += FMath::Lerp(CurrentFramePosition, NextFramePosition, FrameAlpha) * Weight;
		TotalWeight += Weight;
	}

	return TotalWeight > KINDA_SMALL_NUMBER ? SkinnedPosition / TotalWeight : BasePosition;
}

FVector UWoWCharacterPreviewComponent::SampleSkinnedPositionAtTime(
	const FWoWCharacterPreviewAnimationClip& Clip,
	float TimeSeconds,
	const FVector& BasePosition,
	const FIntVector4& Weights,
	const FIntVector4& BoneIndices) const
{
	if (Clip.Frames.Num() == 0 || Clip.SampleRate <= 0.0f)
	{
		return BasePosition;
	}

	int32 FrameIndex = 0;
	int32 NextFrameIndex = 0;
	float FrameAlpha = 0.0f;
	ResolveLoopingClipFrame(Clip, TimeSeconds, FrameIndex, NextFrameIndex, FrameAlpha);
	return SampleSkinnedPosition(Clip, FrameIndex, NextFrameIndex, FrameAlpha, BasePosition, Weights, BoneIndices, nullptr, nullptr);
}

void UWoWCharacterPreviewComponent::SkinSection(
	const FWoWCharacterPreviewAnimationClip& Clip,
	const FWoWCharacterPreviewAnimationClip* StrafeOverlayClip,
	const FWoWCharacterPreviewAnimationClip* ReferenceStandClip,
	int32 FrameIndex,
	int32 NextFrameIndex,
	float FrameAlpha,
	int32 OverlayFrameIndex,
	int32 OverlayNextFrameIndex,
	float OverlayFrameAlpha,
	const FWoWCharacterPreviewAnimationClip* LandingOverlayClip,
	float LandingOverlayTimeSeconds,
	float LandingOverlayAlpha,
	const FWoWCharacterPreviewAnimationClip* WeaponGripClip,
	FWoWCharacterPreviewSectionState& SectionState)
{
	if (!Clip.Frames.IsValidIndex(FrameIndex) || !Clip.Frames.IsValidIndex(NextFrameIndex))
	{
		return;
	}

	const int32 VertexCount = SectionState.BasePositions.Num();
	if (SectionState.SkinnedPositions.Num() != VertexCount)
	{
		SectionState.SkinnedPositions.SetNum(VertexCount);
	}

	for (int32 VertexIndex = 0; VertexIndex < VertexCount; ++VertexIndex)
	{
		const FVector BasePosition = SectionState.BasePositions[VertexIndex];
		const FIntVector4 Weights = SectionState.BoneWeights.IsValidIndex(VertexIndex) ? SectionState.BoneWeights[VertexIndex] : FIntVector4(255, 0, 0, 0);
		const FIntVector4 BoneIndices = SectionState.BoneIndices.IsValidIndex(VertexIndex) ? SectionState.BoneIndices[VertexIndex] : FIntVector4(0, 0, 0, 0);

		FVector NormalizedPosition = SampleSkinnedPosition(Clip, FrameIndex, NextFrameIndex, FrameAlpha, BasePosition, Weights, BoneIndices, ReferenceStandClip, WeaponGripClip);
		if (StrafeOverlayClip && StrafeOverlayAmount > 0.01f)
		{
			const float LowerBodyInfluence = GetBoneSetInfluence(Weights, BoneIndices, LocomotionLowerBodyBoneIndices);
			const float OverlayAlpha = FMath::Clamp(LowerBodyInfluence * StrafeOverlayAmount, 0.0f, 1.0f);
			if (OverlayAlpha > KINDA_SMALL_NUMBER)
			{
				const FVector OverlayPosition = SampleSkinnedPosition(*StrafeOverlayClip, OverlayFrameIndex, OverlayNextFrameIndex, OverlayFrameAlpha, BasePosition, Weights, BoneIndices);
				const FVector ReferencePosition = ReferenceStandClip
					? SampleSkinnedPositionAtTime(*ReferenceStandClip, StrafeOverlayTimeSeconds, BasePosition, Weights, BoneIndices)
					: BasePosition;
				NormalizedPosition += (OverlayPosition - ReferencePosition) * OverlayAlpha;
			}
		}
		if (LandingOverlayClip && LandingOverlayAlpha > 0.01f)
		{
			const FVector LandingPosition = SampleSkinnedPositionAtTime(*LandingOverlayClip, LandingOverlayTimeSeconds, BasePosition, Weights, BoneIndices);
			NormalizedPosition = FMath::Lerp(NormalizedPosition, LandingPosition, FMath::Clamp(LandingOverlayAlpha, 0.0f, 1.0f));
		}
		SectionState.SkinnedPositions[VertexIndex] = ApplyLocomotionPoseToSkinnedPosition(NormalizedPosition, Weights, BoneIndices);
	}
}
