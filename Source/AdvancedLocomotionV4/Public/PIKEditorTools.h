#pragma once
#include "CoreMinimal.h"
#include "Kismet/BlueprintFunctionLibrary.h"
#include "PIKEditorTools.generated.h"

class UAnimBlueprint;

/** Editor setup only. The generated animation graph contains normal, editable UE nodes. */
UCLASS()
class ADVANCEDLOCOMOTIONV4_API UPIKEditorTools : public UBlueprintFunctionLibrary
{
    GENERATED_BODY()
public:
    UFUNCTION(BlueprintCallable, Category="PIK|Editor")
    static FString ConfigurePredictIKLayer(UAnimBlueprint* Blueprint);
};
