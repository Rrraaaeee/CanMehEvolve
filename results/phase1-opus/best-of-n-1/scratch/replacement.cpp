void Decode::ExpandComponents(Field *containingField, const Profile::FIELD_COMPONENT *components, FIT_UINT16 numComponents)
{
    FIT_UINT16 offset = 0;
    const FIT_UINT16 mesgNum = mesg.GetNum();

    for (FIT_UINT16 i = 0; i < numComponents; i++)
    {
        const Profile::FIELD_COMPONENT *component = &components[i];

        if (component->num != FIT_FIELD_NUM_INVALID)
        {
            Field componentField(mesgNum, component->num);
            const FIT_UINT8 fieldNum = componentField.GetNum();
            FIT_FLOAT64 value;
            FIT_UINT64 bitsValue = FIT_UINT64_INVALID;
            FIT_SINT64 signedBitsValue = FIT_SINT64_INVALID;

            componentField.SetIsExpanded(FIT_TRUE);

            const FIT_BOOL isSigned = componentField.IsSignedInteger();

            if (isSigned)
            {
                signedBitsValue = containingField->GetBitsSignedValue64(offset, component->bits);

                if (signedBitsValue == FIT_SINT64_INVALID)
                    break;

                if (component->accumulate)
                    bitsValue = accumulator.Accumulate(mesgNum, component->num, signedBitsValue, component->bits);
            }
            else
            {
                bitsValue = containingField->GetBitsValue64(offset, component->bits);

                if (bitsValue == FIT_UINT64_INVALID)
                    break;

                if (component->accumulate)
                    bitsValue = accumulator.Accumulate(mesgNum, component->num, bitsValue, component->bits);
            }

            const FIT_UINT16 numSubComponents = componentField.GetNumComponents();

            if (numSubComponents == 1)
            {
                const Profile::FIELD_COMPONENT *sub = componentField.GetComponent(0);
                if (isSigned)
                    value = (((signedBitsValue / (FIT_FLOAT64)component->scale) - component->offset) + sub->offset) * sub->scale;
                else
                    value = (((bitsValue / (FIT_FLOAT64)component->scale) - component->offset) + sub->offset) * sub->scale;

                fit::Field *currentField = mesg.GetField(fieldNum);
                if (currentField != FIT_NULL)
                {
                    currentField->AddRawValue(value, currentField->GetNumValues());
                }
                else
                {
                    componentField.AddRawValue(value, componentField.GetNumValues());
                    mesg.AddField(componentField);
                }
            }
            else if (numSubComponents > 1)
            {
                int bitsAdded = 0;
                const FIT_UINT8 typeBits = baseTypeSizes[componentField.GetType() & FIT_BASE_TYPE_NUM_MASK];
                const FIT_UINT64 mask = ((FIT_UINT64)1 << typeBits) - 1;

                while (bitsAdded < component->bits)
                {
                    Field *field = mesg.GetField(fieldNum);
                    if (field != FIT_NULL)
                    {
                        field->AddValue(bitsValue & mask, field->GetNumValues());
                    }
                    else
                    {
                        componentField.AddValue(bitsValue & mask, componentField.GetNumValues());
                        mesg.AddField(componentField);
                    }
                    bitsValue >>= typeBits;
                    bitsAdded += typeBits;
                }
            }
            else
            {
                FIT_UINT16 subfieldIndex = mesg.GetActiveSubFieldIndex(fieldNum);
                if (isSigned)
                    value = (((signedBitsValue / (FIT_FLOAT64)component->scale) - component->offset) + componentField.GetOffset(subfieldIndex)) * componentField.GetScale(subfieldIndex);
                else
                    value = (((bitsValue / (FIT_FLOAT64)component->scale) - component->offset) + componentField.GetOffset(subfieldIndex)) * componentField.GetScale(subfieldIndex);

                fit::Field *currentField = mesg.GetField(fieldNum);
                if (currentField != FIT_NULL)
                {
                    currentField->AddRawValue(value, currentField->GetNumValues());
                }
                else
                {
                    componentField.AddRawValue(value, componentField.GetNumValues());
                    mesg.AddField(componentField);
                }
            }
        }
        offset += component->bits;
    }
}
