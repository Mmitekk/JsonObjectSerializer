// Copyright JsonObjectSerializer. All Rights Reserved.

#include "JsonObjectSerializerBPLibrary.h"
#include "JsonObjectSerializerPlugin.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"
#include "UObject/EnumProperty.h"
#include "Containers/SharedString.h"

// ============================================================================
// Constants
// ============================================================================

static constexpr int32 MAX_DEPTH = 32;

DEFINE_LOG_CATEGORY_STATIC(LogJsonObjSer, Warning, All);

// ============================================================================
// Forward declarations
// ============================================================================

static TSharedPtr<FJsonObject> SerializeObject(UObject* Object, TSet<const UObject*>& Visited, int32 Depth);
static TSharedPtr<FJsonValue>  SerializeProperty(FProperty* Prop, void* Data, TSet<const UObject*>& Visited, int32 Depth);
static bool DeserializeObject(const TSharedPtr<FJsonObject>& Json, UObject* Outer, UObject*& OutObj, int32 Depth);
static bool DeserializeProperty(FProperty* Prop, void* Data, const TSharedPtr<FJsonValue>& Val, UObject* Owner, int32 Depth);

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

        TSet<const UObject*> Visited;
        TSharedPtr<FJsonObject> Obj = SerializeObject(Target, Visited, 0);
        if (!Obj.IsValid())
        {
                return;
        }

        TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
                TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&JsonString);

        Success = FJsonSerializer::Serialize(Obj.ToSharedRef(), Writer);
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
                Outer = GetTransientPackage();
        }

        Success = DeserializeObject(JsonObj, Outer, SpawnedObject, 0);
        UE_LOG(LogJsonObjSer, Log, TEXT("SpawnObjectFromJson result: Success=%s, SpawnedObject=%s"),
                Success ? TEXT("true") : TEXT("false"),
                SpawnedObject ? *SpawnedObject->GetName() : TEXT("nullptr"));
}

// ============================================================================
// Serialization
// ============================================================================

static TSharedPtr<FJsonObject> SerializeObject(UObject* Object, TSet<const UObject*>& Visited, int32 Depth)
{
        if (!IsValid(Object) || Depth > MAX_DEPTH)
        {
                return nullptr;
        }

        if (Visited.Contains(Object))
        {
                return nullptr;
        }
        Visited.Add(Object);

        TSharedPtr<FJsonObject> JsonObj = MakeShared<FJsonObject>();
        JsonObj->SetStringField(TEXT("__ObjectClassPath"), Object->GetClass()->GetPathName());

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

                TSharedPtr<FJsonValue> Val = SerializeProperty(Prop, Data, Visited, Depth + 1);
                if (Val.IsValid())
                {
                        JsonObj->SetField(Prop->GetName(), Val);
                }
        }

        return JsonObj;
}

static TSharedPtr<FJsonValue> SerializeProperty(FProperty* Prop, void* Data, TSet<const UObject*>& Visited, int32 Depth)
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
        // FString
        if (FStrProperty* P = CastField<FStrProperty>(Prop))
        {
                return MakeShared<FJsonValueString>(P->GetPropertyValue(Data));
        }
        // FName
        if (FNameProperty* P = CastField<FNameProperty>(Prop))
        {
                return MakeShared<FJsonValueString>(P->GetPropertyValue(Data).ToString());
        }
        // FText
        if (FTextProperty* P = CastField<FTextProperty>(Prop))
        {
                return MakeShared<FJsonValueString>(P->GetPropertyValue(Data).ToString());
        }
        // UObject* - recursive
        if (FObjectProperty* P = CastField<FObjectProperty>(Prop))
        {
                UObject* SubObj = P->GetPropertyValue(Data);
                if (!IsValid(SubObj) || SubObj->HasAnyFlags(RF_Transient))
                {
                        return MakeShared<FJsonValueNull>();
                }
                TSharedPtr<FJsonObject> SubJson = SerializeObject(SubObj, Visited, Depth);
                if (SubJson.IsValid())
                {
                        return MakeShared<FJsonValueObject>(SubJson);
                }
                return MakeShared<FJsonValueNull>();
        }
        // TArray - recursive
        if (FArrayProperty* P = CastField<FArrayProperty>(Prop))
        {
                FScriptArrayHelper Arr(P, Data);
                TArray<TSharedPtr<FJsonValue>> Items;
                for (int32 i = 0; i < Arr.Num(); ++i)
                {
                        void* Elem = Arr.GetRawPtr(i);
                        TSharedPtr<FJsonValue> Item = SerializeProperty(P->Inner, Elem, Visited, Depth);
                        Items.Add(Item.IsValid() ? Item : MakeShared<FJsonValueNull>());
                }
                return MakeShared<FJsonValueArray>(Items);
        }

        // Unsupported - skip
        return nullptr;
}

// ============================================================================
// Deserialization
// ============================================================================

static bool DeserializeObject(const TSharedPtr<FJsonObject>& Json, UObject* Outer, UObject*& OutObj, int32 Depth)
{
        OutObj = nullptr;

        if (!Json.IsValid() || Depth > MAX_DEPTH)
        {
                UE_LOG(LogJsonObjSer, Error, TEXT("DeserializeObject: Invalid JSON or max depth reached (depth=%d)"), Depth);
                return false;
        }

        // UE 5.8: FJsonObject::Values uses UE::FSharedString keys.
        // Iterate the map directly to avoid GetField()/HasField() API breakage.
        FString ClassPath;
        for (const auto& Pair : Json->Values)
        {
                if (Pair.Value.IsValid() && Pair.Value->Type == EJson::String)
                {
                        FString KeyStr(Pair.Key);
                        if (KeyStr == TEXT("__ObjectClassPath"))
                        {
                                ClassPath = Pair.Value->AsString();
                                break;
                        }
                }
        }

        if (ClassPath.IsEmpty())
        {
                UE_LOG(LogJsonObjSer, Error, TEXT("DeserializeObject: __ObjectClassPath not found in JSON"));
                return false;
        }

        UE_LOG(LogJsonObjSer, Log, TEXT("DeserializeObject: Loading class '%s' (depth=%d, outer=%s)"),
                *ClassPath, Depth, Outer ? *Outer->GetName() : TEXT("nullptr"));

        UClass* Cls = LoadClass<UObject>(nullptr, *ClassPath);
        if (!Cls)
        {
                UE_LOG(LogJsonObjSer, Error, TEXT("DeserializeObject: LoadClass FAILED for '%s'"), *ClassPath);
                return false;
        }

        if (Cls->IsChildOf(AActor::StaticClass()))
        {
                UE_LOG(LogJsonObjSer, Error, TEXT("DeserializeObject: Class '%s' is an Actor, skipping"), *ClassPath);
                return false;
        }

        OutObj = NewObject<UObject>(Outer, Cls);
        if (!OutObj)
        {
                UE_LOG(LogJsonObjSer, Error, TEXT("DeserializeObject: NewObject FAILED for class '%s'"), *ClassPath);
                return false;
        }

        UE_LOG(LogJsonObjSer, Log, TEXT("DeserializeObject: Created object '%s' of class '%s'"),
                *OutObj->GetName(), *Cls->GetName());

        // Build a property lookup map from the class
        TMap<FName, FProperty*> PropMap;
        for (TFieldIterator<FProperty> It(OutObj->GetClass()); It; ++It)
        {
                FProperty* Prop = *It;
                if (!Prop->HasAnyPropertyFlags(CPF_Transient))
                {
                        PropMap.Add(Prop->GetFName(), Prop);
                }
        }

        UE_LOG(LogJsonObjSer, Log, TEXT("DeserializeObject: Class '%s' has %d properties in PropMap"),
                *Cls->GetName(), PropMap.Num());

        // Iterate JSON Values and match to class properties by name
        for (const auto& Pair : Json->Values)
        {
                FString KeyStr(Pair.Key);
                if (KeyStr == TEXT("__ObjectClassPath"))
                {
                        continue;
                }

                FProperty** PropPtr = PropMap.Find(FName(*KeyStr));
                if (!PropPtr)
                {
                        UE_LOG(LogJsonObjSer, Warning, TEXT("DeserializeObject: JSON key '%s' not found in properties of '%s'"),
                                *KeyStr, *Cls->GetName());
                        continue;
                }
                if (!Pair.Value.IsValid())
                {
                        continue;
                }

                FProperty* Prop = *PropPtr;
                void* Data = Prop->ContainerPtrToValuePtr<void>(OutObj);
                if (!Data)
                {
                        continue;
                }

                bool Result = DeserializeProperty(Prop, Data, Pair.Value, OutObj, Depth + 1);
                if (!Result)
                {
                        UE_LOG(LogJsonObjSer, Warning, TEXT("DeserializeObject: Failed to set property '%s' (type=%s) on '%s'"),
                                *KeyStr, *Prop->GetClass()->GetName(), *OutObj->GetName());
                }
        }

        return true;
}

static bool DeserializeProperty(FProperty* Prop, void* Data, const TSharedPtr<FJsonValue>& Val, UObject* Owner, int32 Depth)
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
                        UObject* SubObj = nullptr;
                        if (DeserializeObject(SubJson, Owner, SubObj, Depth))
                        {
                                P->SetPropertyValue(Data, SubObj);
                                UE_LOG(LogJsonObjSer, Log, TEXT("DeserializeProperty: Set UObject* '%s' on property '%s'"),
                                        SubObj ? *SubObj->GetName() : TEXT("nullptr"), *Prop->GetName());
                                return true;
                        }
                        UE_LOG(LogJsonObjSer, Error, TEXT("DeserializeProperty: Failed to deserialize UObject* for property '%s'"),
                                *Prop->GetName());
                        P->SetPropertyValue(Data, nullptr);
                        return false;
                }
                return false;
        }
        // TArray
        if (FArrayProperty* P = CastField<FArrayProperty>(Prop))
        {
                if (Type != EJson::Array)
                {
                        return false;
                }
                const TArray<TSharedPtr<FJsonValue>>& Items = Val->AsArray();
                FScriptArrayHelper Arr(P, Data);
                Arr.Resize(0);
                UE_LOG(LogJsonObjSer, Log, TEXT("DeserializeProperty: TArray '%s' has %d elements, Inner=%s"),
                        *Prop->GetName(), Items.Num(), *P->Inner->GetClass()->GetName());
                for (int32 i = 0; i < Items.Num(); ++i)
                {
                        int32 Idx = Arr.AddValue();
                        void* Elem = Arr.GetRawPtr(Idx);
                        if (Elem)
                        {
                                bool Result = DeserializeProperty(P->Inner, Elem, Items[i], Owner, Depth);
                                if (!Result)
                                {
                                        UE_LOG(LogJsonObjSer, Warning, TEXT("DeserializeProperty: Failed to deserialize array element [%d] of '%s'"),
                                                i, *Prop->GetName());
                                }
                        }
                }
                return true;
        }

        UE_LOG(LogJsonObjSer, Warning, TEXT("DeserializeProperty: Unsupported property type '%s' for property '%s'"),
                *Prop->GetClass()->GetName(), *Prop->GetName());
        return false;
}
