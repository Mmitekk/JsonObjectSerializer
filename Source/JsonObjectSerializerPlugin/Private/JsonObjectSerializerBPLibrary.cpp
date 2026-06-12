// Copyright JsonObjectSerializer. All Rights Reserved.

#include "JsonObjectSerializerBPLibrary.h"
#include "JsonObjectSerializerPlugin.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "Serialization/JsonReader.h"
#include "UObject/UnrealType.h"
#include "UObject/TextProperty.h"
#include "UObject/EnumProperty.h"

// ============================================================================
// Constants
// ============================================================================

static constexpr int32 MAX_DEPTH = 32;

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
                return;
        }

        if (!IsValid(Outer))
        {
                Outer = GetTransientPackage();
        }

        Success = DeserializeObject(JsonObj, Outer, SpawnedObject, 0);
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
                        if (P->IsUnsignedInt())
                        {
                                return MakeShared<FJsonValueNumber>((double)P->GetUnsignedIntPropertyValue(Data));
                        }
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
                return false;
        }

        FString ClassPath;
        if (!Json->TryGetStringField(TEXT("__ObjectClassPath"), ClassPath))
        {
                return false;
        }

        UClass* Cls = LoadClass<UObject>(nullptr, *ClassPath);
        if (!Cls)
        {
                return false;
        }

        if (Cls->IsChildOf(AActor::StaticClass()))
        {
                return false;
        }

        OutObj = NewObject<UObject>(Outer, Cls);
        if (!OutObj)
        {
                return false;
        }

        for (TFieldIterator<FProperty> It(OutObj->GetClass()); It; ++It)
        {
                FProperty* Prop = *It;
                if (Prop->HasAnyPropertyFlags(CPF_Transient))
                {
                        continue;
                }

                FString PropName = Prop->GetName();
                if (!Json->HasField(PropName))
                {
                        continue;
                }

                void* Data = Prop->ContainerPtrToValuePtr<void>(OutObj);
                if (!Data)
                {
                        continue;
                }

                const TSharedPtr<FJsonValue> Val = Json->GetField(PropName);
                if (!Val.IsValid())
                {
                        continue;
                }

                DeserializeProperty(Prop, Data, Val, OutObj, Depth + 1);
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
                                if (P->IsUnsignedInt())
                                {
                                        P->SetUnsignedIntPropertyValue(Data, (uint64)Val->AsNumber());
                                }
                                else
                                {
                                        P->SetIntPropertyValue(Data, (int64)Val->AsNumber());
                                }
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
                                return true;
                        }
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
                for (int32 i = 0; i < Items.Num(); ++i)
                {
                        int32 Idx = Arr.AddValue();
                        void* Elem = Arr.GetRawPtr(Idx);
                        if (Elem)
                        {
                                DeserializeProperty(P->Inner, Elem, Items[i], Owner, Depth);
                        }
                }
                return true;
        }

        return false;
}
