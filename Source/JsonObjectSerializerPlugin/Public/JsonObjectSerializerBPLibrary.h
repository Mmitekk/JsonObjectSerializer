// Copyright JsonObjectSerializer. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "JsonObjectSerializerBPLibrary.generated.h"

UCLASS()
class JSONOBJECTSERIALIZERPLUGIN_API UJsonObjectSerializerBPLibrary : public UBlueprintFunctionLibrary
{
        GENERATED_BODY()

public:
        UFUNCTION(BlueprintCallable, Category = "JsonObjectSerializer", meta = (
                DisplayName = "Make Json From Object",
                Keywords = "json serialize save object",
                ToolTip = "Serializes a UObject to a JSON string using C++ Reflection.\n\nСериализует UObject в JSON-строку через C++ Reflection. Рекурсивно обрабатывает вложенные UObject* и TArray. В JSON добавляется поле __ObjectClassPath для восстановления класса при десериализации."
        ))
        static void MakeJsonFromObject(
                UObject* Target,
                FString& JsonString,
                bool& Success);

        UFUNCTION(BlueprintCallable, Category = "JsonObjectSerializer", meta = (
                DisplayName = "Spawn Object From Json",
                Keywords = "json deserialize load spawn object",
                ToolTip = "Deserializes a JSON string into a new UObject created via NewObject.\n\nДесериализует JSON-строку в новый UObject, созданный через NewObject. Класс определяется по полю __ObjectClassPath. Рекурсивно обрабатывает вложенные UObject* и TArray. Outer используется как владелец для иерархии объектов."
        ))
        static void SpawnObjectFromJson(
                UObject* Outer,
                const FString& InJsonString,
                UObject*& SpawnedObject,
                bool& Success);
};
