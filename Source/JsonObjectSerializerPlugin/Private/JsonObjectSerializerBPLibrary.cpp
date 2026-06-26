// Copyright JsonObjectSerializer. All Rights Reserved.

#include "JsonObjectSerializerBPLibrary.h"
#include "JsonObjectSerializerPlugin.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"
#include "UObject/EnumProperty.h"
#include "UObject/Object.h"
#include "HAL/Platform.h"
#include "Containers/SharedString.h"
#include "GameFramework/Actor.h"
#include "Policies/CondensedJsonPrintPolicy.h"

// ============================================================================
// Constants
// ============================================================================

static constexpr int32 MAX_DEPTH = 64;

DEFINE_LOG_CATEGORY_STATIC(LogJsonObjSer, Warning, All);

// ============================================================================
// Forward declarations
// ============================================================================

// Serialization: maps a visited UObject to its assigned integer ID. First
// occurrence of an object is serialized fully and tagged with __ObjectId;
// subsequent occurrences (cycles / shared references) are emitted as a
// compact {"__ObjectRef": id} stub so the deserializer can rebuild the link.
static TSharedPtr<FJsonObject> SerializeObject(UObject* Object, TMap<const UObject*, int32>& Visited, int32 NextIdHolder[1], int32 Depth);
static TSharedPtr<FJsonValue>  SerializeProperty(FProperty* Prop, void* Data, TMap<const UObject*, int32>& Visited, int32 NextIdHolder[1], int32 Depth);

// Deserialization: two passes.
//   Pass 1 — create every UObject (with correct Outer chain) and register it
//            in IdMap by its __ObjectId. Properties are NOT filled yet.
//            Returns the UObject created from this JSON node (or nullptr on
//            failure). If the JSON node is a {"__ObjectRef": id} stub, returns
//            the previously-registered object from IdMap.
//   Pass 2 — walk the JSON tree again, fill properties, and resolve any
//            __ObjectRef entries against IdMap.
static UObject* DeserializeObjectPass1(const TSharedPtr<FJsonObject>& Json, UObject* Outer, TMap<int32, UObject*>& IdMap, int32 Depth);
static bool DeserializeObjectPass2(const TSharedPtr<FJsonObject>& Json, UObject* Obj, TMap<int32, UObject*>& IdMap, TSet<int32>& Filled, int32 Depth);
static bool DeserializeProperty(FProperty* Prop, void* Data, const TSharedPtr<FJsonValue>& Val, UObject* Owner, TMap<int32, UObject*>& IdMap, TSet<int32>& Filled, int32 Depth);

// ============================================================================
// Helper: convert FJsonObject Values key (UE::FSharedString in 5.8) to FString
// ============================================================================

static FORCEINLINE FString FSharedStringToFString(const UE::FSharedString& SharedStr)
{
        // UE::FSharedString has operator FStringView() which FString can construct from.
        return FString(FStringView(SharedStr));
}

// ============================================================================
// Helper: read integer / string fields from FJsonObject without relying on
// GetField/HasField (API changed in UE 5.8 — Values map uses UE::FSharedString keys)
// ============================================================================

static bool JsonTryGetStringField(const TSharedPtr<FJsonObject>& Json, const FString& FieldName, FString& OutValue)
{
        if (!Json.IsValid()) return false;
        for (const auto& Pair : Json->Values)
        {
                if (Pair.Value.IsValid() && Pair.Value->Type == EJson::String)
                {
                        if (FSharedStringToFString(Pair.Key) == FieldName)
                        {
                                OutValue = Pair.Value->AsString();
                                return true;
                        }
                }
        }
        return false;
}

static bool JsonTryGetIntegerField(const TSharedPtr<FJsonObject>& Json, const FString& FieldName, int32& OutValue)
{
        if (!Json.IsValid()) return false;
        for (const auto& Pair : Json->Values)
        {
                if (Pair.Value.IsValid() && Pair.Value->Type == EJson::Number)
                {
                        if (FSharedStringToFString(Pair.Key) == FieldName)
                        {
                                OutValue = (int32)Pair.Value->AsNumber();
                                return true;
                        }
                }
        }
        return false;
}

// ============================================================================
// Public API
// ============================================================================

void UJsonObjectSerializerBPLibrary::MakeJsonFromObject(UObject* Target, FString& JsonString, bool& Success)
{
        JsonString.Empty();
        Success = false;

        if (!IsValid(Target))
        {
                return;
        }

        TMap<const UObject*, int32> Visited;
        int32 NextIdHolder[1] = { 1 }; // IDs start at 1; 0 means "no id assigned"

        TSharedPtr<FJsonObject> Obj = SerializeObject(Target, Visited, NextIdHolder, 0);
        if (!Obj.IsValid())
        {
                return;
        }

        TSharedRef<TJsonStringWriter<TCondensedJsonPrintPolicy<TCHAR>>> Writer =
                TJsonStringWriter<TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonString);

        Success = FJsonSerializer::Serialize(Obj, *Writer, true);
}

void UJsonObjectSerializerBPLibrary::SpawnObjectFromJson(UObject* Outer, const FString& InJsonString, UObject*& SpawnedObject, bool& Success)
{
        SpawnedObject = nullptr;
        Success = false;

        if (InJsonString.IsEmpty())
        {
                return;
        }

        TSharedPtr<FJsonObject> JsonObj;
        TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(InJsonString);

        if (!FJsonSerializer::Deserialize(Reader, JsonObj) || !JsonObj.IsValid())
        {
                UE_LOG(LogJsonObjSer, Error, TEXT("Failed to parse JSON string"));
                return;
        }

        if (!IsValid(Outer))
        {
                Outer = GetTransientPackageAsObject();
        }

        TMap<int32, UObject*> IdMap;

        // Pass 1: create the root object and every nested UObject, register by __ObjectId
        UObject* RootObj = DeserializeObjectPass1(JsonObj, Outer, IdMap, 0);
        if (!RootObj)
        {
                UE_LOG(LogJsonObjSer, Error, TEXT("SpawnObjectFromJson: Pass 1 (create objects) failed"));
                return;
        }

        // Pass 2: fill properties of every object (resolve __ObjectRef against IdMap)
        TSet<int32> Filled;
        if (!DeserializeObjectPass2(JsonObj, RootObj, IdMap, Filled, 0))
        {
                UE_LOG(LogJsonObjSer, Warning, TEXT("SpawnObjectFromJson: Pass 2 (fill properties) reported errors, but object was created"));
        }

        SpawnedObject = RootObj;
        Success = true;

        UE_LOG(LogJsonObjSer, Log, TEXT("SpawnObjectFromJson result: Success=%s, SpawnedObject=%s, IdMap size=%d"),
                Success ? TEXT("true") : TEXT("false"),
                SpawnedObject ? *SpawnedObject->GetName() : TEXT("nullptr"),
                IdMap.Num());
}

// ============================================================================
// Serialization
// ============================================================================

static TSharedPtr<FJsonObject> SerializeObject(UObject* Object, TMap<const UObject*, int32>& Visited, int32 NextIdHolder[1], int32 Depth)
{
        if (!IsValid(Object) || Depth > MAX_DEPTH)
        {
                return nullptr;
        }

        // If we already serialized this object, emit a __ObjectRef stub instead of null.
        // This preserves array structure AND restores the shared / cyclic reference on load.
        if (const int32* ExistingId = Visited.Find(Object))
        {
                TSharedPtr<FJsonObject> RefStub = MakeShared<FJsonObject>();
                RefStub->SetNumberField(TEXT("__ObjectRef"), (double)(*ExistingId));
                return RefStub;
        }

        // Assign a new id and register before recursing, so cycles resolve correctly.
        const int32 ThisId = NextIdHolder[0]++;
        Visited.Add(Object, ThisId);

        TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
        JsonObj->SetStringField(TEXT("__ObjectClassPath"), Object->GetClass()->GetPathName());
        JsonObj->SetNumberField(TEXT("__ObjectId"), (double)ThisId);

        for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
        {
                FProperty* Prop = *It;
                if (Prop->HasAnyPropertyFlags(CPF_Transient))
                {
                        continue;
                }

                void* Data = Prop->ContainerPtrToValuePtr<void>(Object);
                if (!Data)
                {
                        continue;
                }

                TSharedPtr<FJsonValue> Val = SerializeProperty(Prop, Data, Visited, NextIdHolder, Depth + 1);
                if (Val.IsValid())
                {
                        JsonObj->SetField(Prop->GetName(), Val);
                }
        }

        return JsonObj;
}

static TSharedPtr<FJsonValue> SerializeProperty(FProperty* Prop, void* Data, TMap<const UObject*, int32>& Visited, int32 NextIdHolder[1], int32 Depth)
{
        if (!Prop || !Data || Depth > MAX_DEPTH)
        {
                return MakeShared<FJsonValueNull>();
        }

        // Boolean
        if (FBoolProperty* P = CastField<FBoolProperty>(Prop))
        {
                return MakeShared<FJsonValueBoolean>(P->GetPropertyValue(Data));
        }
        // Float
        if (FFloatProperty* P = CastField<FFloatProperty>(Prop))
        {
                return MakeShared<FJsonValueNumber>((double)P->GetPropertyValue(Data));
        }
        // Double
        if (FDoubleProperty* P = CastField<FDoubleProperty>(Prop))
        {
                return MakeShared<FJsonValueNumber>(P->GetPropertyValue(Data));
        }
        // Int32
        if (FIntProperty* P = CastField<FIntProperty>(Prop))
        {
                return MakeShared<FJsonValueNumber>((double)P->GetPropertyValue(Data));
        }
        // Int64
        if (FInt64Property* P = CastField<FInt64Property>(Prop))
        {
                return MakeShared<FJsonValueNumber>((double)P->GetPropertyValue(Data));
        }
        // Byte (uint8) - includes byte enums
        if (FByteProperty* P = CastField<FByteProperty>(Prop))
        {
                return MakeShared<FJsonValueNumber>((double)P->GetPropertyValue(Data));
        }
        // Enum (non-byte, backed by numeric)
        if (FEnumProperty* P = CastField<FEnumProperty>(Prop))
        {
                int64 Val = P->GetUnderlyingProperty()->GetSignedIntPropertyValue(Data);
                return MakeShared<FJsonValueNumber>((double)Val);
        }
        // Generic numeric (covers uint16, uint32, uint64 which were removed in UE 5.4+)
        if (FNumericProperty* P = CastField<FNumericProperty>(Prop))
        {
                if (P->IsInteger())
                {
                        return MakeShared<FJsonValueNumber>((double)P->GetSignedIntPropertyValue(Data));
                }
                return MakeShared<FJsonValueNumber>(P->GetFloatingPointPropertyValue(Data));
        }
        // FString — if large enough, try to embed pre-serialized JSON as a real JSON
        // value instead of a double-escaped string (prevents growth like NeedsJsonString).
        static constexpr int32 MAX_PARSE_JSON_LEN = 50 * 1024 * 1024; // don't try to parse strings over 50 MB
        if (FStrProperty* P = CastField<FStrProperty>(Prop))
        {
                const FString& Val = P->GetPropertyValue(Data);

                if (Val.Len() > 4096 && Val.Len() <= MAX_PARSE_JSON_LEN)
                {
                        TSharedPtr<FJsonValue> Parsed;
                        TSharedRef<TJsonReader<TCHAR>> Reader = TJsonReaderFactory<TCHAR>::Create(Val);
                        if (FJsonSerializer::Deserialize(Reader, Parsed) && Parsed.IsValid())
                        {
                                TSharedPtr<FJsonObject> Wrapper = MakeShared<FJsonObject>();
                                Wrapper->SetField(TEXT("__StringJson"), Parsed);
                                return MakeShared<FJsonValueObject>(Wrapper);
                        }
                }

                if (Val.Len() > 10 * 1024 * 1024)
                {
                        UE_LOG(LogJsonObjSer, Warning, TEXT("SerializeProperty: Truncating FString '%s' (len=%lld) to 10 MB"), *Prop->GetName(), (int64)Val.Len());
                        return MakeShared<FJsonValueString>(Val.Left(10 * 1024 * 1024));
                }
                return MakeShared<FJsonValueString>(Val);
        }
        // FName
        if (FNameProperty* P = CastField<FNameProperty>(Prop))
        {
                const FString Val = P->GetPropertyValue(Data).ToString();
                if (Val.Len() > 10 * 1024 * 1024)
                {
                        UE_LOG(LogJsonObjSer, Warning, TEXT("SerializeProperty: Truncating FName '%s' (len=%lld) to 10 MB"), *Prop->GetName(), (int64)Val.Len());
                        return MakeShared<FJsonValueString>(Val.Left(10 * 1024 * 1024));
                }
                return MakeShared<FJsonValueString>(Val);
        }
        // FText
        if (FTextProperty* P = CastField<FTextProperty>(Prop))
        {
                const FString Val = P->GetPropertyValue(Data).ToString();
                if (Val.Len() > 10 * 1024 * 1024)
                {
                        UE_LOG(LogJsonObjSer, Warning, TEXT("SerializeProperty: Truncating FText '%s' (len=%lld) to 10 MB"), *Prop->GetName(), (int64)Val.Len());
                        return MakeShared<FJsonValueString>(Val.Left(10 * 1024 * 1024));
                }
                return MakeShared<FJsonValueString>(Val);
        }
        // UObject* - recursive
        if (FObjectProperty* P = CastField<FObjectProperty>(Prop))
        {
                UObject* SubObj = P->GetPropertyValue(Data);
                if (!IsValid(SubObj) || SubObj->HasAnyFlags(RF_Transient))
                {
                        return MakeShared<FJsonValueNull>();
                }
                // Actors cannot be recreated via NewObject — skip them entirely.
                if (SubObj->IsA(AActor::StaticClass()))
                {
                        return MakeShared<FJsonValueNull>();
                }
                // SerializeObject will emit a __ObjectRef stub if SubObj was already visited,
                // which is exactly what we want for shared / cyclic references.
                TSharedPtr<FJsonObject> SubJson = SerializeObject(SubObj, Visited, NextIdHolder, Depth);
                if (SubJson.IsValid())
                {
                        return MakeShared<FJsonValueObject>(SubJson);
                }
                return MakeShared<FJsonValueNull>();
        }
        // TArray — clamp to 1M elements to prevent crash on corrupted data
        if (FArrayProperty* P = CastField<FArrayProperty>(Prop))
        {
                FScriptArrayHelper Arr(P, Data);
                int32 Num = Arr.Num();
                if (Num > 1000000)
                {
                        UE_LOG(LogJsonObjSer, Warning, TEXT("SerializeProperty: Truncating TArray '%s' (num=%d) to 1M elements"), *Prop->GetName(), Num);
                        Num = 1000000;
                }
                TArray<TSharedPtr<FJsonValue>> Items;
                for (int32 i = 0; i < Num; ++i)
                {
                        void* Elem = Arr.GetRawPtr(i);
                        TSharedPtr<FJsonValue> Item = SerializeProperty(P->Inner, Elem, Visited, NextIdHolder, Depth);
                        Items.Add(Item.IsValid() ? Item : MakeShared<FJsonValueNull>());
                }
                return MakeShared<FJsonValueArray>(Items);
        }

        // Unsupported - skip
        return nullptr;
}

// ============================================================================
// Deserialization — Pass 1: create all UObjects, register by __ObjectId
// ============================================================================

// Helper: create a fresh UObject from a JSON object's __ObjectClassPath, using
// the supplied Outer. Does NOT fill any properties. Returns nullptr on failure.
static UObject* CreateBlankObjectFromJson(const TSharedPtr<FJsonObject>& Json, UObject* Outer)
{
        FString ClassPath;
        if (!JsonTryGetStringField(Json, TEXT("__ObjectClassPath"), ClassPath) || ClassPath.IsEmpty())
        {
                UE_LOG(LogJsonObjSer, Error, TEXT("Pass1: __ObjectClassPath missing or empty"));
                return nullptr;
        }

        UClass* Cls = LoadClass<UObject>(nullptr, *ClassPath, nullptr, LOAD_Quiet | LOAD_NoWarn);
        if (!Cls)
        {
                UE_LOG(LogJsonObjSer, Error, TEXT("Pass1: LoadClass FAILED for '%s'"), *ClassPath);
                return nullptr;
        }

        if (Cls->IsChildOf(AActor::StaticClass()))
        {
                UE_LOG(LogJsonObjSer, Error, TEXT("Pass1: Class '%s' is an Actor — actors cannot be created via NewObject"), *ClassPath);
                return nullptr;
        }

        UObject* Obj = NewObject<UObject>(Outer, Cls);
        if (!Obj)
        {
                UE_LOG(LogJsonObjSer, Error, TEXT("Pass1: NewObject FAILED for class '%s'"), *ClassPath);
                return nullptr;
        }

        UE_LOG(LogJsonObjSer, Verbose, TEXT("Pass1: Created '%s' (class=%s, outer=%s)"),
                *Obj->GetName(), *Cls->GetName(), Outer ? *Outer->GetName() : TEXT("null"));
        return Obj;
}

// Helper: does this JSON object represent a reference ({"__ObjectRef": id})?
static bool JsonIsObjectRef(const TSharedPtr<FJsonObject>& Json, int32& OutRefId)
{
        OutRefId = 0;
        return JsonTryGetIntegerField(Json, TEXT("__ObjectRef"), OutRefId) && OutRefId > 0;
}

// Walk a single FJsonValue, recursing into any embedded JSON objects (in
// FObjectProperty slots or TArray<FObjectProperty> elements), creating them
// and registering in IdMap. Primitives are ignored in Pass 1.
static void Pass1_WalkValue(const TSharedPtr<FJsonValue>& Val, FProperty* Prop, UObject* Owner, TMap<int32, UObject*>& IdMap, int32 Depth)
{
        if (!Val.IsValid() || !Prop || Depth > MAX_DEPTH) return;

        if (FObjectProperty* ObjProp = CastField<FObjectProperty>(Prop))
        {
                if (Val->Type != EJson::Object) return;
                const TSharedPtr<FJsonObject> SubJson = Val->AsObject();
                if (!SubJson.IsValid()) return;

                int32 RefId = 0;
                if (JsonIsObjectRef(SubJson, RefId)) return; // references don't need new objects

                DeserializeObjectPass1(SubJson, Owner, IdMap, Depth + 1);
        }
        else if (FArrayProperty* ArrProp = CastField<FArrayProperty>(Prop))
        {
                if (Val->Type != EJson::Array) return;
                const TArray<TSharedPtr<FJsonValue>>& Items = Val->AsArray();
                for (const TSharedPtr<FJsonValue>& Item : Items)
                {
                        Pass1_WalkValue(Item, ArrProp->Inner, Owner, IdMap, Depth + 1);
                }
        }
        // Other property kinds (primitives, maps, sets) are not recursed in Pass 1.
}

static UObject* DeserializeObjectPass1(const TSharedPtr<FJsonObject>& Json, UObject* Outer, TMap<int32, UObject*>& IdMap, int32 Depth)
{
        if (!Json.IsValid() || Depth > MAX_DEPTH)
        {
                UE_LOG(LogJsonObjSer, Error, TEXT("Pass1: Invalid JSON or max depth reached (depth=%d)"), Depth);
                return nullptr;
        }

        // References don't create objects in Pass 1 — return the already-created target.
        int32 RefId = 0;
        if (JsonIsObjectRef(Json, RefId))
        {
                UObject** Found = IdMap.Find(RefId);
                return Found ? *Found : nullptr;
        }

        // If this object was already created (e.g., shared reference first encountered
        // elsewhere), return the existing instance.
        int32 ExistingId = 0;
        if (JsonTryGetIntegerField(Json, TEXT("__ObjectId"), ExistingId) && ExistingId > 0)
        {
                if (UObject** Found = IdMap.Find(ExistingId))
                {
                        return *Found;
                }
        }

        UObject* Obj = CreateBlankObjectFromJson(Json, Outer);
        if (!Obj)
        {
                return nullptr;
        }

        // Register by __ObjectId so Pass 2 can resolve references.
        // If __ObjectId is absent (legacy JSON), the object still exists but cannot
        // be referenced via __ObjectRef — this matches the previous behavior.
        if (ExistingId > 0)
        {
                IdMap.Add(ExistingId, Obj);
        }

        // Build a property lookup map for this object's class
        TMap<FName, FProperty*> PropMap;
        for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
        {
                FProperty* Prop = *It;
                if (!Prop->HasAnyPropertyFlags(CPF_Transient))
                {
                        PropMap.Add(Prop->GetFName(), Prop);
                }
        }

        // Recurse into properties that may contain nested UObjects
        for (const auto& Pair : Json->Values)
        {
                FString KeyStr = FSharedStringToFString(Pair.Key);
                if (KeyStr == TEXT("__ObjectClassPath") || KeyStr == TEXT("__ObjectId") || KeyStr == TEXT("__ObjectRef"))
                {
                        continue;
                }

                FProperty** PropPtr = PropMap.Find(FName(*KeyStr));
                if (!PropPtr) continue;

                Pass1_WalkValue(Pair.Value, *PropPtr, Obj, IdMap, Depth + 1);
        }

        return Obj;
}

// ============================================================================
// Deserialization — Pass 2: fill properties, resolve __ObjectRef
// ============================================================================

static bool DeserializeObjectPass2(const TSharedPtr<FJsonObject>& Json, UObject* Obj, TMap<int32, UObject*>& IdMap, TSet<int32>& Filled, int32 Depth)
{
        if (!Json.IsValid() || !Obj || Depth > MAX_DEPTH)
        {
                return false;
        }

        // Skip if this object was already filled (shared reference resolved earlier)
        int32 ThisId = 0;
        JsonTryGetIntegerField(Json, TEXT("__ObjectId"), ThisId);
        if (ThisId > 0 && Filled.Contains(ThisId))
        {
                return true;
        }
        if (ThisId > 0)
        {
                Filled.Add(ThisId);
        }

        // Build a property lookup map
        TMap<FName, FProperty*> PropMap;
        for (TFieldIterator<FProperty> It(Obj->GetClass()); It; ++It)
        {
                FProperty* Prop = *It;
                if (!Prop->HasAnyPropertyFlags(CPF_Transient))
                {
                        PropMap.Add(Prop->GetFName(), Prop);
                }
        }

        bool bAllOk = true;

        for (const auto& Pair : Json->Values)
        {
                FString KeyStr = FSharedStringToFString(Pair.Key);
                if (KeyStr == TEXT("__ObjectClassPath") || KeyStr == TEXT("__ObjectId") || KeyStr == TEXT("__ObjectRef"))
                {
                        continue;
                }

                FProperty** PropPtr = PropMap.Find(FName(*KeyStr));
                if (!PropPtr)
                {
                        UE_LOG(LogJsonObjSer, Verbose, TEXT("Pass2: JSON key '%s' not found in properties of '%s'"),
                                *KeyStr, *Obj->GetClass()->GetName());
                        continue;
                }
                if (!Pair.Value.IsValid())
                {
                        continue;
                }

                FProperty* Prop = *PropPtr;
                void* Data = Prop->ContainerPtrToValuePtr<void>(Obj);
                if (!Data) continue;

                if (!DeserializeProperty(Prop, Data, Pair.Value, Obj, IdMap, Filled, Depth + 1))
                {
                        UE_LOG(LogJsonObjSer, Warning, TEXT("Pass2: Failed to set property '%s' (type=%s) on '%s'"),
                                *KeyStr, *Prop->GetClass()->GetName(), *Obj->GetName());
                        bAllOk = false;
                }
        }

        return bAllOk;
}

static bool DeserializeProperty(FProperty* Prop, void* Data, const TSharedPtr<FJsonValue>& Val, UObject* Owner, TMap<int32, UObject*>& IdMap, TSet<int32>& Filled, int32 Depth)
{
        if (!Prop || !Data || !Val.IsValid() || Depth > MAX_DEPTH)
        {
                return false;
        }

        EJson Type = Val->Type;

        // Boolean
        if (FBoolProperty* P = CastField<FBoolProperty>(Prop))
        {
                if (Type == EJson::Boolean || Type == EJson::Number)
                {
                        P->SetPropertyValue(Data, Val->AsBool());
                        return true;
                }
                return false;
        }
        // Float
        if (FFloatProperty* P = CastField<FFloatProperty>(Prop))
        {
                if (Type == EJson::Number)
                {
                        P->SetPropertyValue(Data, (float)Val->AsNumber());
                        return true;
                }
                return false;
        }
        // Double
        if (FDoubleProperty* P = CastField<FDoubleProperty>(Prop))
        {
                if (Type == EJson::Number)
                {
                        P->SetPropertyValue(Data, Val->AsNumber());
                        return true;
                }
                return false;
        }
        // Int32
        if (FIntProperty* P = CastField<FIntProperty>(Prop))
        {
                if (Type == EJson::Number)
                {
                        P->SetPropertyValue(Data, (int32)Val->AsNumber());
                        return true;
                }
                return false;
        }
        // Int64
        if (FInt64Property* P = CastField<FInt64Property>(Prop))
        {
                if (Type == EJson::Number)
                {
                        P->SetPropertyValue(Data, (int64)Val->AsNumber());
                        return true;
                }
                return false;
        }
        // Byte (uint8)
        if (FByteProperty* P = CastField<FByteProperty>(Prop))
        {
                if (Type == EJson::Number)
                {
                        P->SetPropertyValue(Data, (uint8)Val->AsNumber());
                        return true;
                }
                return false;
        }
        // Enum (non-byte)
        if (FEnumProperty* P = CastField<FEnumProperty>(Prop))
        {
                if (Type == EJson::Number)
                {
                        P->GetUnderlyingProperty()->SetIntPropertyValue(Data, (int64)Val->AsNumber());
                        return true;
                }
                return false;
        }
        // Generic numeric (covers uint16, uint32, uint64 - removed types in UE 5.4+)
        if (FNumericProperty* P = CastField<FNumericProperty>(Prop))
        {
                if (Type == EJson::Number)
                {
                        if (P->IsInteger())
                        {
                                P->SetIntPropertyValue(Data, (int64)Val->AsNumber());
                        }
                        else
                        {
                                P->SetFloatingPointPropertyValue(Data, Val->AsNumber());
                        }
                        return true;
                }
                return false;
        }
        // FString
        if (FStrProperty* P = CastField<FStrProperty>(Prop))
        {
                if (Type == EJson::String)
                {
                        P->SetPropertyValue(Data, Val->AsString());
                        return true;
                }
                if (Type == EJson::Object)
                {
                        TSharedPtr<FJsonObject> Obj = Val->AsObject();
                        if (Obj.IsValid())
                        {
                                TSharedPtr<FJsonValue> Inner;
                                for (const auto& KVP : Obj->Values)
                                {
                                        if (KVP.Value.IsValid() && FSharedStringToFString(KVP.Key) == TEXT("__StringJson"))
                                        {
                                                Inner = KVP.Value;
                                                break;
                                        }
                                }
                                if (Inner.IsValid())
                                {
                                        FString JsonStr;
                                        TSharedRef<TJsonStringWriter<TCondensedJsonPrintPolicy<TCHAR>>> StrWriter =
                                                TJsonStringWriter<TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonStr);
                                        if (Inner->Type == EJson::Object)
                                        {
                                                FJsonSerializer::Serialize(Inner->AsObject(), *StrWriter, true);
                                        }
                                        else if (Inner->Type == EJson::Array)
                                        {
                                                FJsonSerializer::Serialize(Inner->AsArray(), *StrWriter, true);
                                        }
                                        else
                                        {
                                                TArray<TSharedPtr<FJsonValue>> Wrapper;
                                                Wrapper.Add(Inner);
                                                FJsonSerializer::Serialize(Wrapper, *StrWriter, true);
                                                if (JsonStr.Len() >= 2) JsonStr = JsonStr.Mid(1, JsonStr.Len() - 2);
                                        }
                                        P->SetPropertyValue(Data, JsonStr);
                                        return true;
                                }
                        }
                        return false;
                }
                return false;
        }
        // FName
        if (FNameProperty* P = CastField<FNameProperty>(Prop))
        {
                if (Type == EJson::String)
                {
                        P->SetPropertyValue(Data, FName(*Val->AsString()));
                        return true;
                }
                return false;
        }
        // FText
        if (FTextProperty* P = CastField<FTextProperty>(Prop))
        {
                if (Type == EJson::String)
                {
                        P->SetPropertyValue(Data, FText::FromString(Val->AsString()));
                        return true;
                }
                return false;
        }

        // UObject*
        if (FObjectProperty* P = CastField<FObjectProperty>(Prop))
        {
                if (Type == EJson::Null)
                {
                        P->SetPropertyValue(Data, nullptr);
                        return true;
                }
                if (Type == EJson::Object)
                {
                        const TSharedPtr<FJsonObject> SubJson = Val->AsObject();
                        if (!SubJson.IsValid())
                        {
                                P->SetPropertyValue(Data, nullptr);
                                return true;
                        }

                        // Case 1: {"__ObjectRef": id} — resolve against IdMap
                        int32 RefId = 0;
                        if (JsonIsObjectRef(SubJson, RefId))
                        {
                                UObject** Found = IdMap.Find(RefId);
                                if (Found)
                                {
                                        *(UObject**)Data = *Found;
                                        UE_LOG(LogJsonObjSer, Verbose, TEXT("Pass2: Resolved __ObjectRef %d -> '%s'"),
                                                RefId, *Found ? *(*Found)->GetName() : TEXT("null"));
                                        return true;
                                }
                                UE_LOG(LogJsonObjSer, Error, TEXT("Pass2: __ObjectRef %d not found in IdMap (property '%s')"),
                                        RefId, *Prop->GetName());
                                *(UObject**)Data = nullptr;
                                return false;
                        }

                        // Case 2: object with __ObjectId — look up the already-created UObject
                        int32 SubId = 0;
                        JsonTryGetIntegerField(SubJson, TEXT("__ObjectId"), SubId);
                        if (SubId > 0)
                        {
                                UObject** Found = IdMap.Find(SubId);
                                if (Found)
                                {
                                        UObject* SubObj = *Found;
                                        *(UObject**)Data = SubObj;
                                        // Recurse to fill this sub-object's properties (Pass 2)
                                        if (SubObj)
                                        {
                                                DeserializeObjectPass2(SubJson, SubObj, IdMap, Filled, Depth + 1);
                                        }
                                        return true;
                                }
                                // Not in map — Pass 1 failed for this object. Try to create inline as fallback.
                                UE_LOG(LogJsonObjSer, Warning, TEXT("Pass2: __ObjectId %d missing from IdMap, attempting inline creation"), SubId);
                        }

                        // Case 3: legacy JSON (no __ObjectId) — create inline, no cycle support
                        FString SubClassPath;
                        if (JsonTryGetStringField(SubJson, TEXT("__ObjectClassPath"), SubClassPath) && !SubClassPath.IsEmpty())
                        {
                                UClass* Cls = LoadClass<UObject>(nullptr, *SubClassPath, nullptr, LOAD_Quiet | LOAD_NoWarn);
                                if (Cls && !Cls->IsChildOf(AActor::StaticClass()))
                                {
                                        UObject* SubObj = NewObject<UObject>(Owner, Cls);
                                        if (SubObj)
                                        {
                                                if (SubId > 0) IdMap.Add(SubId, SubObj);
                                                *(UObject**)Data = SubObj;
                                                DeserializeObjectPass2(SubJson, SubObj, IdMap, Filled, Depth + 1);
                                                return true;
                                        }
                                }
                        }

                        UE_LOG(LogJsonObjSer, Error, TEXT("Pass2: Could not resolve sub-object for property '%s'"), *Prop->GetName());
                        *(UObject**)Data = nullptr;
                        return false;
                }
                return false;
        }

        // TArray
        if (FArrayProperty* P = CastField<FArrayProperty>(Prop))
        {
                if (Type != EJson::Array)
                {
                        UE_LOG(LogJsonObjSer, Warning, TEXT("Pass2: TArray '%s' expected Array but got %d"),
                                *Prop->GetName(), (int32)Type);
                        return false;
                }
                const TArray<TSharedPtr<FJsonValue>>& Items = Val->AsArray();
                FScriptArrayHelper Arr(P, Data);

                UE_LOG(LogJsonObjSer, Verbose, TEXT("Pass2: TArray '%s' has %d JSON elements, Inner=%s"),
                        *Prop->GetName(), Items.Num(), *P->Inner->GetClass()->GetName());

                // Destroy existing elements properly, then resize to 0
                Arr.Resize(0);
                // Resize to target size — FScriptArrayHelper::Resize will construct new
                // elements through the property in UE 5.8, but to be safe across versions
                // we explicitly initialize each slot below.
                Arr.Resize(Items.Num());

                bool bAllOk = true;
                for (int32 i = 0; i < Items.Num(); ++i)
                {
                        void* Elem = Arr.GetRawPtr(i);
                        if (!Elem)
                        {
                                UE_LOG(LogJsonObjSer, Error, TEXT("Pass2: TArray '%s' element [%d] GetRawPtr returned null!"),
                                        *Prop->GetName(), i);
                                bAllOk = false;
                                continue;
                        }

                        // Re-initialize the slot through the property (handles FString, nested
                        // arrays, etc. correctly). For UObject* this is equivalent to zeroing.
                        // Note: Elem already points directly at the element, so we use
                        // InitializeValue (NOT InitializeValueInContainer, which expects a
                        // container and is not part of FProperty's public API in UE 5.8).
                        P->Inner->InitializeValue(Elem);

                        const TSharedPtr<FJsonValue>& ItemVal = Items[i];

                        // Special handling for TArray<UObject*> — write pointer directly
                        if (FObjectProperty* InnerObjProp = CastField<FObjectProperty>(P->Inner))
                        {
                                if (!ItemVal.IsValid() || ItemVal->Type == EJson::Null)
                                {
                                        // Slot already initialized to nullptr by InitializeValue
                                        continue;
                                }
                                if (ItemVal->Type == EJson::Object)
                                {
                                        const TSharedPtr<FJsonObject> SubJson = ItemVal->AsObject();
                                        if (SubJson.IsValid())
                                        {
                                                int32 RefId = 0;
                                                if (JsonIsObjectRef(SubJson, RefId))
                                                {
                                                        // Resolve reference
                                                        UObject** Found = IdMap.Find(RefId);
                                                        if (Found)
                                                        {
                                                                *(UObject**)Elem = *Found;
                                                                UE_LOG(LogJsonObjSer, Verbose, TEXT("Pass2: TArray '%s'[%d] -> __ObjectRef %d = '%s'"),
                                                                        *Prop->GetName(), i, RefId, *Found ? *(*Found)->GetName() : TEXT("null"));
                                                        }
                                                        else
                                                        {
                                                                UE_LOG(LogJsonObjSer, Error, TEXT("Pass2: TArray '%s'[%d] __ObjectRef %d not found"),
                                                                        *Prop->GetName(), i, RefId);
                                                                bAllOk = false;
                                                        }
                                                        continue;
                                                }

                                                int32 SubId = 0;
                                                JsonTryGetIntegerField(SubJson, TEXT("__ObjectId"), SubId);
                                                if (SubId > 0)
                                                {
                                                        UObject** Found = IdMap.Find(SubId);
                                                        if (Found)
                                                        {
                                                                UObject* SubObj = *Found;
                                                                *(UObject**)Elem = SubObj;
                                                                if (SubObj)
                                                                {
                                                                        DeserializeObjectPass2(SubJson, SubObj, IdMap, Filled, Depth + 1);
                                                                }
                                                                UE_LOG(LogJsonObjSer, Verbose, TEXT("Pass2: TArray '%s'[%d] -> __ObjectId %d = '%s'"),
                                                                        *Prop->GetName(), i, SubId, SubObj ? *SubObj->GetName() : TEXT("null"));
                                                                continue;
                                                        }
                                                        // Fallback: inline creation (legacy JSON or Pass 1 missed it)
                                                        UE_LOG(LogJsonObjSer, Warning, TEXT("Pass2: TArray '%s'[%d] __ObjectId %d missing — inline create"),
                                                                *Prop->GetName(), i, SubId);
                                                }

                                                // Legacy JSON (no __ObjectId) — create inline
                                                FString SubClassPath;
                                                if (JsonTryGetStringField(SubJson, TEXT("__ObjectClassPath"), SubClassPath) && !SubClassPath.IsEmpty())
                                                {
                                                        UClass* Cls = LoadClass<UObject>(nullptr, *SubClassPath, nullptr, LOAD_Quiet | LOAD_NoWarn);
                                                        if (Cls && !Cls->IsChildOf(AActor::StaticClass()))
                                                        {
                                                                UObject* SubObj = NewObject<UObject>(Owner, Cls);
                                                                if (SubObj)
                                                                {
                                                                        if (SubId > 0) IdMap.Add(SubId, SubObj);
                                                                        *(UObject**)Elem = SubObj;
                                                                        DeserializeObjectPass2(SubJson, SubObj, IdMap, Filled, Depth + 1);
                                                                        UE_LOG(LogJsonObjSer, Verbose, TEXT("Pass2: TArray '%s'[%d] inline-created '%s'"),
                                                                                *Prop->GetName(), i, *SubObj->GetName());
                                                                        continue;
                                                                }
                                                        }
                                                }

                                                UE_LOG(LogJsonObjSer, Error, TEXT("Pass2: TArray '%s'[%d] failed to deserialize sub-object"),
                                                        *Prop->GetName(), i);
                                                bAllOk = false;
                                        }
                                }
                                else
                                {
                                        UE_LOG(LogJsonObjSer, Warning, TEXT("Pass2: TArray '%s' element [%d] unexpected type %d for UObject*"),
                                                *Prop->GetName(), i, (int32)ItemVal->Type);
                                        bAllOk = false;
                                }
                        }
                        else
                        {
                                // For non-UObject* array elements (int, float, string, nested arrays, etc.)
                                if (!DeserializeProperty(P->Inner, Elem, ItemVal, Owner, IdMap, Filled, Depth + 1))
                                {
                                        UE_LOG(LogJsonObjSer, Warning, TEXT("Pass2: Failed to deserialize array element [%d] of '%s'"),
                                                i, *Prop->GetName());
                                        bAllOk = false;
                                }
                        }
                }

                UE_LOG(LogJsonObjSer, Verbose, TEXT("Pass2: TArray '%s' final count=%d"),
                        *Prop->GetName(), Arr.Num());

                return bAllOk;
        }

        UE_LOG(LogJsonObjSer, Warning, TEXT("Pass2: Unsupported property type '%s' for property '%s'"),
                *Prop->GetClass()->GetName(), *Prop->GetName());
        return false;
}
