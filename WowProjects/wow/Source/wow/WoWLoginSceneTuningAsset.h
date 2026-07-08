#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "WoWLoginSceneTuningAsset.generated.h"

UENUM(BlueprintType)
enum class EWoWLoginTextureRenderOverride : uint8
{
	M2Original,
	AdditiveAlpha,
	AlphaBlend,
	Opaque,
	Masked,
	Additive,
	Modulate,
	AlphaComposite
};

USTRUCT(BlueprintType)
struct WOW_API FWoWLoginParticleEmitterTuning
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	int32 EmitterIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	FString Note;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	bool bEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	FVector PositionOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float EmissionRateScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float EmissionSpeedScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float LifespanScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float AreaLengthScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float AreaWidthScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float GravityScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float Gravity2Scale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float VerticalRangeScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float HorizontalRangeScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float SizeScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float LifecycleSizeStartScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float LifecycleSizeMidScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float LifecycleSizeEndScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float AlphaScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float LifecycleAlphaStartScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float LifecycleAlphaMidScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float LifecycleAlphaEndScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float AlphaCutoff = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particle")
	float BrightnessScale = 1.0f;
};

USTRUCT(BlueprintType)
struct WOW_API FWoWLoginTextureSectionTuning
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 SectionIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	FString Note;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	bool bEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	bool bUseM2TextureAnimation = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	bool bForceVisibleDebug = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	EWoWLoginTextureRenderOverride RenderOverride = EWoWLoginTextureRenderOverride::M2Original;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 RenderFlagOverride = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	int32 BlendModeOverride = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	FVector2D UVOffset = FVector2D::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	FVector2D UVScale = FVector2D::UnitVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	float UVRotationDegrees = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	float AlphaScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Texture")
	float BrightnessScale = 1.0f;
};

USTRUCT(BlueprintType)
struct WOW_API FWoWLoginLightTuning
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light")
	int32 LightIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light")
	FString Note;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light")
	bool bEnabled = true;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light")
	float DiffuseIntensityScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light")
	float AmbientIntensityScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light")
	FLinearColor DiffuseColorScale = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light")
	FLinearColor AmbientColorScale = FLinearColor::White;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Light")
	float AttenuationRadiusScale = 1.0f;
};

USTRUCT(BlueprintType)
struct WOW_API FWoWLoginSceneTuningState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scene")
	FVector SceneLocationOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scene")
	FRotator SceneRotationOffset = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Scene")
	FVector SceneScaleMultiplier = FVector::OneVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	FVector CameraPositionOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	FVector CameraTargetOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float CameraVerticalFovOffsetDegrees = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float CameraRollOffsetDegrees = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float CameraNearClipOffsetWowUnits = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float CameraFarClipOffsetWowUnits = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lighting")
	float GlobalBrightnessScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lighting")
	bool bUseM2PhysicalLightBridge = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lighting")
	float M2PhysicalLightIntensityScale = 80.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Lighting")
	TArray<FWoWLoginLightTuning> LightTunings;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Particles")
	TArray<FWoWLoginParticleEmitterTuning> ParticleEmitterTunings;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Textures")
	TArray<FWoWLoginTextureSectionTuning> TextureSectionTunings;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Preview")
	FVector CharacterPreviewLocationOffset = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Preview")
	FRotator CharacterPreviewRotationOffset = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Preview")
	FVector CharacterPreviewScaleMultiplier = FVector::OneVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Preview")
	float CharacterPreviewUniformScale = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Preview")
	int32 CharacterPreviewAnimationOverrideId = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Character Preview")
	FString CharacterPreviewAnimationOverridePath;
};

UCLASS(BlueprintType)
class WOW_API UWoWLoginSceneTuningAsset : public UDataAsset
{
	GENERATED_BODY()

public:
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Login Scene")
	FWoWLoginSceneTuningState Tuning;
};
