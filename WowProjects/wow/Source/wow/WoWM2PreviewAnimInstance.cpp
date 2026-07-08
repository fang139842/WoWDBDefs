#include "WoWM2PreviewAnimInstance.h"

#include "Animation/AnimSequence.h"
#include "Animation/AnimationPoseData.h"
#include "AnimationRuntime.h"
#include "BoneContainer.h"
#include "BonePose.h"

#include UE_INLINE_GENERATED_CPP_BY_NAME(WoWM2PreviewAnimInstance)

FWoWM2PreviewAnimInstanceProxy::FWoWM2PreviewAnimInstanceProxy()
	: FAnimInstanceProxy()
{
}

FWoWM2PreviewAnimInstanceProxy::FWoWM2PreviewAnimInstanceProxy(UAnimInstance* InAnimInstance)
	: FAnimInstanceProxy(InAnimInstance)
{
}

bool FWoWM2PreviewAnimInstanceProxy::Evaluate(FPoseContext& Output)
{
	if (!BaseSequence || BaseSequence->HasAnyFlags(RF_BeginDestroyed))
	{
		Output.ResetToRefPose();
		return true;
	}

	const float BaseLength = FMath::Max(BaseSequence->GetPlayLength(), KINDA_SMALL_NUMBER);
	const float BaseTime = bBaseLooping
		? FMath::Fmod(FMath::Max(BaseTimeSeconds, 0.0f), BaseLength)
		: FMath::Clamp(BaseTimeSeconds, 0.0f, BaseLength);

	FAnimationPoseData BasePoseData(Output);
	const FAnimExtractContext BaseExtractContext(static_cast<double>(BaseTime), BaseSequence->bEnableRootMotion, FDeltaTimeRecord(), bBaseLooping);
	BaseSequence->GetAnimationPose(BasePoseData, BaseExtractContext);

	if (!UpperBodySequence ||
		UpperBodySequence->HasAnyFlags(RF_BeginDestroyed) ||
		UpperBodyWeight <= ZERO_ANIMWEIGHT_THRESH ||
		UpperBodyM2BoneIndices.Num() == 0)
	{
		Output.Pose.NormalizeRotations();
		return true;
	}

	FCompactPose UpperPose;
	FBlendedCurve UpperCurve;
	UE::Anim::FStackAttributeContainer UpperAttributes;
	UpperPose.SetBoneContainer(&Output.Pose.GetBoneContainer());
	UpperCurve.InitFrom(Output.Curve);

	FAnimationPoseData UpperPoseData(UpperPose, UpperCurve, UpperAttributes);
	const float UpperLength = FMath::Max(UpperBodySequence->GetPlayLength(), KINDA_SMALL_NUMBER);
	const float UpperTime = FMath::Clamp(UpperBodyTimeSeconds, 0.0f, UpperLength);
	const FAnimExtractContext UpperExtractContext(static_cast<double>(UpperTime), false, FDeltaTimeRecord(), false);
	UpperBodySequence->GetAnimationPose(UpperPoseData, UpperExtractContext);

	TArray<float> PerBoneWeights;
	const FBoneContainer& BoneContainer = Output.Pose.GetBoneContainer();
	PerBoneWeights.Init(0.0f, BoneContainer.GetCompactPoseNumBones());
	const float ClampedWeight = FMath::Clamp(UpperBodyWeight, 0.0f, 1.0f);
	for (const int32 M2BoneIndex : UpperBodyM2BoneIndices)
	{
		const int32 SkeletonBoneIndex = M2BoneIndex + 1; // generated skeleton has a synthetic M2_Root at raw index 0
		const FCompactPoseBoneIndex CompactIndex = BoneContainer.GetCompactPoseIndexFromSkeletonIndex(SkeletonBoneIndex);
		if (CompactIndex.GetInt() != INDEX_NONE && PerBoneWeights.IsValidIndex(CompactIndex.GetInt()))
		{
			PerBoneWeights[CompactIndex.GetInt()] = ClampedWeight;
		}
	}

	if (PerBoneWeights.ContainsByPredicate([](float Weight) { return Weight > ZERO_ANIMWEIGHT_THRESH; }))
	{
		FAnimationPoseData OutPoseData(Output);
		FAnimationRuntime::BlendTwoPosesTogetherPerBone(BasePoseData, UpperPoseData, PerBoneWeights, OutPoseData);
	}
	else
	{
		Output.Pose.NormalizeRotations();
	}
	return true;
}

void FWoWM2PreviewAnimInstanceProxy::InitializeObjects(UAnimInstance* InAnimInstance)
{
	FAnimInstanceProxy::InitializeObjects(InAnimInstance);
	const UWoWM2PreviewAnimInstance* PreviewInstance = CastChecked<UWoWM2PreviewAnimInstance>(InAnimInstance);
	BaseSequence = PreviewInstance->BaseSequence;
	UpperBodySequence = PreviewInstance->UpperBodySequence;
	BaseTimeSeconds = PreviewInstance->BaseTimeSeconds;
	UpperBodyTimeSeconds = PreviewInstance->UpperBodyTimeSeconds;
	UpperBodyWeight = PreviewInstance->UpperBodyWeight;
	bBaseLooping = PreviewInstance->bBaseLooping;
	UpperBodyM2BoneIndices = PreviewInstance->UpperBodyM2BoneIndices;
}

void FWoWM2PreviewAnimInstanceProxy::ClearObjects()
{
	FAnimInstanceProxy::ClearObjects();
	BaseSequence = nullptr;
	UpperBodySequence = nullptr;
}

void UWoWM2PreviewAnimInstance::ConfigureM2PreviewPose(
	UAnimSequence* InBaseSequence,
	float InBaseTimeSeconds,
	bool bInBaseLooping,
	UAnimSequence* InUpperBodySequence,
	float InUpperBodyTimeSeconds,
	float InUpperBodyWeight,
	const TArray<int32>& InUpperBodyM2BoneIndices)
{
	BaseSequence = InBaseSequence;
	BaseTimeSeconds = InBaseTimeSeconds;
	bBaseLooping = bInBaseLooping;
	UpperBodySequence = InUpperBodySequence;
	UpperBodyTimeSeconds = InUpperBodyTimeSeconds;
	UpperBodyWeight = FMath::Clamp(InUpperBodyWeight, 0.0f, 1.0f);
	UpperBodyM2BoneIndices = InUpperBodyM2BoneIndices;
}

FAnimInstanceProxy* UWoWM2PreviewAnimInstance::CreateAnimInstanceProxy()
{
	return new FWoWM2PreviewAnimInstanceProxy(this);
}

void UWoWM2PreviewAnimInstance::DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy)
{
	delete InProxy;
}
