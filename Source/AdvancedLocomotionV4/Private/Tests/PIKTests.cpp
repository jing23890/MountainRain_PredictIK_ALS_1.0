#if WITH_DEV_AUTOMATION_TESTS && WITH_EDITOR
#include "PIKAnimInstance.h"
#include "Misc/AutomationTest.h"
#include "Engine/World.h"
#include "Components/BoxComponent.h"
#include "Components/SkeletalMeshComponent.h"

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FPIKPathTest, "ALS.PredictIK.PathGeometry", EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FPIKPathTest::RunTest(const FString& Parameters)
{
    TestEqual(TEXT("Projection ignores height"),UPIKAnimInstance::PIK_ProjectProgress(FVector(25,0,999),FVector::ZeroVector,FVector(100,0,20)),0.25f);
    TestEqual(TEXT("Projection clamps before start"),UPIKAnimInstance::PIK_ProjectProgress(FVector(-20,0,0),FVector::ZeroVector,FVector(100,0,0)),0.f);
    TestEqual(TEXT("Zero horizontal span remains finite"),UPIKAnimInstance::PIK_ProjectProgress(FVector(0,0,5),FVector::ZeroVector,FVector(0,0,20)),0.f);
    TArray<FVector> Samples{FVector(0,0,0),FVector(40,0,20),FVector(60,0,20),FVector(100,0,0)};
    TestEqual(TEXT("Ascending height"),UPIKAnimInstance::PIK_SamplePathHeight(Samples,FVector(20,0,500)),10.f);
    TestEqual(TEXT("Obstacle plateau"),UPIKAnimInstance::PIK_SamplePathHeight(Samples,FVector(50,0,-500)),20.f);
    TestEqual(TEXT("Descending height"),UPIKAnimInstance::PIK_SamplePathHeight(Samples,FVector(80,0,0)),10.f);

    UWorld* World=UWorld::CreateWorld(EWorldType::Game,false);
    AActor* Owner=World->SpawnActor<AActor>();
    USkeletalMeshComponent* Mesh=NewObject<USkeletalMeshComponent>(Owner);
    Owner->SetRootComponent(Mesh);Mesh->RegisterComponent();
    UPIKAnimInstance* Solver=NewObject<UPIKAnimInstance>(Mesh);
    auto Box=[&](FVector Center,FVector Extent)
    {
        AActor* Actor=World->SpawnActor<AActor>();
        UBoxComponent* Component=NewObject<UBoxComponent>(Actor);
        Actor->SetRootComponent(Component);Component->SetBoxExtent(Extent);
        Component->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
        Component->SetCollisionResponseToAllChannels(ECR_Block);
        Component->RegisterComponent();Actor->SetActorLocation(Center);
        return Actor;
    };
    Box(FVector(0,0,-10),FVector(1000,1000,10));
    TArray<FVector> Path{FVector(999)};
    TestTrue(TEXT("Flat path"),Solver->PIK_BuildFootPath(FVector(0,0,0),FVector(100,0,0),Path));
    TestEqual(TEXT("Flat rebuild clears old samples"),Path.Num(),2);
    AActor* Step=Box(FVector(150,0,10),FVector(100,100,10));
    TestTrue(TEXT("Ascending 20cm step"),Solver->PIK_BuildFootPath(FVector(0,0,0),FVector(100,0,20),Path));
    TestTrue(TEXT("Step has edge sample"),Path.Num()>=3);
    TestTrue(TEXT("Descending 20cm step"),Solver->PIK_BuildFootPath(FVector(100,0,20),FVector(0,0,0),Path));
    Step->Destroy();
    AActor* Obstacle=Box(FVector(50,0,10),FVector(10,100,10));
    TestTrue(TEXT("Obstacle with both endpoints on floor"),Solver->PIK_BuildFootPath(FVector(0,0,0),FVector(100,0,0),Path));
    TestTrue(TEXT("Bidirectional obstacle samples"),Path.Num()>=4);
    TestTrue(TEXT("Obstacle envelope supports center"),UPIKAnimInstance::PIK_SamplePathHeight(Path,FVector(50,0,0))>=19.9f);
    for(int32 I=1;I<Path.Num();++I)
        TestTrue(TEXT("Samples remain ordered"),Path[I].X>Path[I-1].X);
    Obstacle->Destroy();
    Box(FVector(50,0,100),FVector(10,100,100));
    TestFalse(TEXT("Tall wall rejected"),Solver->PIK_BuildFootPath(FVector(0,0,0),FVector(100,0,0),Path));
    TestTrue(TEXT("Failed path exposes no old points"),Path.IsEmpty());
    TestFalse(TEXT("Excessive endpoint height rejected"),Solver->PIK_BuildFootPath(FVector(0,0,0),FVector(100,0,100),Path));
    World->DestroyWorld(false);
    return true;
}
#endif
