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
	UFUNCTION(BlueprintCallable, Category = "JsonObjectSerializer", meta = (DisplayName = "Make Json From Object", Keywords = "json serialize save object"))
	static void MakeJsonFromObject(UObject* Target, FString& JsonString, bool& Success);

	UFUNCTION(BlueprintCallable, Category = "JsonObjectSerializer", meta = (DisplayName = "Spawn Object From Json", Keywords = "json deserialize load spawn object"))
	static void SpawnObjectFromJson(UObject* Outer, const FString& InJsonString, UObject*& SpawnedObject, bool& Success);
};
