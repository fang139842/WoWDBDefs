#include "WoWM2ModelComponent.h"

#include "Camera/PlayerCameraManager.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "KismetProceduralMeshLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SoftObjectPath.h"

static TAutoConsoleVariable<int32> CVarWoWM2RuntimeUpdateHz(
	TEXT("wow.M2RuntimeUpdateHz"),
	20,
	TEXT("Caps CPU-side runtime M2 mesh-section updates while keeping bone pose and ribbons updated every frame. 0 or lower updates mesh every frame."));

static TAutoConsoleVariable<int32> CVarWoWM2RuntimeParticleUpdateHz(
	TEXT("wow.M2RuntimeParticleUpdateHz"),
	20,
	TEXT("Caps CPU-side runtime M2 particle simulation and procedural mesh updates. 0 or lower updates every frame."));

static TAutoConsoleVariable<int32> CVarWoWM2RuntimeParticles(
	TEXT("wow.M2RuntimeParticles"),
	1,
	TEXT("Enables CPU-side runtime M2 particle mesh updates."));

static TAutoConsoleVariable<int32> CVarWoWM2RuntimeRibbons(
	TEXT("wow.M2RuntimeRibbons"),
	1,
	TEXT("Enables CPU-side runtime M2 ribbon mesh updates."));

namespace
{
constexpr const TCHAR* PreviewTextureParameterName = TEXT("DiffuseTexture");
constexpr const TCHAR* PreviewTintParameterName = TEXT("TintColor");
constexpr int32 M2BoneFlagBillboard = 0x08;
constexpr bool bForceTwoSidedOpaqueModelSections = true;
constexpr float M2LowAlphaAdditiveTintScale = 0.25f;
constexpr float RibbonMinEdgeDistanceWorldUnits = 1.25f;
constexpr float WowScalarToUE = 100.0f;
constexpr int32 MaxItemParticlesPerEmitter = 64;

struct FParsedM2Section
{
	int32 RenderMode = 0;
	bool bUseEnvMap = false;
	bool bTwoSided = false;
	bool bUnlit = false;
	bool bNoDepthTest = false;
	FString PreviewPng;
	TArray<FString> LayerPreviewPngs;
	TArray<FVector> Positions;
	TArray<FIntVector4> BoneWeights;
	TArray<FIntVector4> BoneIndices;
	TArray<FVector> Normals;
	TArray<FVector2D> UV0;
	TArray<FWoWM2TextureTransformSample> TextureTransformSamples;
	TArray<FWoWM2ColorSample> ColorSamples;
	TArray<FLinearColor> VertexColors;
	TArray<int32> Indices;
};

struct FParsedM2Material
{
	int32 Index = INDEX_NONE;
	int32 MaterialFlags = 0;
	int32 RenderMode = 0;
	bool bTwoSided = false;
	bool bUnlit = false;
	bool bNoDepthTest = false;
	bool bNoDepthWrite = false;
};

struct FParsedM2ModelData
{
	TArray<FParsedM2Section> Sections;
	TArray<FWoWM2AnimationClip> AnimationClips;
	TArray<FWoWM2BoneMetadata> Bones;
	TArray<FParsedM2Material> Materials;
	TArray<FWoWM2RibbonEmitter> RibbonEmitters;
	TArray<FWoWM2ParticleEmitter> ParticleEmitters;
};

TMap<FString, TSharedPtr<FParsedM2ModelData>>& GetParsedM2ModelCache()
{
	static TMap<FString, TSharedPtr<FParsedM2ModelData>> Cache;
	return Cache;
}

TMap<FString, TWeakObjectPtr<UTexture2D>>& GetRuntimeTextureCache()
{
	static TMap<FString, TWeakObjectPtr<UTexture2D>> Cache;
	return Cache;
}

TMap<FString, TWeakObjectPtr<UMaterialInterface>>& GetRuntimeParentMaterialCache()
{
	static TMap<FString, TWeakObjectPtr<UMaterialInterface>> Cache;
	return Cache;
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

bool ReadVectorField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FVector& OutValue)
{
	const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Tuple) || !Tuple || Tuple->Num() != 3)
	{
		return false;
	}

	OutValue = FVector((*Tuple)[0]->AsNumber(), (*Tuple)[1]->AsNumber(), (*Tuple)[2]->AsNumber());
	return true;
}

bool ReadVectorArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FVector>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values)
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
		if (!Value.IsValid() || !Value->TryGetArray(Tuple) || !Tuple || Tuple->Num() != 3)
		{
			return false;
		}
		OutValues.Emplace((*Tuple)[0]->AsNumber(), (*Tuple)[1]->AsNumber(), (*Tuple)[2]->AsNumber());
	}
	return true;
}

bool ReadUVArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FVector2D>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values)
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
		if (!Value.IsValid() || !Value->TryGetArray(Tuple) || !Tuple || Tuple->Num() != 2)
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
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values)
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
		if (!Value.IsValid() || !Value->TryGetArray(Tuple) || !Tuple || Tuple->Num() != 4)
		{
			return false;
		}
		OutValues.Emplace((*Tuple)[0]->AsNumber(), (*Tuple)[1]->AsNumber(), (*Tuple)[2]->AsNumber(), (*Tuple)[3]->AsNumber());
	}
	return true;
}

bool ReadIndexArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<int32>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values)
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

bool ReadIntVector4Array(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FIntVector4>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values)
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
		if (!Value.IsValid() || !Value->TryGetArray(Tuple) || !Tuple || Tuple->Num() != 4)
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

bool ReadTextureTransformSamples(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FWoWM2TextureTransformSample>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values)
	{
		return true;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* SampleObject = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(SampleObject) || !SampleObject || !SampleObject->IsValid())
		{
			return false;
		}

		double Number = 0.0;
		FWoWM2TextureTransformSample Sample;
		if ((*SampleObject)->TryGetNumberField(TEXT("time_ms"), Number))
		{
			Sample.TimeSeconds = static_cast<float>(Number / 1000.0);
		}
		if ((*SampleObject)->TryGetNumberField(TEXT("rotation_degrees"), Number))
		{
			Sample.RotationDegrees = static_cast<float>(Number);
		}

		const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
		if ((*SampleObject)->TryGetArrayField(TEXT("translation"), Tuple) && Tuple && Tuple->Num() >= 2)
		{
			Sample.Translation = FVector2D((*Tuple)[0]->AsNumber(), (*Tuple)[1]->AsNumber());
		}
		if ((*SampleObject)->TryGetArrayField(TEXT("scaling"), Tuple) && Tuple && Tuple->Num() >= 2)
		{
			Sample.Scaling = FVector2D((*Tuple)[0]->AsNumber(), (*Tuple)[1]->AsNumber());
		}
		OutValues.Add(Sample);
	}
	return true;
}

bool ReadColorSamples(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FWoWM2ColorSample>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values)
	{
		return true;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* SampleObject = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(SampleObject) || !SampleObject || !SampleObject->IsValid())
		{
			return false;
		}

		double Number = 0.0;
		FWoWM2ColorSample Sample;
		if ((*SampleObject)->TryGetNumberField(TEXT("time_ms"), Number))
		{
			Sample.TimeSeconds = static_cast<float>(Number / 1000.0);
		}

		const TArray<TSharedPtr<FJsonValue>>* ColorValues = nullptr;
		if ((*SampleObject)->TryGetArrayField(TEXT("color"), ColorValues) && ColorValues && ColorValues->Num() == 4)
		{
			Sample.Color = FLinearColor(
				static_cast<float>((*ColorValues)[0]->AsNumber()),
				static_cast<float>((*ColorValues)[1]->AsNumber()),
				static_cast<float>((*ColorValues)[2]->AsNumber()),
				static_cast<float>((*ColorValues)[3]->AsNumber()));
		}
		OutValues.Add(Sample);
	}
	return true;
}

bool ReadMatrixFrameArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<TArray<FMatrix>>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* JsonFrames = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, JsonFrames) || !JsonFrames)
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(JsonFrames->Num());
	for (const TSharedPtr<FJsonValue>& FrameValue : *JsonFrames)
	{
		const TArray<TSharedPtr<FJsonValue>>* JsonBones = nullptr;
		if (!FrameValue.IsValid() || !FrameValue->TryGetArray(JsonBones) || !JsonBones)
		{
			return false;
		}

		TArray<FMatrix> BoneMatrices;
		BoneMatrices.Reserve(JsonBones->Num());
		for (const TSharedPtr<FJsonValue>& BoneValue : *JsonBones)
		{
			const TArray<TSharedPtr<FJsonValue>>* JsonRows = nullptr;
			if (!BoneValue.IsValid() || !BoneValue->TryGetArray(JsonRows) || !JsonRows || JsonRows->Num() != 4)
			{
				return false;
			}

			FMatrix Matrix = FMatrix::Identity;
			for (int32 Row = 0; Row < 4; ++Row)
			{
				const TArray<TSharedPtr<FJsonValue>>* JsonColumns = nullptr;
				if (!(*JsonRows)[Row].IsValid() || !(*JsonRows)[Row]->TryGetArray(JsonColumns) || !JsonColumns || JsonColumns->Num() != 4)
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

bool ReadSections(const TSharedPtr<FJsonObject>& Object, TArray<FParsedM2Section>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("sections"), Values) || !Values)
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* SectionObject = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(SectionObject) || !SectionObject || !SectionObject->IsValid())
		{
			return false;
		}

		FParsedM2Section Section;
		(*SectionObject)->TryGetNumberField(TEXT("render_mode"), Section.RenderMode);
		(*SectionObject)->TryGetBoolField(TEXT("use_env_map"), Section.bUseEnvMap);
		(*SectionObject)->TryGetBoolField(TEXT("two_sided"), Section.bTwoSided);
		(*SectionObject)->TryGetBoolField(TEXT("unlit"), Section.bUnlit);
		(*SectionObject)->TryGetBoolField(TEXT("no_depth_test"), Section.bNoDepthTest);
		(*SectionObject)->TryGetStringField(TEXT("preview_png"), Section.PreviewPng);
		const TArray<TSharedPtr<FJsonValue>>* LayerPreviewValues = nullptr;
		if ((*SectionObject)->TryGetArrayField(TEXT("layer_preview_pngs"), LayerPreviewValues) && LayerPreviewValues)
		{
			for (const TSharedPtr<FJsonValue>& LayerPreviewValue : *LayerPreviewValues)
			{
				if (LayerPreviewValue.IsValid() && LayerPreviewValue->Type == EJson::String)
				{
					Section.LayerPreviewPngs.Add(LayerPreviewValue->AsString());
				}
			}
		}

		if (!ReadVectorArray(*SectionObject, TEXT("positions"), Section.Positions) ||
			!ReadIntVector4Array(*SectionObject, TEXT("bone_weights"), Section.BoneWeights) ||
			!ReadIntVector4Array(*SectionObject, TEXT("bone_indices"), Section.BoneIndices) ||
			!ReadVectorArray(*SectionObject, TEXT("normals"), Section.Normals) ||
			!ReadUVArray(*SectionObject, TEXT("uvs"), Section.UV0) ||
			!ReadTextureTransformSamples(*SectionObject, TEXT("texture_transform_samples"), Section.TextureTransformSamples) ||
			!ReadColorSamples(*SectionObject, TEXT("color_samples"), Section.ColorSamples) ||
			!ReadColorArray(*SectionObject, TEXT("vertex_colors"), Section.VertexColors) ||
			!ReadIndexArray(*SectionObject, TEXT("indices"), Section.Indices))
		{
			return false;
		}
		OutValues.Add(MoveTemp(Section));
	}
	return true;
}

bool ReadAnimations(const TSharedPtr<FJsonObject>& Object, TArray<FWoWM2AnimationClip>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("animations"), Values) || !Values)
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* ClipObject = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(ClipObject) || !ClipObject || !ClipObject->IsValid())
		{
			return false;
		}

		FWoWM2AnimationClip Clip;
		(*ClipObject)->TryGetStringField(TEXT("key"), Clip.Key);
		double Number = 0.0;
		if ((*ClipObject)->TryGetNumberField(TEXT("sequence_index"), Number))
		{
			Clip.SequenceIndex = FMath::RoundToInt(Number);
		}
		if ((*ClipObject)->TryGetNumberField(TEXT("animation_id"), Number))
		{
			Clip.AnimationId = FMath::RoundToInt(Number);
		}
		if ((*ClipObject)->TryGetNumberField(TEXT("sub_animation_id"), Number))
		{
			Clip.SubAnimationId = FMath::RoundToInt(Number);
		}
		if ((*ClipObject)->TryGetNumberField(TEXT("length_ms"), Number))
		{
			Clip.LengthSeconds = static_cast<float>(FMath::Max(Number / 1000.0, 0.0));
		}
		if ((*ClipObject)->TryGetNumberField(TEXT("sample_rate"), Number))
		{
			Clip.SampleRate = static_cast<float>(Number > 0.0 ? Number : 20.0);
		}
		(*ClipObject)->TryGetBoolField(TEXT("loop_end_frame"), Clip.bLoopEndFrame);

		const TArray<TSharedPtr<FJsonValue>>* LookupIdValues = nullptr;
		if ((*ClipObject)->TryGetArrayField(TEXT("lookup_ids"), LookupIdValues) && LookupIdValues)
		{
			for (const TSharedPtr<FJsonValue>& LookupIdValue : *LookupIdValues)
			{
				Clip.LookupIds.Add(static_cast<int32>(LookupIdValue->AsNumber()));
			}
		}
		if (ReadMatrixFrameArray(*ClipObject, TEXT("frames"), Clip.Frames) && Clip.Frames.Num() > 0)
		{
			OutValues.Add(MoveTemp(Clip));
		}
	}
	return true;
}

bool ReadBones(const TSharedPtr<FJsonObject>& Object, TArray<FWoWM2BoneMetadata>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("bones"), Values) || !Values)
	{
		return false;
	}

	OutValues.Reset();
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* BoneObject = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(BoneObject) || !BoneObject || !BoneObject->IsValid())
		{
			return false;
		}

		double Number = 0.0;
		FWoWM2BoneMetadata Bone;
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

bool ReadMaterials(const TSharedPtr<FJsonObject>& Object, TArray<FParsedM2Material>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("materials"), Values) || !Values)
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* MaterialObject = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(MaterialObject) || !MaterialObject || !MaterialObject->IsValid())
		{
			return false;
		}

		double Number = 0.0;
		FParsedM2Material Material;
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
		(*MaterialObject)->TryGetBoolField(TEXT("no_depth_test"), Material.bNoDepthTest);
		(*MaterialObject)->TryGetBoolField(TEXT("no_depth_write"), Material.bNoDepthWrite);
		OutValues.Add(Material);
	}
	return true;
}

bool ApplyMaterialToSection(const TArray<FParsedM2Material>& Materials, int32 MaterialIndex, int32& InOutRenderMode, bool& bOutTwoSided, bool& bOutUnlit, bool& bOutNoDepthTest)
{
	const FParsedM2Material* Material = Materials.FindByPredicate([MaterialIndex](const FParsedM2Material& Candidate)
	{
		return Candidate.Index == MaterialIndex;
	});
	if (!Material)
	{
		return false;
	}

	InOutRenderMode = Material->RenderMode;
	bOutTwoSided = Material->bTwoSided;
	bOutUnlit = Material->bUnlit;
	bOutNoDepthTest = Material->bNoDepthTest;
	return true;
}

bool ReadIntArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<int32>& OutValues)
{
	return ReadIndexArray(Object, FieldName, OutValues);
}

void ReadParticleFloatSamples(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FWoWM2ParticleFloatSample>& OutValues)
{
	OutValues.Reset();
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values)
	{
		return;
	}

	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* SampleObject = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(SampleObject) || !SampleObject || !SampleObject->IsValid())
		{
			continue;
		}

		double Number = 0.0;
		FWoWM2ParticleFloatSample Sample;
		if ((*SampleObject)->TryGetNumberField(TEXT("time_ms"), Number))
		{
			Sample.TimeSeconds = static_cast<float>(Number / 1000.0);
		}
		if ((*SampleObject)->TryGetNumberField(TEXT("value"), Number))
		{
			Sample.Value = static_cast<float>(Number);
		}
		OutValues.Add(Sample);
	}
}

bool ReadLinearColorArray3(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FLinearColor (&OutValues)[3])
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values)
	{
		return false;
	}

	for (int32 Index = 0; Index < FMath::Min(Values->Num(), 3); ++Index)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
		if ((*Values)[Index].IsValid() && (*Values)[Index]->TryGetArray(Tuple) && Tuple && Tuple->Num() >= 4)
		{
			OutValues[Index] = FLinearColor(
				static_cast<float>((*Tuple)[0]->AsNumber()),
				static_cast<float>((*Tuple)[1]->AsNumber()),
				static_cast<float>((*Tuple)[2]->AsNumber()),
				static_cast<float>((*Tuple)[3]->AsNumber()));
		}
	}
	return true;
}

bool ReadFloatArray3(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, float (&OutValues)[3])
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || !Values)
	{
		return false;
	}

	for (int32 Index = 0; Index < FMath::Min(Values->Num(), 3); ++Index)
	{
		if ((*Values)[Index].IsValid())
		{
			OutValues[Index] = static_cast<float>((*Values)[Index]->AsNumber());
		}
	}
	return true;
}

bool ReadParticleEmitters(const TSharedPtr<FJsonObject>& Object, TArray<FWoWM2ParticleEmitter>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("particle_emitters"), Values) || !Values)
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* EmitterObject = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(EmitterObject) || !EmitterObject || !EmitterObject->IsValid())
		{
			continue;
		}

		double Number = 0.0;
		FWoWM2ParticleEmitter Emitter;
		if ((*EmitterObject)->TryGetNumberField(TEXT("index"), Number))
		{
			Emitter.Index = FMath::RoundToInt(Number);
		}
		if ((*EmitterObject)->TryGetNumberField(TEXT("flags"), Number))
		{
			Emitter.Flags = FMath::RoundToInt(Number);
		}
		if ((*EmitterObject)->TryGetNumberField(TEXT("bone"), Number))
		{
			Emitter.BoneIndex = FMath::RoundToInt(Number);
		}
		ReadVectorField(*EmitterObject, TEXT("position"), Emitter.Position);
		(*EmitterObject)->TryGetStringField(TEXT("preview_png"), Emitter.PreviewPng);
		if ((*EmitterObject)->TryGetNumberField(TEXT("blend"), Number))
		{
			Emitter.Blend = FMath::RoundToInt(Number);
		}
		if ((*EmitterObject)->TryGetNumberField(TEXT("emitter_type"), Number))
		{
			Emitter.EmitterType = FMath::RoundToInt(Number);
		}
		if ((*EmitterObject)->TryGetNumberField(TEXT("particle_type"), Number))
		{
			Emitter.ParticleType = FMath::RoundToInt(Number);
		}
		if ((*EmitterObject)->TryGetNumberField(TEXT("texture_tile_rotation"), Number))
		{
			Emitter.TextureTileRotation = FMath::RoundToInt(Number);
		}
		if ((*EmitterObject)->TryGetNumberField(TEXT("texture_cols"), Number))
		{
			Emitter.TextureColumns = FMath::Max(FMath::RoundToInt(Number), 1);
		}
		if ((*EmitterObject)->TryGetNumberField(TEXT("texture_rows"), Number))
		{
			Emitter.TextureRows = FMath::Max(FMath::RoundToInt(Number), 1);
		}
		ReadLinearColorArray3(*EmitterObject, TEXT("lifecycle_colors"), Emitter.LifecycleColors);
		ReadFloatArray3(*EmitterObject, TEXT("lifecycle_sizes"), Emitter.LifecycleSizes);
		if ((*EmitterObject)->TryGetNumberField(TEXT("mid_point"), Number))
		{
			Emitter.MidPoint = static_cast<float>(Number);
		}
		if ((*EmitterObject)->TryGetNumberField(TEXT("slowdown"), Number))
		{
			Emitter.Slowdown = static_cast<float>(Number);
		}
		if ((*EmitterObject)->TryGetNumberField(TEXT("rotation"), Number))
		{
			Emitter.Rotation = static_cast<float>(Number);
		}
		ReadParticleFloatSamples(*EmitterObject, TEXT("emission_speed_samples"), Emitter.EmissionSpeedSamples);
		ReadParticleFloatSamples(*EmitterObject, TEXT("speed_variation_samples"), Emitter.SpeedVariationSamples);
		ReadParticleFloatSamples(*EmitterObject, TEXT("lifespan_samples"), Emitter.LifespanSamples);
		ReadParticleFloatSamples(*EmitterObject, TEXT("emission_rate_samples"), Emitter.EmissionRateSamples);
		ReadParticleFloatSamples(*EmitterObject, TEXT("emission_area_length_samples"), Emitter.EmissionAreaLengthSamples);
		ReadParticleFloatSamples(*EmitterObject, TEXT("emission_area_width_samples"), Emitter.EmissionAreaWidthSamples);
		ReadParticleFloatSamples(*EmitterObject, TEXT("gravity_samples"), Emitter.GravitySamples);
		ReadParticleFloatSamples(*EmitterObject, TEXT("gravity2_samples"), Emitter.Gravity2Samples);
		ReadParticleFloatSamples(*EmitterObject, TEXT("vertical_range_samples"), Emitter.VerticalRangeSamples);
		ReadParticleFloatSamples(*EmitterObject, TEXT("horizontal_range_samples"), Emitter.HorizontalRangeSamples);
		if (!Emitter.PreviewPng.IsEmpty())
		{
			OutValues.Add(MoveTemp(Emitter));
		}
	}
	return true;
}

bool ReadRibbons(const TSharedPtr<FJsonObject>& Object, TArray<FWoWM2RibbonEmitter>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("ribbons"), Values) || !Values)
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* RibbonObject = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(RibbonObject) || !RibbonObject || !RibbonObject->IsValid())
		{
			return false;
		}

		double Number = 0.0;
		FWoWM2RibbonEmitter Ribbon;
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
		ReadIntArrayField(*RibbonObject, TEXT("material_indices"), Ribbon.MaterialIndices);
		if ((*RibbonObject)->TryGetNumberField(TEXT("resolution"), Number))
		{
			Ribbon.EdgesPerSecond = FMath::Max(static_cast<float>(Number), 1.0f);
		}
		if ((*RibbonObject)->TryGetNumberField(TEXT("length"), Number))
		{
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
		if ((*RibbonObject)->TryGetArrayField(TEXT("samples"), SampleValues) && SampleValues)
		{
			for (const TSharedPtr<FJsonValue>& SampleValue : *SampleValues)
			{
				const TSharedPtr<FJsonObject>* SampleObject = nullptr;
				if (!SampleValue.IsValid() || !SampleValue->TryGetObject(SampleObject) || !SampleObject || !SampleObject->IsValid())
				{
					continue;
				}

				FWoWM2RibbonSample Sample;
				if ((*SampleObject)->TryGetNumberField(TEXT("time_ms"), Number))
				{
					Sample.TimeSeconds = static_cast<float>(Number / 1000.0);
				}
				const TArray<TSharedPtr<FJsonValue>>* ColorValues = nullptr;
				if ((*SampleObject)->TryGetArrayField(TEXT("color"), ColorValues) && ColorValues && ColorValues->Num() == 3)
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

		const TArray<TSharedPtr<FJsonValue>>* SlotValues = nullptr;
		if ((*RibbonObject)->TryGetArrayField(TEXT("tex_slot_samples"), SlotValues) && SlotValues)
		{
			for (const TSharedPtr<FJsonValue>& SlotValue : *SlotValues)
			{
				const TSharedPtr<FJsonObject>* SlotObject = nullptr;
				if (!SlotValue.IsValid() || !SlotValue->TryGetObject(SlotObject) || !SlotObject || !SlotObject->IsValid())
				{
					continue;
				}
				FWoWM2RibbonTexSlotSample SlotSample;
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
		if ((*RibbonObject)->TryGetArrayField(TEXT("visibility_samples"), VisibilityValues) && VisibilityValues)
		{
			for (const TSharedPtr<FJsonValue>& VisibilityValue : *VisibilityValues)
			{
				const TSharedPtr<FJsonObject>* VisibilityObject = nullptr;
				if (!VisibilityValue.IsValid() || !VisibilityValue->TryGetObject(VisibilityObject) || !VisibilityObject || !VisibilityObject->IsValid())
				{
					continue;
				}
				FWoWM2RibbonVisibilitySample VisibilitySample;
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

TSharedPtr<FParsedM2ModelData> LoadParsedM2ModelData(const FString& MeshJsonPath)
{
	const FString FullMeshJsonPath = FPaths::ConvertRelativePathToFull(MeshJsonPath);
	TMap<FString, TSharedPtr<FParsedM2ModelData>>& Cache = GetParsedM2ModelCache();
	if (const TSharedPtr<FParsedM2ModelData>* Cached = Cache.Find(FullMeshJsonPath))
	{
		return *Cached;
	}

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *FullMeshJsonPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("M2 model mesh json was not found: %s"), *FullMeshJsonPath);
		return nullptr;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to parse M2 model mesh json: %s"), *FullMeshJsonPath);
		return nullptr;
	}

	TSharedPtr<FParsedM2ModelData> ParsedData = MakeShared<FParsedM2ModelData>();
	if (!ReadSections(RootObject, ParsedData->Sections))
	{
		UE_LOG(LogTemp, Error, TEXT("M2 model mesh json has invalid sections: %s"), *FullMeshJsonPath);
		return nullptr;
	}

	ReadAnimations(RootObject, ParsedData->AnimationClips);
	ReadBones(RootObject, ParsedData->Bones);
	ReadMaterials(RootObject, ParsedData->Materials);
	ReadRibbons(RootObject, ParsedData->RibbonEmitters);
	ReadParticleEmitters(RootObject, ParsedData->ParticleEmitters);
	Cache.Add(FullMeshJsonPath, ParsedData);
	return ParsedData;
}

FVector TransformPointByExportMatrix(const FMatrix& Matrix, const FVector& Point)
{
	return FVector(
		Matrix.M[0][0] * Point.X + Matrix.M[0][1] * Point.Y + Matrix.M[0][2] * Point.Z + Matrix.M[0][3],
		Matrix.M[1][0] * Point.X + Matrix.M[1][1] * Point.Y + Matrix.M[1][2] * Point.Z + Matrix.M[1][3],
		Matrix.M[2][0] * Point.X + Matrix.M[2][1] * Point.Y + Matrix.M[2][2] * Point.Z + Matrix.M[2][3]);
}

int32 GetIntVector4Component(const FIntVector4& Value, int32 Index)
{
	switch (Index)
	{
	case 0: return Value.X;
	case 1: return Value.Y;
	case 2: return Value.Z;
	case 3: return Value.W;
	default: return 0;
	}
}

void ResolveLoopingClipFrame(const FWoWM2AnimationClip& Clip, float TimeSeconds, int32& OutFrameIndex, int32& OutNextFrameIndex, float& OutFrameAlpha)
{
	const int32 FrameCount = Clip.Frames.Num();
	if (FrameCount <= 1 || Clip.SampleRate <= 0.0f)
	{
		OutFrameIndex = 0;
		OutNextFrameIndex = 0;
		OutFrameAlpha = 0.0f;
		return;
	}

	const float EffectiveLength = Clip.LengthSeconds > KINDA_SMALL_NUMBER
		? Clip.LengthSeconds
		: static_cast<float>(FrameCount) / Clip.SampleRate;
	const float WrappedTime = EffectiveLength > KINDA_SMALL_NUMBER
		? FMath::Fmod(FMath::Max(TimeSeconds, 0.0f), EffectiveLength)
		: 0.0f;
	const float FramePosition = WrappedTime * Clip.SampleRate;
	OutFrameIndex = FMath::Clamp(FMath::FloorToInt(FramePosition), 0, FrameCount - 1);
	OutNextFrameIndex = (OutFrameIndex + 1) % FrameCount;
	OutFrameAlpha = FMath::Clamp(FramePosition - static_cast<float>(OutFrameIndex), 0.0f, 1.0f);
}

FWoWM2ColorSample SampleSectionColor(const TArray<FWoWM2ColorSample>& Samples, float TimeSeconds)
{
	if (Samples.Num() == 0)
	{
		return FWoWM2ColorSample();
	}

	const float Duration = FMath::Max(Samples.Last().TimeSeconds, KINDA_SMALL_NUMBER);
	const float WrappedTime = FMath::Fmod(FMath::Max(TimeSeconds, 0.0f), Duration);
	for (int32 Index = 1; Index < Samples.Num(); ++Index)
	{
		if (WrappedTime <= Samples[Index].TimeSeconds)
		{
			const FWoWM2ColorSample& Previous = Samples[Index - 1];
			const FWoWM2ColorSample& Next = Samples[Index];
			const float SegmentDuration = FMath::Max(Next.TimeSeconds - Previous.TimeSeconds, KINDA_SMALL_NUMBER);
			const float Alpha = FMath::Clamp((WrappedTime - Previous.TimeSeconds) / SegmentDuration, 0.0f, 1.0f);
			FWoWM2ColorSample Result;
			Result.TimeSeconds = WrappedTime;
			Result.Color = FMath::Lerp(Previous.Color, Next.Color, Alpha);
			return Result;
		}
	}
	return Samples.Last();
}

FWoWM2TextureTransformSample SampleTextureTransform(const TArray<FWoWM2TextureTransformSample>& Samples, float TimeSeconds)
{
	if (Samples.Num() == 0)
	{
		return FWoWM2TextureTransformSample();
	}

	const float Duration = FMath::Max(Samples.Last().TimeSeconds, KINDA_SMALL_NUMBER);
	const float WrappedTime = FMath::Fmod(FMath::Max(TimeSeconds, 0.0f), Duration);
	for (int32 Index = 1; Index < Samples.Num(); ++Index)
	{
		if (WrappedTime <= Samples[Index].TimeSeconds)
		{
			const FWoWM2TextureTransformSample& Previous = Samples[Index - 1];
			const FWoWM2TextureTransformSample& Next = Samples[Index];
			const float SegmentDuration = FMath::Max(Next.TimeSeconds - Previous.TimeSeconds, KINDA_SMALL_NUMBER);
			const float Alpha = FMath::Clamp((WrappedTime - Previous.TimeSeconds) / SegmentDuration, 0.0f, 1.0f);
			FWoWM2TextureTransformSample Result;
			Result.TimeSeconds = WrappedTime;
			Result.Translation = FMath::Lerp(Previous.Translation, Next.Translation, Alpha);
			Result.Scaling = FMath::Lerp(Previous.Scaling, Next.Scaling, Alpha);
			Result.RotationDegrees = FMath::Lerp(Previous.RotationDegrees, Next.RotationDegrees, Alpha);
			return Result;
		}
	}
	return Samples.Last();
}

void ApplyColorSamples(const TArray<FWoWM2ColorSample>& Samples, float TimeSeconds, bool bScaleLowAlphaAdditive, TArray<FLinearColor>& InOutVertexColors)
{
	if (Samples.Num() == 0 || InOutVertexColors.Num() == 0)
	{
		return;
	}

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

void ApplyTextureTransformToUVs(const TArray<FVector2D>& BaseUVs, const TArray<FWoWM2TextureTransformSample>& Samples, float TimeSeconds, TArray<FVector2D>& OutUVs)
{
	if (Samples.Num() == 0)
	{
		return;
	}
	if (OutUVs.Num() != BaseUVs.Num())
	{
		OutUVs = BaseUVs;
	}

	const FWoWM2TextureTransformSample Sample = SampleTextureTransform(Samples, TimeSeconds);
	const float Radians = FMath::DegreesToRadians(Sample.RotationDegrees);
	const float Cos = FMath::Cos(Radians);
	const float Sin = FMath::Sin(Radians);
	for (int32 Index = 0; Index < BaseUVs.Num(); ++Index)
	{
		const FVector2D Scaled(BaseUVs[Index].X * Sample.Scaling.X, BaseUVs[Index].Y * Sample.Scaling.Y);
		const FVector2D Rotated(Scaled.X * Cos - Scaled.Y * Sin, Scaled.X * Sin + Scaled.Y * Cos);
		OutUVs[Index] = Rotated + Sample.Translation;
	}
}

float SampleParticleFloatTrack(const TArray<FWoWM2ParticleFloatSample>& Samples, float TimeSeconds, float DefaultValue)
{
	if (Samples.Num() == 0)
	{
		return DefaultValue;
	}

	const float Duration = FMath::Max(Samples.Last().TimeSeconds, KINDA_SMALL_NUMBER);
	const float WrappedTime = FMath::Fmod(FMath::Max(TimeSeconds, 0.0f), Duration);
	for (int32 Index = 1; Index < Samples.Num(); ++Index)
	{
		if (WrappedTime <= Samples[Index].TimeSeconds)
		{
			const FWoWM2ParticleFloatSample& Previous = Samples[Index - 1];
			const FWoWM2ParticleFloatSample& Next = Samples[Index];
			const float SegmentDuration = FMath::Max(Next.TimeSeconds - Previous.TimeSeconds, KINDA_SMALL_NUMBER);
			const float Alpha = FMath::Clamp((WrappedTime - Previous.TimeSeconds) / SegmentDuration, 0.0f, 1.0f);
			return FMath::Lerp(Previous.Value, Next.Value, Alpha);
		}
	}
	return Samples.Last().Value;
}

template <typename TObjectType>
TObjectType ParticleLifeRamp(float LifeRatio, float MidPoint, const TObjectType& A, const TObjectType& B, const TObjectType& C)
{
	const float SafeMid = FMath::Clamp(MidPoint, 0.01f, 0.99f);
	if (LifeRatio <= SafeMid)
	{
		return FMath::Lerp(A, B, FMath::Clamp(LifeRatio / SafeMid, 0.0f, 1.0f));
	}
	return FMath::Lerp(B, C, FMath::Clamp((LifeRatio - SafeMid) / (1.0f - SafeMid), 0.0f, 1.0f));
}

FVector ConvertM2PositionToUE(const FVector& M2Position)
{
	return FVector(
		M2Position.X * WowScalarToUE,
		-M2Position.Y * WowScalarToUE,
		M2Position.Z * WowScalarToUE);
}

FVector ConvertWMVParticleVectorToUE(const FVector& WMVVector)
{
	return FVector(WMVVector.X, WMVVector.Z, WMVVector.Y);
}

bool ShouldKeepParticleInModelSpace(int32 ParticleFlags)
{
	return (ParticleFlags & 0x80) != 0;
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

UMaterialInterface* LoadRuntimeParentMaterial(const TCHAR* ParentPath)
{
	if (!ParentPath)
	{
		return nullptr;
	}

	const FString ParentPathString(ParentPath);
	if (TWeakObjectPtr<UMaterialInterface>* CachedMaterial = GetRuntimeParentMaterialCache().Find(ParentPathString))
	{
		if (UMaterialInterface* Material = CachedMaterial->Get())
		{
			return Material;
		}
	}

	UMaterialInterface* ParentMaterial = ResolveLoadedObject<UMaterialInterface>(ParentPathString);
	if (ParentMaterial)
	{
		GetRuntimeParentMaterialCache().Add(ParentPathString, ParentMaterial);
	}
	return ParentMaterial;
}

}

UWoWM2ModelComponent::UWoWM2ModelComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
}

bool UWoWM2ModelComponent::PrewarmMeshJson(const FString& MeshJsonPath, bool bPrewarmTextures)
{
	const TSharedPtr<FParsedM2ModelData> ParsedData = LoadParsedM2ModelData(MeshJsonPath);
	if (!ParsedData.IsValid())
	{
		return false;
	}

	if (!bPrewarmTextures)
	{
		return true;
	}

	TArray<FString> PreviewPngs;
	for (const FParsedM2Section& Section : ParsedData->Sections)
	{
		if (!Section.PreviewPng.IsEmpty())
		{
			PreviewPngs.AddUnique(Section.PreviewPng);
		}
		for (const FString& LayerPreviewPng : Section.LayerPreviewPngs)
		{
			if (!LayerPreviewPng.IsEmpty())
			{
				PreviewPngs.AddUnique(LayerPreviewPng);
			}
		}
	}
	for (const FWoWM2RibbonEmitter& RibbonEmitter : ParsedData->RibbonEmitters)
	{
		if (!RibbonEmitter.PreviewPng.IsEmpty())
		{
			PreviewPngs.AddUnique(RibbonEmitter.PreviewPng);
		}
	}
	for (const FWoWM2ParticleEmitter& ParticleEmitter : ParsedData->ParticleEmitters)
	{
		if (!ParticleEmitter.PreviewPng.IsEmpty())
		{
			PreviewPngs.AddUnique(ParticleEmitter.PreviewPng);
		}
	}

	for (const FString& PreviewPng : PreviewPngs)
	{
		const FString TexturePath = ResolvePreviewTexturePath(PreviewPng);
		if (TexturePath.IsEmpty())
		{
			continue;
		}

		const FString FullTexturePath = FPaths::ConvertRelativePathToFull(TexturePath);
		if (TWeakObjectPtr<UTexture2D>* SharedTexture = GetRuntimeTextureCache().Find(FullTexturePath))
		{
			if (SharedTexture->IsValid())
			{
				continue;
			}
		}

		UTexture2D* Texture = LoadPngFileAsTransientTexture(FullTexturePath);
		if (!Texture)
		{
			continue;
		}
		Texture->SetFlags(RF_Transient | RF_Public);
		Texture->AddToRoot();
		Texture->SRGB = true;
		Texture->CompressionSettings = TC_Default;
		Texture->Filter = TF_Bilinear;
		Texture->AddressX = TA_Clamp;
		Texture->AddressY = TA_Clamp;
		Texture->UpdateResource();
		GetRuntimeTextureCache().Add(FullTexturePath, Texture);
	}

	return true;
}

void UWoWM2ModelComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	if (!ModelMeshComponent.IsValid() && !RibbonMeshComponent.IsValid() && ParticleStates.Num() == 0)
	{
		SetComponentTickEnabled(false);
		return;
	}

	bool bUpdateMeshSections = true;
	const int32 UpdateHz = CVarWoWM2RuntimeUpdateHz.GetValueOnGameThread();
	if (UpdateHz > 0)
	{
		RuntimeUpdateAccumulatorSeconds += DeltaTime;
		const float UpdateIntervalSeconds = 1.0f / FMath::Clamp(UpdateHz, 1, 240);
		bUpdateMeshSections = RuntimeUpdateAccumulatorSeconds >= UpdateIntervalSeconds;
		if (bUpdateMeshSections)
		{
			RuntimeUpdateAccumulatorSeconds = 0.0f;
		}
	}

	UpdateM2Animation(DeltaTime, bUpdateMeshSections);
	if (CVarWoWM2RuntimeRibbons.GetValueOnGameThread() != 0)
	{
		UpdateM2Ribbons(DeltaTime);
	}
	if (CVarWoWM2RuntimeParticles.GetValueOnGameThread() != 0)
	{
		float ParticleUpdateDeltaTime = DeltaTime;
		bool bUpdateParticles = true;
		const int32 ParticleUpdateHz = CVarWoWM2RuntimeParticleUpdateHz.GetValueOnGameThread();
		if (ParticleUpdateHz > 0)
		{
			ParticleUpdateAccumulatorSeconds += DeltaTime;
			const float ParticleUpdateIntervalSeconds = 1.0f / FMath::Clamp(ParticleUpdateHz, 1, 240);
			bUpdateParticles = ParticleUpdateAccumulatorSeconds >= ParticleUpdateIntervalSeconds;
			if (bUpdateParticles)
			{
				ParticleUpdateDeltaTime = ParticleUpdateAccumulatorSeconds;
				ParticleUpdateAccumulatorSeconds = 0.0f;
			}
		}
		if (bUpdateParticles)
		{
			UpdateM2Particles(ParticleUpdateDeltaTime);
		}
	}
}

void UWoWM2ModelComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearModel();
	Super::EndPlay(EndPlayReason);
}

bool UWoWM2ModelComponent::LoadFromMeshJson(USceneComponent* AttachParent, const FName& SocketName, const FString& MeshJsonPath, const FTransform& RelativeTransform)
{
	ClearModel();
	if (!AttachParent || MeshJsonPath.IsEmpty())
	{
		return false;
	}

	const TSharedPtr<FParsedM2ModelData> ParsedData = LoadParsedM2ModelData(MeshJsonPath);
	if (!ParsedData.IsValid())
	{
		return false;
	}
	AnimationClips = ParsedData->AnimationClips;
	Bones = ParsedData->Bones;

	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return false;
	}

	UProceduralMeshComponent* ModelMesh = NewObject<UProceduralMeshComponent>(Owner);
	ModelMesh->bUseAsyncCooking = true;
	ModelMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	ModelMesh->SetCastShadow(false);
	ModelMesh->AttachToComponent(AttachParent, FAttachmentTransformRules::KeepRelativeTransform, SocketName);
	ModelMesh->SetRelativeTransform(RelativeTransform);
	Owner->AddInstanceComponent(ModelMesh);
	ModelMesh->RegisterComponent();
	ModelMeshComponent = ModelMesh;

	UProceduralMeshComponent* RibbonMesh = nullptr;
	if (ParsedData->RibbonEmitters.Num() > 0)
	{
		RibbonMesh = NewObject<UProceduralMeshComponent>(Owner);
		RibbonMesh->bUseAsyncCooking = true;
		RibbonMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		RibbonMesh->SetCastShadow(false);
		RibbonMesh->AttachToComponent(AttachParent, FAttachmentTransformRules::KeepRelativeTransform, SocketName);
		RibbonMesh->SetRelativeTransform(RelativeTransform);
		Owner->AddInstanceComponent(RibbonMesh);
		RibbonMesh->RegisterComponent();
		RibbonMeshComponent = RibbonMesh;
	}

	int32 CreatedSectionCount = 0;
	for (const FParsedM2Section& Section : ParsedData->Sections)
	{
		if (Section.Positions.Num() == 0 || Section.Indices.Num() == 0 || Section.Indices.Num() % 3 != 0)
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
		ModelMesh->CreateMeshSection_LinearColor(CreatedSectionCount, Section.Positions, Section.Indices, SectionNormals, SectionUV0, SectionVertexColors, Tangents, false);
		ModelMesh->SetMaterial(CreatedSectionCount, CreateRuntimeMaterialForSection(
			Section.PreviewPng,
			Section.LayerPreviewPngs,
			Section.RenderMode,
			Section.bUseEnvMap,
			Section.bTwoSided,
			Section.bUnlit,
			Section.bNoDepthTest,
			nullptr));

		FWoWM2SectionState SectionState;
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
		++CreatedSectionCount;
	}

	if (RibbonMesh)
	{
		for (const FWoWM2RibbonEmitter& RibbonEmitter : ParsedData->RibbonEmitters)
		{
			FWoWM2RibbonRuntimeState State;
			State.Emitter = RibbonEmitter;
			const int32 SectionIndex = RibbonStates.Num();
			RibbonMesh->CreateMeshSection_LinearColor(
				SectionIndex,
				TArray<FVector>(),
				TArray<int32>(),
				TArray<FVector>(),
				TArray<FVector2D>(),
				TArray<FLinearColor>(),
				TArray<FProcMeshTangent>(),
				false);

			int32 RenderMode = 4;
			bool bTwoSided = true;
			bool bUnlit = true;
			bool bNoDepthTest = false;
			for (const int32 MaterialIndex : RibbonEmitter.MaterialIndices)
			{
				if (ApplyMaterialToSection(ParsedData->Materials, MaterialIndex, RenderMode, bTwoSided, bUnlit, bNoDepthTest))
				{
					break;
				}
			}
			State.Material = CreateRuntimeMaterialForSection(
				RibbonEmitter.PreviewPng,
				TArray<FString>(),
				RenderMode,
				false,
				bTwoSided,
				bUnlit,
				bNoDepthTest,
				nullptr);
			RibbonMesh->SetMaterial(SectionIndex, State.Material.Get());
			RibbonStates.Add(MoveTemp(State));
		}
	}

	for (const FWoWM2ParticleEmitter& ParticleEmitter : ParsedData->ParticleEmitters)
	{
		UProceduralMeshComponent* ParticleMesh = NewObject<UProceduralMeshComponent>(Owner);
		if (!ParticleMesh)
		{
			continue;
		}

		ParticleMesh->bUseAsyncCooking = false;
	ParticleMesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		ParticleMesh->SetCastShadow(false);
		ParticleMesh->SetTranslucentSortPriority(1200 + ParticleStates.Num());
		Owner->AddInstanceComponent(ParticleMesh);
		ParticleMesh->RegisterComponent();

		FWoWM2ParticleRuntimeState State;
		State.Emitter = ParticleEmitter;
		State.MeshComponent = ParticleMesh;
		State.Material = CreateRuntimeParticleMaterial(ParticleEmitter, ParticleMesh);
		State.RandomSeed = HashCombine(GetTypeHash(MeshJsonPath), GetTypeHash(ParticleEmitter.Index));
		if (State.Material)
		{
			ParticleMesh->SetMaterial(0, State.Material.Get());
		}
		ParticleStates.Add(MoveTemp(State));
	}

	SourceMeshJsonPath = MeshJsonPath;
	AnimationTimeSeconds = 0.0f;
	CurrentAnimationClipIndex = INDEX_NONE;
	UE_LOG(LogTemp, Verbose, TEXT("M2 model loaded: %s sections=%d clips=%d ribbons=%d particles=%d"), *MeshJsonPath, CreatedSectionCount, AnimationClips.Num(), RibbonStates.Num(), ParticleStates.Num());
	return CreatedSectionCount > 0;
}

int32 UWoWM2ModelComponent::OverrideModelDiffuseTexture(const FString& PreviewPng)
{
	UTexture2D* Texture = LoadRuntimeTexture(PreviewPng);
	UProceduralMeshComponent* ModelMesh = ModelMeshComponent.Get();
	if (!Texture || !ModelMesh)
	{
		return 0;
	}

	int32 AppliedCount = 0;
	const int32 SectionCount = ModelMesh->GetNumSections();
	for (int32 SectionIndex = 0; SectionIndex < SectionCount; ++SectionIndex)
	{
		UMaterialInterface* CurrentMaterial = ModelMesh->GetMaterial(SectionIndex);
		UMaterialInstanceDynamic* DynamicMaterial = Cast<UMaterialInstanceDynamic>(CurrentMaterial);
		if (!DynamicMaterial)
		{
			DynamicMaterial = UMaterialInstanceDynamic::Create(CurrentMaterial ? CurrentMaterial : UMaterial::GetDefaultMaterial(MD_Surface), this);
		}
		if (!DynamicMaterial)
		{
			continue;
		}
		DynamicMaterial->SetTextureParameterValue(PreviewTextureParameterName, Texture);
		ModelMesh->SetMaterial(SectionIndex, DynamicMaterial);
		++AppliedCount;
	}
	return AppliedCount;
}

void UWoWM2ModelComponent::ClearModel()
{
	if (UProceduralMeshComponent* ModelMesh = ModelMeshComponent.Get())
	{
		ModelMesh->DestroyComponent();
	}
	if (UProceduralMeshComponent* RibbonMesh = RibbonMeshComponent.Get())
	{
		RibbonMesh->DestroyComponent();
	}
	for (FWoWM2ParticleRuntimeState& ParticleState : ParticleStates)
	{
		if (UProceduralMeshComponent* ParticleMesh = ParticleState.MeshComponent.Get())
		{
			ParticleMesh->DestroyComponent();
		}
	}

	ModelMeshComponent.Reset();
	RibbonMeshComponent.Reset();
	SectionStates.Reset();
	AnimationClips.Reset();
	CurrentBoneMatrices.Reset();
	Bones.Reset();
	RibbonStates.Reset();
	ParticleStates.Reset();
	TextureCache.Reset();
	DynamicMaterials.Reset();
	SourceMeshJsonPath.Empty();
	AnimationTimeSeconds = 0.0f;
	RuntimeUpdateAccumulatorSeconds = 0.0f;
	ParticleUpdateAccumulatorSeconds = 0.0f;
	CurrentAnimationClipIndex = INDEX_NONE;
}

void UWoWM2ModelComponent::UpdateM2Animation(float DeltaTime, bool bUpdateMeshSections)
{
	UProceduralMeshComponent* ModelMesh = ModelMeshComponent.Get();
	if (!ModelMesh || AnimationClips.Num() == 0)
	{
		AnimationTimeSeconds += DeltaTime;
		return;
	}

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

	const FWoWM2AnimationClip& Clip = AnimationClips[ClipIndex];
	if (Clip.Frames.Num() == 0 || Clip.SampleRate <= 0.0f)
	{
		AnimationTimeSeconds += DeltaTime;
		return;
	}

	AnimationTimeSeconds += DeltaTime;
	int32 FrameIndex = 0;
	int32 NextFrameIndex = 0;
	float FrameAlpha = 0.0f;
	ResolveLoopingClipFrame(Clip, AnimationTimeSeconds, FrameIndex, NextFrameIndex, FrameAlpha);

	CurrentBoneMatrices.Reset();
	if (Clip.Frames.IsValidIndex(FrameIndex) && Clip.Frames.IsValidIndex(NextFrameIndex))
	{
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

	ApplyBillboardBones(CurrentBoneMatrices);

	if (!bUpdateMeshSections || SectionStates.Num() == 0)
	{
		return;
	}

	for (int32 SectionIndex = 0; SectionIndex < SectionStates.Num(); ++SectionIndex)
	{
		FWoWM2SectionState& SectionState = SectionStates[SectionIndex];
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
				if (!CurrentBoneMatrices.IsValidIndex(BoneIndex))
				{
					continue;
				}

				const float Weight = static_cast<float>(WeightByte) / 255.0f;
				SkinnedPosition += TransformPointByExportMatrix(CurrentBoneMatrices[BoneIndex], BasePosition) * Weight;
				TotalWeight += Weight;
			}
			SectionState.SkinnedPositions[VertexIndex] = TotalWeight > KINDA_SMALL_NUMBER ? SkinnedPosition / TotalWeight : BasePosition;
		}

		ApplyTextureTransformToUVs(SectionState.BaseUV0, SectionState.TextureTransformSamples, AnimationTimeSeconds, SectionState.UV0);
		ApplyColorSamples(SectionState.ColorSamples, AnimationTimeSeconds, SectionState.bScaleLowAlphaAdditive, SectionState.VertexColors);
		ModelMesh->UpdateMeshSection_LinearColor(
			SectionIndex,
			SectionState.SkinnedPositions,
			SectionState.Normals,
			SectionState.UV0,
			SectionState.VertexColors,
			SectionState.Tangents);
	}
}

void UWoWM2ModelComponent::ApplyBillboardBones(TArray<FMatrix>& InOutBoneMatrices) const
{
	const UProceduralMeshComponent* ModelMesh = ModelMeshComponent.Get();
	const UWorld* World = GetWorld();
	const APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
	const APlayerCameraManager* CameraManager = PlayerController ? PlayerController->PlayerCameraManager : nullptr;
	if (!ModelMesh || !CameraManager || Bones.Num() == 0)
	{
		return;
	}

	const FRotationMatrix CameraRotation(CameraManager->GetCameraRotation());
	const FTransform ModelToWorld = ModelMesh->GetComponentTransform();
	const FVector CameraRightLocal = ModelToWorld.InverseTransformVectorNoScale(CameraRotation.GetScaledAxis(EAxis::Y)).GetSafeNormal();
	const FVector CameraUpLocal = ModelToWorld.InverseTransformVectorNoScale(CameraRotation.GetScaledAxis(EAxis::Z)).GetSafeNormal();
	if (CameraRightLocal.IsNearlyZero() || CameraUpLocal.IsNearlyZero())
	{
		return;
	}

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
		if (!Bones.IsValidIndex(BoneIndex) || (Bones[BoneIndex].Flags & M2BoneFlagBillboard) == 0)
		{
			continue;
		}

		const FWoWM2BoneMetadata& Bone = Bones[BoneIndex];
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

void UWoWM2ModelComponent::UpdateM2Ribbons(float DeltaTime)
{
	UProceduralMeshComponent* RibbonMesh = RibbonMeshComponent.Get();
	UProceduralMeshComponent* ModelMesh = ModelMeshComponent.Get();
	if (!RibbonMesh || !ModelMesh || RibbonStates.Num() == 0 || CurrentBoneMatrices.Num() == 0)
	{
		return;
	}

	for (int32 RibbonIndex = 0; RibbonIndex < RibbonStates.Num(); ++RibbonIndex)
	{
		FWoWM2RibbonRuntimeState& State = RibbonStates[RibbonIndex];
		const FWoWM2RibbonEmitter& Emitter = State.Emitter;
		if (!CurrentBoneMatrices.IsValidIndex(Emitter.BoneIndex))
		{
			continue;
		}

		const FMatrix& BoneMatrix = CurrentBoneMatrices[Emitter.BoneIndex];
		const FVector ModelLocalPosition = TransformPointByExportMatrix(BoneMatrix, Emitter.Position);
		const FVector ModelLocalUpPosition = TransformPointByExportMatrix(BoneMatrix, Emitter.Position + FVector(0.0f, 0.0f, 1.0f));
		const FTransform ModelToWorld = ModelMesh->GetComponentTransform();
		const FTransform RibbonToWorld = RibbonMesh->GetComponentTransform();
		const FVector CurrentWorldPosition = ModelToWorld.TransformPosition(ModelLocalPosition);
		const FVector UpWorldPosition = ModelToWorld.TransformPosition(ModelLocalUpPosition);
		FVector CurrentUp = (UpWorldPosition - CurrentWorldPosition).GetSafeNormal();
		if (CurrentUp.IsNearlyZero())
		{
			CurrentUp = FVector::UpVector;
		}

		if (!State.bHasLastPosition || State.Segments.Num() == 0)
		{
			State.Segments.Reset();
			FWoWM2RibbonSegment InitialSegment;
			InitialSegment.Position = CurrentWorldPosition;
			InitialSegment.Up = CurrentUp;
			State.Segments.Add(InitialSegment);
			State.LastPosition = CurrentWorldPosition;
			State.TimeSinceLastEdgeSeconds = 0.0f;
			State.bHasLastPosition = true;
		}

		const float MovementLength = FVector::Distance(CurrentWorldPosition, State.LastPosition);
		for (FWoWM2RibbonSegment& Segment : State.Segments)
		{
			Segment.AgeSeconds += DeltaTime;
		}

		State.TimeSinceLastEdgeSeconds += DeltaTime;
		FWoWM2RibbonSegment& FirstSegment = State.Segments[0];
		const float EdgeIntervalSeconds = 1.0f / FMath::Max(Emitter.EdgesPerSecond, 1.0f);
		const bool bMovedEnoughForNewEdge = MovementLength >= RibbonMinEdgeDistanceWorldUnits;
		const int32 EdgeSpawnCount = bMovedEnoughForNewEdge
			? FMath::Clamp(FMath::FloorToInt(State.TimeSinceLastEdgeSeconds / EdgeIntervalSeconds), 0, 8)
			: 0;
		if (EdgeSpawnCount > 0)
		{
			FirstSegment.Back = (State.LastPosition - CurrentWorldPosition).GetSafeNormal();
			FirstSegment.OriginalLength = FMath::Max(FirstSegment.Length, 1.0f);
			const FVector PreviousPosition = State.LastPosition;
			for (int32 EdgeIndex = 1; EdgeIndex <= EdgeSpawnCount; ++EdgeIndex)
			{
				const float Alpha = static_cast<float>(EdgeIndex) / static_cast<float>(EdgeSpawnCount);
				const FVector EdgePosition = FMath::Lerp(PreviousPosition, CurrentWorldPosition, Alpha);
				FWoWM2RibbonSegment NewSegment;
				NewSegment.Position = EdgePosition;
				NewSegment.Up = CurrentUp;
				NewSegment.Length = FVector::Distance(EdgePosition, State.Segments[0].Position);
				State.Segments.Insert(NewSegment, 0);
			}
			State.TimeSinceLastEdgeSeconds = FMath::Fmod(State.TimeSinceLastEdgeSeconds, EdgeIntervalSeconds);
		}
		else
		{
			FirstSegment.Position = CurrentWorldPosition;
			FirstSegment.Up = CurrentUp;
			FirstSegment.Length += MovementLength;
			if (!bMovedEnoughForNewEdge)
			{
				State.TimeSinceLastEdgeSeconds = FMath::Min(State.TimeSinceLastEdgeSeconds, EdgeIntervalSeconds);
			}
		}

		for (int32 SegmentIndex = State.Segments.Num() - 1; SegmentIndex >= 0; --SegmentIndex)
		{
			if (State.Segments[SegmentIndex].AgeSeconds <= Emitter.EdgeLifetimeSeconds)
			{
				break;
			}
			State.Segments.RemoveAt(SegmentIndex);
		}
		State.LastPosition = CurrentWorldPosition;

		const FWoWM2RibbonSample RibbonSample = SampleRibbonEmitter(Emitter, AnimationTimeSeconds);
		const bool bRibbonVisible = SampleRibbonVisibility(Emitter, AnimationTimeSeconds);
		const int32 RibbonTexSlot = SampleRibbonTexSlot(Emitter, AnimationTimeSeconds);
		const int32 TextureRows = FMath::Max(Emitter.TextureRows, 1);
		const int32 TextureCols = FMath::Max(Emitter.TextureCols, 1);
		const int32 ClampedTexSlot = FMath::Clamp(RibbonTexSlot, 0, TextureRows * TextureCols - 1);
		const int32 TileColumn = ClampedTexSlot % TextureCols;
		const int32 TileRow = ClampedTexSlot / TextureCols;

		const int32 SegmentCount = State.Segments.Num();
		TArray<FVector> Positions;
		TArray<int32> Indices;
		TArray<FVector> Normals;
		TArray<FVector2D> UV0;
		TArray<FLinearColor> VertexColors;
		TArray<FProcMeshTangent> Tangents;
		if (bRibbonVisible && SegmentCount >= 2)
		{
			Positions.Reserve(SegmentCount * 2);
			Normals.Init(FVector::UpVector, SegmentCount * 2);
			UV0.Reserve(SegmentCount * 2);
			VertexColors.Init(RibbonSample.Color, SegmentCount * 2);
			Tangents.Init(FProcMeshTangent(1.0f, 0.0f, 0.0f), SegmentCount * 2);

			float RibbonLength = 0.0f;
			for (const FWoWM2RibbonSegment& Segment : State.Segments)
			{
				RibbonLength += Segment.Length;
			}
			float U = 0.0f;
			for (int32 SegmentIndex = 0; SegmentIndex < SegmentCount; ++SegmentIndex)
			{
				const FWoWM2RibbonSegment& Segment = State.Segments[SegmentIndex];
				const float UCoord = RibbonLength > KINDA_SMALL_NUMBER ? U / RibbonLength : 0.0f;
				Positions.Add(RibbonToWorld.InverseTransformPosition(Segment.Position + RibbonSample.Above * Segment.Up));
				Positions.Add(RibbonToWorld.InverseTransformPosition(Segment.Position - RibbonSample.Below * Segment.Up));
				UV0.Add(FVector2D((TileColumn + UCoord) / TextureCols, static_cast<float>(TileRow) / TextureRows));
				UV0.Add(FVector2D((TileColumn + UCoord) / TextureCols, static_cast<float>(TileRow + 1) / TextureRows));
				U += Segment.Length;
			}
			const FWoWM2RibbonSegment& LastSegment = State.Segments.Last();
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

int32 UWoWM2ModelComponent::ResolveAnimationClipIndex() const
{
	for (int32 Index = 0; Index < AnimationClips.Num(); ++Index)
	{
		const FWoWM2AnimationClip& Clip = AnimationClips[Index];
		if (Clip.AnimationId == 0 || Clip.LookupIds.Contains(0))
		{
			return Index;
		}
	}
	return AnimationClips.Num() > 0 ? 0 : INDEX_NONE;
}

FWoWM2RibbonSample UWoWM2ModelComponent::SampleRibbonEmitter(const FWoWM2RibbonEmitter& Emitter, float TimeSeconds) const
{
	if (Emitter.Samples.Num() == 0)
	{
		return FWoWM2RibbonSample();
	}

	const float WrappedTime = Emitter.LengthSeconds > KINDA_SMALL_NUMBER
		? FMath::Fmod(FMath::Max(TimeSeconds, 0.0f), Emitter.LengthSeconds)
		: 0.0f;
	for (int32 Index = 1; Index < Emitter.Samples.Num(); ++Index)
	{
		if (WrappedTime <= Emitter.Samples[Index].TimeSeconds)
		{
			const FWoWM2RibbonSample& Previous = Emitter.Samples[Index - 1];
			const FWoWM2RibbonSample& Next = Emitter.Samples[Index];
			const float Duration = FMath::Max(Next.TimeSeconds - Previous.TimeSeconds, KINDA_SMALL_NUMBER);
			const float Alpha = FMath::Clamp((WrappedTime - Previous.TimeSeconds) / Duration, 0.0f, 1.0f);
			FWoWM2RibbonSample Result;
			Result.TimeSeconds = WrappedTime;
			Result.Color = FMath::Lerp(Previous.Color, Next.Color, Alpha);
			Result.Above = FMath::Lerp(Previous.Above, Next.Above, Alpha);
			Result.Below = FMath::Lerp(Previous.Below, Next.Below, Alpha);
			return Result;
		}
	}

	return Emitter.Samples.Last();
}

int32 UWoWM2ModelComponent::SampleRibbonTexSlot(const FWoWM2RibbonEmitter& Emitter, float TimeSeconds) const
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

bool UWoWM2ModelComponent::SampleRibbonVisibility(const FWoWM2RibbonEmitter& Emitter, float TimeSeconds) const
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

void UWoWM2ModelComponent::UpdateM2Particles(float DeltaTime)
{
	if (ParticleStates.Num() == 0)
	{
		return;
	}

	const UWorld* World = GetWorld();
	const APlayerController* PlayerController = World ? World->GetFirstPlayerController() : nullptr;
	const APlayerCameraManager* CameraManager = PlayerController ? PlayerController->PlayerCameraManager : nullptr;
	if (!CameraManager)
	{
		return;
	}

	const float SafeDeltaSeconds = FMath::Clamp(DeltaTime, 0.0f, 1.0f / 20.0f);
	const FRotationMatrix CameraRotation(CameraManager->GetCameraRotation());
	const FVector CameraRight = CameraRotation.GetScaledAxis(EAxis::Y);
	const FVector CameraUp = CameraRotation.GetScaledAxis(EAxis::Z);
	const FVector CameraLocation = CameraManager->GetCameraLocation();

	for (FWoWM2ParticleRuntimeState& State : ParticleStates)
	{
		UProceduralMeshComponent* ParticleMesh = State.MeshComponent.Get();
		if (!ParticleMesh)
		{
			continue;
		}

		const FWoWM2ParticleEmitter& Emitter = State.Emitter;
		const float Lifespan = FMath::Max(SampleParticleFloatTrack(Emitter.LifespanSamples, AnimationTimeSeconds, 0.0f), 0.1f);
		const float RawEmissionRate = FMath::Max(SampleParticleFloatTrack(Emitter.EmissionRateSamples, AnimationTimeSeconds, 0.0f), 0.0f);
		const float SpawnUnits = (SafeDeltaSeconds * RawEmissionRate / Lifespan) + State.SpawnRemainder;
		const int32 SpawnCount = FMath::Min(FMath::FloorToInt(SpawnUnits), MaxItemParticlesPerEmitter - State.Particles.Num());
		State.SpawnRemainder = FMath::Max(0.0f, SpawnUnits - static_cast<float>(SpawnCount));

		const float RawSpeed = SampleParticleFloatTrack(Emitter.EmissionSpeedSamples, AnimationTimeSeconds, 0.0f);
		const float Variation = SampleParticleFloatTrack(Emitter.SpeedVariationSamples, AnimationTimeSeconds, 0.0f);
		const float AreaLengthM2 = SampleParticleFloatTrack(Emitter.EmissionAreaLengthSamples, AnimationTimeSeconds, 1.0f) * 0.5f;
		const float AreaWidthM2 = SampleParticleFloatTrack(Emitter.EmissionAreaWidthSamples, AnimationTimeSeconds, 1.0f) * 0.5f;
		const float Gravity = SampleParticleFloatTrack(Emitter.GravitySamples, AnimationTimeSeconds, 0.0f) * WowScalarToUE;
		const float Deaccel = SampleParticleFloatTrack(Emitter.Gravity2Samples, AnimationTimeSeconds, 0.0f) * WowScalarToUE;
		const bool bModelSpaceParticle = ShouldKeepParticleInModelSpace(Emitter.Flags);
		const int32 TileCount = FMath::Max(Emitter.TextureColumns * Emitter.TextureRows, 1);

		if (SpawnCount > 0)
		{
			FRandomStream Random(State.RandomSeed + FMath::FloorToInt(AnimationTimeSeconds * 1000.0f));
			const FVector EmitterWorldPosition = GetM2BoneWorldPosition(Emitter.BoneIndex, Emitter.Position);
			const FQuat BoneWorldRotation = GetM2BoneWorldRotation(Emitter.BoneIndex);
			const FVector BaseDirUE = ConvertWMVParticleVectorToUE(FVector(0.0f, 1.0f, 0.0f));
			for (int32 Index = 0; Index < SpawnCount; ++Index)
			{
				const FVector AreaOffsetModelUE = ConvertM2PositionToUE(FVector(
					Random.FRandRange(-AreaLengthM2, AreaLengthM2),
					0.0f,
					Random.FRandRange(-AreaWidthM2, AreaWidthM2)));
				const FVector AreaOffsetWorld = BoneWorldRotation.RotateVector(AreaOffsetModelUE);
				const FVector Direction = BoneWorldRotation.RotateVector(BaseDirUE).GetSafeNormal(UE_SMALL_NUMBER, BaseDirUE);

				FWoWM2ParticleInstance Particle;
				Particle.Position = bModelSpaceParticle ? AreaOffsetModelUE : EmitterWorldPosition + AreaOffsetWorld;
				Particle.Origin = Particle.Position;
				Particle.Direction = Direction;
				Particle.Velocity = Direction * (RawSpeed * WowScalarToUE) * (1.0f + Random.FRandRange(-Variation, Variation));
				Particle.Down = ConvertWMVParticleVectorToUE(FVector(0.0f, -1.0f, 0.0f));
				Particle.Lifespan = Lifespan;
				Particle.InitialTileIndex = Random.RandRange(0, TileCount - 1);
				State.Particles.Add(Particle);
			}
		}

		for (int32 ParticleIndex = State.Particles.Num() - 1; ParticleIndex >= 0; --ParticleIndex)
		{
			FWoWM2ParticleInstance& Particle = State.Particles[ParticleIndex];
			Particle.Velocity += Particle.Down * Gravity * SafeDeltaSeconds;
			Particle.Velocity -= Particle.Direction * Deaccel * SafeDeltaSeconds;
			const float MoveSpeed = Emitter.Slowdown > 0.0f ? FMath::Exp(-Emitter.Slowdown * Particle.Age) : 1.0f;
			Particle.Position += Particle.Velocity * MoveSpeed * SafeDeltaSeconds;
			Particle.Age += SafeDeltaSeconds;
			if (Particle.Age >= Particle.Lifespan)
			{
				State.Particles.RemoveAtSwap(ParticleIndex, 1, EAllowShrinking::No);
			}
		}

		TArray<FVector> Vertices;
		TArray<int32> Triangles;
		TArray<FVector> Normals;
		TArray<FVector2D> UVs;
		TArray<FLinearColor> Colors;
		TArray<FProcMeshTangent> Tangents;
		Vertices.Reserve(MaxItemParticlesPerEmitter * 4);
		Triangles.Reserve(MaxItemParticlesPerEmitter * 6);
		Normals.Reserve(MaxItemParticlesPerEmitter * 4);
		UVs.Reserve(MaxItemParticlesPerEmitter * 4);
		Colors.Reserve(MaxItemParticlesPerEmitter * 4);
		Tangents.Reserve(MaxItemParticlesPerEmitter * 4);

		const FQuat EmitterWorldRotation = GetM2BoneWorldRotation(Emitter.BoneIndex);
		const FVector EmitterWorldPosition = GetM2BoneWorldPosition(Emitter.BoneIndex, Emitter.Position);
		const bool bBillboard = (Emitter.Flags & 0x1000) == 0;
		const FVector NonBillboardRight = EmitterWorldRotation.RotateVector(FVector::RightVector);
		const FVector NonBillboardUp = EmitterWorldRotation.RotateVector(FVector::UpVector);

		for (const FWoWM2ParticleInstance& Particle : State.Particles)
		{
			const FVector ParticleWorldPosition = bModelSpaceParticle
				? EmitterWorldPosition + EmitterWorldRotation.RotateVector(Particle.Position)
				: Particle.Position;
			const float LifeRatio = FMath::Clamp(Particle.Age / FMath::Max(Particle.Lifespan, KINDA_SMALL_NUMBER), 0.0f, 1.0f);
			const float M2Size = ParticleLifeRamp<float>(LifeRatio, Emitter.MidPoint, Emitter.LifecycleSizes[0], Emitter.LifecycleSizes[1], Emitter.LifecycleSizes[2]);
			const float HalfSize = M2Size * WowScalarToUE;
			const FLinearColor ParticleColor = ParticleLifeRamp<FLinearColor>(LifeRatio, Emitter.MidPoint, Emitter.LifecycleColors[0], Emitter.LifecycleColors[1], Emitter.LifecycleColors[2]);
			const int32 FrameOffset = TileCount > 1 ? FMath::Clamp(FMath::FloorToInt(LifeRatio * static_cast<float>(TileCount)), 0, TileCount - 1) : 0;
			const int32 TileIndex = (Particle.InitialTileIndex + FrameOffset) % TileCount;
			FVector2D TileUVs[4];
			BuildParticleTileUVs(TileIndex, Emitter.TextureColumns, Emitter.TextureRows, TileUVs);
			ApplyParticleTileRotation(TileUVs, Emitter.TextureTileRotation);

			const FVector QuadRight = bBillboard ? CameraRight : NonBillboardRight;
			const FVector QuadUp = bBillboard ? CameraUp : NonBillboardUp;
			const FVector QuadVertices[4] =
			{
				ParticleWorldPosition - QuadRight * HalfSize - QuadUp * HalfSize,
				ParticleWorldPosition + QuadRight * HalfSize - QuadUp * HalfSize,
				ParticleWorldPosition + QuadRight * HalfSize + QuadUp * HalfSize,
				ParticleWorldPosition - QuadRight * HalfSize + QuadUp * HalfSize
			};

			const int32 BaseVertex = Vertices.Num();
			for (int32 Corner = 0; Corner < 4; ++Corner)
			{
				Vertices.Add(ParticleMesh->GetComponentTransform().InverseTransformPosition(QuadVertices[Corner]));
				Normals.Add((CameraLocation - ParticleWorldPosition).GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector));
				UVs.Add(TileUVs[Corner]);
				Colors.Add(ParticleColor);
				Tangents.Add(FProcMeshTangent(1.0f, 0.0f, 0.0f));
			}
			Triangles.Append({ BaseVertex + 0, BaseVertex + 2, BaseVertex + 1, BaseVertex + 0, BaseVertex + 3, BaseVertex + 2 });
		}

		if (Vertices.Num() == 0)
		{
			if (State.bMeshSectionInitialized)
			{
				ParticleMesh->ClearMeshSection(0);
				State.bMeshSectionInitialized = false;
				State.MeshSectionVertexCount = 0;
			}
		}
		else if (!State.bMeshSectionInitialized || State.MeshSectionVertexCount != Vertices.Num())
		{
			if (State.bMeshSectionInitialized)
			{
				ParticleMesh->ClearMeshSection(0);
			}
			ParticleMesh->CreateMeshSection_LinearColor(0, Vertices, Triangles, Normals, UVs, Colors, Tangents, false);
			State.bMeshSectionInitialized = true;
			State.MeshSectionVertexCount = Vertices.Num();
		}
		else
		{
			ParticleMesh->UpdateMeshSection_LinearColor(0, Vertices, Normals, UVs, Colors, Tangents);
		}
	}
}

FVector UWoWM2ModelComponent::GetM2BoneWorldPosition(int32 BoneIndex, const FVector& M2ModelPosition) const
{
	const UProceduralMeshComponent* ModelMesh = ModelMeshComponent.Get();
	if (!ModelMesh)
	{
		return M2ModelPosition;
	}
	if (CurrentBoneMatrices.IsValidIndex(BoneIndex))
	{
		return ModelMesh->GetComponentTransform().TransformPosition(TransformPointByExportMatrix(CurrentBoneMatrices[BoneIndex], M2ModelPosition));
	}
	return ModelMesh->GetComponentTransform().TransformPosition(M2ModelPosition);
}

FQuat UWoWM2ModelComponent::GetM2BoneWorldRotation(int32 BoneIndex) const
{
	const UProceduralMeshComponent* ModelMesh = ModelMeshComponent.Get();
	const FQuat ModelRotation = ModelMesh ? ModelMesh->GetComponentQuat() : FQuat::Identity;
	if (!CurrentBoneMatrices.IsValidIndex(BoneIndex))
	{
		return ModelRotation;
	}

	const FMatrix& Matrix = CurrentBoneMatrices[BoneIndex];
	const FVector AxisX(Matrix.M[0][0], Matrix.M[1][0], Matrix.M[2][0]);
	const FVector AxisZ(Matrix.M[0][2], Matrix.M[1][2], Matrix.M[2][2]);
	if (AxisX.IsNearlyZero() || AxisZ.IsNearlyZero())
	{
		return ModelRotation;
	}
	return ModelRotation * FRotationMatrix::MakeFromXZ(AxisX.GetSafeNormal(), AxisZ.GetSafeNormal()).ToQuat();
}

UTexture2D* UWoWM2ModelComponent::LoadRuntimeTexture(const FString& PreviewPng)
{
	const FString TexturePath = ResolvePreviewTexturePath(PreviewPng);
	if (TexturePath.IsEmpty())
	{
		return nullptr;
	}
	const FString FullTexturePath = FPaths::ConvertRelativePathToFull(TexturePath);
	if (TObjectPtr<UTexture2D>* CachedTexture = TextureCache.Find(FullTexturePath))
	{
		return CachedTexture->Get();
	}
	if (TWeakObjectPtr<UTexture2D>* SharedTexture = GetRuntimeTextureCache().Find(FullTexturePath))
	{
		if (UTexture2D* Texture = SharedTexture->Get())
		{
			TextureCache.Add(FullTexturePath, Texture);
			return Texture;
		}
	}

	UTexture2D* Texture = LoadPngFileAsTransientTexture(FullTexturePath);
	if (!Texture)
	{
		return nullptr;
	}

	Texture->SetFlags(RF_Transient | RF_Public);
	Texture->AddToRoot();
	Texture->SRGB = true;
	Texture->CompressionSettings = TC_Default;
	Texture->Filter = TF_Bilinear;
	Texture->AddressX = TA_Clamp;
	Texture->AddressY = TA_Clamp;
	Texture->UpdateResource();
	GetRuntimeTextureCache().Add(FullTexturePath, Texture);
	TextureCache.Add(FullTexturePath, Texture);
	return Texture;
}

UMaterialInterface* UWoWM2ModelComponent::CreateRuntimeMaterialForSection(
	const FString& PreviewPng,
	const TArray<FString>& LayerPreviewPngs,
	int32 RenderMode,
	bool bUseEnvMap,
	bool bTwoSided,
	bool bUnlit,
	bool bNoDepthTest,
	const FLinearColor* TintColor)
{
	const TCHAR* ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2Opaque.M_WoWM2Opaque");
	if (RenderMode == 4)
	{
		ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2AdditiveAlphaUnlit.M_WoWM2AdditiveAlphaUnlit");
	}
	else if (RenderMode == 3 || RenderMode == 5 || RenderMode == 6)
	{
		ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2AdditiveSrcColorUnlit.M_WoWM2AdditiveSrcColorUnlit");
	}
	else if (RenderMode == 2)
	{
		ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2AlphaBlendUnlit.M_WoWM2AlphaBlendUnlit");
	}
	else if (RenderMode == 1)
	{
		ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2MaskedTwoSided.M_WoWM2MaskedTwoSided");
	}
	else if (RenderMode != 0)
	{
		ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2AlphaBlendUnlit.M_WoWM2AlphaBlendUnlit");
	}
	(void)bUseEnvMap;
	(void)bTwoSided;
	(void)bUnlit;
	(void)bNoDepthTest;

	UMaterialInterface* ParentMaterial = LoadRuntimeParentMaterial(ParentPath);
	if (!ParentMaterial)
	{
		ParentMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
	}

	UTexture2D* Texture = LoadRuntimeTexture(PreviewPng);
	if (!Texture)
	{
		return ParentMaterial;
	}

	UMaterialInstanceDynamic* DynamicMaterial = UMaterialInstanceDynamic::Create(ParentMaterial, this);
	if (!DynamicMaterial)
	{
		return ParentMaterial;
	}
	DynamicMaterial->SetTextureParameterValue(PreviewTextureParameterName, Texture);
	if (LayerPreviewPngs.Num() > 1)
	{
		if (UTexture2D* LayerTexture = LoadRuntimeTexture(LayerPreviewPngs[1]))
		{
			DynamicMaterial->SetTextureParameterValue(TEXT("LayerTexture1"), LayerTexture);
		}
	}
	DynamicMaterial->SetScalarParameterValue(TEXT("LayerAlphaWeight"), 0.0f);
	DynamicMaterial->SetScalarParameterValue(TEXT("LayerColorWeight"), 0.0f);
	DynamicMaterial->SetScalarParameterValue(TEXT("Brightness"), 1.0f);
	DynamicMaterial->SetScalarParameterValue(TEXT("AlphaCutoff"), 0.0f);
	DynamicMaterial->SetScalarParameterValue(TEXT("LegacyGammaWeight"), 0.0f);
	DynamicMaterial->SetVectorParameterValue(PreviewTintParameterName, FLinearColor::White);
	if (TintColor)
	{
		FLinearColor EffectiveTint = *TintColor;
		if (RenderMode == 4 && EffectiveTint.A > 0.0f && EffectiveTint.A < 0.25f)
		{
			EffectiveTint.A *= M2LowAlphaAdditiveTintScale;
		}
		DynamicMaterial->SetVectorParameterValue(PreviewTintParameterName, EffectiveTint);
	}
	DynamicMaterials.Add(DynamicMaterial);
	return DynamicMaterial;
}

UMaterialInterface* UWoWM2ModelComponent::CreateRuntimeParticleMaterial(const FWoWM2ParticleEmitter& Emitter, UProceduralMeshComponent* MeshComponent)
{
	const TCHAR* ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2AlphaBlendUnlit.M_WoWM2AlphaBlendUnlit");
	if (Emitter.Blend == 4)
	{
		ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2AdditiveAlphaUnlit.M_WoWM2AdditiveAlphaUnlit");
	}
	else if (Emitter.Blend == 3 || Emitter.Blend == 5 || Emitter.Blend == 6)
	{
		ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2AdditiveSrcColorUnlit.M_WoWM2AdditiveSrcColorUnlit");
	}

	UMaterialInterface* ParentMaterial = LoadRuntimeParentMaterial(ParentPath);
	if (!ParentMaterial)
	{
		ParentMaterial = UMaterial::GetDefaultMaterial(MD_Surface);
	}

	UTexture2D* Texture = LoadRuntimeTexture(Emitter.PreviewPng);
	if (!Texture)
	{
		return ParentMaterial;
	}

	UMaterialInstanceDynamic* DynamicMaterial = UMaterialInstanceDynamic::Create(ParentMaterial, MeshComponent ? static_cast<UObject*>(MeshComponent) : static_cast<UObject*>(this));
	if (!DynamicMaterial)
	{
		return ParentMaterial;
	}
	DynamicMaterial->SetTextureParameterValue(PreviewTextureParameterName, Texture);
	DynamicMaterial->SetVectorParameterValue(PreviewTintParameterName, FLinearColor::White);
	DynamicMaterial->SetScalarParameterValue(TEXT("Brightness"), 1.0f);
	DynamicMaterial->SetScalarParameterValue(TEXT("AlphaCutoff"), 0.0f);
	DynamicMaterial->SetScalarParameterValue(TEXT("LegacyGammaWeight"), 0.0f);
	DynamicMaterials.Add(DynamicMaterial);
	return DynamicMaterial;
}
