static const uint kIndirectObjectIndex = 0xffffffffu;

uint ResolveObjectIndex(uint pushedObjectIndex, uint instanceID, uint startInstance)
{
    return pushedObjectIndex == kIndirectObjectIndex
        ? instanceID + startInstance
        : pushedObjectIndex;
}
