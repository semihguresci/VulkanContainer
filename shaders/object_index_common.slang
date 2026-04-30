static const uint kIndirectObjectIndex = 0xffffffffu;

uint ResolveObjectIndex(uint pushedObjectIndex, uint instanceID)
{
    return pushedObjectIndex == kIndirectObjectIndex
        ? instanceID
        : pushedObjectIndex;
}
