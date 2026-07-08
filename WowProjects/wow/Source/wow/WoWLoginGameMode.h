#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Containers/Queue.h"
#include "UObject/SoftObjectPath.h"
#include "WoWAzerothCoreService.h"
#include "WoWLoginSceneTuningAsset.h"
#include "WoWLoginGameMode.generated.h"

class AActor;
class ACameraActor;
class ASkeletalMeshActor;
class ULightComponent;
class UMaterialInstanceDynamic;
class UProceduralMeshComponent;
class USkeletalMeshComponent;
class USkyLightComponent;
class UTexture2D;
class UWoWM2ModelComponent;
class UWoWLoginScenePresetAsset;
class UWoWLoginSceneTuningAsset;
class FWoWGlueFrame;
class FWoWGlueLuaRuntime;
class FWoWGlueUIManager;
struct FStreamableHandle;
class SWidget;
class SWoWGlueRoot;
struct FWoWGlueCharacterSummary;

struct FGlueLoginAsyncResult
{
	int32 RequestSerial = 0;
	FString AccountName;
	FWoWAzerothAccountRecord AccountRecord;
	TArray<FWoWGlueRealmSummary> Realms;
	TArray<FWoWGlueCharacterSummary> Characters;
	FString ErrorText;
	FString CharacterErrorText;
	double ElapsedSeconds = 0.0;
	uint32 WorkerThreadId = 0;
	bool bAuthenticated = false;
	bool bWorldConnected = false;
	bool bCharactersLoaded = false;
};

struct FGlueRealmAsyncResult
{
	int32 RequestSerial = 0;
	int32 RealmIndex = 0;
	TArray<FWoWGlueCharacterSummary> Characters;
	FString ErrorText;
	FString CharacterErrorText;
	double ElapsedSeconds = 0.0;
	uint32 WorkerThreadId = 0;
	bool bConnected = false;
	bool bCharactersLoaded = false;
};

USTRUCT(BlueprintType)
struct WOW_API FWoWLoginSceneM2CameraState
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	int32 Id = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	FVector Pos = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	FVector Target = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float FovRadians = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float NearClipping = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float FarClipping = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Camera")
	float RollRadians = 0.0f;
};

UCLASS()
class WOW_API AWoWLoginGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:
	AWoWLoginGameMode();

	virtual void StartPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	struct FLoginSceneDefinition
	{
		FString Key;
		FString DisplayName;
		FString MeshPath;
		FString AnimationPath;
		FString JsonRelativePath;
		FString MaterialPackagePath;
		FString TuningAssetPath;
		FString TuningAssetPackagePath;
		FString PresetAssetPath;
		FString PresetAssetPackagePath;
	};

	struct FPlayerCharacterPreviewDefinition
	{
		int32 Race = 0;
		int32 Gender = 0;
		FString Id;
		FString DisplayName;
		FString SourceModelPath;
		FString GeneratedAssetStem;
		FString MeshJsonPath;
		FString MeshPath;
		FString AnimationPath;
		FString AnimationPackagePath;
		FString MaterialPackagePath;
		FString StandAnimationPath;
		FString LookupAnimationPath;
		FString SectionMetadataPath;
	};

	struct FCharacterSelectPreviewAppearance
	{
		int32 Guid = 0;
		FString CharacterName;
		int32 Race = 0;
		int32 ClassId = 0;
		int32 Gender = 0;
		int32 Skin = 0;
		int32 Face = 0;
		int32 HairStyle = 0;
		int32 HairColor = 0;
		int32 FacialStyle = 0;
		FString EquipmentSummary;
		FString AppearanceDescriptorPath;
		FString CharacterPreviewMeshPath;
		FString CharacterPreviewMeshJsonPath;
		FString CharacterPreviewMaterialPackagePath;
		FString CharacterPreviewAnimationPath;
		FString CharacterPreviewLookupAnimationPath;
		FString CharacterPreviewAnimationPackagePath;
		FString CharacterPreviewAnimationStem;
		FString CharacterPreviewSectionMetadataPath;
		FString CharacterPreviewAttachmentDescriptorPath;
		FString CacheKey;
		bool bHasAppearanceDescriptor = false;
		bool bHasCharacterPreviewAsset = false;
		bool bUsesBaseRuntimeComposition = false;
		int32 EquipmentEntryCount = 0;
	};

	enum class ECharacterPreviewBuildStatus : uint8
	{
		Unknown,
		Complete,
		NeedsRebuild,
		MissingCriticalSource,
		FailedPreviously
	};

	struct FCharacterSelectPreviewDebugState
	{
		bool bHasPreviewActor = false;
		int32 SelectedGuid = 0;
		int32 Race = 0;
		int32 ClassId = 0;
		int32 Gender = 0;
		int32 Skin = 0;
		int32 Face = 0;
		int32 HairStyle = 0;
		int32 HairColor = 0;
		int32 FacialStyle = 0;
		FString DefinitionId;
		FString DisplayName;
		FString MeshPath;
		FString StandAnimationPath;
		FString ActiveAnimationPath;
		FString EquipmentSummary;
		FString AppearanceDescriptorPath;
		FString CharacterPreviewMeshPath;
		FString CharacterPreviewAnimationPath;
		FString CharacterPreviewLookupAnimationPath;
		FString BaseLookupAnimationPath;
		FString CharacterPreviewSectionMetadataPath;
		FString CharacterPreviewBuildStatus;
		FString CharacterPreviewBuildReason;
		FString CharacterPreviewBuildLogPath;
		FString AppearanceCacheKey;
		bool bHasAppearanceDescriptor = false;
		bool bHasCharacterPreviewAsset = false;
		int32 EquipmentEntryCount = 0;
		FVector BaseLocation = FVector::ZeroVector;
		FRotator BaseRotation = FRotator::ZeroRotator;
		float BaseScale = 1.0f;
		FVector FinalLocation = FVector::ZeroVector;
		FRotator FinalRotation = FRotator::ZeroRotator;
		FVector FinalScale = FVector::OneVector;
	};

	struct FCharacterPreviewAnimationOption
	{
		int32 AnimationId = INDEX_NONE;
		FString Name;
		FString ObjectPath;
		bool bAvailable = false;
	};

	struct FCharacterPreviewAnimationCatalogState
	{
		bool bReady = false;
		bool bWarming = false;
		int32 CandidatePathCount = 0;
		int32 ImportedAnimationIdCount = 0;
		int32 ResolvedOptionCount = 0;
		int32 RemainingAnimationIdCount = 0;
	};

	const TArray<FLoginSceneDefinition>& GetLoginSceneDefinitions() const { return LoginSceneDefinitions; }
	int32 GetActiveLoginSceneIndex() const { return ActiveLoginSceneIndex; }
	FText GetActiveLoginSceneDisplayText() const;
	void SetActiveLoginSceneIndex(int32 NewIndex);
	void RequestActiveLoginSceneIndex(int32 NewIndex);
	void StartLoginSceneAsyncLoad(int32 RequestSerial, int32 SceneIndex, bool bAllowPresetBootstrap = true);
	void CollectLoginSceneAsyncAssets(int32 SceneIndex, TArray<FSoftObjectPath>& InOutAssetsToLoad, bool bIncludePresetRequiredAssets = true);
	bool TryPrepareGlueScreenScene(const FString& TargetScreen);
	bool TryPrepareGlueScreenCharacterPreview(const FString& TargetScreen);
	bool TryResolveCharacterSelectSceneIndex(int32& OutSceneIndex) const;
	void CompletePendingGlueScreenTransition(const FString& TargetScreen);

	FWoWLoginSceneTuningState GetLoginSceneTuningState() const;
	void SetLoginSceneTuningState(const FWoWLoginSceneTuningState& NewState);
	void ResetLoginSceneTuningState();
	FCharacterSelectPreviewDebugState GetCharacterSelectPreviewDebugState() const;
	TArray<FCharacterPreviewAnimationOption> GetCharacterPreviewLogicalAnimationOptions() const;
	TArray<FString> GetCharacterPreviewAvailableAnimationPaths() const;
	FCharacterPreviewAnimationCatalogState GetCharacterPreviewAnimationCatalogState() const;
	void SetCharacterPreviewAnimationOverrideById(int32 AnimationId);
	void SetCharacterPreviewAnimationOverridePath(const FString& ObjectPath);
	void ClearCharacterPreviewAnimationOverridePath();
	FWoWLoginSceneM2CameraState GetLoginSceneBaseM2CameraState() const;
	FWoWLoginSceneM2CameraState GetLoginSceneM2CameraState() const;
	void SetLoginSceneM2CameraState(const FWoWLoginSceneM2CameraState& NewState);
	FString ExportLoginSceneTuningState() const;
	FString ExportLoginSceneBaseCameraState() const;
	FString ExportLoginSceneFinalCameraState() const;
	void SaveLoginSceneTuningAsset();
	bool SaveLoginScenePresetAsset(FString* OutStatusMessage = nullptr);
	bool SaveAllLoginScenePresetAssets(FString* OutStatusMessage = nullptr);

	struct FTextureTransformSample
	{
		float TimeMs = 0.0f;
		FVector2D Translation = FVector2D::ZeroVector;
		FVector2D Scaling = FVector2D::UnitVector;
		float RotationDegrees = 0.0f;
	};

	struct FSectionTextureTransformTrack
	{
		int32 SlotIndex = INDEX_NONE;
		int32 SubmeshIndex = INDEX_NONE;
		int32 SubmeshId = INDEX_NONE;
		int32 BatchFlags = 0;
		int32 MaterialIndex = INDEX_NONE;
		int32 MaterialFlags = 0;
		int32 PriorityPlane = 0;
		int32 ShaderId = 0;
		int32 RenderMode = 0;
		int32 TextureCount = 0;
		int32 TextureComboIndex = INDEX_NONE;
		int32 TextureCoordComboIndex = INDEX_NONE;
		int32 TextureWeightComboIndex = INDEX_NONE;
		int32 TextureTransformComboIndex = INDEX_NONE;
		int32 TextureCombinerIndex = INDEX_NONE;
		int32 TextureCombinerRaw0 = INDEX_NONE;
		int32 TextureCombinerRaw1 = INDEX_NONE;
		int32 SkinTexUnitFlags = 0;
		int32 SkinTexUnitFlags2 = 0;
		int32 SkinTexUnitOrder = 0;
		int32 SkinSubmesh = INDEX_NONE;
		int32 SkinSubmesh2 = INDEX_NONE;
		int32 SkinRenderFlagsIndex = INDEX_NONE;
		int32 SkinTexUnit = INDEX_NONE;
		int32 SkinTexUnitMode = 0;
		int32 SkinTextureLookupIndex = INDEX_NONE;
		int32 SkinTexUnit2 = INDEX_NONE;
		int32 SkinTransparencyLookupIndex = INDEX_NONE;
		int32 SkinTextureAnimLookupIndex = INDEX_NONE;
		int32 TextureUnit = INDEX_NONE;
		int32 ColorIndex = INDEX_NONE;
		int32 TransparencyIndex = INDEX_NONE;
		int32 TextureTransformIndex = INDEX_NONE;
		bool bUseEnvMap = false;
		bool bTwoSided = false;
		bool bUnlit = false;
		bool bBillboard = false;
		bool bNoDepthWrite = false;
		int32 TextureIndex = INDEX_NONE;
		int32 TextureType = INDEX_NONE;
		int32 TextureFlags = 0;
		FString SourceBlp;
		FString PreviewPng;
		TArray<FTextureTransformSample> Samples;
		float DurationMs = 0.0f;
	};

	struct FColorSample
	{
		float TimeMs = 0.0f;
		FLinearColor Color = FLinearColor::White;
	};

	struct FSectionColorTrack
	{
		int32 SlotIndex = INDEX_NONE;
		FLinearColor BaseColor = FLinearColor::White;
		TArray<FColorSample> Samples;
		float DurationMs = 0.0f;
	};

	struct FCameraVectorSample
	{
		float TimeMs = 0.0f;
		FVector Value = FVector::ZeroVector;
	};

	struct FCameraRollSample
	{
		float TimeMs = 0.0f;
		float Degrees = 0.0f;
	};

	struct FLoginBoneInfo
	{
		int32 Index = INDEX_NONE;
		int32 KeyBoneId = INDEX_NONE;
		int32 Flags = 0;
		int32 Parent = INDEX_NONE;
		int32 GeosetId = INDEX_NONE;
		FVector Pivot = FVector::ZeroVector;
	};

	struct FLoginBoneFrameSample
	{
		int32 FrameIndex = 0;
		float TimeMs = 0.0f;
		FTransform Transform = FTransform::Identity;
	};

	struct FLoginBoneDebugSummary
	{
		int32 Index = INDEX_NONE;
		int32 Parent = INDEX_NONE;
		int32 Flags = 0;
		int32 GeosetId = INDEX_NONE;
		FVector Pivot = FVector::ZeroVector;
		FVector MinTranslation = FVector::ZeroVector;
		FVector MaxTranslation = FVector::ZeroVector;
		FVector FirstTranslation = FVector::ZeroVector;
		FVector LastTranslation = FVector::ZeroVector;
		int32 SampleCount = 0;
		float DurationMs = 0.0f;
		bool bHasAnimation = false;
	};

	struct FLoginSceneLight
	{
		int32 Index = INDEX_NONE;
		int32 LightType = 0;
		int32 BoneIndex = INDEX_NONE;
		FVector Position = FVector::ZeroVector;
		FLinearColor AmbientColor = FLinearColor::Black;
		float AmbientIntensity = 0.0f;
		FLinearColor DiffuseColor = FLinearColor::White;
		float DiffuseIntensity = 0.0f;
		float AttenuationStart = 0.0f;
		float AttenuationEnd = 0.0f;
	};

	struct FParticleFloatSample
	{
		float TimeMs = 0.0f;
		float Value = 0.0f;
	};

	struct FLoginParticleEmitter
	{
		int32 Index = INDEX_NONE;
		int32 Flags = 0;
		int32 BoneIndex = INDEX_NONE;
		FVector Position = FVector::ZeroVector;
		FString PreviewPng;
		int32 Blend = 0;
		int32 EmitterType = 0;
		int32 ParticleType = 0;
		int32 HeadOrTail = 0;
		int32 TextureTileRotation = 0;
		int32 TextureColumns = 1;
		int32 TextureRows = 1;
		bool bSnow = false;
		bool bSmoke = false;
		bool bFire = false;
		FLinearColor LifecycleColors[3] = { FLinearColor::White, FLinearColor::White, FLinearColor(1.0f, 1.0f, 1.0f, 0.0f) };
		float LifecycleSizes[3] = { 1.0f, 1.0f, 1.0f };
		float MidPoint = 0.5f;
		float Slowdown = 0.0f;
		float Rotation = 0.0f;
		TArray<FParticleFloatSample> EmissionSpeedSamples;
		TArray<FParticleFloatSample> SpeedVariationSamples;
		TArray<FParticleFloatSample> LifespanSamples;
		TArray<FParticleFloatSample> EmissionRateSamples;
		TArray<FParticleFloatSample> EmissionAreaLengthSamples;
		TArray<FParticleFloatSample> EmissionAreaWidthSamples;
		TArray<FParticleFloatSample> GravitySamples;
		TArray<FParticleFloatSample> Gravity2Samples;
		TArray<FParticleFloatSample> VerticalRangeSamples;
		TArray<FParticleFloatSample> HorizontalRangeSamples;
		TArray<FParticleFloatSample> ZSourceSamples;
	};

	struct FLoginParticleInstance
	{
		FVector Position = FVector::ZeroVector;
		FVector Origin = FVector::ZeroVector;
		FVector Velocity = FVector::ZeroVector;
		FVector Direction = FVector::ForwardVector;
		FVector Down = FVector(0.0f, 0.0f, -1.0f);  // 世界空间向下方向 (WMV: p.down)
		float Age = 0.0f;
		float Lifespan = 1.0f;
		float SizeScale = 1.0f;
		int32 TileIndex = 0;
		int32 InitialTileIndex = 0;
	};

	struct FLoginParticleSystem
	{
		FLoginParticleEmitter Emitter;
		UProceduralMeshComponent* MeshComponent = nullptr;
		UMaterialInstanceDynamic* DynamicMaterial = nullptr;
		TWeakObjectPtr<USkeletalMeshComponent> SourceMeshComponent;
		TArray<FVector> SourceBonePivots;
		int32 RandomSeed = 0;
		TArray<FLoginParticleInstance> Particles;
		TArray<FVector> RenderVertices;
		TArray<int32> RenderTriangles;
		TArray<FVector> RenderNormals;
		TArray<FVector2D> RenderUVs;
		TArray<FLinearColor> RenderVertexColors;
		float SpawnRemainder = 0.0f;
		int32 MaxParticles = 840;
		float BrightnessScale = 1.0f;
		float AlphaCutoff = 0.0f;
		float LegacyGammaWeight = 0.0f;
		bool bUseLoginSceneTuning = true;
		bool bMeshSectionInitialized = false;
	};

	struct FLoginSceneLightComponent
	{
		FLoginSceneLight Light;
		TWeakObjectPtr<ULightComponent> Component;
	};

	struct FLoginSceneRuntimeCache
	{
		bool bCameraLoaded = false;
		int32 CameraId = INDEX_NONE;
		FVector CameraPosition = FVector::ZeroVector;
		FVector CameraTarget = FVector::ZeroVector;
		float CameraFovDegrees = 0.0f;
		float CameraNearClip = 0.0f;
		float CameraFarClip = 0.0f;

		bool bRuntimeLoaded = false;
		TArray<FSectionTextureTransformTrack> TextureTransformTracks;
		TArray<FSectionTextureTransformTrack> RenderSections;
		TArray<FSectionColorTrack> ColorTracks;
		TArray<int32> LateAdditiveOverlaySlots;
		TArray<FCameraVectorSample> CameraPositionSamples;
		TArray<FCameraVectorSample> CameraTargetSamples;
		TArray<FCameraRollSample> CameraRollSamples;
		TArray<FVector> BonePivots;
		TArray<FLoginBoneInfo> LoginBones;
		TMap<int32, TArray<FLoginBoneFrameSample>> BoneAnimationSamples;
		TArray<FLoginSceneLight> LoginSceneLights;
		TArray<FLoginParticleEmitter> ParticleEmitters;
		float CameraTrackDurationMs = 0.0f;
	};

	const TArray<FWoWLoginParticleEmitterTuning>& GetLoginParticleEmitterTunings() const;
	const TArray<FLoginSceneLight>& GetLoginSceneLights() const { return LoginSceneLights; }
	FWoWLoginLightTuning GetLoginSceneLightTuning(int32 LightIndex) const;
	void SetLoginSceneLightTuning(const FWoWLoginLightTuning& NewTuning);
	void ResetLoginSceneLightTuning(int32 LightIndex);
	void ResetAllLoginSceneLightTunings();
	const FLoginParticleEmitter* GetLoginParticleEmitterByIndex(int32 EmitterIndex) const;
	TArray<int32> GetLoginParticleEmitterIndices() const;
	FWoWLoginParticleEmitterTuning GetLoginParticleEmitterTuning(int32 EmitterIndex) const;
	void SetLoginParticleEmitterTuning(const FWoWLoginParticleEmitterTuning& NewTuning);
	void ResetLoginParticleEmitterTuning(int32 EmitterIndex);
	void ResetAllLoginParticleEmitterTunings();
	int32 GetLoginParticleCount(int32 EmitterIndex) const;
	TArray<int32> GetLoginTextureSectionIndices() const;
	const FSectionTextureTransformTrack* GetLoginTextureSectionTrackByIndex(int32 SectionIndex) const;
	TArray<int32> GetLoginRenderSectionIndices() const;
	const FSectionTextureTransformTrack* GetLoginRenderSectionByIndex(int32 SectionIndex) const;
	int32 GetLoginCameraPositionSampleCount() const { return CameraPositionSamples.Num(); }
	int32 GetLoginCameraTargetSampleCount() const { return CameraTargetSamples.Num(); }
	int32 GetLoginCameraRollSampleCount() const { return CameraRollSamples.Num(); }
	float GetLoginCameraTrackDurationMs() const { return CameraTrackDurationMs; }
	TArray<int32> GetLoginBoneIndices() const;
	const FLoginBoneInfo* GetLoginBoneInfoByIndex(int32 BoneIndex) const;
	FLoginBoneDebugSummary GetLoginBoneDebugSummary(int32 BoneIndex) const;
	FWoWLoginTextureSectionTuning GetLoginTextureSectionTuning(int32 SectionIndex) const;
	void SetLoginTextureSectionTuning(const FWoWLoginTextureSectionTuning& NewTuning);
	void ResetLoginTextureSectionTuning(int32 SectionIndex);
	void ResetAllLoginTextureSectionTunings();
	void SetLoginTextureDebugHighlightSection(int32 SectionIndex);
	void AddGlueDebugLogLine(const FString& Message);
	void ClearGlueDebugLog();
	FString GetGlueDebugLogText() const;
	FString GetGlueCharacterInfoDebugText() const;

private:
	void BuildLoginSceneDefinitions();
	void BuildLoginSceneRuntimeCache(int32 SceneIndex);
	void PrewarmLoginSceneRuntimeCaches();
	void ProcessLoginSceneRuntimeCachePrewarmQueue();
	void PrewarmLoginSceneAssets();
	void ReloadActiveLoginScene();
	void LoadOrCreateLoginSceneTuningAsset(bool bAllowSyncLoadIfMissing = false);
	UWoWLoginScenePresetAsset* ResolveLoginScenePresetAsset(int32 SceneIndex, bool bAllowSyncLoadIfMissing = false) const;
	bool ApplyLoginScenePresetRuntimeCache(int32 SceneIndex, const UWoWLoginScenePresetAsset* PresetAsset);
	void SaveLoginScenePresetRuntimeCache(UWoWLoginScenePresetAsset* PresetAsset, const FLoginSceneRuntimeCache& RuntimeCache) const;
	bool SaveLoginScenePresetAssetForIndex(int32 SceneIndex, FString* OutStatusMessage = nullptr);
	void ApplyLoginSceneTuning();
	void ApplyLoginSceneTuningToSceneActor();
	void ApplyLoginSceneTuningToCamera(float TimeSeconds);
	void MarkLoginSceneTuningAssetDirty();
	void ClearTemplateLevelActors();
	void SpawnLoginScene();
	void SetupLoginCamera();
	void PresentLoginSceneVisuals();
	bool LoadLoginM2Camera(FVector& OutPosition, FVector& OutTarget, float& OutFovDegrees, float& OutNearClip, float& OutFarClip);
	bool LoadLoginM2RuntimeTracks();
	bool ApplyLoginSceneRuntimeCache(int32 SceneIndex);
	void CreateLoginSceneLateAdditiveOverlay(USkeletalMeshComponent* SourceMeshComponent);
	void CreateLoginSceneDebugHighlightOverlay(USkeletalMeshComponent* SourceMeshComponent);
	void ApplyTextureSectionRenderOverride(int32 SectionIndex);
	void ReapplyLoginSceneRenderOverrides();
	void ApplyTextureSectionVisibility(int32 SectionIndex);
	void SpawnLoginParticleSystems();
	void SpawnCharacterPreviewAttachmentParticleSystems(USkeletalMeshComponent* AttachmentComponent, const FString& AttachmentName, const FString& AttachmentMeshPath, const FString& AttachmentMeshJsonPath, const FString& AttachmentDescriptorPath);
	void DestroyCharacterSelectPreviewAttachmentParticleSystems();
	void SpawnLoginSceneLights();
	void UpdateLoginSceneLights();
	void ApplyLoginSceneTextureAnimation(float TimeSeconds);
	void ApplyLoginSceneColorAnimation(float TimeSeconds);
	FLinearColor ComputeLoginSceneMaterialLightTint() const;
	float ComputeLoginSceneMaterialLightBrightness() const;
	void ApplyLoginSceneCameraAnimation(float TimeSeconds);
	void UpdateLoginParticleSystems(float DeltaSeconds, float TimeSeconds);
	void UpdateParticleSystems(TArray<FLoginParticleSystem>& Systems, float DeltaSeconds, float TimeSeconds);
	float GetParticleSystemWorldScale(const FLoginParticleSystem& System) const;
	FVector GetParticleSystemBoneWorldPosition(const FLoginParticleSystem& System, int32 BoneIndex, const FVector& M2ModelPosition) const;
	FQuat GetParticleSystemBoneWorldRotation(const FLoginParticleSystem& System, int32 BoneIndex) const;
	FVector GetM2BoneWorldPosition(int32 BoneIndex, const FVector& M2ModelPosition) const;
	FQuat GetM2BoneWorldRotation(int32 BoneIndex) const;
	void BuildLoginOverlay();
	bool ReloadLoginGlueUI(FString* OutError = nullptr);
	TSharedRef<SWidget> BuildLoginGlueErrorDialog();
	void ShowLoginGlueError(const FString& ErrorText);
	void ClearLoginGlueError();
	void SetLoginDebugPanelVisible(bool bVisible);
	FReply HandleGlueRootMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);
	FReply HandleGlueRootMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);
	FReply HandleGlueRootMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent);
	void RotateCharacterSelectPreview(float YawDeltaDegrees);
	FReply HandleEnterTestWorldClicked();
	FReply HandleShowDebugPanelClicked();
	FReply HandleReloadGlueLuaClicked();
	void HandleGlueLogin(const FString& AccountName, const FString& Password);
	void StartGlueLoginRequest(int32 RequestSerial, FString AccountName, FString Password);
	void FinishGlueLogin(const FGlueLoginAsyncResult& LoginResult);
	void HandleGlueRealmSelected(int32 RealmIndex);
	void StartGlueRealmConnectRequest(int32 RequestSerial, int32 RealmIndex);
	void FinishGlueRealmSelected(const FGlueRealmAsyncResult& RealmResult);
	void HandleGlueRealmCancelled();
	void HandleGlueScreenChanged(const FString& ScreenName);
	void HandleGlueBackgroundModelChanged(const FString& ModelPath, int32 Race, int32 Gender);
	void HandleGlueCharacterSelected(int32 CharacterGuid);
	void HandleGlueEnterWorld();
	void ResetGlueLoginSessionState();
	void HandleGlueCharacterCreate(const FString& CharacterName, int32 Race, int32 ClassId, int32 Gender, int32 Skin, int32 Face, int32 HairStyle, int32 HairColor, int32 FacialStyle);
	void ProcessGlueAsyncResults();
	int32 FindLoginSceneIndexByKeyFragment(const FString& KeyFragment) const;
	int32 FindLoginSceneIndexByModelPath(const FString& ModelPath) const;
	void ApplyGlueSceneForScreen(const FString& ScreenName);
	void LoadPlayerCharacterPreviewDefinitions();
	const FPlayerCharacterPreviewDefinition* FindPlayerCharacterPreviewDefinition(int32 Race, int32 Gender) const;
	void QueueCharacterPreviewPrewarm();
	void ProcessCharacterPreviewPrewarmQueue();
	void PrewarmCharacterPreviewRow(const FString& Row);
	void RequestSelectedCharacterPreviewRefresh();
	bool ResolveCharacterPreviewRuntimeAssets(const TArray<FString>& Columns, FCharacterSelectPreviewAppearance& OutAppearance, const FPlayerCharacterPreviewDefinition*& OutDefinition, FString& OutAppearanceSignature, FString& OutCharacterPreviewStem, FString& OutCharacterPreviewPackageRoot, FString& OutCharacterPreviewLookupAnimationPath) const;
	bool EnsureCharacterPreviewCacheAssetReady(const FCharacterSelectPreviewAppearance& Appearance, const FPlayerCharacterPreviewDefinition* Definition, const FString& AppearanceSignature, const FString& CharacterPreviewStem, const FString& CharacterPreviewPackageRoot, const FString& Reason);
	void CollectCharacterPreviewAsyncAssets(const FCharacterSelectPreviewAppearance& Appearance, TArray<FSoftObjectPath>& InOutAssetsToLoad) const;
	bool PrewarmCharacterVisualKit(const FCharacterSelectPreviewAppearance& Appearance);
	bool CommitPreparedCharacterVisualKit(const FString& RequestKey);
	void StartSelectedCharacterPreviewAsyncLoad(int32 RequestSerial, int32 CharacterGuid, const FString& RequestKey, const FCharacterSelectPreviewAppearance& Appearance, bool bRefreshOnComplete);
	bool BuildCharacterPreviewAppearanceFromRow(const TArray<FString>& Columns, FCharacterSelectPreviewAppearance& OutAppearance) const;
	bool WriteCharacterPreviewAppearanceDescriptorFromRow(const TArray<FString>& Columns, const FString& EquipmentSummary, FString& OutPath) const;
	void WriteCharacterPreviewBuildRequest(
		const FCharacterSelectPreviewAppearance& Appearance,
		const FPlayerCharacterPreviewDefinition* Definition,
		const FString& AppearanceSignature,
		const FString& CharacterPreviewStem,
		const FString& CharacterPreviewPackageRoot,
		const FString& Reason) const;
	void ReportCharacterPreviewBuildIssue(
		ECharacterPreviewBuildStatus Status,
		const FString& Stage,
		const FCharacterSelectPreviewAppearance& Appearance,
		const FPlayerCharacterPreviewDefinition* Definition,
		const FString& AppearanceSignature,
		const FString& CharacterPreviewStem,
		const FString& CharacterPreviewPackageRoot,
		const FString& Reason);
	void QueueCharacterPreviewBuild(
		const FCharacterSelectPreviewAppearance& Appearance,
		const FString& AppearanceSignature,
		const FString& Reason);
	bool StartCharacterPreviewBuildProcess(const FString& AppearanceSignature, const FString& GuidText, const FString& Reason);
	bool StartNextQueuedCharacterPreviewBuild();
	void ProcessCharacterPreviewBuildCoordinator();
	void ResetCharacterPreviewBuildProcess();
	bool IsCharacterPreviewBuildRequestComplete(const FString& AppearanceSignature) const;
	FString BuildCharacterPreviewAnimationOptionsCacheKey() const;
	void QueueCurrentCharacterPreviewAnimationOptionsPrewarm();
	void PrewarmCurrentCharacterPreviewAnimationOptions(const FString& ExpectedCacheKey);
	void ProcessCharacterPreviewAnimationCatalogBuild(float DeltaSeconds);
	void StartCharacterPreviewAnimationCatalogBuild(const FString& CacheKey);
	void ResetCharacterPreviewAnimationCatalogBuild();
	void UpdateCharacterPreviewAnimationCatalogSnapshot();
	FString FindCharacterPreviewAnimationCatalogDiskPath() const;
	bool TryLoadCharacterPreviewAnimationCatalog(const FString& CacheKey);
	bool TryGetResolvedCharacterPreviewAnimationPath(const FString& MeshObjectPath, const FString& CacheKey, int32 AnimationId, FString& OutAnimationObjectPath) const;
	void CacheResolvedCharacterPreviewAnimationPath(const FString& MeshObjectPath, const FString& CacheKey, int32 AnimationId, const FString& AnimationObjectPath);
	void CacheMissingResolvedCharacterPreviewAnimation(const FString& MeshObjectPath, const FString& CacheKey, int32 AnimationId);
	void RefreshSelectedCharacterPreview();
	bool IsGlueSceneVisualReadyForScreen(const FString& TargetScreen) const;
	bool IsSelectedCharacterPreviewVisualReady() const;
	bool IsSelectedCharacterPreviewPreparedReady() const;
	void StoreSelectedCharacterRuntimeAppearance(const FCharacterSelectPreviewAppearance& Appearance);
	bool ApplyCharacterPreviewAnimationOnly(const FString& AnimationObjectPath);
	bool ApplyCharacterPreviewAnimationOnlyById(int32 AnimationId);
	void PositionCharacterSelectPreviewActor();
	void DestroyCharacterSelectPreviewActor();
	void CancelPreparedCharacterSelectPreviewStaging(const TCHAR* Reason);
	void DestroyCharacterSelectPreviewAttachments();
	void QueueDeferredCharacterPreviewRuntimeMaterials(USkeletalMeshComponent* MeshComponent, const FPlayerCharacterPreviewDefinition& Definition, const FCharacterSelectPreviewAppearance& Appearance);
	void ProcessDeferredCharacterPreviewRuntimeMaterials(float DeltaSeconds);
	void ApplyCharacterPreviewAttachments(USkeletalMeshComponent* MeshComponent, const FCharacterSelectPreviewAppearance& Appearance);
	void ProcessDeferredCharacterPreviewM2Attachments(float DeltaSeconds);
	void TryRevealPreparedCharacterSelectPreview(const TCHAR* Reason);
	void ApplyCharacterPreviewSectionVisibility(USkeletalMeshComponent* MeshComponent, const FPlayerCharacterPreviewDefinition& Definition, const FCharacterSelectPreviewAppearance& Appearance);
	void RefreshGlueCharacterList();
	void SetGlueCharacterListFromSummaries(const TArray<FWoWGlueCharacterSummary>& Characters, bool bQueueResourceWork);
	void ReconcileGlueCharacterCacheForAccount(const TArray<FWoWGlueCharacterSummary>& Characters);
	void RefreshGlueRealmList();
	void SetGlueRealmListFromSummaries(const TArray<FWoWGlueRealmSummary>& Realms);
	void PushGlueStateToLua();
	void QueueGlueScreenTransition(const FString& TargetScreen, float DelaySeconds);
	void QueueGlueStatusThenScreen(const FString& StatusText, const FString& ButtonText, const FString& TargetScreen, float DelaySeconds);
	void ProcessPendingGlueTransition(float DeltaSeconds);
	void StartCommandLineGlueAutoLoginIfRequested();
	void ProcessCommandLineGlueAutoCycle(float DeltaSeconds);
	void ProcessCommandLineGlueAutoExit(float DeltaSeconds);
	void CallGlueGlobal(const FString& FunctionName, const TArray<FString>& Args = TArray<FString>());
	FString GetGlueString(const FString& Name, const FString& Fallback) const;
	FString GetGlueLoginFailureText(const FString& ErrorText) const;

	static constexpr int32 MaxGlueDebugLogLines = 200;
	TArray<FLoginSceneDefinition> LoginSceneDefinitions;
	TMap<int64, FPlayerCharacterPreviewDefinition> PlayerCharacterPreviewDefinitions;
	TMap<int32, FLoginSceneRuntimeCache> LoginSceneRuntimeCaches;
	int32 ActiveLoginSceneIndex = 0;
	TWeakObjectPtr<ASkeletalMeshActor> LoginSceneActor;
	TWeakObjectPtr<ASkeletalMeshActor> CharacterSelectPreviewActor;
	TWeakObjectPtr<ASkeletalMeshActor> CharacterSelectPreviewPreviousActorDuringStaging;
	TArray<TWeakObjectPtr<USkeletalMeshComponent>> CharacterSelectPreviewAttachmentComponents;
	TArray<TWeakObjectPtr<UWoWM2ModelComponent>> CharacterSelectPreviewM2AttachmentComponents;
	FString PendingCharacterPreviewRuntimeMaterialKey;
	struct FPendingCharacterPreviewRuntimeMaterialTask
	{
		int32 Guid = 0;
		FString MaterialKey;
		FCharacterSelectPreviewAppearance Appearance;
		FString SectionMetadataPath;
		int32 NextSectionIndex = 0;
		bool bInitialized = false;
		int32 BodyApplied = 0;
		int32 FaceApplied = 0;
		int32 HairApplied = 0;
		int32 FurApplied = 0;
		int32 MissingCount = 0;
		int32 ComposedItemLayerCount = 0;
		int32 MissingItemLayerCount = 0;
		TObjectPtr<UTexture2D> ComposedBodyAtlas = nullptr;
		TArray<TSharedPtr<FJsonValue>> Sections;
		TMap<FString, TObjectPtr<UTexture2D>> TextureCache;
		double StartSeconds = 0.0;
	};
	FPendingCharacterPreviewRuntimeMaterialTask PendingCharacterPreviewRuntimeMaterialTask;
	FString CharacterSelectPreviewAttachmentKey;
	FString PendingCharacterSelectPreviewM2AttachmentKey;
	struct FPendingCharacterPreviewM2Attachment
	{
		int32 Guid = 0;
		FString AttachmentKey;
		FString Name;
		int32 Slot = INDEX_NONE;
		int32 DisplayId = 0;
		FName SocketName;
		FString MeshPath;
		FString MeshJsonPath;
		FString AnimationPath;
		FString DescriptorPath;
		FVector RelativeLocation = FVector::ZeroVector;
		FRotator RelativeRotation = FRotator::ZeroRotator;
		FVector RelativeScale = FVector::OneVector;
		bool bUseM2Runtime = false;
	};
	TArray<FPendingCharacterPreviewM2Attachment> PendingCharacterSelectPreviewM2Attachments;
	FString PendingCharacterSelectPreviewM2AttachmentDescriptorPath;
	int32 PendingCharacterSelectPreviewM2AttachmentDefinitionCount = 0;
	int32 PendingCharacterSelectPreviewM2AttachmentAttachedCount = 0;
	double PendingCharacterSelectPreviewM2AttachmentStartSeconds = 0.0;
	int32 PendingCharacterSelectPreviewRevealGuid = 0;
	FString PendingCharacterSelectPreviewRevealAppearanceKey;
	bool bPreparingCharacterSelectPreviewStaging = false;
	FCharacterSelectPreviewDebugState CharacterSelectPreviewDebugState;
	mutable FString CachedCharacterPreviewAnimationOptionsKey;
	mutable TArray<FCharacterPreviewAnimationOption> CachedCharacterPreviewAnimationOptions;
	mutable TArray<FString> CachedCharacterPreviewAnimationPaths;
	mutable TMap<int32, FString> CachedCharacterPreviewAnimationPathById;
	mutable TArray<int32> CachedCharacterPreviewImportedAnimationIds;
	mutable FCharacterPreviewAnimationCatalogState CachedCharacterPreviewAnimationCatalogState;
	TMap<FString, FString> CachedCharacterPreviewResolvedAnimationPaths;
	TSet<FString> MissingCharacterPreviewResolvedAnimationKeys;
	FString PendingCharacterPreviewAnimationOptionsPrewarmKey;
	FTimerHandle CharacterPreviewAnimationOptionsPrewarmTimerHandle;
	int32 CharacterPreviewAnimationCatalogRequestSerial = 0;
	int32 ActiveCharacterPreviewAnimationCatalogBuildSerial = 0;
	bool bCharacterPreviewAnimationCatalogBuildActive = false;
	FString ActiveCharacterPreviewAnimationCatalogBuildKey;
	FString ActiveCharacterPreviewAnimationCatalogBuildMeshObjectPath;
	TObjectPtr<USkeletalMesh> ActiveCharacterPreviewAnimationCatalogBuildMesh = nullptr;
	TArray<FString> ActiveCharacterPreviewAnimationCatalogSearchPaths;
	int32 ActiveCharacterPreviewAnimationCatalogNextSearchPathIndex = 0;
	int32 ActiveCharacterPreviewAnimationCatalogNextResolveIndex = 0;
	FString ActiveCharacterPreviewAnimationCatalogCandidateKey;
	TArray<FString> ActiveCharacterPreviewAnimationCatalogCandidatePaths;
	TMap<int32, FString> ActiveCharacterPreviewAnimationCatalogPathById;
	TArray<int32> ActiveCharacterPreviewAnimationCatalogImportedAnimationIds;
	TArray<FCharacterPreviewAnimationOption> ActiveCharacterPreviewAnimationCatalogResolvedOptions;
	FProcHandle ActiveCharacterPreviewBuildProcess;
	uint32 ActiveCharacterPreviewBuildProcessId = 0;
	FString ActiveCharacterPreviewBuildSignature;
	FString ActiveCharacterPreviewBuildGuid;
	FString ActiveCharacterPreviewBuildReason;
	double ActiveCharacterPreviewBuildStartSeconds = 0.0;
	FTimerHandle SelectedCharacterPreviewRefreshTimerHandle;
	int32 PendingSelectedCharacterPreviewRefreshGuid = 0;
	int32 CharacterPreviewAsyncRequestSerial = 0;
	FString ActiveCharacterPreviewLoadKey;
	TSharedPtr<FStreamableHandle> ActiveCharacterPreviewStreamableHandle;
	FString PreparedCharacterPreviewLoadKey;
	FString CommittedCharacterPreviewLoadKey;
	bool bPendingGlueWaitingForCharacterPreview = false;
	FString PendingGlueCharacterPreviewScreen;
	double PendingGlueCharacterPreviewWaitStartSeconds = 0.0;
	int32 LoginSceneAsyncRequestSerial = 0;
	int32 ActiveLoginSceneAsyncTargetIndex = INDEX_NONE;
	TSharedPtr<FStreamableHandle> ActiveLoginSceneStreamableHandle;
	TSet<FString> QueuedCharacterPreviewBuildSignatures;
	TMap<FString, FString> QueuedCharacterPreviewBuildReasons;
	TMap<FString, FString> QueuedCharacterPreviewBuildGuids;
	TMap<FString, FString> QueuedCharacterPreviewBuildNames;
	TSet<FString> FailedCharacterPreviewBuildSignatures;
	TWeakObjectPtr<USkeletalMeshComponent> LoginSceneLateAdditiveOverlayComponent;
	TWeakObjectPtr<USkeletalMeshComponent> LoginSceneDebugHighlightComponent;
	TWeakObjectPtr<ACameraActor> LoginCameraActor;
	TWeakObjectPtr<USkyLightComponent> LoginSceneAmbientLightComponent;
	TSharedPtr<SWidget> LoginOverlayWidget;
	TSharedPtr<SWidget> LoginDebugPanelWidget;
	TSharedPtr<SWidget> LoginGlueErrorWidget;
	TSharedPtr<SWoWGlueRoot> LoginGlueRootWidget;
	TSharedPtr<FWoWGlueUIManager> LoginGlueUI;
	TSharedPtr<FWoWGlueLuaRuntime> LoginGlueLuaRuntime;
	FString LoginGlueLastError;
	TArray<FString> GlueDebugLogLines;
	bool bCharacterSelectPreviewDragging = false;
	float CharacterSelectPreviewUserYawOffset = 0.0f;
	int32 GlueLoginRequestSerial = 0;
	int32 GlueRealmRequestSerial = 0;
	TSharedPtr<TQueue<FGlueLoginAsyncResult, EQueueMode::Mpsc>, ESPMode::ThreadSafe> GlueLoginResultQueue;
	TSharedPtr<TQueue<FGlueRealmAsyncResult, EQueueMode::Mpsc>, ESPMode::ThreadSafe> GlueRealmResultQueue;
	TObjectPtr<UWoWLoginSceneTuningAsset> LoginSceneTuningAsset = nullptr;
	TObjectPtr<UWoWLoginScenePresetAsset> LoginScenePresetAsset = nullptr;
	bool bBypassLoginScenePresetCacheForBake = false;
	FWoWLoginSceneTuningState LoginSceneTuningState;
	int32 BaseCameraId = INDEX_NONE;
	FVector BaseCameraPosition = FVector::ZeroVector;
	FVector BaseCameraTarget = FVector::ZeroVector;
	float BaseCameraFovDegrees = 0.0f;
	float BaseCameraProjectionFovDegrees = 0.0f;
	float BaseCameraNearClip = 0.0f;
	float BaseCameraFarClip = 0.0f;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> LoginSceneDynamicMaterials;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> LoginSceneLateAdditiveOverlayDynamicMaterials;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UMaterialInstanceDynamic>> LoginSceneDebugHighlightDynamicMaterials;

	TArray<FSectionTextureTransformTrack> TextureTransformTracks;
	TArray<FSectionTextureTransformTrack> RenderSections;
	TArray<FSectionColorTrack> ColorTracks;
	TArray<int32> LoginSceneLateAdditiveOverlaySlots;
	TArray<FCameraVectorSample> CameraPositionSamples;
	TArray<FCameraVectorSample> CameraTargetSamples;
	TArray<FCameraRollSample> CameraRollSamples;
	TArray<FVector> BonePivots;
	TArray<FLoginBoneInfo> LoginBones;
	TMap<int32, TArray<FLoginBoneFrameSample>> BoneAnimationSamples;
	TArray<FLoginSceneLight> LoginSceneLights;
	TArray<FLoginSceneLightComponent> LoginSceneLightComponents;
	TArray<FLoginParticleEmitter> ParticleEmitters;
	TArray<FString> CachedGlueRealmRows;
	TArray<FLoginParticleSystem> ParticleSystems;
	TArray<FLoginParticleSystem> CharacterSelectPreviewAttachmentParticleSystems;
	int32 HighlightedTextureSectionIndex = INDEX_NONE;
	FString LoggedInAccountName;
	int32 LoggedInAccountId = 0;
	TArray<FString> CachedGlueCharacterRows;
	int32 SelectedGlueCharacterGuid = 0;
	FString SelectedGlueCharacterModelKey;
	FString SelectedGlueCharacterAnimationKey;
	FString SelectedGlueCharacterAppearanceKey;
	TMap<TWeakObjectPtr<USkeletalMeshComponent>, FString> CharacterSelectPreviewAttachmentAnimationKeys;
	TSet<FString> MissingCharacterSelectPreviewAnimationPaths;
	TArray<FString> PendingCharacterPreviewPrewarmRows;
	TSet<int32> PrewarmedCharacterPreviewGuids;
	int32 CharacterPreviewPrewarmRequestSerial = 0;
	FString ActiveCharacterPreviewPrewarmKey;
	TSharedPtr<FStreamableHandle> ActiveCharacterPreviewPrewarmStreamableHandle;
	TArray<TSharedPtr<FStreamableHandle>> RetainedCharacterPreviewPrewarmHandles;
	TArray<int32> PendingLoginSceneRuntimeCachePrewarmIndices;
	int32 LoginScreenSceneIndex = INDEX_NONE;
	int32 LastCreatedRace = 1;
	int32 LastCreatedClass = 1;
	int32 LastCreatedGender = 0;
	FString PendingGlueScreen;
	float PendingGlueScreenDelaySeconds = 0.0f;
	bool bPendingGlueCloseStatusDialog = false;
	bool bPendingGlueWaitingForScene = false;
	FString PendingGlueSceneScreen;
	double PendingGlueSceneWaitStartSeconds = 0.0;
	bool bCommandLineGlueAutoLoginStarted = false;
	bool bCommandLineGlueAutoCycleRequested = false;
	bool bCommandLineGlueAutoCycleActive = false;
	bool bCommandLineGlueAutoCycleCompleted = false;
	int32 CommandLineGlueAutoCycleRemaining = 0;
	int32 CommandLineGlueAutoCycleNextIndex = 0;
	float CommandLineGlueAutoCycleDelaySeconds = 0.35f;
	float CommandLineGlueAutoCycleElapsedSeconds = 0.0f;
	bool bCommandLineGlueAutoExitRequested = false;
	float CommandLineGlueAutoExitDelaySeconds = 0.0f;
	float CommandLineGlueAutoExitElapsedSeconds = 0.0f;

	float CameraTrackDurationMs = 0.0f;
	float LoginSceneVisualUpdateAccumulatorSeconds = 0.0f;
	float LoginParticleUpdateAccumulatorSeconds = 0.0f;
};
