// UE-native M2 preview animation instance.

#pragma once

#include "CoreMinimal.h"
#include "Animation/AnimInstance.h"
#include "Animation/AnimInstanceProxy.h"
#include "WoWM2PreviewAnimInstance.generated.h"

class UAnimSequence;

USTRUCT()
struct FWoWM2PreviewAnimInstanceProxy : public FAnimInstanceProxy
{
	GENERATED_BODY()

	FWoWM2PreviewAnimInstanceProxy();
	FWoWM2PreviewAnimInstanceProxy(UAnimInstance* InAnimInstance);

	virtual bool Evaluate(FPoseContext& Output) override;
	virtual void InitializeObjects(UAnimInstance* InAnimInstance) override;
	virtual void ClearObjects() override;

private:
	UAnimSequence* BaseSequence = nullptr;
	UAnimSequence* UpperBodySequence = nullptr;
	float BaseTimeSeconds = 0.0f;
	float UpperBodyTimeSeconds = 0.0f;
	float UpperBodyWeight = 0.0f;
	bool bBaseLooping = true;
	TArray<int32> UpperBodyM2BoneIndices;
};

UCLASS(Transient)
class WOW_API UWoWM2PreviewAnimInstance : public UAnimInstance
{
	GENERATED_BODY()

public:
	void ConfigureM2PreviewPose(
		UAnimSequence* InBaseSequence,
		float InBaseTimeSeconds,
		bool bInBaseLooping,
		UAnimSequence* InUpperBodySequence,
		float InUpperBodyTimeSeconds,
		float InUpperBodyWeight,
		const TArray<int32>& InUpperBodyM2BoneIndices);

protected:
	virtual FAnimInstanceProxy* CreateAnimInstanceProxy() override;
	virtual void DestroyAnimInstanceProxy(FAnimInstanceProxy* InProxy) override;

private:
	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> BaseSequence = nullptr;

	UPROPERTY(Transient)
	TObjectPtr<UAnimSequence> UpperBodySequence = nullptr;

	float BaseTimeSeconds = 0.0f;
	float UpperBodyTimeSeconds = 0.0f;
	float UpperBodyWeight = 0.0f;
	bool bBaseLooping = true;
	TArray<int32> UpperBodyM2BoneIndices;

	friend struct FWoWM2PreviewAnimInstanceProxy;
};
