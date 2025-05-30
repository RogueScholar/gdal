/******************************************************************************
 * Project:  OGR
 * Purpose:  OGRGMLASDriver implementation
 * Author:   Even Rouault, <even dot rouault at spatialys dot com>
 *
 * Initial development funded by the European Earth observation programme
 * Copernicus
 *
 ******************************************************************************
 * Copyright (c) 2016, Even Rouault, <even dot rouault at spatialys dot com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "ogr_gmlas.h"

#include <map>
#include <set>

/************************************************************************/
/*                  OGRGMLASTruncateIdentifier()                        */
/************************************************************************/

CPLString OGRGMLASTruncateIdentifier(const CPLString &osName,
                                     int nIdentMaxLength)
{
    int nExtra = static_cast<int>(osName.size()) - nIdentMaxLength;
    CPLAssert(nExtra > 0);

    // Decompose in tokens
    char **papszTokens = CSLTokenizeString2(osName, "_", CSLT_ALLOWEMPTYTOKENS);
    std::vector<char> achDelimiters;
    std::vector<CPLString> aosTokens;
    for (int j = 0; papszTokens[j] != nullptr; ++j)
    {
        const char *pszToken = papszTokens[j];
        bool bIsCamelCase = false;
        // Split parts like camelCase or CamelCase into several tokens
        if (pszToken[0] != '\0' && pszToken[1] >= 'a' && pszToken[1] <= 'z')
        {
            bIsCamelCase = true;
            bool bLastIsLower = true;
            std::vector<CPLString> aoParts;
            CPLString osCurrentPart;
            osCurrentPart += pszToken[0];
            osCurrentPart += pszToken[1];
            for (int k = 2; pszToken[k]; ++k)
            {
                if (pszToken[k] >= 'A' && pszToken[k] <= 'Z')
                {
                    if (!bLastIsLower)
                    {
                        bIsCamelCase = false;
                        break;
                    }
                    aoParts.push_back(osCurrentPart);
                    osCurrentPart.clear();
                    bLastIsLower = false;
                }
                else
                {
                    bLastIsLower = true;
                }
                osCurrentPart += pszToken[k];
            }
            if (bIsCamelCase)
            {
                if (!osCurrentPart.empty())
                    aoParts.push_back(std::move(osCurrentPart));
                for (size_t k = 0; k < aoParts.size(); ++k)
                {
                    achDelimiters.push_back((j > 0 && k == 0) ? '_' : '\0');
                    aosTokens.push_back(aoParts[k]);
                }
            }
        }
        if (!bIsCamelCase)
        {
            achDelimiters.push_back((j > 0) ? '_' : '\0');
            aosTokens.push_back(pszToken);
        }
    }
    CSLDestroy(papszTokens);

    // Truncate identifier by removing last character of longest part
    std::map<int, std::set<size_t>> oMapLengthToIdx;
    // Ignore last token in map creation
    for (size_t j = 0; j + 1 < aosTokens.size(); ++j)
    {
        const int nTokenLen = static_cast<int>(aosTokens[j].size());
        oMapLengthToIdx[nTokenLen].insert(j);
    }
    int nLastTokenSize = static_cast<int>(aosTokens.back().size());
    if (oMapLengthToIdx.empty())
    {
        if (nLastTokenSize > nExtra)
        {
            aosTokens.back().resize(nLastTokenSize - nExtra);
            nExtra = 0;
        }
    }
    else
    {
        bool bHasDoneSomething = true;
        while (nExtra > 0 && bHasDoneSomething)
        {
            bHasDoneSomething = false;
            auto iter = oMapLengthToIdx.end();
            --iter;
            // Avoid truncating last token unless it is excessively longer
            // than previous ones.
            if (nLastTokenSize > 2 * iter->first)
            {
                aosTokens.back().resize(nLastTokenSize - 1);
                nLastTokenSize--;
                bHasDoneSomething = true;
                nExtra--;
            }
            else if (iter->first > 1)
            {
                // Reduce one token by one character
                const size_t j = *iter->second.begin();
                aosTokens[j].resize(iter->first - 1);

                // Move it to a new bucket
                iter->second.erase(iter->second.begin());
                oMapLengthToIdx[iter->first - 1].insert(j);

                // Remove this bucket if is empty
                if (iter->second.empty())
                {
                    oMapLengthToIdx.erase(iter);
                }

                nExtra--;
                bHasDoneSomething = true;
            }
        }
    }

    // Reassemble truncated parts
    CPLString osNewName;
    for (size_t j = 0; j < aosTokens.size(); ++j)
    {
        if (achDelimiters[j])
            osNewName += achDelimiters[j];
        osNewName += aosTokens[j];
    }

    // If we are still longer than max allowed, truncate beginning of name
    if (nExtra > 0)
    {
        osNewName = osNewName.substr(nExtra);
    }
    CPLAssert(static_cast<int>(osNewName.size()) == nIdentMaxLength);
    return osNewName;
}

/************************************************************************/
/*                      OGRGMLASAddSerialNumber()                       */
/************************************************************************/

CPLString OGRGMLASAddSerialNumber(const CPLString &osNameIn, int iOccurrence,
                                  size_t nOccurrences, int nIdentMaxLength)
{
    CPLString osName(osNameIn);
    const int nDigitsSize = (nOccurrences < 10)    ? 1
                            : (nOccurrences < 100) ? 2
                                                   : 3;
    char szDigits[4];
    snprintf(szDigits, sizeof(szDigits), "%0*d", nDigitsSize, iOccurrence);
    if (nIdentMaxLength >= MIN_VALUE_OF_MAX_IDENTIFIER_LENGTH)
    {
        if (static_cast<int>(osName.size()) < nIdentMaxLength)
        {
            if (static_cast<int>(osName.size()) + nDigitsSize < nIdentMaxLength)
            {
                osName += szDigits;
            }
            else
            {
                osName.resize(nIdentMaxLength - nDigitsSize);
                osName += szDigits;
            }
        }
        else
        {
            const int nTruncatedSize =
                static_cast<int>(osName.size()) - nDigitsSize;
            if (nTruncatedSize >= 0)
                osName.resize(nTruncatedSize);
            osName += szDigits;
        }
    }
    else
    {
        osName += szDigits;
    }
    return osName;
}
