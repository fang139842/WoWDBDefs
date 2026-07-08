#include "WoWRuntimeCharacterAppearance.h"

#include "WoWCharacterEquipment.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Engine/Texture2D.h"
#include "IImageWrapper.h"
#include "IImageWrapperModule.h"
#include "Json.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/Crc.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Modules/ModuleManager.h"
#include "UObject/SoftObjectPath.h"

#include <initializer_list>
#include <string>

namespace
{
struct FRuntimeCharSectionKey
{
	int32 Race = 0;
	int32 Gender = 0;
	int32 SectionType = 0;
	int32 Section = 0;
	int32 Color = 0;

	friend uint32 GetTypeHash(const FRuntimeCharSectionKey& Key)
	{
		uint32 Hash = HashCombine(::GetTypeHash(Key.Race), ::GetTypeHash(Key.Gender));
		Hash = HashCombine(Hash, ::GetTypeHash(Key.SectionType));
		Hash = HashCombine(Hash, ::GetTypeHash(Key.Section));
		Hash = HashCombine(Hash, ::GetTypeHash(Key.Color));
		return Hash;
	}

	bool operator==(const FRuntimeCharSectionKey& Other) const
	{
		return Race == Other.Race &&
			Gender == Other.Gender &&
			SectionType == Other.SectionType &&
			Section == Other.Section &&
			Color == Other.Color;
	}
};

struct FRuntimeCharSectionRecord
{
	int32 Race = 0;
	int32 Gender = 0;
	int32 SectionType = 0;
	int32 Section = 0;
	int32 Color = 0;
	FString Tex1;
	FString Tex2;
	FString Tex3;
};

struct FRuntimeItemDisplayTextureRecord
{
	int32 DisplayId = 0;
	FString Model;
	FString Model2;
	FString Skin;
	FString Skin2;
	FString ArmUpper;
	FString ArmLower;
	FString Hand;
	FString TorsoUpper;
	FString TorsoLower;
	FString LegUpper;
	FString LegLower;
	FString Foot;
};

struct FRuntimeCharHairGeosetRecord
{
	int32 Race = 0;
	int32 Gender = 0;
	int32 HairStyle = 0;
	int32 GeosetId = 0;
	bool bBald = false;
};

struct FRuntimeFacialHairStyleRecord
{
	int32 Race = 0;
	int32 Gender = 0;
	int32 Style = 0;
	int32 Geoset100 = 0;
	int32 Geoset200 = 0;
	int32 Geoset300 = 0;
};

struct FRuntimeAppearanceDbcCache
{
	bool bLoaded = false;
	bool bAttemptedLoad = false;
	TMap<FRuntimeCharSectionKey, FRuntimeCharSectionRecord> CharSectionsByKey;
	TMultiMap<uint64, FRuntimeCharSectionRecord> CharSectionsByGroup;
	TMap<int32, FRuntimeItemDisplayTextureRecord> ItemDisplayTexturesById;
	TArray<FRuntimeCharHairGeosetRecord> CharHairGeosets;
	TArray<FRuntimeFacialHairStyleRecord> FacialHairStyles;
};

struct FRuntimeBodyAtlasStats
{
	int32 AppliedLayers = 0;
	int32 MissingLayers = 0;
};

using FEquipmentDisplay = FWoWCharacterEquipmentDisplay;

TMap<FString, TObjectPtr<UTexture2D>>& GetBodyAtlasCache()
{
	static TMap<FString, TObjectPtr<UTexture2D>> CachedAtlases;
	return CachedAtlases;
}

TMap<FString, FRuntimeBodyAtlasStats>& GetBodyAtlasStatsCache()
{
	static TMap<FString, FRuntimeBodyAtlasStats> CachedStats;
	return CachedStats;
}

FRuntimeAppearanceDbcCache& GetRuntimeAppearanceDbcCacheStorage()
{
	static FRuntimeAppearanceDbcCache Cache;
	return Cache;
}

TSharedPtr<FJsonObject> TryReadJsonObject(const TSharedPtr<FJsonValue>& Value)
{
	const TSharedPtr<FJsonObject>* Object = nullptr;
	if (!Value.IsValid() || !Value->TryGetObject(Object) || !Object || !Object->IsValid())
	{
		return nullptr;
	}
	return *Object;
}

int32 ResolveRuntimeMaterialSlotForM2Section(const USkeletalMeshComponent* SkeletalMeshComponent, int32 SectionIndex)
{
	if (!SkeletalMeshComponent || SectionIndex < 0)
	{
		return INDEX_NONE;
	}

	const TArray<FName> MaterialSlotNames = SkeletalMeshComponent->GetMaterialSlotNames();
	for (int32 SlotIndex = 0; SlotIndex < MaterialSlotNames.Num(); ++SlotIndex)
	{
		const FString SlotName = MaterialSlotNames[SlotIndex].ToString();
		if (!SlotName.StartsWith(TEXT("M2_Section_")))
		{
			continue;
		}

		int32 SourceSectionIndex = INDEX_NONE;
		LexFromString(SourceSectionIndex, *SlotName.RightChop(11));
		if (SourceSectionIndex == SectionIndex)
		{
			return SlotIndex;
		}
	}

	return SectionIndex < SkeletalMeshComponent->GetNumMaterials()
		? SectionIndex
		: INDEX_NONE;
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

uint64 UpdateFnv1a64(uint64 Hash, const FString& Value)
{
	FTCHARToUTF8 Utf8(*Value);
	for (int32 Index = 0; Index < Utf8.Length(); ++Index)
	{
		Hash ^= static_cast<uint8>(Utf8.Get()[Index]);
		Hash *= 0x00000100000001B3ull;
	}
	return Hash;
}

FString BuildAppearanceHash(const FWoWRuntimeCharacterAppearanceDescriptor& Appearance)
{
	const FString Key = FString::Printf(
		TEXT("race=%d|gender=%d|skin=%d|face=%d|hairStyle=%d|hairColor=%d|facial=%d|equipment=%s"),
		Appearance.Race,
		Appearance.Gender,
		Appearance.Skin,
		Appearance.Face,
		Appearance.HairStyle,
		Appearance.HairColor,
		Appearance.FacialStyle,
		*Appearance.EquipmentSummary);
	const uint64 Hash = UpdateFnv1a64(0xCBF29CE484222325ull, Key);
	return FString::Printf(TEXT("%016llx"), static_cast<unsigned long long>(Hash));
}

bool ReadPngFileBgra(const FString& PreviewPng, TArray<uint8>& OutBgra, int32& OutWidth, int32& OutHeight);
void AlphaBlendScaledBgraLayer(
	TArray<uint8>& DestBgra,
	int32 DestWidth,
	int32 DestHeight,
	const TArray<uint8>& SrcBgra,
	int32 SrcWidth,
	int32 SrcHeight,
	int32 DestX,
	int32 DestY,
	int32 DestRegionWidth,
	int32 DestRegionHeight);

bool ReadLittleEndianUInt32(const TArray<uint8>& Data, int32 Offset, uint32& OutValue)
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

FString ReadDbcString(const TArray<uint8>& Data, int32 StringBlockStart, uint32 RelativeOffset)
{
	if (RelativeOffset == 0)
	{
		return FString();
	}

	const int32 Start = StringBlockStart + static_cast<int32>(RelativeOffset);
	if (Start < 0 || Start >= Data.Num())
	{
		return FString();
	}

	int32 End = Start;
	while (End < Data.Num() && Data[End] != 0)
	{
		++End;
	}
	if (End <= Start)
	{
		return FString();
	}

	const std::string Utf8String(reinterpret_cast<const char*>(Data.GetData() + Start), End - Start);
	return FString(UTF8_TO_TCHAR(Utf8String.c_str()));
}

bool ReadWdbcHeader(const FString& Path, TArray<uint8>& OutData, uint32& OutRecordCount, uint32& OutFieldCount, uint32& OutRecordSize, int32& OutStringBlockStart)
{
	OutData.Reset();
	if (!FFileHelper::LoadFileToArray(OutData, *Path) || OutData.Num() < 20)
	{
		return false;
	}
	if (OutData[0] != 'W' || OutData[1] != 'D' || OutData[2] != 'B' || OutData[3] != 'C')
	{
		return false;
	}

	uint32 StringBlockSize = 0;
	if (!ReadLittleEndianUInt32(OutData, 4, OutRecordCount) ||
		!ReadLittleEndianUInt32(OutData, 8, OutFieldCount) ||
		!ReadLittleEndianUInt32(OutData, 12, OutRecordSize) ||
		!ReadLittleEndianUInt32(OutData, 16, StringBlockSize))
	{
		return false;
	}

	OutStringBlockStart = 20 + static_cast<int32>(OutRecordCount * OutRecordSize);
	return OutRecordCount > 0 &&
		OutFieldCount > 0 &&
		OutRecordSize > 0 &&
		OutStringBlockStart >= 20 &&
		OutStringBlockStart <= OutData.Num() &&
		OutStringBlockStart + static_cast<int32>(StringBlockSize) <= OutData.Num();
}

uint64 BuildRuntimeCharSectionGroupHash(int32 Race, int32 Gender, int32 SectionType, int32 Section)
{
	uint64 Hash = 1469598103934665603ull;
	auto MixInt = [&Hash](int32 Value)
	{
		Hash ^= static_cast<uint32>(Value);
		Hash *= 1099511628211ull;
	};
	MixInt(Race);
	MixInt(Gender);
	MixInt(SectionType);
	MixInt(Section);
	return Hash;
}

FString SanitizeTexturePathSegment(const FString& RawSegment)
{
	FString Result;
	Result.Reserve(RawSegment.Len());
	for (const TCHAR Ch : RawSegment)
	{
		Result.AppendChar(FChar::IsAlnum(Ch) || Ch == TEXT('_') ? Ch : TEXT('_'));
	}
	Result.TrimStartAndEndInline();
	while (Result.StartsWith(TEXT("_")))
	{
		Result.RightChopInline(1);
	}
	while (Result.EndsWith(TEXT("_")))
	{
		Result.LeftChopInline(1);
	}
	return Result.IsEmpty() ? TEXT("Texture") : Result;
}

FString NormalizeWowTexturePath(FString Value)
{
	Value.ReplaceInline(TEXT("/"), TEXT("\\"));
	Value.TrimStartAndEndInline();
	while (Value.StartsWith(TEXT("\\")))
	{
		Value.RightChopInline(1);
	}
	while (Value.Contains(TEXT("\\\\")))
	{
		Value.ReplaceInline(TEXT("\\\\"), TEXT("\\"));
	}
	if (!Value.IsEmpty() && !Value.EndsWith(TEXT(".blp"), ESearchCase::IgnoreCase))
	{
		Value += TEXT(".blp");
	}
	return Value;
}

FString BuildConvertedTexturePngPath(const FString& TextureRootRelativeToSavedTextures, const FString& WowPath)
{
	FString Normalized = NormalizeWowTexturePath(WowPath);
	if (Normalized.IsEmpty())
	{
		return FString();
	}

	TArray<FString> Parts;
	Normalized.ParseIntoArray(Parts, TEXT("\\"), true);
	if (Parts.IsEmpty())
	{
		return FString();
	}

	FString OutputPath = FPaths::ProjectSavedDir() / TEXT("WoWTextures") / TextureRootRelativeToSavedTextures;
	for (int32 Index = 0; Index + 1 < Parts.Num(); ++Index)
	{
		OutputPath /= SanitizeTexturePathSegment(Parts[Index]);
	}
	OutputPath /= SanitizeTexturePathSegment(FPaths::GetBaseFilename(Parts.Last())) + TEXT(".png");
	return OutputPath;
}

FString BuildGeneratedRuntimeTexturePngPath(const FString& WowPath)
{
	FString Normalized = NormalizeWowTexturePath(WowPath);
	if (Normalized.IsEmpty())
	{
		return FString();
	}

	TArray<FString> Parts;
	Normalized.ParseIntoArray(Parts, TEXT("\\"), true);
	if (Parts.IsEmpty())
	{
		return FString();
	}

	FString OutputPath = FPaths::ProjectContentDir() / TEXT("WoW/Generated/RuntimeData/Textures");
	for (int32 Index = 0; Index + 1 < Parts.Num(); ++Index)
	{
		OutputPath /= SanitizeTexturePathSegment(Parts[Index]);
	}
	OutputPath /= SanitizeTexturePathSegment(FPaths::GetBaseFilename(Parts.Last())) + TEXT(".png");
	return OutputPath;
}

bool FindFirstExistingPng(const TArray<FString>& Candidates, FString& OutPreviewPng)
{
	for (const FString& Candidate : Candidates)
	{
		if (!Candidate.IsEmpty() && FPaths::FileExists(Candidate))
		{
			OutPreviewPng = Candidate;
			return true;
		}
	}
	return false;
}

FString GetCharSectionTextureField(const FRuntimeCharSectionRecord& Record, const TCHAR* Field)
{
	const FString FieldName(Field);
	if (FieldName.Equals(TEXT("tex2"), ESearchCase::IgnoreCase))
	{
		return Record.Tex2;
	}
	if (FieldName.Equals(TEXT("tex3"), ESearchCase::IgnoreCase))
	{
		return Record.Tex3;
	}
	return Record.Tex1;
}

int32 ResolveRuntimeHdHairColorIndex(
	const FRuntimeAppearanceDbcCache& Cache,
	int32 Race,
	int32 Gender,
	int32 HairStyle,
	int32 HairColor)
{
	TSet<int32> NonBaldStyles;
	for (const FRuntimeCharHairGeosetRecord& Record : Cache.CharHairGeosets)
	{
		if (Record.Race == Race &&
			Record.Gender == Gender &&
			!Record.bBald &&
			Record.GeosetId != 0)
		{
			NonBaldStyles.Add(Record.HairStyle);
		}
	}
	if (NonBaldStyles.IsEmpty() || !NonBaldStyles.Contains(HairStyle))
	{
		return INDEX_NONE;
	}

	TArray<FRuntimeCharSectionRecord> SectionZeroRows;
	Cache.CharSectionsByGroup.MultiFind(BuildRuntimeCharSectionGroupHash(Race, Gender, 3, 0), SectionZeroRows);
	int32 TexturedRowCount = 0;
	for (const FRuntimeCharSectionRecord& Record : SectionZeroRows)
	{
		if (!Record.Tex1.IsEmpty())
		{
			++TexturedRowCount;
		}
	}
	if (TexturedRowCount <= 0 || TexturedRowCount % NonBaldStyles.Num() != 0)
	{
		return INDEX_NONE;
	}

	const int32 ColorCount = TexturedRowCount / NonBaldStyles.Num();
	if (ColorCount <= 1 || HairColor < 0 || HairColor >= ColorCount)
	{
		return INDEX_NONE;
	}
	return HairStyle * ColorCount + HairColor;
}

const FRuntimeCharSectionRecord* FindRuntimeCharSectionRecord(
	const FRuntimeAppearanceDbcCache& Cache,
	int32 Race,
	int32 Gender,
	int32 SectionType,
	int32 Section,
	int32 Color,
	int32& OutResolvedColor)
{
	if (!Cache.bLoaded)
	{
		return nullptr;
	}
	OutResolvedColor = Color;

	if (SectionType == 3)
	{
		const int32 HdColor = ResolveRuntimeHdHairColorIndex(Cache, Race, Gender, Section, Color);
		if (HdColor != INDEX_NONE && HdColor != Color)
		{
			const FRuntimeCharSectionKey HdKey{ Race, Gender, SectionType, Section, HdColor };
			if (const FRuntimeCharSectionRecord* HdRecord = Cache.CharSectionsByKey.Find(HdKey))
			{
				OutResolvedColor = HdColor;
				return HdRecord;
			}
		}
	}

	const FRuntimeCharSectionKey ExactKey{ Race, Gender, SectionType, Section, Color };
	return Cache.CharSectionsByKey.Find(ExactKey);
}

bool FindRuntimeCharSectionTexturePath(
	const FRuntimeAppearanceDbcCache& Cache,
	int32 Race,
	int32 Gender,
	int32 SectionType,
	int32 Section,
	int32 Color,
	const TCHAR* Field,
	FString& OutPreviewPng)
{
	OutPreviewPng.Reset();
	int32 ResolvedColor = Color;
	if (const FRuntimeCharSectionRecord* ExactRecord = FindRuntimeCharSectionRecord(Cache, Race, Gender, SectionType, Section, Color, ResolvedColor))
	{
		const FString TexturePath = GetCharSectionTextureField(*ExactRecord, Field);
		if (!TexturePath.IsEmpty())
		{
			OutPreviewPng = BuildConvertedTexturePngPath(TEXT("CharacterAppearance"), TexturePath);
			return !OutPreviewPng.IsEmpty();
		}
	}

	return false;
}

bool LogMissingCharacterAppearanceTexture(
	const FRuntimeAppearanceDbcCache& Cache,
	const FWoWRuntimeCharacterAppearanceDescriptor& Appearance,
	int32 SectionType,
	int32 Section,
	int32 Color,
	const TCHAR* Field,
	const TCHAR* Usage)
{
	int32 ResolvedColor = Color;
	const FRuntimeCharSectionRecord* Record = FindRuntimeCharSectionRecord(Cache, Appearance.Race, Appearance.Gender, SectionType, Section, Color, ResolvedColor);
	if (!Record)
	{
		UE_LOG(LogTemp, Warning, TEXT("角色 CharSections DBC 记录缺失，无法随意使用相近颜色贴图: guid=%d usage=%s race=%d gender=%d sectionType=%d section=%d color=%d field=%s"),
			Appearance.Guid,
			Usage,
			Appearance.Race,
			Appearance.Gender,
			SectionType,
			Section,
			Color,
			Field);
		return true;
	}

	const FString WowTexturePath = GetCharSectionTextureField(*Record, Field);
	if (WowTexturePath.IsEmpty())
	{
		UE_LOG(LogTemp, Verbose, TEXT("角色 CharSections DBC 字段为空，跳过该层贴图: guid=%d usage=%s race=%d gender=%d sectionType=%d section=%d color=%d resolvedColor=%d field=%s"),
			Appearance.Guid,
			Usage,
			Appearance.Race,
			Appearance.Gender,
			SectionType,
			Section,
			Color,
			ResolvedColor,
			Field);
		return false;
	}

	const FString PreviewPng = BuildConvertedTexturePngPath(TEXT("CharacterAppearance"), WowTexturePath);
	UE_LOG(LogTemp, Warning, TEXT("角色 CharSections 贴图 PNG 缺失或不可读，请补充素材: guid=%d usage=%s race=%d gender=%d sectionType=%d section=%d color=%d resolvedColor=%d field=%s dbcTexture=%s wanted=%s"),
		Appearance.Guid,
		Usage,
		Appearance.Race,
		Appearance.Gender,
		SectionType,
		Section,
		Color,
		ResolvedColor,
		Field,
		*WowTexturePath,
		*PreviewPng);
	return true;
}

TArray<FString> BuildItemTextureCandidateWowPaths(const FString& ComponentDir, const FString& ComponentName, std::initializer_list<const TCHAR*> Suffixes)
{
	TArray<FString> Paths;
	if (ComponentDir.IsEmpty() || ComponentName.IsEmpty())
	{
		return Paths;
	}

	for (const TCHAR* Suffix : Suffixes)
	{
		if (!Suffix || FCString::Strlen(Suffix) == 0)
		{
			continue;
		}
		Paths.Add(FString::Printf(TEXT("Item\\TextureComponents\\%s\\%s_%s.blp"), *ComponentDir, *ComponentName, Suffix));
	}
	Paths.Add(FString::Printf(TEXT("Item\\TextureComponents\\%s\\%s.blp"), *ComponentDir, *ComponentName));
	return Paths;
}

TArray<FString> BuildItemTextureCandidatePngPaths(const FString& ComponentDir, const FString& ComponentName, std::initializer_list<const TCHAR*> Suffixes)
{
	TArray<FString> Paths;
	for (const FString& WowPath : BuildItemTextureCandidateWowPaths(ComponentDir, ComponentName, Suffixes))
	{
		Paths.Add(BuildConvertedTexturePngPath(TEXT("ItemTextureComponents"), WowPath));
	}
	return Paths;
}

FString JoinTextureCandidatesForLog(const TArray<FString>& Candidates)
{
	FString Joined;
	for (const FString& Candidate : Candidates)
	{
		if (!Joined.IsEmpty())
		{
			Joined += TEXT(" | ");
		}
		Joined += Candidate;
	}
	return Joined;
}

bool IsHdIndependentFacePreviewTexture(const FString& PreviewPng)
{
	const FString BaseName = FPaths::GetBaseFilename(PreviewPng).ToLower();
	return BaseName.Contains(TEXT("faceupper")) ||
		BaseName.Contains(TEXT("facelower")) ||
		(BaseName.Contains(TEXT("_hd_")) &&
			(BaseName.Contains(TEXT("eye")) ||
			BaseName.Contains(TEXT("eyebrow"))));
}

bool IsRuntimeCharacterEyeGeosetSubmesh(int32 SubmeshId)
{
	if (SubmeshId <= 0)
	{
		return false;
	}

	const int32 Group = SubmeshId / 100;
	const int32 Value = SubmeshId - Group * 100;
	return Group == 17 && Value > 0;
}

bool TryResolveFaceSectionFieldFromPreview(const FString& PreviewPng, const TCHAR*& OutField)
{
	const FString BaseName = FPaths::GetBaseFilename(PreviewPng).ToLower();
	if (BaseName.Contains(TEXT("faceupper")))
	{
		OutField = TEXT("tex2");
		return true;
	}
	if (BaseName.Contains(TEXT("facelower")))
	{
		OutField = TEXT("tex1");
		return true;
	}
	return false;
}

FString ResolvePreviewPngPath(const FString& PreviewPng)
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

bool ReadPngFileBgra(const FString& PreviewPng, TArray<uint8>& OutBgra, int32& OutWidth, int32& OutHeight)
{
	OutBgra.Reset();
	OutWidth = 0;
	OutHeight = 0;

	const FString TexturePath = ResolvePreviewPngPath(PreviewPng);
	if (TexturePath.IsEmpty())
	{
		return false;
	}

	TArray<uint8> CompressedData;
	if (!FFileHelper::LoadFileToArray(CompressedData, *TexturePath))
	{
		return false;
	}

	IImageWrapperModule& ImageWrapperModule = FModuleManager::LoadModuleChecked<IImageWrapperModule>(TEXT("ImageWrapper"));
	TSharedPtr<IImageWrapper> ImageWrapper = ImageWrapperModule.CreateImageWrapper(EImageFormat::PNG);
	if (!ImageWrapper.IsValid() || !ImageWrapper->SetCompressed(CompressedData.GetData(), CompressedData.Num()))
	{
		return false;
	}

	TArray<uint8> RawBgra;
	if (!ImageWrapper->GetRaw(ERGBFormat::BGRA, 8, RawBgra))
	{
		return false;
	}

	OutWidth = ImageWrapper->GetWidth();
	OutHeight = ImageWrapper->GetHeight();
	OutBgra = MoveTemp(RawBgra);
	return OutWidth > 0 && OutHeight > 0 && OutBgra.Num() >= OutWidth * OutHeight * 4;
}

void AlphaBlendScaledBgraLayer(
	TArray<uint8>& DestBgra,
	int32 DestWidth,
	int32 DestHeight,
	const TArray<uint8>& SrcBgra,
	int32 SrcWidth,
	int32 SrcHeight,
	int32 DestX,
	int32 DestY,
	int32 DestRegionWidth,
	int32 DestRegionHeight)
{
	if (DestBgra.Num() < DestWidth * DestHeight * 4 ||
		SrcBgra.Num() < SrcWidth * SrcHeight * 4 ||
		SrcWidth <= 0 ||
		SrcHeight <= 0 ||
		DestRegionWidth <= 0 ||
		DestRegionHeight <= 0)
	{
		return;
	}

	for (int32 Y = 0; Y < DestRegionHeight; ++Y)
	{
		const int32 OutY = DestY + Y;
		if (OutY < 0 || OutY >= DestHeight)
		{
			continue;
		}
		const int32 SrcY = FMath::Clamp((Y * SrcHeight) / DestRegionHeight, 0, SrcHeight - 1);
		for (int32 X = 0; X < DestRegionWidth; ++X)
		{
			const int32 OutX = DestX + X;
			if (OutX < 0 || OutX >= DestWidth)
			{
				continue;
			}
			const int32 SrcX = FMath::Clamp((X * SrcWidth) / DestRegionWidth, 0, SrcWidth - 1);
			const int32 SrcIndex = (SrcY * SrcWidth + SrcX) * 4;
			const int32 DestIndex = (OutY * DestWidth + OutX) * 4;
			const uint8 SrcA = SrcBgra[SrcIndex + 3];
			if (SrcA == 0)
			{
				continue;
			}
			if (SrcA == 255)
			{
				DestBgra[DestIndex + 0] = SrcBgra[SrcIndex + 0];
				DestBgra[DestIndex + 1] = SrcBgra[SrcIndex + 1];
				DestBgra[DestIndex + 2] = SrcBgra[SrcIndex + 2];
				DestBgra[DestIndex + 3] = 255;
				continue;
			}
			const int32 InvA = 255 - SrcA;
			DestBgra[DestIndex + 0] = static_cast<uint8>((SrcBgra[SrcIndex + 0] * SrcA + DestBgra[DestIndex + 0] * InvA) / 255);
			DestBgra[DestIndex + 1] = static_cast<uint8>((SrcBgra[SrcIndex + 1] * SrcA + DestBgra[DestIndex + 1] * InvA) / 255);
			DestBgra[DestIndex + 2] = static_cast<uint8>((SrcBgra[SrcIndex + 2] * SrcA + DestBgra[DestIndex + 2] * InvA) / 255);
			DestBgra[DestIndex + 3] = 255;
		}
	}
}

FString CanonicalizeRuntimeCharSectionTextureBase(const FString& TexturePath)
{
	FString Base = FPaths::GetBaseFilename(TexturePath).ToLower();
	TArray<FString> Parts;
	Base.ParseIntoArray(Parts, TEXT("_"), true);
	for (int32 Index = Parts.Num() - 1; Index >= 0; --Index)
	{
		if (Parts[Index].Equals(TEXT("hd"), ESearchCase::IgnoreCase))
		{
			continue;
		}

		bool bNumeric = !Parts[Index].IsEmpty() && Parts[Index].Len() <= 2;
		for (const TCHAR Ch : Parts[Index])
		{
			bNumeric = bNumeric && FChar::IsDigit(Ch);
		}
		if (bNumeric)
		{
			Parts.RemoveAt(Index);
			break;
		}
	}
	return FString::Join(Parts, TEXT("_"));
}

const FRuntimeCharSectionRecord* FindCharSectionRecordMatchingExportPreview(
	const FRuntimeAppearanceDbcCache& Cache,
	int32 Race,
	int32 Gender,
	int32 SectionType,
	const FString& ExportPreviewPng,
	const TCHAR*& OutField)
{
	const FString WantedBase = CanonicalizeRuntimeCharSectionTextureBase(ExportPreviewPng);
	if (WantedBase.IsEmpty())
	{
		return nullptr;
	}

	for (const TPair<FRuntimeCharSectionKey, FRuntimeCharSectionRecord>& Pair : Cache.CharSectionsByKey)
	{
		const FRuntimeCharSectionRecord& Record = Pair.Value;
		if (Record.Race != Race || Record.Gender != Gender || Record.SectionType != SectionType)
		{
			continue;
		}

		if (!Record.Tex1.IsEmpty() && CanonicalizeRuntimeCharSectionTextureBase(Record.Tex1) == WantedBase)
		{
			OutField = TEXT("tex1");
			return &Record;
		}
		if (!Record.Tex2.IsEmpty() && CanonicalizeRuntimeCharSectionTextureBase(Record.Tex2) == WantedBase)
		{
			OutField = TEXT("tex2");
			return &Record;
		}
		if (!Record.Tex3.IsEmpty() && CanonicalizeRuntimeCharSectionTextureBase(Record.Tex3) == WantedBase)
		{
			OutField = TEXT("tex3");
			return &Record;
		}
	}
	return nullptr;
}

bool TryResolveRuntimeSubmeshGroupValue(int32 SubmeshId, int32& OutGroup, int32& OutValue)
{
	if (SubmeshId <= 0)
	{
		return false;
	}
	if (SubmeshId < 100)
	{
		OutGroup = 0;
		OutValue = SubmeshId;
		return true;
	}
	OutGroup = SubmeshId / 100;
	OutValue = SubmeshId - OutGroup * 100;
	return OutValue > 0;
}

const FRuntimeFacialHairStyleRecord* ResolveRuntimeFacialHairStyle(
	const FRuntimeAppearanceDbcCache& Cache,
	int32 Race,
	int32 Gender,
	int32 FacialStyle)
{
	for (const FRuntimeFacialHairStyleRecord& Record : Cache.FacialHairStyles)
	{
		if (Record.Race == Race && Record.Gender == Gender && Record.Style == FacialStyle)
		{
			return &Record;
		}
	}
	return nullptr;
}

void LogMissingItemTextureComponent(
	const FWoWRuntimeCharacterAppearanceDescriptor& Appearance,
	const FEquipmentDisplay& Item,
	const FString& Field,
	const FString& ComponentDir,
	const FString& ComponentName,
	const TArray<FString>& WowCandidates,
	const TArray<FString>& PngCandidates)
{
	UE_LOG(LogTemp, Warning, TEXT("装备身体 TextureComponents PNG 缺失或不可读，请补充素材: guid=%d displayId=%d slot=%d inventoryType=%d field=%s componentDir=%s componentName=%s wantedBlp=%s triedPng=%s"),
		Appearance.Guid,
		Item.DisplayId,
		Item.Slot,
		Item.InventoryType,
		*Field,
		*ComponentDir,
		*ComponentName,
		*JoinTextureCandidatesForLog(WowCandidates),
		*JoinTextureCandidatesForLog(PngCandidates));
}

bool BlendFirstAvailablePngLayer(
	TArray<uint8>& AtlasBgra,
	int32 AtlasWidth,
	int32 AtlasHeight,
	const TArray<FString>& PreviewPngCandidates,
	int32 DestX,
	int32 DestY,
	int32 DestWidth,
	int32 DestHeight,
	int32& InOutAppliedLayers,
	int32& InOutMissingLayers)
{
	for (const FString& PreviewPng : PreviewPngCandidates)
	{
		TArray<uint8> LayerBgra;
		int32 LayerWidth = 0;
		int32 LayerHeight = 0;
		if (!ReadPngFileBgra(PreviewPng, LayerBgra, LayerWidth, LayerHeight))
		{
			continue;
		}
		AlphaBlendScaledBgraLayer(AtlasBgra, AtlasWidth, AtlasHeight, LayerBgra, LayerWidth, LayerHeight, DestX, DestY, DestWidth, DestHeight);
		++InOutAppliedLayers;
		return true;
	}

	++InOutMissingLayers;
	return false;
}

bool LoadRuntimeAppearanceDbcCache(FRuntimeAppearanceDbcCache& OutCache)
{
	OutCache = FRuntimeAppearanceDbcCache();
	OutCache.bAttemptedLoad = true;

	const FString DbcRoot = FPaths::ProjectDir() / TEXT("Data/DBFilesClient");
	{
		const FString Path = DbcRoot / TEXT("CharSections.dbc");
		TArray<uint8> Data;
		uint32 RecordCount = 0;
		uint32 FieldCount = 0;
		uint32 RecordSize = 0;
		int32 StringBlockStart = 0;
		if (!ReadWdbcHeader(Path, Data, RecordCount, FieldCount, RecordSize, StringBlockStart) || FieldCount != 10 || RecordSize != 40)
		{
			UE_LOG(LogTemp, Warning, TEXT("角色运行态 DBC 读取失败: %s"), *Path);
			return false;
		}

		for (uint32 RecordIndex = 0; RecordIndex < RecordCount; ++RecordIndex)
		{
			const int32 Offset = 20 + static_cast<int32>(RecordIndex * RecordSize);
			uint32 Values[10] = {};
			for (int32 FieldIndex = 0; FieldIndex < 10; ++FieldIndex)
			{
				ReadLittleEndianUInt32(Data, Offset + FieldIndex * 4, Values[FieldIndex]);
			}

			FRuntimeCharSectionRecord Record;
			Record.Race = static_cast<int32>(Values[1]);
			Record.Gender = static_cast<int32>(Values[2]);
			Record.SectionType = static_cast<int32>(Values[3]);
			Record.Tex1 = ReadDbcString(Data, StringBlockStart, Values[4]);
			Record.Tex2 = ReadDbcString(Data, StringBlockStart, Values[5]);
			Record.Tex3 = ReadDbcString(Data, StringBlockStart, Values[6]);
			Record.Section = static_cast<int32>(Values[8]);
			Record.Color = static_cast<int32>(Values[9]);

			const FRuntimeCharSectionKey Key{ Record.Race, Record.Gender, Record.SectionType, Record.Section, Record.Color };
			OutCache.CharSectionsByKey.Add(Key, Record);
			OutCache.CharSectionsByGroup.Add(BuildRuntimeCharSectionGroupHash(Record.Race, Record.Gender, Record.SectionType, Record.Section), MoveTemp(Record));
		}
	}

	{
		const FString Path = DbcRoot / TEXT("ItemDisplayInfo.dbc");
		TArray<uint8> Data;
		uint32 RecordCount = 0;
		uint32 FieldCount = 0;
		uint32 RecordSize = 0;
		int32 StringBlockStart = 0;
		if (!ReadWdbcHeader(Path, Data, RecordCount, FieldCount, RecordSize, StringBlockStart) || FieldCount != 25 || RecordSize != 100)
		{
			UE_LOG(LogTemp, Warning, TEXT("装备贴图 DBC 读取失败: %s"), *Path);
			return false;
		}

		for (uint32 RecordIndex = 0; RecordIndex < RecordCount; ++RecordIndex)
		{
			const int32 Offset = 20 + static_cast<int32>(RecordIndex * RecordSize);
			uint32 DisplayId = 0;
			ReadLittleEndianUInt32(Data, Offset, DisplayId);
			if (DisplayId == 0)
			{
				continue;
			}

			FRuntimeItemDisplayTextureRecord Record;
			Record.DisplayId = static_cast<int32>(DisplayId);
			uint32 StringOffset = 0;
			ReadLittleEndianUInt32(Data, Offset + 4, StringOffset); Record.Model = ReadDbcString(Data, StringBlockStart, StringOffset);
			ReadLittleEndianUInt32(Data, Offset + 8, StringOffset); Record.Model2 = ReadDbcString(Data, StringBlockStart, StringOffset);
			ReadLittleEndianUInt32(Data, Offset + 12, StringOffset); Record.Skin = ReadDbcString(Data, StringBlockStart, StringOffset);
			ReadLittleEndianUInt32(Data, Offset + 16, StringOffset); Record.Skin2 = ReadDbcString(Data, StringBlockStart, StringOffset);
			ReadLittleEndianUInt32(Data, Offset + 60, StringOffset); Record.ArmUpper = ReadDbcString(Data, StringBlockStart, StringOffset);
			ReadLittleEndianUInt32(Data, Offset + 64, StringOffset); Record.ArmLower = ReadDbcString(Data, StringBlockStart, StringOffset);
			ReadLittleEndianUInt32(Data, Offset + 68, StringOffset); Record.Hand = ReadDbcString(Data, StringBlockStart, StringOffset);
			ReadLittleEndianUInt32(Data, Offset + 72, StringOffset); Record.TorsoUpper = ReadDbcString(Data, StringBlockStart, StringOffset);
			ReadLittleEndianUInt32(Data, Offset + 76, StringOffset); Record.TorsoLower = ReadDbcString(Data, StringBlockStart, StringOffset);
			ReadLittleEndianUInt32(Data, Offset + 80, StringOffset); Record.LegUpper = ReadDbcString(Data, StringBlockStart, StringOffset);
			ReadLittleEndianUInt32(Data, Offset + 84, StringOffset); Record.LegLower = ReadDbcString(Data, StringBlockStart, StringOffset);
			ReadLittleEndianUInt32(Data, Offset + 88, StringOffset); Record.Foot = ReadDbcString(Data, StringBlockStart, StringOffset);
			OutCache.ItemDisplayTexturesById.Add(Record.DisplayId, MoveTemp(Record));
		}
	}

	{
		const FString Path = DbcRoot / TEXT("CharHairGeosets.dbc");
		TArray<uint8> Data;
		uint32 RecordCount = 0;
		uint32 FieldCount = 0;
		uint32 RecordSize = 0;
		int32 StringBlockStart = 0;
		if (!ReadWdbcHeader(Path, Data, RecordCount, FieldCount, RecordSize, StringBlockStart) || FieldCount != 6 || RecordSize != 24)
		{
			UE_LOG(LogTemp, Warning, TEXT("头发 geoset DBC 读取失败: %s"), *Path);
			return false;
		}

		for (uint32 RecordIndex = 0; RecordIndex < RecordCount; ++RecordIndex)
		{
			const int32 Offset = 20 + static_cast<int32>(RecordIndex * RecordSize);
			uint32 Value = 0;
			FRuntimeCharHairGeosetRecord Record;
			ReadLittleEndianUInt32(Data, Offset + 4, Value); Record.Race = static_cast<int32>(Value);
			ReadLittleEndianUInt32(Data, Offset + 8, Value); Record.Gender = static_cast<int32>(Value);
			ReadLittleEndianUInt32(Data, Offset + 12, Value); Record.HairStyle = static_cast<int32>(Value);
			ReadLittleEndianUInt32(Data, Offset + 16, Value); Record.GeosetId = static_cast<int32>(Value);
			ReadLittleEndianUInt32(Data, Offset + 20, Value); Record.bBald = Value != 0;
			OutCache.CharHairGeosets.Add(Record);
		}
	}

	{
		const FString Path = DbcRoot / TEXT("CharacterFacialHairStyles.dbc");
		TArray<uint8> Data;
		uint32 RecordCount = 0;
		uint32 FieldCount = 0;
		uint32 RecordSize = 0;
		int32 StringBlockStart = 0;
		if (!ReadWdbcHeader(Path, Data, RecordCount, FieldCount, RecordSize, StringBlockStart) || FieldCount != 8 || RecordSize != 32)
		{
			UE_LOG(LogTemp, Warning, TEXT("面部毛发 geoset DBC 读取失败: %s"), *Path);
			return false;
		}

		for (uint32 RecordIndex = 0; RecordIndex < RecordCount; ++RecordIndex)
		{
			const int32 Offset = 20 + static_cast<int32>(RecordIndex * RecordSize);
			uint32 Values[8] = {};
			for (int32 FieldIndex = 0; FieldIndex < 8; ++FieldIndex)
			{
				ReadLittleEndianUInt32(Data, Offset + FieldIndex * 4, Values[FieldIndex]);
			}

			FRuntimeFacialHairStyleRecord Record;
			Record.Race = static_cast<int32>(Values[0]);
			Record.Gender = static_cast<int32>(Values[1]);
			Record.Style = static_cast<int32>(Values[2]);
			Record.Geoset100 = static_cast<int32>(Values[3]);
			Record.Geoset200 = static_cast<int32>(Values[5]);
			Record.Geoset300 = static_cast<int32>(Values[4]);
			OutCache.FacialHairStyles.Add(Record);
		}
	}

	OutCache.bLoaded = true;
	UE_LOG(LogTemp, Display, TEXT("角色运行态 DBC 已加载到内存: charSections=%d itemDisplayTextures=%d hairGeosets=%d facialHair=%d"),
		OutCache.CharSectionsByKey.Num(),
		OutCache.ItemDisplayTexturesById.Num(),
		OutCache.CharHairGeosets.Num(),
		OutCache.FacialHairStyles.Num());
	return true;
}

const FRuntimeAppearanceDbcCache& GetRuntimeAppearanceDbcCache()
{
	FRuntimeAppearanceDbcCache& Cache = GetRuntimeAppearanceDbcCacheStorage();
	if (!Cache.bAttemptedLoad)
	{
		LoadRuntimeAppearanceDbcCache(Cache);
	}
	return Cache;
}

TArray<FEquipmentDisplay> ParseEquipmentSummary(const FString& EquipmentSummary)
{
	return WoWCharacterEquipment::ParseEquipmentSummary(EquipmentSummary);
}

bool ShouldApplyItemTextureFieldToEquipment(const FString& Field, const FEquipmentDisplay& Item, TOptional<int32> RobeDisplayId)
{
	if (Field == TEXT("arm_upper") || Field == TEXT("arm_lower"))
	{
		return Item.Slot == 3 || Item.Slot == 4 || Item.Slot == 8 || Item.Slot == 9 ||
			Item.InventoryType == 4 || Item.InventoryType == 5 || Item.InventoryType == 9 || Item.InventoryType == 10 || Item.InventoryType == 20;
	}
	if (Field == TEXT("hand"))
	{
		return Item.Slot == 9 || Item.InventoryType == 10;
	}
	if (Field == TEXT("torso_upper") || Field == TEXT("torso_lower"))
	{
		return Item.Slot == 3 || Item.Slot == 4 || Item.InventoryType == 4 || Item.InventoryType == 5 || Item.InventoryType == 20;
	}
	if (Field == TEXT("leg_upper"))
	{
		if (RobeDisplayId.IsSet() && Item.DisplayId != RobeDisplayId.GetValue() && Item.InventoryType != 6)
		{
			return false;
		}
		return Item.Slot == 4 || Item.Slot == 5 || Item.Slot == 6 || Item.InventoryType == 6 || Item.InventoryType == 7 || Item.InventoryType == 20;
	}
	if (Field == TEXT("leg_lower"))
	{
		if (RobeDisplayId.IsSet() && Item.DisplayId != RobeDisplayId.GetValue() && Item.InventoryType != 6 && Item.InventoryType != 8)
		{
			return false;
		}
		return Item.Slot == 4 || Item.Slot == 5 || Item.Slot == 6 || Item.Slot == 7 ||
			Item.InventoryType == 6 || Item.InventoryType == 7 || Item.InventoryType == 8 || Item.InventoryType == 20;
	}
	if (Field == TEXT("foot"))
	{
		return Item.Slot == 7 || Item.InventoryType == 8;
	}
	return false;
}

bool GetCharacterAtlasRegionForItemField(
	const FString& Field,
	int32& OutX,
	int32& OutY,
	int32& OutWidth,
	int32& OutHeight)
{
	if (Field == TEXT("arm_upper"))
	{
		OutX = 0; OutY = 0; OutWidth = 256; OutHeight = 128; return true;
	}
	if (Field == TEXT("arm_lower"))
	{
		OutX = 0; OutY = 128; OutWidth = 256; OutHeight = 128; return true;
	}
	if (Field == TEXT("hand"))
	{
		OutX = 0; OutY = 256; OutWidth = 256; OutHeight = 64; return true;
	}
	if (Field == TEXT("torso_upper"))
	{
		OutX = 256; OutY = 0; OutWidth = 256; OutHeight = 128; return true;
	}
	if (Field == TEXT("torso_lower"))
	{
		OutX = 256; OutY = 128; OutWidth = 256; OutHeight = 64; return true;
	}
	if (Field == TEXT("leg_upper"))
	{
		OutX = 256; OutY = 192; OutWidth = 256; OutHeight = 128; return true;
	}
	if (Field == TEXT("leg_lower"))
	{
		OutX = 256; OutY = 320; OutWidth = 256; OutHeight = 128; return true;
	}
	if (Field == TEXT("foot"))
	{
		OutX = 256; OutY = 448; OutWidth = 256; OutHeight = 64; return true;
	}
	return false;
}

UTexture2D* CreateTransientBgraTexture(const FString& Name, const TArray<uint8>& Bgra, int32 Width, int32 Height)
{
	if (Bgra.Num() < Width * Height * 4 || Width <= 0 || Height <= 0)
	{
		return nullptr;
	}

	UTexture2D* Texture = UTexture2D::CreateTransient(Width, Height, PF_B8G8R8A8, *Name);
	if (!Texture || !Texture->GetPlatformData() || Texture->GetPlatformData()->Mips.Num() == 0)
	{
		return nullptr;
	}
	Texture->MipGenSettings = TMGS_NoMipmaps;
	Texture->NeverStream = true;
	void* TextureData = Texture->GetPlatformData()->Mips[0].BulkData.Lock(LOCK_READ_WRITE);
	FMemory::Memcpy(TextureData, Bgra.GetData(), Bgra.Num());
	Texture->GetPlatformData()->Mips[0].BulkData.Unlock();
	Texture->UpdateResource();
	return Texture;
}

}

bool WoWRuntimeCharacterAppearance::WarmupRuntimeDbc()
{
	return GetRuntimeAppearanceDbcCache().bLoaded;
}

bool WoWRuntimeCharacterAppearance::ResolveItemDisplayObjectSkinPng(int32 DisplayId, const FString& AttachmentMeshPath, FString& OutPreviewPng)
{
	OutPreviewPng.Reset();
	if (DisplayId <= 0 || AttachmentMeshPath.IsEmpty())
	{
		return false;
	}

	const FRuntimeAppearanceDbcCache& DbcCache = GetRuntimeAppearanceDbcCache();
	const FRuntimeItemDisplayTextureRecord* Record = DbcCache.ItemDisplayTexturesById.Find(DisplayId);
	if (!Record)
	{
		UE_LOG(LogTemp, Warning, TEXT("装备 ObjectComponent DBC 记录缺失: displayId=%d mesh=%s table=ItemDisplayInfo.dbc"),
			DisplayId,
			*AttachmentMeshPath);
		return false;
	}

	const FString MeshBaseName = FPaths::GetBaseFilename(AttachmentMeshPath)
		.Replace(TEXT("_M2Mesh"), TEXT(""), ESearchCase::IgnoreCase);
	const FString ModelBaseName = FPaths::GetBaseFilename(Record->Model);
	const FString Model2BaseName = FPaths::GetBaseFilename(Record->Model2);
	FString SkinName;
	if (!Model2BaseName.IsEmpty() && MeshBaseName.Contains(Model2BaseName, ESearchCase::IgnoreCase))
	{
		SkinName = Record->Skin2;
	}
	else if (!ModelBaseName.IsEmpty() && MeshBaseName.Contains(ModelBaseName, ESearchCase::IgnoreCase))
	{
		SkinName = Record->Skin;
	}
	else if (MeshBaseName.StartsWith(TEXT("RShoulder_"), ESearchCase::IgnoreCase) && !Record->Skin2.IsEmpty())
	{
		SkinName = Record->Skin2;
	}
	else
	{
		SkinName = Record->Skin;
	}

	if (SkinName.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("装备 ObjectComponent DBC skin 字段为空: displayId=%d mesh=%s model=%s model2=%s skin=%s skin2=%s table=ItemDisplayInfo.dbc fields=ModelTexture_1/ModelTexture_2"),
			DisplayId,
			*AttachmentMeshPath,
			*Record->Model,
			*Record->Model2,
			*Record->Skin,
			*Record->Skin2);
		return false;
	}

	const FString ComponentDir = MeshBaseName.Contains(TEXT("Shoulder"), ESearchCase::IgnoreCase) ? TEXT("Shoulder") :
		(MeshBaseName.Contains(TEXT("Helm"), ESearchCase::IgnoreCase) || MeshBaseName.Contains(TEXT("Head"), ESearchCase::IgnoreCase) ? TEXT("Head") : FString());
	if (ComponentDir.IsEmpty())
	{
		UE_LOG(LogTemp, Warning, TEXT("装备 ObjectComponent 类型无法从 mesh 推断，跳过 skin 且不使用错误兜底: displayId=%d mesh=%s model=%s model2=%s skin=%s skin2=%s"),
			DisplayId,
			*AttachmentMeshPath,
			*Record->Model,
			*Record->Model2,
			*Record->Skin,
			*Record->Skin2);
		return false;
	}

	const FString WowTexturePath = FString::Printf(TEXT("Item\\ObjectComponents\\%s\\%s.blp"), *ComponentDir, *SkinName);
	const TArray<FString> Candidates =
	{
		BuildGeneratedRuntimeTexturePngPath(WowTexturePath),
		BuildConvertedTexturePngPath(TEXT("ItemObjectComponents"), WowTexturePath),
		BuildConvertedTexturePngPath(TEXT(""), WowTexturePath)
	};

	if (FindFirstExistingPng(Candidates, OutPreviewPng))
	{
		return true;
	}

	UE_LOG(LogTemp, Warning, TEXT("装备 ObjectComponent DBC skin PNG 缺失，请补充素材: displayId=%d mesh=%s model=%s model2=%s skin=%s skin2=%s wanted=%s tried=%s | %s | %s"),
		DisplayId,
		*AttachmentMeshPath,
		*Record->Model,
		*Record->Model2,
		*Record->Skin,
		*Record->Skin2,
		*WowTexturePath,
		*Candidates[0],
		*Candidates[1],
		*Candidates[2]);
	return false;
}

bool WoWRuntimeCharacterAppearance::ResolveRuntimeSectionTexturePng(
	const FWoWRuntimeCharacterAppearanceDescriptor& Appearance,
	int32 TextureType,
	int32 SubmeshId,
	const FString& ExportPreviewPng,
	FString& OutPreviewPng)
{
	OutPreviewPng.Reset();
	const FRuntimeAppearanceDbcCache& DbcCache = GetRuntimeAppearanceDbcCache();
	if (!DbcCache.bLoaded)
	{
		return false;
	}

	const FString ResolvedExportPreviewPng = ResolvePreviewPngPath(ExportPreviewPng);
	if (IsRuntimeCharacterEyeGeosetSubmesh(SubmeshId) &&
		!ResolvedExportPreviewPng.IsEmpty() &&
		FPaths::FileExists(ResolvedExportPreviewPng))
	{
		OutPreviewPng = ResolvedExportPreviewPng;
		UE_LOG(LogTemp, Verbose, TEXT("角色 eye geoset 保留 M2 导出贴图: guid=%d submesh=%d textureType=%d texture=%s"),
			Appearance.Guid,
			SubmeshId,
			TextureType,
			*ResolvedExportPreviewPng);
		return true;
	}

	int32 SectionType = INDEX_NONE;
	int32 CharSection = 0;
	int32 Color = 0;
	const TCHAR* Field = TEXT("tex1");
	if (TextureType == 1)
	{
		SectionType = 0;
		CharSection = 0;
		Color = Appearance.Skin;
		Field = TEXT("tex1");
	}
	else if (TextureType == 2)
	{
		const FString ResolvedPreviewPng = ResolvePreviewPngPath(ExportPreviewPng);
		const FString FacePreviewProbe = !ResolvedPreviewPng.IsEmpty() ? ResolvedPreviewPng : ExportPreviewPng;
		if (TryResolveFaceSectionFieldFromPreview(FacePreviewProbe, Field))
		{
			SectionType = 1;
			CharSection = Appearance.Face;
			Color = Appearance.Skin;
		}
		else if (IsHdIndependentFacePreviewTexture(FacePreviewProbe) &&
			FPaths::FileExists(FacePreviewProbe))
		{
			OutPreviewPng = FacePreviewProbe;
			return true;
		}
		else
		{
			SectionType = 0;
			CharSection = 0;
			Color = Appearance.Skin;
			Field = TEXT("tex1");
		}
	}
	else if (TextureType == 6)
	{
		SectionType = 3;
		CharSection = Appearance.HairStyle;
		Color = Appearance.HairColor;
		Field = TEXT("tex1");
	}
	else if (TextureType == 8)
	{
		SectionType = 0;
		CharSection = 0;
		Color = Appearance.Skin;
		Field = TEXT("tex2");
	}
	else
	{
		OutPreviewPng = ExportPreviewPng;
		return !OutPreviewPng.IsEmpty();
	}

	if (!FindRuntimeCharSectionTexturePath(DbcCache, Appearance.Race, Appearance.Gender, SectionType, CharSection, Color, Field, OutPreviewPng) ||
		OutPreviewPng.IsEmpty())
	{
		LogMissingCharacterAppearanceTexture(DbcCache, Appearance, SectionType, CharSection, Color, Field, TEXT("material_section_resolve"));
		return false;
	}
	if (TextureType == 2)
	{
		if (!FPaths::FileExists(OutPreviewPng))
		{
			LogMissingCharacterAppearanceTexture(DbcCache, Appearance, SectionType, CharSection, Color, Field, TEXT("material_section_resolve_file_missing"));
			return false;
		}
	}

	return true;
}

bool WoWRuntimeCharacterAppearance::ShouldUseComposedBodyAtlasForSection(int32 TextureType, int32 SubmeshId, const FString& ExportPreviewPng)
{
	const FString ResolvedPreviewPng = ResolvePreviewPngPath(ExportPreviewPng);
	const FString PreviewProbe = !ResolvedPreviewPng.IsEmpty() ? ResolvedPreviewPng : ExportPreviewPng;
	if (IsRuntimeCharacterEyeGeosetSubmesh(SubmeshId))
	{
		return false;
	}
	if (TextureType == 1)
	{
		return true;
	}
	if (TextureType == 2)
	{
		return !IsHdIndependentFacePreviewTexture(PreviewProbe);
	}
	return false;
}

FString WoWRuntimeCharacterAppearance::BuildBodyAtlasSignature(const FWoWRuntimeCharacterAppearanceDescriptor& Appearance)
{
	return FString::Printf(
		TEXT("race=%d|gender=%d|skin=%d|face=%d|hair=%d|hairColor=%d|facial=%d|equipment=%s"),
		Appearance.Race,
		Appearance.Gender,
		Appearance.Skin,
		Appearance.Face,
		Appearance.HairStyle,
		Appearance.HairColor,
		Appearance.FacialStyle,
		*Appearance.EquipmentSummary);
}

UTexture2D* WoWRuntimeCharacterAppearance::ComposeBodyAtlas(const FWoWRuntimeCharacterAppearanceDescriptor& Appearance, int32& OutAppliedLayers, int32& OutMissingLayers)
{
	OutAppliedLayers = 0;
	OutMissingLayers = 0;

	const FString Signature = BuildBodyAtlasSignature(Appearance);
	TMap<FString, TObjectPtr<UTexture2D>>& CachedAtlases = GetBodyAtlasCache();
	if (TObjectPtr<UTexture2D>* CachedTexture = CachedAtlases.Find(Signature))
	{
		if (const FRuntimeBodyAtlasStats* CachedStats = GetBodyAtlasStatsCache().Find(Signature))
		{
			OutAppliedLayers = CachedStats->AppliedLayers;
			OutMissingLayers = CachedStats->MissingLayers;
		}
		return CachedTexture->Get();
	}

	const double ComposeStartSeconds = FPlatformTime::Seconds();
	const FRuntimeAppearanceDbcCache& DbcCache = GetRuntimeAppearanceDbcCache();
	if (!DbcCache.bLoaded)
	{
		return nullptr;
	}

	FString BaseBodyPng;
	if (!FindRuntimeCharSectionTexturePath(DbcCache, Appearance.Race, Appearance.Gender, 0, 0, Appearance.Skin, TEXT("tex1"), BaseBodyPng))
	{
		LogMissingCharacterAppearanceTexture(DbcCache, Appearance, 0, 0, Appearance.Skin, TEXT("tex1"), TEXT("base_body"));
		return nullptr;
	}

	TArray<uint8> AtlasBgra;
	int32 AtlasWidth = 0;
	int32 AtlasHeight = 0;
	if (!ReadPngFileBgra(BaseBodyPng, AtlasBgra, AtlasWidth, AtlasHeight))
	{
		LogMissingCharacterAppearanceTexture(DbcCache, Appearance, 0, 0, Appearance.Skin, TEXT("tex1"), TEXT("base_body"));
		return nullptr;
	}

	{
		auto BlendCharSection = [&](int32 SectionType, int32 Section, int32 Color, const TCHAR* Field, int32 DestX, int32 DestY, int32 DestWidth, int32 DestHeight)
		{
			FString PreviewPng;
			if (!FindRuntimeCharSectionTexturePath(DbcCache, Appearance.Race, Appearance.Gender, SectionType, Section, Color, Field, PreviewPng))
			{
				if (LogMissingCharacterAppearanceTexture(DbcCache, Appearance, SectionType, Section, Color, Field, TEXT("body_layer")))
				{
					++OutMissingLayers;
				}
				return;
			}
			TArray<FString> PreviewPngCandidates;
			PreviewPngCandidates.Add(PreviewPng);
			if (!BlendFirstAvailablePngLayer(AtlasBgra, AtlasWidth, AtlasHeight, PreviewPngCandidates, DestX, DestY, DestWidth, DestHeight, OutAppliedLayers, OutMissingLayers))
			{
				LogMissingCharacterAppearanceTexture(DbcCache, Appearance, SectionType, Section, Color, Field, TEXT("body_layer"));
			}
		};

		BlendCharSection(1, Appearance.Face, Appearance.Skin, TEXT("tex1"), 0, 384, 256, 128);
		BlendCharSection(1, Appearance.Face, Appearance.Skin, TEXT("tex2"), 0, 320, 256, 64);
		BlendCharSection(4, 0, Appearance.Skin, TEXT("tex1"), 256, 192, 256, 128);
		BlendCharSection(4, 0, Appearance.Skin, TEXT("tex2"), 256, 0, 256, 128);
		BlendCharSection(3, Appearance.HairStyle, Appearance.HairColor, TEXT("tex2"), 0, 384, 256, 128);
		BlendCharSection(3, Appearance.HairStyle, Appearance.HairColor, TEXT("tex3"), 0, 320, 256, 64);
		BlendCharSection(2, Appearance.FacialStyle, Appearance.HairColor, TEXT("tex1"), 0, 384, 256, 128);
		BlendCharSection(2, Appearance.FacialStyle, Appearance.HairColor, TEXT("tex2"), 0, 320, 256, 64);
	}

	const TArray<FEquipmentDisplay> Equipment = ParseEquipmentSummary(Appearance.EquipmentSummary);
	TOptional<int32> RobeDisplayId;
	for (const FEquipmentDisplay& Item : Equipment)
	{
		if (Item.DisplayId > 0 && Item.InventoryType == 20)
		{
			RobeDisplayId = Item.DisplayId;
			break;
		}
	}

	for (const FEquipmentDisplay& Item : Equipment)
	{
		if (Item.DisplayId <= 0)
		{
			continue;
		}
		const FRuntimeItemDisplayTextureRecord* ItemTextureRecord = DbcCache.ItemDisplayTexturesById.Find(Item.DisplayId);
		if (!ItemTextureRecord)
		{
			continue;
		}

		auto BlendItemField = [&](const FString& Field, const FString& ComponentDir, const FString& ComponentName, std::initializer_list<const TCHAR*> Suffixes)
		{
			if (ComponentName.IsEmpty())
			{
				return;
			}
			if (!ShouldApplyItemTextureFieldToEquipment(Field, Item, RobeDisplayId))
			{
				return;
			}
			int32 DestX = 0;
			int32 DestY = 0;
			int32 DestWidth = 0;
			int32 DestHeight = 0;
			if (!GetCharacterAtlasRegionForItemField(Field, DestX, DestY, DestWidth, DestHeight))
			{
				return;
			}
			const TArray<FString> WowCandidates = BuildItemTextureCandidateWowPaths(ComponentDir, ComponentName, Suffixes);
			const TArray<FString> PngCandidates = BuildItemTextureCandidatePngPaths(ComponentDir, ComponentName, Suffixes);
			if (!BlendFirstAvailablePngLayer(
				AtlasBgra,
				AtlasWidth,
				AtlasHeight,
				PngCandidates,
				DestX,
				DestY,
				DestWidth,
				DestHeight,
				OutAppliedLayers,
				OutMissingLayers))
			{
				LogMissingItemTextureComponent(Appearance, Item, Field, ComponentDir, ComponentName, WowCandidates, PngCandidates);
			}
		};

		const TCHAR* GenderSuffix = Appearance.Gender == 1 ? TEXT("F") : TEXT("M");
		BlendItemField(TEXT("arm_upper"), TEXT("ArmUpperTexture"), ItemTextureRecord->ArmUpper, { TEXT("U") });
		BlendItemField(TEXT("arm_lower"), TEXT("ArmLowerTexture"), ItemTextureRecord->ArmLower, { TEXT("U") });
		BlendItemField(TEXT("hand"), TEXT("HandTexture"), ItemTextureRecord->Hand, { TEXT("U") });
		BlendItemField(TEXT("torso_upper"), TEXT("TorsoUpperTexture"), ItemTextureRecord->TorsoUpper, { GenderSuffix, TEXT("U") });
		BlendItemField(TEXT("torso_lower"), TEXT("TorsoLowerTexture"), ItemTextureRecord->TorsoLower, { TEXT("U"), GenderSuffix });
		BlendItemField(TEXT("leg_upper"), TEXT("LegUpperTexture"), ItemTextureRecord->LegUpper, { GenderSuffix, TEXT("U") });
		BlendItemField(TEXT("leg_lower"), TEXT("LegLowerTexture"), ItemTextureRecord->LegLower, { GenderSuffix, TEXT("U") });
		BlendItemField(TEXT("foot"), TEXT("FootTexture"), ItemTextureRecord->Foot, { GenderSuffix, TEXT("U") });
	}

	UTexture2D* ComposedTexture = CreateTransientBgraTexture(TEXT("WoWRuntimeBodyAtlas_") + BuildAppearanceHash(Appearance), AtlasBgra, AtlasWidth, AtlasHeight);
	if (ComposedTexture)
	{
		ComposedTexture->AddToRoot();
		CachedAtlases.Add(Signature, ComposedTexture);
		FRuntimeBodyAtlasStats Stats;
		Stats.AppliedLayers = OutAppliedLayers;
		Stats.MissingLayers = OutMissingLayers;
		GetBodyAtlasStatsCache().Add(Signature, Stats);
	}
	UE_LOG(LogTemp, Display, TEXT("角色运行态身体贴图合成完成: guid=%d race=%d gender=%d layers=%d missing=%d elapsed=%.2fms"),
		Appearance.Guid,
		Appearance.Race,
		Appearance.Gender,
		OutAppliedLayers,
		OutMissingLayers,
		(FPlatformTime::Seconds() - ComposeStartSeconds) * 1000.0);
	return ComposedTexture;
}

UTexture2D* WoWRuntimeCharacterAppearance::PrewarmBodyAtlas(const FWoWRuntimeCharacterAppearanceDescriptor& Appearance)
{
	const double PrewarmStartSeconds = FPlatformTime::Seconds();
	int32 AppliedLayers = 0;
	int32 MissingLayers = 0;
	UTexture2D* Texture = ComposeBodyAtlas(Appearance, AppliedLayers, MissingLayers);
	UE_LOG(LogTemp, Display, TEXT("角色运行态身体 atlas 预热完成: guid=%d race=%d gender=%d cached=%d layers=%d missing=%d elapsed=%.2fms"),
		Appearance.Guid,
		Appearance.Race,
		Appearance.Gender,
		Texture ? 1 : 0,
		AppliedLayers,
		MissingLayers,
		(FPlatformTime::Seconds() - PrewarmStartSeconds) * 1000.0);
	return Texture;
}

UTexture2D* WoWRuntimeCharacterAppearance::FindCachedBodyAtlas(const FWoWRuntimeCharacterAppearanceDescriptor& Appearance)
{
	const FString Signature = BuildBodyAtlasSignature(Appearance);
	if (TObjectPtr<UTexture2D>* CachedTexture = GetBodyAtlasCache().Find(Signature))
	{
		return CachedTexture->Get();
	}
	return nullptr;
}

bool WoWRuntimeCharacterAppearance::HasTextureSupport(const FWoWRuntimeCharacterAppearanceDescriptor& Appearance)
{
	const FRuntimeAppearanceDbcCache& DbcCache = GetRuntimeAppearanceDbcCache();
	if (!DbcCache.bLoaded)
	{
		return false;
	}

	FString BodyTexturePath;
	return FindRuntimeCharSectionTexturePath(DbcCache, Appearance.Race, Appearance.Gender, 0, 0, Appearance.Skin, TEXT("tex1"), BodyTexturePath) &&
		!BodyTexturePath.IsEmpty() &&
		FPaths::FileExists(BodyTexturePath);
}

FWoWRuntimeCharacterMaterialApplyResult WoWRuntimeCharacterAppearance::ApplyRuntimeBodyTextures(
	USkeletalMeshComponent* SkeletalMeshComponent,
	const FWoWRuntimeCharacterAppearanceDescriptor& Appearance,
	const FString& SectionMetadataPath,
	bool bAllowComposeIfMissing)
{
	FWoWRuntimeCharacterMaterialApplyResult Result;
	if (!SkeletalMeshComponent)
	{
		return Result;
	}

	const FRuntimeAppearanceDbcCache& DbcCache = GetRuntimeAppearanceDbcCache();
	if (!DbcCache.bLoaded)
	{
		UE_LOG(LogTemp, Warning, TEXT("角色运行态 DBC 未加载: guid=%d race=%d gender=%d"),
			Appearance.Guid,
			Appearance.Race,
			Appearance.Gender);
		return Result;
	}

	FString JsonText;
	if (!FFileHelper::LoadFileToString(JsonText, *SectionMetadataPath))
	{
		UE_LOG(LogTemp, Warning, TEXT("角色运行态基础 mesh 缺少 section metadata，无法套用外观纹理: guid=%d path=%s"),
			Appearance.Guid,
			*SectionMetadataPath);
		return Result;
	}

	TSharedPtr<FJsonObject> RootObject;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(JsonText);
	if (!FJsonSerializer::Deserialize(Reader, RootObject) || !RootObject.IsValid())
	{
		UE_LOG(LogTemp, Warning, TEXT("角色运行态基础 mesh section metadata 解析失败，无法套用外观纹理: guid=%d path=%s"),
			Appearance.Guid,
			*SectionMetadataPath);
		return Result;
	}

	const TArray<TSharedPtr<FJsonValue>>* Sections = nullptr;
	if (!RootObject->TryGetArrayField(TEXT("sections"), Sections) || !Sections)
	{
		return Result;
	}

	TMap<FString, UTexture2D*> TextureCache;
	UTexture2D* ComposedBodyAtlas = FindCachedBodyAtlas(Appearance);
	if (!ComposedBodyAtlas)
	{
		if (bAllowComposeIfMissing)
		{
			ComposedBodyAtlas = ComposeBodyAtlas(Appearance, Result.AppliedItemLayers, Result.MissingItemLayers);
		}
		else
		{
			++Result.MissingItemLayers;
			UE_LOG(LogTemp, Warning, TEXT("角色运行态身体 atlas 未预热，展示阶段跳过合成: guid=%d race=%d gender=%d"),
				Appearance.Guid,
				Appearance.Race,
				Appearance.Gender);
		}
	}
	for (const TSharedPtr<FJsonValue>& SectionValue : *Sections)
	{
		const TSharedPtr<FJsonObject> SectionObject = TryReadJsonObject(SectionValue);
		if (!SectionObject.IsValid())
		{
			continue;
		}

		double SectionNumber = 0.0;
		double SubmeshNumber = 0.0;
		double TextureTypeNumber = -1.0;
		if (!SectionObject->TryGetNumberField(TEXT("section_index"), SectionNumber) ||
			!SectionObject->TryGetNumberField(TEXT("texture_type"), TextureTypeNumber))
		{
			continue;
		}

		const int32 SectionIndex = FMath::RoundToInt(SectionNumber);
		const int32 TextureType = FMath::RoundToInt(TextureTypeNumber);
		const int32 MaterialSlotIndex = ResolveRuntimeMaterialSlotForM2Section(SkeletalMeshComponent, SectionIndex);
		if (MaterialSlotIndex == INDEX_NONE)
		{
			++Result.MissingSlots;
			continue;
		}
		FString SectionPreviewPng;
		SectionObject->TryGetStringField(TEXT("preview_png"), SectionPreviewPng);
		const int32 SubmeshId = SectionObject->TryGetNumberField(TEXT("submesh_id"), SubmeshNumber)
			? FMath::RoundToInt(SubmeshNumber)
			: INDEX_NONE;

		int32 SectionType = INDEX_NONE;
		int32 CharSection = 0;
		int32 Color = 0;
		const TCHAR* Field = TEXT("tex1");
		if (TextureType == 1)
		{
			SectionType = 0;
			CharSection = 0;
			Color = Appearance.Skin;
			Field = TEXT("tex1");
		}
		else if (TextureType == 2)
		{
			const FString ResolvedPreviewPng = ResolvePreviewPngPath(SectionPreviewPng);
			const FString FacePreviewProbe = !ResolvedPreviewPng.IsEmpty() ? ResolvedPreviewPng : SectionPreviewPng;
			if (TryResolveFaceSectionFieldFromPreview(FacePreviewProbe, Field))
			{
				SectionType = 1;
				CharSection = Appearance.Face;
			}
			else
			{
				SectionType = 0;
				CharSection = 0;
				Field = TEXT("tex1");
			}
			Color = Appearance.Skin;
		}
		else if (TextureType == 6)
		{
			SectionType = 3;
			CharSection = Appearance.HairStyle;
			Color = Appearance.HairColor;
			Field = TEXT("tex1");
		}
		else if (TextureType == 8)
		{
			SectionType = 0;
			CharSection = 0;
			Color = Appearance.Skin;
			Field = TEXT("tex2");
		}
		else
		{
			continue;
		}

		FString TexturePngPath;
		FString ResolvedRuntimeTexturePng;
		UTexture2D* Texture = nullptr;
		const bool bUseComposedBodyAtlas = WoWRuntimeCharacterAppearance::ShouldUseComposedBodyAtlasForSection(TextureType, SubmeshId, SectionPreviewPng);
		if (bUseComposedBodyAtlas && ComposedBodyAtlas)
		{
			Texture = ComposedBodyAtlas;
			ResolvedRuntimeTexturePng = TEXT("<body-atlas>");
		}
		else if (bUseComposedBodyAtlas)
		{
			UE_LOG(LogTemp, Warning, TEXT("角色运行态身体 atlas 不可用，section 无法贴图且不会使用错误兜底: guid=%d sectionIndex=%d textureType=%d race=%d gender=%d skin=%d"),
				Appearance.Guid,
				SectionIndex,
				TextureType,
				Appearance.Race,
				Appearance.Gender,
				Appearance.Skin);
		}
		else
		{
			if (!WoWRuntimeCharacterAppearance::ResolveRuntimeSectionTexturePng(Appearance, TextureType, SubmeshId, SectionPreviewPng, TexturePngPath) ||
				TexturePngPath.IsEmpty())
			{
				if (LogMissingCharacterAppearanceTexture(DbcCache, Appearance, SectionType, CharSection, Color, Field, TEXT("material_section")))
				{
					++Result.MissingSlots;
				}
				continue;
			}
			ResolvedRuntimeTexturePng = TexturePngPath;
			if (UTexture2D** CachedTexture = TextureCache.Find(TexturePngPath))
			{
				Texture = *CachedTexture;
				if (!Texture)
				{
					LogMissingCharacterAppearanceTexture(DbcCache, Appearance, SectionType, CharSection, Color, Field, TEXT("material_section_cached"));
				}
			}
			else
			{
				TArray<uint8> TextureBgra;
				int32 TextureWidth = 0;
				int32 TextureHeight = 0;
				if (ReadPngFileBgra(TexturePngPath, TextureBgra, TextureWidth, TextureHeight))
				{
					Texture = CreateTransientBgraTexture(TEXT("WoWRuntimeSection_") + FString::Printf(TEXT("%08x"), FCrc::StrCrc32(*TexturePngPath)), TextureBgra, TextureWidth, TextureHeight);
					if (Texture)
					{
						Texture->AddToRoot();
					}
				}
				else
				{
					LogMissingCharacterAppearanceTexture(DbcCache, Appearance, SectionType, CharSection, Color, Field, TEXT("material_section"));
				}
				TextureCache.Add(TexturePngPath, Texture);
			}
		}

		if (!Texture)
		{
			++Result.MissingSlots;
			continue;
		}

		UMaterialInstanceDynamic* DynamicMaterial = Cast<UMaterialInstanceDynamic>(SkeletalMeshComponent->GetMaterial(MaterialSlotIndex));
		if (!DynamicMaterial)
		{
			DynamicMaterial = SkeletalMeshComponent->CreateAndSetMaterialInstanceDynamic(MaterialSlotIndex);
		}
		if (!DynamicMaterial)
		{
			continue;
		}

		DynamicMaterial->SetTextureParameterValue(TEXT("DiffuseTexture"), Texture);
		if (IsRuntimeCharacterEyeGeosetSubmesh(SubmeshId) || (SubmeshId >= 1500 && SubmeshId < 1800))
		{
			UE_LOG(LogTemp, Display, TEXT("角色运行态关键 section 贴图: guid=%d section=%d slot=%d submesh=%d textureType=%d useAtlas=%d texture=%s exportPreview=%s"),
				Appearance.Guid,
				SectionIndex,
				MaterialSlotIndex,
				SubmeshId,
				TextureType,
				bUseComposedBodyAtlas ? 1 : 0,
				*ResolvedRuntimeTexturePng,
				*SectionPreviewPng);
		}
		if (bUseComposedBodyAtlas) { ++Result.BodySlots; }
		else if (SectionType == 1) { ++Result.FaceSlots; }
		else if (TextureType == 6) { ++Result.HairSlots; }
		else if (TextureType == 8) { ++Result.FurSlots; }
	}

	UE_LOG(LogTemp, Display, TEXT("角色运行态外观纹理已应用: guid=%d race=%d gender=%d skin=%d face=%d hair=%d/%d body=%d faceSlots=%d hairSlots=%d fur=%d itemLayers=%d missingItemLayers=%d missing=%d metadata=%s"),
		Appearance.Guid,
		Appearance.Race,
		Appearance.Gender,
		Appearance.Skin,
		Appearance.Face,
		Appearance.HairStyle,
		Appearance.HairColor,
		Result.BodySlots,
		Result.FaceSlots,
		Result.HairSlots,
		Result.FurSlots,
		Result.AppliedItemLayers,
		Result.MissingItemLayers,
		Result.MissingSlots,
		*SectionMetadataPath);
	return Result;
}
