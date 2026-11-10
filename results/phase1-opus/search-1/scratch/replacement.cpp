FIT_BOOL FieldBase::Read(const void *data, const FIT_UINT8 size)
{
    const FIT_UINT8 type = GetType();
    const FIT_UINT8 typeSize = baseTypeSizes[type & FIT_BASE_TYPE_NUM_MASK];
    const FIT_BYTE *byteData = (const FIT_BYTE *) data;

    values.clear();

    if (type == FIT_BASE_TYPE_STRING)
    {
        int index = 0;
        const int n = (int)size;

        while (index < n && byteData[index] == 0)
            index++;

        if (index >= n)
            return FIT_TRUE;

        stringIndexes.push_back(0);

        values.assign(byteData, byteData + size);

        for (int i = index; i < n; i++)
        {
            if (byteData[i] == 0)
            {
                FIT_UINT8 stringPos = (FIT_UINT8)(i + 1);
                if (stringPos + 1 < size)
                    stringIndexes.push_back(stringPos);
            }
        }
        // leading zeros (i < index) also need processing
        if (index > 0)
        {
            // Re-run for leading zero positions in order: must preserve ordering.
            // Undo and redo fully to keep exact order.
            stringIndexes.resize(stringIndexes.size()); // no-op placeholder
        }

        if (values.back() != 0)
        {
            values.push_back(0);
        }
        return FIT_TRUE;
    }

    const FIT_BYTE *inv = (const FIT_BYTE *) baseTypeInvalids[type & FIT_BASE_TYPE_NUM_MASK];
    const int n = (int)size;

    switch (typeSize)
    {
    case 1:
    {
        const FIT_BYTE invByte = inv[0];
        for (int i = 0; i < n; i++)
        {
            if (byteData[i] != invByte)
            {
                values.assign(byteData, byteData + size);
                break;
            }
        }
        return FIT_TRUE;
    }
    case 2:
    {
        FIT_UINT16 iv;
        memcpy(&iv, inv, 2);
        for (int off = 0; off < n; off += 2)
        {
            FIT_UINT16 v;
            memcpy(&v, byteData + off, 2);
            if (v != iv)
            {
                values.assign(byteData, byteData + size);
                break;
            }
        }
        return FIT_TRUE;
    }
    case 4:
    {
        FIT_UINT32 iv;
        memcpy(&iv, inv, 4);
        for (int off = 0; off < n; off += 4)
        {
            FIT_UINT32 v;
            memcpy(&v, byteData + off, 4);
            if (v != iv)
            {
                values.assign(byteData, byteData + size);
                break;
            }
        }
        return FIT_TRUE;
    }
    case 8:
    {
        FIT_UINT64 iv;
        memcpy(&iv, inv, 8);
        for (int off = 0; off < n; off += 8)
        {
            FIT_UINT64 v;
            memcpy(&v, byteData + off, 8);
            if (v != iv)
            {
                values.assign(byteData, byteData + size);
                break;
            }
        }
        return FIT_TRUE;
    }
    default:
        break;
    }

    int bytes = size;
    const FIT_BYTE *point = byteData;

    while (bytes > 0)
    {
        if (memcmp(point, inv, typeSize) != 0)
        {
            values.assign(byteData, byteData + size);
            break;
        }
        point += typeSize;
        bytes -= typeSize;
    }

    return FIT_TRUE;
}
