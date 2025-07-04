/******************************************************************************
 *
 * Project:  GDAL WMTS driver
 * Purpose:  Implement GDAL WMTS support
 * Author:   Even Rouault, <even dot rouault at spatialys dot com>
 * Funded by Land Information New Zealand (LINZ)
 *
 **********************************************************************
 * Copyright (c) 2015, Even Rouault <even dot rouault at spatialys dot com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_http.h"
#include "cpl_minixml.h"
#include "gdal_frmts.h"
#include "gdal_pam.h"
#include "ogr_spatialref.h"
#include "../vrt/gdal_vrt.h"
#include "wmtsdrivercore.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <vector>
#include <limits>

extern "C" void GDALRegister_WMTS();

// g++ -g -Wall -fPIC frmts/wmts/wmtsdataset.cpp -shared -o gdal_WMTS.so -Iport
// -Igcore -Iogr -Iogr/ogrsf_frmts -L. -lgdal

/* Set in stone by WMTS spec. In pixel/meter */
#define WMTS_PITCH 0.00028

#define WMTS_WGS84_DEG_PER_METER (180 / M_PI / SRS_WGS84_SEMIMAJOR)

typedef enum
{
    AUTO,
    LAYER_BBOX,
    TILE_MATRIX_SET,
    MOST_PRECISE_TILE_MATRIX
} ExtentMethod;

/************************************************************************/
/* ==================================================================== */
/*                            WMTSTileMatrix                            */
/* ==================================================================== */
/************************************************************************/

class WMTSTileMatrix
{
  public:
    CPLString osIdentifier{};
    double dfScaleDenominator = 0;
    double dfPixelSize = 0;
    double dfTLX = 0;
    double dfTLY = 0;
    int nTileWidth = 0;
    int nTileHeight = 0;
    int nMatrixWidth = 0;
    int nMatrixHeight = 0;

    OGREnvelope GetExtent() const
    {
        OGREnvelope sExtent;
        sExtent.MinX = dfTLX;
        sExtent.MaxX = dfTLX + nMatrixWidth * dfPixelSize * nTileWidth;
        sExtent.MaxY = dfTLY;
        sExtent.MinY = dfTLY - nMatrixHeight * dfPixelSize * nTileHeight;
        return sExtent;
    }
};

/************************************************************************/
/* ==================================================================== */
/*                          WMTSTileMatrixLimits                        */
/* ==================================================================== */
/************************************************************************/

class WMTSTileMatrixLimits
{
  public:
    CPLString osIdentifier{};
    int nMinTileRow = 0;
    int nMaxTileRow = 0;
    int nMinTileCol = 0;
    int nMaxTileCol = 0;

    OGREnvelope GetExtent(const WMTSTileMatrix &oTM) const
    {
        OGREnvelope sExtent;
        const double dfTileWidthUnits = oTM.dfPixelSize * oTM.nTileWidth;
        const double dfTileHeightUnits = oTM.dfPixelSize * oTM.nTileHeight;
        sExtent.MinX = oTM.dfTLX + nMinTileCol * dfTileWidthUnits;
        sExtent.MaxY = oTM.dfTLY - nMinTileRow * dfTileHeightUnits;
        sExtent.MaxX = oTM.dfTLX + (nMaxTileCol + 1) * dfTileWidthUnits;
        sExtent.MinY = oTM.dfTLY - (nMaxTileRow + 1) * dfTileHeightUnits;
        return sExtent;
    }
};

/************************************************************************/
/* ==================================================================== */
/*                          WMTSTileMatrixSet                           */
/* ==================================================================== */
/************************************************************************/

class WMTSTileMatrixSet
{
  public:
    OGRSpatialReference oSRS{};
    CPLString osSRS{};
    bool bBoundingBoxValid = false;
    OGREnvelope sBoundingBox{}; /* expressed in TMS SRS */
    std::vector<WMTSTileMatrix> aoTM{};

    WMTSTileMatrixSet()
    {
        oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    }
};

/************************************************************************/
/* ==================================================================== */
/*                              WMTSDataset                             */
/* ==================================================================== */
/************************************************************************/

class WMTSDataset final : public GDALPamDataset
{
    friend class WMTSBand;

    CPLString osLayer{};
    CPLString osTMS{};
    CPLString osXML{};
    CPLString osURLFeatureInfoTemplate{};
    WMTSTileMatrixSet oTMS{};

    CPLStringList m_aosHTTPOptions{};

    std::vector<GDALDataset *> apoDatasets{};
    OGRSpatialReference m_oSRS{};
    GDALGeoTransform m_gt{};

    CPLString osLastGetFeatureInfoURL{};
    CPLString osMetadataItemGetFeatureInfo{};

    static CPLStringList BuildHTTPRequestOpts(CPLString osOtherXML);
    static CPLXMLNode *GetCapabilitiesResponse(const CPLString &osFilename,
                                               CSLConstList papszHTTPOptions);
    static CPLString FixCRSName(const char *pszCRS);
    static CPLString Replace(const CPLString &osStr, const char *pszOld,
                             const char *pszNew);
    static CPLString GetOperationKVPURL(CPLXMLNode *psXML,
                                        const char *pszOperation);
    static int ReadTMS(CPLXMLNode *psContents, const CPLString &osIdentifier,
                       const CPLString &osMaxTileMatrixIdentifier,
                       int nMaxZoomLevel, WMTSTileMatrixSet &oTMS,
                       bool &bHasWarnedAutoSwap);
    static int ReadTMLimits(
        CPLXMLNode *psTMSLimits,
        std::map<CPLString, WMTSTileMatrixLimits> &aoMapTileMatrixLimits);

  public:
    WMTSDataset();
    virtual ~WMTSDataset();

    virtual CPLErr GetGeoTransform(GDALGeoTransform &gt) const override;
    const OGRSpatialReference *GetSpatialRef() const override;
    virtual const char *GetMetadataItem(const char *pszName,
                                        const char *pszDomain) override;

    static GDALDataset *Open(GDALOpenInfo *);
    static GDALDataset *CreateCopy(const char *pszFilename,
                                   GDALDataset *poSrcDS, CPL_UNUSED int bStrict,
                                   CPL_UNUSED char **papszOptions,
                                   CPL_UNUSED GDALProgressFunc pfnProgress,
                                   CPL_UNUSED void *pProgressData);

  protected:
    virtual int CloseDependentDatasets() override;

    virtual CPLErr IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                             int nXSize, int nYSize, void *pData, int nBufXSize,
                             int nBufYSize, GDALDataType eBufType,
                             int nBandCount, BANDMAP_TYPE panBandMap,
                             GSpacing nPixelSpace, GSpacing nLineSpace,
                             GSpacing nBandSpace,
                             GDALRasterIOExtraArg *psExtraArg) override;
};

/************************************************************************/
/* ==================================================================== */
/*                               WMTSBand                               */
/* ==================================================================== */
/************************************************************************/

class WMTSBand final : public GDALPamRasterBand
{
  public:
    WMTSBand(WMTSDataset *poDS, int nBand, GDALDataType eDataType);

    virtual GDALRasterBand *GetOverview(int nLevel) override;
    virtual int GetOverviewCount() override;
    virtual GDALColorInterp GetColorInterpretation() override;
    virtual const char *GetMetadataItem(const char *pszName,
                                        const char *pszDomain) override;

  protected:
    virtual CPLErr IReadBlock(int nBlockXOff, int nBlockYOff,
                              void *pImage) override;
    virtual CPLErr IRasterIO(GDALRWFlag, int, int, int, int, void *, int, int,
                             GDALDataType, GSpacing, GSpacing,
                             GDALRasterIOExtraArg *psExtraArg) override;
};

/************************************************************************/
/*                            WMTSBand()                                */
/************************************************************************/

WMTSBand::WMTSBand(WMTSDataset *poDSIn, int nBandIn, GDALDataType eDataTypeIn)
{
    poDS = poDSIn;
    nBand = nBandIn;
    eDataType = eDataTypeIn;
    poDSIn->apoDatasets[0]->GetRasterBand(1)->GetBlockSize(&nBlockXSize,
                                                           &nBlockYSize);
}

/************************************************************************/
/*                            IReadBlock()                              */
/************************************************************************/

CPLErr WMTSBand::IReadBlock(int nBlockXOff, int nBlockYOff, void *pImage)
{
    WMTSDataset *poGDS = cpl::down_cast<WMTSDataset *>(poDS);
    return poGDS->apoDatasets[0]->GetRasterBand(nBand)->ReadBlock(
        nBlockXOff, nBlockYOff, pImage);
}

/************************************************************************/
/*                             IRasterIO()                              */
/************************************************************************/

CPLErr WMTSBand::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff, int nXSize,
                           int nYSize, void *pData, int nBufXSize,
                           int nBufYSize, GDALDataType eBufType,
                           GSpacing nPixelSpace, GSpacing nLineSpace,
                           GDALRasterIOExtraArg *psExtraArg)
{
    WMTSDataset *poGDS = cpl::down_cast<WMTSDataset *>(poDS);

    if ((nBufXSize < nXSize || nBufYSize < nYSize) &&
        poGDS->apoDatasets.size() > 1 && eRWFlag == GF_Read)
    {
        int bTried;
        CPLErr eErr = TryOverviewRasterIO(
            eRWFlag, nXOff, nYOff, nXSize, nYSize, pData, nBufXSize, nBufYSize,
            eBufType, nPixelSpace, nLineSpace, psExtraArg, &bTried);
        if (bTried)
            return eErr;
    }

    return poGDS->apoDatasets[0]->GetRasterBand(nBand)->RasterIO(
        eRWFlag, nXOff, nYOff, nXSize, nYSize, pData, nBufXSize, nBufYSize,
        eBufType, nPixelSpace, nLineSpace, psExtraArg);
}

/************************************************************************/
/*                         GetOverviewCount()                           */
/************************************************************************/

int WMTSBand::GetOverviewCount()
{
    WMTSDataset *poGDS = cpl::down_cast<WMTSDataset *>(poDS);

    if (poGDS->apoDatasets.size() > 1)
        return static_cast<int>(poGDS->apoDatasets.size()) - 1;
    else
        return 0;
}

/************************************************************************/
/*                              GetOverview()                           */
/************************************************************************/

GDALRasterBand *WMTSBand::GetOverview(int nLevel)
{
    WMTSDataset *poGDS = cpl::down_cast<WMTSDataset *>(poDS);

    if (nLevel < 0 || nLevel >= GetOverviewCount())
        return nullptr;

    GDALDataset *poOvrDS = poGDS->apoDatasets[nLevel + 1];
    if (poOvrDS)
        return poOvrDS->GetRasterBand(nBand);
    else
        return nullptr;
}

/************************************************************************/
/*                   GetColorInterpretation()                           */
/************************************************************************/

GDALColorInterp WMTSBand::GetColorInterpretation()
{
    WMTSDataset *poGDS = cpl::down_cast<WMTSDataset *>(poDS);
    if (poGDS->nBands == 1)
    {
        return GCI_GrayIndex;
    }
    else if (poGDS->nBands == 3 || poGDS->nBands == 4)
    {
        if (nBand == 1)
            return GCI_RedBand;
        else if (nBand == 2)
            return GCI_GreenBand;
        else if (nBand == 3)
            return GCI_BlueBand;
        else if (nBand == 4)
            return GCI_AlphaBand;
    }

    return GCI_Undefined;
}

/************************************************************************/
/*                         GetMetadataItem()                            */
/************************************************************************/

const char *WMTSBand::GetMetadataItem(const char *pszName,
                                      const char *pszDomain)
{
    WMTSDataset *poGDS = cpl::down_cast<WMTSDataset *>(poDS);

    /* ==================================================================== */
    /*      LocationInfo handling.                                          */
    /* ==================================================================== */
    if (pszDomain != nullptr && EQUAL(pszDomain, "LocationInfo") &&
        pszName != nullptr && STARTS_WITH_CI(pszName, "Pixel_") &&
        !poGDS->oTMS.aoTM.empty() && !poGDS->osURLFeatureInfoTemplate.empty())
    {
        int iPixel, iLine;

        /* --------------------------------------------------------------------
         */
        /*      What pixel are we aiming at? */
        /* --------------------------------------------------------------------
         */
        if (sscanf(pszName + 6, "%d_%d", &iPixel, &iLine) != 2)
            return nullptr;

        const WMTSTileMatrix &oTM = poGDS->oTMS.aoTM.back();

        iPixel += static_cast<int>(
            std::round((poGDS->m_gt[0] - oTM.dfTLX) / oTM.dfPixelSize));
        iLine += static_cast<int>(
            std::round((oTM.dfTLY - poGDS->m_gt[3]) / oTM.dfPixelSize));

        CPLString osURL(poGDS->osURLFeatureInfoTemplate);
        osURL = WMTSDataset::Replace(osURL, "{TileMatrixSet}", poGDS->osTMS);
        osURL = WMTSDataset::Replace(osURL, "{TileMatrix}", oTM.osIdentifier);
        osURL = WMTSDataset::Replace(osURL, "{TileCol}",
                                     CPLSPrintf("%d", iPixel / oTM.nTileWidth));
        osURL = WMTSDataset::Replace(osURL, "{TileRow}",
                                     CPLSPrintf("%d", iLine / oTM.nTileHeight));
        osURL = WMTSDataset::Replace(osURL, "{I}",
                                     CPLSPrintf("%d", iPixel % oTM.nTileWidth));
        osURL = WMTSDataset::Replace(osURL, "{J}",
                                     CPLSPrintf("%d", iLine % oTM.nTileHeight));

        if (poGDS->osLastGetFeatureInfoURL.compare(osURL) != 0)
        {
            poGDS->osLastGetFeatureInfoURL = osURL;
            poGDS->osMetadataItemGetFeatureInfo = "";
            char *pszRes = nullptr;
            CPLHTTPResult *psResult =
                CPLHTTPFetch(osURL, poGDS->m_aosHTTPOptions.List());
            if (psResult && psResult->nStatus == 0 && psResult->pabyData)
                pszRes = CPLStrdup(
                    reinterpret_cast<const char *>(psResult->pabyData));
            CPLHTTPDestroyResult(psResult);

            if (pszRes)
            {
                poGDS->osMetadataItemGetFeatureInfo = "<LocationInfo>";
                CPLPushErrorHandler(CPLQuietErrorHandler);
                CPLXMLNode *psXML = CPLParseXMLString(pszRes);
                CPLPopErrorHandler();
                if (psXML != nullptr && psXML->eType == CXT_Element)
                {
                    if (strcmp(psXML->pszValue, "?xml") == 0)
                    {
                        if (psXML->psNext)
                        {
                            char *pszXML = CPLSerializeXMLTree(psXML->psNext);
                            poGDS->osMetadataItemGetFeatureInfo += pszXML;
                            CPLFree(pszXML);
                        }
                    }
                    else
                    {
                        poGDS->osMetadataItemGetFeatureInfo += pszRes;
                    }
                }
                else
                {
                    char *pszEscapedXML =
                        CPLEscapeString(pszRes, -1, CPLES_XML_BUT_QUOTES);
                    poGDS->osMetadataItemGetFeatureInfo += pszEscapedXML;
                    CPLFree(pszEscapedXML);
                }
                if (psXML != nullptr)
                    CPLDestroyXMLNode(psXML);

                poGDS->osMetadataItemGetFeatureInfo += "</LocationInfo>";
                CPLFree(pszRes);
            }
        }
        return poGDS->osMetadataItemGetFeatureInfo.c_str();
    }

    return GDALPamRasterBand::GetMetadataItem(pszName, pszDomain);
}

/************************************************************************/
/*                          WMTSDataset()                               */
/************************************************************************/

WMTSDataset::WMTSDataset()
{
    m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
}

/************************************************************************/
/*                        ~WMTSDataset()                                */
/************************************************************************/

WMTSDataset::~WMTSDataset()
{
    WMTSDataset::CloseDependentDatasets();
}

/************************************************************************/
/*                      CloseDependentDatasets()                        */
/************************************************************************/

int WMTSDataset::CloseDependentDatasets()
{
    int bRet = GDALPamDataset::CloseDependentDatasets();
    if (!apoDatasets.empty())
    {
        for (size_t i = 0; i < apoDatasets.size(); i++)
            delete apoDatasets[i];
        apoDatasets.resize(0);
        bRet = TRUE;
    }
    return bRet;
}

/************************************************************************/
/*                             IRasterIO()                              */
/************************************************************************/

CPLErr WMTSDataset::IRasterIO(GDALRWFlag eRWFlag, int nXOff, int nYOff,
                              int nXSize, int nYSize, void *pData,
                              int nBufXSize, int nBufYSize,
                              GDALDataType eBufType, int nBandCount,
                              BANDMAP_TYPE panBandMap, GSpacing nPixelSpace,
                              GSpacing nLineSpace, GSpacing nBandSpace,
                              GDALRasterIOExtraArg *psExtraArg)
{
    if ((nBufXSize < nXSize || nBufYSize < nYSize) && apoDatasets.size() > 1 &&
        eRWFlag == GF_Read)
    {
        int bTried;
        CPLErr eErr = TryOverviewRasterIO(
            eRWFlag, nXOff, nYOff, nXSize, nYSize, pData, nBufXSize, nBufYSize,
            eBufType, nBandCount, panBandMap, nPixelSpace, nLineSpace,
            nBandSpace, psExtraArg, &bTried);
        if (bTried)
            return eErr;
    }

    return apoDatasets[0]->RasterIO(eRWFlag, nXOff, nYOff, nXSize, nYSize,
                                    pData, nBufXSize, nBufYSize, eBufType,
                                    nBandCount, panBandMap, nPixelSpace,
                                    nLineSpace, nBandSpace, psExtraArg);
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr WMTSDataset::GetGeoTransform(GDALGeoTransform &gt) const
{
    gt = m_gt;
    return CE_None;
}

/************************************************************************/
/*                         GetSpatialRef()                              */
/************************************************************************/

const OGRSpatialReference *WMTSDataset::GetSpatialRef() const
{
    return m_oSRS.IsEmpty() ? nullptr : &m_oSRS;
}

/************************************************************************/
/*                          WMTSEscapeXML()                             */
/************************************************************************/

static CPLString WMTSEscapeXML(const char *pszUnescapedXML)
{
    CPLString osRet;
    char *pszTmp = CPLEscapeString(pszUnescapedXML, -1, CPLES_XML);
    osRet = pszTmp;
    CPLFree(pszTmp);
    return osRet;
}

/************************************************************************/
/*                         GetMetadataItem()                            */
/************************************************************************/

const char *WMTSDataset::GetMetadataItem(const char *pszName,
                                         const char *pszDomain)
{
    if (pszName != nullptr && EQUAL(pszName, "XML") && pszDomain != nullptr &&
        EQUAL(pszDomain, "WMTS"))
    {
        return osXML.c_str();
    }

    return GDALPamDataset::GetMetadataItem(pszName, pszDomain);
}

/************************************************************************/
/*                          QuoteIfNecessary()                          */
/************************************************************************/

static CPLString QuoteIfNecessary(const char *pszVal)
{
    if (strchr(pszVal, ' ') || strchr(pszVal, ',') || strchr(pszVal, '='))
    {
        CPLString osVal;
        osVal += "\"";
        osVal += pszVal;
        osVal += "\"";
        return osVal;
    }
    else
        return pszVal;
}

/************************************************************************/
/*                             FixCRSName()                             */
/************************************************************************/

CPLString WMTSDataset::FixCRSName(const char *pszCRS)
{
    while (*pszCRS == ' ' || *pszCRS == '\r' || *pszCRS == '\n')
        pszCRS++;

    /* http://maps.wien.gv.at/wmts/1.0.0/WMTSCapabilities.xml uses
     * urn:ogc:def:crs:EPSG:6.18:3:3857 */
    /* instead of urn:ogc:def:crs:EPSG:6.18.3:3857. Coming from an incorrect
     * example of URN in WMTS spec */
    /* https://portal.opengeospatial.org/files/?artifact_id=50398 */
    if (STARTS_WITH_CI(pszCRS, "urn:ogc:def:crs:EPSG:6.18:3:"))
    {
        return CPLSPrintf("urn:ogc:def:crs:EPSG::%s",
                          pszCRS + strlen("urn:ogc:def:crs:EPSG:6.18:3:"));
    }

    if (EQUAL(pszCRS, "urn:ogc:def:crs:EPSG::102100"))
        return "EPSG:3857";

    CPLString osRet(pszCRS);
    while (osRet.size() && (osRet.back() == ' ' || osRet.back() == '\r' ||
                            osRet.back() == '\n'))
    {
        osRet.pop_back();
    }
    return osRet;
}

/************************************************************************/
/*                              ReadTMS()                               */
/************************************************************************/

int WMTSDataset::ReadTMS(CPLXMLNode *psContents, const CPLString &osIdentifier,
                         const CPLString &osMaxTileMatrixIdentifier,
                         int nMaxZoomLevel, WMTSTileMatrixSet &oTMS,
                         bool &bHasWarnedAutoSwap)
{
    for (CPLXMLNode *psIter = psContents->psChild; psIter != nullptr;
         psIter = psIter->psNext)
    {
        if (psIter->eType != CXT_Element ||
            strcmp(psIter->pszValue, "TileMatrixSet") != 0)
            continue;
        const char *pszIdentifier = CPLGetXMLValue(psIter, "Identifier", "");
        if (!EQUAL(osIdentifier, pszIdentifier))
            continue;
        const char *pszSupportedCRS =
            CPLGetXMLValue(psIter, "SupportedCRS", nullptr);
        if (pszSupportedCRS == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Missing SupportedCRS");
            return FALSE;
        }
        oTMS.osSRS = pszSupportedCRS;
        if (oTMS.oSRS.SetFromUserInput(
                FixCRSName(pszSupportedCRS),
                OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get()) !=
            OGRERR_NONE)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Cannot parse CRS '%s'",
                     pszSupportedCRS);
            return FALSE;
        }
        const bool bSwap =
            !STARTS_WITH_CI(pszSupportedCRS, "EPSG:") &&
            (CPL_TO_BOOL(oTMS.oSRS.EPSGTreatsAsLatLong()) ||
             CPL_TO_BOOL(oTMS.oSRS.EPSGTreatsAsNorthingEasting()));
        CPLXMLNode *psBB = CPLGetXMLNode(psIter, "BoundingBox");
        oTMS.bBoundingBoxValid = false;
        if (psBB != nullptr)
        {
            CPLString osCRS = CPLGetXMLValue(psBB, "crs", "");
            if (EQUAL(osCRS, "") || EQUAL(osCRS, pszSupportedCRS))
            {
                CPLString osLowerCorner =
                    CPLGetXMLValue(psBB, "LowerCorner", "");
                CPLString osUpperCorner =
                    CPLGetXMLValue(psBB, "UpperCorner", "");
                if (!osLowerCorner.empty() && !osUpperCorner.empty())
                {
                    char **papszLC = CSLTokenizeString(osLowerCorner);
                    char **papszUC = CSLTokenizeString(osUpperCorner);
                    if (CSLCount(papszLC) == 2 && CSLCount(papszUC) == 2)
                    {
                        oTMS.sBoundingBox.MinX =
                            CPLAtof(papszLC[(bSwap) ? 1 : 0]);
                        oTMS.sBoundingBox.MinY =
                            CPLAtof(papszLC[(bSwap) ? 0 : 1]);
                        oTMS.sBoundingBox.MaxX =
                            CPLAtof(papszUC[(bSwap) ? 1 : 0]);
                        oTMS.sBoundingBox.MaxY =
                            CPLAtof(papszUC[(bSwap) ? 0 : 1]);
                        oTMS.bBoundingBoxValid = true;
                    }
                    CSLDestroy(papszLC);
                    CSLDestroy(papszUC);
                }
            }
        }
        else
        {
            const char *pszWellKnownScaleSet =
                CPLGetXMLValue(psIter, "WellKnownScaleSet", "");
            if (EQUAL(pszIdentifier, "GoogleCRS84Quad") ||
                EQUAL(pszWellKnownScaleSet,
                      "urn:ogc:def:wkss:OGC:1.0:GoogleCRS84Quad") ||
                EQUAL(pszIdentifier, "GlobalCRS84Scale") ||
                EQUAL(pszWellKnownScaleSet,
                      "urn:ogc:def:wkss:OGC:1.0:GlobalCRS84Scale"))
            {
                oTMS.sBoundingBox.MinX = -180;
                oTMS.sBoundingBox.MinY = -90;
                oTMS.sBoundingBox.MaxX = 180;
                oTMS.sBoundingBox.MaxY = 90;
                oTMS.bBoundingBoxValid = true;
            }
        }

        bool bFoundTileMatrix = false;
        for (CPLXMLNode *psSubIter = psIter->psChild; psSubIter != nullptr;
             psSubIter = psSubIter->psNext)
        {
            if (psSubIter->eType != CXT_Element ||
                strcmp(psSubIter->pszValue, "TileMatrix") != 0)
                continue;
            const char *l_pszIdentifier =
                CPLGetXMLValue(psSubIter, "Identifier", nullptr);
            const char *pszScaleDenominator =
                CPLGetXMLValue(psSubIter, "ScaleDenominator", nullptr);
            const char *pszTopLeftCorner =
                CPLGetXMLValue(psSubIter, "TopLeftCorner", nullptr);
            const char *pszTileWidth =
                CPLGetXMLValue(psSubIter, "TileWidth", nullptr);
            const char *pszTileHeight =
                CPLGetXMLValue(psSubIter, "TileHeight", nullptr);
            const char *pszMatrixWidth =
                CPLGetXMLValue(psSubIter, "MatrixWidth", nullptr);
            const char *pszMatrixHeight =
                CPLGetXMLValue(psSubIter, "MatrixHeight", nullptr);
            if (l_pszIdentifier == nullptr || pszScaleDenominator == nullptr ||
                pszTopLeftCorner == nullptr ||
                strchr(pszTopLeftCorner, ' ') == nullptr ||
                pszTileWidth == nullptr || pszTileHeight == nullptr ||
                pszMatrixWidth == nullptr || pszMatrixHeight == nullptr)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Missing required element in TileMatrix element");
                return FALSE;
            }
            WMTSTileMatrix oTM;
            oTM.osIdentifier = l_pszIdentifier;
            oTM.dfScaleDenominator = CPLAtof(pszScaleDenominator);
            oTM.dfPixelSize = oTM.dfScaleDenominator * WMTS_PITCH;
            if (oTM.dfPixelSize <= 0.0)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Invalid ScaleDenominator");
                return FALSE;
            }
            if (oTMS.oSRS.IsGeographic())
                oTM.dfPixelSize *= WMTS_WGS84_DEG_PER_METER;
            double dfVal1 = CPLAtof(pszTopLeftCorner);
            double dfVal2 = CPLAtof(strchr(pszTopLeftCorner, ' ') + 1);
            if (!bSwap)
            {
                oTM.dfTLX = dfVal1;
                oTM.dfTLY = dfVal2;
            }
            else
            {
                oTM.dfTLX = dfVal2;
                oTM.dfTLY = dfVal1;
            }

            // Hack for http://osm.geobretagne.fr/gwc01/service/wmts?request=getcapabilities
            // or https://trek.nasa.gov/tiles/Mars/EQ/THEMIS_NightIR_ControlledMosaics_100m_v2_oct2018/1.0.0/WMTSCapabilities.xml
            if (oTM.dfTLY == -180.0 &&
                (STARTS_WITH_CI(l_pszIdentifier, "EPSG:4326:") ||
                 (oTMS.oSRS.IsGeographic() && oTM.dfTLX == 90)))
            {
                if (!bHasWarnedAutoSwap)
                {
                    bHasWarnedAutoSwap = true;
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "Auto-correcting wrongly swapped "
                             "TileMatrix.TopLeftCorner coordinates. "
                             "They should be in latitude, longitude order "
                             "but are presented in longitude, latitude order. "
                             "This should be reported to the server "
                             "administrator.");
                }
                std::swap(oTM.dfTLX, oTM.dfTLY);
            }

            // Hack for "https://t0.tianditu.gov.cn/img_w/wmts?SERVICE=WMTS&REQUEST=GetCapabilities&VERSION=1.0.0&tk=ec899a50c7830ea2416ca182285236f3"
            // which returns swapped coordinates for WebMercator
            if (std::fabs(oTM.dfTLX - 20037508.3427892) < 1e-4 &&
                std::fabs(oTM.dfTLY - (-20037508.3427892)) < 1e-4)
            {
                if (!bHasWarnedAutoSwap)
                {
                    bHasWarnedAutoSwap = true;
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "Auto-correcting wrongly swapped "
                             "TileMatrix.TopLeftCorner coordinates. This "
                             "should be reported to the server administrator.");
                }
                std::swap(oTM.dfTLX, oTM.dfTLY);
            }

            oTM.nTileWidth = atoi(pszTileWidth);
            oTM.nTileHeight = atoi(pszTileHeight);
            if (oTM.nTileWidth <= 0 || oTM.nTileWidth > 4096 ||
                oTM.nTileHeight <= 0 || oTM.nTileHeight > 4096)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Invalid TileWidth/TileHeight element");
                return FALSE;
            }
            oTM.nMatrixWidth = atoi(pszMatrixWidth);
            oTM.nMatrixHeight = atoi(pszMatrixHeight);
            // http://datacarto.geonormandie.fr/mapcache/wmts?SERVICE=WMTS&REQUEST=GetCapabilities
            // has a TileMatrix 0 with MatrixWidth = MatrixHeight = 0
            if (oTM.nMatrixWidth < 1 || oTM.nMatrixHeight < 1)
                continue;
            oTMS.aoTM.push_back(std::move(oTM));
            if ((nMaxZoomLevel >= 0 &&
                 static_cast<int>(oTMS.aoTM.size()) - 1 == nMaxZoomLevel) ||
                (!osMaxTileMatrixIdentifier.empty() &&
                 EQUAL(osMaxTileMatrixIdentifier, l_pszIdentifier)))
            {
                bFoundTileMatrix = true;
                break;
            }
        }
        if (nMaxZoomLevel >= 0 && !bFoundTileMatrix)
        {
            CPLError(
                CE_Failure, CPLE_AppDefined,
                "Cannot find TileMatrix of zoom level %d in TileMatrixSet '%s'",
                nMaxZoomLevel, osIdentifier.c_str());
            return FALSE;
        }
        if (!osMaxTileMatrixIdentifier.empty() && !bFoundTileMatrix)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot find TileMatrix '%s' in TileMatrixSet '%s'",
                     osMaxTileMatrixIdentifier.c_str(), osIdentifier.c_str());
            return FALSE;
        }
        if (oTMS.aoTM.empty())
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot find TileMatrix in TileMatrixSet '%s'",
                     osIdentifier.c_str());
            return FALSE;
        }
        return TRUE;
    }
    CPLError(CE_Failure, CPLE_AppDefined, "Cannot find TileMatrixSet '%s'",
             osIdentifier.c_str());
    return FALSE;
}

/************************************************************************/
/*                              ReadTMLimits()                          */
/************************************************************************/

int WMTSDataset::ReadTMLimits(
    CPLXMLNode *psTMSLimits,
    std::map<CPLString, WMTSTileMatrixLimits> &aoMapTileMatrixLimits)
{
    for (CPLXMLNode *psIter = psTMSLimits->psChild; psIter;
         psIter = psIter->psNext)
    {
        if (psIter->eType != CXT_Element ||
            strcmp(psIter->pszValue, "TileMatrixLimits") != 0)
            continue;
        WMTSTileMatrixLimits oTMLimits;
        const char *pszTileMatrix =
            CPLGetXMLValue(psIter, "TileMatrix", nullptr);
        const char *pszMinTileRow =
            CPLGetXMLValue(psIter, "MinTileRow", nullptr);
        const char *pszMaxTileRow =
            CPLGetXMLValue(psIter, "MaxTileRow", nullptr);
        const char *pszMinTileCol =
            CPLGetXMLValue(psIter, "MinTileCol", nullptr);
        const char *pszMaxTileCol =
            CPLGetXMLValue(psIter, "MaxTileCol", nullptr);
        if (pszTileMatrix == nullptr || pszMinTileRow == nullptr ||
            pszMaxTileRow == nullptr || pszMinTileCol == nullptr ||
            pszMaxTileCol == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Missing required element in TileMatrixLimits element");
            return FALSE;
        }
        oTMLimits.osIdentifier = pszTileMatrix;
        oTMLimits.nMinTileRow = atoi(pszMinTileRow);
        oTMLimits.nMaxTileRow = atoi(pszMaxTileRow);
        oTMLimits.nMinTileCol = atoi(pszMinTileCol);
        oTMLimits.nMaxTileCol = atoi(pszMaxTileCol);
        aoMapTileMatrixLimits[pszTileMatrix] = std::move(oTMLimits);
    }
    return TRUE;
}

/************************************************************************/
/*                               Replace()                              */
/************************************************************************/

CPLString WMTSDataset::Replace(const CPLString &osStr, const char *pszOld,
                               const char *pszNew)
{
    size_t nPos = osStr.ifind(pszOld);
    if (nPos == std::string::npos)
        return osStr;
    CPLString osRet(osStr.substr(0, nPos));
    osRet += pszNew;
    osRet += osStr.substr(nPos + strlen(pszOld));
    return osRet;
}

/************************************************************************/
/*                       GetCapabilitiesResponse()                      */
/************************************************************************/

CPLXMLNode *WMTSDataset::GetCapabilitiesResponse(const CPLString &osFilename,
                                                 CSLConstList papszHTTPOptions)
{
    CPLXMLNode *psXML;
    VSIStatBufL sStat;
    if (VSIStatL(osFilename, &sStat) == 0)
        psXML = CPLParseXMLFile(osFilename);
    else
    {
        CPLHTTPResult *psResult = CPLHTTPFetch(osFilename, papszHTTPOptions);
        if (psResult == nullptr)
            return nullptr;
        if (psResult->pabyData == nullptr)
        {
            CPLHTTPDestroyResult(psResult);
            return nullptr;
        }
        psXML = CPLParseXMLString(
            reinterpret_cast<const char *>(psResult->pabyData));
        CPLHTTPDestroyResult(psResult);
    }
    return psXML;
}

/************************************************************************/
/*                          WMTSAddOtherXML()                           */
/************************************************************************/

static void WMTSAddOtherXML(CPLXMLNode *psRoot, const char *pszElement,
                            CPLString &osOtherXML)
{
    CPLXMLNode *psElement = CPLGetXMLNode(psRoot, pszElement);
    if (psElement)
    {
        CPLXMLNode *psNext = psElement->psNext;
        psElement->psNext = nullptr;
        char *pszTmp = CPLSerializeXMLTree(psElement);
        osOtherXML += pszTmp;
        CPLFree(pszTmp);
        psElement->psNext = psNext;
    }
}

/************************************************************************/
/*                          GetOperationKVPURL()                        */
/************************************************************************/

CPLString WMTSDataset::GetOperationKVPURL(CPLXMLNode *psXML,
                                          const char *pszOperation)
{
    CPLString osRet;
    CPLXMLNode *psOM = CPLGetXMLNode(psXML, "=Capabilities.OperationsMetadata");
    for (CPLXMLNode *psIter = psOM ? psOM->psChild : nullptr; psIter != nullptr;
         psIter = psIter->psNext)
    {
        if (psIter->eType != CXT_Element ||
            strcmp(psIter->pszValue, "Operation") != 0 ||
            !EQUAL(CPLGetXMLValue(psIter, "name", ""), pszOperation))
        {
            continue;
        }
        CPLXMLNode *psHTTP = CPLGetXMLNode(psIter, "DCP.HTTP");
        for (CPLXMLNode *psGet = psHTTP ? psHTTP->psChild : nullptr;
             psGet != nullptr; psGet = psGet->psNext)
        {
            if (psGet->eType != CXT_Element ||
                strcmp(psGet->pszValue, "Get") != 0)
            {
                continue;
            }
            if (!EQUAL(CPLGetXMLValue(psGet, "Constraint.AllowedValues.Value",
                                      "KVP"),
                       "KVP"))
                continue;
            osRet = CPLGetXMLValue(psGet, "href", "");
        }
    }
    return osRet;
}

/************************************************************************/
/*                           BuildHTTPRequestOpts()                     */
/************************************************************************/

CPLStringList WMTSDataset::BuildHTTPRequestOpts(CPLString osOtherXML)
{
    osOtherXML = "<Root>" + osOtherXML + "</Root>";
    CPLXMLNode *psXML = CPLParseXMLString(osOtherXML);
    CPLStringList opts;
    for (const char *pszOptionName :
         {"Timeout", "UserAgent", "Accept", "Referer", "UserPwd"})
    {
        if (const char *pszVal = CPLGetXMLValue(psXML, pszOptionName, nullptr))
        {
            opts.SetNameValue(CPLString(pszOptionName).toupper(), pszVal);
        }
    }
    if (CPLTestBool(CPLGetXMLValue(psXML, "UnsafeSSL", "false")))
    {
        opts.SetNameValue("UNSAFESSL", "1");
    }
    CPLDestroyXMLNode(psXML);
    return opts;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *WMTSDataset::Open(GDALOpenInfo *poOpenInfo)
{
    if (!WMTSDriverIdentify(poOpenInfo))
        return nullptr;

    CPLXMLNode *psXML = nullptr;
    CPLString osTileFormat;
    CPLString osInfoFormat;

    CPLString osGetCapabilitiesURL =
        CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "URL", "");
    CPLString osLayer =
        CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "LAYER", "");
    CPLString osTMS =
        CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "TILEMATRIXSET", "");
    CPLString osMaxTileMatrixIdentifier =
        CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "TILEMATRIX", "");
    int nUserMaxZoomLevel = atoi(CSLFetchNameValueDef(
        poOpenInfo->papszOpenOptions, "ZOOM_LEVEL",
        CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "ZOOMLEVEL", "-1")));
    CPLString osStyle =
        CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "STYLE", "");

    int bExtendBeyondDateLine = CPLFetchBool(poOpenInfo->papszOpenOptions,
                                             "EXTENDBEYONDDATELINE", false);

    CPLString osOtherXML =
        "<Cache />"
        "<UnsafeSSL>true</UnsafeSSL>"
        "<ZeroBlockHttpCodes>204,404</ZeroBlockHttpCodes>"
        "<ZeroBlockOnServerException>true</ZeroBlockOnServerException>";

    if (STARTS_WITH_CI(poOpenInfo->pszFilename, "WMTS:"))
    {
        char **papszTokens = CSLTokenizeString2(poOpenInfo->pszFilename + 5,
                                                ",", CSLT_HONOURSTRINGS);
        if (papszTokens && papszTokens[0])
        {
            osGetCapabilitiesURL = papszTokens[0];
            for (char **papszIter = papszTokens + 1; *papszIter; papszIter++)
            {
                char *pszKey = nullptr;
                const char *pszValue = CPLParseNameValue(*papszIter, &pszKey);
                if (pszKey && pszValue)
                {
                    if (EQUAL(pszKey, "layer"))
                        osLayer = pszValue;
                    else if (EQUAL(pszKey, "tilematrixset"))
                        osTMS = pszValue;
                    else if (EQUAL(pszKey, "tilematrix"))
                        osMaxTileMatrixIdentifier = pszValue;
                    else if (EQUAL(pszKey, "zoom_level") ||
                             EQUAL(pszKey, "zoomlevel"))
                        nUserMaxZoomLevel = atoi(pszValue);
                    else if (EQUAL(pszKey, "style"))
                        osStyle = pszValue;
                    else if (EQUAL(pszKey, "extendbeyonddateline"))
                        bExtendBeyondDateLine = CPLTestBool(pszValue);
                    else
                        CPLError(CE_Warning, CPLE_AppDefined,
                                 "Unknown parameter: %s'", pszKey);
                }
                CPLFree(pszKey);
            }
        }
        CSLDestroy(papszTokens);

        const CPLStringList aosHTTPOptions(BuildHTTPRequestOpts(osOtherXML));
        psXML = GetCapabilitiesResponse(osGetCapabilitiesURL,
                                        aosHTTPOptions.List());
    }
    else if (poOpenInfo->IsSingleAllowedDriver("WMTS") &&
             (STARTS_WITH(poOpenInfo->pszFilename, "http://") ||
              STARTS_WITH(poOpenInfo->pszFilename, "https://")))
    {
        const CPLStringList aosHTTPOptions(BuildHTTPRequestOpts(osOtherXML));
        psXML = GetCapabilitiesResponse(poOpenInfo->pszFilename,
                                        aosHTTPOptions.List());
    }

    int bHasAOI = FALSE;
    OGREnvelope sAOI;
    int nBands = 4;
    GDALDataType eDataType = GDT_Byte;
    CPLString osProjection;
    CPLString osExtraQueryParameters;

    if ((psXML != nullptr && CPLGetXMLNode(psXML, "=GDAL_WMTS") != nullptr) ||
        STARTS_WITH_CI(poOpenInfo->pszFilename, "<GDAL_WMTS") ||
        (poOpenInfo->nHeaderBytes > 0 &&
         strstr(reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                "<GDAL_WMTS")))
    {
        CPLXMLNode *psGDALWMTS;
        if (psXML != nullptr && CPLGetXMLNode(psXML, "=GDAL_WMTS") != nullptr)
            psGDALWMTS = CPLCloneXMLTree(psXML);
        else if (STARTS_WITH_CI(poOpenInfo->pszFilename, "<GDAL_WMTS"))
            psGDALWMTS = CPLParseXMLString(poOpenInfo->pszFilename);
        else
            psGDALWMTS = CPLParseXMLFile(poOpenInfo->pszFilename);
        if (psGDALWMTS == nullptr)
            return nullptr;
        CPLXMLNode *psRoot = CPLGetXMLNode(psGDALWMTS, "=GDAL_WMTS");
        if (psRoot == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot find root <GDAL_WMTS>");
            CPLDestroyXMLNode(psGDALWMTS);
            return nullptr;
        }
        osGetCapabilitiesURL = CPLGetXMLValue(psRoot, "GetCapabilitiesUrl", "");
        if (osGetCapabilitiesURL.empty())
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Missing <GetCapabilitiesUrl>");
            CPLDestroyXMLNode(psGDALWMTS);
            return nullptr;
        }
        osExtraQueryParameters =
            CPLGetXMLValue(psRoot, "ExtraQueryParameters", "");
        if (!osExtraQueryParameters.empty() && osExtraQueryParameters[0] != '&')
            osExtraQueryParameters = '&' + osExtraQueryParameters;

        osGetCapabilitiesURL += osExtraQueryParameters;

        osLayer = CPLGetXMLValue(psRoot, "Layer", osLayer);
        osTMS = CPLGetXMLValue(psRoot, "TileMatrixSet", osTMS);
        osMaxTileMatrixIdentifier =
            CPLGetXMLValue(psRoot, "TileMatrix", osMaxTileMatrixIdentifier);
        nUserMaxZoomLevel = atoi(CPLGetXMLValue(
            psRoot, "ZoomLevel", CPLSPrintf("%d", nUserMaxZoomLevel)));
        osStyle = CPLGetXMLValue(psRoot, "Style", osStyle);
        osTileFormat = CPLGetXMLValue(psRoot, "Format", osTileFormat);
        osInfoFormat = CPLGetXMLValue(psRoot, "InfoFormat", osInfoFormat);
        osProjection = CPLGetXMLValue(psRoot, "Projection", osProjection);
        bExtendBeyondDateLine = CPLTestBool(
            CPLGetXMLValue(psRoot, "ExtendBeyondDateLine",
                           (bExtendBeyondDateLine) ? "true" : "false"));

        osOtherXML = "";
        for (const char *pszXMLElement :
             {"Cache", "MaxConnections", "Timeout", "OfflineMode", "UserAgent",
              "Accept", "UserPwd", "UnsafeSSL", "Referer", "ZeroBlockHttpCodes",
              "ZeroBlockOnServerException"})
        {
            WMTSAddOtherXML(psRoot, pszXMLElement, osOtherXML);
        }

        nBands = atoi(CPLGetXMLValue(psRoot, "BandsCount", "4"));
        const char *pszDataType = CPLGetXMLValue(psRoot, "DataType", "Byte");
        eDataType = GDALGetDataTypeByName(pszDataType);
        if ((eDataType == GDT_Unknown) || (eDataType >= GDT_TypeCount))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "GDALWMTS: Invalid value in DataType. Data type \"%s\" is "
                     "not supported.",
                     pszDataType);
            CPLDestroyXMLNode(psGDALWMTS);
            return nullptr;
        }

        const char *pszULX =
            CPLGetXMLValue(psRoot, "DataWindow.UpperLeftX", nullptr);
        const char *pszULY =
            CPLGetXMLValue(psRoot, "DataWindow.UpperLeftY", nullptr);
        const char *pszLRX =
            CPLGetXMLValue(psRoot, "DataWindow.LowerRightX", nullptr);
        const char *pszLRY =
            CPLGetXMLValue(psRoot, "DataWindow.LowerRightY", nullptr);
        if (pszULX && pszULY && pszLRX && pszLRY)
        {
            sAOI.MinX = CPLAtof(pszULX);
            sAOI.MaxY = CPLAtof(pszULY);
            sAOI.MaxX = CPLAtof(pszLRX);
            sAOI.MinY = CPLAtof(pszLRY);
            bHasAOI = TRUE;
        }

        CPLDestroyXMLNode(psGDALWMTS);

        CPLDestroyXMLNode(psXML);
        const CPLStringList aosHTTPOptions(BuildHTTPRequestOpts(osOtherXML));
        psXML = GetCapabilitiesResponse(osGetCapabilitiesURL,
                                        aosHTTPOptions.List());
    }
    else if (!STARTS_WITH_CI(poOpenInfo->pszFilename, "WMTS:") &&
             !STARTS_WITH(poOpenInfo->pszFilename, "http://") &&
             !STARTS_WITH(poOpenInfo->pszFilename, "https://"))
    {
        osGetCapabilitiesURL = poOpenInfo->pszFilename;
        psXML = CPLParseXMLFile(poOpenInfo->pszFilename);
    }
    if (psXML == nullptr)
        return nullptr;
    CPLStripXMLNamespace(psXML, nullptr, TRUE);

    CPLXMLNode *psContents = CPLGetXMLNode(psXML, "=Capabilities.Contents");
    if (psContents == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Missing Capabilities.Contents element");
        CPLDestroyXMLNode(psXML);
        return nullptr;
    }

    if (STARTS_WITH(osGetCapabilitiesURL, "/vsimem/"))
    {
        osGetCapabilitiesURL = GetOperationKVPURL(psXML, "GetCapabilities");
        if (osGetCapabilitiesURL.empty())
        {
            // (ERO) I'm not even sure this is correct at all...
            const char *pszHref = CPLGetXMLValue(
                psXML, "=Capabilities.ServiceMetadataURL.href", nullptr);
            if (pszHref)
                osGetCapabilitiesURL = pszHref;
        }
        else
        {
            osGetCapabilitiesURL =
                CPLURLAddKVP(osGetCapabilitiesURL, "service", "WMTS");
            osGetCapabilitiesURL = CPLURLAddKVP(osGetCapabilitiesURL, "request",
                                                "GetCapabilities");
        }
    }
    CPLString osCapabilitiesFilename(osGetCapabilitiesURL);
    if (!STARTS_WITH_CI(osCapabilitiesFilename, "WMTS:"))
        osCapabilitiesFilename = "WMTS:" + osGetCapabilitiesURL;

    int nLayerCount = 0;
    CPLStringList aosSubDatasets;
    CPLString osSelectLayer(osLayer), osSelectTMS(osTMS),
        osSelectStyle(osStyle);
    CPLString osSelectLayerTitle, osSelectLayerAbstract;
    CPLString osSelectTileFormat(osTileFormat),
        osSelectInfoFormat(osInfoFormat);
    int nCountTileFormat = 0;
    int nCountInfoFormat = 0;
    CPLString osURLTileTemplate;
    CPLString osURLFeatureInfoTemplate;
    std::set<CPLString> aoSetLayers;
    std::map<CPLString, OGREnvelope> aoMapBoundingBox;
    std::map<CPLString, WMTSTileMatrixLimits> aoMapTileMatrixLimits;
    std::map<CPLString, CPLString> aoMapDimensions;
    bool bHasWarnedAutoSwap = false;
    bool bHasWarnedAutoSwapBoundingBox = false;

    // Collect TileMatrixSet identifiers
    std::set<std::string> oSetTMSIdentifiers;
    for (CPLXMLNode *psIter = psContents->psChild; psIter != nullptr;
         psIter = psIter->psNext)
    {
        if (psIter->eType != CXT_Element ||
            strcmp(psIter->pszValue, "TileMatrixSet") != 0)
            continue;
        const char *pszIdentifier =
            CPLGetXMLValue(psIter, "Identifier", nullptr);
        if (pszIdentifier)
            oSetTMSIdentifiers.insert(pszIdentifier);
    }

    for (CPLXMLNode *psIter = psContents->psChild; psIter != nullptr;
         psIter = psIter->psNext)
    {
        if (psIter->eType != CXT_Element ||
            strcmp(psIter->pszValue, "Layer") != 0)
            continue;
        const char *pszIdentifier = CPLGetXMLValue(psIter, "Identifier", "");
        if (aoSetLayers.find(pszIdentifier) != aoSetLayers.end())
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Several layers with identifier '%s'. Only first one kept",
                     pszIdentifier);
        }
        aoSetLayers.insert(pszIdentifier);
        if (!osLayer.empty() && strcmp(osLayer, pszIdentifier) != 0)
            continue;
        const char *pszTitle = CPLGetXMLValue(psIter, "Title", nullptr);
        if (osSelectLayer.empty())
        {
            osSelectLayer = pszIdentifier;
        }
        if (strcmp(osSelectLayer, pszIdentifier) == 0)
        {
            if (pszTitle != nullptr)
                osSelectLayerTitle = pszTitle;
            const char *pszAbstract =
                CPLGetXMLValue(psIter, "Abstract", nullptr);
            if (pszAbstract != nullptr)
                osSelectLayerAbstract = pszAbstract;
        }

        std::vector<CPLString> aosTMS;
        std::vector<CPLString> aosStylesIdentifier;
        std::vector<CPLString> aosStylesTitle;

        CPLXMLNode *psSubIter = psIter->psChild;
        for (; psSubIter != nullptr; psSubIter = psSubIter->psNext)
        {
            if (psSubIter->eType != CXT_Element)
                continue;
            if (strcmp(osSelectLayer, pszIdentifier) == 0 &&
                strcmp(psSubIter->pszValue, "Format") == 0)
            {
                const char *pszValue = CPLGetXMLValue(psSubIter, "", "");
                if (!osTileFormat.empty() &&
                    strcmp(osTileFormat, pszValue) != 0)
                    continue;
                nCountTileFormat++;
                if (osSelectTileFormat.empty() || EQUAL(pszValue, "image/png"))
                {
                    osSelectTileFormat = pszValue;
                }
            }
            else if (strcmp(osSelectLayer, pszIdentifier) == 0 &&
                     strcmp(psSubIter->pszValue, "InfoFormat") == 0)
            {
                const char *pszValue = CPLGetXMLValue(psSubIter, "", "");
                if (!osInfoFormat.empty() &&
                    strcmp(osInfoFormat, pszValue) != 0)
                    continue;
                nCountInfoFormat++;
                if (osSelectInfoFormat.empty() ||
                    (EQUAL(pszValue, "application/vnd.ogc.gml") &&
                     !EQUAL(osSelectInfoFormat,
                            "application/vnd.ogc.gml/3.1.1")) ||
                    EQUAL(pszValue, "application/vnd.ogc.gml/3.1.1"))
                {
                    osSelectInfoFormat = pszValue;
                }
            }
            else if (strcmp(osSelectLayer, pszIdentifier) == 0 &&
                     strcmp(psSubIter->pszValue, "Dimension") == 0)
            {
                /* Cf http://wmts.geo.admin.ch/1.0.0/WMTSCapabilities.xml */
                const char *pszDimensionIdentifier =
                    CPLGetXMLValue(psSubIter, "Identifier", nullptr);
                const char *pszDefault =
                    CPLGetXMLValue(psSubIter, "Default", "");
                if (pszDimensionIdentifier != nullptr)
                    aoMapDimensions[pszDimensionIdentifier] = pszDefault;
            }
            else if (strcmp(psSubIter->pszValue, "TileMatrixSetLink") == 0)
            {
                const char *pszTMS =
                    CPLGetXMLValue(psSubIter, "TileMatrixSet", "");
                if (oSetTMSIdentifiers.find(pszTMS) == oSetTMSIdentifiers.end())
                {
                    CPLDebug("WMTS",
                             "Layer %s has a TileMatrixSetLink to %s, "
                             "but it is not defined as a TileMatrixSet",
                             pszIdentifier, pszTMS);
                    continue;
                }
                if (!osTMS.empty() && strcmp(osTMS, pszTMS) != 0)
                    continue;
                if (strcmp(osSelectLayer, pszIdentifier) == 0 &&
                    osSelectTMS.empty())
                {
                    osSelectTMS = pszTMS;
                }
                if (strcmp(osSelectLayer, pszIdentifier) == 0 &&
                    strcmp(osSelectTMS, pszTMS) == 0)
                {
                    CPLXMLNode *psTMSLimits =
                        CPLGetXMLNode(psSubIter, "TileMatrixSetLimits");
                    if (psTMSLimits)
                        ReadTMLimits(psTMSLimits, aoMapTileMatrixLimits);
                }
                aosTMS.push_back(pszTMS);
            }
            else if (strcmp(psSubIter->pszValue, "Style") == 0)
            {
                int bIsDefault = CPLTestBool(
                    CPLGetXMLValue(psSubIter, "isDefault", "false"));
                const char *l_pszIdentifier =
                    CPLGetXMLValue(psSubIter, "Identifier", "");
                if (!osStyle.empty() && strcmp(osStyle, l_pszIdentifier) != 0)
                    continue;
                const char *pszStyleTitle =
                    CPLGetXMLValue(psSubIter, "Title", l_pszIdentifier);
                if (bIsDefault)
                {
                    aosStylesIdentifier.insert(aosStylesIdentifier.begin(),
                                               CPLString(l_pszIdentifier));
                    aosStylesTitle.insert(aosStylesTitle.begin(),
                                          CPLString(pszStyleTitle));
                    if (strcmp(osSelectLayer, l_pszIdentifier) == 0 &&
                        osSelectStyle.empty())
                    {
                        osSelectStyle = l_pszIdentifier;
                    }
                }
                else
                {
                    aosStylesIdentifier.push_back(l_pszIdentifier);
                    aosStylesTitle.push_back(pszStyleTitle);
                }
            }
            else if (strcmp(osSelectLayer, pszIdentifier) == 0 &&
                     (strcmp(psSubIter->pszValue, "BoundingBox") == 0 ||
                      strcmp(psSubIter->pszValue, "WGS84BoundingBox") == 0))
            {
                CPLString osCRS = CPLGetXMLValue(psSubIter, "crs", "");
                if (osCRS.empty())
                {
                    if (strcmp(psSubIter->pszValue, "WGS84BoundingBox") == 0)
                    {
                        osCRS = "EPSG:4326";
                    }
                    else
                    {
                        int nCountTileMatrixSet = 0;
                        CPLString osSingleTileMatrixSet;
                        for (CPLXMLNode *psIter3 = psContents->psChild;
                             psIter3 != nullptr; psIter3 = psIter3->psNext)
                        {
                            if (psIter3->eType != CXT_Element ||
                                strcmp(psIter3->pszValue, "TileMatrixSet") != 0)
                                continue;
                            nCountTileMatrixSet++;
                            if (nCountTileMatrixSet == 1)
                                osSingleTileMatrixSet =
                                    CPLGetXMLValue(psIter3, "Identifier", "");
                        }
                        if (nCountTileMatrixSet == 1)
                        {
                            // For
                            // 13-082_WMTS_Simple_Profile/schemas/wmts/1.0/profiles/WMTSSimple/examples/wmtsGetCapabilities_response_OSM.xml
                            WMTSTileMatrixSet oTMS;
                            if (ReadTMS(psContents, osSingleTileMatrixSet,
                                        CPLString(), -1, oTMS,
                                        bHasWarnedAutoSwap))
                            {
                                osCRS = oTMS.osSRS;
                            }
                        }
                    }
                }
                CPLString osLowerCorner =
                    CPLGetXMLValue(psSubIter, "LowerCorner", "");
                CPLString osUpperCorner =
                    CPLGetXMLValue(psSubIter, "UpperCorner", "");
                OGRSpatialReference oSRS;
                oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                if (!osCRS.empty() && !osLowerCorner.empty() &&
                    !osUpperCorner.empty() &&
                    oSRS.SetFromUserInput(FixCRSName(osCRS)) == OGRERR_NONE)
                {
                    const bool bSwap =
                        !STARTS_WITH_CI(osCRS, "EPSG:") &&
                        (CPL_TO_BOOL(oSRS.EPSGTreatsAsLatLong()) ||
                         CPL_TO_BOOL(oSRS.EPSGTreatsAsNorthingEasting()));
                    char **papszLC = CSLTokenizeString(osLowerCorner);
                    char **papszUC = CSLTokenizeString(osUpperCorner);
                    if (CSLCount(papszLC) == 2 && CSLCount(papszUC) == 2)
                    {
                        OGREnvelope sEnvelope;
                        sEnvelope.MinX = CPLAtof(papszLC[(bSwap) ? 1 : 0]);
                        sEnvelope.MinY = CPLAtof(papszLC[(bSwap) ? 0 : 1]);
                        sEnvelope.MaxX = CPLAtof(papszUC[(bSwap) ? 1 : 0]);
                        sEnvelope.MaxY = CPLAtof(papszUC[(bSwap) ? 0 : 1]);

                        if (bSwap && oSRS.IsGeographic() &&
                            (std::fabs(sEnvelope.MinY) > 90 ||
                             std::fabs(sEnvelope.MaxY) > 90))
                        {
                            if (!bHasWarnedAutoSwapBoundingBox)
                            {
                                bHasWarnedAutoSwapBoundingBox = true;
                                CPLError(
                                    CE_Warning, CPLE_AppDefined,
                                    "Auto-correcting wrongly swapped "
                                    "ows:%s coordinates. "
                                    "They should be in latitude, longitude "
                                    "order "
                                    "but are presented in longitude, latitude "
                                    "order. "
                                    "This should be reported to the server "
                                    "administrator.",
                                    psSubIter->pszValue);
                            }
                            std::swap(sEnvelope.MinX, sEnvelope.MinY);
                            std::swap(sEnvelope.MaxX, sEnvelope.MaxY);
                        }

                        aoMapBoundingBox[osCRS] = sEnvelope;
                    }
                    CSLDestroy(papszLC);
                    CSLDestroy(papszUC);
                }
            }
            else if (strcmp(osSelectLayer, pszIdentifier) == 0 &&
                     strcmp(psSubIter->pszValue, "ResourceURL") == 0)
            {
                if (EQUAL(CPLGetXMLValue(psSubIter, "resourceType", ""),
                          "tile"))
                {
                    const char *pszFormat =
                        CPLGetXMLValue(psSubIter, "format", "");
                    if (!osTileFormat.empty() &&
                        strcmp(osTileFormat, pszFormat) != 0)
                        continue;
                    if (osURLTileTemplate.empty())
                        osURLTileTemplate =
                            CPLGetXMLValue(psSubIter, "template", "");
                }
                else if (EQUAL(CPLGetXMLValue(psSubIter, "resourceType", ""),
                               "FeatureInfo"))
                {
                    const char *pszFormat =
                        CPLGetXMLValue(psSubIter, "format", "");
                    if (!osInfoFormat.empty() &&
                        strcmp(osInfoFormat, pszFormat) != 0)
                        continue;
                    if (osURLFeatureInfoTemplate.empty())
                        osURLFeatureInfoTemplate =
                            CPLGetXMLValue(psSubIter, "template", "");
                }
            }
        }
        if (strcmp(osSelectLayer, pszIdentifier) == 0 &&
            osSelectStyle.empty() && !aosStylesIdentifier.empty())
        {
            osSelectStyle = aosStylesIdentifier[0];
        }
        for (size_t i = 0; i < aosTMS.size(); i++)
        {
            for (size_t j = 0; j < aosStylesIdentifier.size(); j++)
            {
                int nIdx = 1 + aosSubDatasets.size() / 2;
                CPLString osName(osCapabilitiesFilename);
                osName += ",layer=";
                osName += QuoteIfNecessary(pszIdentifier);
                if (aosTMS.size() > 1)
                {
                    osName += ",tilematrixset=";
                    osName += QuoteIfNecessary(aosTMS[i]);
                }
                if (aosStylesIdentifier.size() > 1)
                {
                    osName += ",style=";
                    osName += QuoteIfNecessary(aosStylesIdentifier[j]);
                }
                aosSubDatasets.AddNameValue(
                    CPLSPrintf("SUBDATASET_%d_NAME", nIdx), osName);

                CPLString osDesc("Layer ");
                osDesc += pszTitle ? pszTitle : pszIdentifier;
                if (aosTMS.size() > 1)
                {
                    osDesc += ", tile matrix set ";
                    osDesc += aosTMS[i];
                }
                if (aosStylesIdentifier.size() > 1)
                {
                    osDesc += ", style ";
                    osDesc += QuoteIfNecessary(aosStylesTitle[j]);
                }
                aosSubDatasets.AddNameValue(
                    CPLSPrintf("SUBDATASET_%d_DESC", nIdx), osDesc);
            }
        }
        if (!aosTMS.empty() && !aosStylesIdentifier.empty())
            nLayerCount++;
        else
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Missing TileMatrixSetLink and/or Style");
    }

    if (nLayerCount == 0)
    {
        CPLDestroyXMLNode(psXML);
        return nullptr;
    }

    WMTSDataset *poDS = new WMTSDataset();

    if (aosSubDatasets.size() > 2)
        poDS->SetMetadata(aosSubDatasets.List(), "SUBDATASETS");

    if (nLayerCount == 1)
    {
        if (!osSelectLayerTitle.empty())
            poDS->SetMetadataItem("TITLE", osSelectLayerTitle);
        if (!osSelectLayerAbstract.empty())
            poDS->SetMetadataItem("ABSTRACT", osSelectLayerAbstract);

        poDS->m_aosHTTPOptions = BuildHTTPRequestOpts(osOtherXML);
        poDS->osLayer = osSelectLayer;
        poDS->osTMS = osSelectTMS;

        WMTSTileMatrixSet oTMS;
        if (!ReadTMS(psContents, osSelectTMS, osMaxTileMatrixIdentifier,
                     nUserMaxZoomLevel, oTMS, bHasWarnedAutoSwap))
        {
            CPLDestroyXMLNode(psXML);
            delete poDS;
            return nullptr;
        }

        const char *pszExtentMethod = CSLFetchNameValueDef(
            poOpenInfo->papszOpenOptions, "EXTENT_METHOD", "AUTO");
        ExtentMethod eExtentMethod = AUTO;
        if (EQUAL(pszExtentMethod, "LAYER_BBOX"))
            eExtentMethod = LAYER_BBOX;
        else if (EQUAL(pszExtentMethod, "TILE_MATRIX_SET"))
            eExtentMethod = TILE_MATRIX_SET;
        else if (EQUAL(pszExtentMethod, "MOST_PRECISE_TILE_MATRIX"))
            eExtentMethod = MOST_PRECISE_TILE_MATRIX;

        bool bAOIFromLayer = false;

        // Use in priority layer bounding box expressed in the SRS of the TMS
        if ((!bHasAOI || bExtendBeyondDateLine) &&
            (eExtentMethod == AUTO || eExtentMethod == LAYER_BBOX) &&
            aoMapBoundingBox.find(oTMS.osSRS) != aoMapBoundingBox.end())
        {
            if (!bHasAOI)
            {
                sAOI = aoMapBoundingBox[oTMS.osSRS];
                bAOIFromLayer = true;
                bHasAOI = TRUE;
            }

            int bRecomputeAOI = FALSE;
            if (bExtendBeyondDateLine)
            {
                bExtendBeyondDateLine = FALSE;

                OGRSpatialReference oWGS84;
                oWGS84.SetFromUserInput(SRS_WKT_WGS84_LAT_LONG);
                oWGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                OGRCoordinateTransformation *poCT =
                    OGRCreateCoordinateTransformation(&oTMS.oSRS, &oWGS84);
                if (poCT != nullptr)
                {
                    double dfX1 = sAOI.MinX;
                    double dfY1 = sAOI.MinY;
                    double dfX2 = sAOI.MaxX;
                    double dfY2 = sAOI.MaxY;
                    if (poCT->Transform(1, &dfX1, &dfY1) &&
                        poCT->Transform(1, &dfX2, &dfY2))
                    {
                        if (fabs(dfX1 + 180) < 1e-8 && fabs(dfX2 - 180) < 1e-8)
                        {
                            bExtendBeyondDateLine = TRUE;
                            bRecomputeAOI = TRUE;
                        }
                        else if (dfX2 < dfX1)
                        {
                            bExtendBeyondDateLine = TRUE;
                        }
                        else
                        {
                            CPLError(
                                CE_Warning, CPLE_AppDefined,
                                "ExtendBeyondDateLine disabled, since "
                                "longitudes of %s "
                                "BoundingBox do not span from -180 to 180 but "
                                "from %.16g to %.16g, "
                                "or longitude of upper right corner is not "
                                "lesser than the one of lower left corner",
                                oTMS.osSRS.c_str(), dfX1, dfX2);
                        }
                    }
                    delete poCT;
                }
            }
            if (bExtendBeyondDateLine && bRecomputeAOI)
            {
                bExtendBeyondDateLine = FALSE;

                std::map<CPLString, OGREnvelope>::iterator oIter =
                    aoMapBoundingBox.begin();
                for (; oIter != aoMapBoundingBox.end(); ++oIter)
                {
                    OGRSpatialReference oSRS;
                    if (oSRS.SetFromUserInput(
                            FixCRSName(oIter->first),
                            OGRSpatialReference::
                                SET_FROM_USER_INPUT_LIMITATIONS_get()) ==
                        OGRERR_NONE)
                    {
                        OGRSpatialReference oWGS84;
                        oWGS84.SetFromUserInput(SRS_WKT_WGS84_LAT_LONG);
                        oWGS84.SetAxisMappingStrategy(
                            OAMS_TRADITIONAL_GIS_ORDER);
                        auto poCT =
                            std::unique_ptr<OGRCoordinateTransformation>(
                                OGRCreateCoordinateTransformation(&oSRS,
                                                                  &oWGS84));
                        double dfX1 = oIter->second.MinX;
                        double dfY1 = oIter->second.MinY;
                        double dfX2 = oIter->second.MaxX;
                        double dfY2 = oIter->second.MaxY;
                        if (poCT != nullptr &&
                            poCT->Transform(1, &dfX1, &dfY1) &&
                            poCT->Transform(1, &dfX2, &dfY2) && dfX2 < dfX1)
                        {
                            dfX2 += 360;
                            OGRSpatialReference oWGS84_with_over;
                            oWGS84_with_over.SetFromUserInput(
                                "+proj=longlat +datum=WGS84 +over +wktext");
                            char *pszProj4 = nullptr;
                            oTMS.oSRS.exportToProj4(&pszProj4);
                            oSRS.SetFromUserInput(
                                CPLSPrintf("%s +over +wktext", pszProj4));
                            CPLFree(pszProj4);
                            poCT.reset(OGRCreateCoordinateTransformation(
                                &oWGS84_with_over, &oSRS));
                            if (poCT && poCT->Transform(1, &dfX1, &dfY1) &&
                                poCT->Transform(1, &dfX2, &dfY2))
                            {
                                bExtendBeyondDateLine = TRUE;
                                sAOI.MinX = std::min(dfX1, dfX2);
                                sAOI.MinY = std::min(dfY1, dfY2);
                                sAOI.MaxX = std::max(dfX1, dfX2);
                                sAOI.MaxY = std::max(dfY1, dfY2);
                                CPLDebug("WMTS",
                                         "ExtendBeyondDateLine using %s "
                                         "bounding box",
                                         oIter->first.c_str());
                            }
                            break;
                        }
                    }
                }
            }
        }
        else
        {
            if (bExtendBeyondDateLine)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "ExtendBeyondDateLine disabled, since BoundingBox of "
                         "%s is missing",
                         oTMS.osSRS.c_str());
                bExtendBeyondDateLine = FALSE;
            }
        }

        // Otherwise default to reproject a layer bounding box expressed in
        // another SRS
        if (!bHasAOI && !aoMapBoundingBox.empty() &&
            (eExtentMethod == AUTO || eExtentMethod == LAYER_BBOX))
        {
            std::map<CPLString, OGREnvelope>::iterator oIter =
                aoMapBoundingBox.begin();
            for (; oIter != aoMapBoundingBox.end(); ++oIter)
            {
                OGRSpatialReference oSRS;
                oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                if (oSRS.SetFromUserInput(
                        FixCRSName(oIter->first),
                        OGRSpatialReference::
                            SET_FROM_USER_INPUT_LIMITATIONS_get()) ==
                    OGRERR_NONE)
                {
                    // Check if this doesn't match the most precise tile matrix
                    // by densifying its contour
                    const WMTSTileMatrix &oTM = oTMS.aoTM.back();

                    bool bMatchFound = false;
                    const char *pszProjectionTMS =
                        oTMS.oSRS.GetAttrValue("PROJECTION");
                    const char *pszProjectionBBOX =
                        oSRS.GetAttrValue("PROJECTION");
                    const bool bIsTMerc =
                        (pszProjectionTMS != nullptr &&
                         EQUAL(pszProjectionTMS, SRS_PT_TRANSVERSE_MERCATOR)) ||
                        (pszProjectionBBOX != nullptr &&
                         EQUAL(pszProjectionBBOX, SRS_PT_TRANSVERSE_MERCATOR));
                    // If one of the 2 SRS is a TMerc, try with classical tmerc
                    // or etmerc.
                    for (int j = 0; j < (bIsTMerc ? 2 : 1); j++)
                    {
                        CPLString osOldVal = CPLGetThreadLocalConfigOption(
                            "OSR_USE_APPROX_TMERC", "");
                        if (bIsTMerc)
                        {
                            CPLSetThreadLocalConfigOption(
                                "OSR_USE_APPROX_TMERC",
                                (j == 0) ? "NO" : "YES");
                        }
                        OGRCoordinateTransformation *poRevCT =
                            OGRCreateCoordinateTransformation(&oTMS.oSRS,
                                                              &oSRS);
                        if (bIsTMerc)
                        {
                            CPLSetThreadLocalConfigOption(
                                "OSR_USE_APPROX_TMERC",
                                osOldVal.empty() ? nullptr : osOldVal.c_str());
                        }
                        if (poRevCT != nullptr)
                        {
                            const auto sTMExtent = oTM.GetExtent();
                            const double dfX0 = sTMExtent.MinX;
                            const double dfY1 = sTMExtent.MaxY;
                            const double dfX1 = sTMExtent.MaxX;
                            const double dfY0 = sTMExtent.MinY;
                            double dfXMin =
                                std::numeric_limits<double>::infinity();
                            double dfYMin =
                                std::numeric_limits<double>::infinity();
                            double dfXMax =
                                -std::numeric_limits<double>::infinity();
                            double dfYMax =
                                -std::numeric_limits<double>::infinity();

                            const int NSTEPS = 20;
                            for (int i = 0; i <= NSTEPS; i++)
                            {
                                double dfX = dfX0 + (dfX1 - dfX0) * i / NSTEPS;
                                double dfY = dfY0;
                                if (poRevCT->Transform(1, &dfX, &dfY))
                                {
                                    dfXMin = std::min(dfXMin, dfX);
                                    dfYMin = std::min(dfYMin, dfY);
                                    dfXMax = std::max(dfXMax, dfX);
                                    dfYMax = std::max(dfYMax, dfY);
                                }

                                dfX = dfX0 + (dfX1 - dfX0) * i / NSTEPS;
                                dfY = dfY1;
                                if (poRevCT->Transform(1, &dfX, &dfY))
                                {
                                    dfXMin = std::min(dfXMin, dfX);
                                    dfYMin = std::min(dfYMin, dfY);
                                    dfXMax = std::max(dfXMax, dfX);
                                    dfYMax = std::max(dfYMax, dfY);
                                }

                                dfX = dfX0;
                                dfY = dfY0 + (dfY1 - dfY0) * i / NSTEPS;
                                if (poRevCT->Transform(1, &dfX, &dfY))
                                {
                                    dfXMin = std::min(dfXMin, dfX);
                                    dfYMin = std::min(dfYMin, dfY);
                                    dfXMax = std::max(dfXMax, dfX);
                                    dfYMax = std::max(dfYMax, dfY);
                                }

                                dfX = dfX1;
                                dfY = dfY0 + (dfY1 - dfY0) * i / NSTEPS;
                                if (poRevCT->Transform(1, &dfX, &dfY))
                                {
                                    dfXMin = std::min(dfXMin, dfX);
                                    dfYMin = std::min(dfYMin, dfY);
                                    dfXMax = std::max(dfXMax, dfX);
                                    dfYMax = std::max(dfYMax, dfY);
                                }
                            }

                            delete poRevCT;
#ifdef DEBUG_VERBOSE
                            CPLDebug(
                                "WMTS",
                                "Reprojected densified bbox of most "
                                "precise tile matrix in %s: %.8g %8g %8g %8g",
                                oIter->first.c_str(), dfXMin, dfYMin, dfXMax,
                                dfYMax);
#endif
                            if (fabs(oIter->second.MinX - dfXMin) <
                                    1e-5 * std::max(fabs(oIter->second.MinX),
                                                    fabs(dfXMin)) &&
                                fabs(oIter->second.MinY - dfYMin) <
                                    1e-5 * std::max(fabs(oIter->second.MinY),
                                                    fabs(dfYMin)) &&
                                fabs(oIter->second.MaxX - dfXMax) <
                                    1e-5 * std::max(fabs(oIter->second.MaxX),
                                                    fabs(dfXMax)) &&
                                fabs(oIter->second.MaxY - dfYMax) <
                                    1e-5 * std::max(fabs(oIter->second.MaxY),
                                                    fabs(dfYMax)))
                            {
                                bMatchFound = true;
#ifdef DEBUG_VERBOSE
                                CPLDebug("WMTS",
                                         "Matches layer bounding box, so "
                                         "that one is not significant");
#endif
                                break;
                            }
                        }
                    }

                    if (bMatchFound)
                    {
                        if (eExtentMethod == LAYER_BBOX)
                            eExtentMethod = MOST_PRECISE_TILE_MATRIX;
                        break;
                    }

                    // Otherwise try to reproject the bounding box of the
                    // layer from its SRS to the TMS SRS. Except in some cases
                    // where this would result in non-sense. (this could be
                    // improved !)
                    if (!(bIsTMerc && oSRS.IsGeographic() &&
                          fabs(oIter->second.MinX - -180) < 1e-8 &&
                          fabs(oIter->second.MaxX - 180) < 1e-8))
                    {
                        OGRCoordinateTransformation *poCT =
                            OGRCreateCoordinateTransformation(&oSRS,
                                                              &oTMS.oSRS);
                        if (poCT != nullptr)
                        {
                            double dfX1 = oIter->second.MinX;
                            double dfY1 = oIter->second.MinY;
                            double dfX2 = oIter->second.MaxX;
                            double dfY2 = oIter->second.MinY;
                            double dfX3 = oIter->second.MaxX;
                            double dfY3 = oIter->second.MaxY;
                            double dfX4 = oIter->second.MinX;
                            double dfY4 = oIter->second.MaxY;
                            if (poCT->Transform(1, &dfX1, &dfY1) &&
                                poCT->Transform(1, &dfX2, &dfY2) &&
                                poCT->Transform(1, &dfX3, &dfY3) &&
                                poCT->Transform(1, &dfX4, &dfY4))
                            {
                                sAOI.MinX = std::min(std::min(dfX1, dfX2),
                                                     std::min(dfX3, dfX4));
                                sAOI.MinY = std::min(std::min(dfY1, dfY2),
                                                     std::min(dfY3, dfY4));
                                sAOI.MaxX = std::max(std::max(dfX1, dfX2),
                                                     std::max(dfX3, dfX4));
                                sAOI.MaxY = std::max(std::max(dfY1, dfY2),
                                                     std::max(dfY3, dfY4));
                                bHasAOI = TRUE;
                                bAOIFromLayer = true;
                            }
                            delete poCT;
                        }
                    }
                    break;
                }
            }
        }

        // Clip the computed AOI with the union of the extent of the tile
        // matrices
        if (bHasAOI && !bExtendBeyondDateLine)
        {
            OGREnvelope sUnionTM;
            for (const WMTSTileMatrix &oTM : oTMS.aoTM)
            {
                if (!sUnionTM.IsInit())
                    sUnionTM = oTM.GetExtent();
                else
                    sUnionTM.Merge(oTM.GetExtent());
            }
            sAOI.Intersect(sUnionTM);
        }

        // Otherwise default to BoundingBox of the TMS
        if (!bHasAOI && oTMS.bBoundingBoxValid &&
            (eExtentMethod == AUTO || eExtentMethod == TILE_MATRIX_SET))
        {
            CPLDebug("WMTS", "Using TMS bounding box as layer extent");
            sAOI = oTMS.sBoundingBox;
            bHasAOI = TRUE;
        }

        // Otherwise default to implied BoundingBox of the most precise TM
        if (!bHasAOI && (eExtentMethod == AUTO ||
                         eExtentMethod == MOST_PRECISE_TILE_MATRIX))
        {
            const WMTSTileMatrix &oTM = oTMS.aoTM.back();
            CPLDebug("WMTS", "Using TM level %s bounding box as layer extent",
                     oTM.osIdentifier.c_str());

            sAOI = oTM.GetExtent();
            bHasAOI = TRUE;
        }

        if (!bHasAOI)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Could not determine raster extent");
            CPLDestroyXMLNode(psXML);
            delete poDS;
            return nullptr;
        }

        if (CPLTestBool(CSLFetchNameValueDef(
                poOpenInfo->papszOpenOptions,
                "CLIP_EXTENT_WITH_MOST_PRECISE_TILE_MATRIX",
                bAOIFromLayer ? "NO" : "YES")))
        {
            // Clip with implied BoundingBox of the most precise TM
            // Useful for http://tileserver.maptiler.com/wmts
            const WMTSTileMatrix &oTM = oTMS.aoTM.back();
            const OGREnvelope sTMExtent = oTM.GetExtent();
            OGREnvelope sAOINew(sAOI);

            // For
            // https://data.linz.govt.nz/services;key=XXXXXXXX/wmts/1.0.0/set/69/WMTSCapabilities.xml
            // only clip in Y since there's a warp over dateline.
            // Update: it sems that the content of the server has changed since
            // initial coding. So do X clipping in default mode.
            if (!bExtendBeyondDateLine)
            {
                sAOINew.MinX = std::max(sAOI.MinX, sTMExtent.MinX);
                sAOINew.MaxX = std::min(sAOI.MaxX, sTMExtent.MaxX);
            }
            sAOINew.MaxY = std::min(sAOI.MaxY, sTMExtent.MaxY);
            sAOINew.MinY = std::max(sAOI.MinY, sTMExtent.MinY);
            if (sAOI != sAOINew)
            {
                CPLDebug(
                    "WMTS",
                    "Layer extent has been restricted from "
                    "(%f,%f,%f,%f) to (%f,%f,%f,%f) using the "
                    "implied bounding box of the most precise tile matrix. "
                    "You may disable this by specifying the "
                    "CLIP_EXTENT_WITH_MOST_PRECISE_TILE_MATRIX open option "
                    "to NO.",
                    sAOI.MinX, sAOI.MinY, sAOI.MaxX, sAOI.MaxY, sAOINew.MinX,
                    sAOINew.MinY, sAOINew.MaxX, sAOINew.MaxY);
            }
            sAOI = sAOINew;
        }

        // Clip with limits of most precise TM when available
        if (CPLTestBool(CSLFetchNameValueDef(
                poOpenInfo->papszOpenOptions,
                "CLIP_EXTENT_WITH_MOST_PRECISE_TILE_MATRIX_LIMITS",
                bAOIFromLayer ? "NO" : "YES")))
        {
            const WMTSTileMatrix &oTM = oTMS.aoTM.back();
            if (aoMapTileMatrixLimits.find(oTM.osIdentifier) !=
                aoMapTileMatrixLimits.end())
            {
                OGREnvelope sAOINew(sAOI);

                const WMTSTileMatrixLimits &oTMLimits =
                    aoMapTileMatrixLimits[oTM.osIdentifier];
                const OGREnvelope sTMLimitsExtent = oTMLimits.GetExtent(oTM);
                sAOINew.Intersect(sTMLimitsExtent);

                if (sAOI != sAOINew)
                {
                    CPLDebug(
                        "WMTS",
                        "Layer extent has been restricted from "
                        "(%f,%f,%f,%f) to (%f,%f,%f,%f) using the "
                        "implied bounding box of the most precise tile matrix. "
                        "You may disable this by specifying the "
                        "CLIP_EXTENT_WITH_MOST_PRECISE_TILE_MATRIX_LIMITS open "
                        "option "
                        "to NO.",
                        sAOI.MinX, sAOI.MinY, sAOI.MaxX, sAOI.MaxY,
                        sAOINew.MinX, sAOINew.MinY, sAOINew.MaxX, sAOINew.MaxY);
                }
                sAOI = sAOINew;
            }
        }

        if (!osProjection.empty())
        {
            poDS->m_oSRS.SetFromUserInput(
                osProjection,
                OGRSpatialReference::SET_FROM_USER_INPUT_LIMITATIONS_get());
        }
        if (poDS->m_oSRS.IsEmpty())
        {
            poDS->m_oSRS = oTMS.oSRS;
        }

        if (osURLTileTemplate.empty())
        {
            osURLTileTemplate = GetOperationKVPURL(psXML, "GetTile");
            if (osURLTileTemplate.empty())
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "No RESTful nor KVP GetTile operation found");
                CPLDestroyXMLNode(psXML);
                delete poDS;
                return nullptr;
            }
            osURLTileTemplate =
                CPLURLAddKVP(osURLTileTemplate, "service", "WMTS");
            osURLTileTemplate =
                CPLURLAddKVP(osURLTileTemplate, "request", "GetTile");
            osURLTileTemplate =
                CPLURLAddKVP(osURLTileTemplate, "version", "1.0.0");
            osURLTileTemplate =
                CPLURLAddKVP(osURLTileTemplate, "layer", osSelectLayer);
            osURLTileTemplate =
                CPLURLAddKVP(osURLTileTemplate, "style", osSelectStyle);
            osURLTileTemplate =
                CPLURLAddKVP(osURLTileTemplate, "format", osSelectTileFormat);
            osURLTileTemplate =
                CPLURLAddKVP(osURLTileTemplate, "TileMatrixSet", osSelectTMS);
            osURLTileTemplate += "&TileMatrix={TileMatrix}";
            osURLTileTemplate += "&TileRow=${y}";
            osURLTileTemplate += "&TileCol=${x}";

            std::map<CPLString, CPLString>::iterator oIter =
                aoMapDimensions.begin();
            for (; oIter != aoMapDimensions.end(); ++oIter)
            {
                osURLTileTemplate = CPLURLAddKVP(osURLTileTemplate,
                                                 oIter->first, oIter->second);
            }
            // CPLDebug("WMTS", "osURLTileTemplate = %s",
            // osURLTileTemplate.c_str());
        }
        else
        {
            osURLTileTemplate =
                Replace(osURLTileTemplate, "{Style}", osSelectStyle);
            osURLTileTemplate =
                Replace(osURLTileTemplate, "{TileMatrixSet}", osSelectTMS);
            osURLTileTemplate = Replace(osURLTileTemplate, "{TileCol}", "${x}");
            osURLTileTemplate = Replace(osURLTileTemplate, "{TileRow}", "${y}");

            std::map<CPLString, CPLString>::iterator oIter =
                aoMapDimensions.begin();
            for (; oIter != aoMapDimensions.end(); ++oIter)
            {
                osURLTileTemplate = Replace(
                    osURLTileTemplate, CPLSPrintf("{%s}", oIter->first.c_str()),
                    oIter->second);
            }
        }
        osURLTileTemplate += osExtraQueryParameters;

        if (osURLFeatureInfoTemplate.empty() && !osSelectInfoFormat.empty())
        {
            osURLFeatureInfoTemplate =
                GetOperationKVPURL(psXML, "GetFeatureInfo");
            if (!osURLFeatureInfoTemplate.empty())
            {
                osURLFeatureInfoTemplate =
                    CPLURLAddKVP(osURLFeatureInfoTemplate, "service", "WMTS");
                osURLFeatureInfoTemplate = CPLURLAddKVP(
                    osURLFeatureInfoTemplate, "request", "GetFeatureInfo");
                osURLFeatureInfoTemplate =
                    CPLURLAddKVP(osURLFeatureInfoTemplate, "version", "1.0.0");
                osURLFeatureInfoTemplate = CPLURLAddKVP(
                    osURLFeatureInfoTemplate, "layer", osSelectLayer);
                osURLFeatureInfoTemplate = CPLURLAddKVP(
                    osURLFeatureInfoTemplate, "style", osSelectStyle);
                // osURLFeatureInfoTemplate =
                // CPLURLAddKVP(osURLFeatureInfoTemplate, "format",
                // osSelectTileFormat);
                osURLFeatureInfoTemplate = CPLURLAddKVP(
                    osURLFeatureInfoTemplate, "InfoFormat", osSelectInfoFormat);
                osURLFeatureInfoTemplate += "&TileMatrixSet={TileMatrixSet}";
                osURLFeatureInfoTemplate += "&TileMatrix={TileMatrix}";
                osURLFeatureInfoTemplate += "&TileRow={TileRow}";
                osURLFeatureInfoTemplate += "&TileCol={TileCol}";
                osURLFeatureInfoTemplate += "&J={J}";
                osURLFeatureInfoTemplate += "&I={I}";

                std::map<CPLString, CPLString>::iterator oIter =
                    aoMapDimensions.begin();
                for (; oIter != aoMapDimensions.end(); ++oIter)
                {
                    osURLFeatureInfoTemplate = CPLURLAddKVP(
                        osURLFeatureInfoTemplate, oIter->first, oIter->second);
                }
                // CPLDebug("WMTS", "osURLFeatureInfoTemplate = %s",
                // osURLFeatureInfoTemplate.c_str());
            }
        }
        else
        {
            osURLFeatureInfoTemplate =
                Replace(osURLFeatureInfoTemplate, "{Style}", osSelectStyle);

            std::map<CPLString, CPLString>::iterator oIter =
                aoMapDimensions.begin();
            for (; oIter != aoMapDimensions.end(); ++oIter)
            {
                osURLFeatureInfoTemplate = Replace(
                    osURLFeatureInfoTemplate,
                    CPLSPrintf("{%s}", oIter->first.c_str()), oIter->second);
            }
        }
        if (!osURLFeatureInfoTemplate.empty())
            osURLFeatureInfoTemplate += osExtraQueryParameters;
        poDS->osURLFeatureInfoTemplate = osURLFeatureInfoTemplate;
        CPL_IGNORE_RET_VAL(osURLFeatureInfoTemplate);

        // Build all TMS datasets, wrapped in VRT datasets
        for (int i = static_cast<int>(oTMS.aoTM.size() - 1); i >= 0; i--)
        {
            const WMTSTileMatrix &oTM = oTMS.aoTM[i];
            double dfRasterXSize = (sAOI.MaxX - sAOI.MinX) / oTM.dfPixelSize;
            double dfRasterYSize = (sAOI.MaxY - sAOI.MinY) / oTM.dfPixelSize;
            if (dfRasterXSize > INT_MAX || dfRasterYSize > INT_MAX)
            {
                continue;
            }

            if (poDS->apoDatasets.empty())
            {
                // Align AOI on pixel boundaries with respect to TopLeftCorner
                // of this tile matrix
                poDS->m_gt[0] =
                    oTM.dfTLX +
                    floor((sAOI.MinX - oTM.dfTLX) / oTM.dfPixelSize + 1e-10) *
                        oTM.dfPixelSize;
                poDS->m_gt[1] = oTM.dfPixelSize;
                poDS->m_gt[2] = 0.0;
                poDS->m_gt[3] =
                    oTM.dfTLY +
                    ceil((sAOI.MaxY - oTM.dfTLY) / oTM.dfPixelSize - 1e-10) *
                        oTM.dfPixelSize;
                poDS->m_gt[4] = 0.0;
                poDS->m_gt[5] = -oTM.dfPixelSize;
                poDS->nRasterXSize =
                    int(0.5 + (sAOI.MaxX - poDS->m_gt[0]) / oTM.dfPixelSize);
                poDS->nRasterYSize =
                    int(0.5 + (poDS->m_gt[3] - sAOI.MinY) / oTM.dfPixelSize);
            }

            const int nRasterXSize =
                int(0.5 + poDS->nRasterXSize / oTM.dfPixelSize * poDS->m_gt[1]);
            const int nRasterYSize =
                int(0.5 + poDS->nRasterYSize / oTM.dfPixelSize * poDS->m_gt[1]);
            if (!poDS->apoDatasets.empty() &&
                (nRasterXSize < 128 || nRasterYSize < 128))
            {
                break;
            }
            CPLString osURL(
                Replace(osURLTileTemplate, "{TileMatrix}", oTM.osIdentifier));

            const double dfTileWidthUnits = oTM.dfPixelSize * oTM.nTileWidth;
            const double dfTileHeightUnits = oTM.dfPixelSize * oTM.nTileHeight;

            // Get bounds of this tile matrix / tile matrix limits
            auto sTMExtent = oTM.GetExtent();
            if (aoMapTileMatrixLimits.find(oTM.osIdentifier) !=
                aoMapTileMatrixLimits.end())
            {
                const WMTSTileMatrixLimits &oTMLimits =
                    aoMapTileMatrixLimits[oTM.osIdentifier];
                sTMExtent.Intersect(oTMLimits.GetExtent(oTM));
            }

            // Compute the shift in terms of tiles between AOI and TM origin
            const int nTileX =
                static_cast<int>(floor(std::max(sTMExtent.MinX, poDS->m_gt[0]) -
                                       oTM.dfTLX + 1e-10) /
                                 dfTileWidthUnits);
            const int nTileY = static_cast<int>(
                floor(oTM.dfTLY - std::min(poDS->m_gt[3], sTMExtent.MaxY) +
                      1e-10) /
                dfTileHeightUnits);

            // Compute extent of this zoom level slightly larger than the AOI
            // and aligned on tile boundaries at this TM
            double dfULX = oTM.dfTLX + nTileX * dfTileWidthUnits;
            double dfULY = oTM.dfTLY - nTileY * dfTileHeightUnits;
            double dfLRX = poDS->m_gt[0] + poDS->nRasterXSize * poDS->m_gt[1];
            double dfLRY = poDS->m_gt[3] + poDS->nRasterYSize * poDS->m_gt[5];
            dfLRX = dfULX + ceil((dfLRX - dfULX) / dfTileWidthUnits - 1e-10) *
                                dfTileWidthUnits;
            dfLRY = dfULY + floor((dfLRY - dfULY) / dfTileHeightUnits + 1e-10) *
                                dfTileHeightUnits;

            // Clip TMS extent to the one of this TM
            if (!bExtendBeyondDateLine)
                dfLRX = std::min(dfLRX, sTMExtent.MaxX);
            dfLRY = std::max(dfLRY, sTMExtent.MinY);

            const double dfSizeX = 0.5 + (dfLRX - dfULX) / oTM.dfPixelSize;
            const double dfSizeY = 0.5 + (dfULY - dfLRY) / oTM.dfPixelSize;
            if (dfSizeX > INT_MAX || dfSizeY > INT_MAX)
            {
                continue;
            }
            if (poDS->apoDatasets.empty())
            {
                CPLDebug("WMTS", "Using tilematrix=%s (zoom level %d)",
                         oTMS.aoTM[i].osIdentifier.c_str(), i);
                oTMS.aoTM.resize(1 + i);
                poDS->oTMS = oTMS;
            }

            const int nSizeX = static_cast<int>(dfSizeX);
            const int nSizeY = static_cast<int>(dfSizeY);

            const double dfDateLineX =
                oTM.dfTLX + oTM.nMatrixWidth * dfTileWidthUnits;
            const int nSizeX1 =
                int(0.5 + (dfDateLineX - dfULX) / oTM.dfPixelSize);
            const int nSizeX2 =
                int(0.5 + (dfLRX - dfDateLineX) / oTM.dfPixelSize);
            if (bExtendBeyondDateLine && dfDateLineX > dfLRX)
            {
                CPLDebug("WMTS", "ExtendBeyondDateLine ignored in that case");
                bExtendBeyondDateLine = FALSE;
            }

#define WMS_TMS_TEMPLATE                                                       \
    "<GDAL_WMS>"                                                               \
    "<Service name=\"TMS\">"                                                   \
    "    <ServerUrl>%s</ServerUrl>"                                            \
    "</Service>"                                                               \
    "<DataWindow>"                                                             \
    "    <UpperLeftX>%.16g</UpperLeftX>"                                       \
    "    <UpperLeftY>%.16g</UpperLeftY>"                                       \
    "    <LowerRightX>%.16g</LowerRightX>"                                     \
    "    <LowerRightY>%.16g</LowerRightY>"                                     \
    "    <TileLevel>0</TileLevel>"                                             \
    "    <TileX>%d</TileX>"                                                    \
    "    <TileY>%d</TileY>"                                                    \
    "    <SizeX>%d</SizeX>"                                                    \
    "    <SizeY>%d</SizeY>"                                                    \
    "    <YOrigin>top</YOrigin>"                                               \
    "</DataWindow>"                                                            \
    "<BlockSizeX>%d</BlockSizeX>"                                              \
    "<BlockSizeY>%d</BlockSizeY>"                                              \
    "<BandsCount>%d</BandsCount>"                                              \
    "<DataType>%s</DataType>"                                                  \
    "%s"                                                                       \
    "</GDAL_WMS>"

            CPLString osStr(CPLSPrintf(
                WMS_TMS_TEMPLATE, WMTSEscapeXML(osURL).c_str(), dfULX, dfULY,
                (bExtendBeyondDateLine) ? dfDateLineX : dfLRX, dfLRY, nTileX,
                nTileY, (bExtendBeyondDateLine) ? nSizeX1 : nSizeX, nSizeY,
                oTM.nTileWidth, oTM.nTileHeight, nBands,
                GDALGetDataTypeName(eDataType), osOtherXML.c_str()));
            const auto eLastErrorType = CPLGetLastErrorType();
            const auto eLastErrorNum = CPLGetLastErrorNo();
            const std::string osLastErrorMsg = CPLGetLastErrorMsg();
            GDALDataset *poWMSDS = GDALDataset::Open(
                osStr, GDAL_OF_RASTER | GDAL_OF_SHARED | GDAL_OF_VERBOSE_ERROR,
                nullptr, nullptr, nullptr);
            if (poWMSDS == nullptr)
            {
                CPLDestroyXMLNode(psXML);
                delete poDS;
                return nullptr;
            }
            // Restore error state to what it was prior to WMS dataset opening
            // if WMS dataset opening did not cause any new error to be emitted
            if (CPLGetLastErrorType() == CE_None)
                CPLErrorSetState(eLastErrorType, eLastErrorNum,
                                 osLastErrorMsg.c_str());

            VRTDatasetH hVRTDS = VRTCreate(nRasterXSize, nRasterYSize);
            for (int iBand = 1; iBand <= nBands; iBand++)
            {
                VRTAddBand(hVRTDS, eDataType, nullptr);
            }

            int nSrcXOff, nSrcYOff, nDstXOff, nDstYOff;

            nSrcXOff = 0;
            nDstXOff = static_cast<int>(
                std::round((dfULX - poDS->m_gt[0]) / oTM.dfPixelSize));

            nSrcYOff = 0;
            nDstYOff = static_cast<int>(
                std::round((poDS->m_gt[3] - dfULY) / oTM.dfPixelSize));

            if (bExtendBeyondDateLine)
            {
                int nSrcXOff2, nDstXOff2;

                nSrcXOff2 = 0;
                nDstXOff2 = static_cast<int>(std::round(
                    (dfDateLineX - poDS->m_gt[0]) / oTM.dfPixelSize));

                osStr = CPLSPrintf(
                    WMS_TMS_TEMPLATE, WMTSEscapeXML(osURL).c_str(),
                    -dfDateLineX, dfULY, dfLRX - 2 * dfDateLineX, dfLRY, 0,
                    nTileY, nSizeX2, nSizeY, oTM.nTileWidth, oTM.nTileHeight,
                    nBands, GDALGetDataTypeName(eDataType), osOtherXML.c_str());

                GDALDataset *poWMSDS2 =
                    GDALDataset::Open(osStr, GDAL_OF_RASTER | GDAL_OF_SHARED,
                                      nullptr, nullptr, nullptr);
                CPLAssert(poWMSDS2);

                for (int iBand = 1; iBand <= nBands; iBand++)
                {
                    VRTSourcedRasterBandH hVRTBand =
                        reinterpret_cast<VRTSourcedRasterBandH>(
                            GDALGetRasterBand(hVRTDS, iBand));
                    VRTAddSimpleSource(
                        hVRTBand, GDALGetRasterBand(poWMSDS, iBand), nSrcXOff,
                        nSrcYOff, nSizeX1, nSizeY, nDstXOff, nDstYOff, nSizeX1,
                        nSizeY, "NEAR", VRT_NODATA_UNSET);
                    VRTAddSimpleSource(
                        hVRTBand, GDALGetRasterBand(poWMSDS2, iBand), nSrcXOff2,
                        nSrcYOff, nSizeX2, nSizeY, nDstXOff2, nDstYOff, nSizeX2,
                        nSizeY, "NEAR", VRT_NODATA_UNSET);
                }

                poWMSDS2->Dereference();
            }
            else
            {
                for (int iBand = 1; iBand <= nBands; iBand++)
                {
                    VRTSourcedRasterBandH hVRTBand =
                        reinterpret_cast<VRTSourcedRasterBandH>(
                            GDALGetRasterBand(hVRTDS, iBand));
                    VRTAddSimpleSource(
                        hVRTBand, GDALGetRasterBand(poWMSDS, iBand), nSrcXOff,
                        nSrcYOff, nSizeX, nSizeY, nDstXOff, nDstYOff, nSizeX,
                        nSizeY, "NEAR", VRT_NODATA_UNSET);
                }
            }

            poWMSDS->Dereference();

            poDS->apoDatasets.push_back(GDALDataset::FromHandle(hVRTDS));
        }

        if (poDS->apoDatasets.empty())
        {
            CPLError(CE_Failure, CPLE_AppDefined, "No zoom level found");
            CPLDestroyXMLNode(psXML);
            delete poDS;
            return nullptr;
        }

        poDS->SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
        for (int i = 0; i < nBands; i++)
            poDS->SetBand(i + 1, new WMTSBand(poDS, i + 1, eDataType));

        poDS->osXML = "<GDAL_WMTS>\n";
        poDS->osXML += "  <GetCapabilitiesUrl>" +
                       WMTSEscapeXML(osGetCapabilitiesURL) +
                       "</GetCapabilitiesUrl>\n";
        if (!osSelectLayer.empty())
            poDS->osXML +=
                "  <Layer>" + WMTSEscapeXML(osSelectLayer) + "</Layer>\n";
        if (!osSelectStyle.empty())
            poDS->osXML +=
                "  <Style>" + WMTSEscapeXML(osSelectStyle) + "</Style>\n";
        if (!osSelectTMS.empty())
            poDS->osXML += "  <TileMatrixSet>" + WMTSEscapeXML(osSelectTMS) +
                           "</TileMatrixSet>\n";
        if (!osMaxTileMatrixIdentifier.empty())
            poDS->osXML += "  <TileMatrix>" +
                           WMTSEscapeXML(osMaxTileMatrixIdentifier) +
                           "</TileMatrix>\n";
        if (nUserMaxZoomLevel >= 0)
            poDS->osXML += "  <ZoomLevel>" +
                           CPLString().Printf("%d", nUserMaxZoomLevel) +
                           "</ZoomLevel>\n";
        if (nCountTileFormat > 1 && !osSelectTileFormat.empty())
            poDS->osXML += "  <Format>" + WMTSEscapeXML(osSelectTileFormat) +
                           "</Format>\n";
        if (nCountInfoFormat > 1 && !osSelectInfoFormat.empty())
            poDS->osXML += "  <InfoFormat>" +
                           WMTSEscapeXML(osSelectInfoFormat) +
                           "</InfoFormat>\n";
        poDS->osXML += "  <DataWindow>\n";
        poDS->osXML +=
            CPLSPrintf("    <UpperLeftX>%.16g</UpperLeftX>\n", poDS->m_gt[0]);
        poDS->osXML +=
            CPLSPrintf("    <UpperLeftY>%.16g</UpperLeftY>\n", poDS->m_gt[3]);
        poDS->osXML +=
            CPLSPrintf("    <LowerRightX>%.16g</LowerRightX>\n",
                       poDS->m_gt[0] + poDS->m_gt[1] * poDS->nRasterXSize);
        poDS->osXML +=
            CPLSPrintf("    <LowerRightY>%.16g</LowerRightY>\n",
                       poDS->m_gt[3] + poDS->m_gt[5] * poDS->nRasterYSize);
        poDS->osXML += "  </DataWindow>\n";
        if (bExtendBeyondDateLine)
            poDS->osXML +=
                "  <ExtendBeyondDateLine>true</ExtendBeyondDateLine>\n";
        poDS->osXML += CPLSPrintf("  <BandsCount>%d</BandsCount>\n", nBands);
        poDS->osXML += CPLSPrintf("  <DataType>%s</DataType>\n",
                                  GDALGetDataTypeName(eDataType));
        poDS->osXML += "  <Cache />\n";
        poDS->osXML += "  <UnsafeSSL>true</UnsafeSSL>\n";
        poDS->osXML += "  <ZeroBlockHttpCodes>204,404</ZeroBlockHttpCodes>\n";
        poDS->osXML +=
            "  <ZeroBlockOnServerException>true</ZeroBlockOnServerException>\n";
        poDS->osXML += "</GDAL_WMTS>\n";
    }

    CPLDestroyXMLNode(psXML);

    poDS->SetPamFlags(poDS->GetPamFlags() & ~GPF_DIRTY);
    return poDS;
}

/************************************************************************/
/*                             CreateCopy()                             */
/************************************************************************/

GDALDataset *WMTSDataset::CreateCopy(const char *pszFilename,
                                     GDALDataset *poSrcDS,
                                     CPL_UNUSED int bStrict,
                                     CPL_UNUSED char **papszOptions,
                                     CPL_UNUSED GDALProgressFunc pfnProgress,
                                     CPL_UNUSED void *pProgressData)
{
    if (poSrcDS->GetDriver() == nullptr ||
        poSrcDS->GetDriver() != GDALGetDriverByName("WMTS"))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Source dataset must be a WMTS dataset");
        return nullptr;
    }

    const char *pszXML = poSrcDS->GetMetadataItem("XML", "WMTS");
    if (pszXML == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot get XML definition of source WMTS dataset");
        return nullptr;
    }

    VSILFILE *fp = VSIFOpenL(pszFilename, "wb");
    if (fp == nullptr)
        return nullptr;

    VSIFWriteL(pszXML, 1, strlen(pszXML), fp);
    VSIFCloseL(fp);

    GDALOpenInfo oOpenInfo(pszFilename, GA_ReadOnly);
    return Open(&oOpenInfo);
}

/************************************************************************/
/*                       GDALRegister_WMTS()                            */
/************************************************************************/

void GDALRegister_WMTS()

{
    if (!GDAL_CHECK_VERSION("WMTS driver"))
        return;

    if (GDALGetDriverByName(DRIVER_NAME) != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();
    WMTSDriverSetCommonMetadata(poDriver);

    poDriver->pfnOpen = WMTSDataset::Open;
    poDriver->pfnCreateCopy = WMTSDataset::CreateCopy;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
