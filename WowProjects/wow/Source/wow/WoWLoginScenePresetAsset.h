#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "UObject/SoftObjectPath.h"
#include "WoWLoginSceneTuningAsset.h"
#include "WoWLoginScenePresetAsset.generated.h"

USTRUCT(BlueprintType)
struct WOW_API FWoWLoginSceneTextureTransformSample
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	float TimeMs = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	FVector2D Translation = FVector2D::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	FVector2D Scaling = FVector2D::UnitVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	float RotationDegrees = 0.0f;
};

USTRUCT(BlueprintType)
struct WOW_API FWoWLoginSceneTextureSectionTrack
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 SlotIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 SubmeshIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 SubmeshId = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 BatchFlags = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 MaterialIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 MaterialFlags = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 PriorityPlane = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 ShaderId = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 RenderMode = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 TextureCount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 TextureComboIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 TextureCoordComboIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 TextureWeightComboIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 TextureTransformComboIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 TextureCombinerIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 TextureCombinerRaw0 = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 TextureCombinerRaw1 = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 SkinTexUnitFlags = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 SkinTexUnitFlags2 = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 SkinTexUnitOrder = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 SkinSubmesh = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 SkinSubmesh2 = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 SkinRenderFlagsIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 SkinTexUnit = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 SkinTexUnitMode = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 SkinTextureLookupIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 SkinTexUnit2 = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 SkinTransparencyLookupIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 SkinTextureAnimLookupIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 TextureUnit = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 ColorIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 TransparencyIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 TextureTransformIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	bool bUseEnvMap = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	bool bTwoSided = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	bool bUnlit = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	bool bBillboard = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	bool bNoDepthWrite = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 TextureIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 TextureType = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 TextureFlags = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	FString SourceBlp;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	FString PreviewPng;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	TArray<FWoWLoginSceneTextureTransformSample> Samples;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	float DurationMs = 0.0f;
};

USTRUCT(BlueprintType)
struct WOW_API FWoWLoginSceneColorSample
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Color")
	float TimeMs = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Color")
	FLinearColor Color = FLinearColor::White;
};

USTRUCT(BlueprintType)
struct WOW_API FWoWLoginSceneSectionColorTrack
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Color")
	int32 SlotIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Color")
	FLinearColor BaseColor = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Color")
	TArray<FWoWLoginSceneColorSample> Samples;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Color")
	float DurationMs = 0.0f;
};

USTRUCT(BlueprintType)
struct WOW_API FWoWLoginSceneCameraVectorSample
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float TimeMs = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	FVector Value = FVector::ZeroVector;
};

USTRUCT(BlueprintType)
struct WOW_API FWoWLoginSceneCameraRollSample
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float TimeMs = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float Degrees = 0.0f;
};

USTRUCT(BlueprintType)
struct WOW_API FWoWLoginSceneBoneInfo
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bones")
	int32 Index = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bones")
	int32 KeyBoneId = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bones")
	int32 Flags = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bones")
	int32 Parent = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bones")
	int32 GeosetId = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bones")
	FVector Pivot = FVector::ZeroVector;
};

USTRUCT(BlueprintType)
struct WOW_API FWoWLoginSceneBoneFrameSample
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bones")
	int32 FrameIndex = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bones")
	float TimeMs = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bones")
	FTransform Transform = FTransform::Identity;
};

USTRUCT(BlueprintType)
struct WOW_API FWoWLoginSceneBoneAnimationTrack
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bones")
	int32 BoneIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Bones")
	TArray<FWoWLoginSceneBoneFrameSample> Samples;
};

USTRUCT(BlueprintType)
struct WOW_API FWoWLoginSceneLightSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lights")
	int32 Index = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lights")
	int32 LightType = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lights")
	int32 BoneIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lights")
	FVector Position = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lights")
	FLinearColor AmbientColor = FLinearColor::Black;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lights")
	float AmbientIntensity = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lights")
	FLinearColor DiffuseColor = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lights")
	float DiffuseIntensity = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lights")
	float AttenuationStart = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lights")
	float AttenuationEnd = 0.0f;
};

USTRUCT(BlueprintType)
struct WOW_API FWoWLoginSceneParticleFloatSample
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	float TimeMs = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	float Value = 0.0f;
};

USTRUCT(BlueprintType)
struct WOW_API FWoWLoginSceneParticleEmitterSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	int32 Index = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	int32 Flags = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	int32 BoneIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	FVector Position = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	FString PreviewPng;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	int32 Blend = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	int32 EmitterType = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	int32 ParticleType = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	int32 HeadOrTail = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	int32 TextureTileRotation = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	int32 TextureColumns = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	int32 TextureRows = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	bool bSnow = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	bool bSmoke = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	bool bFire = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	TArray<FLinearColor> LifecycleColors;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	TArray<float> LifecycleSizes;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	float MidPoint = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	float Slowdown = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	float Rotation = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	TArray<FWoWLoginSceneParticleFloatSample> EmissionSpeedSamples;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	TArray<FWoWLoginSceneParticleFloatSample> SpeedVariationSamples;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	TArray<FWoWLoginSceneParticleFloatSample> LifespanSamples;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	TArray<FWoWLoginSceneParticleFloatSample> EmissionRateSamples;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	TArray<FWoWLoginSceneParticleFloatSample> EmissionAreaLengthSamples;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	TArray<FWoWLoginSceneParticleFloatSample> EmissionAreaWidthSamples;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	TArray<FWoWLoginSceneParticleFloatSample> GravitySamples;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	TArray<FWoWLoginSceneParticleFloatSample> Gravity2Samples;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	TArray<FWoWLoginSceneParticleFloatSample> VerticalRangeSamples;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	TArray<FWoWLoginSceneParticleFloatSample> HorizontalRangeSamples;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	TArray<FWoWLoginSceneParticleFloatSample> ZSourceSamples;
};

USTRUCT(BlueprintType)
struct WOW_API FWoWLoginSceneRuntimeCacheSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	bool bCameraLoaded = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	int32 CameraId = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	FVector CameraPosition = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	FVector CameraTarget = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float CameraFovDegrees = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float CameraNearClip = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float CameraFarClip = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	bool bRuntimeLoaded = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	TArray<FWoWLoginSceneTextureSectionTrack> TextureTransformTracks;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	TArray<FWoWLoginSceneTextureSectionTrack> RenderSections;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	TArray<FWoWLoginSceneSectionColorTrack> ColorTracks;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	TArray<int32> LateAdditiveOverlaySlots;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	TArray<FWoWLoginSceneCameraVectorSample> CameraPositionSamples;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	TArray<FWoWLoginSceneCameraVectorSample> CameraTargetSamples;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	TArray<FWoWLoginSceneCameraRollSample> CameraRollSamples;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	TArray<FVector> BonePivots;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	TArray<FWoWLoginSceneBoneInfo> LoginBones;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	TArray<FWoWLoginSceneBoneAnimationTrack> BoneAnimationTracks;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	TArray<FWoWLoginSceneLightSnapshot> LoginSceneLights;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	TArray<FWoWLoginSceneParticleEmitterSnapshot> ParticleEmitters;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	float CameraTrackDurationMs = 0.0f;
};

UCLASS(BlueprintType)
class WOW_API UWoWLoginScenePresetAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Login Scene")
	FString SceneKey;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Login Scene")
	FString DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Login Scene")
	FString JsonRelativePath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Login Scene")
	int64 SourceJsonCrc = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assets")
	FString MeshPath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assets")
	FString AnimationPath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assets")
	FString MaterialPackagePath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assets")
	FString TuningAssetPath;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Assets")
	TArray<FSoftObjectPath> RequiredAssets;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Tuning")
	FWoWLoginSceneTuningState Tuning;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Runtime")
	FWoWLoginSceneRuntimeCacheSnapshot RuntimeCache;
};
