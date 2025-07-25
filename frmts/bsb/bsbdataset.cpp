/******************************************************************************
 *
 * Project:  BSB Reader
 * Purpose:  BSBDataset implementation for BSB format.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 * Copyright (c) 2001, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2008-2012, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "bsb_read.h"
#include "cpl_string.h"
#include "gdal_frmts.h"
#include "gdal_pam.h"
#include "ogr_spatialref.h"

#include <cstdlib>
#include <algorithm>

// Disabled as people may worry about the BSB patent
// #define BSB_CREATE

/************************************************************************/
/* ==================================================================== */
/*                              BSBDataset                              */
/* ==================================================================== */
/************************************************************************/

class BSBRasterBand;

class BSBDataset final : public GDALPamDataset
{
    int nGCPCount;
    GDAL_GCP *pasGCPList;
    OGRSpatialReference m_oGCPSRS{};

    GDALGeoTransform m_gt{};
    int bGeoTransformSet;

    void ScanForGCPs(bool isNos, const char *pszFilename);
    void ScanForGCPsNos(const char *pszFilename);
    void ScanForGCPsBSB();

    void ScanForCutline();

    static int IdentifyInternal(GDALOpenInfo *, bool &isNosOut);

  public:
    BSBDataset();
    ~BSBDataset() override;

    BSBInfo *psInfo;

    static GDALDataset *Open(GDALOpenInfo *);
    static int Identify(GDALOpenInfo *);

    int GetGCPCount() override;
    const OGRSpatialReference *GetSpatialRef() const override;
    const GDAL_GCP *GetGCPs() override;

    CPLErr GetGeoTransform(GDALGeoTransform &gt) const override;
    const OGRSpatialReference *GetGCPSpatialRef() const override;
};

/************************************************************************/
/* ==================================================================== */
/*                            BSBRasterBand                             */
/* ==================================================================== */
/************************************************************************/

class BSBRasterBand final : public GDALPamRasterBand
{
    GDALColorTable oCT;

  public:
    explicit BSBRasterBand(BSBDataset *);

    CPLErr IReadBlock(int, int, void *) override;
    GDALColorTable *GetColorTable() override;
    GDALColorInterp GetColorInterpretation() override;
};

/************************************************************************/
/*                           BSBRasterBand()                            */
/************************************************************************/

BSBRasterBand::BSBRasterBand(BSBDataset *poDSIn)

{
    poDS = poDSIn;
    nBand = 1;

    eDataType = GDT_Byte;

    nBlockXSize = poDS->GetRasterXSize();
    nBlockYSize = 1;

    // Note that the first color table entry is dropped, everything is
    // shifted down.
    for (int i = 0; i < poDSIn->psInfo->nPCTSize - 1; i++)
    {
        GDALColorEntry oColor = {poDSIn->psInfo->pabyPCT[i * 3 + 0 + 3],
                                 poDSIn->psInfo->pabyPCT[i * 3 + 1 + 3],
                                 poDSIn->psInfo->pabyPCT[i * 3 + 2 + 3], 255};

        oCT.SetColorEntry(i, &oColor);
    }
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr BSBRasterBand::IReadBlock(CPL_UNUSED int nBlockXOff, int nBlockYOff,
                                 void *pImage)
{
    BSBDataset *poGDS = cpl::down_cast<BSBDataset *>(poDS);
    GByte *pabyScanline = (GByte *)pImage;

    if (BSBReadScanline(poGDS->psInfo, nBlockYOff, pabyScanline))
    {
        for (int i = 0; i < nBlockXSize; i++)
        {
            /* The indices start at 1, except in case of some charts */
            /* where there are missing values, which are filled to 0 */
            /* by BSBReadScanline */
            if (pabyScanline[i] > 0)
                pabyScanline[i] -= 1;
        }

        return CE_None;
    }

    return CE_Failure;
}

/************************************************************************/
/*                           GetColorTable()                            */
/************************************************************************/

GDALColorTable *BSBRasterBand::GetColorTable()

{
    return &oCT;
}

/************************************************************************/
/*                       GetColorInterpretation()                       */
/************************************************************************/

GDALColorInterp BSBRasterBand::GetColorInterpretation()

{
    return GCI_PaletteIndex;
}

/************************************************************************/
/* ==================================================================== */
/*                              BSBDataset                              */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                           BSBDataset()                               */
/************************************************************************/

BSBDataset::BSBDataset()
    : nGCPCount(0), pasGCPList(nullptr), bGeoTransformSet(FALSE),
      psInfo(nullptr)
{
    m_oGCPSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    m_oGCPSRS.importFromWkt(SRS_WKT_WGS84_LAT_LONG);
}

/************************************************************************/
/*                            ~BSBDataset()                             */
/************************************************************************/

BSBDataset::~BSBDataset()

{
    FlushCache(true);

    GDALDeinitGCPs(nGCPCount, pasGCPList);
    CPLFree(pasGCPList);

    if (psInfo != nullptr)
        BSBClose(psInfo);
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr BSBDataset::GetGeoTransform(GDALGeoTransform &gt) const

{
    gt = m_gt;

    if (bGeoTransformSet)
        return CE_None;

    return CE_Failure;
}

/************************************************************************/
/*                          GetSpatialRef()                             */
/************************************************************************/

const OGRSpatialReference *BSBDataset::GetSpatialRef() const

{
    if (bGeoTransformSet)
        return &m_oGCPSRS;

    return nullptr;
}

/************************************************************************/
/*                     GDALHeuristicDatelineWrap()                      */
/************************************************************************/

static void GDALHeuristicDatelineWrap(int nPointCount, double *padfX)

{
    if (nPointCount < 2)
        return;

    /* -------------------------------------------------------------------- */
    /*      Work out what the longitude range will be centering on the      */
    /*      prime meridian (-180 to 180) and centering on the dateline      */
    /*      (0 to 360).                                                     */
    /* -------------------------------------------------------------------- */
    /* Following inits are useless but keep GCC happy */
    double dfX_PM_Min = 0.0;
    double dfX_PM_Max = 0.0;
    double dfX_Dateline_Min = 0.0;
    double dfX_Dateline_Max = 0.0;

    for (int i = 0; i < nPointCount; i++)
    {
        double dfX_PM = padfX[i];
        if (dfX_PM > 180)
            dfX_PM -= 360.0;

        double dfX_Dateline = padfX[i];
        if (dfX_Dateline < 0)
            dfX_Dateline += 360.0;

        if (i == 0)
        {
            dfX_PM_Min = dfX_PM;
            dfX_PM_Max = dfX_PM;
            dfX_Dateline_Min = dfX_Dateline;
            dfX_Dateline_Max = dfX_Dateline;
        }
        else
        {
            dfX_PM_Min = std::min(dfX_PM_Min, dfX_PM);
            dfX_PM_Max = std::max(dfX_PM_Max, dfX_PM);
            dfX_Dateline_Min = std::min(dfX_Dateline_Min, dfX_Dateline);
            dfX_Dateline_Max = std::max(dfX_Dateline_Max, dfX_Dateline);
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Do nothing if the range is always fairly small - no apparent    */
    /*      wrapping issues.                                                */
    /* -------------------------------------------------------------------- */
    if ((dfX_PM_Max - dfX_PM_Min) < 270.0 &&
        (dfX_Dateline_Max - dfX_Dateline_Min) < 270.0)
        return;

    /* -------------------------------------------------------------------- */
    /*      Do nothing if both approach have a wide range - best not to    */
    /*      fiddle if we aren't sure we are improving things.               */
    /* -------------------------------------------------------------------- */
    if ((dfX_PM_Max - dfX_PM_Min) > 270.0 &&
        (dfX_Dateline_Max - dfX_Dateline_Min) > 270.0)
        return;

    /* -------------------------------------------------------------------- */
    /*      Pick which way to transform things.                             */
    /* -------------------------------------------------------------------- */
    bool bUsePMWrap;

    if ((dfX_PM_Max - dfX_PM_Min) > 270.0 &&
        (dfX_Dateline_Max - dfX_Dateline_Min) < 270.0)
        bUsePMWrap = false;
    else
        bUsePMWrap = true;

    /* -------------------------------------------------------------------- */
    /*      Apply rewrapping.                                               */
    /* -------------------------------------------------------------------- */
    for (int i = 0; i < nPointCount; i++)
    {
        if (bUsePMWrap)
        {
            if (padfX[i] > 180)
                padfX[i] -= 360.0;
        }
        else
        {
            if (padfX[i] < 0)
                padfX[i] += 360.0;
        }
    }
}

/************************************************************************/
/*                   GDALHeuristicDatelineWrapGCPs()                    */
/************************************************************************/

static void GDALHeuristicDatelineWrapGCPs(int nPointCount, GDAL_GCP *pasGCPList)
{
    std::vector<double> oadfX;

    oadfX.resize(nPointCount);
    for (int i = 0; i < nPointCount; i++)
        oadfX[i] = pasGCPList[i].dfGCPX;

    GDALHeuristicDatelineWrap(nPointCount, &(oadfX[0]));

    for (int i = 0; i < nPointCount; i++)
        pasGCPList[i].dfGCPX = oadfX[i];
}

/************************************************************************/
/*                            ScanForGCPs()                             */
/************************************************************************/

void BSBDataset::ScanForGCPs(bool isNos, const char *pszFilename)

{
    /* -------------------------------------------------------------------- */
    /*      Collect GCPs as appropriate to source.                          */
    /* -------------------------------------------------------------------- */
    nGCPCount = 0;

    if (isNos)
    {
        ScanForGCPsNos(pszFilename);
    }
    else
    {
        ScanForGCPsBSB();
    }

    /* -------------------------------------------------------------------- */
    /*      Apply heuristics to re-wrap GCPs to maintain continuity        */
    /*      over the international dateline.                                */
    /* -------------------------------------------------------------------- */
    if (nGCPCount > 1)
        GDALHeuristicDatelineWrapGCPs(nGCPCount, pasGCPList);

    /* -------------------------------------------------------------------- */
    /*      Collect coordinate system related parameters from header.       */
    /* -------------------------------------------------------------------- */
    const char *pszKNP = nullptr;
    const char *pszKNQ = nullptr;

    for (int i = 0; psInfo->papszHeader[i] != nullptr; i++)
    {
        if (STARTS_WITH_CI(psInfo->papszHeader[i], "KNP/"))
        {
            pszKNP = psInfo->papszHeader[i];
            SetMetadataItem("BSB_KNP", pszKNP + 4);
        }
        if (STARTS_WITH_CI(psInfo->papszHeader[i], "KNQ/"))
        {
            pszKNQ = psInfo->papszHeader[i];
            SetMetadataItem("BSB_KNQ", pszKNQ + 4);
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Can we derive a reasonable coordinate system definition for     */
    /*      this file?  For now we keep it simple, just handling            */
    /*      mercator. In the future we should consider others.              */
    /* -------------------------------------------------------------------- */
    CPLString osUnderlyingSRS;
    if (pszKNP != nullptr)
    {
        const char *pszPR = strstr(pszKNP, "PR=");
        const char *pszGD = strstr(pszKNP, "GD=");
        const char *pszGEOGCS = SRS_WKT_WGS84_LAT_LONG;
        CPLString osPP;

        // Capture the PP string.
        const char *pszValue = strstr(pszKNP, "PP=");
        const char *pszEnd = pszValue ? strstr(pszValue, ",") : nullptr;
        if (pszValue && pszEnd)
            osPP.assign(pszValue + 3, pszEnd - pszValue - 3);

        // Look at the datum
        if (pszGD == nullptr)
        {
            /* no match. We'll default to EPSG:4326 */
        }
        else if (STARTS_WITH_CI(pszGD, "GD=European 1950"))
        {
            pszGEOGCS =
                "GEOGCS[\"ED50\",DATUM[\"European_Datum_1950\",SPHEROID["
                "\"International "
                "1924\",6378388,297,AUTHORITY[\"EPSG\",\"7022\"]],TOWGS84[-87,-"
                "98,-121,0,0,0,0],AUTHORITY[\"EPSG\",\"6230\"]],PRIMEM["
                "\"Greenwich\",0,AUTHORITY[\"EPSG\",\"8901\"]],UNIT[\"degree\","
                "0.0174532925199433,AUTHORITY[\"EPSG\",\"9122\"]],AUTHORITY["
                "\"EPSG\",\"4230\"]]";
        }

        // Look at the projection
        if (pszPR == nullptr)
        {
            /* no match */
        }
        else if (STARTS_WITH_CI(pszPR, "PR=MERCATOR") && nGCPCount > 0)
        {
            // We somewhat arbitrarily select our first GCPX as our
            // central meridian.  This is mostly helpful to ensure
            // that regions crossing the dateline will be contiguous
            // in mercator.
            osUnderlyingSRS.Printf(
                "PROJCS[\"Global "
                "Mercator\",%s,PROJECTION[\"Mercator_2SP\"],PARAMETER["
                "\"standard_parallel_1\",0],PARAMETER[\"latitude_of_origin\",0]"
                ",PARAMETER[\"central_meridian\",%d],PARAMETER[\"false_"
                "easting\",0],PARAMETER[\"false_northing\",0],UNIT[\"Meter\",1]"
                "]",
                pszGEOGCS, (int)pasGCPList[0].dfGCPX);
        }

        else if (STARTS_WITH_CI(pszPR, "PR=TRANSVERSE MERCATOR") &&
                 !osPP.empty())
        {

            osUnderlyingSRS.Printf(
                "PROJCS[\"unnamed\",%s,PROJECTION[\"Transverse_Mercator\"],"
                "PARAMETER[\"latitude_of_origin\",0],PARAMETER[\"central_"
                "meridian\",%s],PARAMETER[\"scale_factor\",1],PARAMETER["
                "\"false_easting\",0],PARAMETER[\"false_northing\",0],UNIT["
                "\"Meter\",1]]",
                pszGEOGCS, osPP.c_str());
        }

        else if (STARTS_WITH_CI(pszPR, "PR=UNIVERSAL TRANSVERSE MERCATOR") &&
                 !osPP.empty())
        {
            // This is not *really* UTM unless the central meridian
            // matches a zone which it does not in some (most?) maps.
            osUnderlyingSRS.Printf(
                "PROJCS[\"unnamed\",%s,PROJECTION[\"Transverse_Mercator\"],"
                "PARAMETER[\"latitude_of_origin\",0],PARAMETER[\"central_"
                "meridian\",%s],PARAMETER[\"scale_factor\",0.9996],PARAMETER["
                "\"false_easting\",500000],PARAMETER[\"false_northing\",0],"
                "UNIT[\"Meter\",1]]",
                pszGEOGCS, osPP.c_str());
        }

        else if (STARTS_WITH_CI(pszPR, "PR=POLYCONIC") && !osPP.empty())
        {
            osUnderlyingSRS.Printf(
                "PROJCS[\"unnamed\",%s,PROJECTION[\"Polyconic\"],PARAMETER["
                "\"latitude_of_origin\",0],PARAMETER[\"central_meridian\",%s],"
                "PARAMETER[\"false_easting\",0],PARAMETER[\"false_northing\",0]"
                ",UNIT[\"Meter\",1]]",
                pszGEOGCS, osPP.c_str());
        }

        else if (STARTS_WITH_CI(pszPR, "PR=LAMBERT CONFORMAL CONIC") &&
                 !osPP.empty() && pszKNQ != nullptr)
        {
            CPLString osP2, osP3;

            // Capture the KNQ/P2 string.
            pszValue = strstr(pszKNQ, "P2=");
            if (pszValue)
                pszEnd = strstr(pszValue, ",");
            if (pszValue && pszEnd)
                osP2.assign(pszValue + 3, pszEnd - pszValue - 3);

            // Capture the KNQ/P3 string.
            pszValue = strstr(pszKNQ, "P3=");
            if (pszValue)
                pszEnd = strstr(pszValue, ",");
            if (pszValue)
            {
                if (pszEnd)
                    osP3.assign(pszValue + 3, pszEnd - pszValue - 3);
                else
                    osP3.assign(pszValue + 3);
            }

            if (!osP2.empty() && !osP3.empty())
                osUnderlyingSRS.Printf(
                    "PROJCS[\"unnamed\",%s,PROJECTION[\"Lambert_Conformal_"
                    "Conic_2SP\"],PARAMETER[\"standard_parallel_1\",%s],"
                    "PARAMETER[\"standard_parallel_2\",%s],PARAMETER["
                    "\"latitude_of_origin\",0.0],PARAMETER[\"central_"
                    "meridian\",%s],PARAMETER[\"false_easting\",0.0],PARAMETER["
                    "\"false_northing\",0.0],UNIT[\"Meter\",1]]",
                    pszGEOGCS, osP2.c_str(), osP3.c_str(), osPP.c_str());
        }
    }

    /* -------------------------------------------------------------------- */
    /*      If we got an alternate underlying coordinate system, try        */
    /*      converting the GCPs to that coordinate system.                  */
    /* -------------------------------------------------------------------- */
    if (osUnderlyingSRS.length() > 0)
    {
        OGRSpatialReference oGeog_SRS, oProjected_SRS;

        oProjected_SRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        oProjected_SRS.SetFromUserInput(osUnderlyingSRS);
        oGeog_SRS.CopyGeogCSFrom(&oProjected_SRS);
        oGeog_SRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

        OGRCoordinateTransformation *poCT =
            OGRCreateCoordinateTransformation(&oGeog_SRS, &oProjected_SRS);
        if (poCT != nullptr)
        {
            for (int i = 0; i < nGCPCount; i++)
            {
                poCT->Transform(1, &(pasGCPList[i].dfGCPX),
                                &(pasGCPList[i].dfGCPY),
                                &(pasGCPList[i].dfGCPZ));
            }

            m_oGCPSRS.importFromWkt(osUnderlyingSRS.c_str());

            delete poCT;
        }
        else
            CPLErrorReset();
    }

    /* -------------------------------------------------------------------- */
    /*      Attempt to prepare a geotransform from the GCPs.                */
    /* -------------------------------------------------------------------- */
    if (GDALGCPsToGeoTransform(nGCPCount, pasGCPList, m_gt.data(), FALSE))
    {
        bGeoTransformSet = TRUE;
    }
}

/************************************************************************/
/*                           ScanForGCPsNos()                           */
/*                                                                      */
/*      Nos files have an accompanying .geo file, that contains some    */
/*      of the information normally contained in the header section     */
/*      with BSB files. we try and open a file with the same name,      */
/*      but a .geo extension, and look for lines like...                */
/*      PointX=long lat line pixel    (using the same naming system     */
/*      as BSB) Point1=-22.0000 64.250000 197 744                       */
/************************************************************************/

void BSBDataset::ScanForGCPsNos(const char *pszFilename)
{
    const std::string extension = CPLGetExtensionSafe(pszFilename);

    // pseudointelligently try and guess whether we want a .geo or a .GEO
    std::string geofile;
    if (extension.size() >= 2 && extension[1] == 'O')
    {
        geofile = CPLResetExtensionSafe(pszFilename, "GEO");
    }
    else
    {
        geofile = CPLResetExtensionSafe(pszFilename, "geo");
    }

    FILE *gfp = VSIFOpen(geofile.c_str(), "r");  // Text files
    if (gfp == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Couldn't find a matching .GEO file: %s", geofile.c_str());
        return;
    }

    char *thisLine = (char *)CPLMalloc(80);  // FIXME

    // Count the GCPs (reference points) and seek the file pointer 'gfp' to the
    // starting point
    int fileGCPCount = 0;
    while (fgets(thisLine, 80, gfp))
    {
        if (STARTS_WITH_CI(thisLine, "Point"))
            fileGCPCount++;
    }
    VSIRewind(gfp);

    // Memory has not been allocated to fileGCPCount yet
    pasGCPList = (GDAL_GCP *)CPLCalloc(sizeof(GDAL_GCP), fileGCPCount + 1);

    while (fgets(thisLine, 80, gfp))
    {
        if (STARTS_WITH_CI(thisLine, "Point"))
        {
            // got a point line, turn it into a gcp
            char **Tokens =
                CSLTokenizeStringComplex(thisLine, "= ", FALSE, FALSE);
            if (CSLCount(Tokens) >= 5)
            {
                GDALInitGCPs(1, pasGCPList + nGCPCount);
                pasGCPList[nGCPCount].dfGCPX = CPLAtof(Tokens[1]);
                pasGCPList[nGCPCount].dfGCPY = CPLAtof(Tokens[2]);
                pasGCPList[nGCPCount].dfGCPPixel = CPLAtof(Tokens[4]);
                pasGCPList[nGCPCount].dfGCPLine = CPLAtof(Tokens[3]);

                CPLFree(pasGCPList[nGCPCount].pszId);
                char szName[50];
                snprintf(szName, sizeof(szName), "GCP_%d", nGCPCount + 1);
                pasGCPList[nGCPCount].pszId = CPLStrdup(szName);

                nGCPCount++;
            }
            CSLDestroy(Tokens);
        }
    }

    CPLFree(thisLine);
    VSIFClose(gfp);
}

/************************************************************************/
/*                            ScanForGCPsBSB()                          */
/************************************************************************/

void BSBDataset::ScanForGCPsBSB()
{
    /* -------------------------------------------------------------------- */
    /*      Collect standalone GCPs.  They look like:                       */
    /*                                                                      */
    /*      REF/1,115,2727,32.346666666667,-60.881666666667                 */
    /*      REF/n,pixel,line,lat,long                                       */
    /* -------------------------------------------------------------------- */
    int fileGCPCount = 0;

    // Count the GCPs (reference points) in psInfo->papszHeader
    for (int i = 0; psInfo->papszHeader[i] != nullptr; i++)
        if (STARTS_WITH_CI(psInfo->papszHeader[i], "REF/"))
            fileGCPCount++;

    // Memory has not been allocated to fileGCPCount yet
    pasGCPList = (GDAL_GCP *)CPLCalloc(sizeof(GDAL_GCP), fileGCPCount + 1);

    for (int i = 0; psInfo->papszHeader[i] != nullptr; i++)
    {

        if (!STARTS_WITH_CI(psInfo->papszHeader[i], "REF/"))
            continue;

        char **papszTokens = CSLTokenizeStringComplex(
            psInfo->papszHeader[i] + 4, ",", FALSE, FALSE);

        if (CSLCount(papszTokens) > 4)
        {
            GDALInitGCPs(1, pasGCPList + nGCPCount);

            pasGCPList[nGCPCount].dfGCPX = CPLAtof(papszTokens[4]);
            pasGCPList[nGCPCount].dfGCPY = CPLAtof(papszTokens[3]);
            pasGCPList[nGCPCount].dfGCPPixel = CPLAtof(papszTokens[1]);
            pasGCPList[nGCPCount].dfGCPLine = CPLAtof(papszTokens[2]);

            CPLFree(pasGCPList[nGCPCount].pszId);
            if (CSLCount(papszTokens) > 5)
            {
                pasGCPList[nGCPCount].pszId = CPLStrdup(papszTokens[5]);
            }
            else
            {
                char szName[50];
                snprintf(szName, sizeof(szName), "GCP_%d", nGCPCount + 1);
                pasGCPList[nGCPCount].pszId = CPLStrdup(szName);
            }

            nGCPCount++;
        }
        CSLDestroy(papszTokens);
    }
}

/************************************************************************/
/*                            ScanForCutline()                          */
/************************************************************************/

void BSBDataset::ScanForCutline()
{
    /* PLY: Border Polygon Record - coordinates of the panel within the
     * raster image, given in chart datum lat/long. Any shape polygon.
     * They look like:
     *      PLY/1,32.346666666667,-60.881666666667
     *      PLY/n,lat,long
     *
     * If found then we return it via a BSB_CUTLINE metadata item as a WKT
     * POLYGON.
     */

    std::string wkt;
    for (int i = 0; psInfo->papszHeader[i] != nullptr; i++)
    {
        if (!STARTS_WITH_CI(psInfo->papszHeader[i], "PLY/"))
            continue;

        const CPLStringList aosTokens(
            CSLTokenizeString2(psInfo->papszHeader[i] + 4, ",", 0));

        if (aosTokens.size() >= 3)
        {
            if (wkt.empty())
                wkt = "POLYGON ((";
            else
                wkt += ',';
            wkt += aosTokens[2];
            wkt += ' ';
            wkt += aosTokens[1];
        }
    }

    if (!wkt.empty())
    {
        wkt += "))";
        SetMetadataItem("BSB_CUTLINE", wkt.c_str());
    }
}

/************************************************************************/
/*                          IdentifyInternal()                          */
/************************************************************************/

int BSBDataset::IdentifyInternal(GDALOpenInfo *poOpenInfo, bool &isNosOut)

{
    /* -------------------------------------------------------------------- */
    /*      Check for BSB/ keyword.                                         */
    /* -------------------------------------------------------------------- */
    isNosOut = false;

    if (poOpenInfo->nHeaderBytes < 1000)
        return FALSE;

    int i = 0;
    for (; i < poOpenInfo->nHeaderBytes - 4; i++)
    {
        if (poOpenInfo->pabyHeader[i + 0] == 'B' &&
            poOpenInfo->pabyHeader[i + 1] == 'S' &&
            poOpenInfo->pabyHeader[i + 2] == 'B' &&
            poOpenInfo->pabyHeader[i + 3] == '/')
            break;
        if (poOpenInfo->pabyHeader[i + 0] == 'N' &&
            poOpenInfo->pabyHeader[i + 1] == 'O' &&
            poOpenInfo->pabyHeader[i + 2] == 'S' &&
            poOpenInfo->pabyHeader[i + 3] == '/')
        {
            isNosOut = true;
            break;
        }
        if (poOpenInfo->pabyHeader[i + 0] == 'W' &&
            poOpenInfo->pabyHeader[i + 1] == 'X' &&
            poOpenInfo->pabyHeader[i + 2] == '\\' &&
            poOpenInfo->pabyHeader[i + 3] == '8')
            break;
    }

    if (i == poOpenInfo->nHeaderBytes - 4)
        return FALSE;

    /* Additional test to avoid false positive. See #2881 */
    const char *pszHeader =
        reinterpret_cast<const char *>(poOpenInfo->pabyHeader);
    const char *pszShiftedHeader = pszHeader + i;
    const char *pszRA = strstr(pszShiftedHeader, "RA=");
    if (pszRA == nullptr) /* This may be a NO1 file */
        pszRA = strstr(pszShiftedHeader, "[JF");
    if (pszRA == nullptr)
        return FALSE;
    if (pszRA - pszShiftedHeader > 100 && !strstr(pszHeader, "VER/") &&
        !strstr(pszHeader, "KNP/") && !strstr(pszHeader, "KNQ/") &&
        !strstr(pszHeader, "RGB/"))
        return FALSE;

    return TRUE;
}

/************************************************************************/
/*                              Identify()                              */
/************************************************************************/

int BSBDataset::Identify(GDALOpenInfo *poOpenInfo)

{
    bool isNos;
    return IdentifyInternal(poOpenInfo, isNos);
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *BSBDataset::Open(GDALOpenInfo *poOpenInfo)

{
    bool isNos = false;
    if (!IdentifyInternal(poOpenInfo, isNos))
        return nullptr;

    if (poOpenInfo->eAccess == GA_Update)
    {
        ReportUpdateNotSupportedByDriver("BSB");
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Create a corresponding GDALDataset.                             */
    /* -------------------------------------------------------------------- */
    BSBDataset *poDS = new BSBDataset();

    /* -------------------------------------------------------------------- */
    /*      Open the file.                                                  */
    /* -------------------------------------------------------------------- */
    poDS->psInfo = BSBOpen(poOpenInfo->pszFilename);
    if (poDS->psInfo == nullptr)
    {
        delete poDS;
        return nullptr;
    }

    poDS->nRasterXSize = poDS->psInfo->nXSize;
    poDS->nRasterYSize = poDS->psInfo->nYSize;

    /* -------------------------------------------------------------------- */
    /*      Create band information objects.                                */
    /* -------------------------------------------------------------------- */
    poDS->SetBand(1, new BSBRasterBand(poDS));

    poDS->ScanForGCPs(isNos, poOpenInfo->pszFilename);

    /* -------------------------------------------------------------------- */
    /*      Set CUTLINE metadata if a bounding polygon is available         */
    /* -------------------------------------------------------------------- */
    poDS->ScanForCutline();

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    poDS->SetDescription(poOpenInfo->pszFilename);
    poDS->TryLoadXML();

    poDS->oOvManager.Initialize(poDS, poOpenInfo->pszFilename);

    return poDS;
}

/************************************************************************/
/*                            GetGCPCount()                             */
/************************************************************************/

int BSBDataset::GetGCPCount()

{
    return nGCPCount;
}

/************************************************************************/
/*                          GetGCPSpatialRef()                          */
/************************************************************************/

const OGRSpatialReference *BSBDataset::GetGCPSpatialRef() const

{
    return m_oGCPSRS.IsEmpty() ? nullptr : &m_oGCPSRS;
}

/************************************************************************/
/*                               GetGCP()                               */
/************************************************************************/

const GDAL_GCP *BSBDataset::GetGCPs()

{
    return pasGCPList;
}

#ifdef BSB_CREATE

/************************************************************************/
/*                             BSBIsSRSOK()                             */
/************************************************************************/

static int BSBIsSRSOK(const char *pszWKT)
{
    bool bOK = false;
    OGRSpatialReference oSRS, oSRS_WGS84, oSRS_NAD83;

    if (pszWKT != NULL && pszWKT[0] != '\0')
    {
        oSRS.importFromWkt(pszWKT);

        oSRS_WGS84.SetWellKnownGeogCS("WGS84");
        oSRS_NAD83.SetWellKnownGeogCS("NAD83");
        if ((oSRS.IsSameGeogCS(&oSRS_WGS84) ||
             oSRS.IsSameGeogCS(&oSRS_NAD83)) &&
            oSRS.IsGeographic() && oSRS.GetPrimeMeridian() == 0.0)
        {
            bOK = true;
        }
    }

    if (!bOK)
    {
        CPLError(CE_Warning, CPLE_NotSupported,
                 "BSB only supports WGS84 or NAD83 geographic projections.\n");
    }

    return bOK;
}

/************************************************************************/
/*                           BSBCreateCopy()                            */
/************************************************************************/

static GDALDataset *BSBCreateCopy(const char *pszFilename, GDALDataset *poSrcDS,
                                  int bStrict, char ** /*papszOptions*/,
                                  GDALProgressFunc /*pfnProgress*/,
                                  void * /*pProgressData*/)

{
    /* -------------------------------------------------------------------- */
    /*      Some some rudimentary checks                                    */
    /* -------------------------------------------------------------------- */
    const int nBands = poSrcDS->GetRasterCount();
    if (nBands != 1)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "BSB driver only supports one band images.\n");

        return NULL;
    }

    if (poSrcDS->GetRasterBand(1)->GetRasterDataType() != GDT_Byte && bStrict)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "BSB driver doesn't support data type %s. "
                 "Only eight bit bands supported.\n",
                 GDALGetDataTypeName(
                     poSrcDS->GetRasterBand(1)->GetRasterDataType()));

        return NULL;
    }

    const int nXSize = poSrcDS->GetRasterXSize();
    const int nYSize = poSrcDS->GetRasterYSize();

    /* -------------------------------------------------------------------- */
    /*      Open the output file.                                           */
    /* -------------------------------------------------------------------- */
    BSBInfo *psBSB = BSBCreate(pszFilename, 0, 200, nXSize, nYSize);
    if (psBSB == NULL)
        return NULL;

    /* -------------------------------------------------------------------- */
    /*      Prepare initial color table.colortable.                         */
    /* -------------------------------------------------------------------- */
    GDALRasterBand *poBand = poSrcDS->GetRasterBand(1);
    unsigned char abyPCT[771];
    int nPCTSize;
    int anRemap[256];

    abyPCT[0] = 0;
    abyPCT[1] = 0;
    abyPCT[2] = 0;

    if (poBand->GetColorTable() == NULL)
    {
        /* map greyscale down to 63 grey levels. */
        for (int iColor = 0; iColor < 256; iColor++)
        {
            int nOutValue = (int)(iColor / 4.1) + 1;

            anRemap[iColor] = nOutValue;
            abyPCT[nOutValue * 3 + 0] = (unsigned char)iColor;
            abyPCT[nOutValue * 3 + 1] = (unsigned char)iColor;
            abyPCT[nOutValue * 3 + 2] = (unsigned char)iColor;
        }
        nPCTSize = 64;
    }
    else
    {
        GDALColorTable *poCT = poBand->GetColorTable();
        int nColorTableSize = poCT->GetColorEntryCount();
        if (nColorTableSize > 255)
            nColorTableSize = 255;

        for (int iColor = 0; iColor < nColorTableSize; iColor++)
        {
            GDALColorEntry sEntry;

            poCT->GetColorEntryAsRGB(iColor, &sEntry);

            anRemap[iColor] = iColor + 1;
            abyPCT[(iColor + 1) * 3 + 0] = (unsigned char)sEntry.c1;
            abyPCT[(iColor + 1) * 3 + 1] = (unsigned char)sEntry.c2;
            abyPCT[(iColor + 1) * 3 + 2] = (unsigned char)sEntry.c3;
        }

        nPCTSize = nColorTableSize + 1;

        // Add entries for pixel values which apparently will not occur.
        for (int iColor = nPCTSize; iColor < 256; iColor++)
            anRemap[iColor] = 1;
    }

    /* -------------------------------------------------------------------- */
    /*      Boil out all duplicate entries.                                 */
    /* -------------------------------------------------------------------- */
    for (int i = 1; i < nPCTSize - 1; i++)
    {
        for (int j = i + 1; j < nPCTSize; j++)
        {
            if (abyPCT[i * 3 + 0] == abyPCT[j * 3 + 0] &&
                abyPCT[i * 3 + 1] == abyPCT[j * 3 + 1] &&
                abyPCT[i * 3 + 2] == abyPCT[j * 3 + 2])
            {
                nPCTSize--;
                abyPCT[j * 3 + 0] = abyPCT[nPCTSize * 3 + 0];
                abyPCT[j * 3 + 1] = abyPCT[nPCTSize * 3 + 1];
                abyPCT[j * 3 + 2] = abyPCT[nPCTSize * 3 + 2];

                for (int k = 0; k < 256; k++)
                {
                    // merge matching entries.
                    if (anRemap[k] == j)
                        anRemap[k] = i;

                    // shift the last PCT entry into the new hole.
                    if (anRemap[k] == nPCTSize)
                        anRemap[k] = j;
                }
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Boil out all duplicate entries.                                 */
    /* -------------------------------------------------------------------- */
    if (nPCTSize > 128)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Having to merge color table entries to reduce %d real\n"
                 "color table entries down to 127 values.",
                 nPCTSize);
    }

    while (nPCTSize > 128)
    {
        int nBestRange = 768;
        int iBestMatch1 = -1;
        int iBestMatch2 = -1;

        // Find the closest pair of color table entries.

        for (int i = 1; i < nPCTSize - 1; i++)
        {
            for (int j = i + 1; j < nPCTSize; j++)
            {
                int nRange = std::abs(abyPCT[i * 3 + 0] - abyPCT[j * 3 + 0]) +
                             std::abs(abyPCT[i * 3 + 1] - abyPCT[j * 3 + 1]) +
                             std::abs(abyPCT[i * 3 + 2] - abyPCT[j * 3 + 2]);

                if (nRange < nBestRange)
                {
                    iBestMatch1 = i;
                    iBestMatch2 = j;
                    nBestRange = nRange;
                }
            }
        }

        // Merge the second entry into the first.
        nPCTSize--;
        abyPCT[iBestMatch2 * 3 + 0] = abyPCT[nPCTSize * 3 + 0];
        abyPCT[iBestMatch2 * 3 + 1] = abyPCT[nPCTSize * 3 + 1];
        abyPCT[iBestMatch2 * 3 + 2] = abyPCT[nPCTSize * 3 + 2];

        for (int i = 0; i < 256; i++)
        {
            // merge matching entries.
            if (anRemap[i] == iBestMatch2)
                anRemap[i] = iBestMatch1;

            // shift the last PCT entry into the new hole.
            if (anRemap[i] == nPCTSize)
                anRemap[i] = iBestMatch2;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Write the PCT.                                                  */
    /* -------------------------------------------------------------------- */
    if (!BSBWritePCT(psBSB, nPCTSize, abyPCT))
    {
        BSBClose(psBSB);
        return NULL;
    }

    /* -------------------------------------------------------------------- */
    /*      Write the GCPs.                                                 */
    /* -------------------------------------------------------------------- */
    GDALGeoTransform gt;
    int nGCPCount = poSrcDS->GetGCPCount();
    if (nGCPCount)
    {
        const char *pszGCPProjection = poSrcDS->GetGCPProjection();
        if (BSBIsSRSOK(pszGCPProjection))
        {
            const GDAL_GCP *pasGCPList = poSrcDS->GetGCPs();
            for (int i = 0; i < nGCPCount; i++)
            {
                VSIFPrintfL(psBSB->fp, "REF/%d,%f,%f,%f,%f\n", i + 1,
                            pasGCPList[i].dfGCPPixel, pasGCPList[i].dfGCPLine,
                            pasGCPList[i].dfGCPY, pasGCPList[i].dfGCPX);
            }
        }
    }
    else if (poSrcDS->GetGeoTransform(gt) == CE_None)
    {
        const char *pszProjection = poSrcDS->GetProjectionRef();
        if (BSBIsSRSOK(pszProjection))
        {
            VSIFPrintfL(psBSB->fp, "REF/%d,%d,%d,%f,%f\n", 1, 0, 0,
                        gt[3] + 0 * gt[4] + 0 * gt[5],
                        gt[0] + 0 * gt[1] + 0 * gt[2]);
            VSIFPrintfL(psBSB->fp, "REF/%d,%d,%d,%f,%f\n", 2, nXSize, 0,
                        gt[3] + nXSize * gt[4] + 0 * gt[5],
                        gt[0] + nXSize * gt[1] + 0 * gt[2]);
            VSIFPrintfL(psBSB->fp, "REF/%d,%d,%d,%f,%f\n", 3, nXSize, nYSize,
                        gt[3] + nXSize * gt[4] + nYSize * gt[5],
                        gt[0] + nXSize * gt[1] + nYSize * gt[2]);
            VSIFPrintfL(psBSB->fp, "REF/%d,%d,%d,%f,%f\n", 4, 0, nYSize,
                        gt[3] + 0 * gt[4] + nYSize * gt[5],
                        gt[0] + 0 * gt[1] + nYSize * gt[2]);
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Loop over image, copying image data.                            */
    /* -------------------------------------------------------------------- */
    CPLErr eErr = CE_None;

    GByte *pabyScanline = (GByte *)CPLMalloc(nXSize);

    for (int iLine = 0; iLine < nYSize && eErr == CE_None; iLine++)
    {
        eErr =
            poBand->RasterIO(GF_Read, 0, iLine, nXSize, 1, pabyScanline, nXSize,
                             1, GDT_Byte, nBands, nBands * nXSize, NULL);
        if (eErr == CE_None)
        {
            for (int i = 0; i < nXSize; i++)
                pabyScanline[i] = (GByte)anRemap[pabyScanline[i]];

            if (!BSBWriteScanline(psBSB, pabyScanline))
                eErr = CE_Failure;
        }
    }

    CPLFree(pabyScanline);

    /* -------------------------------------------------------------------- */
    /*      cleanup                                                         */
    /* -------------------------------------------------------------------- */
    BSBClose(psBSB);

    if (eErr != CE_None)
    {
        VSIUnlink(pszFilename);
        return NULL;
    }

    return (GDALDataset *)GDALOpen(pszFilename, GA_ReadOnly);
}
#endif

/************************************************************************/
/*                        GDALRegister_BSB()                            */
/************************************************************************/

void GDALRegister_BSB()

{
    if (GDALGetDriverByName("BSB") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("BSB");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "Maptech BSB Nautical Charts");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/raster/bsb.html");
    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_EXTENSION, "kap");
#ifdef BSB_CREATE
    poDriver->SetMetadataItem(GDAL_DMD_CREATIONDATATYPES, "Byte");
#endif
    poDriver->pfnOpen = BSBDataset::Open;
    poDriver->pfnIdentify = BSBDataset::Identify;
#ifdef BSB_CREATE
    poDriver->pfnCreateCopy = BSBCreateCopy;
#endif

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
