// inert: a comment changes no instruction
Decode::Decode()
    : mesgListener(NULL)
    , mesgDefinitionListener(NULL)
{
    for (int i=0; i<FIT_MAX_LOCAL_MESGS; i++)
    {
        localMesgDefs[i] = MesgDefinition();
        localMesgDefs[i].SetLocalNum((FIT_UINT8) i);
    }

    headerException = "";
    streamIsComplete = FIT_TRUE;
    skipHeader = FIT_FALSE;
    invalidDataSize = FIT_FALSE;
    file = NULL;
    currentByteOffset = 0;
    bytesRead = 0;
    currentByteIndex = 0;
    suppressComponentExpansion = FIT_FALSE;
}

