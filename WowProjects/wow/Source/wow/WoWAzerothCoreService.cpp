#include "WoWAzerothCoreService.h"

#include "Common/TcpSocketBuilder.h"
#include "HAL/PlatformProcess.h"
#include "IPAddress.h"
#include "Misc/ScopeExit.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

THIRD_PARTY_INCLUDES_START
#pragma push_macro("UI")
#define UI OPENSSL_UI_COMPAT
#include <openssl/bn.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/rc4.h>
#include <openssl/sha.h>
#include <zlib.h>
#pragma pop_macro("UI")
THIRD_PARTY_INCLUDES_END

#ifdef U64
#undef U64
#endif

#include <cstring>

namespace
{
constexpr const TCHAR* DefaultAuthHost = TEXT("127.0.0.1");
constexpr int32 DefaultAuthPort = 3724;
constexpr uint32 ClientBuild = 12340;
constexpr uint8 AuthProtocolVersion = 8;
constexpr float SocketTimeoutSeconds = 12.0f;

constexpr uint16 CMSG_CHAR_CREATE = 0x036;
constexpr uint16 CMSG_CHAR_ENUM = 0x037;
constexpr uint16 SMSG_CHAR_CREATE = 0x03A;
constexpr uint16 SMSG_CHAR_ENUM = 0x03B;
constexpr uint16 SMSG_AUTH_CHALLENGE = 0x1EC;
constexpr uint16 CMSG_AUTH_SESSION = 0x1ED;
constexpr uint16 SMSG_AUTH_RESPONSE = 0x1EE;

constexpr uint8 CHAR_CREATE_IN_PROGRESS = 0x2E;
constexpr uint8 CHAR_CREATE_SUCCESS = 0x2F;
constexpr uint8 WORLD_AUTH_OK = 0x0C;

constexpr uint8 WotlkEncryptKey[16] =
{
	0xC2, 0xB3, 0x72, 0x3C, 0xC6, 0xAE, 0xD9, 0xB5,
	0x34, 0x3C, 0x53, 0xEE, 0x2F, 0x43, 0x67, 0xCE
};

constexpr uint8 WotlkDecryptKey[16] =
{
	0xCC, 0x98, 0xAE, 0x04, 0xE8, 0x97, 0xEA, 0xCA,
	0x12, 0xDD, 0xC0, 0x93, 0x42, 0x91, 0x53, 0x57
};

struct FRealmInfo
{
	FString Name;
	FString Address;
	uint8 Icon = 0;
	uint8 Lock = 0;
	uint8 Flags = 0;
	float Population = 0.0f;
	uint8 CharacterCount = 0;
	uint8 Timezone = 0;
	uint8 Id = 1;
	uint8 MajorVersion = 0;
	uint8 MinorVersion = 0;
	uint8 PatchVersion = 0;
	uint16 Build = 0;
};

struct FByteReader
{
	const TArray<uint8>& Data;
	int32 Offset = 0;

	bool Has(int32 Count) const { return Count >= 0 && Offset + Count <= Data.Num(); }
	uint8 U8() { return Has(1) ? Data[Offset++] : 0; }
	uint16 U16()
	{
		const uint16 Value = static_cast<uint16>(U8()) | (static_cast<uint16>(U8()) << 8);
		return Value;
	}
	uint32 U32()
	{
		const uint32 Value = static_cast<uint32>(U8()) |
			(static_cast<uint32>(U8()) << 8) |
			(static_cast<uint32>(U8()) << 16) |
			(static_cast<uint32>(U8()) << 24);
		return Value;
	}
	uint64 U64()
	{
		const uint64 Lo = U32();
		const uint64 Hi = U32();
		return Lo | (Hi << 32);
	}
	float F32()
	{
		const uint32 Bits = U32();
		float Value = 0.0f;
		std::memcpy(&Value, &Bits, sizeof(float));
		return Value;
	}
	FString CString()
	{
		TArray<ANSICHAR> Bytes;
		while (Has(1))
		{
			const uint8 C = U8();
			if (C == 0)
			{
				break;
			}
			Bytes.Add(static_cast<ANSICHAR>(C));
		}
		Bytes.Add('\0');
		return UTF8_TO_TCHAR(Bytes.GetData());
	}
};

struct FByteWriter
{
	TArray<uint8> Data;

	void U8(uint8 Value) { Data.Add(Value); }
	void U16(uint16 Value)
	{
		Data.Add(static_cast<uint8>(Value & 0xFF));
		Data.Add(static_cast<uint8>((Value >> 8) & 0xFF));
	}
	void U32(uint32 Value)
	{
		Data.Add(static_cast<uint8>(Value & 0xFF));
		Data.Add(static_cast<uint8>((Value >> 8) & 0xFF));
		Data.Add(static_cast<uint8>((Value >> 16) & 0xFF));
		Data.Add(static_cast<uint8>((Value >> 24) & 0xFF));
	}
	void U64(uint64 Value)
	{
		U32(static_cast<uint32>(Value & 0xFFFFFFFFULL));
		U32(static_cast<uint32>((Value >> 32) & 0xFFFFFFFFULL));
	}
	void CString(const FString& Value)
	{
		FTCHARToUTF8 Utf8(*Value);
		Data.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
		Data.Add(0);
	}
	void Bytes(const uint8* Ptr, int32 Count)
	{
		if (Ptr && Count > 0)
		{
			Data.Append(Ptr, Count);
		}
	}
	void Bytes(const TArray<uint8>& BytesValue)
	{
		Data.Append(BytesValue);
	}
};

struct FSrpProof
{
	TArray<uint8> A;
	TArray<uint8> M1;
	TArray<uint8> M2;
	TArray<uint8> SessionKey;
};

struct FAuthChallenge
{
	TArray<uint8> B;
	TArray<uint8> G;
	TArray<uint8> N;
	TArray<uint8> Salt;
	uint8 SecurityFlags = 0;
};

struct FProtocolSession
{
	TUniquePtr<FSocket> AuthSocket;
	TUniquePtr<FSocket> WorldSocket;
	TArray<uint8> AuthBuffer;
	TArray<uint8> WorldBuffer;
	TArray<FWoWGlueCharacterSummary> Characters;
	FString AccountName;
	TArray<uint8> SessionKey;
	TArray<FRealmInfo> Realms;
	FRealmInfo Realm;
	RC4_KEY EncryptRc4 = {};
	RC4_KEY DecryptRc4 = {};
	bool bWorldHeaderEncrypted = false;
};

FProtocolSession GSession;

FString ToLatinUpper(FString Value)
{
	for (TCHAR& Character : Value)
	{
		if (Character <= 0x7F)
		{
			Character = FChar::ToUpper(Character);
		}
	}
	return Value;
}

TArray<uint8> Utf8Bytes(const FString& Value)
{
	FTCHARToUTF8 Utf8(*Value);
	TArray<uint8> Result;
	Result.Append(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	return Result;
}

TArray<uint8> Sha1Bytes(const TArray<uint8>& Input)
{
	TArray<uint8> Digest;
	Digest.SetNumZeroed(SHA_DIGEST_LENGTH);
	SHA1(Input.GetData(), Input.Num(), Digest.GetData());
	return Digest;
}

TArray<uint8> Sha1String(const FString& Value)
{
	return Sha1Bytes(Utf8Bytes(Value));
}

TArray<uint8> HmacSha1(const uint8* Key, int32 KeyLen, const TArray<uint8>& Input)
{
	TArray<uint8> Digest;
	Digest.SetNumZeroed(SHA_DIGEST_LENGTH);
	unsigned int Len = 0;
	HMAC(EVP_sha1(), Key, KeyLen, Input.GetData(), Input.Num(), Digest.GetData(), &Len);
	Digest.SetNum(static_cast<int32>(Len));
	return Digest;
}

BIGNUM* BigNumFromLittle(const TArray<uint8>& Bytes)
{
	return BN_lebin2bn(Bytes.GetData(), Bytes.Num(), nullptr);
}

TArray<uint8> BigNumToLittle(const BIGNUM* Value, int32 MinBytes = 0)
{
	if (!Value)
	{
		return {};
	}

	const int32 NaturalSize = BN_num_bytes(Value);
	const int32 Size = FMath::Max(NaturalSize, MinBytes);
	TArray<uint8> BigEndian;
	BigEndian.SetNumZeroed(Size);
	if (NaturalSize > 0)
	{
		BN_bn2bin(Value, BigEndian.GetData() + (Size - NaturalSize));
	}
	for (int32 Left = 0, Right = BigEndian.Num() - 1; Left < Right; ++Left, --Right)
	{
		Swap(BigEndian[Left], BigEndian[Right]);
	}
	return BigEndian;
}

TArray<uint8> MakeRandomBytes(int32 Count)
{
	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(Count);
	RAND_bytes(Bytes.GetData(), Count);
	return Bytes;
}

bool ComputeSrpProof(const FString& AccountName, const FString& Password, const FAuthChallenge& Challenge, FSrpProof& OutProof, FString& OutError)
{
	BN_CTX* Ctx = BN_CTX_new();
	BIGNUM* G = BigNumFromLittle(Challenge.G);
	BIGNUM* N = BigNumFromLittle(Challenge.N);
	BIGNUM* B = BigNumFromLittle(Challenge.B);
	BIGNUM* KMultiplier = BN_new();
	BIGNUM* X = nullptr;
	BIGNUM* APrivate = nullptr;
	BIGNUM* APublic = BN_new();
	BIGNUM* U = nullptr;
	BIGNUM* GX = BN_new();
	BIGNUM* KGX = BN_new();
	BIGNUM* KN = BN_new();
	BIGNUM* Diff = BN_new();
	BIGNUM* UX = BN_new();
	BIGNUM* AUX = BN_new();
	BIGNUM* S = BN_new();

	ON_SCOPE_EXIT
	{
		BN_free(S);
		BN_free(AUX);
		BN_free(UX);
		BN_free(Diff);
		BN_free(KN);
		BN_free(KGX);
		BN_free(GX);
		BN_free(APublic);
		BN_free(APrivate);
		BN_free(X);
		BN_free(KMultiplier);
		BN_free(B);
		BN_free(N);
		BN_free(G);
		BN_CTX_free(Ctx);
	};

	if (!Ctx || !G || !N || !B || !KMultiplier || !APublic || !GX || !KGX || !KN || !Diff || !UX || !AUX || !S)
	{
		OutError = TEXT("SRP 初始化失败。");
		return false;
	}

	BN_set_word(KMultiplier, 3);

	const FString UpperAccount = ToLatinUpper(AccountName);
	const FString UpperPassword = ToLatinUpper(Password);
	const TArray<uint8> InnerHash = Sha1String(UpperAccount + TEXT(":") + UpperPassword);
	TArray<uint8> XInput = Challenge.Salt;
	XInput.Append(InnerHash);
	const TArray<uint8> XHash = Sha1Bytes(XInput);
	X = BigNumFromLittle(XHash);

	for (int32 Attempt = 0; Attempt < 100; ++Attempt)
	{
		const TArray<uint8> RandomA = MakeRandomBytes(19);
		APrivate = BigNumFromLittle(RandomA);
		BN_mod_exp(APublic, G, APrivate, N, Ctx);
		if (!BN_is_zero(APublic))
		{
			break;
		}
		BN_free(APrivate);
		APrivate = nullptr;
	}

	if (!APrivate || BN_is_zero(APublic))
	{
		OutError = TEXT("SRP 随机数生成失败。");
		return false;
	}

	const TArray<uint8> AWire = BigNumToLittle(APublic, 32);
	const TArray<uint8> ANatural = BigNumToLittle(APublic);
	const TArray<uint8> BNatural = BigNumToLittle(B);

	TArray<uint8> UInput = ANatural;
	UInput.Append(BNatural);
	const TArray<uint8> UHash = Sha1Bytes(UInput);
	U = BigNumFromLittle(UHash);

	BN_mod_exp(GX, G, X, N, Ctx);
	BN_mul(KGX, KMultiplier, GX, Ctx);
	BN_mul(KN, KMultiplier, N, Ctx);
	BN_add(Diff, B, KN);
	BN_sub(Diff, Diff, KGX);
	BN_mul(UX, U, X, Ctx);
	BN_add(AUX, APrivate, UX);
	BN_mod_exp(S, Diff, AUX, N, Ctx);

	const TArray<uint8> SBytes = BigNumToLittle(S, 32);
	TArray<uint8> S1;
	TArray<uint8> S2;
	for (int32 Index = 0; Index < 16; ++Index)
	{
		S1.Add(SBytes[Index * 2]);
		S2.Add(SBytes[Index * 2 + 1]);
	}
	const TArray<uint8> S1Hash = Sha1Bytes(S1);
	const TArray<uint8> S2Hash = Sha1Bytes(S2);
	TArray<uint8> SessionKey;
	SessionKey.Reserve(40);
	for (int32 Index = 0; Index < SHA_DIGEST_LENGTH; ++Index)
	{
		SessionKey.Add(S1Hash[Index]);
		SessionKey.Add(S2Hash[Index]);
	}

	TArray<uint8> NHash = Sha1Bytes(BigNumToLittle(N));
	TArray<uint8> GHash = Sha1Bytes(BigNumToLittle(G));
	TArray<uint8> NgXor;
	NgXor.SetNumZeroed(SHA_DIGEST_LENGTH);
	for (int32 Index = 0; Index < SHA_DIGEST_LENGTH; ++Index)
	{
		NgXor[Index] = NHash[Index] ^ GHash[Index];
	}

	const TArray<uint8> UserHash = Sha1String(UpperAccount);
	BIGNUM* SaltNatural = BigNumFromLittle(Challenge.Salt);
	TArray<uint8> M1Input = NgXor;
	M1Input.Append(UserHash);
	M1Input.Append(BigNumToLittle(SaltNatural));
	BN_free(SaltNatural);
	M1Input.Append(ANatural);
	M1Input.Append(BNatural);
	M1Input.Append(SessionKey);
	const TArray<uint8> M1 = Sha1Bytes(M1Input);

	TArray<uint8> M2Input = ANatural;
	M2Input.Append(M1);
	M2Input.Append(SessionKey);

	OutProof.A = AWire;
	OutProof.M1 = M1;
	OutProof.M2 = Sha1Bytes(M2Input);
	OutProof.SessionKey = SessionKey;
	return true;
}

bool SendAll(FSocket* Socket, const TArray<uint8>& Bytes, FString& OutError)
{
	if (!Socket)
	{
		OutError = TEXT("Socket 未连接。");
		return false;
	}

	int32 TotalSent = 0;
	while (TotalSent < Bytes.Num())
	{
		int32 Sent = 0;
		if (!Socket->Send(Bytes.GetData() + TotalSent, Bytes.Num() - TotalSent, Sent) || Sent <= 0)
		{
			OutError = TEXT("发送网络包失败。");
			return false;
		}
		TotalSent += Sent;
	}
	return true;
}

TUniquePtr<FSocket> ConnectTcp(const FString& Host, int32 Port, FString& OutError)
{
	FString ResolvedHost = Host.Equals(TEXT("localhost"), ESearchCase::IgnoreCase) ? TEXT("127.0.0.1") : Host;
	TSharedRef<FInternetAddr> Addr = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM)->CreateInternetAddr();
	bool bValidIp = false;
	Addr->SetIp(*ResolvedHost, bValidIp);
	Addr->SetPort(Port);
	if (!bValidIp)
	{
		OutError = FString::Printf(TEXT("暂不支持非 IPv4 地址：%s"), *Host);
		return nullptr;
	}

	FSocket* RawSocket = FTcpSocketBuilder(TEXT("WoWAuthWorldSocket"))
		.AsBlocking()
		.WithReceiveBufferSize(1024 * 1024)
		.WithSendBufferSize(1024 * 1024);
	if (!RawSocket)
	{
		OutError = TEXT("创建 Socket 失败。");
		return nullptr;
	}

	TUniquePtr<FSocket> Socket(RawSocket);
	if (!Socket->Connect(*Addr))
	{
		OutError = FString::Printf(TEXT("无法连接 %s:%d。请确认 AzerothCore auth/world 服务已启动。"), *Host, Port);
		return nullptr;
	}
	return Socket;
}

bool PumpSocket(FSocket* Socket, TArray<uint8>& Buffer, FString& OutError)
{
	if (!Socket)
	{
		OutError = TEXT("Socket 未连接。");
		return false;
	}

	uint32 Pending = 0;
	while (Socket->HasPendingData(Pending) && Pending > 0)
	{
		const int32 ReadSize = FMath::Min<int32>(static_cast<int32>(Pending), 64 * 1024);
		const int32 OldNum = Buffer.Num();
		Buffer.SetNumUninitialized(OldNum + ReadSize);
		int32 Read = 0;
		if (!Socket->Recv(Buffer.GetData() + OldNum, ReadSize, Read))
		{
			Buffer.SetNum(OldNum);
			OutError = TEXT("接收网络数据失败。");
			return false;
		}
		Buffer.SetNum(OldNum + Read);
	}
	return true;
}

bool WaitUntil(TFunctionRef<bool()> TryConsume, TFunctionRef<bool(FString&)> Pump, FString& OutError)
{
	const double Start = FPlatformTime::Seconds();
	while (FPlatformTime::Seconds() - Start < SocketTimeoutSeconds)
	{
		if (TryConsume())
		{
			return true;
		}
		if (!Pump(OutError))
		{
			return false;
		}
		if (TryConsume())
		{
			return true;
		}
		FPlatformProcess::Sleep(0.01f);
	}
	OutError = TEXT("等待服务器响应超时。");
	return false;
}

int32 ExpectedAuthPacketSize(const TArray<uint8>& Buffer)
{
	if (Buffer.Num() < 1)
	{
		return 0;
	}
	const uint8 Opcode = Buffer[0];
	if (Opcode == 0x00)
	{
		if (Buffer.Num() < 3)
		{
			return 0;
		}
		const uint8 Status = Buffer[2];
		if (Status != 0)
		{
			return 3;
		}
		if (Buffer.Num() < 36)
		{
			return 0;
		}
		const uint8 GLen = Buffer[35];
		const int32 NLenOffset = 36 + GLen;
		if (Buffer.Num() <= NLenOffset)
		{
			return 0;
		}
		const uint8 NLen = Buffer[NLenOffset];
		const int32 BaseSize = 36 + GLen + 1 + NLen + 32 + 16 + 1;
		if (Buffer.Num() < BaseSize)
		{
			return 0;
		}
		const uint8 SecurityFlags = Buffer[BaseSize - 1];
		int32 Extra = 0;
		if (SecurityFlags & 0x01) Extra += 20;
		if (SecurityFlags & 0x02) Extra += 12;
		if (SecurityFlags & 0x04) Extra += 1;
		return BaseSize + Extra;
	}
	if (Opcode == 0x01)
	{
		if (Buffer.Num() < 2)
		{
			return 0;
		}
		return Buffer[1] == 0 ? 32 : FMath::Min(FMath::Max(Buffer.Num(), 2), 4);
	}
	if (Opcode == 0x10)
	{
		if (Buffer.Num() < 3)
		{
			return 0;
		}
		const uint16 Size = static_cast<uint16>(Buffer[1]) | (static_cast<uint16>(Buffer[2]) << 8);
		return 3 + Size;
	}
	return 0;
}

bool PopAuthPacket(TArray<uint8>& Buffer, TArray<uint8>& OutPacket)
{
	const int32 Expected = ExpectedAuthPacketSize(Buffer);
	if (Expected <= 0 || Buffer.Num() < Expected)
	{
		return false;
	}
	OutPacket.Reset(Expected);
	OutPacket.Append(Buffer.GetData(), Expected);
	Buffer.RemoveAt(0, Expected, EAllowShrinking::No);
	return true;
}

void WriteAuthHeaderLogonChallenge(FByteWriter& Writer, const FString& Account)
{
	const FString UpperAccount = ToLatinUpper(Account);
	const TArray<uint8> AccountBytes = Utf8Bytes(UpperAccount);
	const uint16 PayloadSize = static_cast<uint16>(30 + AccountBytes.Num());
	Writer.U8(0x00);
	Writer.U8(AuthProtocolVersion);
	Writer.U16(PayloadSize);

	auto FourCC = [&Writer](const ANSICHAR* Text)
	{
		uint8 Bytes[4] = { 0, 0, 0, 0 };
		const int32 Len = FMath::Min<int32>(FCStringAnsi::Strlen(Text), 4);
		for (int32 Index = 0; Index < Len; ++Index)
		{
			Bytes[Index] = static_cast<uint8>(Text[Len - 1 - Index]);
		}
		Writer.Bytes(Bytes, 4);
	};

	FourCC("WoW");
	Writer.U8(3);
	Writer.U8(3);
	Writer.U8(5);
	Writer.U16(ClientBuild);
	FourCC("x86");
	FourCC("Win");
	FourCC("zhCN");
	Writer.U32(0);
	Writer.U32(0);
	Writer.U8(static_cast<uint8>(AccountBytes.Num()));
	Writer.Bytes(AccountBytes);
}

bool ParseAuthChallengePacket(const TArray<uint8>& Packet, FAuthChallenge& OutChallenge, FString& OutError)
{
	if (Packet.Num() < 3 || Packet[0] != 0x00)
	{
		OutError = TEXT("AuthServer 返回了无效 LOGON_CHALLENGE。");
		return false;
	}
	const uint8 Result = Packet[2];
	if (Result != 0)
	{
		OutError = FString::Printf(TEXT("LOGON_CHALLENGE 失败，错误码：0x%02X。"), Result);
		return false;
	}

	FByteReader Reader{ Packet, 3 };
	OutChallenge.B.Reset(32);
	for (int32 Index = 0; Index < 32; ++Index) OutChallenge.B.Add(Reader.U8());
	const uint8 GLen = Reader.U8();
	for (int32 Index = 0; Index < GLen; ++Index) OutChallenge.G.Add(Reader.U8());
	const uint8 NLen = Reader.U8();
	for (int32 Index = 0; Index < NLen; ++Index) OutChallenge.N.Add(Reader.U8());
	for (int32 Index = 0; Index < 32; ++Index) OutChallenge.Salt.Add(Reader.U8());
	Reader.Offset += 16;
	OutChallenge.SecurityFlags = Reader.U8();
	if (OutChallenge.SecurityFlags != 0)
	{
		OutError = FString::Printf(TEXT("AuthServer 要求额外安全验证 flags=0x%02X，当前 UE 客户端暂未接入。"), OutChallenge.SecurityFlags);
		return false;
	}
	return true;
}

bool SendLogonProof(FSocket* Socket, const FSrpProof& Proof, uint8 SecurityFlags, FString& OutError)
{
	FByteWriter Writer;
	Writer.U8(0x01);
	Writer.Bytes(Proof.A);
	Writer.Bytes(Proof.M1);
	for (int32 Index = 0; Index < 20; ++Index) Writer.U8(0);
	Writer.U8(0);
	Writer.U8(SecurityFlags);
	return SendAll(Socket, Writer.Data, OutError);
}

bool ParseProofPacket(const TArray<uint8>& Packet, const FSrpProof& Proof, FString& OutError)
{
	if (Packet.Num() < 2 || Packet[0] != 0x01)
	{
		OutError = TEXT("AuthServer 返回了无效 LOGON_PROOF。");
		return false;
	}
	if (Packet[1] != 0)
	{
		OutError = FString::Printf(TEXT("账号或密码错误,LOGON_PROOF 错误码:0x%02X。"), Packet[1]);
		return false;
	}
	if (Packet.Num() >= 22)
	{
		const bool bM2Matches = FMemory::Memcmp(Packet.GetData() + 2, Proof.M2.GetData(), Proof.M2.Num()) == 0;
		if (!bM2Matches)
		{
			OutError = TEXT("服务器 proof 校验失败。");
			return false;
		}
	}
	return true;
}

bool SendRealmList(FSocket* Socket, FString& OutError)
{
	FByteWriter Writer;
	Writer.U8(0x10);
	Writer.U32(0);
	return SendAll(Socket, Writer.Data, OutError);
}

bool ParseRealmList(const TArray<uint8>& Packet, TArray<FRealmInfo>& OutRealms, FString& OutError)
{
	if (Packet.Num() < 3 || Packet[0] != 0x10)
	{
		OutError = TEXT("AuthServer 返回了无效 RealmList。");
		return false;
	}
	FByteReader Reader{ Packet, 3 };
	Reader.U32();
	const uint16 RealmCount = Reader.U16();
	if (RealmCount == 0)
	{
		OutError = TEXT("RealmList 为空，请确认 AzerothCore realmlist 配置。");
		return false;
	}

	OutRealms.Reset();
	OutRealms.Reserve(RealmCount);
	for (uint16 Index = 0; Index < RealmCount; ++Index)
	{
		FRealmInfo Realm;
		Realm.Icon = Reader.U8();
		Realm.Lock = Reader.U8();
		Realm.Flags = Reader.U8();
		Realm.Name = Reader.CString();
		Realm.Address = Reader.CString();
		Realm.Population = Reader.F32();
		Realm.CharacterCount = Reader.U8();
		Realm.Timezone = Reader.U8();
		Realm.Id = Reader.U8();
		if (Realm.Flags & 0x04)
		{
			Realm.MajorVersion = Reader.U8();
			Realm.MinorVersion = Reader.U8();
			Realm.PatchVersion = Reader.U8();
			Realm.Build = Reader.U16();
		}
		OutRealms.Add(MoveTemp(Realm));
	}
	return true;
}

bool ParseHostPort(const FString& Address, FString& OutHost, int32& OutPort)
{
	OutHost = Address;
	OutPort = 8085;
	FString PortText;
	if (Address.Split(TEXT(":"), &OutHost, &PortText, ESearchCase::IgnoreCase, ESearchDir::FromEnd))
	{
		OutPort = FCString::Atoi(*PortText);
	}
	return !OutHost.IsEmpty() && OutPort > 0;
}

bool SendWorldPacket(FProtocolSession& Session, uint16 Opcode, const TArray<uint8>& Payload, FString& OutError)
{
	FByteWriter Writer;
	const uint16 SizeField = static_cast<uint16>(Payload.Num() + 4);
	Writer.U8(static_cast<uint8>((SizeField >> 8) & 0xFF));
	Writer.U8(static_cast<uint8>(SizeField & 0xFF));
	Writer.U8(static_cast<uint8>(Opcode & 0xFF));
	Writer.U8(static_cast<uint8>((Opcode >> 8) & 0xFF));
	Writer.U8(0);
	Writer.U8(0);
	if (Session.bWorldHeaderEncrypted)
	{
		RC4(&Session.EncryptRc4, 6, Writer.Data.GetData(), Writer.Data.GetData());
	}
	Writer.Bytes(Payload);
	return SendAll(Session.WorldSocket.Get(), Writer.Data, OutError);
}

bool PopWorldPacket(FProtocolSession& Session, uint16& OutOpcode, TArray<uint8>& OutPayload)
{
	TArray<uint8>& Buffer = Session.WorldBuffer;
	if (Buffer.Num() < 4)
	{
		return false;
	}

	uint8 Header[4] = { Buffer[0], Buffer[1], Buffer[2], Buffer[3] };
	RC4_KEY PendingDecryptRc4 = {};
	if (Session.bWorldHeaderEncrypted)
	{
		PendingDecryptRc4 = Session.DecryptRc4;
		RC4(&PendingDecryptRc4, 4, Header, Header);
	}

	const uint16 Size = (static_cast<uint16>(Header[0]) << 8) | Header[1];
	if (Size < 2 || Size > 0x8000)
	{
		return false;
	}
	const uint16 PayloadLen = Size - 2;
	const int32 TotalSize = 4 + PayloadLen;
	if (Buffer.Num() < TotalSize)
	{
		return false;
	}
	if (Session.bWorldHeaderEncrypted)
	{
		Session.DecryptRc4 = PendingDecryptRc4;
	}

	OutOpcode = static_cast<uint16>(Header[2]) | (static_cast<uint16>(Header[3]) << 8);
	OutPayload.Reset(PayloadLen);
	if (PayloadLen > 0)
	{
		OutPayload.Append(Buffer.GetData() + 4, PayloadLen);
	}
	Buffer.RemoveAt(0, TotalSize, EAllowShrinking::No);
	return true;
}

void InitWorldHeaderCrypto(FProtocolSession& Session)
{
	const TArray<uint8> EncryptHash = HmacSha1(WotlkEncryptKey, UE_ARRAY_COUNT(WotlkEncryptKey), Session.SessionKey);
	const TArray<uint8> DecryptHash = HmacSha1(WotlkDecryptKey, UE_ARRAY_COUNT(WotlkDecryptKey), Session.SessionKey);
	RC4_set_key(&Session.EncryptRc4, EncryptHash.Num(), EncryptHash.GetData());
	RC4_set_key(&Session.DecryptRc4, DecryptHash.Num(), DecryptHash.GetData());
	uint8 Drop[1024] = {};
	RC4(&Session.EncryptRc4, sizeof(Drop), Drop, Drop);
	FMemory::Memzero(Drop);
	RC4(&Session.DecryptRc4, sizeof(Drop), Drop, Drop);
	Session.bWorldHeaderEncrypted = true;
}

bool ParseAuthChallengeWorld(const TArray<uint8>& Payload, uint32& OutServerSeed)
{
	FByteReader Reader{ Payload };
	if (Payload.Num() >= 40)
	{
		Reader.U32();
		OutServerSeed = Reader.U32();
		return true;
	}
	if (Payload.Num() >= 4)
	{
		OutServerSeed = Reader.U32();
		return true;
	}
	return false;
}

TArray<uint8> BuildAddonInfoCompressed()
{
	FByteWriter Addon;
	Addon.U32(0);
	Addon.U32(0);

	uLongf CompressedSize = compressBound(Addon.Data.Num());
	TArray<uint8> Compressed;
	Compressed.SetNumUninitialized(static_cast<int32>(CompressedSize));
	if (compress(Compressed.GetData(), &CompressedSize, Addon.Data.GetData(), Addon.Data.Num()) != Z_OK)
	{
		Compressed.Reset();
	}
	Compressed.SetNum(static_cast<int32>(CompressedSize));

	FByteWriter Result;
	Result.U32(Addon.Data.Num());
	Result.Bytes(Compressed);
	return Result.Data;
}

TArray<uint8> BuildAuthSessionPayload(const FProtocolSession& Session, uint32 ClientSeed, uint32 ServerSeed)
{
	const FString UpperAccount = ToLatinUpper(Session.AccountName);
	TArray<uint8> HashInput = Utf8Bytes(UpperAccount);
	HashInput.AddZeroed(4);
	HashInput.Add(static_cast<uint8>(ClientSeed & 0xFF));
	HashInput.Add(static_cast<uint8>((ClientSeed >> 8) & 0xFF));
	HashInput.Add(static_cast<uint8>((ClientSeed >> 16) & 0xFF));
	HashInput.Add(static_cast<uint8>((ClientSeed >> 24) & 0xFF));
	HashInput.Add(static_cast<uint8>(ServerSeed & 0xFF));
	HashInput.Add(static_cast<uint8>((ServerSeed >> 8) & 0xFF));
	HashInput.Add(static_cast<uint8>((ServerSeed >> 16) & 0xFF));
	HashInput.Add(static_cast<uint8>((ServerSeed >> 24) & 0xFF));
	HashInput.Append(Session.SessionKey);
	const TArray<uint8> Digest = Sha1Bytes(HashInput);

	FByteWriter Writer;
	Writer.U32(ClientBuild);
	Writer.U32(0);
	Writer.CString(UpperAccount);
	Writer.U32(0);
	Writer.U32(ClientSeed);
	Writer.U32(0);
	Writer.U32(0);
	Writer.U32(Session.Realm.Id);
	Writer.U64(0);
	Writer.Bytes(Digest);
	Writer.Bytes(BuildAddonInfoCompressed());
	return Writer.Data;
}

bool ParseCharEnum(const TArray<uint8>& Payload, TArray<FWoWGlueCharacterSummary>& OutCharacters, FString& OutError)
{
	FByteReader Reader{ Payload };
	if (!Reader.Has(1))
	{
		OutError = TEXT("SMSG_CHAR_ENUM 数据为空。");
		return false;
	}
	const uint8 Count = Reader.U8();
	OutCharacters.Reset();
	for (uint8 Index = 0; Index < Count; ++Index)
	{
		if (!Reader.Has(8))
		{
			break;
		}
		FWoWGlueCharacterSummary Character;
		Character.Guid = static_cast<int32>(Reader.U64());
		Character.Name = Reader.CString();
		Character.Race = Reader.U8();
		Character.ClassId = Reader.U8();
		Character.Gender = Reader.U8();
		const uint32 Appearance = Reader.U32();
		Character.Skin = static_cast<uint8>(Appearance & 0xFF);
		Character.Face = static_cast<uint8>((Appearance >> 8) & 0xFF);
		Character.HairStyle = static_cast<uint8>((Appearance >> 16) & 0xFF);
		Character.HairColor = static_cast<uint8>((Appearance >> 24) & 0xFF);
		Character.FacialStyle = Reader.U8();
		Character.Level = Reader.U8();
		Character.Zone = static_cast<int32>(Reader.U32());
		Character.Map = static_cast<int32>(Reader.U32());
		Character.PositionX = Reader.F32();
		Character.PositionY = Reader.F32();
		Character.PositionZ = Reader.F32();
		Reader.U32(); // guild id
		Character.Flags = Reader.U32();
		if (Reader.Has(4))
		{
			Character.AtLoginFlags = Reader.U32();
		}
		if (Reader.Has(1)) Reader.U8();
		if (Reader.Has(12))
		{
			Reader.U32();
			Reader.U32();
			Reader.U32();
		}
		for (int32 Slot = 0; Slot < 23 && Reader.Has(9); ++Slot)
		{
			FWoWGlueCharacterEquipmentSlot EquipmentSlot;
			EquipmentSlot.DisplayId = Reader.U32();
			EquipmentSlot.InventoryType = Reader.U8();
			EquipmentSlot.EnchantId = Reader.U32();
			Character.Equipment.Add(EquipmentSlot);
		}
		OutCharacters.Add(MoveTemp(Character));
	}
	return true;
}

bool WaitForWorldOpcode(FProtocolSession& Session, uint16 WantedOpcode, TArray<uint8>& OutPayload, FString& OutError)
{
	return WaitUntil(
		[&Session, WantedOpcode, &OutPayload]()
		{
			uint16 Opcode = 0;
			TArray<uint8> Payload;
			while (PopWorldPacket(Session, Opcode, Payload))
			{
				if (Opcode == WantedOpcode)
				{
					OutPayload = MoveTemp(Payload);
					return true;
				}
			}
			return false;
		},
		[&Session](FString& Error)
		{
			return PumpSocket(Session.WorldSocket.Get(), Session.WorldBuffer, Error);
		},
		OutError);
}

bool RequestCharacterEnum(FProtocolSession& Session, FString& OutError)
{
	const TArray<uint8> EmptyPayload;
	if (!SendWorldPacket(Session, CMSG_CHAR_ENUM, EmptyPayload, OutError))
	{
		return false;
	}

	TArray<uint8> Payload;
	if (!WaitForWorldOpcode(Session, SMSG_CHAR_ENUM, Payload, OutError))
	{
		return false;
	}
	return ParseCharEnum(Payload, Session.Characters, OutError);
}

bool ConnectAuthAndRealmList(const FString& AccountName, const FString& Password, FString& OutError)
{
	FWoWAzerothCoreService::Disconnect();
	GSession.AccountName = AccountName;

	GSession.AuthSocket = ConnectTcp(DefaultAuthHost, DefaultAuthPort, OutError);
	if (!GSession.AuthSocket)
	{
		return false;
	}

	FByteWriter ChallengeWriter;
	WriteAuthHeaderLogonChallenge(ChallengeWriter, AccountName);
	if (!SendAll(GSession.AuthSocket.Get(), ChallengeWriter.Data, OutError))
	{
		return false;
	}

	TArray<uint8> Packet;
	if (!WaitUntil(
		[&]() { return PopAuthPacket(GSession.AuthBuffer, Packet); },
		[&](FString& Error) { return PumpSocket(GSession.AuthSocket.Get(), GSession.AuthBuffer, Error); },
		OutError))
	{
		return false;
	}

	FAuthChallenge Challenge;
	if (!ParseAuthChallengePacket(Packet, Challenge, OutError))
	{
		return false;
	}

	FSrpProof Proof;
	if (!ComputeSrpProof(AccountName, Password, Challenge, Proof, OutError))
	{
		return false;
	}
	if (!SendLogonProof(GSession.AuthSocket.Get(), Proof, Challenge.SecurityFlags, OutError))
	{
		return false;
	}
	Packet.Reset();
	if (!WaitUntil(
		[&]() { return PopAuthPacket(GSession.AuthBuffer, Packet); },
		[&](FString& Error) { return PumpSocket(GSession.AuthSocket.Get(), GSession.AuthBuffer, Error); },
		OutError))
	{
		return false;
	}
	if (!ParseProofPacket(Packet, Proof, OutError))
	{
		return false;
	}

	GSession.SessionKey = Proof.SessionKey;
	if (!SendRealmList(GSession.AuthSocket.Get(), OutError))
	{
		return false;
	}
	Packet.Reset();
	if (!WaitUntil(
		[&]() { return PopAuthPacket(GSession.AuthBuffer, Packet); },
		[&](FString& Error) { return PumpSocket(GSession.AuthSocket.Get(), GSession.AuthBuffer, Error); },
		OutError))
	{
		return false;
	}
	if (!ParseRealmList(Packet, GSession.Realms, OutError))
	{
		return false;
	}

	return true;
}

bool ConnectWorldForRealm(FProtocolSession& Session, int32 RealmIndex, FString& OutError)
{
	if (!Session.Realms.IsValidIndex(RealmIndex))
	{
		OutError = TEXT("请选择有效的服务器。");
		return false;
	}

	if (Session.WorldSocket)
	{
		Session.WorldSocket->Close();
		Session.WorldSocket.Reset();
	}
	Session.WorldBuffer.Reset();
	Session.Characters.Reset();
	Session.bWorldHeaderEncrypted = false;
	FMemory::Memzero(&Session.EncryptRc4, sizeof(Session.EncryptRc4));
	FMemory::Memzero(&Session.DecryptRc4, sizeof(Session.DecryptRc4));
	Session.Realm = Session.Realms[RealmIndex];

	FString WorldHost;
	int32 WorldPort = 8085;
	if (!ParseHostPort(Session.Realm.Address, WorldHost, WorldPort))
	{
		OutError = FString::Printf(TEXT("Realm 地址无效：%s"), *Session.Realm.Address);
		return false;
	}

	Session.WorldSocket = ConnectTcp(WorldHost, WorldPort, OutError);
	if (!Session.WorldSocket)
	{
		return false;
	}

	TArray<uint8> WorldPayload;
	if (!WaitForWorldOpcode(Session, SMSG_AUTH_CHALLENGE, WorldPayload, OutError))
	{
		return false;
	}
	uint32 ServerSeed = 0;
	if (!ParseAuthChallengeWorld(WorldPayload, ServerSeed))
	{
		OutError = TEXT("WorldServer 返回了无效 SMSG_AUTH_CHALLENGE。");
		return false;
	}

	const uint32 ClientSeed = static_cast<uint32>(FPlatformTime::Cycles());
	const TArray<uint8> AuthSessionPayload = BuildAuthSessionPayload(Session, ClientSeed, ServerSeed);
	if (!SendWorldPacket(Session, CMSG_AUTH_SESSION, AuthSessionPayload, OutError))
	{
		return false;
	}
	InitWorldHeaderCrypto(Session);

	if (!WaitForWorldOpcode(Session, SMSG_AUTH_RESPONSE, WorldPayload, OutError))
	{
		return false;
	}
	FByteReader AuthResponseReader{ WorldPayload };
	const uint8 AuthResult = AuthResponseReader.U8();
	if (AuthResult != WORLD_AUTH_OK)
	{
		OutError = FString::Printf(TEXT("WorldServer 拒绝认证，SMSG_AUTH_RESPONSE=0x%02X。"), AuthResult);
		return false;
	}

	return RequestCharacterEnum(Session, OutError);
}

bool CharacterNameLooksValid(const FString& Name)
{
	return Name.Len() >= 2 && Name.Len() <= 12;
}
}

bool FWoWAzerothCoreService::AuthenticateAccount(
	const FString& AccountName,
	const FString& Password,
	FWoWAzerothAccountRecord& OutAccount,
	FString& OutError)
{
	if (AccountName.IsEmpty() || Password.IsEmpty())
	{
		OutError = TEXT("请输入账号和密码。");
		return false;
	}

	if (!ConnectAuthAndRealmList(AccountName, Password, OutError))
	{
		Disconnect();
		return false;
	}

	OutAccount.AccountId = 1;
	OutAccount.Username = ToLatinUpper(AccountName);
	return true;
}

bool FWoWAzerothCoreService::LoadRealmList(
	TArray<FWoWGlueRealmSummary>& OutRealms,
	FString& OutError)
{
	if (!GSession.AuthSocket)
	{
		OutError = TEXT("尚未连接 AuthServer。");
		return false;
	}

	OutRealms.Reset();
	for (int32 Index = 0; Index < GSession.Realms.Num(); ++Index)
	{
		const FRealmInfo& Source = GSession.Realms[Index];
		FWoWGlueRealmSummary Realm;
		Realm.Index = Index + 1;
		Realm.Name = Source.Name;
		Realm.Address = Source.Address;
		Realm.Icon = Source.Icon;
		Realm.Lock = Source.Lock;
		Realm.Flags = Source.Flags;
		Realm.Population = Source.Population;
		Realm.CharacterCount = Source.CharacterCount;
		Realm.Timezone = Source.Timezone;
		Realm.RealmId = Source.Id;
		Realm.MajorVersion = Source.MajorVersion;
		Realm.MinorVersion = Source.MinorVersion;
		Realm.PatchVersion = Source.PatchVersion;
		Realm.Build = Source.Build;
		OutRealms.Add(MoveTemp(Realm));
	}
	return true;
}

bool FWoWAzerothCoreService::ConnectToRealm(
	int32 RealmIndex,
	FString& OutError)
{
	if (!GSession.AuthSocket)
	{
		OutError = TEXT("尚未连接 AuthServer。");
		return false;
	}

	return ConnectWorldForRealm(GSession, RealmIndex - 1, OutError);
}

bool FWoWAzerothCoreService::LoadCharactersForAccount(
	int32 AccountId,
	TArray<FWoWGlueCharacterSummary>& OutCharacters,
	FString& OutError)
{
	if (!GSession.WorldSocket)
	{
		OutError = TEXT("尚未连接 WorldServer。");
		return false;
	}
	if (!RequestCharacterEnum(GSession, OutError))
	{
		return false;
	}
	OutCharacters = GSession.Characters;
	return true;
}

bool FWoWAzerothCoreService::GetCachedCharactersForAccount(
	int32 AccountId,
	TArray<FWoWGlueCharacterSummary>& OutCharacters,
	FString& OutError)
{
	if (!GSession.WorldSocket)
	{
		OutError = TEXT("尚未连接 WorldServer。");
		return false;
	}
	OutCharacters = GSession.Characters;
	return true;
}

bool FWoWAzerothCoreService::CreateCharacter(
	int32 AccountId,
	const FWoWGlueCharacterCreateRequest& Request,
	FWoWGlueCharacterSummary& OutCharacter,
	FString& OutError)
{
	if (!GSession.WorldSocket)
	{
		OutError = TEXT("尚未连接 WorldServer。");
		return false;
	}
	if (!CharacterNameLooksValid(Request.Name))
	{
		OutError = TEXT("角色名长度必须在 2 到 12 之间。");
		return false;
	}

	FByteWriter Payload;
	Payload.CString(Request.Name);
	Payload.U8(Request.Race);
	Payload.U8(Request.ClassId);
	Payload.U8(Request.Gender == 1 ? 1 : 0);
	Payload.U8(Request.Skin);
	Payload.U8(Request.Face);
	Payload.U8(Request.HairStyle);
	Payload.U8(Request.HairColor);
	Payload.U8(Request.FacialStyle);
	Payload.U8(0);

	if (!SendWorldPacket(GSession, CMSG_CHAR_CREATE, Payload.Data, OutError))
	{
		return false;
	}

	TArray<uint8> ResponsePayload;
	if (!WaitForWorldOpcode(GSession, SMSG_CHAR_CREATE, ResponsePayload, OutError))
	{
		return false;
	}
	FByteReader Reader{ ResponsePayload };
	const uint8 Result = Reader.U8();
	if (Result != CHAR_CREATE_SUCCESS && Result != CHAR_CREATE_IN_PROGRESS)
	{
		OutError = FString::Printf(TEXT("服务器拒绝创建角色，错误码：0x%02X。"), Result);
		return false;
	}

	if (!RequestCharacterEnum(GSession, OutError))
	{
		return false;
	}

	for (const FWoWGlueCharacterSummary& Character : GSession.Characters)
	{
		if (Character.Name.Equals(Request.Name, ESearchCase::IgnoreCase))
		{
			OutCharacter = Character;
			return true;
		}
	}

	OutError = TEXT("角色创建成功，但新的角色未出现在服务器角色列表中。");
	return false;
}

void FWoWAzerothCoreService::Disconnect()
{
	if (GSession.WorldSocket)
	{
		GSession.WorldSocket->Close();
		GSession.WorldSocket.Reset();
	}
	if (GSession.AuthSocket)
	{
		GSession.AuthSocket->Close();
		GSession.AuthSocket.Reset();
	}
	GSession.AuthBuffer.Reset();
	GSession.WorldBuffer.Reset();
	GSession.Characters.Reset();
	GSession.AccountName.Reset();
	GSession.SessionKey.Reset();
	GSession.Realms.Reset();
	GSession.Realm = FRealmInfo();
	FMemory::Memzero(&GSession.EncryptRc4, sizeof(GSession.EncryptRc4));
	FMemory::Memzero(&GSession.DecryptRc4, sizeof(GSession.DecryptRc4));
	GSession.bWorldHeaderEncrypted = false;
}
