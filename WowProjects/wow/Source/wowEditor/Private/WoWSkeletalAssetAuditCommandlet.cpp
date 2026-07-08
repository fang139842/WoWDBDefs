#include "WoWSkeletalAssetAuditCommandlet.h"

#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AnimationUtils.h"
#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetToolsModule.h"
#include "AutomatedAssetImportData.h"
#include "BoneWeights.h"
#include "Dom/JsonObject.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "MeshDescription.h"
#include "MeshDescriptionBuilder.h"
#include "ReferenceSkeleton.h"
#include "SkeletalMeshAttributes.h"
#include "StaticToSkeletalMeshConverter.h"
#include "Materials/MaterialInstanceConstant.h"
#include "Materials/MaterialInterface.h"
#include "Materials/Material.h"
#include "Features/IModularFeatures.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/PackageName.h"
#include "Misc/Parse.h"
#include "Misc/Paths.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "UObject/SavePackage.h"

DEFINE_LOG_CATEGORY_STATIC(LogWoWSkeletalAssetAudit, Log, All);

namespace
{
struct FWoWM2AuditBone
{
	int32 Index = INDEX_NONE;
	int32 Parent = INDEX_NONE;
	int32 Flags = 0;
	FVector Pivot = FVector::ZeroVector;
};

struct FWoWM2AuditAttachment
{
	int32 Index = INDEX_NONE;
	int32 Id = INDEX_NONE;
	int32 BoneIndex = INDEX_NONE;
	FVector Position = FVector::ZeroVector;
};

struct FWoWM2AuditSection
{
	int32 SectionIndex = INDEX_NONE;
	int32 SubmeshId = 0;
	int32 BatchFlags = 0;
	int32 PriorityPlane = 0;
	int32 ShaderId = 0;
	int32 MaterialIndex = INDEX_NONE;
	int32 MaterialLayer = 0;
	int32 MaterialFlags = 0;
	int32 RenderMode = 0;
	int32 TextureCount = 0;
	int32 TextureComboIndex = INDEX_NONE;
	int32 TextureCoordComboIndex = INDEX_NONE;
	int32 TextureWeightComboIndex = INDEX_NONE;
	int32 TextureTransformComboIndex = INDEX_NONE;
	int32 TextureUnit = INDEX_NONE;
	int32 ColorIndex = INDEX_NONE;
	int32 TransparencyIndex = INDEX_NONE;
	int32 TextureTransformIndex = INDEX_NONE;
	int32 TextureIndex = INDEX_NONE;
	int32 TextureFlags = 0;
	int32 TriangleCount = 0;
	bool bTwoSided = false;
	bool bUnlit = false;
	bool bNoDepthTest = false;
	bool bNoDepthWrite = false;
	FString PreviewPng;
	TArray<FString> LayerPreviewPngs;
	TArray<int32> LayerTextureFlags;
	TArray<FVector3f> Positions;
	TArray<FVector3f> Normals;
	TArray<FVector2f> UVs;
	TArray<FVector4f> VertexColors;
	TArray<int32> Indices;
	TArray<TArray<int32>> BoneIndices;
	TArray<TArray<int32>> BoneWeights;
};

bool ReadJsonObjectFile(const FString& InputPath, TSharedPtr<FJsonObject>& OutRootObject);

float ResolveLayerAlphaWeight(const FWoWM2AuditSection& Section)
{
	// 中文说明：当前还没有把 WotLK shader_id/mode/shading 的固定管线 combiner
	// 完整映射到 UE 母材质。把第二层临时当 alpha/颜色权重使用会破坏原始 M2 语义，
	// Northrend 的 SNOW_SHIELD2 + T_VFX_SMOKE_C 就会被错误桥接成大块烟雾。
	// 在得到权威 combiner 解析前，这里只保留第二层贴图资源到资产，不参与错误混合。
	(void)Section;
	return 0.0f;
}

float ResolveLayerColorWeight(const FWoWM2AuditSection& Section)
{
	(void)Section;
	return 0.0f;
}

struct FWoWM2AuditParticleEmitter
{
	int32 Index = INDEX_NONE;
	FString PreviewPng;
};

struct FWoWM2AuditAnimationClip
{
	FString Key;
	int32 SequenceIndex = INDEX_NONE;
	int32 AnimationId = INDEX_NONE;
	int32 LogicalAnimationId = INDEX_NONE;
	int32 SubAnimationId = 0;
	int32 LengthMs = 0;
	float SampleRate = 20.0f;
	int32 FrameCount = 0;
	int32 BoneCount = 0;
	TArray<int32> LookupIds;
	TArray<TArray<FMatrix>> Frames;
};

struct FWoWM2GeneratedAnimationCatalogEntry
{
	int32 LogicalAnimationId = INDEX_NONE;
	int32 SourceAnimationId = INDEX_NONE;
	int32 SubAnimationId = 0;
	int32 SequenceIndex = INDEX_NONE;
	int32 LookupSequenceIndex = INDEX_NONE;
	int32 LengthMs = 0;
	float SampleRate = 0.0f;
	int32 FrameCount = 0;
	FString Name;
	FString ObjectPath;
	FString Variant;
};

enum class EWoWGeneratedGripVariant : uint8
{
	None,
	RightHand,
	LeftHand,
};

enum class EWoWAnimIdSelectionMode : uint8
{
	Explicit,
	Lookup,
	AllSequences,
};

struct FWoWAnimIdSelection
{
	EWoWAnimIdSelectionMode Mode = EWoWAnimIdSelectionMode::Explicit;
	TSet<int32> ExplicitIds;
};

class FScopedLegacyAnimDataModel
{
public:
	FScopedLegacyAnimDataModel()
	{
		const FName FeatureName = UE::Anim::DataModel::IAnimationDataModels::GetModularFeatureName();
		IModularFeatures& ModularFeatures = IModularFeatures::Get();
		DisabledModels = ModularFeatures.GetModularFeatureImplementations<UE::Anim::DataModel::IAnimationDataModels>(FeatureName);

		for (UE::Anim::DataModel::IAnimationDataModels* Model : DisabledModels)
		{
			if (Model)
			{
				ModularFeatures.UnregisterModularFeature(FeatureName, Model);
			}
		}
	}

	~FScopedLegacyAnimDataModel()
	{
		const FName FeatureName = UE::Anim::DataModel::IAnimationDataModels::GetModularFeatureName();
		IModularFeatures& ModularFeatures = IModularFeatures::Get();

		for (UE::Anim::DataModel::IAnimationDataModels* Model : DisabledModels)
		{
			if (Model)
			{
				ModularFeatures.RegisterModularFeature(FeatureName, Model);
			}
		}
	}

private:
	TArray<UE::Anim::DataModel::IAnimationDataModels*> DisabledModels;
};

int32 CountArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || Values == nullptr)
	{
		return 0;
	}
	return Values->Num();
}

FString GetStringFieldOrEmpty(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName)
{
	FString Value;
	if (Object.IsValid())
	{
		Object->TryGetStringField(FieldName, Value);
	}
	return Value;
}

bool ReadVectorField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, FVector& OutValue)
{
	const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Tuple) || Tuple == nullptr || Tuple->Num() != 3)
	{
		return false;
	}

	OutValue = FVector((*Tuple)[0]->AsNumber(), (*Tuple)[1]->AsNumber(), (*Tuple)[2]->AsNumber());
	return true;
}

bool ReadVector3fArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FVector3f>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || Values == nullptr)
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
		if (!Value.IsValid() || !Value->TryGetArray(Tuple) || Tuple == nullptr || Tuple->Num() != 3)
		{
			return false;
		}
		OutValues.Add(FVector3f(
			static_cast<float>((*Tuple)[0]->AsNumber()),
			static_cast<float>((*Tuple)[1]->AsNumber()),
			static_cast<float>((*Tuple)[2]->AsNumber())));
	}

	return true;
}

bool ReadVector2fArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FVector2f>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || Values == nullptr)
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
		if (!Value.IsValid() || !Value->TryGetArray(Tuple) || Tuple == nullptr || Tuple->Num() != 2)
		{
			return false;
		}
		OutValues.Add(FVector2f(
			static_cast<float>((*Tuple)[0]->AsNumber()),
			static_cast<float>((*Tuple)[1]->AsNumber())));
	}

	return true;
}

bool ReadVector4fArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<FVector4f>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || Values == nullptr)
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
		if (!Value.IsValid() || !Value->TryGetArray(Tuple) || Tuple == nullptr || Tuple->Num() != 4)
		{
			return false;
		}
		OutValues.Add(FVector4f(
			static_cast<float>((*Tuple)[0]->AsNumber()),
			static_cast<float>((*Tuple)[1]->AsNumber()),
			static_cast<float>((*Tuple)[2]->AsNumber()),
			static_cast<float>((*Tuple)[3]->AsNumber())));
	}

	return true;
}

bool ReadIntArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<int32>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || Values == nullptr)
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		// 中文说明：M2 lookup 表允许缺项。JSON 缓存里可能用 null 表示空槽，导入 UE 时统一成 INDEX_NONE。
		OutValues.Add(Value.IsValid() && Value->Type == EJson::Number ? FMath::RoundToInt(Value->AsNumber()) : INDEX_NONE);
	}

	return true;
}

bool ReadIntTupleArrayField(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<TArray<int32>>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, Values) || Values == nullptr)
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TArray<TSharedPtr<FJsonValue>>* Tuple = nullptr;
		if (!Value.IsValid() || !Value->TryGetArray(Tuple) || Tuple == nullptr)
		{
			return false;
		}

		TArray<int32> Row;
		Row.Reserve(Tuple->Num());
		for (const TSharedPtr<FJsonValue>& TupleValue : *Tuple)
		{
			Row.Add(FMath::RoundToInt(TupleValue->AsNumber()));
		}
		OutValues.Add(MoveTemp(Row));
	}

	return true;
}

bool ReadMatrixFrameArray(const TSharedPtr<FJsonObject>& Object, const TCHAR* FieldName, TArray<TArray<FMatrix>>& OutValues)
{
	const TArray<TSharedPtr<FJsonValue>>* JsonFrames = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(FieldName, JsonFrames) || JsonFrames == nullptr)
	{
		return false;
	}

	OutValues.Reset();
	OutValues.Reserve(JsonFrames->Num());
	for (const TSharedPtr<FJsonValue>& FrameValue : *JsonFrames)
	{
		const TArray<TSharedPtr<FJsonValue>>* JsonBones = nullptr;
		if (!FrameValue.IsValid() || !FrameValue->TryGetArray(JsonBones) || JsonBones == nullptr)
		{
			return false;
		}

		TArray<FMatrix> BoneMatrices;
		BoneMatrices.Reserve(JsonBones->Num());
		for (const TSharedPtr<FJsonValue>& BoneValue : *JsonBones)
		{
			const TArray<TSharedPtr<FJsonValue>>* JsonRows = nullptr;
			if (!BoneValue.IsValid() || !BoneValue->TryGetArray(JsonRows) || JsonRows == nullptr || JsonRows->Num() != 4)
			{
				return false;
			}

			FMatrix Matrix = FMatrix::Identity;
			for (int32 Row = 0; Row < 4; ++Row)
			{
				const TArray<TSharedPtr<FJsonValue>>* JsonColumns = nullptr;
				if (!(*JsonRows)[Row].IsValid() || !(*JsonRows)[Row]->TryGetArray(JsonColumns) || JsonColumns == nullptr || JsonColumns->Num() != 4)
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

bool ReadM2Bones(const TSharedPtr<FJsonObject>& Object, TArray<FWoWM2AuditBone>& OutBones)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("bones"), Values) || Values == nullptr)
	{
		return false;
	}

	OutBones.Reset();
	OutBones.SetNum(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* BoneObject = nullptr;
		if (!Value->TryGetObject(BoneObject) || BoneObject == nullptr || !BoneObject->IsValid())
		{
			return false;
		}

		FWoWM2AuditBone Bone;
		double Number = 0.0;
		if ((*BoneObject)->TryGetNumberField(TEXT("index"), Number))
		{
			Bone.Index = FMath::RoundToInt(Number);
		}
		if ((*BoneObject)->TryGetNumberField(TEXT("parent"), Number))
		{
			Bone.Parent = FMath::RoundToInt(Number);
		}
		if ((*BoneObject)->TryGetNumberField(TEXT("flags"), Number))
		{
			Bone.Flags = FMath::RoundToInt(Number);
		}
		ReadVectorField(*BoneObject, TEXT("pivot"), Bone.Pivot);

		if (!OutBones.IsValidIndex(Bone.Index))
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("非法 M2 bone index: %d"), Bone.Index);
			return false;
		}
		OutBones[Bone.Index] = Bone;
	}

	return true;
}

bool ReadM2Attachments(const TSharedPtr<FJsonObject>& Object, TArray<FWoWM2AuditAttachment>& OutAttachments)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("attachments"), Values) || Values == nullptr)
	{
		return false;
	}

	OutAttachments.Reset();
	OutAttachments.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* AttachmentObject = nullptr;
		if (!Value->TryGetObject(AttachmentObject) || AttachmentObject == nullptr || !AttachmentObject->IsValid())
		{
			return false;
		}

		FWoWM2AuditAttachment Attachment;
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
			OutAttachments.Add(Attachment);
		}
	}

	return true;
}

bool ValidateM2Section(const FWoWM2AuditSection& Section)
{
	const int32 VertexCount = Section.Positions.Num();
	if (VertexCount <= 0 || Section.Indices.Num() <= 0 || Section.Indices.Num() % 3 != 0)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("M2 section 数据为空或索引不是三角面: section=%d"), Section.SectionIndex);
		return false;
	}
	if (Section.Normals.Num() != VertexCount || Section.UVs.Num() != VertexCount ||
		Section.BoneIndices.Num() != VertexCount || Section.BoneWeights.Num() != VertexCount)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("M2 section 顶点属性数量不一致: section=%d verts=%d normals=%d uvs=%d bones=%d weights=%d"),
			Section.SectionIndex,
			VertexCount,
			Section.Normals.Num(),
			Section.UVs.Num(),
			Section.BoneIndices.Num(),
			Section.BoneWeights.Num());
		return false;
	}
	if (Section.VertexColors.Num() > 0 && Section.VertexColors.Num() != VertexCount)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("M2 section 顶点色数量不一致: section=%d verts=%d colors=%d"),
			Section.SectionIndex,
			VertexCount,
			Section.VertexColors.Num());
		return false;
	}

	for (const int32 Index : Section.Indices)
	{
		if (!Section.Positions.IsValidIndex(Index))
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("M2 section 索引越界: section=%d index=%d verts=%d"),
				Section.SectionIndex,
				Index,
				VertexCount);
			return false;
		}
	}

	return true;
}

bool ReadM2Sections(const TSharedPtr<FJsonObject>& Object, TArray<FWoWM2AuditSection>& OutSections)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("sections"), Values) || Values == nullptr)
	{
		return false;
	}

	OutSections.Reset();
	OutSections.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* SectionObject = nullptr;
		if (!Value->TryGetObject(SectionObject) || SectionObject == nullptr || !SectionObject->IsValid())
		{
			return false;
		}

		FWoWM2AuditSection Section;
		double Number = 0.0;
		if ((*SectionObject)->TryGetNumberField(TEXT("section_index"), Number))
		{
			Section.SectionIndex = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("submesh_id"), Number))
		{
			Section.SubmeshId = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("batch_flags"), Number))
		{
			Section.BatchFlags = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("priority_plane"), Number))
		{
			Section.PriorityPlane = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("shader_id"), Number))
		{
			Section.ShaderId = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("material_index"), Number))
		{
			Section.MaterialIndex = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("material_layer"), Number))
		{
			Section.MaterialLayer = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("material_flags"), Number))
		{
			Section.MaterialFlags = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("render_mode"), Number))
		{
			Section.RenderMode = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("texture_count"), Number))
		{
			Section.TextureCount = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("texture_combo_index"), Number))
		{
			Section.TextureComboIndex = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("texture_coord_combo_index"), Number))
		{
			Section.TextureCoordComboIndex = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("texture_weight_combo_index"), Number))
		{
			Section.TextureWeightComboIndex = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("texture_transform_combo_index"), Number))
		{
			Section.TextureTransformComboIndex = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("texture_unit"), Number))
		{
			Section.TextureUnit = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("color_index"), Number))
		{
			Section.ColorIndex = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("transparency_index"), Number))
		{
			Section.TransparencyIndex = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("texture_transform_index"), Number))
		{
			Section.TextureTransformIndex = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("texture_index"), Number))
		{
			Section.TextureIndex = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("texture_flags"), Number))
		{
			Section.TextureFlags = FMath::RoundToInt(Number);
		}
		if ((*SectionObject)->TryGetNumberField(TEXT("triangle_count"), Number))
		{
			Section.TriangleCount = FMath::RoundToInt(Number);
		}
		(*SectionObject)->TryGetBoolField(TEXT("two_sided"), Section.bTwoSided);
		(*SectionObject)->TryGetBoolField(TEXT("unlit"), Section.bUnlit);
		(*SectionObject)->TryGetBoolField(TEXT("no_depth_test"), Section.bNoDepthTest);
		(*SectionObject)->TryGetBoolField(TEXT("no_depth_write"), Section.bNoDepthWrite);
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
		const TArray<TSharedPtr<FJsonValue>>* LayerTextureFlagValues = nullptr;
		if ((*SectionObject)->TryGetArrayField(TEXT("layer_texture_flags"), LayerTextureFlagValues) && LayerTextureFlagValues)
		{
			for (const TSharedPtr<FJsonValue>& LayerTextureFlagValue : *LayerTextureFlagValues)
			{
				if (LayerTextureFlagValue.IsValid() && LayerTextureFlagValue->Type == EJson::Number)
				{
					Section.LayerTextureFlags.Add(FMath::RoundToInt(LayerTextureFlagValue->AsNumber()));
				}
			}
		}

		const bool bReadRequiredFields =
			ReadVector3fArrayField(*SectionObject, TEXT("positions"), Section.Positions) &&
			ReadVector3fArrayField(*SectionObject, TEXT("normals"), Section.Normals) &&
			ReadVector2fArrayField(*SectionObject, TEXT("uvs"), Section.UVs) &&
			ReadIntArrayField(*SectionObject, TEXT("indices"), Section.Indices) &&
			ReadIntTupleArrayField(*SectionObject, TEXT("bone_indices"), Section.BoneIndices) &&
			ReadIntTupleArrayField(*SectionObject, TEXT("bone_weights"), Section.BoneWeights);

		if (!bReadRequiredFields)
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("无法读取 M2 section 必要字段: section=%d"), Section.SectionIndex);
			return false;
		}

		ReadVector4fArrayField(*SectionObject, TEXT("vertex_colors"), Section.VertexColors);
		if (!ValidateM2Section(Section))
		{
			return false;
		}

		OutSections.Add(MoveTemp(Section));
	}

	return true;
}

bool ReadM2ParticleEmitters(const TSharedPtr<FJsonObject>& Object, TArray<FWoWM2AuditParticleEmitter>& OutEmitters)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("particle_emitters"), Values) || Values == nullptr)
	{
		return false;
	}

	OutEmitters.Reset();
	OutEmitters.Reserve(Values->Num());
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* EmitterObject = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(EmitterObject) || EmitterObject == nullptr || !EmitterObject->IsValid())
		{
			return false;
		}

		FWoWM2AuditParticleEmitter Emitter;
		double Number = 0.0;
		if ((*EmitterObject)->TryGetNumberField(TEXT("index"), Number))
		{
			Emitter.Index = FMath::RoundToInt(Number);
		}
		(*EmitterObject)->TryGetStringField(TEXT("preview_png"), Emitter.PreviewPng);
		if (!Emitter.PreviewPng.IsEmpty())
		{
			OutEmitters.Add(MoveTemp(Emitter));
		}
	}

	return true;
}

bool IsM2SubmeshVisible(int32 SubmeshId, const TMap<int32, int32>& GeosetGroups)
{
	if (SubmeshId == 0)
	{
		return true;
	}

	const int32 Group = SubmeshId / 100;
	const int32 Value = SubmeshId % 100;
	const int32* SelectedValue = GeosetGroups.Find(Group);
	if (SelectedValue)
	{
		return Value == *SelectedValue;
	}

	// WMV only resolves the standard player-character geoset groups 0..19.
	// Custom client models may contain extra groups such as 20xx feet pieces or
	// 83xx demon-hunter horns; those are authored M2 sections and must remain
	// visible unless a future DBC rule explicitly controls them.
	return Group >= 20;
}

TMap<int32, int32> LoadConfiguredGeosetsFromAppearance(const FString& AppearancePath)
{
	TMap<int32, int32> Geosets;
	if (AppearancePath.IsEmpty())
	{
		return Geosets;
	}

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *AppearancePath))
	{
		return Geosets;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Warning, TEXT("无法解析 appearance geoset 配置: %s"), *AppearancePath);
		return Geosets;
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("enabled_geosets"), Values) || Values == nullptr)
	{
		return Geosets;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		if (!Value.IsValid() || Value->Type != EJson::Number)
		{
			continue;
		}

		const int32 GeosetId = FMath::RoundToInt(Value->AsNumber());
		Geosets.Add(GeosetId / 100, GeosetId % 100);
	}
	return Geosets;
}

FString MakeSectionVisibilityPathForMeshJson(const FString& InputPath)
{
	const FString Directory = FPaths::GetPath(InputPath);
	const FString Filename = FPaths::GetCleanFilename(InputPath);
	const FString MeshJsonSuffix = TEXT(".mesh.json");

	if (Filename.EndsWith(MeshJsonSuffix, ESearchCase::IgnoreCase))
	{
		FString SectionName = Filename.LeftChop(MeshJsonSuffix.Len());
		SectionName += TEXT(".sections.json");
		return Directory / SectionName;
	}

	return Directory / (FPaths::GetBaseFilename(InputPath) + TEXT(".sections.json"));
}

TMap<int32, int32> LoadConfiguredGeosetsFromSectionVisibility(const FString& SectionVisibilityPath)
{
	TMap<int32, int32> Geosets;
	if (SectionVisibilityPath.IsEmpty())
	{
		return Geosets;
	}

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *SectionVisibilityPath))
	{
		return Geosets;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Warning, TEXT("无法解析 section geoset 配置: %s"), *SectionVisibilityPath);
		return Geosets;
	}

	const TSharedPtr<FJsonObject>* GroupObject = nullptr;
	if (RootObject->TryGetObjectField(TEXT("geoset_groups"), GroupObject) && GroupObject && GroupObject->IsValid())
	{
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : (*GroupObject)->Values)
		{
			if (!Pair.Value.IsValid() || Pair.Value->Type != EJson::Number)
			{
				continue;
			}

			Geosets.Add(FCString::Atoi(*Pair.Key), FMath::RoundToInt(Pair.Value->AsNumber()));
		}
		if (!Geosets.IsEmpty())
		{
			return Geosets;
		}
	}

	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("enabled_geosets"), Values) || Values == nullptr)
	{
		return Geosets;
	}

	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		if (!Value.IsValid() || Value->Type != EJson::Number)
		{
			continue;
		}

		const int32 GeosetId = FMath::RoundToInt(Value->AsNumber());
		Geosets.Add(GeosetId / 100, GeosetId % 100);
	}
	return Geosets;
}

void BuildDefaultGeosetSelectionFromSections(const TArray<FWoWM2AuditSection>& Sections, TMap<int32, int32>& InOutGeosetGroups)
{
	if (InOutGeosetGroups.Num() > 0)
	{
		return;
	}

	for (const FWoWM2AuditSection& Section : Sections)
	{
		if (Section.SubmeshId >= 100)
		{
			InOutGeosetGroups.Add(Section.SubmeshId / 100, 1);
		}
	}

	// 中文说明：这和运行时程序化预览保持一致，只是裸模/默认外观的临时兜底。
	// 真正角色外观必须由 CharSections / CharHairGeosets / CharacterFacialHairStyles / 装备 DBC 决定。
	InOutGeosetGroups.Add(0, 7);
	InOutGeosetGroups.Add(7, 2);
	InOutGeosetGroups.Add(17, 0);
}

TArray<FWoWM2AuditSection> FilterSectionsByGeosets(const TArray<FWoWM2AuditSection>& Sections, const TMap<int32, int32>& GeosetGroups)
{
	TArray<FWoWM2AuditSection> Filtered;
	Filtered.Reserve(Sections.Num());
	for (const FWoWM2AuditSection& Section : Sections)
	{
		if (IsM2SubmeshVisible(Section.SubmeshId, GeosetGroups))
		{
			Filtered.Add(Section);
		}
	}
	return Filtered;
}

const TCHAR* GetWotLKBlendModeName(int32 RenderMode)
{
	switch (RenderMode)
	{
	case 0: return TEXT("BM_OPAQUE");
	case 1: return TEXT("BM_TRANSPARENT_ALPHA_TEST");
	case 2: return TEXT("BM_ALPHA_BLEND");
	case 3: return TEXT("BM_ADDITIVE_SRC_COLOR_ONE");
	case 4: return TEXT("BM_ADDITIVE_ALPHA_SRC_ALPHA_ONE");
	case 5: return TEXT("BM_MODULATE_DST_COLOR_SRC_COLOR");
	case 6: return TEXT("BM_MODULATEX2_DST_COLOR_SRC_COLOR");
	default: return TEXT("BM_UNKNOWN");
	}
}

const TCHAR* GetWotLKBlendEquationNote(int32 RenderMode)
{
	switch (RenderMode)
	{
	case 0: return TEXT("opaque/no blend");
	case 1: return TEXT("alpha test GEQUAL 0.7");
	case 2: return TEXT("blend SRC_ALPHA, ONE_MINUS_SRC_ALPHA");
	case 3: return TEXT("blend SRC_COLOR, ONE");
	case 4: return TEXT("blend SRC_ALPHA, ONE");
	case 5: return TEXT("blend DST_COLOR, SRC_COLOR");
	case 6: return TEXT("blend DST_COLOR, SRC_COLOR");
	default: return TEXT("unknown");
	}
}

const TCHAR* GetWotLKBridgeMaterialFamily(const FWoWM2AuditSection& Section)
{
	if (Section.TextureUnit == -1 && (Section.BatchFlags & 0x10) != 0 && Section.RenderMode > 2)
	{
		return TEXT("EnvMap/SphereMap");
	}

	switch (Section.RenderMode)
	{
	case 0: return Section.bTwoSided ? TEXT("OpaqueTwoSided") : TEXT("OpaqueOneSided");
	case 1: return TEXT("MaskedAlphaTestTwoSided");
	case 2: return TEXT("TranslucentAlphaBlend");
	case 3: return TEXT("AdditiveSrcColor");
	case 4: return TEXT("AdditiveAlpha");
	case 5: return TEXT("Modulate");
	case 6: return TEXT("ModulateX2");
	default: return TEXT("Unknown");
	}
}

void WriteM2SectionRenderAudit(const FString& InputPath, const TArray<FWoWM2AuditSection>& Sections)
{
	FString Report;
	Report += TEXT("# WoW M2 Section Render Audit\n");
	Report += FString::Printf(TEXT("input=%s\n"), *InputPath);
	Report += FString::Printf(TEXT("section_count=%d\n"), Sections.Num());
	Report += TEXT("说明：这些字段来自 M2 + .skin batch + lookup 表，是 UE 材质桥接的权威输入。\n");
	Report += TEXT("后续 WoW 风格材质必须优先按 shader_id/texture_count/material_flags/render_mode/texture_*_combo_index 解析，不允许只按截图猜。\n\n");

	for (const FWoWM2AuditSection& Section : Sections)
	{
		Report += FString::Printf(
			TEXT("section=%d submesh=%d shader_id=%d batch_flags=0x%02X priority=%d material=%d layer=%d material_flags=0x%02X render_mode=%d blend=%s family=%s equation=\"%s\" texture_count=%d tex=%d texUnit=%d color=%d alpha=%d uvAnim=%d combo={tex:%d coord:%d weight:%d transform:%d} twoSided=%s unlit=%s noDepthTest=%s noDepthWrite=%s png=%s\n"),
			Section.SectionIndex,
			Section.SubmeshId,
			Section.ShaderId,
			Section.BatchFlags,
			Section.PriorityPlane,
			Section.MaterialIndex,
			Section.MaterialLayer,
			Section.MaterialFlags,
			Section.RenderMode,
			GetWotLKBlendModeName(Section.RenderMode),
			GetWotLKBridgeMaterialFamily(Section),
			GetWotLKBlendEquationNote(Section.RenderMode),
			Section.TextureCount,
			Section.TextureIndex,
			Section.TextureUnit,
			Section.ColorIndex,
			Section.TransparencyIndex,
			Section.TextureTransformIndex,
			Section.TextureComboIndex,
			Section.TextureCoordComboIndex,
			Section.TextureWeightComboIndex,
			Section.TextureTransformComboIndex,
			Section.bTwoSided ? TEXT("true") : TEXT("false"),
			Section.bUnlit ? TEXT("true") : TEXT("false"),
			Section.bNoDepthTest ? TEXT("true") : TEXT("false"),
			Section.bNoDepthWrite ? TEXT("true") : TEXT("false"),
			*Section.PreviewPng);
	}

	const FString ReportPath = FPaths::ProjectSavedDir() / TEXT("WoWM2SectionRenderAudit.txt");
	if (FFileHelper::SaveStringToFile(Report, *ReportPath))
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("已写出 M2 section 渲染审计: %s"), *ReportPath);
	}
	else
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Warning, TEXT("无法写出 M2 section 渲染审计: %s"), *ReportPath);
	}
}

bool ReadM2AnimationClips(
	const TSharedPtr<FJsonObject>& Object,
	const TSet<int32>& WantedAnimIds,
	bool bIncludeSubAnimations,
	TArray<FWoWM2AuditAnimationClip>& OutClips)
{
	const TArray<TSharedPtr<FJsonValue>>* Values = nullptr;
	if (!Object.IsValid() || !Object->TryGetArrayField(TEXT("animations"), Values) || Values == nullptr)
	{
		return false;
	}

	OutClips.Reset();
	for (const TSharedPtr<FJsonValue>& Value : *Values)
	{
		const TSharedPtr<FJsonObject>* ClipObject = nullptr;
		if (!Value.IsValid() || !Value->TryGetObject(ClipObject) || ClipObject == nullptr || !ClipObject->IsValid())
		{
			return false;
		}

		FWoWM2AuditAnimationClip Clip;
		double Number = 0.0;
		(*ClipObject)->TryGetStringField(TEXT("key"), Clip.Key);
		if ((*ClipObject)->TryGetNumberField(TEXT("sequence_index"), Number))
		{
			Clip.SequenceIndex = FMath::RoundToInt(Number);
		}
		if ((*ClipObject)->TryGetNumberField(TEXT("animation_id"), Number))
		{
			Clip.AnimationId = FMath::RoundToInt(Number);
			Clip.LogicalAnimationId = Clip.AnimationId;
		}
		if ((*ClipObject)->TryGetNumberField(TEXT("sub_animation_id"), Number))
		{
			Clip.SubAnimationId = FMath::RoundToInt(Number);
		}
		if ((*ClipObject)->TryGetNumberField(TEXT("length_ms"), Number))
		{
			Clip.LengthMs = FMath::RoundToInt(Number);
		}
		if ((*ClipObject)->TryGetNumberField(TEXT("sample_rate"), Number))
		{
			Clip.SampleRate = static_cast<float>(Number);
		}
		if ((*ClipObject)->TryGetNumberField(TEXT("frame_count"), Number))
		{
			Clip.FrameCount = FMath::RoundToInt(Number);
		}
		if ((*ClipObject)->TryGetNumberField(TEXT("bone_count"), Number))
		{
			Clip.BoneCount = FMath::RoundToInt(Number);
		}
		ReadIntArrayField(*ClipObject, TEXT("lookup_ids"), Clip.LookupIds);

		if (!bIncludeSubAnimations && Clip.SubAnimationId != 0)
		{
			continue;
		}

		if (!ReadMatrixFrameArray(*ClipObject, TEXT("frames"), Clip.Frames) || Clip.Frames.IsEmpty())
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("无法读取动画 frames: anim=%d sub=%d"), Clip.AnimationId, Clip.SubAnimationId);
			return false;
		}
		if (WantedAnimIds.IsEmpty())
		{
			OutClips.Add(MoveTemp(Clip));
			continue;
		}

		TArray<int32> MatchedLogicalIds;
		for (const int32 LookupId : Clip.LookupIds)
		{
			if (WantedAnimIds.Contains(LookupId))
			{
				MatchedLogicalIds.AddUnique(LookupId);
			}
		}
		if (MatchedLogicalIds.IsEmpty() && Clip.LookupIds.IsEmpty() && WantedAnimIds.Contains(Clip.AnimationId))
		{
			// 旧 JSON 没有 lookup_ids 时才按 sequence animation_id 兼容；新版 M2 JSON 必须走 animation_lookup。
			MatchedLogicalIds.Add(Clip.AnimationId);
		}

		for (const int32 LogicalAnimationId : MatchedLogicalIds)
		{
			FWoWM2AuditAnimationClip LogicalClip = Clip;
			LogicalClip.LogicalAnimationId = LogicalAnimationId;
			OutClips.Add(MoveTemp(LogicalClip));
		}
	}

	return true;
}

FName MakeM2BoneName(int32 BoneIndex)
{
	return FName(*FString::Printf(TEXT("M2_Bone_%03d"), BoneIndex));
}

FName MakeM2SyntheticRootBoneName()
{
	return TEXT("M2_Root");
}

bool BuildReferenceSkeletonFromM2Bones(const TArray<FWoWM2AuditBone>& Bones, FReferenceSkeleton& OutReferenceSkeleton)
{
	FReferenceSkeletonModifier Modifier(OutReferenceSkeleton, nullptr);
	// 中文说明：M2 允许多个 parent=-1 的骨骼；UE Skeleton 默认要求单根骨。
	// 这里增加一个合成根骨，所有原始 M2 bone 的 UE 索引都变为 M2Index + 1。
	Modifier.Add(
		FMeshBoneInfo(MakeM2SyntheticRootBoneName(), TEXT("Synthetic single root for M2 multi-root skeleton"), INDEX_NONE),
		FTransform::Identity);

	for (int32 BoneIndex = 0; BoneIndex < Bones.Num(); ++BoneIndex)
	{
		const FWoWM2AuditBone& Bone = Bones[BoneIndex];
		if (Bone.Index != BoneIndex)
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("M2 bone 顺序不连续: expected=%d actual=%d"), BoneIndex, Bone.Index);
			return false;
		}
		if (Bone.Parent >= BoneIndex)
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("M2 bone 父级尚未创建: bone=%d parent=%d"), BoneIndex, Bone.Parent);
			return false;
		}

		const FVector ParentPivot = Bones.IsValidIndex(Bone.Parent) ? Bones[Bone.Parent].Pivot : FVector::ZeroVector;
		FTransform LocalTransform = FTransform::Identity;
		LocalTransform.SetTranslation(Bone.Parent >= 0 ? Bone.Pivot - ParentPivot : Bone.Pivot);
		const int32 UEBoneParentIndex = Bone.Parent >= 0 ? Bone.Parent + 1 : 0;

		Modifier.Add(
			FMeshBoneInfo(MakeM2BoneName(BoneIndex), FString::Printf(TEXT("M2_Bone_%03d"), BoneIndex), UEBoneParentIndex),
			LocalTransform);
	}

	return OutReferenceSkeleton.GetRawBoneNum() == Bones.Num() + 1;
}

FName MakeM2SectionMaterialSlotName(int32 SectionIndex)
{
	return FName(*FString::Printf(TEXT("M2_Section_%03d"), SectionIndex));
}

void FillMeshDescriptionBoneAttributes(const FReferenceSkeleton& ReferenceSkeleton, FSkeletalMeshAttributes& Attributes)
{
	const int32 NumBones = ReferenceSkeleton.GetRawBoneNum();
	Attributes.ReserveNewBones(NumBones);

	FSkeletalMeshAttributes::FBoneNameAttributesRef BoneNames = Attributes.GetBoneNames();
	FSkeletalMeshAttributes::FBoneParentIndexAttributesRef BoneParentIndices = Attributes.GetBoneParentIndices();
	FSkeletalMeshAttributes::FBonePoseAttributesRef BonePoses = Attributes.GetBonePoses();

	for (int32 BoneIndex = 0; BoneIndex < NumBones; ++BoneIndex)
	{
		const FBoneID BoneID = Attributes.CreateBone();
		BoneNames.Set(BoneID, ReferenceSkeleton.GetRawRefBoneInfo()[BoneIndex].Name);
		BoneParentIndices.Set(BoneID, ReferenceSkeleton.GetRawRefBoneInfo()[BoneIndex].ParentIndex);
		BonePoses.Set(BoneID, ReferenceSkeleton.GetRawRefBonePose()[BoneIndex]);
	}
}

UE::AnimationCore::FBoneWeights MakeUEBoneWeightsFromM2Vertex(
	const TArray<int32>& M2BoneIndices,
	const TArray<int32>& M2BoneWeights,
	const FReferenceSkeleton& ReferenceSkeleton)
{
	TArray<UE::AnimationCore::FBoneWeight, TInlineAllocator<4>> Influences;
	const int32 InfluenceCount = FMath::Min(M2BoneIndices.Num(), M2BoneWeights.Num());
	for (int32 InfluenceIndex = 0; InfluenceIndex < InfluenceCount; ++InfluenceIndex)
	{
		const int32 RawWeight = FMath::Clamp(M2BoneWeights[InfluenceIndex], 0, 255);
		if (RawWeight <= 0)
		{
			continue;
		}

		// 中文说明：UE bone 0 是我们新增的 M2_Root，所以所有 M2 原始 bone index 都要 +1。
		const int32 UEBoneIndex = M2BoneIndices[InfluenceIndex] + 1;
		if (!ReferenceSkeleton.GetRawRefBoneInfo().IsValidIndex(UEBoneIndex))
		{
			continue;
		}

		Influences.Add(UE::AnimationCore::FBoneWeight(UEBoneIndex, static_cast<float>(RawWeight) / 255.0f));
	}

	if (Influences.IsEmpty())
	{
		Influences.Add(UE::AnimationCore::FBoneWeight(0, 1.0f));
	}

	return UE::AnimationCore::FBoneWeights::Create(Influences);
}

bool BuildMeshDescriptionFromM2Sections(
	const TArray<FWoWM2AuditSection>& Sections,
	const FReferenceSkeleton& ReferenceSkeleton,
	FMeshDescription& OutMeshDescription,
	TArray<FSkeletalMaterial>& OutMaterials)
{
	OutMeshDescription.Empty();

	FSkeletalMeshAttributes Attributes(OutMeshDescription);
	Attributes.Register();
	FillMeshDescriptionBoneAttributes(ReferenceSkeleton, Attributes);

	FMeshDescriptionBuilder Builder;
	Builder.SetMeshDescription(&OutMeshDescription);
	Builder.SetNumUVLayers(1);

	int32 TotalVertexCount = 0;
	int32 TotalTriangleCount = 0;
	for (const FWoWM2AuditSection& Section : Sections)
	{
		TotalVertexCount += Section.Positions.Num();
		TotalTriangleCount += Section.Indices.Num() / 3;
	}

	Builder.ReserveNewVertices(TotalVertexCount);
	OutMeshDescription.ReserveNewVertexInstances(TotalVertexCount);
	OutMeshDescription.ReserveNewTriangles(TotalTriangleCount);
	OutMeshDescription.ReserveNewPolygonGroups(Sections.Num());

	FSkinWeightsVertexAttributesRef VertexSkinWeights = Attributes.GetVertexSkinWeights();
	OutMaterials.Reset();
	OutMaterials.Reserve(Sections.Num());

	for (const FWoWM2AuditSection& Section : Sections)
	{
		const FName SlotName = MakeM2SectionMaterialSlotName(Section.SectionIndex);
		const FPolygonGroupID PolygonGroupID = Builder.AppendPolygonGroup(SlotName);
		OutMaterials.Add(FSkeletalMaterial(nullptr, true, false, SlotName, SlotName));

		TArray<FVertexID> VertexIDs;
		VertexIDs.Reserve(Section.Positions.Num());
		for (int32 VertexIndex = 0; VertexIndex < Section.Positions.Num(); ++VertexIndex)
		{
			const FVertexID VertexID = Builder.AppendVertex(FVector(Section.Positions[VertexIndex]));
			VertexIDs.Add(VertexID);
			VertexSkinWeights.Set(
				VertexID,
				MakeUEBoneWeightsFromM2Vertex(Section.BoneIndices[VertexIndex], Section.BoneWeights[VertexIndex], ReferenceSkeleton));
		}

		for (int32 IndexOffset = 0; IndexOffset + 2 < Section.Indices.Num(); IndexOffset += 3)
		{
			const int32 LocalIndex0 = Section.Indices[IndexOffset + 0];
			const int32 LocalIndex1 = Section.Indices[IndexOffset + 1];
			const int32 LocalIndex2 = Section.Indices[IndexOffset + 2];

			const FVertexInstanceID Instance0 = Builder.AppendInstance(VertexIDs[LocalIndex0]);
			const FVertexInstanceID Instance1 = Builder.AppendInstance(VertexIDs[LocalIndex1]);
			const FVertexInstanceID Instance2 = Builder.AppendInstance(VertexIDs[LocalIndex2]);

			Builder.SetInstanceNormal(Instance0, FVector(Section.Normals[LocalIndex0]));
			Builder.SetInstanceNormal(Instance1, FVector(Section.Normals[LocalIndex1]));
			Builder.SetInstanceNormal(Instance2, FVector(Section.Normals[LocalIndex2]));

			Builder.SetInstanceUV(Instance0, FVector2D(Section.UVs[LocalIndex0]), 0);
			Builder.SetInstanceUV(Instance1, FVector2D(Section.UVs[LocalIndex1]), 0);
			Builder.SetInstanceUV(Instance2, FVector2D(Section.UVs[LocalIndex2]), 0);

			if (Section.VertexColors.Num() == Section.Positions.Num())
			{
				Builder.SetInstanceColor(Instance0, Section.VertexColors[LocalIndex0]);
				Builder.SetInstanceColor(Instance1, Section.VertexColors[LocalIndex1]);
				Builder.SetInstanceColor(Instance2, Section.VertexColors[LocalIndex2]);
			}

			// 中文说明：M2/SKIN 的三角绕序和 UE MeshDescription 的正面方向相反。
			// 原生 SkeletalMesh 必须在这里翻转，否则会像“看到模型内部/法线反了”一样发黑。
			Builder.AppendTriangle(Instance0, Instance2, Instance1, PolygonGroupID);
		}
	}

	return TotalVertexCount > 0 && TotalTriangleCount > 0 && OutMaterials.Num() > 0;
}

FName MakeM2AttachmentSocketName(const FWoWM2AuditAttachment& Attachment)
{
	return FName(*FString::Printf(TEXT("ATT_%03d_ID_%03d"), Attachment.Index, Attachment.Id));
}

int32 AddM2AttachmentSocketsToSkeletalMesh(
	USkeletalMesh* SkeletalMesh,
	const TArray<FWoWM2AuditAttachment>& Attachments,
	const TArray<FWoWM2AuditBone>& Bones)
{
	if (!SkeletalMesh)
	{
		return 0;
	}

	int32 AddedSocketCount = 0;
	for (const FWoWM2AuditAttachment& Attachment : Attachments)
	{
		if (Attachment.BoneIndex < 0 || !Bones.IsValidIndex(Attachment.BoneIndex))
		{
			continue;
		}

		const FName ParentBoneName = MakeM2BoneName(Attachment.BoneIndex);
		if (SkeletalMesh->GetRefSkeleton().FindRawBoneIndex(ParentBoneName) == INDEX_NONE)
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Warning, TEXT("跳过 M2 attachment socket，找不到父骨骼: attachment=%d id=%d bone=%d"),
				Attachment.Index,
				Attachment.Id,
				Attachment.BoneIndex);
			continue;
		}

		USkeletalMeshSocket* Socket = NewObject<USkeletalMeshSocket>(SkeletalMesh);
		Socket->SocketName = MakeM2AttachmentSocketName(Attachment);
		Socket->BoneName = ParentBoneName;
		// 中文说明：M2 attachment.position 是模型/绑定姿态空间的位置。
		// UE socket 的 RelativeLocation 必须是相对父骨骼绑定 pivot 的本地偏移；直接写 position 会把武器固定偏到角色外侧。
		Socket->RelativeLocation = Attachment.Position - Bones[Attachment.BoneIndex].Pivot;
		Socket->RelativeRotation = FRotator::ZeroRotator;
		Socket->RelativeScale = FVector::OneVector;
		Socket->bForceAlwaysAnimated = true;
		SkeletalMesh->AddSocket(Socket, false);
		++AddedSocketCount;
	}

	return AddedSocketCount;
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

const TCHAR* GetM2AnimationName(int32 AnimationId)
{
	// 中文说明：这里使用 M2/AnimationData 的官方动画枚举名，不使用 WMV 下拉框里的显示序号。
	// 资产名保留 animation_id，名字只用于人类阅读；运行时仍应通过 animation_lookup 解析到具体 sequence。
	switch (AnimationId)
	{
	case 0: return TEXT("Stand");
	case 1: return TEXT("Death");
	case 2: return TEXT("Spell");
	case 3: return TEXT("Stop");
	case 4: return TEXT("Walk");
	case 5: return TEXT("Run");
	case 6: return TEXT("Dead");
	case 7: return TEXT("Rise");
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
	case 31: return TEXT("SpellPrecast");
	case 32: return TEXT("SpellCast");
	case 33: return TEXT("SpellCastArea");
	case 34: return TEXT("NPCWelcome");
	case 35: return TEXT("NPCGoodbye");
	case 36: return TEXT("Block");
	case 37: return TEXT("JumpStart");
	case 38: return TEXT("Jump");
	case 39: return TEXT("JumpEnd");
	case 40: return TEXT("Fall");
	case 41: return TEXT("SwimIdle");
	case 42: return TEXT("Swim");
	case 43: return TEXT("SwimLeft");
	case 44: return TEXT("SwimRight");
	case 45: return TEXT("SwimBackwards");
	case 46: return TEXT("AttackBow");
	case 47: return TEXT("FireBow");
	case 48: return TEXT("ReadyRifle");
	case 49: return TEXT("AttackRifle");
	case 50: return TEXT("Loot");
	case 51: return TEXT("ReadySpellDirected");
	case 52: return TEXT("ReadySpellOmni");
	case 53: return TEXT("SpellCastDirected");
	case 54: return TEXT("SpellCastOmni");
	case 55: return TEXT("BattleRoar");
	case 56: return TEXT("ReadyAbility");
	case 57: return TEXT("Special1H");
	case 58: return TEXT("Special2H");
	case 59: return TEXT("ShieldBash");
	case 60: return TEXT("EmoteTalk");
	case 61: return TEXT("EmoteEat");
	case 62: return TEXT("EmoteWork");
	case 63: return TEXT("EmoteUseStanding");
	case 64: return TEXT("EmoteTalkExclamation");
	case 65: return TEXT("EmoteTalkQuestion");
	case 66: return TEXT("EmoteBow");
	case 67: return TEXT("EmoteWave");
	case 68: return TEXT("EmoteCheer");
	case 69: return TEXT("EmoteDance");
	case 70: return TEXT("EmoteLaugh");
	case 71: return TEXT("EmoteSleep");
	case 72: return TEXT("EmoteSitGround");
	case 73: return TEXT("EmoteRude");
	case 74: return TEXT("EmoteRoar");
	case 75: return TEXT("EmoteKneel");
	case 76: return TEXT("EmoteKiss");
	case 77: return TEXT("EmoteCry");
	case 78: return TEXT("EmoteChicken");
	case 79: return TEXT("EmoteBeg");
	case 80: return TEXT("EmoteApplaud");
	case 81: return TEXT("EmoteShout");
	case 82: return TEXT("EmoteFlex");
	case 83: return TEXT("EmoteShy");
	case 84: return TEXT("EmotePoint");
	case 85: return TEXT("Attack1HPierce");
	case 86: return TEXT("Attack2HLoosePierce");
	case 87: return TEXT("AttackOff");
	case 88: return TEXT("AttackOffPierce");
	case 89: return TEXT("Sheath");
	case 90: return TEXT("HipSheath");
	case 91: return TEXT("Mount");
	case 92: return TEXT("RunRight");
	case 93: return TEXT("RunLeft");
	case 94: return TEXT("MountSpecial");
	case 95: return TEXT("Kick");
	case 96: return TEXT("SitGroundDown");
	case 97: return TEXT("SitGround");
	case 98: return TEXT("SitGroundUp");
	case 99: return TEXT("SleepDown");
	case 100: return TEXT("Sleep");
	case 101: return TEXT("SleepUp");
	case 102: return TEXT("SitChairLow");
	case 103: return TEXT("SitChairMed");
	case 104: return TEXT("SitChairHigh");
	case 105: return TEXT("LoadBow");
	case 106: return TEXT("LoadRifle");
	case 107: return TEXT("AttackThrown");
	case 108: return TEXT("ReadyThrown");
	case 109: return TEXT("HoldBow");
	case 110: return TEXT("HoldRifle");
	case 111: return TEXT("HoldThrown");
	case 112: return TEXT("LoadThrown");
	case 113: return TEXT("EmoteSalute");
	case 114: return TEXT("KneelStart");
	case 115: return TEXT("KneelLoop");
	case 116: return TEXT("KneelEnd");
	case 117: return TEXT("AttackUnarmedOff");
	case 118: return TEXT("SpecialUnarmed");
	case 119: return TEXT("StealthWalk");
	case 120: return TEXT("StealthStand");
	case 121: return TEXT("Knockdown");
	case 122: return TEXT("EatingLoop");
	case 123: return TEXT("UseStandingLoop");
	case 124: return TEXT("ChannelCastDirected");
	case 125: return TEXT("ChannelCastOmni");
	case 126: return TEXT("Whirlwind");
	case 127: return TEXT("Birth");
	case 128: return TEXT("UseStandingStart");
	case 129: return TEXT("UseStandingEnd");
	case 130: return TEXT("CreatureSpecial");
	case 131: return TEXT("Drown");
	case 132: return TEXT("Drowned");
	case 133: return TEXT("FishingCast");
	case 134: return TEXT("FishingLoop");
	case 135: return TEXT("Fly");
	case 136: return TEXT("EmoteWorkNoSheathe");
	case 137: return TEXT("EmoteStunNoSheathe");
	case 138: return TEXT("EmoteUseStandingNoSheathe");
	case 139: return TEXT("SpellSleepDown");
	case 140: return TEXT("SpellKneelStart");
	case 141: return TEXT("SpellKneelLoop");
	case 142: return TEXT("SpellKneelEnd");
	case 143: return TEXT("Sprint");
	case 144: return TEXT("InFlight");
	case 145: return TEXT("Spawn");
	case 146: return TEXT("Close");
	case 147: return TEXT("Closed");
	case 148: return TEXT("Open");
	case 149: return TEXT("Opened");
	case 150: return TEXT("Destroy");
	case 151: return TEXT("Destroyed");
	case 152: return TEXT("Rebuild");
	case 153: return TEXT("Custom0");
	case 154: return TEXT("Custom1");
	case 155: return TEXT("Custom2");
	case 156: return TEXT("Custom3");
	case 157: return TEXT("Despawn");
	case 158: return TEXT("Hold");
	case 159: return TEXT("Decay");
	case 160: return TEXT("BowPull");
	case 161: return TEXT("BowRelease");
	case 162: return TEXT("ShipStart");
	case 163: return TEXT("ShipMoving");
	case 164: return TEXT("ShipStop");
	case 165: return TEXT("GroupArrow");
	case 166: return TEXT("Arrow");
	case 167: return TEXT("CorpseArrow");
	case 168: return TEXT("GuideArrow");
	case 169: return TEXT("Sway");
	case 170: return TEXT("DruidCatPounce");
	case 171: return TEXT("DruidCatRip");
	case 172: return TEXT("DruidCatRake");
	case 173: return TEXT("DruidCatRavage");
	case 174: return TEXT("DruidCatClaw");
	case 175: return TEXT("DruidCatCower");
	case 176: return TEXT("DruidBearSwipe");
	case 177: return TEXT("DruidBearBite");
	case 178: return TEXT("DruidBearMaul");
	case 179: return TEXT("DruidBearBash");
	case 180: return TEXT("DragonTail");
	case 181: return TEXT("DragonStomp");
	case 182: return TEXT("DragonSpit");
	case 183: return TEXT("DragonSpitHover");
	case 184: return TEXT("DragonSpitFly");
	case 185: return TEXT("EmoteYes");
	case 186: return TEXT("EmoteNo");
	case 187: return TEXT("JumpLandRun");
	case 188: return TEXT("LootHold");
	case 189: return TEXT("LootUp");
	case 190: return TEXT("StandHigh");
	case 191: return TEXT("Impact");
	case 192: return TEXT("LiftOff");
	case 193: return TEXT("Hover");
	case 194: return TEXT("SuccubusEntice");
	case 195: return TEXT("EmoteTrain");
	case 196: return TEXT("EmoteDead");
	case 197: return TEXT("EmoteDanceOnce");
	case 198: return TEXT("Deflect");
	case 199: return TEXT("EmoteEatNoSheathe");
	case 200: return TEXT("Land");
	case 201: return TEXT("Submerge");
	case 202: return TEXT("Submerged");
	case 203: return TEXT("Cannibalize");
	case 204: return TEXT("ArrowBirth");
	case 205: return TEXT("GroupArrowBirth");
	case 206: return TEXT("CorpseArrowBirth");
	case 207: return TEXT("GuideArrowBirth");
	case 208: return TEXT("EmoteTalkNoSheathe");
	case 209: return TEXT("EmotePointNoSheathe");
	case 210: return TEXT("EmoteSaluteNoSheathe");
	case 211: return TEXT("EmoteDanceSpecial");
	case 212: return TEXT("Mutilate");
	case 213: return TEXT("CustomSpell01");
	case 214: return TEXT("CustomSpell02");
	case 215: return TEXT("CustomSpell03");
	case 216: return TEXT("CustomSpell04");
	case 217: return TEXT("CustomSpell05");
	case 218: return TEXT("CustomSpell06");
	case 219: return TEXT("CustomSpell07");
	case 220: return TEXT("CustomSpell08");
	case 221: return TEXT("CustomSpell09");
	case 222: return TEXT("CustomSpell10");
	case 223: return TEXT("StealthRun");
	case 224: return TEXT("Emerge");
	case 225: return TEXT("Cower");
	case 226: return TEXT("Grab");
	case 227: return TEXT("GrabClosed");
	case 228: return TEXT("GrabThrown");
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

FString MakeM2AnimationAssetName(const FString& ModelName, const FWoWM2AuditAnimationClip& Clip, EWoWGeneratedGripVariant GripVariant)
{
	const int32 AssetAnimationId = Clip.LogicalAnimationId == INDEX_NONE ? Clip.AnimationId : Clip.LogicalAnimationId;
	const TCHAR* AnimationName = GetM2AnimationName(AssetAnimationId);
	FString VariantSuffix;
	switch (GripVariant)
	{
	case EWoWGeneratedGripVariant::RightHand:
		VariantSuffix = TEXT("_GripR");
		break;
	case EWoWGeneratedGripVariant::LeftHand:
		VariantSuffix = TEXT("_GripL");
		break;
	default:
		break;
	}

	if (AnimationName)
	{
		return FString::Printf(TEXT("A_%s_%03d_%s_%02d%s"),
			*ModelName,
			AssetAnimationId,
			*SanitizeAssetName(AnimationName),
			Clip.SubAnimationId,
			*VariantSuffix);
	}

	return FString::Printf(TEXT("A_%s_%03d_Anim_%02d%s"), *ModelName, AssetAnimationId, Clip.SubAnimationId, *VariantSuffix);
}

FString MakeM2AnimationAssetName(const FString& ModelName, const FWoWM2AuditAnimationClip& Clip)
{
	return MakeM2AnimationAssetName(ModelName, Clip, EWoWGeneratedGripVariant::None);
}

FString MakeObjectPathFromPackageName(const FString& PackageName)
{
	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	return PackageName + TEXT(".") + AssetName;
}

FString MakeAnimationCatalogDiskPath(const FString& AnimationPackagePath)
{
	return FPackageName::LongPackageNameToFilename(AnimationPackagePath / TEXT("AnimationCatalog"), TEXT(".wowanimcatalog"));
}

bool SaveAnimationCatalogFile(
	const FString& AnimationPackagePath,
	const FString& ModelName,
	const FString& MeshObjectPath,
	const FString& SkeletonObjectPath,
	const TArray<FWoWM2GeneratedAnimationCatalogEntry>& Entries)
{
	TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetNumberField(TEXT("version"), 1);
	Root->SetStringField(TEXT("model_name"), ModelName);
	Root->SetStringField(TEXT("mesh"), MeshObjectPath);
	Root->SetStringField(TEXT("skeleton"), SkeletonObjectPath);
	Root->SetStringField(TEXT("animation_package"), AnimationPackagePath);
	Root->SetNumberField(TEXT("entry_count"), Entries.Num());

	TArray<TSharedPtr<FJsonValue>> EntryValues;
	EntryValues.Reserve(Entries.Num());
	for (const FWoWM2GeneratedAnimationCatalogEntry& Entry : Entries)
	{
		TSharedRef<FJsonObject> EntryObject = MakeShared<FJsonObject>();
		EntryObject->SetNumberField(TEXT("anim_id"), Entry.LogicalAnimationId);
		EntryObject->SetNumberField(TEXT("source_anim_id"), Entry.SourceAnimationId);
		EntryObject->SetNumberField(TEXT("sub_anim_id"), Entry.SubAnimationId);
		EntryObject->SetNumberField(TEXT("sequence_index"), Entry.SequenceIndex);
		EntryObject->SetNumberField(TEXT("lookup_sequence_index"), Entry.LookupSequenceIndex);
		EntryObject->SetNumberField(TEXT("length_ms"), Entry.LengthMs);
		EntryObject->SetNumberField(TEXT("sample_rate"), Entry.SampleRate);
		EntryObject->SetNumberField(TEXT("frame_count"), Entry.FrameCount);
		EntryObject->SetStringField(TEXT("name"), Entry.Name);
		EntryObject->SetStringField(TEXT("object_path"), Entry.ObjectPath);
		EntryObject->SetStringField(TEXT("variant"), Entry.Variant);
		EntryValues.Add(MakeShared<FJsonValueObject>(EntryObject));
	}
	Root->SetArrayField(TEXT("animations"), MoveTemp(EntryValues));

	FString OutputText;
	TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&OutputText);
	if (!FJsonSerializer::Serialize(Root, Writer))
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("序列化 AnimationCatalog 失败: %s"), *AnimationPackagePath);
		return false;
	}

	const FString DiskPath = MakeAnimationCatalogDiskPath(AnimationPackagePath);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(DiskPath), true);
	if (!FFileHelper::SaveStringToFile(OutputText, *DiskPath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("写出 AnimationCatalog 失败: %s"), *DiskPath);
		return false;
	}

	UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("已写出 AnimationCatalog: %s entries=%d"), *DiskPath, Entries.Num());
	return true;
}

FWoWAnimIdSelection ParseAnimIdSelection(const FString& Params)
{
	FWoWAnimIdSelection Selection;
	if (FParse::Param(*Params, TEXT("AllAnimSequences")))
	{
		// 中文说明：这个模式导入 M2 animations[] 的全部 sequence，不走 animation_lookup 逻辑动作筛选。
		Selection.Mode = EWoWAnimIdSelectionMode::AllSequences;
		return Selection;
	}

	FString AnimIdsText;
	const FString Key = TEXT("AnimIds=");
	const int32 KeyIndex = Params.Find(Key, ESearchCase::IgnoreCase);
	if (KeyIndex != INDEX_NONE)
	{
		const int32 ValueStart = KeyIndex + Key.Len();
		const bool bQuoted = Params.IsValidIndex(ValueStart) && (Params[ValueStart] == TEXT('"') || Params[ValueStart] == TEXT('\''));
		const TCHAR QuoteChar = bQuoted ? Params[ValueStart] : 0;
		const int32 ScanStart = bQuoted ? ValueStart + 1 : ValueStart;
		int32 ScanEnd = ScanStart;
		while (Params.IsValidIndex(ScanEnd))
		{
			const TCHAR Ch = Params[ScanEnd];
			if ((bQuoted && Ch == QuoteChar) || (!bQuoted && FChar::IsWhitespace(Ch)))
			{
				break;
			}
			++ScanEnd;
		}
		AnimIdsText = Params.Mid(ScanStart, ScanEnd - ScanStart);
	}
	if (AnimIdsText.Equals(TEXT("all"), ESearchCase::IgnoreCase) || AnimIdsText.Equals(TEXT("*"), ESearchCase::IgnoreCase))
	{
		Selection.Mode = EWoWAnimIdSelectionMode::AllSequences;
		return Selection;
	}
	if (AnimIdsText.Equals(TEXT("lookup"), ESearchCase::IgnoreCase) || AnimIdsText.Equals(TEXT("lookup-all"), ESearchCase::IgnoreCase))
	{
		// 中文说明：这个模式导入 animation_lookup 中暴露给客户端状态机的全部逻辑动作 ID。
		// 它不同于 all；all 是全部 sequence，lookup 是 WoW 运行时可通过 animId 查到的动作。
		Selection.Mode = EWoWAnimIdSelectionMode::Lookup;
		return Selection;
	}
	if (AnimIdsText.IsEmpty())
	{
		AnimIdsText = TEXT("0,4,5,26,27,28");
	}

	TSet<int32> Result;
	TArray<FString> Parts;
	AnimIdsText.ParseIntoArray(Parts, TEXT(","), true);
	if (Parts.Num() <= 1)
	{
		Parts.Reset();
		AnimIdsText.ParseIntoArray(Parts, TEXT("+"), true);
	}
	if (Parts.Num() <= 1)
	{
		Parts.Reset();
		AnimIdsText.ParseIntoArray(Parts, TEXT(";"), true);
	}
	for (FString Part : Parts)
	{
		Part.TrimStartAndEndInline();
		if (!Part.IsEmpty())
		{
			Selection.ExplicitIds.Add(FCString::Atoi(*Part));
		}
	}
	return Selection;
}

TSet<int32> BuildWantedAnimIdSet(const FWoWAnimIdSelection& Selection, const TArray<int32>& AnimationLookup)
{
	if (Selection.Mode == EWoWAnimIdSelectionMode::AllSequences)
	{
		return TSet<int32>();
	}
	if (Selection.Mode == EWoWAnimIdSelectionMode::Explicit)
	{
		return Selection.ExplicitIds;
	}

	TSet<int32> Result;
	for (int32 LookupId = 0; LookupId < AnimationLookup.Num(); ++LookupId)
	{
		if (AnimationLookup[LookupId] >= 0)
		{
			Result.Add(LookupId);
		}
	}
	return Result;
}

bool SaveGeneratedAsset(UObject* Asset)
{
	if (!Asset)
	{
		return false;
	}

	UPackage* Package = Asset->GetOutermost();
	if (!Package)
	{
		return false;
	}

	const FString PackageFileName = FPackageName::LongPackageNameToFilename(Package->GetName(), FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	return UPackage::SavePackage(Package, Asset, *PackageFileName, SaveArgs);
}

bool RetireExistingAssetForOverwrite(UPackage* Package, const FString& AssetName, UClass* AssetClass, bool bOverwrite)
{
	if (!Package)
	{
		return false;
	}

	UObject* ExistingAsset = StaticFindObject(AssetClass, Package, *AssetName);
	if (!ExistingAsset)
	{
		return true;
	}

	if (!bOverwrite)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("目标资产已存在，本次不覆盖: %s.%s"), *Package->GetName(), *AssetName);
		return false;
	}

	// 中文说明：资产桥接工具需要能反复运行验证。覆盖时先把旧对象移到 Transient 包，避免同名 NewObject 冲突。
	ExistingAsset->ClearFlags(RF_Public | RF_Standalone);
	const FString RetiredName = FString::Printf(TEXT("%s_RETIRED_%08x"), *AssetName, FMath::Rand());
	ExistingAsset->Rename(*RetiredName, GetTransientPackage(), REN_DontCreateRedirectors | REN_NonTransactional);
	Package->MarkPackageDirty();
	return true;
}

bool DeleteExistingPackageFilesForOverwrite(const FString& PackageName, bool bOverwrite)
{
	if (!bOverwrite)
	{
		return true;
	}

	const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	if (!FPaths::FileExists(PackageFileName))
	{
		return true;
	}

	// 中文说明：UE 命令行反复生成同名资产时，旧包文件可能让 SavePackage 失败；覆盖模式下先清掉磁盘旧包。
	if (!IFileManager::Get().Delete(*PackageFileName, false, true, true))
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("覆盖模式下无法删除旧 UE 包文件: %s"), *PackageFileName);
		return false;
	}

	return true;
}

bool CleanGeneratedAnimationPackagePathForOverwrite(const FString& PackagePath, bool bOverwrite)
{
	if (!bOverwrite)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("-CleanAnimSequenceFolder 需要同时传入 -Overwrite: %s"), *PackagePath);
		return false;
	}
	if (!FPackageName::IsValidLongPackageName(PackagePath))
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("非法动画包目录: %s"), *PackagePath);
		return false;
	}
	const FString LeafName = FPackageName::GetLongPackageAssetName(PackagePath);
	if (!PackagePath.StartsWith(TEXT("/Game/WoW/Generated/")) || !LeafName.StartsWith(TEXT("Animations")))
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("拒绝清理非标准生成动画目录: %s"), *PackagePath);
		return false;
	}

	const FString DiskDirectory = FPackageName::LongPackageNameToFilename(PackagePath);
	if (!IFileManager::Get().DirectoryExists(*DiskDirectory))
	{
		return true;
	}

	TArray<FString> PackageFiles;
	IFileManager::Get().FindFilesRecursive(
		PackageFiles,
		*DiskDirectory,
		*FString::Printf(TEXT("*%s"), *FPackageName::GetAssetPackageExtension()),
		true,
		false);

	int32 DeletedCount = 0;
	for (const FString& PackageFile : PackageFiles)
	{
		if (!IFileManager::Get().Delete(*PackageFile, false, true, true))
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("无法删除旧动画资产文件: %s"), *PackageFile);
			return false;
		}
		++DeletedCount;
	}

	UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("已清理生成动画目录: %s deleted=%d"), *PackagePath, DeletedCount);
	return true;
}

FMatrix MakeRawBindGlobalMatrix(const FVector& Pivot)
{
	FMatrix Matrix = FMatrix::Identity;
	Matrix.M[0][3] = Pivot.X;
	Matrix.M[1][3] = Pivot.Y;
	Matrix.M[2][3] = Pivot.Z;
	return Matrix;
}

FMatrix MultiplyRawMatrices(const FMatrix& A, const FMatrix& B)
{
	FMatrix Result = FMatrix::Identity;
	for (int32 Row = 0; Row < 4; ++Row)
	{
		for (int32 Column = 0; Column < 4; ++Column)
		{
			float Value = 0.0f;
			for (int32 K = 0; K < 4; ++K)
			{
				Value += A.M[Row][K] * B.M[K][Column];
			}
			Result.M[Row][Column] = Value;
		}
	}
	return Result;
}

FTransform MakeUETransformFromRawColumnVectorMatrix(const FMatrix& RawMatrix)
{
	FMatrix UEMatrix = FMatrix::Identity;
	for (int32 Row = 0; Row < 3; ++Row)
	{
		for (int32 Column = 0; Column < 3; ++Column)
		{
			UEMatrix.M[Column][Row] = RawMatrix.M[Row][Column];
		}
	}
	UEMatrix.M[3][0] = RawMatrix.M[0][3];
	UEMatrix.M[3][1] = RawMatrix.M[1][3];
	UEMatrix.M[3][2] = RawMatrix.M[2][3];
	UEMatrix.M[3][3] = 1.0f;

	FTransform Transform(UEMatrix);
	Transform.NormalizeRotation();
	return Transform;
}

bool BuildLocalTransformsFromM2Clip(
	const FWoWM2AuditAnimationClip& Clip,
	const TArray<FWoWM2AuditBone>& Bones,
	int32 FrameCount,
	TArray<TArray<FTransform>>& OutLocalTransformsByBone)
{
	if (Clip.Frames.IsEmpty() || Clip.BoneCount != Bones.Num() || FrameCount <= 0)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("动画局部轨道输入不合法: anim=%d frames=%d clipBones=%d m2Bones=%d targetFrames=%d"),
			Clip.AnimationId,
			Clip.Frames.Num(),
			Clip.BoneCount,
			Bones.Num(),
			FrameCount);
		return false;
	}

	TArray<FMatrix> BindGlobalMatrices;
	BindGlobalMatrices.SetNum(Bones.Num());
	for (int32 BoneIndex = 0; BoneIndex < Bones.Num(); ++BoneIndex)
	{
		BindGlobalMatrices[BoneIndex] = MakeRawBindGlobalMatrix(Bones[BoneIndex].Pivot);
	}

	OutLocalTransformsByBone.Reset();
	OutLocalTransformsByBone.SetNum(Bones.Num());
	for (TArray<FTransform>& BoneKeys : OutLocalTransformsByBone)
	{
		BoneKeys.Reserve(FrameCount);
	}

	for (int32 FrameIndex = 0; FrameIndex < FrameCount; ++FrameIndex)
	{
		const TArray<FMatrix>& SkinMatrices = Clip.Frames[FMath::Min(FrameIndex, Clip.Frames.Num() - 1)];
		if (SkinMatrices.Num() != Bones.Num())
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("动画帧骨骼数量不一致: anim=%d frame=%d bones=%d expected=%d"),
				Clip.AnimationId,
				FrameIndex,
				SkinMatrices.Num(),
				Bones.Num());
			return false;
		}

		TArray<FMatrix> CurrentGlobalMatrices;
		CurrentGlobalMatrices.SetNum(Bones.Num());
		for (int32 BoneIndex = 0; BoneIndex < Bones.Num(); ++BoneIndex)
		{
			// 中文说明：导出的 frames 是蒙皮矩阵，先乘回绑定全局矩阵，得到当前全局骨骼矩阵。
			CurrentGlobalMatrices[BoneIndex] = MultiplyRawMatrices(SkinMatrices[BoneIndex], BindGlobalMatrices[BoneIndex]);
		}

		for (int32 BoneIndex = 0; BoneIndex < Bones.Num(); ++BoneIndex)
		{
			FMatrix LocalRawMatrix = CurrentGlobalMatrices[BoneIndex];
			const int32 ParentIndex = Bones[BoneIndex].Parent;
			if (CurrentGlobalMatrices.IsValidIndex(ParentIndex))
			{
				LocalRawMatrix = MultiplyRawMatrices(CurrentGlobalMatrices[ParentIndex].Inverse(), CurrentGlobalMatrices[BoneIndex]);
			}
			OutLocalTransformsByBone[BoneIndex].Add(MakeUETransformFromRawColumnVectorMatrix(LocalRawMatrix));
		}
	}

	return true;
}

void AddM2BoneSubtreeToSet(const TArray<FWoWM2AuditBone>& Bones, int32 RootBoneIndex, TSet<int32>& OutBoneSet)
{
	if (!Bones.IsValidIndex(RootBoneIndex))
	{
		return;
	}

	TArray<int32> Stack;
	Stack.Add(RootBoneIndex);
	while (!Stack.IsEmpty())
	{
		const int32 BoneIndex = Stack.Pop(EAllowShrinking::No);
		if (!Bones.IsValidIndex(BoneIndex) || OutBoneSet.Contains(BoneIndex))
		{
			continue;
		}

		OutBoneSet.Add(BoneIndex);
		for (int32 ChildIndex = 0; ChildIndex < Bones.Num(); ++ChildIndex)
		{
			if (Bones[ChildIndex].Parent == BoneIndex)
			{
				Stack.Add(ChildIndex);
			}
		}
	}
}

TSet<int32> BuildM2GripBoneSet(
	const TArray<FWoWM2AuditBone>& Bones,
	const TArray<int32>& KeyBoneLookup,
	EWoWGeneratedGripVariant GripVariant)
{
	TSet<int32> Result;
	const int32 FirstGripKeyBone = GripVariant == EWoWGeneratedGripVariant::LeftHand ? 13 : 8;
	const int32 LastGripKeyBone = GripVariant == EWoWGeneratedGripVariant::LeftHand ? 17 : 12;
	for (int32 KeyBoneIndex = FirstGripKeyBone; KeyBoneIndex <= LastGripKeyBone; ++KeyBoneIndex)
	{
		if (KeyBoneLookup.IsValidIndex(KeyBoneIndex) && KeyBoneLookup[KeyBoneIndex] >= 0)
		{
			AddM2BoneSubtreeToSet(Bones, KeyBoneLookup[KeyBoneIndex], Result);
		}
	}
	return Result;
}

bool SaveNewAnimSequenceAsset(
	const FString& PackageName,
	const FWoWM2AuditAnimationClip& Clip,
	const TArray<FWoWM2AuditBone>& Bones,
	USkeleton* Skeleton,
	USkeletalMesh* PreviewMesh,
	bool bOverwrite,
	UAnimSequence*& OutAnimSequence,
	const FWoWM2AuditAnimationClip* GripClip = nullptr,
	const TSet<int32>* GripBoneIndices = nullptr)
{
	if (!Skeleton)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("生成 AnimSequence 失败：Skeleton 为空。"));
		return false;
	}
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("非法 UE 包名: %s"), *PackageName);
		return false;
	}
	if (Clip.Frames.IsEmpty() || Clip.BoneCount != Bones.Num())
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("动画采样数量不合法: anim=%d frames=%d clipBones=%d m2Bones=%d"),
			Clip.AnimationId,
			Clip.Frames.Num(),
			Clip.BoneCount,
			Bones.Num());
		return false;
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return false;
	}
	if (!RetireExistingAssetForOverwrite(Package, AssetName, UAnimSequence::StaticClass(), bOverwrite))
	{
		return false;
	}
	if (!DeleteExistingPackageFilesForOverwrite(PackageName, bOverwrite))
	{
		return false;
	}

	UAnimSequence* AnimSequence = nullptr;
	{
		// 中文说明：UE 5.8 默认给新 AnimSequence 创建 Sequencer/ControlRig DataModel。
		// 自动转换 M2 时应写入传统骨骼动画轨道，否则命令行环境没有 MovieScene 会导致轨道写入失败。
		FScopedLegacyAnimDataModel LegacyAnimDataModelScope;
		AnimSequence = NewObject<UAnimSequence>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	}
	if (!AnimSequence)
	{
		return false;
	}
	AnimSequence->BoneCompressionSettings = FAnimationUtils::GetDefaultAnimationBoneCompressionSettings();
	AnimSequence->CurveCompressionSettings = FAnimationUtils::GetDefaultAnimationCurveCompressionSettings();
	AnimSequence->VariableFrameStrippingSettings = FAnimationUtils::GetDefaultVariableFrameStrippingSettings();
	AnimSequence->SetSkeleton(Skeleton);
	if (PreviewMesh)
	{
		AnimSequence->SetPreviewMesh(PreviewMesh, false);
	}

	AnimSequence->ResetAnimation();
	IAnimationDataController& Controller = AnimSequence->GetController();
	IAnimationDataController::FScopedBracket ScopedBracket(Controller, FText::FromString(TEXT("Import WoW M2 sampled animation")), false);
	Controller.UpdateWithSkeleton(Skeleton, false);

	const int32 SourceFrameCount = Clip.Frames.Num();
	// 中文说明：M2 中 HandsClosed 等姿态可以只有 1 帧。UE AnimSequence 写轨道时保留为极短双关键帧，
	// 这样不会改变姿态含义，又能通过 UE 的帧区间要求。
	const int32 FrameCount = FMath::Max(2, SourceFrameCount);
	const int32 FrameRateNumerator = FMath::Max(1, FMath::RoundToInt(Clip.SampleRate));
	Controller.SetFrameRate(FFrameRate(FrameRateNumerator, 1), false);
	Controller.SetNumberOfFrames(FFrameNumber(FrameCount - 1), false);

	TArray<TArray<FTransform>> LocalTransformsByBone;
	if (!BuildLocalTransformsFromM2Clip(Clip, Bones, FrameCount, LocalTransformsByBone))
	{
		return false;
	}

	if (GripClip && GripBoneIndices && GripBoneIndices->Num() > 0)
	{
		TArray<TArray<FTransform>> GripLocalTransformsByBone;
		if (!BuildLocalTransformsFromM2Clip(*GripClip, Bones, 1, GripLocalTransformsByBone))
		{
			return false;
		}

		int32 AppliedGripBoneCount = 0;
		for (const int32 BoneIndex : *GripBoneIndices)
		{
			if (!LocalTransformsByBone.IsValidIndex(BoneIndex) ||
				!GripLocalTransformsByBone.IsValidIndex(BoneIndex) ||
				GripLocalTransformsByBone[BoneIndex].IsEmpty())
			{
				continue;
			}

			for (FTransform& LocalTransform : LocalTransformsByBone[BoneIndex])
			{
				LocalTransform = GripLocalTransformsByBone[BoneIndex][0];
			}
			++AppliedGripBoneCount;
		}

		UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("已烘焙 HandsClosed 握持层: anim=%d sub=%d gripBones=%d package=%s"),
			Clip.AnimationId,
			Clip.SubAnimationId,
			AppliedGripBoneCount,
			*PackageName);
	}

	{
		const FName RootName = MakeM2SyntheticRootBoneName();
		if (!Controller.AddBoneCurve(RootName, false))
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("AnimSequence 添加根骨轨道失败: %s"), *RootName.ToString());
			return false;
		}
		TArray<FVector3f> PosKeys;
		TArray<FQuat4f> RotKeys;
		TArray<FVector3f> ScaleKeys;
		PosKeys.Init(FVector3f::ZeroVector, FrameCount);
		RotKeys.Init(FQuat4f::Identity, FrameCount);
		ScaleKeys.Init(FVector3f::OneVector, FrameCount);
		if (!Controller.SetBoneTrackKeys(RootName, PosKeys, RotKeys, ScaleKeys, false))
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("AnimSequence 写入根骨轨道失败: %s"), *RootName.ToString());
			return false;
		}
	}

	for (int32 BoneIndex = 0; BoneIndex < Bones.Num(); ++BoneIndex)
	{
		const FName BoneName = MakeM2BoneName(BoneIndex);
		if (!Controller.AddBoneCurve(BoneName, false))
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("AnimSequence 添加骨骼轨道失败: %s"), *BoneName.ToString());
			return false;
		}

		TArray<FVector3f> PosKeys;
		TArray<FQuat4f> RotKeys;
		TArray<FVector3f> ScaleKeys;
		PosKeys.Reserve(FrameCount);
		RotKeys.Reserve(FrameCount);
		ScaleKeys.Reserve(FrameCount);
		for (const FTransform& LocalTransform : LocalTransformsByBone[BoneIndex])
		{
			PosKeys.Add(FVector3f(LocalTransform.GetTranslation()));
			RotKeys.Add(FQuat4f(LocalTransform.GetRotation()));
			ScaleKeys.Add(FVector3f(LocalTransform.GetScale3D()));
		}
		if (!Controller.SetBoneTrackKeys(BoneName, PosKeys, RotKeys, ScaleKeys, false))
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("AnimSequence 写入骨骼轨道失败: %s"), *BoneName.ToString());
			return false;
		}
	}

	Controller.NotifyPopulated();
	AnimSequence->MarkPackageDirty();
	FAssetRegistryModule::AssetCreated(AnimSequence);
	if (!SaveGeneratedAsset(AnimSequence))
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("保存 AnimSequence 失败: %s"), *PackageName);
		return false;
	}

	OutAnimSequence = AnimSequence;
	return true;
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
			TArray<FString> Parts;
			RelativeDir.ParseIntoArray(Parts, TEXT("/"), true);
			for (FString& Part : Parts)
			{
				Part = SanitizeAssetName(Part);
			}
			return TEXT("/Game/WoW/Generated/Textures/") + FString::Join(Parts, TEXT("/"));
		}
	}

	return TEXT("/Game/WoW/Generated/Textures/Misc");
}

void ApplyM2TextureAddressSettings(UTexture2D* Texture, int32 TextureFlags)
{
	if (!Texture)
	{
		return;
	}

	// WotLK M2 texture flags use bit 0/1 for wrap X/Y. Without the bit, keep clamp.
	// This matters for Glue cloud/light-ray planes: using clamp on wrap textures creates visible seams.
	Texture->AddressX = (TextureFlags & 0x1) != 0 ? TA_Wrap : TA_Clamp;
	Texture->AddressY = (TextureFlags & 0x2) != 0 ? TA_Wrap : TA_Clamp;
	// M2/SKIN render batches reference color/alpha BLPs. UE's PNG importer can mis-detect blue landscape
	// layers such as WOTLK_LOGIN_LANDING01/02 as normal maps, which turns them into yellow/green slabs.
	// Keep generated M2 textures on ordinary color compression unless a future parser proves a true normal-map use.
	Texture->CompressionSettings = TC_Default;
	Texture->LODGroup = TEXTUREGROUP_World;
	Texture->Filter = TF_Bilinear;
	Texture->SRGB = true;

	const FString AssetPath = Texture->GetPathName();
	const FString AssetPathUpper = AssetPath.ToUpper();

	// 中文说明：WotLK 登录界面的 SNOW_SHIELD2 这类细碎雪点纹理如果直接走 UE 默认 mip 生成，
	// 远处会很容易被平均成连续灰雾，导致雪花几乎消失，只剩贴在几何面上的模糊层。
	// 在保留 M2 原始 section/material 语义不变的前提下，先对这类“高频稀疏粒点”贴图关闭自动 mip，
	// 避免 UE 用 PNG mip0 重新生成的 mip 链破坏原始 BLP 的可见性。
	const bool bSparseHighFrequencyEffect =
		AssetPathUpper.Contains(TEXT("/CREATURE/SNOWFLAKECREATURE/SNOW_SHIELD2.")) ||
		AssetPathUpper.Contains(TEXT("/INTERFACE/GLUES/MODELS/UI_MAINMENU_NORTHREND/SNOWFLAKE01B."));
	if (bSparseHighFrequencyEffect)
	{
		Texture->MipGenSettings = TMGS_NoMipmaps;
		Texture->LODGroup = TEXTUREGROUP_Effects;
	}
}

UTexture2D* ImportOrLoadGeneratedTexture(const FString& PreviewPng, int32 TextureFlags = 0)
{
	if (PreviewPng.IsEmpty() || !FPaths::FileExists(PreviewPng))
	{
		return nullptr;
	}

	const FString DestinationPath = MakeGeneratedTextureDestinationPath(PreviewPng);
	const FString AssetName = SanitizeAssetName(FPaths::GetBaseFilename(PreviewPng));
	const FString ObjectPath = DestinationPath / AssetName + TEXT(".") + AssetName;

	UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
	ImportData->Filenames.Add(PreviewPng);
	ImportData->DestinationPath = DestinationPath;
	// Generated WoW textures are current-state caches. The object path stays stable, but the PNG
	// content changes whenever appearance/equipment composition changes, so reruns must overwrite.
	ImportData->bReplaceExisting = true;
	ImportData->bSkipReadOnly = true;

	FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
	TArray<UObject*> ImportedAssets = AssetToolsModule.Get().ImportAssetsAutomated(ImportData);

	UTexture2D* ImportedTexture = nullptr;
	for (UObject* ImportedAsset : ImportedAssets)
	{
		ImportedTexture = Cast<UTexture2D>(ImportedAsset);
		if (ImportedTexture)
		{
			break;
		}
	}
	if (!ImportedTexture)
	{
		ImportedTexture = LoadObject<UTexture2D>(nullptr, *ObjectPath);
	}
	if (ImportedTexture)
	{
		ApplyM2TextureAddressSettings(ImportedTexture, TextureFlags);
		ImportedTexture->MarkPackageDirty();
		SaveGeneratedAsset(ImportedTexture);
	}

	return ImportedTexture;
}

FString MakeGeneratedTextureDestinationPathFromManifestEntry(
	const TSharedPtr<FJsonObject>& EntryObject,
	const FString& PreviewPng)
{
	FString PackagePath;
	if (EntryObject.IsValid() && EntryObject->TryGetStringField(TEXT("packagePath"), PackagePath) && !PackagePath.IsEmpty())
	{
		return PackagePath;
	}

	return MakeGeneratedTextureDestinationPath(PreviewPng);
}

FString MakeGeneratedTextureAssetNameFromManifestEntry(
	const TSharedPtr<FJsonObject>& EntryObject,
	const FString& PreviewPng)
{
	FString AssetName;
	if (EntryObject.IsValid() && EntryObject->TryGetStringField(TEXT("assetName"), AssetName) && !AssetName.IsEmpty())
	{
		return SanitizeAssetName(AssetName);
	}

	return SanitizeAssetName(FPaths::GetBaseFilename(PreviewPng));
}

bool ImportCharacterAppearanceTextureManifest(const FString& ManifestPath, bool bOverwrite)
{
	TSharedPtr<FJsonObject> RootObject;
	if (!ReadJsonObjectFile(ManifestPath, RootObject))
	{
		return false;
	}

	const TArray<TSharedPtr<FJsonValue>>* Entries = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("entries"), Entries) || !Entries)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("角色外观纹理 manifest 缺少 entries: %s"), *ManifestPath);
		return false;
	}

	int32 ImportedCount = 0;
	int32 SkippedCount = 0;
	int32 MissingCount = 0;
	for (const TSharedPtr<FJsonValue>& EntryValue : *Entries)
	{
		const TSharedPtr<FJsonObject>* EntryObjectPtr = nullptr;
		if (!EntryValue.IsValid() || !EntryValue->TryGetObject(EntryObjectPtr) || !EntryObjectPtr || !EntryObjectPtr->IsValid())
		{
			continue;
		}
		const TSharedPtr<FJsonObject>& EntryObject = *EntryObjectPtr;

		FString PreviewPng;
		EntryObject->TryGetStringField(TEXT("previewPng"), PreviewPng);
		if (PreviewPng.IsEmpty())
		{
			EntryObject->TryGetStringField(TEXT("preview_png"), PreviewPng);
		}
		if (PreviewPng.IsEmpty() || !FPaths::FileExists(PreviewPng))
		{
			++MissingCount;
			UE_LOG(LogWoWSkeletalAssetAudit, Warning, TEXT("角色外观纹理 PNG 不存在，跳过: %s"), *PreviewPng);
			continue;
		}

		const FString DestinationPath = MakeGeneratedTextureDestinationPathFromManifestEntry(EntryObject, PreviewPng);
		const FString AssetName = MakeGeneratedTextureAssetNameFromManifestEntry(EntryObject, PreviewPng);
		const FString ObjectPath = DestinationPath / AssetName + TEXT(".") + AssetName;
		if (!bOverwrite && LoadObject<UTexture2D>(nullptr, *ObjectPath))
		{
			++SkippedCount;
			continue;
		}

		UAutomatedAssetImportData* ImportData = NewObject<UAutomatedAssetImportData>();
		ImportData->Filenames.Add(PreviewPng);
		ImportData->DestinationPath = DestinationPath;
		ImportData->bReplaceExisting = bOverwrite;
		ImportData->bSkipReadOnly = true;

		FAssetToolsModule& AssetToolsModule = FModuleManager::LoadModuleChecked<FAssetToolsModule>(TEXT("AssetTools"));
		TArray<UObject*> ImportedAssets = AssetToolsModule.Get().ImportAssetsAutomated(ImportData);

		UTexture2D* ImportedTexture = nullptr;
		for (UObject* ImportedAsset : ImportedAssets)
		{
			ImportedTexture = Cast<UTexture2D>(ImportedAsset);
			if (ImportedTexture)
			{
				break;
			}
		}
		if (!ImportedTexture)
		{
			ImportedTexture = LoadObject<UTexture2D>(nullptr, *ObjectPath);
		}
		if (ImportedTexture)
		{
			ApplyM2TextureAddressSettings(ImportedTexture, 0);
			ImportedTexture->MarkPackageDirty();
			SaveGeneratedAsset(ImportedTexture);
			++ImportedCount;
		}
		else
		{
			++MissingCount;
			UE_LOG(LogWoWSkeletalAssetAudit, Warning, TEXT("角色外观纹理导入失败: png=%s object=%s"), *PreviewPng, *ObjectPath);
		}
	}

	UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("角色外观纹理 manifest 导入完成: manifest=%s imported=%d skipped=%d missing=%d overwrite=%d"),
		*ManifestPath,
		ImportedCount,
		SkippedCount,
		MissingCount,
		bOverwrite ? 1 : 0);
	return MissingCount == 0;
}

void ClearGeneratedLayerTexture(UMaterialInstanceConstant* MaterialInstance)
{
	if (!MaterialInstance)
	{
		return;
	}

	MaterialInstance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("LayerTexture1")), nullptr);
}

UMaterialInterface* SelectBridgeParentMaterial(const FWoWM2AuditSection& Section)
{
	const TCHAR* ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2Opaque.M_WoWM2Opaque");
	const bool bNeedsTwoSidedTranslucentBridge = Section.RenderMode != 0;
	if (Section.RenderMode == 4)
	{
		ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2AdditiveAlphaUnlit.M_WoWM2AdditiveAlphaUnlit");
	}
	else if (Section.RenderMode == 3 || Section.RenderMode == 5 || Section.RenderMode == 6)
	{
		ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2AdditiveSrcColorUnlit.M_WoWM2AdditiveSrcColorUnlit");
	}
	else if (Section.RenderMode == 2)
	{
		// 中文说明：登录/Glue M2 大量使用 BM_ALPHA_BLEND 做天空、云、雾和远景面。
		// 不能落回不透明运行时预览材质，否则会出现整屏半透明方块。
		ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2AlphaBlendUnlit.M_WoWM2AlphaBlendUnlit");
	}
	else if (Section.RenderMode == 1)
	{
		ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2MaskedTwoSided.M_WoWM2MaskedTwoSided");
	}
	else if (bNeedsTwoSidedTranslucentBridge)
	{
		ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2AlphaBlendUnlit.M_WoWM2AlphaBlendUnlit");
	}
	else if (!Section.bTwoSided)
	{
		ParentPath = TEXT("/Game/WoW/Materials/M_WoWM2Opaque.M_WoWM2Opaque");
	}

	UMaterialInterface* ParentMaterial = LoadObject<UMaterialInterface>(nullptr, ParentPath);
	if (!ParentMaterial)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Warning, TEXT("找不到 M2 桥接母材质 %s，回退到 M_WoWM2Opaque。"), ParentPath);
		ParentMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/WoW/Materials/M_WoWM2Opaque.M_WoWM2Opaque"));
	}
	return ParentMaterial;
}

UMaterialInterface* CreateOrUpdateGeneratedSectionMaterial(const FWoWM2AuditSection& Section, const FString& MaterialPackagePath)
{
	UMaterialInterface* ParentMaterial = SelectBridgeParentMaterial(Section);
	UTexture2D* Texture = ImportOrLoadGeneratedTexture(Section.PreviewPng, Section.TextureFlags);
	if (!ParentMaterial || !Texture)
	{
		return ParentMaterial;
	}

	if (UMaterial* ParentBaseMaterial = Cast<UMaterial>(ParentMaterial))
	{
		// 中文说明：UE 原生 SkeletalMesh 需要父材质显式启用 Used with Skeletal Mesh。
		// 注意：这里不能保存共享母材质资产。桥接生成器只应该写 Generated 目录下的实例/网格；
		// 否则 Substrate 项目里反复重存旧母材质，可能把已经验证可用的运行时材质写坏。
		ParentBaseMaterial->SetMaterialUsage(MATUSAGE_SkeletalMesh);
		ParentBaseMaterial->MarkPackageDirty();
	}

	const FString AssetName = FString::Printf(TEXT("MI_M2_Section_%03d"), Section.SectionIndex);
	const FString PackageName = MaterialPackagePath / AssetName;
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		return ParentMaterial;
	}

	UMaterialInstanceConstant* MaterialInstance = FindObject<UMaterialInstanceConstant>(Package, *AssetName);
	if (!MaterialInstance)
	{
		MaterialInstance = NewObject<UMaterialInstanceConstant>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
		FAssetRegistryModule::AssetCreated(MaterialInstance);
	}
	if (!MaterialInstance)
	{
		return ParentMaterial;
	}

	MaterialInstance->SetParentEditorOnly(ParentMaterial, true);
	MaterialInstance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("DiffuseTexture")), Texture);
	if (Section.LayerPreviewPngs.Num() > 1)
	{
		const int32 LayerTextureFlags = Section.LayerTextureFlags.IsValidIndex(1) ? Section.LayerTextureFlags[1] : 0;
		if (UTexture2D* LayerTexture = ImportOrLoadGeneratedTexture(Section.LayerPreviewPngs[1], LayerTextureFlags))
		{
			MaterialInstance->SetTextureParameterValueEditorOnly(FMaterialParameterInfo(TEXT("LayerTexture1")), LayerTexture);
			MaterialInstance->SetScalarParameterValueEditorOnly(
				FMaterialParameterInfo(TEXT("LayerAlphaWeight")),
				ResolveLayerAlphaWeight(Section));
			MaterialInstance->SetScalarParameterValueEditorOnly(
				FMaterialParameterInfo(TEXT("LayerColorWeight")),
				ResolveLayerColorWeight(Section));
		}
		else
		{
			ClearGeneratedLayerTexture(MaterialInstance);
			MaterialInstance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("LayerAlphaWeight")), 0.0f);
			MaterialInstance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("LayerColorWeight")), 0.0f);
		}
	}
	else
	{
		ClearGeneratedLayerTexture(MaterialInstance);
		MaterialInstance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("LayerAlphaWeight")), 0.0f);
		MaterialInstance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("LayerColorWeight")), 0.0f);
	}
	// 中文说明：Section.VertexColors 已经承载了导出器从 M2 Color/Transparency 轨道采样出来的结果。
	// 如果这里再把同一份值写进 TintColor，母材质里会把同一轨道重复乘一次，
	// 尤其是雪幕这种本来 alpha 就不高的贴图，会被压到几乎不可见。
	// 因此桥接实例默认保持白色 Tint，M2 轨道只走一条链。
	MaterialInstance->SetVectorParameterValueEditorOnly(FMaterialParameterInfo(TEXT("TintColor")), FLinearColor::White);
	// 中文说明：Brightness 不是 M2 内置字段。登录界面这类贴图层若统一压到 0.55，
	// 会把本来就依赖细小 alpha 的雪幕进一步压暗，导致只在深色背景上勉强可见。
	// 在实现权威 WoW 固定管线材质前，这里保持 1.0，避免再引入非资产来源的全局衰减。
	MaterialInstance->SetScalarParameterValueEditorOnly(FMaterialParameterInfo(TEXT("Brightness")), 1.0f);
	MaterialInstance->PostEditChange();
	MaterialInstance->MarkPackageDirty();
	SaveGeneratedAsset(MaterialInstance);
	return MaterialInstance;
}

void CleanGeneratedMaterialPackagePathForOverwrite(const FString& MaterialPackagePath, bool bOverwrite)
{
	if (!bOverwrite || !MaterialPackagePath.StartsWith(TEXT("/Game/WoW/Generated/")))
	{
		return;
	}

	const FString DiskPath = FPackageName::LongPackageNameToFilename(MaterialPackagePath);
	TArray<FString> MaterialFiles;
	IFileManager::Get().FindFiles(MaterialFiles, *(DiskPath / TEXT("MI_M2_Section_*.uasset")), true, false);
	for (const FString& FileName : MaterialFiles)
	{
		const FString FullPath = DiskPath / FileName;
		IFileManager::Get().Delete(*FullPath, false, true, true);
	}
}

void AssignGeneratedMaterialsToSections(
	const TArray<FWoWM2AuditSection>& Sections,
	const FString& MaterialPackagePath,
	TArray<FSkeletalMaterial>& InOutMaterials)
{
	const int32 Count = FMath::Min(Sections.Num(), InOutMaterials.Num());
	for (int32 SectionArrayIndex = 0; SectionArrayIndex < Count; ++SectionArrayIndex)
	{
		UMaterialInterface* Material = CreateOrUpdateGeneratedSectionMaterial(Sections[SectionArrayIndex], MaterialPackagePath);
		if (Material)
		{
			InOutMaterials[SectionArrayIndex].MaterialInterface = Material;
		}
	}
}

void ImportParticleEmitterTextures(const TArray<FWoWM2AuditParticleEmitter>& ParticleEmitters)
{
	int32 ImportedCount = 0;
	for (const FWoWM2AuditParticleEmitter& Emitter : ParticleEmitters)
	{
		if (ImportOrLoadGeneratedTexture(Emitter.PreviewPng))
		{
			++ImportedCount;
		}
	}

	if (ParticleEmitters.Num() > 0)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("M2 particle贴图导入: emitters=%d imported=%d"), ParticleEmitters.Num(), ImportedCount);
	}
}

void ImportSectionLayerTextures(const TArray<FWoWM2AuditSection>& Sections)
{
	int32 RequestedCount = 0;
	int32 ImportedCount = 0;
	TSet<FString> SeenPreviewPngs;
	for (const FWoWM2AuditSection& Section : Sections)
	{
		for (int32 LayerIndex = 0; LayerIndex < Section.LayerPreviewPngs.Num(); ++LayerIndex)
		{
			const FString& PreviewPng = Section.LayerPreviewPngs[LayerIndex];
			if (PreviewPng.IsEmpty() || SeenPreviewPngs.Contains(PreviewPng))
			{
				continue;
			}

			SeenPreviewPngs.Add(PreviewPng);
			++RequestedCount;
			const int32 TextureFlags = Section.LayerTextureFlags.IsValidIndex(LayerIndex) ? Section.LayerTextureFlags[LayerIndex] : 0;
			if (ImportOrLoadGeneratedTexture(PreviewPng, TextureFlags))
			{
				++ImportedCount;
			}
		}
	}

	if (RequestedCount > 0)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("M2 section多贴图层导入: requested=%d imported=%d"), RequestedCount, ImportedCount);
	}
}

bool SaveNewSkeletonAsset(
	const FString& PackageName,
	const TArray<FWoWM2AuditBone>& Bones,
	USkeletalMesh* SourceSkeletalMesh,
	bool bOverwrite,
	USkeleton*& OutSkeleton)
{
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("非法 UE 包名: %s"), *PackageName);
		return false;
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("无法创建 UE 包: %s"), *PackageName);
		return false;
	}

	if (!RetireExistingAssetForOverwrite(Package, AssetName, USkeleton::StaticClass(), bOverwrite))
	{
		return false;
	}
	if (!DeleteExistingPackageFilesForOverwrite(PackageName, bOverwrite))
	{
		return false;
	}

	USkeleton* Skeleton = NewObject<USkeleton>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!Skeleton)
	{
		return false;
	}

	if (SourceSkeletalMesh)
	{
		// 中文说明：UE 官方 SkeletonFactory 也是从 SkeletalMesh 的 RefSkeleton 反建 Skeleton。
		// 这样会同步创建 BoneTree / ReferenceSkeleton / 兼容性数据，Persona 才认为这是“有效骨骼”。
		if (!Skeleton->MergeAllBonesToBoneTree(SourceSkeletalMesh, false))
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("从 SkeletalMesh 生成 Skeleton BoneTree 失败: %s"), *PackageName);
			return false;
		}
		Skeleton->SetPreviewMesh(SourceSkeletalMesh, false);
		if (SourceSkeletalMesh->GetSkeleton() != Skeleton)
		{
			SourceSkeletalMesh->SetSkeleton(Skeleton);
			SourceSkeletalMesh->MarkPackageDirty();
		}
		SaveGeneratedAsset(SourceSkeletalMesh);
	}
	else
	{
		FReferenceSkeleton ReferenceSkeleton;
		if (!BuildReferenceSkeletonFromM2Bones(Bones, ReferenceSkeleton))
		{
			return false;
		}

		FReferenceSkeletonModifier Modifier(Skeleton);
		for (int32 BoneIndex = 0; BoneIndex < ReferenceSkeleton.GetRawBoneNum(); ++BoneIndex)
		{
			Modifier.Add(ReferenceSkeleton.GetRawRefBoneInfo()[BoneIndex], ReferenceSkeleton.GetRawRefBonePose()[BoneIndex]);
		}
	}

	FAssetRegistryModule::AssetCreated(Skeleton);
	Package->MarkPackageDirty();

	const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, Skeleton, *PackageFileName, SaveArgs))
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("保存 Skeleton 失败: %s"), *PackageFileName);
		return false;
	}

	OutSkeleton = Skeleton;
	return true;
}

bool SaveNewSkeletalMeshAsset(
	const FString& PackageName,
	const FString& MaterialPackagePath,
	const TArray<FWoWM2AuditBone>& Bones,
	const TArray<FWoWM2AuditSection>& Sections,
	const TArray<FWoWM2AuditAttachment>& Attachments,
	USkeleton* Skeleton,
	bool bOverwrite,
	USkeletalMesh*& OutSkeletalMesh)
{
	if (!FPackageName::IsValidLongPackageName(PackageName))
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("非法 UE 包名: %s"), *PackageName);
		return false;
	}

	const FString AssetName = FPackageName::GetLongPackageAssetName(PackageName);
	UPackage* Package = CreatePackage(*PackageName);
	if (!Package)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("无法创建 UE 包: %s"), *PackageName);
		return false;
	}

	if (!RetireExistingAssetForOverwrite(Package, AssetName, USkeletalMesh::StaticClass(), bOverwrite))
	{
		return false;
	}
	if (!DeleteExistingPackageFilesForOverwrite(PackageName, bOverwrite))
	{
		return false;
	}

	FReferenceSkeleton ReferenceSkeleton;
	if (!BuildReferenceSkeletonFromM2Bones(Bones, ReferenceSkeleton))
	{
		return false;
	}

	FMeshDescription MeshDescription;
	TArray<FSkeletalMaterial> Materials;
	if (!BuildMeshDescriptionFromM2Sections(Sections, ReferenceSkeleton, MeshDescription, Materials))
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("生成 MeshDescription 失败: %s"), *PackageName);
		return false;
	}
	AssignGeneratedMaterialsToSections(Sections, MaterialPackagePath, Materials);

	USkeletalMesh* SkeletalMesh = NewObject<USkeletalMesh>(Package, *AssetName, RF_Public | RF_Standalone | RF_Transactional);
	if (!SkeletalMesh)
	{
		return false;
	}

	const FMeshDescription* MeshDescriptionPtr = &MeshDescription;
	FStaticToSkeletalMeshConverter::FInitializationParams InitParams;
	InitParams.Materials = Materials;
	InitParams.bRecomputeNormals = false;
	InitParams.bRecomputeTangents = true;
	InitParams.bCacheOptimize = true;

	if (!FStaticToSkeletalMeshConverter::InitializeSkeletalMeshFromMeshDescriptions(
		SkeletalMesh,
		MakeArrayView(&MeshDescriptionPtr, 1),
		ReferenceSkeleton,
		InitParams))
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("UE SkeletalMesh 初始化失败: %s"), *PackageName);
		return false;
	}
	if (Skeleton)
	{
		SkeletalMesh->SetSkeleton(Skeleton);
		Skeleton->SetPreviewMesh(SkeletalMesh, false);
		if (!Skeleton->MergeAllBonesToBoneTree(SkeletalMesh, false))
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("Skeleton 合并 SkeletalMesh 骨骼树失败: %s"), *PackageName);
			return false;
		}
		Skeleton->MarkPackageDirty();
		SaveGeneratedAsset(Skeleton);
	}

	const int32 AddedSocketCount = AddM2AttachmentSocketsToSkeletalMesh(SkeletalMesh, Attachments, Bones);
	UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("已从 M2 attachments 生成 UE mesh sockets: %d/%d"), AddedSocketCount, Attachments.Num());

	FAssetRegistryModule::AssetCreated(SkeletalMesh);
	Package->MarkPackageDirty();

	const FString PackageFileName = FPackageName::LongPackageNameToFilename(PackageName, FPackageName::GetAssetPackageExtension());
	FSavePackageArgs SaveArgs;
	SaveArgs.TopLevelFlags = RF_Public | RF_Standalone;
	SaveArgs.SaveFlags = SAVE_NoError;
	if (!UPackage::SavePackage(Package, SkeletalMesh, *PackageFileName, SaveArgs))
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("保存 SkeletalMesh 失败: %s"), *PackageFileName);
		return false;
	}

	OutSkeletalMesh = SkeletalMesh;
	return true;
}

FString MakeDefaultModelName(const TSharedPtr<FJsonObject>& RootObject, const FString& InputPath)
{
	FString SourceM2 = GetStringFieldOrEmpty(RootObject, TEXT("source_m2"));
	FString BaseName = FPaths::GetBaseFilename(SourceM2);
	if (BaseName.IsEmpty())
	{
		BaseName = FPaths::GetBaseFilename(InputPath);
	}
	return SanitizeAssetName(BaseName);
}

bool ReadJsonObjectFile(const FString& InputPath, TSharedPtr<FJsonObject>& OutRootObject)
{
	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *InputPath))
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("无法读取 M2 预览 JSON: %s"), *InputPath);
		return false;
	}

	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, OutRootObject) || !OutRootObject.IsValid())
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("无法解析 M2 预览 JSON: %s"), *InputPath);
		return false;
	}

	return true;
}

FString MakeDefaultAppearancePathForMeshJson(const FString& InputPath)
{
	const FString Directory = FPaths::GetPath(InputPath);
	const FString Filename = FPaths::GetCleanFilename(InputPath);
	const FString MeshJsonSuffix = TEXT(".mesh.json");

	if (Filename.EndsWith(MeshJsonSuffix, ESearchCase::IgnoreCase))
	{
		FString AppearanceName = Filename.LeftChop(MeshJsonSuffix.Len());
		AppearanceName += TEXT(".appearance.json");
		return Directory / AppearanceName;
	}

	return Directory / (FPaths::GetBaseFilename(InputPath) + TEXT(".appearance.json"));
}
}

UWoWSkeletalAssetAuditCommandlet::UWoWSkeletalAssetAuditCommandlet()
{
	IsClient = false;
	IsEditor = true;
	LogToConsole = true;
}

int32 UWoWSkeletalAssetAuditCommandlet::Main(const FString& Params)
{
	const bool bOverwrite = FParse::Param(*Params, TEXT("Overwrite"));
	FString TextureManifestPath;
	if (FParse::Value(*Params, TEXT("TextureManifest="), TextureManifestPath) && !TextureManifestPath.IsEmpty())
	{
		TextureManifestPath = FPaths::ConvertRelativePathToFull(TextureManifestPath);
		FPaths::NormalizeFilename(TextureManifestPath);
		return ImportCharacterAppearanceTextureManifest(TextureManifestPath, bOverwrite) ? 0 : 11;
	}

	FString InputPath;
	if (!FParse::Value(*Params, TEXT("Input="), InputPath) || InputPath.IsEmpty())
	{
		// 中文说明：这个默认输入只用于第一阶段资产桥接验证，后续会替换为二进制缓存或直接 M2/SKIN 读取。
		InputPath = FPaths::ProjectSavedDir() / TEXT("M2Preview/HumanMale.mesh.json");
	}

	InputPath = FPaths::ConvertRelativePathToFull(InputPath);
	FPaths::NormalizeFilename(InputPath);

	TSharedPtr<FJsonObject> RootObject;
	if (!ReadJsonObjectFile(InputPath, RootObject))
	{
		return 1;
	}

	const FString Format = GetStringFieldOrEmpty(RootObject, TEXT("format"));
	const FString SourceM2 = GetStringFieldOrEmpty(RootObject, TEXT("source_m2"));
	const FString SourceSkin = GetStringFieldOrEmpty(RootObject, TEXT("source_skin"));
	const int32 VertexCount = RootObject->GetIntegerField(TEXT("vertex_count"));
	const int32 TriangleCount = RootObject->GetIntegerField(TEXT("triangle_count"));
	const int32 SectionCount = CountArrayField(RootObject, TEXT("sections"));
	const int32 BoneCount = CountArrayField(RootObject, TEXT("bones"));
	const int32 AnimationCount = CountArrayField(RootObject, TEXT("animations"));
	const int32 AttachmentCount = CountArrayField(RootObject, TEXT("attachments"));
	const int32 MaterialCount = CountArrayField(RootObject, TEXT("materials"));
	const int32 RibbonCount = CountArrayField(RootObject, TEXT("ribbon_emitters"));

	TArray<FWoWM2AuditBone> Bones;
	TArray<FWoWM2AuditAttachment> Attachments;
	TArray<FWoWM2AuditSection> Sections;
	TArray<FWoWM2AuditParticleEmitter> ParticleEmitters;
	const bool bReadBones = ReadM2Bones(RootObject, Bones);
	const bool bReadAttachments = ReadM2Attachments(RootObject, Attachments);
	const bool bReadSections = ReadM2Sections(RootObject, Sections);
	ReadM2ParticleEmitters(RootObject, ParticleEmitters);
	const FString ModelName = MakeDefaultModelName(RootObject, InputPath);
	const FString DefaultGeneratedRoot = TEXT("/Game/WoW/Generated/") + ModelName;

	UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("WoW M2 skeletal asset audit"));
	UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("Input: %s"), *InputPath);
	UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("Format: %s"), *Format);
	UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("Source M2: %s"), *SourceM2);
	UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("Source Skin: %s"), *SourceSkin);
	UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("Vertices=%d Triangles=%d Sections=%d"), VertexCount, TriangleCount, SectionCount);
	UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("Bones=%d Animations=%d Attachments=%d Materials=%d Ribbons=%d"), BoneCount, AnimationCount, AttachmentCount, MaterialCount, RibbonCount);

	if (BoneCount <= 0 || SectionCount <= 0 || VertexCount <= 0 || TriangleCount <= 0)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("输入数据不足，不能进入 UE 原生骨骼资产生成阶段。"));
		return 2;
	}

	if (AnimationCount <= 0)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Warning, TEXT("没有检测到动画。仍可生成静态骨骼网格，但不能生成 UAnimSequence。"));
	}

	if (AttachmentCount <= 0)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Warning, TEXT("没有检测到挂载点。后续无法权威生成武器/坐骑/特效 Socket。"));
	}

	if (!bReadBones)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("无法读取 M2 bones[] 详细数据。"));
		return 3;
	}

	if (!bReadAttachments)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Warning, TEXT("无法读取 M2 attachments[] 详细数据；本次只能生成骨架，不能审计挂点。"));
	}

	if (!bReadSections)
	{
		UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("无法读取 M2 sections[] 详细数据。"));
		return 5;
	}

	FString SkeletonPackage = DefaultGeneratedRoot / (ModelName + TEXT("_M2Skeleton"));
	FString MeshPackage = DefaultGeneratedRoot / (ModelName + TEXT("_M2Mesh"));
	FString MaterialPackagePath = DefaultGeneratedRoot / TEXT("Materials");
	FString AnimationPackagePath = DefaultGeneratedRoot / TEXT("Animations");
	FString AppearancePath = MakeDefaultAppearancePathForMeshJson(InputPath);
	FString SectionVisibilityPath = MakeSectionVisibilityPathForMeshJson(InputPath);
	FParse::Value(*Params, TEXT("SkeletonPackage="), SkeletonPackage);
	FParse::Value(*Params, TEXT("MeshPackage="), MeshPackage);
	FParse::Value(*Params, TEXT("MaterialPackagePath="), MaterialPackagePath);
	FParse::Value(*Params, TEXT("AnimationPackagePath="), AnimationPackagePath);
	FParse::Value(*Params, TEXT("Appearance="), AppearancePath);
	FParse::Value(*Params, TEXT("SectionVisibility="), SectionVisibilityPath);
	AppearancePath = FPaths::ConvertRelativePathToFull(AppearancePath);
	SectionVisibilityPath = FPaths::ConvertRelativePathToFull(SectionVisibilityPath);
	FPaths::NormalizeFilename(AppearancePath);
	FPaths::NormalizeFilename(SectionVisibilityPath);

	TArray<FWoWM2AuditSection> MeshSections = Sections;
	if (!FParse::Param(*Params, TEXT("NoGeosetFilter")))
	{
		TMap<int32, int32> GeosetGroups = LoadConfiguredGeosetsFromSectionVisibility(SectionVisibilityPath);
		if (GeosetGroups.IsEmpty())
		{
			GeosetGroups = LoadConfiguredGeosetsFromAppearance(AppearancePath);
		}
		BuildDefaultGeosetSelectionFromSections(Sections, GeosetGroups);
		MeshSections = FilterSectionsByGeosets(Sections, GeosetGroups);
		UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("Geoset过滤: inputSections=%d visibleSections=%d groups=%d sectionVisibility=%s appearance=%s"),
			Sections.Num(),
			MeshSections.Num(),
			GeosetGroups.Num(),
			*SectionVisibilityPath,
			*AppearancePath);
	}
	WriteM2SectionRenderAudit(InputPath, MeshSections);
	ImportSectionLayerTextures(MeshSections);
	ImportParticleEmitterTextures(ParticleEmitters);

	USkeleton* BridgeSkeleton = nullptr;
	USkeletalMesh* BridgeMesh = nullptr;

	const bool bCreateSkeleton = FParse::Param(*Params, TEXT("CreateSkeleton"));
	const bool bCreateSkeletalMesh = FParse::Param(*Params, TEXT("CreateSkeletalMesh"));
	const bool bCreateAnimSequences = FParse::Param(*Params, TEXT("CreateAnimSequences"));
	const bool bCreateAnimationCatalog = bCreateAnimSequences || FParse::Param(*Params, TEXT("CreateAnimationCatalog"));

	if (bCreateSkeletalMesh)
	{
		if (MeshSections.IsEmpty())
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("Geoset过滤后没有任何可见 section，不能生成 SkeletalMesh。"));
			return 6;
		}

		if (!bCreateSkeleton)
		{
			BridgeSkeleton = LoadObject<USkeleton>(nullptr, *MakeObjectPathFromPackageName(SkeletonPackage));
		}

		CleanGeneratedMaterialPackagePathForOverwrite(MaterialPackagePath, bOverwrite);
		if (!SaveNewSkeletalMeshAsset(MeshPackage, MaterialPackagePath, Bones, MeshSections, Attachments, BridgeSkeleton, bOverwrite, BridgeMesh))
		{
			return 6;
		}

		UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("已生成 UE SkeletalMesh: %s sections=%d ueBones=%d"),
			*MeshPackage,
			MeshSections.Num(),
			Bones.Num() + 1);
	}

	if (bCreateSkeleton)
	{
		if (!BridgeMesh)
		{
			BridgeMesh = LoadObject<USkeletalMesh>(nullptr, *MakeObjectPathFromPackageName(MeshPackage));
		}
		if (!SaveNewSkeletonAsset(SkeletonPackage, Bones, BridgeMesh, bOverwrite, BridgeSkeleton))
		{
			return 4;
		}

		if (BridgeMesh && BridgeMesh->GetSkeleton() != BridgeSkeleton)
		{
			BridgeMesh->SetSkeleton(BridgeSkeleton);
			BridgeMesh->MarkPackageDirty();
		}
		if (BridgeMesh)
		{
			// 中文说明：从 SkeletalMesh 反建 Skeleton 后，Skeleton 引用是写回到 Mesh 资产上的。
			// 必须无条件再保存一次 Mesh；否则命令行进程退出后磁盘上的 HumanMale_M2Mesh 仍然是 Skeleton=None。
			SaveGeneratedAsset(BridgeMesh);
		}

		UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("已生成 UE Skeleton: %s bones=%d"), *SkeletonPackage, Bones.Num());
		UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("M2 attachments 审计数量: %d；Socket 将在 USkeletalMesh 生成阶段一起落地。"), Attachments.Num());
	}

	if (bCreateAnimationCatalog)
	{
		if (!BridgeSkeleton)
		{
			BridgeSkeleton = LoadObject<USkeleton>(nullptr, *MakeObjectPathFromPackageName(SkeletonPackage));
		}
		if (!BridgeMesh)
		{
			BridgeMesh = LoadObject<USkeletalMesh>(nullptr, *MakeObjectPathFromPackageName(MeshPackage));
		}
		if (!BridgeSkeleton)
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("无法生成 AnimSequence：找不到 SkeletonPackage=%s"), *SkeletonPackage);
			return 7;
		}

		TArray<FWoWM2AuditAnimationClip> Clips;
		TArray<int32> AnimationLookup;
		TArray<int32> KeyBoneLookup;
		ReadIntArrayField(RootObject, TEXT("animation_lookup"), AnimationLookup);
		ReadIntArrayField(RootObject, TEXT("key_bone_lookup"), KeyBoneLookup);
		const FWoWAnimIdSelection AnimIdSelection = ParseAnimIdSelection(Params);
		const TSet<int32> WantedAnimIds = BuildWantedAnimIdSet(AnimIdSelection, AnimationLookup);
		const bool bAllAnimSequences = FParse::Param(*Params, TEXT("AllAnimSequences"));
		const bool bIncludeSubAnimations = bAllAnimSequences || FParse::Param(*Params, TEXT("IncludeSubAnims"));
		const bool bCreateGripVariants = FParse::Param(*Params, TEXT("CreateGripVariants"));
		if (FParse::Param(*Params, TEXT("CleanAnimSequenceFolder")) &&
			!CleanGeneratedAnimationPackagePathForOverwrite(AnimationPackagePath, bOverwrite))
		{
			return 8;
		}
		if (!ReadM2AnimationClips(RootObject, WantedAnimIds, bIncludeSubAnimations, Clips))
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("无法读取 M2 animations[] 详细数据。"));
			return 9;
		}
		if (Clips.IsEmpty())
		{
			UE_LOG(LogWoWSkeletalAssetAudit, Error, TEXT("没有找到请求的动画 AnimIds。"));
			return 10;
		}
		UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("M2 动画导入选择: mode=%d requested=%d clips=%d lookupEntries=%d includeSub=%d gripVariants=%d"),
			static_cast<int32>(AnimIdSelection.Mode),
			WantedAnimIds.Num(),
			Clips.Num(),
			AnimationLookup.Num(),
			bIncludeSubAnimations ? 1 : 0,
			bCreateGripVariants ? 1 : 0);

		const FWoWM2AuditAnimationClip* HandsClosedClip = nullptr;
		for (const FWoWM2AuditAnimationClip& Clip : Clips)
		{
			const int32 LogicalAnimationId = Clip.LogicalAnimationId == INDEX_NONE ? Clip.AnimationId : Clip.LogicalAnimationId;
			if (LogicalAnimationId == 15 && Clip.SubAnimationId == 0)
			{
				HandsClosedClip = &Clip;
				break;
			}
		}

		TSet<int32> RightGripBoneIndices;
		TSet<int32> LeftGripBoneIndices;
		if (bCreateGripVariants)
		{
			if (!HandsClosedClip)
			{
				UE_LOG(LogWoWSkeletalAssetAudit, Warning, TEXT("-CreateGripVariants 已启用，但当前 Clips 中没有 HandsClosed(15,0)。请把 -AnimIds 加入 15。"));
			}
			RightGripBoneIndices = BuildM2GripBoneSet(Bones, KeyBoneLookup, EWoWGeneratedGripVariant::RightHand);
			LeftGripBoneIndices = BuildM2GripBoneSet(Bones, KeyBoneLookup, EWoWGeneratedGripVariant::LeftHand);
			UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("M2 握持骨骼集合: right=%d left=%d keyBoneLookup=%d"),
				RightGripBoneIndices.Num(),
				LeftGripBoneIndices.Num(),
				KeyBoneLookup.Num());
		}

		int32 GeneratedAnimCount = 0;
		TArray<FWoWM2GeneratedAnimationCatalogEntry> GeneratedAnimationCatalogEntries;
		for (const FWoWM2AuditAnimationClip& Clip : Clips)
		{
			const FString AnimAssetName = MakeM2AnimationAssetName(ModelName, Clip);
			const FString AnimPackage = AnimationPackagePath / AnimAssetName;
			const int32 LogicalAnimationId = Clip.LogicalAnimationId == INDEX_NONE ? Clip.AnimationId : Clip.LogicalAnimationId;
			const int32 LookupSequenceIndex = AnimationLookup.IsValidIndex(LogicalAnimationId) ? AnimationLookup[LogicalAnimationId] : INDEX_NONE;
			if (Clip.SubAnimationId == 0 && LookupSequenceIndex != INDEX_NONE && LookupSequenceIndex != Clip.SequenceIndex)
			{
				UE_LOG(LogWoWSkeletalAssetAudit, Warning, TEXT("M2 animation_lookup 校验不一致: logicalAnim=%d sourceAnim=%d lookupSequence=%d clipSequence=%d。仍按当前 clip 导出。"),
					LogicalAnimationId,
					Clip.AnimationId,
					LookupSequenceIndex,
					Clip.SequenceIndex);
			}

			UAnimSequence* AnimSequence = nullptr;
			if (bCreateAnimSequences && !SaveNewAnimSequenceAsset(AnimPackage, Clip, Bones, BridgeSkeleton, BridgeMesh, bOverwrite, AnimSequence))
			{
				return 11;
			}
			if (bCreateAnimSequences)
			{
				++GeneratedAnimCount;
			}
			FWoWM2GeneratedAnimationCatalogEntry CatalogEntry;
			CatalogEntry.LogicalAnimationId = LogicalAnimationId;
			CatalogEntry.SourceAnimationId = Clip.AnimationId;
			CatalogEntry.SubAnimationId = Clip.SubAnimationId;
			CatalogEntry.SequenceIndex = Clip.SequenceIndex;
			CatalogEntry.LookupSequenceIndex = LookupSequenceIndex;
			CatalogEntry.LengthMs = Clip.LengthMs;
			CatalogEntry.SampleRate = Clip.SampleRate;
			CatalogEntry.FrameCount = Clip.Frames.Num();
			CatalogEntry.Name = GetM2AnimationName(LogicalAnimationId) ? GetM2AnimationName(LogicalAnimationId) : TEXT("Unknown");
			CatalogEntry.ObjectPath = MakeObjectPathFromPackageName(AnimPackage);
			CatalogEntry.Variant = FString();
			GeneratedAnimationCatalogEntries.Add(MoveTemp(CatalogEntry));
			if (bCreateAnimSequences)
			{
				UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("已生成 UE AnimSequence: %s logicalAnim=%d sourceAnim=%d name=%s sub=%d sequence=%d lookupSequence=%d frames=%d rate=%.2f"),
					*AnimPackage,
					LogicalAnimationId,
					Clip.AnimationId,
					GetM2AnimationName(LogicalAnimationId) ? GetM2AnimationName(LogicalAnimationId) : TEXT("Unknown"),
					Clip.SubAnimationId,
					Clip.SequenceIndex,
					LookupSequenceIndex,
					Clip.Frames.Num(),
					Clip.SampleRate);
			}
			else
			{
				UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("已登记 AnimationCatalog 项: %s logicalAnim=%d sourceAnim=%d name=%s sub=%d sequence=%d lookupSequence=%d frames=%d rate=%.2f"),
					*AnimPackage,
					LogicalAnimationId,
					Clip.AnimationId,
					GetM2AnimationName(LogicalAnimationId) ? GetM2AnimationName(LogicalAnimationId) : TEXT("Unknown"),
					Clip.SubAnimationId,
					Clip.SequenceIndex,
					LookupSequenceIndex,
					Clip.Frames.Num(),
					Clip.SampleRate);
			}

			const bool bShouldCreateGripVariantForClip =
				bCreateGripVariants &&
				HandsClosedClip &&
				AnimIdSelection.Mode == EWoWAnimIdSelectionMode::Explicit &&
				LogicalAnimationId != 15;
			if (bShouldCreateGripVariantForClip)
			{
				const struct FGripVariantRequest
				{
					EWoWGeneratedGripVariant Variant;
					const TSet<int32>* BoneIndices;
				} GripVariantRequests[] = {
					{ EWoWGeneratedGripVariant::RightHand, &RightGripBoneIndices },
					{ EWoWGeneratedGripVariant::LeftHand, &LeftGripBoneIndices },
				};

				for (const FGripVariantRequest& GripRequest : GripVariantRequests)
				{
					if (!GripRequest.BoneIndices || GripRequest.BoneIndices->IsEmpty())
					{
						continue;
					}

					const FString GripAnimAssetName = MakeM2AnimationAssetName(ModelName, Clip, GripRequest.Variant);
					const FString GripAnimPackage = AnimationPackagePath / GripAnimAssetName;
					UAnimSequence* GripAnimSequence = nullptr;
					if (bCreateAnimSequences && !SaveNewAnimSequenceAsset(
						GripAnimPackage,
						Clip,
						Bones,
						BridgeSkeleton,
						BridgeMesh,
						bOverwrite,
						GripAnimSequence,
						HandsClosedClip,
						GripRequest.BoneIndices))
					{
						return 12;
					}
					if (bCreateAnimSequences)
					{
						++GeneratedAnimCount;
					}
					FWoWM2GeneratedAnimationCatalogEntry GripCatalogEntry;
					GripCatalogEntry.LogicalAnimationId = LogicalAnimationId;
					GripCatalogEntry.SourceAnimationId = Clip.AnimationId;
					GripCatalogEntry.SubAnimationId = Clip.SubAnimationId;
					GripCatalogEntry.SequenceIndex = Clip.SequenceIndex;
					GripCatalogEntry.LookupSequenceIndex = LookupSequenceIndex;
					GripCatalogEntry.LengthMs = Clip.LengthMs;
					GripCatalogEntry.SampleRate = Clip.SampleRate;
					GripCatalogEntry.FrameCount = Clip.Frames.Num();
					GripCatalogEntry.Name = GetM2AnimationName(LogicalAnimationId) ? GetM2AnimationName(LogicalAnimationId) : TEXT("Unknown");
					GripCatalogEntry.ObjectPath = MakeObjectPathFromPackageName(GripAnimPackage);
					GripCatalogEntry.Variant = GripRequest.Variant == EWoWGeneratedGripVariant::RightHand ? TEXT("GripR") : TEXT("GripL");
					GeneratedAnimationCatalogEntries.Add(MoveTemp(GripCatalogEntry));
					if (bCreateAnimSequences)
					{
						UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("已生成 UE Grip AnimSequence: %s logicalAnim=%d sourceAnim=%d sub=%d variant=%d"),
							*GripAnimPackage,
							LogicalAnimationId,
							Clip.AnimationId,
							Clip.SubAnimationId,
							static_cast<int32>(GripRequest.Variant));
					}
					else
					{
						UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("已登记 Grip AnimationCatalog 项: %s logicalAnim=%d sourceAnim=%d sub=%d variant=%d"),
							*GripAnimPackage,
							LogicalAnimationId,
							Clip.AnimationId,
							Clip.SubAnimationId,
							static_cast<int32>(GripRequest.Variant));
					}
				}
			}
		}
		if (!SaveAnimationCatalogFile(
			AnimationPackagePath,
			ModelName,
			MakeObjectPathFromPackageName(MeshPackage),
			MakeObjectPathFromPackageName(SkeletonPackage),
			GeneratedAnimationCatalogEntries))
		{
			return 13;
		}
		UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("已生成 UE AnimSequence 数量: %d"), GeneratedAnimCount);
	}

	UE_LOG(LogWoWSkeletalAssetAudit, Display, TEXT("审计通过：下一步可以基于这些字段生成 USkeleton / USkeletalMesh / sockets。"));
	return 0;
}
