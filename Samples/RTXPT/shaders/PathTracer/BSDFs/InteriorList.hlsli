#ifndef __INTERIOR_LIST_HLSLI__
#define __INTERIOR_LIST_HLSLI__

#ifndef INTERIOR_LIST_SLOT_COUNT
#    define INTERIOR_LIST_SLOT_COUNT 2
#endif

struct InteriorList
{
    static const uint kNoMaterial           = 0xffffffffu;
    static const uint kMaterialBits         = 28u;
    static const uint kNestedPriorityBits   = 4u;
    static const uint kMaterialOffset       = 0u;
    static const uint kNestedPriorityOffset = kMaterialOffset + kMaterialBits;
    static const uint kMaterialMask         = ((1u << kMaterialBits) - 1u) << kMaterialOffset;
    static const uint kMaxNestedPriority    = (1u << kNestedPriorityBits) - 1u;

    uint2 slots;

    static InteriorList make()
    {
        InteriorList list;
        list.slots = uint2(0u, 0u);
        return list;
    }

    uint makeSlot(uint materialID, uint nestedPriority)
    {
        return (nestedPriority << kNestedPriorityOffset) | (materialID & kMaterialMask);
    }

    bool isSlotActive(uint slot) { return slot != 0u; }
    bool isEmpty() { return !isSlotActive(slots.x); }
    uint getSlotNestedPriority(uint slot) { return slot >> kNestedPriorityOffset; }
    uint getSlotMaterialID(uint slot) { return slot & kMaterialMask; }
    uint getTopNestedPriority() { return getSlotNestedPriority(slots.x); }
    uint getTopMaterialID() { return isSlotActive(slots.x) ? getSlotMaterialID(slots.x) : kNoMaterial; }
    uint getNextMaterialID() { return isSlotActive(slots.y) ? getSlotMaterialID(slots.y) : kNoMaterial; }

    bool isTrueIntersection(uint nestedPriority)
    {
        if (isEmpty())
            return true;
        return nestedPriority == 0u || nestedPriority >= getTopNestedPriority();
    }

    void sortSlots()
    {
        if (slots.x < slots.y)
        {
            uint tmp = slots.x;
            slots.x  = slots.y;
            slots.y  = tmp;
        }
    }

    void handleIntersection(uint materialID, uint nestedPriority, bool entering)
    {
        // Authored priority 0 is highest; internally slot priority 0 means empty.
        if (nestedPriority == 0u)
            nestedPriority = kMaxNestedPriority;

        if (entering && slots.x == 0u)
            slots.x = makeSlot(materialID, nestedPriority);
        else if (!entering && isSlotActive(slots.x) && getSlotMaterialID(slots.x) == materialID)
            slots.x = 0u;
        else if (entering && slots.y == 0u)
            slots.y = makeSlot(materialID, nestedPriority);
        else if (!entering && isSlotActive(slots.y) && getSlotMaterialID(slots.y) == materialID)
            slots.y = 0u;

        sortSlots();
    }
};

#endif // __INTERIOR_LIST_HLSLI__
