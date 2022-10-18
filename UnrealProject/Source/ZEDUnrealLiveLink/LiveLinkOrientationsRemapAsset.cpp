#include "LiveLinkOrientationsRemapAsset.h"

#include "Roles/LiveLinkAnimationTypes.h"
#include "BonePose.h"

float ULiveLinkOrientationsRemapAsset::ComputeRootTranslationFactor(FCompactPose& OutPose, TArray<FName, TMemStackAllocator<>> TransformedBoneNames, const FLiveLinkAnimationFrameData* InFrameData) {
    float avatarTotalTranslation = 0.f;
    float SDKTotalTranslation = 0.f;
    for (int32 i = 23; i < 25; i++)
    {
        FTransform BoneTransform = InFrameData->Transforms[i];
        FCompactPoseBoneIndex CPIndex = getCPIndex(i, OutPose, TransformedBoneNames);
        if (CPIndex != INDEX_NONE)
        {
            avatarTotalTranslation += OutPose[CPIndex].GetTranslation().Size();
            SDKTotalTranslation += BoneTransform.GetTranslation().Size();
        }
    }

    float factor = avatarTotalTranslation / SDKTotalTranslation;
    float scale = 1.f;
    FCompactPoseBoneIndex CPIndexRoot = getCPIndex(0, OutPose, TransformedBoneNames);
    if (CPIndexRoot != INDEX_NONE)
        scale = OutPose[CPIndexRoot].GetScale3D().Z;
	return FMath::Abs(scale * factor);
}

FCompactPoseBoneIndex ULiveLinkOrientationsRemapAsset::getCPIndex(int32 idx, FCompactPose& OutPose, TArray<FName, TMemStackAllocator<>> TransformedBoneNames){
    FName BoneName = TransformedBoneNames[idx];
    const int32 MeshIndex = OutPose.GetBoneContainer().GetPoseBoneIndexForBoneName(BoneName);
    if (MeshIndex != INDEX_NONE)
    {
        FCompactPoseBoneIndex CPIndex = OutPose.GetBoneContainer().MakeCompactPoseIndex(
            FMeshPoseBoneIndex(MeshIndex));
        return CPIndex;
    }
    return (FCompactPoseBoneIndex) INDEX_NONE;
}

void ULiveLinkOrientationsRemapAsset::putInRefPose(FCompactPose& OutPose, TArray<FName, TMemStackAllocator<>> TransformedBoneNames){
    for (int32 i = 0; i < TransformedBoneNames.Num(); i++)
    {
        FCompactPoseBoneIndex CPIndex = getCPIndex(i, OutPose, TransformedBoneNames);
        if (CPIndex != INDEX_NONE)
        {
            auto RefBoneTransform = OutPose.GetRefPose(CPIndex);
            OutPose[CPIndex].SetRotation(RefBoneTransform.GetRotation());
        }
    }
}

void ULiveLinkOrientationsRemapAsset::propagateRestPoseRotations(int32 parentIdx, FCompactPose& OutPose, TArray<FName, TMemStackAllocator<>> TransformedBoneNames, TArray<int32> SourceBoneParents, FQuat restPoseRot, bool inverse){
	for (int32 i = 0; i < SourceBoneParents.Num(); i++){
        FCompactPoseBoneIndex CPIndex = getCPIndex(i, OutPose, TransformedBoneNames);
        if(SourceBoneParents[i] == parentIdx)
        {
            FQuat restPoseRotChild;
            if (CPIndex != INDEX_NONE)
            {   
                FQuat jointRotation = restPoseRot * OutPose[CPIndex].GetRotation();
                OutPose[CPIndex].SetRotation(jointRotation);
                if (!inverse)
                    restPoseRotChild = restPoseRot * OutPose.GetRefPose(CPIndex).GetRotation();
                else
                    restPoseRotChild = OutPose.GetRefPose(CPIndex).GetRotation().Inverse() * restPoseRot;
            }
            else
                restPoseRotChild = restPoseRot; // in case the parent bone doesn't appear in 
                                                // the Unreal avatar, we propagate its own restposerot to 
                                                // its children 
            propagateRestPoseRotations(i, OutPose, TransformedBoneNames, SourceBoneParents, restPoseRotChild, inverse);
        }
	}
}

// Take Live Link data and apply it to skeleton bones in UE4.
void ULiveLinkOrientationsRemapAsset::BuildPoseFromAnimationData(float DeltaTime,
    const FLiveLinkSkeletonStaticData* InSkeletonData,
    const FLiveLinkAnimationFrameData* InFrameData,
    FCompactPose& OutPose,
    USkeletalMeshComponent* SkeletalMesh)
{
    const TArray<FName>& SourceBoneNames = InSkeletonData->BoneNames;
    const TArray<int32>& SourceBoneParents = InSkeletonData->BoneParents;

    TArray<FName, TMemStackAllocator<>> TransformedBoneNames;
    TransformedBoneNames.Reserve(SourceBoneNames.Num());

    // Find remapped bone names and cache them for fast subsequent retrieval.
    for (const FName& SrcBoneName : SourceBoneNames)
    {
        FName* TargetBoneName = BoneNameMap.Find(SrcBoneName);
        if (TargetBoneName == nullptr)
        {
            /* User will create a blueprint child class and implement this function using a switch statement. */
            FName NewName = GetRemappedBoneName(SrcBoneName);
            TransformedBoneNames.Add(NewName);
            BoneNameMap.Add(SrcBoneName, NewName);
        }
        else
        {
            TransformedBoneNames.Add(*TargetBoneName);
        }
    }


    if (InFrameData->Transforms[34 + 20].GetLocation().X != 0 && InFrameData->Transforms[34 + 24].GetLocation().X != 0) { //if both foot are visible/detected
        if (SkeletalMesh) {
            float LeftFootHeight = SkeletalMesh->GetBoneLocation(TransformedBoneNames[20]).Z;
            float RightFootHeight = SkeletalMesh->GetBoneLocation(TransformedBoneNames[24]).Z;
            FeetOffset = Alpha * fmin(LeftFootHeight, RightFootHeight) + (1 - Alpha) * FeetOffset;
        }
    }

    putInRefPose(OutPose, TransformedBoneNames);
    FCompactPoseBoneIndex CPIndexRoot = getCPIndex(0, OutPose, TransformedBoneNames);

	if (CPIndexRoot != INDEX_NONE)
        propagateRestPoseRotations(0, OutPose, TransformedBoneNames, SourceBoneParents, OutPose.GetRefPose(CPIndexRoot).GetRotation(), false);
	
    // Iterate over remapped bone names, find the index of that bone on the skeleton, and apply the Live Link pose data.
    for (int32 i = 0; i < TransformedBoneNames.Num(); i++)
    {
        FName BoneName = TransformedBoneNames[i];
        

        if (!BoneName.ToString().ToLower().Contains("conf")) { // ignore kp confidence stored as kp
            FTransform BoneTransform = InFrameData->Transforms[i];
            FCompactPoseBoneIndex CPIndex = getCPIndex(i, OutPose, TransformedBoneNames);
            if (CPIndex != INDEX_NONE)
            {
                FQuat Rotation;
                FVector Translation;

                // Only use position + rotation data for root. For all other bones, set rotation only.
                if (BoneName == *BoneNameMap.Find(GetTargetRootName()))
                {

                    float rootTranslationFactor = ComputeRootTranslationFactor(OutPose, TransformedBoneNames, InFrameData);
                    FVector RootPosition = BoneTransform.GetTranslation();
                    FCompactPoseBoneIndex leftUpLegIndex = getCPIndex(18, OutPose, TransformedBoneNames);
                    float HipOffset = FMath::Abs(OutPose[leftUpLegIndex].GetTranslation().Z) * OutPose[CPIndexRoot].GetScale3D().Z;

                    RootPosition.Z += HipOffset;
                    RootPosition.Z -= FeetOffset;
                    RootPosition.Z *= rootTranslationFactor;

                    Translation = RootPosition;
                    Rotation = BoneTransform.GetRotation();
                }
                else
                {
                    Rotation = BoneTransform.GetRotation();
                    Translation = OutPose[CPIndex].GetTranslation();
                }

                // Retrieves the default reference pose for the skeleton. Live Link data contains relative transforms from the default pose.
                FQuat FinalRotation = Rotation * OutPose[CPIndex].GetRotation();
                OutPose[CPIndex].SetRotation(FinalRotation);
                OutPose[CPIndex].SetTranslation(Translation);
            }
        }
    }
    if (CPIndexRoot != INDEX_NONE)
        propagateRestPoseRotations(0, OutPose, TransformedBoneNames, SourceBoneParents, OutPose.GetRefPose(CPIndexRoot).GetRotation().Inverse(), true);
}
