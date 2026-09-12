#include "PIKEditorTools.h"
#include "PIKAnimInstance.h"
#include "Animation/AnimBlueprint.h"

#if WITH_EDITOR
#include "AnimGraphNode_Base.h"
#include "AnimGraphNode_ModifyBone.h"
#include "AnimGraphNode_TwoBoneIK.h"
#include "AnimGraphNode_LocalToComponentSpace.h"
#include "AnimGraphNode_ComponentToLocalSpace.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_LinkedInputPose.h"
#include "EdGraph/EdGraph.h"
#include "EdGraph/EdGraphNodeUtils.h"
#include "EdGraphSchema_K2.h"
#include "K2Node_VariableGet.h"
#include "K2Node_CallFunction.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Kismet2/CompilerResultsLog.h"

namespace PIKEditor
{
    template<class T> T* Add(UEdGraph* Graph, int32 X, int32 Y)
    {
        FGraphNodeCreator<T> Creator(*Graph);
        T* Node=Creator.CreateNode();
        Node->NodePosX=X; Node->NodePosY=Y;
        Creator.Finalize();
        return Node;
    }
    void ShowPins(UAnimGraphNode_Base* Node, const TArray<FName>& Names)
    {
        for (FOptionalPinFromProperty& Pin : Node->ShowPinForProperties)
            if (Names.Contains(Pin.PropertyName)) Pin.bShowPin=true;
        Node->ReconstructNode();
    }
    UEdGraphPin* Pin(UEdGraphNode* Node, const FName Name, EEdGraphPinDirection Direction)
    {
        if (!Node) return nullptr;
        for (UEdGraphPin* P : Node->Pins) if (P->PinName==Name && P->Direction==Direction) return P;
        return nullptr;
    }
    bool Connect(UEdGraph* Graph, UEdGraphNode* A, FName OutName, UEdGraphNode* B, FName InName)
    {
        UEdGraphPin* Out=Pin(A,OutName,EGPD_Output); UEdGraphPin* In=Pin(B,InName,EGPD_Input);
        return Out && In && Graph->GetSchema()->TryCreateConnection(Out,In);
    }
    UK2Node_VariableGet* Get(UEdGraph* Graph,FName Name,int32 X,int32 Y)
    {
        FGraphNodeCreator<UK2Node_VariableGet> Creator(*Graph);
        UK2Node_VariableGet* Node=Creator.CreateNode();
        Node->VariableReference.SetSelfMember(Name);
        Node->NodePosX=X;Node->NodePosY=Y;
        Creator.Finalize();
        return Node;
    }
    void Bypass(UK2Node_CallFunction* Node)
    {
        UEdGraphPin* In=Node->FindPin(UEdGraphSchema_K2::PN_Execute);
        UEdGraphPin* Out=Node->FindPin(UEdGraphSchema_K2::PN_Then);
        if (!In || !Out) return;
        TArray<UEdGraphPin*> Before=In->LinkedTo, After=Out->LinkedTo;
        In->BreakAllPinLinks();Out->BreakAllPinLinks();
        for(UEdGraphPin* A:Before) for(UEdGraphPin* B:After) Node->GetGraph()->GetSchema()->TryCreateConnection(A,B);
        Node->NodeComment=TEXT("Legacy prototype: bypassed. Prediction now updates in PIKAnimInstance.");
        Node->bCommentBubbleVisible=true;
    }
}
#endif

FString UPIKEditorTools::ConfigurePredictIKLayer(UAnimBlueprint* Blueprint)
{
#if WITH_EDITOR
    if (!Blueprint || Blueprint->GetName()!=TEXT("ALS_AnimBP")) return TEXT("ERROR: expected ALS_AnimBP");
    TArray<UEdGraph*> Graphs; Blueprint->GetAllGraphs(Graphs);
    UEdGraph* PredictGraph=nullptr;
    for(UEdGraph* G:Graphs) if(G->GetFName()==TEXT("PredictIK")) PredictGraph=G;
    if (!PredictGraph) return TEXT("ERROR: PredictIK layer missing");
    UAnimGraphNode_LinkedInputPose* Input=nullptr; UAnimGraphNode_Root* Output=nullptr;
    for(UEdGraphNode* N:PredictGraph->Nodes)
    {
        if(auto* I=Cast<UAnimGraphNode_LinkedInputPose>(N)) Input=I;
        if(auto* O=Cast<UAnimGraphNode_Root>(N)) Output=O;
    }
    if(!Input || !Output) return TEXT("ERROR: layer input/output missing");

    Blueprint->Modify();PredictGraph->Modify();
    Blueprint->ParentClass=UPIKAnimInstance::StaticClass();
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FKismetEditorUtilities::CompileBlueprint(Blueprint);
    for(UEdGraph* G:Graphs)
    {
        if(G->GetFName()!=TEXT("UpdateGraph")) continue;
        for(UEdGraphNode* N:G->Nodes)
            if(auto* Call=Cast<UK2Node_CallFunction>(N))
                if(Call->GetFunctionName()==TEXT("PredictFootIKInfo") || Call->GetFunctionName()==TEXT("UpdateFootIK")) PIKEditor::Bypass(Call);
    }

    const TArray<TObjectPtr<UEdGraphNode>> OldNodes=PredictGraph->Nodes;
    for(UEdGraphNode* N:OldNodes)
        if(N!=Input && N!=Output) FBlueprintEditorUtils::RemoveNode(Blueprint,N,true);
    Input->BreakAllNodeLinks();Output->BreakAllNodeLinks();
    Input->NodePosX=0;Input->NodePosY=0;
    using namespace PIKEditor;
    auto* ToCS=Add<UAnimGraphNode_LocalToComponentSpace>(PredictGraph,250,0);
    auto* Pelvis=Add<UAnimGraphNode_ModifyBone>(PredictGraph,500,0);
    Pelvis->Node.BoneToModify.BoneName=TEXT("pelvis");
    Pelvis->Node.TranslationMode=BMM_Additive;
    Pelvis->Node.TranslationSpace=BCS_ComponentSpace;
    Pelvis->Node.AlphaInputType=EAnimAlphaInputType::Float;
    ShowPins(Pelvis,{TEXT("Translation"),TEXT("Alpha")});
    Pelvis->NodeComment=TEXT("Bounded pelvis offset. Mesh transform and reference IK bones stay unchanged.");
    Pelvis->bCommentBubbleVisible=true;
    bool bOK=Connect(PredictGraph,Input,TEXT("Pose"),ToCS,TEXT("LocalPose"));
    bOK &= Connect(PredictGraph,ToCS,TEXT("ComponentPose"),Pelvis,TEXT("ComponentPose"));
    auto* PelvisValue=Get(PredictGraph,TEXT("PIK_PelvisOffsetCS"),500,230);
    auto* Alpha=Get(PredictGraph,TEXT("PIK_Alpha"),280,370);
    bOK &= Connect(PredictGraph,PelvisValue,TEXT("PIK_PelvisOffsetCS"),Pelvis,TEXT("Translation"));
    bOK &= Connect(PredictGraph,Alpha,TEXT("PIK_Alpha"),Pelvis,TEXT("Alpha"));
    UEdGraphNode* Previous=Pelvis;
    for(int32 Side=0;Side<2;++Side)
    {
        const bool bLeft=Side==0; const int32 X=900+Side*850;
        const FName Target=bLeft?TEXT("PIK_FootTargetCS_L"):TEXT("PIK_FootTargetCS_R");
        const FName Knee=bLeft?TEXT("PIK_KneeTargetCS_L"):TEXT("PIK_KneeTargetCS_R");
        const FName Rotation=bLeft?TEXT("PIK_FootRotationCS_L"):TEXT("PIK_FootRotationCS_R");
        auto* IK=Add<UAnimGraphNode_TwoBoneIK>(PredictGraph,X,0);
        IK->Node.IKBone.BoneName=bLeft?TEXT("foot_l"):TEXT("foot_r");
        IK->Node.EffectorLocationSpace=BCS_ComponentSpace;
        IK->Node.JointTargetLocationSpace=BCS_ComponentSpace;
        IK->Node.bAllowStretching=false;
        IK->Node.bMaintainEffectorRelRot=true;
        IK->Node.bTakeRotationFromEffectorSpace=false;
        IK->Node.AlphaInputType=EAnimAlphaInputType::Float;
        ShowPins(IK,{TEXT("EffectorLocation"),TEXT("JointTargetLocation"),TEXT("Alpha")});
        auto* TargetValue=Get(PredictGraph,Target,X-100,270);
        auto* KneeValue=Get(PredictGraph,Knee,X-100,350);
        bOK &= Connect(PredictGraph,Previous,TEXT("Pose"),IK,TEXT("ComponentPose"));
        bOK &= Connect(PredictGraph,TargetValue,Target,IK,TEXT("EffectorLocation"));
        bOK &= Connect(PredictGraph,KneeValue,Knee,IK,TEXT("JointTargetLocation"));
        bOK &= Connect(PredictGraph,Alpha,TEXT("PIK_Alpha"),IK,TEXT("Alpha"));
        IK->NodeComment=TEXT("Absolute ankle target after pelvis adjustment; no limb stretching.");IK->bCommentBubbleVisible=true;
        auto* Tilt=Add<UAnimGraphNode_ModifyBone>(PredictGraph,X+400,0);
        Tilt->Node.BoneToModify.BoneName=bLeft?TEXT("foot_l"):TEXT("foot_r");
        Tilt->Node.TranslationMode=BMM_Ignore;
        Tilt->Node.RotationMode=BMM_Replace;Tilt->Node.RotationSpace=BCS_ComponentSpace;
        Tilt->Node.AlphaInputType=EAnimAlphaInputType::Float;
        ShowPins(Tilt,{TEXT("Rotation"),TEXT("Alpha")});
        auto* RotationValue=Get(PredictGraph,Rotation,X+380,270);
        bOK &= Connect(PredictGraph,IK,TEXT("Pose"),Tilt,TEXT("ComponentPose"));
        bOK &= Connect(PredictGraph,RotationValue,Rotation,Tilt,TEXT("Rotation"));
        bOK &= Connect(PredictGraph,Alpha,TEXT("PIK_Alpha"),Tilt,TEXT("Alpha"));
        Previous=Tilt;
    }
    auto* ToLocal=Add<UAnimGraphNode_ComponentToLocalSpace>(PredictGraph,2700,0);
    Output->NodePosX=2950;Output->NodePosY=0;
    bOK &= Connect(PredictGraph,Previous,TEXT("Pose"),ToLocal,TEXT("ComponentPose"));
    bOK &= Connect(PredictGraph,ToLocal,TEXT("Pose"),Output,TEXT("Result"));
    FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
    FCompilerResultsLog Results;
    FKismetEditorUtilities::CompileBlueprint(Blueprint,EBlueprintCompileOptions::None,&Results);
    if(!bOK || Results.NumErrors) return FString::Printf(TEXT("ERROR: connections=%d compile_errors=%d"),bOK,Results.NumErrors);
    return FString::Printf(TEXT("OK: PredictIK configured; %d nodes; %d compile warnings. Not saved automatically."),PredictGraph->Nodes.Num(),Results.NumWarnings);
#else
    return TEXT("Editor only");
#endif
}
