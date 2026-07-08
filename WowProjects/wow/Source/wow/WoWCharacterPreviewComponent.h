// WoWCharacterPreviewComponent.h
// 用于在UE中实现魔兽世界风格的角色预览组件，包含程序化网格蒙皮、动画播放与混合等功能。

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "ProceduralMeshComponent.h"
#include "WoWCharacterPreviewComponent.generated.h"

class ACharacter;
class UAnimSequence;
class UMaterialInterface;
class UWoWM2ModelComponent;
class USkeletalMeshComponent;
class UWoWM2PreviewAnimInstance;
struct FWoWSelectedCharacterRuntimeAppearance;

struct FWoWTextureTransformSample
{
	float TimeSeconds = 0.0f;
	FVector2D Translation = FVector2D::ZeroVector;
	float RotationDegrees = 0.0f;
	FVector2D Scaling = FVector2D(1.0f, 1.0f);
};

struct FWoWSectionColorSample
{
	float TimeSeconds = 0.0f;
	FLinearColor Color = FLinearColor::White;
};

struct FWoWAnimationDataRecord
{
	int32 Id = INDEX_NONE;
	FString Name;
	int32 WeaponFlags = 0;
	int32 BodyFlags = 0;
	int32 Flags = 0;
	int32 Fallback = 0;
	int32 BehaviorId = INDEX_NONE;
	int32 BehaviorTier = 0;
};

/**
 * 单个网格体分段（Section）的蒙皮状态数据
 * 存储从原始静态顶点到当前动画帧蒙皮后顶点所需的所有信息
 */
struct FWoWCharacterPreviewSectionState
{
	// 基础顶点位置（模型空间，未蒙皮）
	TArray<FVector> BasePositions;
	// 四骨骼影响权重（每个顶点最多4根骨骼，权重为0-255整数，需除以255）
	TArray<FIntVector4> BoneWeights;
	// 影响该顶点的骨骼索引（最多4个，对应BoneWeights）
	TArray<FIntVector4> BoneIndices;
	// 基础法线（模型空间）
	TArray<FVector> Normals;
	// UV坐标通道0
	TArray<FVector2D> UV0;
	// 原始UV坐标；M2 TextureTransform 会在运行时基于它重新计算 UV0，不能累积修改。
	TArray<FVector2D> BaseUV0;
	// 顶点颜色
	TArray<FLinearColor> VertexColors;
	// 切线（用于法线贴图计算）
	TArray<FProcMeshTangent> Tangents;
	// 当前帧蒙皮后的顶点位置（世界/组件空间）
	TArray<FVector> SkinnedPositions;
	// M2 TextureTransform 采样曲线；用于还原 UV 滚动/旋转/缩放动画。
	TArray<FWoWTextureTransformSample> TextureTransformSamples;
	// M2 Color/Transparency 采样曲线；用于还原发光层透明度/颜色动画，不能只烘第0帧。
	TArray<FWoWSectionColorSample> ColorSamples;
	// RenderMode=4 的 additive-alpha 弱光层在 UE 中需要稍微压低低 alpha，避免显示成硬白条。
	bool bScaleLowAlphaAdditive = false;
	// 用于动画混合的上一个姿势的蒙皮位置（当前帧与上一帧之间的过渡）
	TArray<FVector> PreviousBlendPositions;
};

/**
 * 一个动画剪辑的数据结构
 * 包含动画的帧骨骼矩阵、播放参数及循环/过渡信息
 */
struct FWoWCharacterPreviewAnimationClip
{
	// 动画键名（标识用）
	FString Key;
	// 序列索引
	int32 SequenceIndex = -1;
	// 参与该动画的骨骼查找ID列表（用于动画混合层选择）
	TArray<int32> LookupIds;
	// 动画ID（对应游戏内的动画枚举值）
	int32 AnimationId = -1;
	// 子动画ID（同一动画的不同变体）
	int32 SubAnimationId = -1;
	// 动画总时长（秒）
	float LengthSeconds = 0.0f;
	// 移动速度系数（用于与角色移动速度匹配）
	float MovingSpeed = 0.0f;
	// 混合时间（从上一个动画过渡到该动画的秒数）
	float BlendTimeSeconds = 0.0f;
	// 播放频率（可能用于随机选取等）
	int32 Frequency = 0;
	// 下一个动画索引（-1表示不自动连接）
	int32 NextAnimation = -1;
	// 别名下一个动画（用于不同动画路径）
	int32 AliasNext = 0;
	// 采样率（每秒帧数，用于计算帧索引）
	float SampleRate = 20.0f;
	// 是否为外部动画（如战斗待机等特殊动画）
	bool bExternalAnim = false;
	// 导出器是否额外写入了首帧副本作为循环闭合帧；由 WoW 持续态动画ID决定，不能用 M2Sequence.flags 推断。
	bool bLoopEndFrame = false;
	// 动画帧数据，外层数组为帧，内层为骨骼变换矩阵列表（行优先，对应骨骼索引）
	TArray<TArray<FMatrix>> Frames;
};

/**
 * M2 模型挂点数据。角色/生物的武器、盾牌、背部、坐骑等都应优先走 attachment 表，
 * 不要把 KeyBoneLookup 的语义编号误当作装备挂点编号。
 */
struct FWoWCharacterAttachment
{
	// attachment 表中的顺序索引
	int32 Index = INDEX_NONE;
	// POSITION_SLOTS / ATT_* 语义ID，例如 1=右手掌，2=左手掌
	int32 Id = INDEX_NONE;
	// 此挂点依附的 M2 骨骼索引
	int32 BoneIndex = INDEX_NONE;
	// 相对该骨骼的本地位置，单位和导出后的UE预览模型一致
	FVector Position = FVector::ZeroVector;
};

struct FWoWM2EventTimestampTrack
{
	int32 SequenceIndex = INDEX_NONE;
	int32 AnimationId = INDEX_NONE;
	int32 SubAnimationId = INDEX_NONE;
	TArray<int32> TimestampsMs;
};

struct FWoWM2EventRecord
{
	int32 Index = INDEX_NONE;
	FString Identifier;
	int32 BoneIndex = INDEX_NONE;
	TArray<FWoWM2EventTimestampTrack> TimestampTracks;
};

struct FWoWBoneMetadata
{
	int32 Index = INDEX_NONE;
	int32 Flags = 0;
	int32 Parent = INDEX_NONE;
	FVector Pivot = FVector::ZeroVector;
};

struct FWoWRibbonSample
{
	float TimeSeconds = 0.0f;
	FLinearColor Color = FLinearColor::White;
	float Above = 0.0f;
	float Below = 0.0f;
};

struct FWoWRibbonTexSlotSample
{
	float TimeSeconds = 0.0f;
	int32 Slot = 0;
};

struct FWoWRibbonVisibilitySample
{
	float TimeSeconds = 0.0f;
	bool bVisible = true;
};

struct FWoWSheathedWeaponDebugState
{
	int32 AttachmentId = INDEX_NONE;
	FString AttachmentName;
	int32 RotationIndex = 0;
	FRotator BaseRotation = FRotator::ZeroRotator;
	FVector LocationOffset = FVector::ZeroVector;
	FRotator RotationOffset = FRotator::ZeroRotator;
	FString ExportText;
};

struct FWoWRibbonEmitter
{
	int32 Index = INDEX_NONE;
	int32 Id = INDEX_NONE;
	int32 BoneIndex = INDEX_NONE;
	FVector Position = FVector::ZeroVector;
	FString PreviewPng;
	// M2 Ribbon.MaterialIndices 指向模型全局 Materials[]；丝带混合模式/双面/深度写入必须从这里继承，不能在 UE 侧猜。
	TArray<int32> MaterialIndices;
	// M2 Ribbon: EdgesPerSec，每秒生成多少条边/段，控制丝带细腻度。
	float EdgesPerSecond = 1.0f;
	// M2 Ribbon: EdgeLifeSpanInSec，每条边保留多少秒；不能换算成空间长度，否则跑动会把中心能量拖成超长线。
	float EdgeLifetimeSeconds = 1.0f;
	float Gravity = 0.0f;
	int32 TextureRows = 1;
	int32 TextureCols = 1;
	float LengthSeconds = 1.0f;
	TArray<FWoWRibbonSample> Samples;
	TArray<FWoWRibbonTexSlotSample> TexSlotSamples;
	TArray<FWoWRibbonVisibilitySample> VisibilitySamples;
};

struct FWoWRibbonSegment
{
	// 世界空间历史点；WoW Ribbon 的拖尾必须留下在世界里，不能跟随武器或角色组件整体移动。
	FVector Position = FVector::ZeroVector;
	// 世界空间宽度方向。
	FVector Up = FVector::UpVector;
	// 世界空间尾段补点方向，对应 WMV RibbonEmitter::draw 里的 last segment back。
	FVector Back = FVector::ZeroVector;
	float Length = 0.0f;
	float OriginalLength = 1.0f;
	float AgeSeconds = 0.0f;
};

struct FWoWRibbonRuntimeState
{
	FWoWRibbonEmitter Emitter;
	TArray<FWoWRibbonSegment> Segments;
	TObjectPtr<UMaterialInterface> Material = nullptr;
	// 世界空间上一帧发射点。
	FVector LastPosition = FVector::ZeroVector;
	float TimeSinceLastEdgeSeconds = 0.0f;
	bool bHasLastPosition = false;
};

struct FWoWRuntimeCharacterAttachmentDefinition
{
	FString Name;

	FString ComponentType;

	int32 Slot = INDEX_NONE;

	int32 DisplayId = 0;

	int32 InventoryType = 0;

	FName SocketName;

	FString MeshJsonPath;

	FVector RelativeLocation = FVector::ZeroVector;

	FRotator RelativeRotation = FRotator::ZeroRotator;

	FVector RelativeScale = FVector::OneVector;
};

/**
 * 魔兽世界角色预览组件
 * 通过程序化网格体动态蒙皮并播放M2模型动画，支持移动、攻击、跳跃等状态下的动画选择与混合
 */
UCLASS(ClassGroup = (WoW), meta = (BlueprintSpawnableComponent))
class WOW_API UWoWCharacterPreviewComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UWoWCharacterPreviewComponent();

	// 组件激活时调用
	virtual void BeginPlay() override;
	// 每帧更新动画与渲染
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/**
	 * 将组件绑定到指定角色，提取其骨架网格体的几何数据并创建程序化网格体进行预览
	 * @param InCharacter 要预览的角色
	 */
	void InitializeForCharacter(ACharacter* InCharacter);
	void SetRuntimeAppearance(const FWoWSelectedCharacterRuntimeAppearance& InAppearance);
	
	/**
	 * 设置当前移动输入状态，用于驱动移动动画选择
	 * @param bInMoveForward 是否按下前进
	 * @param bInMoveBackward 是否按下后退
	 * @param bInStrafeLeft 是否按下左平移
	 * @param bInStrafeRight 是否按下右平移
	 * @param bInTurnLeft 是否按下左转
	 * @param bInTurnRight 是否按下右转
	 * @param bInMouseTurnLeft 鼠标左转
	 * @param bInMouseTurnRight 鼠标右转
	 * @param bInLeftMouseHeld 鼠标左键按住
	 * @param bInRightMouseHeld 鼠标右键按住
	 * @param InForwardInput 前进输入量（摇杆轴向）
	 * @param InStrafeInput 平移输入量
	 */
	void SetMovementInputState(bool bInMoveForward, bool bInMoveBackward, bool bInStrafeLeft, bool bInStrafeRight, bool bInTurnLeft, bool bInTurnRight, bool bInMouseTurnLeft, bool bInMouseTurnRight, bool bInLeftMouseHeld, bool bInRightMouseHeld, float InForwardInput = 0.0f, float InStrafeInput = 0.0f);
	
	/**
	 * 设置战斗待机状态
	 * @param bInCombatReady 是否进入战斗待机
	 * @param LingerSeconds 战斗状态保持时间（秒），到期后若没有攻击操作则退出
	 */
	void SetCombatReady(bool bInCombatReady, float LingerSeconds);
	
	// 触发一次攻击动画（一次性动画，优先级高于移动）
	void TriggerAttack();
	// 触发跳跃开始动画
	void TriggerJumpStart();
	void SetJumpStartedWithGroundMovement(bool bInJumpStartedWithGroundMovement);
	// X键坐下状态：进入时播放SitGroundDown，保持SitGround，再按X播放SitGroundUp。
	void SetSitting(bool bInSitting, bool bPlayTransition = true);
	bool IsSitting() const;
	bool IsSittingTransitionActive() const;
	// Z键正式收/出鞘状态：不同于 B/C/V/N/M 的调试预览，后续应由装备槽和 Item.dbc SheatheType 驱动。
	void ToggleWeaponSheathed();
	void SetWeaponSheathed(bool bInSheathed, bool bPlayTransition = true);
	bool IsWeaponSheathed() const;
	
	// 调试：切换到上一个动画
	void DebugPreviousAnimation();
	// 调试：切换到下一个动画
	void DebugNextAnimation();
	// 调试：清除调试动画，恢复自动选择
	void DebugClearAnimation();
	// 调试：临时切换预览装备槽，用于在没有背包系统时快速验证左右手挂点和握持层。
	void DebugEquipWeaponRightHand();
	void DebugEquipWeaponLeftHand();
	void DebugUnequipWeapon();
	void DebugCycleSheathedAttachment();
	void DebugCycleSheathedRotation();
	void DebugCycleSheathedNudgeMode();
	void DebugAdjustSheathedNudge(float Direction);
	FWoWSheathedWeaponDebugState GetSheathedWeaponDebugState() const;
	void SetDebugSheathedLocationOffset(const FVector& InOffset);
	void SetDebugSheathedRotationOffset(const FRotator& InOffset);
	void SetDebugWeaponSheathedPreview(bool bInPreview);
	void ResetDebugSheathedOffsetsToDefault();

	UPROPERTY(EditAnywhere, Category = "WoW|Equipment Preview")
	bool bEnableDefaultPreviewEquipment = true;

	UPROPERTY(EditAnywhere, Category = "WoW|Equipment Preview", meta = (EditCondition = "bEnableDefaultPreviewEquipment"))
	int32 PreviewMainHandItemDisplayId = 0;

	UPROPERTY()
	int32 DebugAttachedWeaponDisplayId_DEPRECATED = 0;

private:
	FString RuntimeAppearanceKey;
	FString RuntimeEquipmentSummary;
	FString GeneratedCharacterMeshJsonPath;
	FString GeneratedCharacterMeshPath;
	FString GeneratedCharacterMaterialPackagePath;
	FString GeneratedCharacterAnimationPackagePath;
	FString GeneratedCharacterAnimationStem;
	FString RuntimeSectionMetadataPath;
	FString RuntimeAttachmentDescriptorPath;
	FString AppliedGeneratedMaterialKey;
	FString AppliedRuntimeBodyTextureKey;
	TArray<FWoWRuntimeCharacterAttachmentDefinition> RuntimeAttachmentDefinitions;
	UPROPERTY()
	TArray<TObjectPtr<UWoWM2ModelComponent>> RuntimeEquipmentAttachmentComponents;
	int32 RuntimeRace = 0;
	int32 RuntimeClassId = 0;
	int32 RuntimeGuid = 0;
	int32 RuntimeGender = 0;
	int32 RuntimeSkin = 0;
	int32 RuntimeFace = 0;
	int32 RuntimeHairStyle = 0;
	int32 RuntimeHairColor = 0;
	int32 RuntimeFacialStyle = 0;
	bool bHasRuntimeAppearance = false;
	bool bUsingBakedRuntimeAppearanceAsset = false;

	/**
	 * 从角色当前骨骼网格体中提取几何信息，创建并配置程序化网格体组件用于预览
	 * @param Character 目标角色
	 */
	void ApplyM2PreviewMeshToCharacter(ACharacter* Character);
	void ApplyGeneratedSkeletalPreviewToCharacter(ACharacter* Character);
	void ApplyGeneratedRuntimeAppearanceIfNeeded(USkeletalMeshComponent* SkeletalPreview);
	void ApplyRuntimeGeosetVisibility();
	void ApplyPreviewEquipmentToCharacter(ACharacter* Character);
	void DestroyRuntimeEquipmentAttachments();
	bool LoadRuntimeAttachmentDefinitions();
	int32 ResolvePreviewMainHandDisplayId() const;
	UAnimSequence* LoadGeneratedCharacterAnimation(int32 AnimationId, int32 SubAnimationId, int32 GripVariant = 0) const;
	void SetDebugAttachedWeaponSlot(int32 AttachmentId);
	void CycleDebugSheathedAttachment();
	void CycleDebugSheathedRotation();
	void CycleDebugSheathedNudgeMode();
	void AdjustDebugSheathedNudge(float Direction);
	void ShowDebugSheathedMessage() const;
	void LoadAnimationData();
	const FWoWAnimationDataRecord* FindAnimationDataById(int32 AnimationId) const;
	const FWoWAnimationDataRecord* FindAnimationDataByName(const FString& AnimationName) const;
	int32 ResolveAnimationDataIdByName(const FString& AnimationName, int32 FallbackAnimationId) const;
	int32 GetWeaponSheatheAnimationId() const;
	float GetWeaponSheatheAttachmentSwitchSeconds(int32 AnimationId, int32 SubAnimationId, float FallbackDurationSeconds) const;
	void UpdateCurrentBoneMatrices(const FWoWCharacterPreviewAnimationClip& Clip, int32 FrameIndex, int32 NextFrameIndex, float FrameAlpha);
	void UpdateAttachedWeaponTransform();
	void UpdateAttachedWeaponTransform(const FWoWCharacterPreviewAnimationClip* LandingOverlayClip, float LandingOverlayTimeSeconds, float LandingOverlayAlpha);
	bool TryUpdateAttachedWeaponTransformFromGeneratedSkeleton(const FWoWCharacterAttachment& Attachment);
	FName MakeGeneratedAttachmentSocketName(const FWoWCharacterAttachment& Attachment) const;
	bool IsWeaponVisuallySheathed() const;
	bool IsDebugSheathedTransformActive() const;
	int32 ResolveVisualWeaponAttachmentId() const;
	void UpdateAttachedM2Animation(float DeltaTime);
	void ApplyAttachedM2BillboardBones(TArray<FMatrix>& InOutBoneMatrices) const;
	void UpdateAttachedM2Ribbons(float DeltaTime);
	int32 ResolveAttachedM2AnimationClipIndex() const;
	FWoWRibbonSample SampleRibbonEmitter(const FWoWRibbonEmitter& Emitter, float TimeSeconds) const;
	int32 SampleRibbonTexSlot(const FWoWRibbonEmitter& Emitter, float TimeSeconds) const;
	bool SampleRibbonVisibility(const FWoWRibbonEmitter& Emitter, float TimeSeconds) const;
	bool ResolveAttachmentById(int32 AttachmentId, FWoWCharacterAttachment& OutAttachment) const;
	
	/**
	 * 每帧更新动画进度，计算当前帧骨骼矩阵，执行蒙皮并更新网格顶点
	 * @param DeltaTime 帧间隔时间
	 */
	void UpdateAnimation(float DeltaTime);
	void UpdateGeneratedSkeletalPreviewAnimation(int32 ClipIndex, float PlaybackRate, bool bLooping);
	
	/**
	 * 根据当前输入状态、战斗状态等解析出应该播放的动画剪辑索引
	 * @return 动画剪辑在AnimationClips数组中的索引，未找到返回INDEX_NONE
	 */
	int32 ResolveAnimationClipIndex() const;
	int32 ResolveStrafeOverlayClipIndex() const;
	int32 ResolveReferenceStandClipIndex() const;
	int32 ResolveKeyboardTurnShuffleDirection() const;
	
	/**
	 * 根据动画ID和子ID查找对应的动画剪辑索引
	 * @param AnimationId 动画ID
	 * @param SubAnimationId 子动画ID
	 * @return 索引，未找到返回INDEX_NONE
	 */
	int32 FindAnimationClipIndex(int32 AnimationId, int32 SubAnimationId = 0) const;
	
	/**
	 * 检查一个动画剪辑是否匹配给定的动画ID（忽略子ID）
	 * @param Clip 要检查的剪辑
	 * @param AnimationId 要匹配的ID
	 * @return 是否匹配
	 */
	bool IsClipForAnimationId(const FWoWCharacterPreviewAnimationClip& Clip, int32 AnimationId) const;
	// 只判断会产生位移的输入；原地转身/鼠标看向不应缩短站立落地动作。
	bool HasActiveMovementInput() const;
	void StartMovingLandingOverlay(float InitialTimeSeconds = 0.0f);
	
	/**
	 * 启动一次性动画（如攻击、跳跃），播放完后自动回退到移动动画
	 * @param AnimationId 动画ID
	 * @param SubAnimationId 子动画ID
	 */
	void StartOneShotAnimation(int32 AnimationId, int32 SubAnimationId, float MaxDurationSeconds = 0.0f);
	
	/**
	 * 获取动画播放速率，用于匹配不同移动速度
	 * @param Clip 目标动画剪辑
	 * @return 播放速率系数
	 */
	float GetAnimationPlaybackRate(const FWoWCharacterPreviewAnimationClip& Clip) const;
	
	// 从配置数据中加载移动姿态混合相关的参数（如各部位旋转角度、插值速度等）
	void LoadLocomotionTuning();
	
	/**
	 * 更新视觉朝向偏移（用于平滑模拟角色转身）
	 * @param DeltaTime 帧间隔
	 */
	void UpdateVisualMovementFacing(float DeltaTime);
	
	// 重建移动姿态混合层所需的骨骼集合（躯干、头部）
	void RebuildLocomotionPoseLayers();
	// 重建装备握持层所需的手指/手掌骨骼集合。
	void RebuildWeaponGripPoseLayer();
	void RebuildWeaponSheatheUpperBodyPoseLayer();
	
	/**
	 * 从指定根骨骼向下递归，将其所有子骨骼索引添加到集合中（包括根骨骼自身）
	 * @param RootBoneIndex 根骨骼索引
	 * @param OutBoneSet 输出骨骼索引集合
	 */
	void AddBoneSubtreeToSet(int32 RootBoneIndex, TSet<int32>& OutBoneSet) const;
	
	/**
	 * 根据顶点骨骼权重和骨骼索引，计算该顶点属于某骨骼集合的影响程度（权重和）
	 * @param Weights 骨骼权重（0-255）
	 * @param BoneIndices 骨骼索引
	 * @param BoneSet 目标骨骼集合
	 * @return 影响度（0.0-1.0），所有权重为0-255值之和再除以255归一化
	 */
	float GetBoneSetInfluence(const FIntVector4& Weights, const FIntVector4& BoneIndices, const TSet<int32>& BoneSet) const;
	FVector SampleSkinnedPosition(
		const FWoWCharacterPreviewAnimationClip& Clip,
		int32 FrameIndex,
		int32 NextFrameIndex,
		float FrameAlpha,
		const FVector& BasePosition,
		const FIntVector4& Weights,
		const FIntVector4& BoneIndices,
		const FWoWCharacterPreviewAnimationClip* ReferenceStandClip = nullptr,
		const FWoWCharacterPreviewAnimationClip* WeaponGripClip = nullptr) const;
	FVector SampleSkinnedPositionAtTime(const FWoWCharacterPreviewAnimationClip& Clip, float TimeSeconds, const FVector& BasePosition, const FIntVector4& Weights, const FIntVector4& BoneIndices) const;
	FVector SampleAttachmentPointFromCurrentPose(const FWoWCharacterAttachment& Attachment, const FVector& LocalPoint, const FWoWCharacterPreviewAnimationClip* LandingOverlayClip, float LandingOverlayTimeSeconds, float LandingOverlayAlpha) const;
	
	/**
	 * 对蒙皮后的顶点位置施加移动姿态调整（如上半身反向旋转、头部倾斜等）
	 * @param SkinnedPosition 原始蒙皮位置
	 * @param Weights 顶点骨骼权重
	 * @param BoneIndices 骨骼索引
	 * @return 调整后的位置
	 */
	FVector ApplyLocomotionPoseToSkinnedPosition(const FVector& SkinnedPosition, const FIntVector4& Weights, const FIntVector4& BoneIndices) const;
	
	/**
	 * 对指定分段执行动画蒙皮计算，混合当前帧与下一帧
	 * @param Clip 动画剪辑
	 * @param FrameIndex 当前帧整数索引
	 * @param NextFrameIndex 下一帧整数索引
	 * @param FrameAlpha 帧间混合因子（0-1）
	 * @param SectionState 该分段的状态数据（输入基础数据，输出蒙皮后顶点）
	 */
	void SkinSection(
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
		FWoWCharacterPreviewSectionState& SectionState);

	// 预览的角色弱引用
	TWeakObjectPtr<ACharacter> PreviewCharacter;
	// 用于渲染的程序化网格体组件
	TWeakObjectPtr<UProceduralMeshComponent> PreviewMeshComponent;
	// UE原生骨骼网格预览组件；用于验证 M2 -> USkeletalMesh/UAnimSequence 的生产路径。
	TWeakObjectPtr<USkeletalMeshComponent> GeneratedSkeletalMeshComponent;
	// 原生骨骼预览当前播放的动画ID，用于避免每帧重复切换 UAnimSequence。
	int32 GeneratedSkeletalAnimationId = INDEX_NONE;
	int32 GeneratedSkeletalSubAnimationId = INDEX_NONE;
	int32 GeneratedSkeletalGripVariant = 0;
	// 原生 UE SkeletalMesh 是当前可见角色；这里记录单节点动画时间，避免武器还跟旧 CPU 采样相位走。
	float GeneratedSkeletalAnimationTimeSeconds = 0.0f;
	float RuntimeUpdateAccumulatorSeconds = 0.0f;
	// UE AnimSequence 运行时解析缓存；缺失资源也缓存，避免移动/收武器时每帧触发同步 LoadObject 查询。
	mutable TMap<FString, TWeakObjectPtr<UAnimSequence>> GeneratedAnimationCache;
	mutable TSet<FString> MissingGeneratedAnimationPaths;
	// 预览装备的程序化网格体组件；当前测试关卡使用同一套 Items/M2 runtime 验证装备挂点。
	TWeakObjectPtr<UProceduralMeshComponent> AttachedWeaponMeshComponent;
	// 附加M2的Ribbon预览组件；Ribbon是M2自己的发射器数据，不应伪造为普通材质动画。
	TWeakObjectPtr<UProceduralMeshComponent> AttachedRibbonMeshComponent;
	TWeakObjectPtr<UWoWM2ModelComponent> AttachedM2ModelComponent;
	// 附加M2自己的蒙皮状态；武器/物件如果带骨骼动画，必须播放自身M2动画，而不是只跟随角色手部。
	TArray<FWoWCharacterPreviewSectionState> AttachedM2SectionStates;
	// 附加M2自己的动画剪辑。此处保持通用M2路径，不写死为某一把武器。
	TArray<FWoWCharacterPreviewAnimationClip> AttachedM2AnimationClips;
	// 附加M2当前插值后的骨骼矩阵，供Ribbon/粒子/子挂点等特效使用。
	TArray<FMatrix> AttachedM2CurrentBoneMatrices;
	// 附加M2骨骼元数据；M2 flags 中的 billboard 骨骼必须运行时按相机朝向求矩阵。
	TArray<FWoWBoneMetadata> AttachedM2Bones;
	// 附加M2的Ribbon运行状态。
	TArray<FWoWRibbonRuntimeState> AttachedRibbonStates;
	// 附加M2动画播放时间。
	float AttachedM2AnimationTimeSeconds = 0.0f;
	// 当前附加M2动画剪辑索引。
	int32 AttachedM2CurrentAnimationClipIndex = INDEX_NONE;
	// WotLK AnimationData.dbc 导出的权威动画行为表；用于按动画名/行为记录选择动作，而不是在交互逻辑里散落裸 ID。
	TArray<FWoWAnimationDataRecord> AnimationDataRecords;
	TMap<int32, int32> AnimationDataIndexById;
	TMap<FString, int32> AnimationDataIndexByName;
	// 是否已完成预览网格初始化
	bool bPreviewMeshInitialized = false;
	
	// 所有网格分段的状态（每个Section一个元素）
	TArray<FWoWCharacterPreviewSectionState> SectionStates;
	// 源 M2 mesh JSON 中每个 section 对应的 submesh/geoset id；用于 UE SkeletalMesh LOD0 section 可见性，不重复解析顶点。
	TArray<int32> RuntimeSectionSubmeshIds;
	// 所有加载的动画剪辑列表
	TArray<FWoWCharacterPreviewAnimationClip> AnimationClips;
	// 动画查找表（可能用于动画ID到剪辑索引的快速映射）
	TArray<int32> AnimationLookup;
	// 关键骨骼查找表（用于动画层混合）
	TArray<int32> KeyBoneLookup;
	// 每个骨骼的父骨骼索引，用于构建骨骼层次
	TArray<int32> BoneParents;
	// 当前主动画帧插值后的骨骼矩阵，用于驱动武器/后续装备挂点。
	TArray<FMatrix> CurrentBoneMatrices;
	// M2 attachment 表，装备/特效挂点应从这里解析。
	TArray<FWoWCharacterAttachment> Attachments;
	// M2 attachment_lookup 表：下标是 ATT_* 语义ID，值是 Attachments 数组索引。
	TArray<int32> AttachmentLookup;
	// M2 events 表；$SHL/$SHR 等事件提供收/拔刀挂点切换的权威时间点。
	TArray<FWoWM2EventRecord> M2Events;
	
	// 移动姿态调整：躯干部分骨骼索引集合
	TSet<int32> LocomotionTorsoBoneIndices;
	// 移动姿态调整：头部骨骼索引集合
	TSet<int32> LocomotionHeadBoneIndices;
	// 横移覆盖层：下半身骨骼索引集合
	TSet<int32> LocomotionLowerBodyBoneIndices;
	// 装备武器时叠加 HandsClosed 的手指/手掌骨骼集合，避免只挂武器但手仍保持张开。
	TSet<int32> WeaponGripBoneIndices;
	// Z键收/拔刀的 UE 原生上半身遮罩。当前来自 M2 key bone 的左右肩/臂子树，不覆盖腿/骨盆。
	TArray<int32> WeaponSheatheUpperBodyBoneIndices;
	// 当前预览装备是否显示；正式游戏内装备系统接入后由装备槽状态驱动。
	bool bPreviewEquipmentVisible = true;
	// 当前预览主手挂载的 M2 attachment id，1=右手掌，2=左手掌。
	int32 ActiveWeaponAttachmentId = 1;
	// 当前预览武器先按主手收鞘处理。正式 Z/X 收鞘优先使用角色 M2 attachment_lookup[26]=AT_SheathMainHand。
	// 接入 Item.dbc SheatheType 后再按武器类型映射到 26/27/32/33/盾牌等具体收纳挂点。
	int32 ActiveWeaponSheathedAttachmentId = 26;
	// 背负武器的临时旋转候选索引；V 键调试时会显示 rotIndex，确认后可把默认索引固定到这里。
	// WMV 对背负武器会额外设置模型旋转，不能只换 attachment。
	int32 ActiveWeaponSheathedRotationIndex = 0;
	// 调试背负预览：C/V 只切换背部挂点/旋转，不改变 X 键控制的坐下状态。
	bool bDebugWeaponSheathedPreview = false;
	// 正式武器收/出鞘状态；Z 键切换。坐下会临时强制视觉收起，但不改这个持久状态。
	bool bWeaponSheathed = false;
	// Z键收/出鞘过渡期间，武器挂点要等手部动画到达事件点后才切换，避免按键瞬间穿帮。
	bool bWeaponSheatheTransitionActive = false;
	bool bWeaponSheatheTransitionStartVisualSheathed = false;
	int32 WeaponSheatheTransitionAnimationId = INDEX_NONE;
	float WeaponSheatheTransitionElapsedSeconds = 0.0f;
	float WeaponSheatheTransitionSwitchSeconds = 0.0f;
	float WeaponSheatheTransitionClipSeconds = 0.0f;
	float WeaponSheatheTransitionBlendOutSeconds = 0.0f;
	float WeaponSheatheTransitionDurationSeconds = 0.0f;
	// 背负武器人工微调：B 切换通道，N/M 减/加；F4 面板可用滑块调整。
	// 当前默认值来自运行时调参面板，针对测试双手剑背负位置，后续应由物品/模型配置表驱动。
	FVector DebugSheathedBackWeaponLocationOffset = FVector(9.088f, -7.008f, -50.697f);
	FRotator DebugSheathedBackWeaponRotationOffset = FRotator(26.138878f, 46.833817f, 171.520889f);
	int32 DebugSheathedNudgeMode = 0;
	
	// 模型包围盒最小值（用于视图）
	FVector PreviewBoundsMin = FVector::ZeroVector;
	// 模型包围盒最大值
	FVector PreviewBoundsMax = FVector::ZeroVector;
	
	// 当前动画已播放的时间（秒）
	float AnimationTimeSeconds = 0.0f;
	// 当前动画的采样率（帧/秒）
	float AnimationSampleRate = 20.0f;
	
	// 视觉上的偏航偏移量（用于平滑角色转身表现）
	float VisualYawOffsetDegrees = 0.0f;
	// 侧向移动（Strafe）姿态混合系数（0为无，1为完全）
	float StrafePoseAmount = 0.0f;
	// 是否启用侧向移动姿态层
	bool bEnableStrafePoseLayer = true;
	
	// 以下为移动姿态调优参数（从外部配置加载）
	// 侧向视觉偏航角度
	float StrafeVisualYawDegrees = 65.0f;
	// 偏航插值速度
	float StrafeVisualYawInterpSpeed = 12.0f;
	// 侧向姿态混合插值速度
	float StrafePoseInterpSpeed = 12.0f;
	// 下半身 Shuffle 覆盖层插值速度
	float StrafeOverlayInterpSpeed = 14.0f;
	// 上半身反向偏航角度（用于模拟侧向移动时身体朝向）
	float StrafeUpperBodyCounterYawDegrees = 45.0f;
	// 头部额外的反向偏航
	float StrafeHeadExtraCounterYawDegrees = 0.0f;
	// 头部倾斜角度
	float StrafeHeadTiltDegrees = 0.0f;
	// 躯干影响开始的高度比例（相对于模型高度）
	float StrafeUpperBodyStartHeightRatio = 0.42f;
	// 躯干影响结束的高度比例
	float StrafeUpperBodyEndHeightRatio = 0.82f;
	// 侧向姿态旋转枢轴高度比例
	float StrafePosePivotHeightRatio = 0.45f;
	// 头部旋转枢轴高度比例
	float StrafeHeadPivotHeightRatio = 0.52f;
	// 躯干骨骼影响缩放系数
	float StrafeTorsoInfluenceScale = 0.72f;
	// 头部骨骼影响缩放系数
	float StrafeHeadInfluenceScale = 1.0f;
	// 横移时下半身覆盖层权重
	float StrafeOverlayAmount = 0.0f;
	// 横移覆盖层独立播放时间
	float StrafeOverlayTimeSeconds = 0.0f;
	// 松开纯横移后，短暂播放 Shuffle 作为脚步归正，而不是按住期间全程叠加
	float StrafeReleaseShuffleRemainingSeconds = 0.0f;
	float StrafeReleaseShuffleDurationSeconds = 0.50f;
	float StrafeReleaseShuffleBlendOutSeconds = 0.15f;
	int32 StrafeReleaseShuffleDirection = 0;
	
	// 当前播放的主动画剪辑索引（移动动画）
	int32 CurrentAnimationClipIndex = INDEX_NONE;
	// 当前横移下半身覆盖动画剪辑索引
	int32 CurrentStrafeOverlayClipIndex = INDEX_NONE;
	// 上次记录日志的动画索引（用于避免重复输出）
	int32 LastLoggedAnimationClipIndex = INDEX_NONE;
	// 一次性动画剪辑索引（攻击/跳跃等，优先级最高）
	int32 OneShotAnimationClipIndex = INDEX_NONE;
	// 一次性动画剩余播放时间（秒）
	float OneShotRemainingSeconds = 0.0f;
	// 一次性动画播放速率，用于把较长的衔接动作压缩到更接近客户端手感的时间
	float OneShotPlaybackRate = 1.0f;
	// 上一帧是否处于下落状态，用于检测落地瞬间并触发JumpEnd
	bool bWasFallingLastTick = false;
	// 本次跳跃是否从地面移动状态起跳；只有这种情况落地才使用JumpLandRun连接跑步。
	bool bJumpStartedWithGroundMovement = false;
	// X键坐下状态；移动/跳跃会自动退出，避免坐着滑动。
	bool bSitting = false;
	// 移动中落地的短覆盖层索引；不抢占跑步主动画，只给一点触地缓冲。
	int32 MovingLandingOverlayClipIndex = INDEX_NONE;
	// 移动落地覆盖层的播放时间。
	float MovingLandingOverlayTimeSeconds = 0.0f;
	// 移动落地覆盖层剩余时间；到期后完全交回跑步/后退/横移动画。
	float MovingLandingOverlayRemainingSeconds = 0.0f;
	// 调试状态下强制播放的动画剪辑索引（非INDEX_NONE时覆盖自动选择）
	int32 DebugAnimationClipIndex = INDEX_NONE;
	
	// 战斗待机剩余停留时间（秒）
	float CombatReadyLingerSeconds = 0.0f;
	// 是否处于战斗待机状态
	bool bCombatReady = false;
	
// ============================================
// 虚拟输入状态变量
// 用于在预览组件中模拟玩家操控输入，驱动动画选择与角色朝向
// 这些状态通过 SetMovementInputState() 函数从外部设置
// ============================================

// --- 键盘移动按键 ---
// 前进键按下（对应W键/上方向键）—— 驱动前进动画
bool bMoveForwardHeld = false;
// 后退键按下（对应S键/下方向键）—— 驱动后退动画
bool bMoveBackwardHeld = false;
// 左平移键按下（对应Q键/A键的平移模式）—— 驱动左平移动画
bool bStrafeLeftHeld = false;
// 右平移键按下（对应E键/D键的平移模式）—— 驱动右平移动画
bool bStrafeRightHeld = false;

// --- 键盘转身按键 ---
// 左转键按下（对应A键的转身模式）—— 驱动原地左转动画 + 更新视觉偏航
bool bTurnLeftHeld = false;
// 右转键按下（对应D键的转身模式）—— 驱动原地右转动画 + 更新视觉偏航
bool bTurnRightHeld = false;

// --- 鼠标转身输入 ---
// 鼠标左转（右键拖拽向左/鼠标左移）—— 驱动平滑视觉偏航旋转
bool bMouseTurnLeft = false;
// 鼠标右转（右键拖拽向右/鼠标右移）—— 驱动平滑视觉偏航旋转
bool bMouseTurnRight = false;

// --- 鼠标按键状态 ---
// 鼠标左键按住（用于视角旋转但不改变角色朝向）
bool bLeftMouseHeld = false;
// 鼠标右键按住（用于视角旋转并改变角色朝向，即鼠标转向模式）
bool bRightMouseHeld = false;

// --- 模拟摇杆/轴输入强度（0.0~1.0） ---
// 前进方向输入强度（0=停止, 0.5=慢走, 1.0=跑步）—— 影响动画播放速率
float ForwardInputAmount = 0.0f;
// 侧向平移输入强度（0=停止, 1.0=全速平移）—— 影响侧向动画播放速率
float StrafeInputAmount = 0.0f;
};
