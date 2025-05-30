/******************************************************************************
 *
 * Name:     georaster_rasterband.cpp
 * Project:  Oracle Spatial GeoRaster Driver
 * Purpose:  Implement GeoRasterRasterBand methods
 * Author:   Ivan Lucena [ivan.lucena at oracle.com]
 *
 ******************************************************************************
 * Copyright (c) 2008, Ivan Lucena
 *
 * SPDX-License-Identifier: MIT
 *****************************************************************************/

#include "gdal_priv.h"

#include <string.h>

#include "georaster_priv.h"
#include "cpl_vsi.h"
#include "cpl_error.h"

//  ---------------------------------------------------------------------------
//                                                        GeoRasterRasterBand()
//  ---------------------------------------------------------------------------

GeoRasterRasterBand::GeoRasterRasterBand(GeoRasterDataset *poGDS, int nBandIn,
                                         int nLevel,
                                         GDALDataset *poJP2DatasetIn)
{
    poDS = (GDALDataset *)poGDS;
    poGeoRaster = poGDS->poGeoRaster;
    this->nBand = nBandIn;
    this->eDataType = OWGetDataType(poGeoRaster->sCellDepth.c_str());
    poColorTable = new GDALColorTable();
    poDefaultRAT = nullptr;
    pszVATName = nullptr;
    nRasterXSize = poGeoRaster->nRasterColumns;
    nRasterYSize = poGeoRaster->nRasterRows;
    nBlockXSize = poGeoRaster->nColumnBlockSize;
    nBlockYSize = poGeoRaster->nRowBlockSize;
    dfNoData = 0.0;
    bValidStats = false;
    nOverviewLevel = nLevel;
    papoOverviews = nullptr;
    nOverviewCount = 0;
    pahNoDataArray = nullptr;
    nNoDataArraySz = 0;
    bHasNoDataArray = false;
    dfMin = 0.0;
    dfMax = 0.0;
    dfMean = 0.0;
    dfMedian = 0.0;
    dfMode = 0.0;
    dfStdDev = 0.0;

    poJP2Dataset = poJP2DatasetIn;

    //  -----------------------------------------------------------------------
    //  Initialize overview list
    //  -----------------------------------------------------------------------

    if (nLevel == 0 && poGeoRaster->nPyramidMaxLevel > 0)
    {
        nOverviewCount = poGeoRaster->nPyramidMaxLevel;
        papoOverviews = (GeoRasterRasterBand **)VSIMalloc(
            sizeof(GeoRasterRasterBand *) * nOverviewCount);
        for (int i = 0; i < nOverviewCount; i++)
        {
            papoOverviews[i] = new GeoRasterRasterBand(
                cpl::down_cast<GeoRasterDataset *>(poDS), nBand, i + 1,
                poJP2Dataset);
        }
    }

    //  -----------------------------------------------------------------------
    //  Initialize this band as an overview
    //  -----------------------------------------------------------------------

    if (nLevel)
    {
        double dfScale = pow((double)2.0, (double)nLevel);

        nRasterXSize = (int)floor(nRasterXSize / dfScale);
        nRasterYSize = (int)floor(nRasterYSize / dfScale);

        if (nRasterXSize <= (nBlockXSize / 2.0) &&
            nRasterYSize <= (nBlockYSize / 2.0))
        {
            nBlockXSize = nRasterXSize;
            nBlockYSize = nRasterYSize;
        }
    }

    //  -----------------------------------------------------------------------
    //  Load NoData values and value ranges for this band (layer)
    //  -----------------------------------------------------------------------

    if ((cpl::down_cast<GeoRasterDataset *>(poDS))->bApplyNoDataArray)
    {
        CPLList *psList = nullptr;
        int nLayerCount = 0;
        int nObjCount = 0;

        /*
         *  Count the number of NoData values and value ranges
         */

        for (psList = poGeoRaster->psNoDataList; psList;
             psList = psList->psNext)
        {
            hNoDataItem *phItem = (hNoDataItem *)psList->pData;

            if (phItem->nBand == nBand)
            {
                nLayerCount++;
            }

            if (phItem->nBand == 0)
            {
                nObjCount++;
            }

            if (phItem->nBand > nBand)
            {
                break;
            }
        }

        /*
         * Join the object nodata values to layer NoData values
         */

        nNoDataArraySz = nLayerCount + nObjCount;

        pahNoDataArray =
            (hNoDataItem *)VSIMalloc2(sizeof(hNoDataItem), nNoDataArraySz);

        int i = 0;
        bool bFirst = true;

        for (psList = poGeoRaster->psNoDataList; psList && i < nNoDataArraySz;
             psList = psList->psNext)
        {
            hNoDataItem *phItem = (hNoDataItem *)psList->pData;

            if (phItem->nBand == nBand || phItem->nBand == 0)
            {
                pahNoDataArray[i].nBand = nBand;
                pahNoDataArray[i].dfLower = phItem->dfLower;
                pahNoDataArray[i].dfUpper = phItem->dfUpper;
                i++;

                if (bFirst)
                {
                    bFirst = false;

                    /*
                     * Use the first value to assigned pixel values
                     * on method ApplyNoDataArray()
                     */

                    dfNoData = phItem->dfLower;
                }
            }
        }

        bHasNoDataArray = nNoDataArraySz > 0;
    }
}

//  ---------------------------------------------------------------------------
//                                                       ~GeoRasterRasterBand()
//  ---------------------------------------------------------------------------

GeoRasterRasterBand::~GeoRasterRasterBand()
{
    delete poColorTable;
    delete poDefaultRAT;

    CPLFree(pszVATName);
    CPLFree(pahNoDataArray);

    if (nOverviewCount && papoOverviews)
    {
        for (int i = 0; i < nOverviewCount; i++)
        {
            delete papoOverviews[i];
        }

        CPLFree(papoOverviews);
    }
}

//  ---------------------------------------------------------------------------
//                                                                 IReadBlock()
//  ---------------------------------------------------------------------------

CPLErr GeoRasterRasterBand::IReadBlock(int nBlockXOff, int nBlockYOff,
                                       void *pImage)
{
    if (poJP2Dataset)
    {
        int nXOff = nBlockXOff * poGeoRaster->nColumnBlockSize;
        int nYOff = nBlockYOff * poGeoRaster->nRowBlockSize;
        int nXSize = poGeoRaster->nColumnBlockSize;
        int nYSize = poGeoRaster->nRowBlockSize;
        int nBufXSize = nBlockXSize;
        int nBufYSize = nBlockYSize;

        return GDALDatasetRasterIO(poJP2Dataset, GF_Read, nXOff, nYOff, nXSize,
                                   nYSize, pImage, nBufXSize, nBufYSize,
                                   eDataType, 1, &nBand, 0, 0, 0);
    }

    if (poGeoRaster->GetDataBlock(nBand, nOverviewLevel, nBlockXOff, nBlockYOff,
                                  pImage))
    {
        if (bHasNoDataArray)
        {
            ApplyNoDataArray(pImage);
        }

        return CE_None;
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Error reading GeoRaster offset X (%d) offset Y (%d) "
                 "band (%d)",
                 nBlockXOff, nBlockYOff, nBand);
        return CE_Failure;
    }
}

//  ---------------------------------------------------------------------------
//                                                                IWriteBlock()
//  ---------------------------------------------------------------------------

CPLErr GeoRasterRasterBand::IWriteBlock(int nBlockXOff, int nBlockYOff,
                                        void *pImage)
{
    if (poJP2Dataset)
    {
        int nXOff = nBlockXOff * poGeoRaster->nColumnBlockSize;
        int nYOff = nBlockYOff * poGeoRaster->nRowBlockSize;
        int nXSize = poGeoRaster->nColumnBlockSize;
        int nYSize = poGeoRaster->nRowBlockSize;
        int nBufXSize = nBlockXSize;
        int nBufYSize = nBlockYSize;

        return GDALDatasetRasterIO(poJP2Dataset, GF_Write, nXOff, nYOff, nXSize,
                                   nYSize, pImage, nBufXSize, nBufYSize,
                                   eDataType, 1, &nBand, 0, 0, 0);
    }

    if (poGeoRaster->SetDataBlock(nBand, nOverviewLevel, nBlockXOff, nBlockYOff,
                                  pImage))
    {
        return CE_None;
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Error writing GeoRaster offset X (%d) offset Y (%d) "
                 "band (%d)",
                 nBlockXOff, nBlockYOff, nBand);
        return CE_Failure;
    }
}

//  ---------------------------------------------------------------------------
//                                                     GetColorInterpretation()
//  ---------------------------------------------------------------------------

GDALColorInterp GeoRasterRasterBand::GetColorInterpretation()
{
    GeoRasterDataset *poGDS = cpl::down_cast<GeoRasterDataset *>(poDS);

    if (eDataType == GDT_Byte && poGDS->nBands > 2)
    {
        if (nBand == poGeoRaster->iDefaultRedBand)
        {
            return GCI_RedBand;
        }
        else if (nBand == poGeoRaster->iDefaultGreenBand)
        {
            return GCI_GreenBand;
        }
        else if (nBand == poGeoRaster->iDefaultBlueBand)
        {
            return GCI_BlueBand;
        }
        else
        {
            if (nBand == 4 && poGDS->nBands == 4 &&
                poGeoRaster->iDefaultRedBand == 1 &&
                poGeoRaster->iDefaultGreenBand == 2 &&
                poGeoRaster->iDefaultBlueBand == 3)
            {
                return GCI_AlphaBand;
            }
            else
            {
                return GCI_Undefined;
            }
        }
    }

    if (poGeoRaster->HasColorMap(nBand))
    {
        return GCI_PaletteIndex;
    }
    else
    {
        return GCI_GrayIndex;
    }
}

//  ---------------------------------------------------------------------------
//                                                              GetColorTable()
//  ---------------------------------------------------------------------------

GDALColorTable *GeoRasterRasterBand::GetColorTable()
{
    poGeoRaster->GetColorMap(nBand, poColorTable);

    if (poColorTable->GetColorEntryCount() == 0)
    {
        return nullptr;
    }

    return poColorTable;
}

//  ---------------------------------------------------------------------------
//                                                              SetColorTable()
//  ---------------------------------------------------------------------------

CPLErr GeoRasterRasterBand::SetColorTable(GDALColorTable *poInColorTable)
{
    if (poInColorTable == nullptr)
    {
        return CE_None;
    }

    if (poInColorTable->GetColorEntryCount() == 0)
    {
        return CE_None;
    }

    delete poColorTable;

    poColorTable = poInColorTable->Clone();

    poGeoRaster->SetColorMap(nBand, poColorTable);

    return CE_None;
}

//  ---------------------------------------------------------------------------
//                                                                 GetMinimum()
//  ---------------------------------------------------------------------------

double GeoRasterRasterBand::GetMinimum(int *pbSuccess)
{
    *pbSuccess = (int)bValidStats;

    return dfMin;
}

//  ---------------------------------------------------------------------------
//                                                                 GetMaximum()
//  ---------------------------------------------------------------------------

double GeoRasterRasterBand::GetMaximum(int *pbSuccess)
{
    *pbSuccess = (int)bValidStats;

    return dfMax;
}

//  ---------------------------------------------------------------------------
//                                                              GetStatistics()
//  ---------------------------------------------------------------------------

CPLErr GeoRasterRasterBand::GetStatistics(int bApproxOK, int bForce,
                                          double *pdfMin, double *pdfMax,
                                          double *pdfMean, double *pdfStdDev)
{
    (void)bForce;
    (void)bApproxOK;

    char szMin[MAX_DOUBLE_STR_REP + 1] = {0};
    char szMax[MAX_DOUBLE_STR_REP + 1] = {0};
    char szMean[MAX_DOUBLE_STR_REP + 1] = {0};
    char szMedian[MAX_DOUBLE_STR_REP + 1] = {0};
    char szMode[MAX_DOUBLE_STR_REP + 1] = {0};
    char szStdDev[MAX_DOUBLE_STR_REP + 1] = {0};
    char szSampling[MAX_DOUBLE_STR_REP + 1] = {0};

    if (!bValidStats)
    {
        bValidStats =
            poGeoRaster->GetStatistics(nBand, szMin, szMax, szMean, szMedian,
                                       szMode, szStdDev, szSampling);
    }

    if (bValidStats)
    {
        dfMin = CPLScanDouble(szMin, MAX_DOUBLE_STR_REP);
        dfMax = CPLScanDouble(szMax, MAX_DOUBLE_STR_REP);
        dfMean = CPLScanDouble(szMean, MAX_DOUBLE_STR_REP);
        dfMedian = CPLScanDouble(szMedian, MAX_DOUBLE_STR_REP);
        dfMode = CPLScanDouble(szMode, MAX_DOUBLE_STR_REP);
        dfStdDev = CPLScanDouble(szStdDev, MAX_DOUBLE_STR_REP);

        SetMetadataItem("STATISTICS_MINIMUM", szMin);
        SetMetadataItem("STATISTICS_MAXIMUM", szMax);
        SetMetadataItem("STATISTICS_MEAN", szMean);
        SetMetadataItem("STATISTICS_MEDIAN", szMedian);
        SetMetadataItem("STATISTICS_MODE", szMode);
        SetMetadataItem("STATISTICS_STDDEV", szStdDev);
        SetMetadataItem("STATISTICS_SKIPFACTORX", szSampling);
        SetMetadataItem("STATISTICS_SKIPFACTORY", szSampling);

        *pdfMin = dfMin;
        *pdfMax = dfMax;
        *pdfMean = dfMean;
        *pdfStdDev = dfStdDev;

        return CE_None;
    }

    return CE_Failure;
}

//  ---------------------------------------------------------------------------
//                                                              SetStatistics()
//  ---------------------------------------------------------------------------

CPLErr GeoRasterRasterBand::SetStatistics(double dfMinIn, double dfMaxIn,
                                          double dfMeanIn, double dfStdDevIn)
{
    this->dfMin = dfMinIn;
    this->dfMax = dfMaxIn;
    this->dfMean = dfMeanIn;
    this->dfStdDev = dfStdDevIn;

    return CE_None;
}

//  ---------------------------------------------------------------------------
//                                                             GetNoDataValue()
//  ---------------------------------------------------------------------------

double GeoRasterRasterBand::GetNoDataValue(int *pbSuccess)
{
    if (pbSuccess)
    {
        if (nNoDataArraySz)
        {
            *pbSuccess = true;
        }
        else
        {
            *pbSuccess = (int)poGeoRaster->GetNoData(nBand, &dfNoData);
        }
    }

    return dfNoData;
}

//  ---------------------------------------------------------------------------
//                                                             SetNoDataValue()
//  ---------------------------------------------------------------------------

CPLErr GeoRasterRasterBand::SetNoDataValue(double dfNoDataValue)
{
    const char *pszFormat =
        (eDataType == GDT_Float32 || eDataType == GDT_Float64) ? "%f" : "%.0f";

    poGeoRaster->SetNoData((poDS->GetRasterCount() == 1) ? 0 : nBand,
                           CPLSPrintf(pszFormat, dfNoDataValue));

    return CE_None;
}

//  ---------------------------------------------------------------------------
//                                                              SetDefaultRAT()
//  ---------------------------------------------------------------------------

CPLErr GeoRasterRasterBand::SetDefaultRAT(const GDALRasterAttributeTable *poRAT)
{
    GeoRasterDataset *poGDS = cpl::down_cast<GeoRasterDataset *>(poDS);

    if (!poRAT)
    {
        return CE_Failure;
    }

    if (poDefaultRAT)
    {
        delete poDefaultRAT;
    }

    poDefaultRAT = poRAT->Clone();

    // ----------------------------------------------------------
    // Check if RAT is just colortable and/or histogram
    // ----------------------------------------------------------

    int nColCount = poRAT->GetColumnCount();

    for (int iCol = 0; iCol < poRAT->GetColumnCount(); iCol++)
    {
        const CPLString sColName = poRAT->GetNameOfCol(iCol);

        if (EQUAL(sColName, "histogram") || EQUAL(sColName, "red") ||
            EQUAL(sColName, "green") || EQUAL(sColName, "blue") ||
            EQUAL(sColName, "opacity"))
        {
            nColCount--;
        }
    }

    if (nColCount < 2)
    {
        delete poDefaultRAT;

        poDefaultRAT = nullptr;

        return CE_None;
    }

    // ----------------------------------------------------------
    // Format Table description
    // ----------------------------------------------------------

    CPLString osDescription = "( ID NUMBER";

    for (int iCol = 0; iCol < poRAT->GetColumnCount(); iCol++)
    {
        osDescription += ", ";
        osDescription += poRAT->GetNameOfCol(iCol);

        if (poRAT->GetTypeOfCol(iCol) == GFT_Integer)
        {
            osDescription += " NUMBER";
        }
        if (poRAT->GetTypeOfCol(iCol) == GFT_Real)
        {
            osDescription += " NUMBER";
        }
        if (poRAT->GetTypeOfCol(iCol) == GFT_String)
        {
            osDescription += CPLSPrintf(" VARCHAR2(%d)", MAXLEN_VATSTR);
        }
    }
    osDescription += " )";

    // ----------------------------------------------------------
    // Create VAT named based on RDT and RID and Layer (nBand)
    // ----------------------------------------------------------

    if (poGeoRaster->sValueAttributeTab.length() > 0)
    {
        pszVATName = CPLStrdup(poGeoRaster->sValueAttributeTab.c_str());
    }

    if (!pszVATName)
    {
        pszVATName = CPLStrdup(CPLSPrintf("RAT_%s_%lld_%d",
                                          poGeoRaster->sDataTable.c_str(),
                                          poGeoRaster->nRasterId, nBand));
    }

    // ----------------------------------------------------------
    // Create VAT table
    // ----------------------------------------------------------

    OWStatement *poStmt = poGeoRaster->poConnection->CreateStatement(
        CPLSPrintf("DECLARE\n"
                   "  TAB VARCHAR2(128) := UPPER(:1);\n"
                   "  CNT NUMBER        := 0;\n"
                   "BEGIN\n"
                   "  EXECUTE IMMEDIATE 'SELECT COUNT(*) FROM USER_TABLES\n"
                   "    WHERE TABLE_NAME = :1' INTO CNT USING TAB;\n"
                   "\n"
                   "  IF NOT CNT = 0 THEN\n"
                   "    EXECUTE IMMEDIATE 'DROP TABLE '||TAB||' PURGE';\n"
                   "  END IF;\n"
                   "\n"
                   "  EXECUTE IMMEDIATE 'CREATE TABLE '||TAB||' %s';\n"
                   "END;",
                   osDescription.c_str()));

    poStmt->Bind(pszVATName);

    if (!poStmt->Execute())
    {
        delete poStmt;
        CPLError(CE_Failure, CPLE_AppDefined, "Create VAT Table Error!");
        return CE_Failure;
    }

    delete poStmt;

    // ----------------------------------------------------------
    // Insert Data to VAT
    // ----------------------------------------------------------

    int iEntry = 0;
    int nEntryCount = poRAT->GetRowCount();
    int nColunsCount = poRAT->GetColumnCount();
    int nVATStrSize = MAXLEN_VATSTR * poGeoRaster->poConnection->GetCharSize();

    // ---------------------------
    // Allocate array of buffers
    // ---------------------------

    void **papWriteFields =
        (void **)VSIMalloc2(sizeof(void *), nColunsCount + 1);

    papWriteFields[0] =
        (void *)VSIMalloc3(sizeof(int), sizeof(int), nEntryCount);  // ID field

    for (int iCol = 0; iCol < nColunsCount; iCol++)
    {
        if (poRAT->GetTypeOfCol(iCol) == GFT_String)
        {
            papWriteFields[iCol + 1] =
                (void *)VSIMalloc3(sizeof(char), nVATStrSize, nEntryCount);
        }
        if (poRAT->GetTypeOfCol(iCol) == GFT_Integer)
        {
            papWriteFields[iCol + 1] =
                (void *)VSIMalloc3(sizeof(int), sizeof(int), nEntryCount);
        }
        if (poRAT->GetTypeOfCol(iCol) == GFT_Real)
        {
            papWriteFields[iCol + 1] =
                (void *)VSIMalloc3(sizeof(double), sizeof(double), nEntryCount);
        }
    }

    // ---------------------------
    // Load data to buffers
    // ---------------------------

    for (iEntry = 0; iEntry < nEntryCount; iEntry++)
    {
        ((int *)(papWriteFields[0]))[iEntry] = iEntry;  // ID field

        for (int iCol = 0; iCol < nColunsCount; iCol++)
        {
            if (poRAT->GetTypeOfCol(iCol) == GFT_String)
            {

                int nOffset = iEntry * nVATStrSize;
                char *pszTarget = ((char *)papWriteFields[iCol + 1]) + nOffset;
                const char *pszStrValue = poRAT->GetValueAsString(iEntry, iCol);
                int nLen = static_cast<int>(strlen(pszStrValue));
                nLen =
                    nLen > (nVATStrSize - 1) ? nVATStrSize : (nVATStrSize - 1);
                strncpy(pszTarget, pszStrValue, nLen);
                pszTarget[nLen] = '\0';
            }
            if (poRAT->GetTypeOfCol(iCol) == GFT_Integer)
            {
                ((int *)(papWriteFields[iCol + 1]))[iEntry] =
                    poRAT->GetValueAsInt(iEntry, iCol);
            }
            if (poRAT->GetTypeOfCol(iCol) == GFT_Real)
            {
                ((double *)(papWriteFields[iCol + 1]))[iEntry] =
                    poRAT->GetValueAsDouble(iEntry, iCol);
            }
        }
    }

    // ---------------------------
    // Prepare insert statement
    // ---------------------------

    CPLString osInsert = CPLSPrintf("INSERT INTO %s VALUES (", pszVATName);

    for (int iCol = 0; iCol < (nColunsCount + 1); iCol++)
    {
        if (iCol > 0)
        {
            osInsert.append(", ");
        }
        osInsert.append(CPLSPrintf(":%d", iCol + 1));
    }
    osInsert.append(")");

    poStmt = poGeoRaster->poConnection->CreateStatement(osInsert.c_str());

    // ---------------------------
    // Bind buffers to columns
    // ---------------------------

    poStmt->Bind((int *)papWriteFields[0]);  // ID field

    for (int iCol = 0; iCol < nColunsCount; iCol++)
    {
        if (poRAT->GetTypeOfCol(iCol) == GFT_String)
        {
            poStmt->Bind((char *)papWriteFields[iCol + 1], nVATStrSize);
        }
        if (poRAT->GetTypeOfCol(iCol) == GFT_Integer)
        {
            poStmt->Bind((int *)papWriteFields[iCol + 1]);
        }
        if (poRAT->GetTypeOfCol(iCol) == GFT_Real)
        {
            poStmt->Bind((double *)papWriteFields[iCol + 1]);
        }
    }

    if (poStmt->Execute(iEntry))
    {
        poGDS->poGeoRaster->SetVAT(nBand, pszVATName);
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Insert VAT Error!");
    }

    // ---------------------------
    // Clean up
    // ---------------------------

    for (int iCol = 0; iCol < (nColunsCount + 1); iCol++)
    {
        CPLFree(papWriteFields[iCol]);
    }

    CPLFree(papWriteFields);

    delete poStmt;

    return CE_None;
}

//  ---------------------------------------------------------------------------
//                                                              GetDefaultRAT()
//  ---------------------------------------------------------------------------

GDALRasterAttributeTable *GeoRasterRasterBand::GetDefaultRAT()
{
    if (poDefaultRAT)
    {
        return poDefaultRAT;
    }
    else
    {
        poDefaultRAT = new GDALDefaultRasterAttributeTable();
    }

    GeoRasterDataset *poGDS = cpl::down_cast<GeoRasterDataset *>(poDS);

    // ----------------------------------------------------------
    // Get the name of the VAT Table
    // ----------------------------------------------------------

    char *l_pszVATName = poGDS->poGeoRaster->GetVAT(nBand);

    if (l_pszVATName == nullptr)
    {
        return nullptr;
    }

    OCIParam *phDesc =
        poGDS->poGeoRaster->poConnection->GetDescription(l_pszVATName);

    if (phDesc == nullptr)
    {
        return nullptr;
    }

    // ----------------------------------------------------------
    // Create the RAT and the SELECT statement based on fields description.
    // ----------------------------------------------------------

    int iCol = 0;
    char szField[OWNAME];
    int hType = 0;
    int nSize = 0;
    int nPrecision = 0;
    signed short nScale = 0;

    CPLString osColumnList;

    while (poGDS->poGeoRaster->poConnection->GetNextField(
        phDesc, iCol, szField, &hType, &nSize, &nPrecision, &nScale))
    {
        switch (hType)
        {
            case SQLT_FLT:
                poDefaultRAT->CreateColumn(szField, GFT_Real, GFU_Generic);
                break;
            case SQLT_NUM:
                if (nPrecision == 0)
                {
                    poDefaultRAT->CreateColumn(szField, GFT_Integer,
                                               GFU_Generic);
                }
                else
                {
                    poDefaultRAT->CreateColumn(szField, GFT_Real, GFU_Generic);
                }
                break;
            case SQLT_CHR:
            case SQLT_AFC:
            case SQLT_DAT:
            case SQLT_DATE:
            case SQLT_TIMESTAMP:
            case SQLT_TIMESTAMP_TZ:
            case SQLT_TIMESTAMP_LTZ:
            case SQLT_TIME:
            case SQLT_TIME_TZ:
                poDefaultRAT->CreateColumn(szField, GFT_String, GFU_Generic);
                break;
            default:
                CPLDebug("GEORASTER",
                         "VAT (%s) Column (%s) type (%d) not supported"
                         "as GDAL RAT",
                         l_pszVATName, szField, hType);
                continue;
        }
        osColumnList +=
            CPLSPrintf("substr(%s,1,%d),", szField, MIN(nSize, OWNAME));

        iCol++;
    }

    if (!osColumnList.empty())
        osColumnList.pop_back();  // remove the last comma

    // ----------------------------------------------------------
    // Read VAT and load RAT
    // ----------------------------------------------------------

    OWStatement *poStmt = poGeoRaster->poConnection->CreateStatement(
        CPLSPrintf("SELECT %s FROM %s", osColumnList.c_str(), l_pszVATName));

    char **papszValue = (char **)CPLCalloc(sizeof(char *), iCol + 1);

    int i = 0;

    for (i = 0; i < iCol; i++)
    {
        papszValue[i] = (char *)CPLCalloc(1, OWNAME + 1);
        poStmt->Define(papszValue[i]);
    }

    if (!poStmt->Execute())
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Error reading VAT %s",
                 l_pszVATName);
        CSLDestroy(papszValue);
        delete poStmt;
        return nullptr;
    }

    int iRow = 0;

    while (poStmt->Fetch())
    {
        for (i = 0; i < iCol; i++)
        {
            poDefaultRAT->SetValue(iRow, i, papszValue[i]);
        }
        iRow++;
    }

    CSLDestroy(papszValue);

    delete poStmt;

    CPLFree(l_pszVATName);

    return poDefaultRAT;
}

//  ---------------------------------------------------------------------------
//                                                           GetOverviewCount()
//  ---------------------------------------------------------------------------

int GeoRasterRasterBand::GetOverviewCount()
{
    return nOverviewCount;
}

//  ---------------------------------------------------------------------------
//                                                           GetOverviewCount()
//  ---------------------------------------------------------------------------

GDALRasterBand *GeoRasterRasterBand::GetOverview(int nLevel)
{
    if (nLevel < nOverviewCount && papoOverviews[nLevel])
    {
        return (GDALRasterBand *)papoOverviews[nLevel];
    }
    return (GDALRasterBand *)nullptr;
}

//  ---------------------------------------------------------------------------
//                                                             CreateMaskBand()
//  ---------------------------------------------------------------------------

CPLErr GeoRasterRasterBand::CreateMaskBand(int /*nFlags*/)
{
    if (!poGeoRaster->bHasBitmapMask)
    {
        return CE_Failure;
    }

    return CE_None;
}

//  ---------------------------------------------------------------------------
//                                                                GetMaskBand()
//  ---------------------------------------------------------------------------

GDALRasterBand *GeoRasterRasterBand::GetMaskBand()
{
    GeoRasterDataset *poGDS = cpl::down_cast<GeoRasterDataset *>(poDS);

    if (poGDS->poMaskBand != nullptr)
    {
        return (GDALRasterBand *)poGDS->poMaskBand;
    }

    return (GDALRasterBand *)nullptr;
}

//  ---------------------------------------------------------------------------
//                                                               GetMaskFlags()
//  ---------------------------------------------------------------------------

int GeoRasterRasterBand::GetMaskFlags()
{
    GeoRasterDataset *poGDS = cpl::down_cast<GeoRasterDataset *>(poDS);

    if (poGDS->poMaskBand != nullptr)
    {
        return GMF_PER_DATASET;
    }

    return GMF_ALL_VALID;
}

//  ---------------------------------------------------------------------------
//                                                            ApplyNoDataArray()
//  ---------------------------------------------------------------------------

void GeoRasterRasterBand::ApplyNoDataArray(void *pBuffer) const
{
    size_t i = 0;
    int j = 0;
    size_t n = static_cast<size_t>(nBlockXSize) * nBlockYSize;

    switch (eDataType)
    {
        case GDT_Byte:
        {
            GByte *pbBuffer = (GByte *)pBuffer;

            for (i = 0; i < n; i++)
            {
                for (j = 0; j < nNoDataArraySz; j++)
                {
                    if (pbBuffer[i] == (GByte)pahNoDataArray[j].dfLower ||
                        (pbBuffer[i] > (GByte)pahNoDataArray[j].dfLower &&
                         pbBuffer[i] < (GByte)pahNoDataArray[j].dfUpper))
                    {
                        pbBuffer[i] = (GByte)dfNoData;
                    }
                }
            }

            break;
        }
        case GDT_Float32:
        case GDT_CFloat32:
        {
            float *pfBuffer = (float *)pBuffer;

            for (i = 0; i < n; i++)
            {
                for (j = 0; j < nNoDataArraySz; j++)
                {
                    if (pfBuffer[i] == (float)pahNoDataArray[j].dfLower ||
                        (pfBuffer[i] > (float)pahNoDataArray[j].dfLower &&
                         pfBuffer[i] < (float)pahNoDataArray[j].dfUpper))
                    {
                        pfBuffer[i] = (float)dfNoData;
                    }
                }
            }

            break;
        }
        case GDT_Float64:
        case GDT_CFloat64:
        {
            double *pdfBuffer = (double *)pBuffer;

            for (i = 0; i < n; i++)
            {
                for (j = 0; j < nNoDataArraySz; j++)
                {
                    if (pdfBuffer[i] == (double)pahNoDataArray[j].dfLower ||
                        (pdfBuffer[i] > (double)pahNoDataArray[j].dfLower &&
                         pdfBuffer[i] < (double)pahNoDataArray[j].dfUpper))
                    {
                        pdfBuffer[i] = (double)dfNoData;
                    }
                }
            }

            break;
        }
        case GDT_Int16:
        case GDT_CInt16:
        {
            GInt16 *pnBuffer = (GInt16 *)pBuffer;

            for (i = 0; i < n; i++)
            {
                for (j = 0; j < nNoDataArraySz; j++)
                {
                    if (pnBuffer[i] == (GInt16)pahNoDataArray[j].dfLower ||
                        (pnBuffer[i] > (GInt16)pahNoDataArray[j].dfLower &&
                         pnBuffer[i] < (GInt16)pahNoDataArray[j].dfUpper))
                    {
                        pnBuffer[i] = (GInt16)dfNoData;
                    }
                }
            }

            break;
        }
        case GDT_Int32:
        case GDT_CInt32:
        {
            GInt32 *pnBuffer = (GInt32 *)pBuffer;

            for (i = 0; i < n; i++)
            {
                for (j = 0; j < nNoDataArraySz; j++)
                {
                    if (pnBuffer[i] == (GInt32)pahNoDataArray[j].dfLower ||
                        (pnBuffer[i] > (GInt32)pahNoDataArray[j].dfLower &&
                         pnBuffer[i] < (GInt32)pahNoDataArray[j].dfUpper))
                    {
                        pnBuffer[i] = (GInt32)dfNoData;
                    }
                }
            }

            break;
        }
        case GDT_UInt16:
        {
            GUInt16 *pnBuffer = (GUInt16 *)pBuffer;

            for (i = 0; i < n; i++)
            {
                for (j = 0; j < nNoDataArraySz; j++)
                {
                    if (pnBuffer[i] == (GUInt16)pahNoDataArray[j].dfLower ||
                        (pnBuffer[i] > (GUInt16)pahNoDataArray[j].dfLower &&
                         pnBuffer[i] < (GUInt16)pahNoDataArray[j].dfUpper))
                    {
                        pnBuffer[i] = (GUInt16)dfNoData;
                    }
                }
            }

            break;
        }
        case GDT_UInt32:
        {
            GUInt32 *pnBuffer = (GUInt32 *)pBuffer;

            for (i = 0; i < n; i++)
            {
                for (j = 0; j < nNoDataArraySz; j++)
                {
                    if (pnBuffer[i] == (GUInt32)pahNoDataArray[j].dfLower ||
                        (pnBuffer[i] > (GUInt32)pahNoDataArray[j].dfLower &&
                         pnBuffer[i] < (GUInt32)pahNoDataArray[j].dfUpper))
                    {
                        pnBuffer[i] = (GUInt32)dfNoData;
                    }
                }
            }

            break;
        }
        default:;
    }
}
