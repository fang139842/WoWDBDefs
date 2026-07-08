#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ProceduralMeshComponent.h"
#include "WoWM2ModelComponent.generated.h"

class UMaterialInterface;
class UTexture2D;

struct FWoWM2TextureTransformSample
{
	float TimeSeconds = 0.0f;
	FVector2D Translation = FVector2D::ZeroVector;
	FVector2D Scaling = FVector2D::UnitVector;
	float RotationDegrees = 0.0f;
};

struct FWoWM2ColorSample
{
	float TimeSeconds = 0.0f;
	FLinearColor Color = FLinearColor::White;
};

struct FWoWM2SectionState
{
	TArray<FVector> BasePositions;
	TArray<FIntVector4> BoneWeights;
	TArray<FIntVector4> BoneIndices;
	TArray<FVector> Normals;
	TArray<FVector2D> UV0;
	TArray<FVector2D> BaseUV0;
	TArray<FLinearColor> VertexColors;
	TArray<FProcMeshTangent> Tangents;
	TArray<FVector> SkinnedPositions;
	TArray<FWoWM2TextureTransformSample> TextureTransformSamples;
	TArray<FWoWM2ColorSample> ColorSamples;
	bool bScaleLowAlphaAdditive = false;
};

struct FWoWM2AnimationClip
{
	FString Key;
	int32 SequenceIndex = INDEX_NONE;
	TArray<int32> LookupIds;
	int32 AnimationId = INDEX_NONE;
	int32 SubAnimationId = INDEX_NONE;
	float LengthSeconds = 0.0f;
	float SampleRate = 20.0f;
	bool bLoopEndFrame = false;
	TArray<TArray<FMatrix>> Frames;
};

struct FWoWM2BoneMetadata
{
	int32 Index = INDEX_NONE;
	int32 Flags = 0;
	int32 Parent = INDEX_NONE;
	FVector Pivot = FVector::ZeroVector;
};

struct FWoWM2RibbonSample
{
	float TimeSeconds = 0.0f;
	FLinearColor Color = FLinearColor::White;
	float Above = 0.0f;
	float Below = 0.0f;
};

struct FWoWM2RibbonTexSlotSample
{
	float TimeSeconds = 0.0f;
	int32 Slot = 0;
};

struct FWoWM2RibbonVisibilitySample
{
	float TimeSeconds = 0.0f;
	bool bVisible = true;
};

struct FWoWM2RibbonEmitter
{
	int32 Index = INDEX_NONE;
	int32 Id = INDEX_NONE;
	int32 BoneIndex = INDEX_NONE;
	FVector Position = FVector::ZeroVector;
	FString PreviewPng;
	TArray<int32> MaterialIndices;
	float EdgesPerSecond = 1.0f;
	float EdgeLifetimeSeconds = 1.0f;
	float Gravity = 0.0f;
	int32 TextureRows = 1;
	int32 TextureCols = 1;
	float LengthSeconds = 1.0f;
	TArray<FWoWM2RibbonSample> Samples;
	TArray<FWoWM2RibbonTexSlotSample> TexSlotSamples;
	TArray<FWoWM2RibbonVisibilitySample> VisibilitySamples;
};

struct FWoWM2RibbonSegment
{
	FVector Position = FVector::ZeroVector;
	FVector Up = FVector::UpVector;
	FVector Back = FVector::ZeroVector;
	float Length = 0.0f;
	float OriginalLength = 1.0f;
	float AgeSeconds = 0.0f;
};

struct FWoWM2RibbonRuntimeState
{
	FWoWM2RibbonEmitter Emitter;
	TArray<FWoWM2RibbonSegment> Segments;
	TObjectPtr<UMaterialInterface> Material = nullptr;
	FVector LastPosition = FVector::ZeroVector;
	float TimeSinceLastEdgeSeconds = 0.0f;
	bool bHasLastPosition = false;
};

struct FWoWM2ParticleFloatSample
{
	float TimeSeconds = 0.0f;
	float Value = 0.0f;
};

struct FWoWM2ParticleEmitter
{
	int32 Index = INDEX_NONE;
	int32 Flags = 0;
	int32 BoneIndex = INDEX_NONE;
	FVector Position = FVector::ZeroVector;
	FString PreviewPng;
	int32 Blend = 0;
	int32 EmitterType = 0;
	int32 ParticleType = 0;
	int32 TextureTileRotation = 0;
	int32 TextureColumns = 1;
	int32 TextureRows = 1;
	FLinearColor LifecycleColors[3] = { FLinearColor::White, FLinearColor::White, FLinearColor(1.0f, 1.0f, 1.0f, 0.0f) };
	float LifecycleSizes[3] = { 1.0f, 1.0f, 1.0f };
	float MidPoint = 0.5f;
	float Slowdown = 0.0f;
	float Rotation = 0.0f;
	TArray<FWoWM2ParticleFloatSample> EmissionSpeedSamples;
	TArray<FWoWM2ParticleFloatSample> SpeedVariationSamples;
	TArray<FWoWM2ParticleFloatSample> LifespanSamples;
	TArray<FWoWM2ParticleFloatSample> EmissionRateSamples;
	TArray<FWoWM2ParticleFloatSample> EmissionAreaLengthSamples;
	TArray<FWoWM2ParticleFloatSample> EmissionAreaWidthSamples;
	TArray<FWoWM2ParticleFloatSample> GravitySamples;
	TArray<FWoWM2ParticleFloatSample> Gravity2Samples;
	TArray<FWoWM2ParticleFloatSample> VerticalRangeSamples;
	TArray<FWoWM2ParticleFloatSample> HorizontalRangeSamples;
};

struct FWoWM2ParticleInstance
{
	FVector Position = FVector::ZeroVector;
	FVector Origin = FVector::ZeroVector;
	FVector Velocity = FVector::ZeroVector;
	FVector Direction = FVector::ForwardVector;
	FVector Down = FVector(0.0f, 0.0f, -1.0f);
	float Age = 0.0f;
	float Lifespan = 1.0f;
	int32 InitialTileIndex = 0;
};

struct FWoWM2ParticleRuntimeState
{
	FWoWM2ParticleEmitter Emitter;
	TWeakObjectPtr<UProceduralMeshComponent> MeshComponent;
	TObjectPtr<UMaterialInterface> Material = nullptr;
	TArray<FWoWM2ParticleInstance> Particles;
	float SpawnRemainder = 0.0f;
	int32 RandomSeed = 0;
	int32 MeshSectionVertexCount = 0;
	bool bMeshSectionInitialized = false;
};

UCLASS(ClassGroup = (WoW), meta = (BlueprintSpawnableComponent))
class WOW_API UWoWM2ModelComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UWoWM2ModelComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	bool LoadFromMeshJson(
		USceneComponent* AttachParent,
		const FName& SocketName,
		const FString& MeshJsonPath,
		const FTransform& RelativeTransform = FTransform::Identity);

	static bool PrewarmMeshJson(const FString& MeshJsonPath, bool bPrewarmTextures = true);

	void ClearModel();
	UProceduralMeshComponent* GetModelMeshComponent() const { return ModelMeshComponent.Get(); }
	UProceduralMeshComponent* GetRibbonMeshComponent() const { return RibbonMeshComponent.Get(); }
	const FString& GetSourceMeshJsonPath() const { return SourceMeshJsonPath; }
	int32 OverrideModelDiffuseTexture(const FString& PreviewPng);

private:
	void UpdateM2Animation(float DeltaTime, bool bUpdateMeshSections);
	void UpdateM2Ribbons(float DeltaTime);
	void UpdateM2Particles(float DeltaTime);
	void ApplyBillboardBones(TArray<FMatrix>& InOutBoneMatrices) const;
	int32 ResolveAnimationClipIndex() const;
	FWoWM2RibbonSample SampleRibbonEmitter(const FWoWM2RibbonEmitter& Emitter, float TimeSeconds) const;
	int32 SampleRibbonTexSlot(const FWoWM2RibbonEmitter& Emitter, float TimeSeconds) const;
	bool SampleRibbonVisibility(const FWoWM2RibbonEmitter& Emitter, float TimeSeconds) const;
	FVector GetM2BoneWorldPosition(int32 BoneIndex, const FVector& M2ModelPosition) const;
	FQuat GetM2BoneWorldRotation(int32 BoneIndex) const;

	UTexture2D* LoadRuntimeTexture(const FString& PreviewPng);
	UMaterialInterface* CreateRuntimeMaterialForSection(
		const FString& PreviewPng,
		const TArray<FString>& LayerPreviewPngs,
		int32 RenderMode,
		bool bUseEnvMap,
		bool bTwoSided,
		bool bUnlit,
		bool bNoDepthTest,
		const FLinearColor* TintColor);
	UMaterialInterface* CreateRuntimeParticleMaterial(const FWoWM2ParticleEmitter& Emitter, UProceduralMeshComponent* MeshComponent);

	TWeakObjectPtr<UProceduralMeshComponent> ModelMeshComponent;
	TWeakObjectPtr<UProceduralMeshComponent> RibbonMeshComponent;
	TArray<FWoWM2SectionState> SectionStates;
	TArray<FWoWM2AnimationClip> AnimationClips;
	TArray<FMatrix> CurrentBoneMatrices;
	TArray<FWoWM2BoneMetadata> Bones;
	TArray<FWoWM2RibbonRuntimeState> RibbonStates;
	TArray<FWoWM2ParticleRuntimeState> ParticleStates;
	TMap<FString, TObjectPtr<UTexture2D>> TextureCache;
	TArray<TObjectPtr<UMaterialInstanceDynamic>> DynamicMaterials;
	FString SourceMeshJsonPath;
	float AnimationTimeSeconds = 0.0f;
	float RuntimeUpdateAccumulatorSeconds = 0.0f;
	float ParticleUpdateAccumulatorSeconds = 0.0f;
	int32 CurrentAnimationClipIndex = INDEX_NONE;
};
