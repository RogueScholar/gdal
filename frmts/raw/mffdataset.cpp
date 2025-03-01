/******************************************************************************
 *
 * Project:  GView
 * Purpose:  Implementation of Atlantis MFF Support
 * Author:   Frank Warmerdam, warmerda@home.com
 *
 ******************************************************************************
 * Copyright (c) 2000, Frank Warmerdam
 * Copyright (c) 2008-2012, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "atlsci_spheroid.h"
#include "cpl_string.h"
#include "gdal_frmts.h"
#include "ogr_spatialref.h"
#include "rawdataset.h"

#include <cctype>
#include <cmath>
#include <algorithm>

enum
{
    MFFPRJ_NONE,
    MFFPRJ_LL,
    MFFPRJ_UTM,
    MFFPRJ_UNRECOGNIZED
};

static int GetMFFProjectionType(const OGRSpatialReference *poSRS);

/************************************************************************/
/* ==================================================================== */
/*                              MFFDataset                              */
/* ==================================================================== */
/************************************************************************/

class MFFDataset final : public RawDataset
{
    int nGCPCount;
    GDAL_GCP *pasGCPList;

    OGRSpatialReference m_oSRS{};
    OGRSpatialReference m_oGCPSRS{};
    double adfGeoTransform[6];
    char **m_papszFileList;

    void ScanForGCPs();
    void ScanForProjectionInfo();

    CPL_DISALLOW_COPY_ASSIGN(MFFDataset)

    CPLErr Close() override;

  public:
    MFFDataset();
    ~MFFDataset() override;

    char **papszHdrLines;

    VSILFILE **pafpBandFiles;

    char **GetFileList() override;

    int GetGCPCount() override;

    const OGRSpatialReference *GetGCPSpatialRef() const override
    {
        return m_oGCPSRS.IsEmpty() ? nullptr : &m_oGCPSRS;
    }

    const GDAL_GCP *GetGCPs() override;

    const OGRSpatialReference *GetSpatialRef() const override
    {
        return m_oSRS.IsEmpty() ? nullptr : &m_oSRS;
    }

    CPLErr GetGeoTransform(double *) override;

    static GDALDataset *Open(GDALOpenInfo *);
    static GDALDataset *Create(const char *pszFilename, int nXSize, int nYSize,
                               int nBandsIn, GDALDataType eType,
                               char **papszParamList);
    static GDALDataset *CreateCopy(const char *pszFilename,
                                   GDALDataset *poSrcDS, int bStrict,
                                   char **papszOptions,
                                   GDALProgressFunc pfnProgress,
                                   void *pProgressData);
};

/************************************************************************/
/* ==================================================================== */
/*                            MFFTiledBand                              */
/* ==================================================================== */
/************************************************************************/

class MFFTiledBand final : public GDALRasterBand
{
    friend class MFFDataset;

    VSILFILE *fpRaw;
    RawRasterBand::ByteOrder eByteOrder;

    CPL_DISALLOW_COPY_ASSIGN(MFFTiledBand)

  public:
    MFFTiledBand(MFFDataset *, int, VSILFILE *, int, int, GDALDataType,
                 RawRasterBand::ByteOrder);
    ~MFFTiledBand() override;

    CPLErr IReadBlock(int, int, void *) override;
};

/************************************************************************/
/*                            MFFTiledBand()                            */
/************************************************************************/

MFFTiledBand::MFFTiledBand(MFFDataset *poDSIn, int nBandIn, VSILFILE *fp,
                           int nTileXSize, int nTileYSize,
                           GDALDataType eDataTypeIn,
                           RawRasterBand::ByteOrder eByteOrderIn)
    : fpRaw(fp), eByteOrder(eByteOrderIn)
{
    poDS = poDSIn;
    nBand = nBandIn;

    eDataType = eDataTypeIn;

    nBlockXSize = nTileXSize;
    nBlockYSize = nTileYSize;
}

/************************************************************************/
/*                           ~MFFTiledBand()                            */
/************************************************************************/

MFFTiledBand::~MFFTiledBand()

{
    if (VSIFCloseL(fpRaw) != 0)
    {
        CPLError(CE_Failure, CPLE_FileIO, "I/O error");
    }
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr MFFTiledBand::IReadBlock(int nBlockXOff, int nBlockYOff, void *pImage)

{
    const int nTilesPerRow = (nRasterXSize + nBlockXSize - 1) / nBlockXSize;
    const int nWordSize = GDALGetDataTypeSize(eDataType) / 8;
    const int nBlockSize = nWordSize * nBlockXSize * nBlockYSize;

    const vsi_l_offset nOffset =
        nBlockSize *
        (nBlockXOff + static_cast<vsi_l_offset>(nBlockYOff) * nTilesPerRow);

    if (VSIFSeekL(fpRaw, nOffset, SEEK_SET) == -1 ||
        VSIFReadL(pImage, 1, nBlockSize, fpRaw) < 1)
    {
        CPLError(CE_Failure, CPLE_FileIO,
                 "Read of tile %d/%d failed with fseek or fread error.",
                 nBlockXOff, nBlockYOff);
        return CE_Failure;
    }

    if (eByteOrder != RawRasterBand::NATIVE_BYTE_ORDER && nWordSize > 1)
    {
        if (GDALDataTypeIsComplex(eDataType))
        {
            GDALSwapWords(pImage, nWordSize / 2, nBlockXSize * nBlockYSize,
                          nWordSize);
            GDALSwapWords(reinterpret_cast<GByte *>(pImage) + nWordSize / 2,
                          nWordSize / 2, nBlockXSize * nBlockYSize, nWordSize);
        }
        else
            GDALSwapWords(pImage, nWordSize, nBlockXSize * nBlockYSize,
                          nWordSize);
    }

    return CE_None;
}

/************************************************************************/
/*                      MFF Spheroids                                   */
/************************************************************************/

class MFFSpheroidList : public SpheroidList
{
  public:
    MFFSpheroidList();

    ~MFFSpheroidList()
    {
    }
};

MFFSpheroidList ::MFFSpheroidList()
{
    num_spheroids = 18;

    epsilonR = 0.1;
    epsilonI = 0.000001;

    spheroids[0].SetValuesByRadii("SPHERE", 6371007.0, 6371007.0);
    spheroids[1].SetValuesByRadii("EVEREST", 6377304.0, 6356103.0);
    spheroids[2].SetValuesByRadii("BESSEL", 6377397.0, 6356082.0);
    spheroids[3].SetValuesByRadii("AIRY", 6377563.0, 6356300.0);
    spheroids[4].SetValuesByRadii("CLARKE_1858", 6378294.0, 6356621.0);
    spheroids[5].SetValuesByRadii("CLARKE_1866", 6378206.4, 6356583.8);
    spheroids[6].SetValuesByRadii("CLARKE_1880", 6378249.0, 6356517.0);
    spheroids[7].SetValuesByRadii("HAYFORD", 6378388.0, 6356915.0);
    spheroids[8].SetValuesByRadii("KRASOVSKI", 6378245.0, 6356863.0);
    spheroids[9].SetValuesByRadii("HOUGH", 6378270.0, 6356794.0);
    spheroids[10].SetValuesByRadii("FISHER_60", 6378166.0, 6356784.0);
    spheroids[11].SetValuesByRadii("KAULA", 6378165.0, 6356345.0);
    spheroids[12].SetValuesByRadii("IUGG_67", 6378160.0, 6356775.0);
    spheroids[13].SetValuesByRadii("FISHER_68", 6378150.0, 6356330.0);
    spheroids[14].SetValuesByRadii("WGS_72", 6378135.0, 6356751.0);
    spheroids[15].SetValuesByRadii("IUGG_75", 6378140.0, 6356755.0);
    spheroids[16].SetValuesByRadii("WGS_84", 6378137.0, 6356752.0);
    spheroids[17].SetValuesByRadii("HUGHES", 6378273.0, 6356889.4);
}

/************************************************************************/
/* ==================================================================== */
/*                              MFFDataset                              */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                            MFFDataset()                             */
/************************************************************************/

MFFDataset::MFFDataset()
    : nGCPCount(0), pasGCPList(nullptr), m_papszFileList(nullptr),
      papszHdrLines(nullptr), pafpBandFiles(nullptr)
{
    m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    m_oGCPSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    adfGeoTransform[0] = 0.0;
    adfGeoTransform[1] = 1.0;
    adfGeoTransform[2] = 0.0;
    adfGeoTransform[3] = 0.0;
    adfGeoTransform[4] = 0.0;
    adfGeoTransform[5] = 1.0;
}

/************************************************************************/
/*                            ~MFFDataset()                            */
/************************************************************************/

MFFDataset::~MFFDataset()

{
    MFFDataset::Close();
}

/************************************************************************/
/*                              Close()                                 */
/************************************************************************/

CPLErr MFFDataset::Close()
{
    CPLErr eErr = CE_None;
    if (nOpenFlags != OPEN_FLAGS_CLOSED)
    {
        if (MFFDataset::FlushCache(true) != CE_None)
            eErr = CE_Failure;

        CSLDestroy(papszHdrLines);
        if (pafpBandFiles)
        {
            for (int i = 0; i < GetRasterCount(); i++)
            {
                if (pafpBandFiles[i])
                {
                    if (VSIFCloseL(pafpBandFiles[i]) != 0)
                    {
                        eErr = CE_Failure;
                        CPLError(CE_Failure, CPLE_FileIO, "I/O error");
                    }
                }
            }
            CPLFree(pafpBandFiles);
        }

        if (nGCPCount > 0)
        {
            GDALDeinitGCPs(nGCPCount, pasGCPList);
        }
        CPLFree(pasGCPList);
        CSLDestroy(m_papszFileList);

        if (GDALPamDataset::Close() != CE_None)
            eErr = CE_Failure;
    }
    return eErr;
}

/************************************************************************/
/*                            GetFileList()                             */
/************************************************************************/

char **MFFDataset::GetFileList()
{
    char **papszFileList = RawDataset::GetFileList();
    papszFileList = CSLInsertStrings(papszFileList, -1, m_papszFileList);
    return papszFileList;
}

/************************************************************************/
/*                            GetGCPCount()                             */
/************************************************************************/

int MFFDataset::GetGCPCount()

{
    return nGCPCount;
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr MFFDataset::GetGeoTransform(double *padfTransform)

{
    memcpy(padfTransform, adfGeoTransform, sizeof(double) * 6);
    return CE_None;
}

/************************************************************************/
/*                               GetGCP()                               */
/************************************************************************/

const GDAL_GCP *MFFDataset::GetGCPs()

{
    return pasGCPList;
}

/************************************************************************/
/*                            ScanForGCPs()                             */
/************************************************************************/

void MFFDataset::ScanForGCPs()

{
    int NUM_GCPS = 0;

    if (CSLFetchNameValue(papszHdrLines, "NUM_GCPS") != nullptr)
        NUM_GCPS = atoi(CSLFetchNameValue(papszHdrLines, "NUM_GCPS"));
    if (NUM_GCPS < 0)
        return;

    nGCPCount = 0;
    pasGCPList =
        static_cast<GDAL_GCP *>(VSICalloc(sizeof(GDAL_GCP), 5 + NUM_GCPS));
    if (pasGCPList == nullptr)
        return;

    for (int nCorner = 0; nCorner < 5; nCorner++)
    {
        const char *pszBase = nullptr;
        double dfRasterX = 0.0;
        double dfRasterY = 0.0;

        if (nCorner == 0)
        {
            dfRasterX = 0.5;
            dfRasterY = 0.5;
            pszBase = "TOP_LEFT_CORNER";
        }
        else if (nCorner == 1)
        {
            dfRasterX = GetRasterXSize() - 0.5;
            dfRasterY = 0.5;
            pszBase = "TOP_RIGHT_CORNER";
        }
        else if (nCorner == 2)
        {
            dfRasterX = GetRasterXSize() - 0.5;
            dfRasterY = GetRasterYSize() - 0.5;
            pszBase = "BOTTOM_RIGHT_CORNER";
        }
        else if (nCorner == 3)
        {
            dfRasterX = 0.5;
            dfRasterY = GetRasterYSize() - 0.5;
            pszBase = "BOTTOM_LEFT_CORNER";
        }
        else /* if( nCorner == 4 ) */
        {
            dfRasterX = GetRasterXSize() / 2.0;
            dfRasterY = GetRasterYSize() / 2.0;
            pszBase = "CENTRE";
        }

        char szLatName[40] = {'\0'};
        char szLongName[40] = {'\0'};
        snprintf(szLatName, sizeof(szLatName), "%s_LATITUDE", pszBase);
        snprintf(szLongName, sizeof(szLongName), "%s_LONGITUDE", pszBase);

        if (CSLFetchNameValue(papszHdrLines, szLatName) != nullptr &&
            CSLFetchNameValue(papszHdrLines, szLongName) != nullptr)
        {
            GDALInitGCPs(1, pasGCPList + nGCPCount);

            CPLFree(pasGCPList[nGCPCount].pszId);

            pasGCPList[nGCPCount].pszId = CPLStrdup(pszBase);

            pasGCPList[nGCPCount].dfGCPX =
                CPLAtof(CSLFetchNameValue(papszHdrLines, szLongName));
            pasGCPList[nGCPCount].dfGCPY =
                CPLAtof(CSLFetchNameValue(papszHdrLines, szLatName));
            pasGCPList[nGCPCount].dfGCPZ = 0.0;

            pasGCPList[nGCPCount].dfGCPPixel = dfRasterX;
            pasGCPList[nGCPCount].dfGCPLine = dfRasterY;

            nGCPCount++;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Collect standalone GCPs.  They look like:                       */
    /*                                                                      */
    /*      GCPn = row, col, lat, long                                      */
    /*      GCP1 = 1, 1, 45.0, -75.0                                        */
    /* -------------------------------------------------------------------- */
    for (int i = 0; i < NUM_GCPS; i++)
    {
        char szName[25] = {'\0'};
        snprintf(szName, sizeof(szName), "GCP%d", i + 1);
        if (CSLFetchNameValue(papszHdrLines, szName) == nullptr)
            continue;

        char **papszTokens = CSLTokenizeStringComplex(
            CSLFetchNameValue(papszHdrLines, szName), ",", FALSE, FALSE);
        if (CSLCount(papszTokens) == 4)
        {
            GDALInitGCPs(1, pasGCPList + nGCPCount);

            CPLFree(pasGCPList[nGCPCount].pszId);
            pasGCPList[nGCPCount].pszId = CPLStrdup(szName);

            pasGCPList[nGCPCount].dfGCPX = CPLAtof(papszTokens[3]);
            pasGCPList[nGCPCount].dfGCPY = CPLAtof(papszTokens[2]);
            pasGCPList[nGCPCount].dfGCPZ = 0.0;
            pasGCPList[nGCPCount].dfGCPPixel = CPLAtof(papszTokens[1]) + 0.5;
            pasGCPList[nGCPCount].dfGCPLine = CPLAtof(papszTokens[0]) + 0.5;

            nGCPCount++;
        }

        CSLDestroy(papszTokens);
    }
}

/************************************************************************/
/*                        ScanForProjectionInfo                         */
/************************************************************************/

void MFFDataset::ScanForProjectionInfo()
{
    const char *pszProjName =
        CSLFetchNameValue(papszHdrLines, "PROJECTION_NAME");
    const char *pszOriginLong =
        CSLFetchNameValue(papszHdrLines, "PROJECTION_ORIGIN_LONGITUDE");
    const char *pszSpheroidName =
        CSLFetchNameValue(papszHdrLines, "SPHEROID_NAME");

    if (pszProjName == nullptr)
    {
        m_oSRS.Clear();
        m_oGCPSRS.Clear();
        return;
    }
    else if ((!EQUAL(pszProjName, "utm")) && (!EQUAL(pszProjName, "ll")))
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Only utm and lat/long projections are currently supported.");
        m_oSRS.Clear();
        m_oGCPSRS.Clear();
        return;
    }
    MFFSpheroidList *mffEllipsoids = new MFFSpheroidList;

    OGRSpatialReference oProj;
    oProj.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    if (EQUAL(pszProjName, "utm"))
    {
        int nZone;

        if (pszOriginLong == nullptr)
        {
            // If origin not specified, assume 0.0.
            CPLError(
                CE_Warning, CPLE_AppDefined,
                "No projection origin longitude specified.  Assuming 0.0.");
            nZone = 31;
        }
        else
            nZone = 31 + static_cast<int>(floor(CPLAtof(pszOriginLong) / 6.0));

        if (nGCPCount >= 5 && pasGCPList[4].dfGCPY < 0)
            oProj.SetUTM(nZone, 0);
        else
            oProj.SetUTM(nZone, 1);

        if (pszOriginLong != nullptr)
            oProj.SetProjParm(SRS_PP_CENTRAL_MERIDIAN, CPLAtof(pszOriginLong));
    }

    OGRSpatialReference oLL;
    oLL.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    if (pszOriginLong != nullptr)
        oLL.SetProjParm(SRS_PP_LONGITUDE_OF_ORIGIN, CPLAtof(pszOriginLong));

    if (pszSpheroidName == nullptr)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Unspecified ellipsoid.  Using wgs-84 parameters.\n");

        oProj.SetWellKnownGeogCS("WGS84");
        oLL.SetWellKnownGeogCS("WGS84");
    }
    else
    {
        if (mffEllipsoids->SpheroidInList(pszSpheroidName))
        {
            oProj.SetGeogCS(
                "unknown", "unknown", pszSpheroidName,
                mffEllipsoids->GetSpheroidEqRadius(pszSpheroidName),
                mffEllipsoids->GetSpheroidInverseFlattening(pszSpheroidName));
            oLL.SetGeogCS(
                "unknown", "unknown", pszSpheroidName,
                mffEllipsoids->GetSpheroidEqRadius(pszSpheroidName),
                mffEllipsoids->GetSpheroidInverseFlattening(pszSpheroidName));
        }
        else if (EQUAL(pszSpheroidName, "USER_DEFINED"))
        {
            const char *pszSpheroidEqRadius =
                CSLFetchNameValue(papszHdrLines, "SPHEROID_EQUATORIAL_RADIUS");
            const char *pszSpheroidPolarRadius =
                CSLFetchNameValue(papszHdrLines, "SPHEROID_POLAR_RADIUS");
            if ((pszSpheroidEqRadius != nullptr) &&
                (pszSpheroidPolarRadius != nullptr))
            {
                const double eq_radius = CPLAtof(pszSpheroidEqRadius);
                const double polar_radius = CPLAtof(pszSpheroidPolarRadius);
                oProj.SetGeogCS("unknown", "unknown", "unknown", eq_radius,
                                eq_radius / (eq_radius - polar_radius));
                oLL.SetGeogCS("unknown", "unknown", "unknown", eq_radius,
                              eq_radius / (eq_radius - polar_radius));
            }
            else
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Radii not specified for user-defined ellipsoid. "
                         "Using wgs-84 parameters.");
                oProj.SetWellKnownGeogCS("WGS84");
                oLL.SetWellKnownGeogCS("WGS84");
            }
        }
        else
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Unrecognized ellipsoid.  Using wgs-84 parameters.");
            oProj.SetWellKnownGeogCS("WGS84");
            oLL.SetWellKnownGeogCS("WGS84");
        }
    }

    /* If a geotransform is sufficient to represent the GCP's (i.e. each */
    /* estimated gcp is within 0.25*pixel size of the actual value- this */
    /* is the test applied by GDALGCPsToGeoTransform), store the         */
    /* geotransform.                                                     */
    bool transform_ok = false;

    if (EQUAL(pszProjName, "LL"))
    {
        transform_ok = CPL_TO_BOOL(
            GDALGCPsToGeoTransform(nGCPCount, pasGCPList, adfGeoTransform, 0));
    }
    else
    {
        OGRCoordinateTransformation *poTransform =
            OGRCreateCoordinateTransformation(&oLL, &oProj);
        bool bSuccess = true;
        if (poTransform == nullptr)
        {
            CPLErrorReset();
            bSuccess = FALSE;
        }

        double *dfPrjX =
            static_cast<double *>(CPLMalloc(nGCPCount * sizeof(double)));
        double *dfPrjY =
            static_cast<double *>(CPLMalloc(nGCPCount * sizeof(double)));

        for (int gcp_index = 0; gcp_index < nGCPCount; gcp_index++)
        {
            dfPrjX[gcp_index] = pasGCPList[gcp_index].dfGCPX;
            dfPrjY[gcp_index] = pasGCPList[gcp_index].dfGCPY;

            if (bSuccess && !poTransform->Transform(1, &(dfPrjX[gcp_index]),
                                                    &(dfPrjY[gcp_index])))
                bSuccess = FALSE;
        }

        if (bSuccess)
        {
            for (int gcp_index = 0; gcp_index < nGCPCount; gcp_index++)
            {
                pasGCPList[gcp_index].dfGCPX = dfPrjX[gcp_index];
                pasGCPList[gcp_index].dfGCPY = dfPrjY[gcp_index];
            }
            transform_ok = CPL_TO_BOOL(GDALGCPsToGeoTransform(
                nGCPCount, pasGCPList, adfGeoTransform, 0));
        }

        if (poTransform)
            delete poTransform;

        CPLFree(dfPrjX);
        CPLFree(dfPrjY);
    }

    m_oSRS = oProj;
    m_oGCPSRS = std::move(oProj);

    if (!transform_ok)
    {
        /* transform is sufficient in some cases (slant range, standalone gcps)
         */
        adfGeoTransform[0] = 0.0;
        adfGeoTransform[1] = 1.0;
        adfGeoTransform[2] = 0.0;
        adfGeoTransform[3] = 0.0;
        adfGeoTransform[4] = 0.0;
        adfGeoTransform[5] = 1.0;
        m_oSRS.Clear();
    }

    delete mffEllipsoids;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *MFFDataset::Open(GDALOpenInfo *poOpenInfo)

{
    /* -------------------------------------------------------------------- */
    /*      We assume the user is pointing to the header file.              */
    /* -------------------------------------------------------------------- */
    if (poOpenInfo->nHeaderBytes < 17 || poOpenInfo->fpL == nullptr)
        return nullptr;

    if (!poOpenInfo->IsExtensionEqualToCI("hdr"))
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Load the .hdr file, and compress white space out around the     */
    /*      equal sign.                                                     */
    /* -------------------------------------------------------------------- */
    char **papszHdrLines = CSLLoad(poOpenInfo->pszFilename);
    if (papszHdrLines == nullptr)
        return nullptr;

    // Remove spaces.  e.g.
    // SPHEROID_NAME = CLARKE_1866 -> SPHEROID_NAME=CLARKE_1866
    for (int i = 0; papszHdrLines[i] != nullptr; i++)
    {
        int iDst = 0;
        char *pszLine = papszHdrLines[i];

        for (int iSrc = 0; pszLine[iSrc] != '\0'; iSrc++)
        {
            if (pszLine[iSrc] != ' ')
            {
                pszLine[iDst++] = pszLine[iSrc];
            }
        }
        pszLine[iDst] = '\0';
    }

    /* -------------------------------------------------------------------- */
    /*      Verify it is an MFF file.                                       */
    /* -------------------------------------------------------------------- */
    if (CSLFetchNameValue(papszHdrLines, "IMAGE_FILE_FORMAT") != nullptr &&
        !EQUAL(CSLFetchNameValue(papszHdrLines, "IMAGE_FILE_FORMAT"), "MFF"))
    {
        CSLDestroy(papszHdrLines);
        return nullptr;
    }

    if ((CSLFetchNameValue(papszHdrLines, "IMAGE_LINES") == nullptr ||
         CSLFetchNameValue(papszHdrLines, "LINE_SAMPLES") == nullptr) &&
        (CSLFetchNameValue(papszHdrLines, "no_rows") == nullptr ||
         CSLFetchNameValue(papszHdrLines, "no_columns") == nullptr))
    {
        CSLDestroy(papszHdrLines);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Create a corresponding GDALDataset.                             */
    /* -------------------------------------------------------------------- */
    auto poDS = std::make_unique<MFFDataset>();

    poDS->papszHdrLines = papszHdrLines;

    poDS->eAccess = poOpenInfo->eAccess;

    /* -------------------------------------------------------------------- */
    /*      Set some dataset wide information.                              */
    /* -------------------------------------------------------------------- */
    if (CSLFetchNameValue(papszHdrLines, "no_rows") != nullptr &&
        CSLFetchNameValue(papszHdrLines, "no_columns") != nullptr)
    {
        poDS->nRasterXSize =
            atoi(CSLFetchNameValue(papszHdrLines, "no_columns"));
        poDS->nRasterYSize = atoi(CSLFetchNameValue(papszHdrLines, "no_rows"));
    }
    else
    {
        poDS->nRasterXSize =
            atoi(CSLFetchNameValue(papszHdrLines, "LINE_SAMPLES"));
        poDS->nRasterYSize =
            atoi(CSLFetchNameValue(papszHdrLines, "IMAGE_LINES"));
    }

    if (!GDALCheckDatasetDimensions(poDS->nRasterXSize, poDS->nRasterYSize))
    {
        return nullptr;
    }

    RawRasterBand::ByteOrder eByteOrder = RawRasterBand::NATIVE_BYTE_ORDER;

    const char *pszByteOrder = CSLFetchNameValue(papszHdrLines, "BYTE_ORDER");
    if (pszByteOrder)
    {
        eByteOrder = EQUAL(pszByteOrder, "LSB")
                         ? RawRasterBand::ByteOrder::ORDER_LITTLE_ENDIAN
                         : RawRasterBand::ByteOrder::ORDER_BIG_ENDIAN;
    }

    /* -------------------------------------------------------------------- */
    /*      Get some information specific to APP tiled files.               */
    /* -------------------------------------------------------------------- */
    int nTileXSize = 0;
    int nTileYSize = 0;
    const char *pszRefinedType = CSLFetchNameValue(papszHdrLines, "type");
    const bool bTiled = CSLFetchNameValue(papszHdrLines, "no_rows") != nullptr;

    if (bTiled)
    {
        if (CSLFetchNameValue(papszHdrLines, "tile_size_rows"))
            nTileYSize =
                atoi(CSLFetchNameValue(papszHdrLines, "tile_size_rows"));
        if (CSLFetchNameValue(papszHdrLines, "tile_size_columns"))
            nTileXSize =
                atoi(CSLFetchNameValue(papszHdrLines, "tile_size_columns"));

        if (nTileXSize <= 0 || nTileYSize <= 0 ||
            poDS->nRasterXSize - 1 > INT_MAX - nTileXSize ||
            poDS->nRasterYSize - 1 > INT_MAX - nTileYSize)
        {
            return nullptr;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Read the directory to find matching band files.                 */
    /* -------------------------------------------------------------------- */
    char *const pszTargetPath =
        CPLStrdup(CPLGetPathSafe(poOpenInfo->pszFilename).c_str());
    char *const pszTargetBase =
        CPLStrdup(CPLGetBasenameSafe(poOpenInfo->pszFilename).c_str());
    char **papszDirFiles =
        VSIReadDir(CPLGetPathSafe(poOpenInfo->pszFilename).c_str());
    if (papszDirFiles == nullptr)
    {
        CPLFree(pszTargetPath);
        CPLFree(pszTargetBase);
        return nullptr;
    }

    int nSkipped = 0;
    for (int nRawBand = 0; true; nRawBand++)
    {
        /* Find the next raw band file. */

        int i = 0;  // Used after for.
        for (; papszDirFiles[i] != nullptr; i++)
        {
            if (!EQUAL(CPLGetBasenameSafe(papszDirFiles[i]).c_str(),
                       pszTargetBase))
                continue;

            const std::string osExtension =
                CPLGetExtensionSafe(papszDirFiles[i]);
            if (osExtension.size() >= 2 &&
                isdigit(static_cast<unsigned char>(osExtension[1])) &&
                atoi(osExtension.c_str() + 1) == nRawBand &&
                strchr("bBcCiIjJrRxXzZ", osExtension[0]) != nullptr)
                break;
        }

        if (papszDirFiles[i] == nullptr)
            break;

        /* open the file for required level of access */
        const std::string osRawFilename =
            CPLFormFilenameSafe(pszTargetPath, papszDirFiles[i], nullptr);

        VSILFILE *fpRaw = nullptr;
        if (poOpenInfo->eAccess == GA_Update)
            fpRaw = VSIFOpenL(osRawFilename.c_str(), "rb+");
        else
            fpRaw = VSIFOpenL(osRawFilename.c_str(), "rb");

        if (fpRaw == nullptr)
        {
            CPLError(CE_Warning, CPLE_OpenFailed,
                     "Unable to open %s ... skipping.", osRawFilename.c_str());
            nSkipped++;
            continue;
        }
        poDS->m_papszFileList =
            CSLAddString(poDS->m_papszFileList, osRawFilename.c_str());

        GDALDataType eDataType = GDT_Unknown;
        const std::string osExt = CPLGetExtensionSafe(papszDirFiles[i]);
        if (pszRefinedType != nullptr)
        {
            if (EQUAL(pszRefinedType, "C*4"))
                eDataType = GDT_CFloat32;
            else if (EQUAL(pszRefinedType, "C*8"))
                eDataType = GDT_CFloat64;
            else if (EQUAL(pszRefinedType, "R*4"))
                eDataType = GDT_Float32;
            else if (EQUAL(pszRefinedType, "R*8"))
                eDataType = GDT_Float64;
            else if (EQUAL(pszRefinedType, "I*1"))
                eDataType = GDT_Byte;
            else if (EQUAL(pszRefinedType, "I*2"))
                eDataType = GDT_Int16;
            else if (EQUAL(pszRefinedType, "I*4"))
                eDataType = GDT_Int32;
            else if (EQUAL(pszRefinedType, "U*2"))
                eDataType = GDT_UInt16;
            else if (EQUAL(pszRefinedType, "U*4"))
                eDataType = GDT_UInt32;
            else if (EQUAL(pszRefinedType, "J*1"))
            {
                CPLError(
                    CE_Warning, CPLE_OpenFailed,
                    "Unable to open band %d because type J*1 is not handled. "
                    "Skipping.",
                    nRawBand + 1);
                nSkipped++;
                CPL_IGNORE_RET_VAL(VSIFCloseL(fpRaw));
                continue;  // Does not support 1 byte complex.
            }
            else if (EQUAL(pszRefinedType, "J*2"))
                eDataType = GDT_CInt16;
            else if (EQUAL(pszRefinedType, "K*4"))
                eDataType = GDT_CInt32;
            else
            {
                CPLError(
                    CE_Warning, CPLE_OpenFailed,
                    "Unable to open band %d because type %s is not handled. "
                    "Skipping.\n",
                    nRawBand + 1, pszRefinedType);
                nSkipped++;
                CPL_IGNORE_RET_VAL(VSIFCloseL(fpRaw));
                continue;
            }
        }
        else if (STARTS_WITH_CI(osExt.c_str(), "b"))
        {
            eDataType = GDT_Byte;
        }
        else if (STARTS_WITH_CI(osExt.c_str(), "i"))
        {
            eDataType = GDT_UInt16;
        }
        else if (STARTS_WITH_CI(osExt.c_str(), "j"))
        {
            eDataType = GDT_CInt16;
        }
        else if (STARTS_WITH_CI(osExt.c_str(), "r"))
        {
            eDataType = GDT_Float32;
        }
        else if (STARTS_WITH_CI(osExt.c_str(), "x"))
        {
            eDataType = GDT_CFloat32;
        }
        else
        {
            CPLError(CE_Warning, CPLE_OpenFailed,
                     "Unable to open band %d because extension %s is not "
                     "handled.  Skipping.",
                     nRawBand + 1, osExt.c_str());
            nSkipped++;
            CPL_IGNORE_RET_VAL(VSIFCloseL(fpRaw));
            continue;
        }

        const int nBand = poDS->GetRasterCount() + 1;

        const int nPixelOffset = GDALGetDataTypeSizeBytes(eDataType);
        std::unique_ptr<GDALRasterBand> poBand;

        if (bTiled)
        {
            poBand = std::make_unique<MFFTiledBand>(poDS.get(), nBand, fpRaw,
                                                    nTileXSize, nTileYSize,
                                                    eDataType, eByteOrder);
        }
        else
        {
            if (nPixelOffset != 0 &&
                poDS->GetRasterXSize() > INT_MAX / nPixelOffset)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Int overflow occurred... skipping");
                nSkipped++;
                CPL_IGNORE_RET_VAL(VSIFCloseL(fpRaw));
                continue;
            }

            poBand = RawRasterBand::Create(
                poDS.get(), nBand, fpRaw, 0, nPixelOffset,
                nPixelOffset * poDS->GetRasterXSize(), eDataType, eByteOrder,
                RawRasterBand::OwnFP::YES);
        }

        poDS->SetBand(nBand, std::move(poBand));
    }

    CPLFree(pszTargetPath);
    CPLFree(pszTargetBase);
    CSLDestroy(papszDirFiles);

    /* -------------------------------------------------------------------- */
    /*      Check if we have bands.                                         */
    /* -------------------------------------------------------------------- */
    if (poDS->GetRasterCount() == 0)
    {
        if (nSkipped > 0 && poOpenInfo->eAccess)
        {
            CPLError(CE_Failure, CPLE_OpenFailed,
                     "Failed to open %d files that were apparently bands.  "
                     "Perhaps this dataset is readonly?",
                     nSkipped);
            return nullptr;
        }
        else
        {
            CPLError(CE_Failure, CPLE_OpenFailed,
                     "MFF header file read successfully, but no bands "
                     "were successfully found and opened.");
            return nullptr;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Set all information from the .hdr that isn't well know to be    */
    /*      metadata.                                                       */
    /* -------------------------------------------------------------------- */
    for (int i = 0; papszHdrLines[i] != nullptr; i++)
    {
        char *pszName = nullptr;

        const char *pszValue = CPLParseNameValue(papszHdrLines[i], &pszName);
        if (pszName == nullptr || pszValue == nullptr)
            continue;

        if (!EQUAL(pszName, "END") && !EQUAL(pszName, "FILE_TYPE") &&
            !EQUAL(pszName, "BYTE_ORDER") && !EQUAL(pszName, "no_columns") &&
            !EQUAL(pszName, "no_rows") && !EQUAL(pszName, "type") &&
            !EQUAL(pszName, "tile_size_rows") &&
            !EQUAL(pszName, "tile_size_columns") &&
            !EQUAL(pszName, "IMAGE_FILE_FORMAT") &&
            !EQUAL(pszName, "IMAGE_LINES") && !EQUAL(pszName, "LINE_SAMPLES"))
        {
            poDS->SetMetadataItem(pszName, pszValue);
        }

        CPLFree(pszName);
    }

    /* -------------------------------------------------------------------- */
    /*      Any GCPs in header file?                                        */
    /* -------------------------------------------------------------------- */
    poDS->ScanForGCPs();
    poDS->ScanForProjectionInfo();
    if (poDS->nGCPCount == 0)
        poDS->m_oGCPSRS.Clear();

    /* -------------------------------------------------------------------- */
    /*      Initialize any PAM information.                                 */
    /* -------------------------------------------------------------------- */
    poDS->SetDescription(poOpenInfo->pszFilename);
    poDS->TryLoadXML();

    /* -------------------------------------------------------------------- */
    /*      Check for overviews.                                            */
    /* -------------------------------------------------------------------- */
    poDS->oOvManager.Initialize(poDS.get(), poOpenInfo->pszFilename);

    return poDS.release();
}

int GetMFFProjectionType(const OGRSpatialReference *poSRS)
{
    if (poSRS == nullptr)
    {
        return MFFPRJ_NONE;
    }
    if (poSRS->IsProjected() && poSRS->GetAttrValue("PROJECTION") &&
        EQUAL(poSRS->GetAttrValue("PROJECTION"), SRS_PT_TRANSVERSE_MERCATOR))
    {
        return MFFPRJ_UTM;
    }
    else if (poSRS->IsGeographic())
    {
        return MFFPRJ_LL;
    }
    else
    {
        return MFFPRJ_UNRECOGNIZED;
    }
}

/************************************************************************/
/*                               Create()                               */
/************************************************************************/

GDALDataset *MFFDataset::Create(const char *pszFilenameIn, int nXSize,
                                int nYSize, int nBandsIn, GDALDataType eType,
                                char **papszParamList)

{
    /* -------------------------------------------------------------------- */
    /*      Verify input options.                                           */
    /* -------------------------------------------------------------------- */
    if (nBandsIn <= 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "MFF driver does not support %d bands.", nBandsIn);
        return nullptr;
    }

    if (eType != GDT_Byte && eType != GDT_Float32 && eType != GDT_UInt16 &&
        eType != GDT_CInt16 && eType != GDT_CFloat32)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Attempt to create MFF file with currently unsupported\n"
                 "data type (%s).\n",
                 GDALGetDataTypeName(eType));

        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Establish the base filename (path+filename, less extension).    */
    /* -------------------------------------------------------------------- */
    char *pszBaseFilename =
        static_cast<char *>(CPLMalloc(strlen(pszFilenameIn) + 5));
    strcpy(pszBaseFilename, pszFilenameIn);

    for (int i = static_cast<int>(strlen(pszBaseFilename)) - 1; i > 0; i--)
    {
        if (pszBaseFilename[i] == '.')
        {
            pszBaseFilename[i] = '\0';
            break;
        }

        if (pszBaseFilename[i] == '/' || pszBaseFilename[i] == '\\')
            break;
    }

    /* -------------------------------------------------------------------- */
    /*      Create the header file.                                         */
    /* -------------------------------------------------------------------- */
    std::string osFilename =
        CPLFormFilenameSafe(nullptr, pszBaseFilename, "hdr");

    VSILFILE *fp = VSIFOpenL(osFilename.c_str(), "wt");
    if (fp == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed, "Couldn't create %s.\n",
                 osFilename.c_str());
        CPLFree(pszBaseFilename);
        return nullptr;
    }

    bool bOK = VSIFPrintfL(fp, "IMAGE_FILE_FORMAT = MFF\n") >= 0;
    bOK &= VSIFPrintfL(fp, "FILE_TYPE = IMAGE\n") >= 0;
    bOK &= VSIFPrintfL(fp, "IMAGE_LINES = %d\n", nYSize) >= 0;
    bOK &= VSIFPrintfL(fp, "LINE_SAMPLES = %d\n", nXSize) >= 0;
#ifdef CPL_MSB
    bOK &= VSIFPrintfL(fp, "BYTE_ORDER = MSB\n") >= 0;
#else
    bOK &= VSIFPrintfL(fp, "BYTE_ORDER = LSB\n") >= 0;
#endif

    if (CSLFetchNameValue(papszParamList, "NO_END") == nullptr)
        bOK &= VSIFPrintfL(fp, "END\n") >= 0;

    if (VSIFCloseL(fp) != 0)
        bOK = false;

    /* -------------------------------------------------------------------- */
    /*      Create the data files, but don't bother writing any data to them.*/
    /* -------------------------------------------------------------------- */
    for (int iBand = 0; bOK && iBand < nBandsIn; iBand++)
    {
        char szExtension[4] = {'\0'};

        if (eType == GDT_Byte)
            CPLsnprintf(szExtension, sizeof(szExtension), "b%02d", iBand);
        else if (eType == GDT_UInt16)
            CPLsnprintf(szExtension, sizeof(szExtension), "i%02d", iBand);
        else if (eType == GDT_Float32)
            CPLsnprintf(szExtension, sizeof(szExtension), "r%02d", iBand);
        else if (eType == GDT_CInt16)
            CPLsnprintf(szExtension, sizeof(szExtension), "j%02d", iBand);
        else if (eType == GDT_CFloat32)
            CPLsnprintf(szExtension, sizeof(szExtension), "x%02d", iBand);

        osFilename = CPLFormFilenameSafe(nullptr, pszBaseFilename, szExtension);
        fp = VSIFOpenL(osFilename.c_str(), "wb");
        if (fp == nullptr)
        {
            CPLError(CE_Failure, CPLE_OpenFailed, "Couldn't create %s.\n",
                     osFilename.c_str());
            CPLFree(pszBaseFilename);
            return nullptr;
        }

        bOK &= VSIFWriteL("", 1, 1, fp) == 1;
        if (VSIFCloseL(fp) != 0)
            bOK = false;
    }

    if (!bOK)
    {
        CPLFree(pszBaseFilename);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Open the dataset normally.                                      */
    /* -------------------------------------------------------------------- */
    strcat(pszBaseFilename, ".hdr");
    GDALDataset *poDS =
        GDALDataset::Open(pszBaseFilename, GDAL_OF_RASTER | GDAL_OF_UPDATE);
    CPLFree(pszBaseFilename);

    return poDS;
}

/************************************************************************/
/*                             CreateCopy()                             */
/************************************************************************/

GDALDataset *MFFDataset::CreateCopy(const char *pszFilename,
                                    GDALDataset *poSrcDS, int /* bStrict */,
                                    char **papszOptions,
                                    GDALProgressFunc pfnProgress,
                                    void *pProgressData)
{
    const int nBands = poSrcDS->GetRasterCount();
    if (nBands == 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "MFF driver does not support source dataset with zero band.");
        return nullptr;
    }

    GDALDataType eType = poSrcDS->GetRasterBand(1)->GetRasterDataType();
    if (!pfnProgress(0.0, nullptr, pProgressData))
        return nullptr;

    // Check that other bands match type- sets type
    // to unknown if they differ.
    for (int iBand = 1; iBand < poSrcDS->GetRasterCount(); iBand++)
    {
        GDALRasterBand *poBand = poSrcDS->GetRasterBand(iBand + 1);
        eType = GDALDataTypeUnion(eType, poBand->GetRasterDataType());
    }

    char **newpapszOptions = CSLDuplicate(papszOptions);
    newpapszOptions = CSLSetNameValue(newpapszOptions, "NO_END", "TRUE");

    MFFDataset *poDS = reinterpret_cast<MFFDataset *>(Create(
        pszFilename, poSrcDS->GetRasterXSize(), poSrcDS->GetRasterYSize(),
        poSrcDS->GetRasterCount(), eType, newpapszOptions));

    CSLDestroy(newpapszOptions);

    if (poDS == nullptr)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      Copy the image data.                                            */
    /* -------------------------------------------------------------------- */
    const int nXSize = poDS->GetRasterXSize();
    const int nYSize = poDS->GetRasterYSize();

    int nBlockXSize = 0;
    int nBlockYSize = 0;
    poDS->GetRasterBand(1)->GetBlockSize(&nBlockXSize, &nBlockYSize);

    const int nBlockTotal = ((nXSize + nBlockXSize - 1) / nBlockXSize) *
                            ((nYSize + nBlockYSize - 1) / nBlockYSize) *
                            poSrcDS->GetRasterCount();

    int nBlocksDone = 0;
    for (int iBand = 0; iBand < poSrcDS->GetRasterCount(); iBand++)
    {
        GDALRasterBand *poSrcBand = poSrcDS->GetRasterBand(iBand + 1);
        GDALRasterBand *poDstBand = poDS->GetRasterBand(iBand + 1);

        void *pData = CPLMalloc(static_cast<size_t>(nBlockXSize) * nBlockYSize *
                                GDALGetDataTypeSizeBytes(eType));

        for (int iYOffset = 0; iYOffset < nYSize; iYOffset += nBlockYSize)
        {
            for (int iXOffset = 0; iXOffset < nXSize; iXOffset += nBlockXSize)
            {
                if (!pfnProgress((nBlocksDone++) /
                                     static_cast<float>(nBlockTotal),
                                 nullptr, pProgressData))
                {
                    CPLError(CE_Failure, CPLE_UserInterrupt, "User terminated");
                    delete poDS;
                    CPLFree(pData);

                    GDALDriver *poMFFDriver =
                        static_cast<GDALDriver *>(GDALGetDriverByName("MFF"));
                    poMFFDriver->Delete(pszFilename);
                    return nullptr;
                }

                const int nTBXSize = std::min(nBlockXSize, nXSize - iXOffset);
                const int nTBYSize = std::min(nBlockYSize, nYSize - iYOffset);

                CPLErr eErr = poSrcBand->RasterIO(
                    GF_Read, iXOffset, iYOffset, nTBXSize, nTBYSize, pData,
                    nTBXSize, nTBYSize, eType, 0, 0, nullptr);

                if (eErr != CE_None)
                {
                    delete poDS;
                    CPLFree(pData);
                    return nullptr;
                }

                eErr = poDstBand->RasterIO(GF_Write, iXOffset, iYOffset,
                                           nTBXSize, nTBYSize, pData, nTBXSize,
                                           nTBYSize, eType, 0, 0, nullptr);

                if (eErr != CE_None)
                {
                    delete poDS;
                    CPLFree(pData);
                    return nullptr;
                }
            }
        }

        CPLFree(pData);
    }

    /* -------------------------------------------------------------------- */
    /*      Copy georeferencing information, if enough is available.        */
    /* -------------------------------------------------------------------- */

    /* -------------------------------------------------------------------- */
    /*      Establish the base filename (path+filename, less extension).    */
    /* -------------------------------------------------------------------- */
    char *pszBaseFilename =
        static_cast<char *>(CPLMalloc(strlen(pszFilename) + 5));
    strcpy(pszBaseFilename, pszFilename);

    for (int i = static_cast<int>(strlen(pszBaseFilename)) - 1; i > 0; i--)
    {
        if (pszBaseFilename[i] == '.')
        {
            pszBaseFilename[i] = '\0';
            break;
        }

        if (pszBaseFilename[i] == '/' || pszBaseFilename[i] == '\\')
            break;
    }

    const std::string osFilenameGEO =
        CPLFormFilenameSafe(nullptr, pszBaseFilename, "hdr");

    VSILFILE *fp = VSIFOpenL(osFilenameGEO.c_str(), "at");
    if (fp == nullptr)
    {
        CPLError(CE_Failure, CPLE_OpenFailed,
                 "Couldn't open %s for appending.\n", osFilenameGEO.c_str());
        CPLFree(pszBaseFilename);
        return nullptr;
    }

    /* MFF requires corner and center gcps */
    bool georef_created = false;

    double *padfTiepoints =
        static_cast<double *>(CPLMalloc(2 * sizeof(double) * 5));

    const int src_prj = GetMFFProjectionType(poSrcDS->GetSpatialRef());

    if ((src_prj != MFFPRJ_NONE) && (src_prj != MFFPRJ_UNRECOGNIZED))
    {
        double *tempGeoTransform =
            static_cast<double *>(CPLMalloc(6 * sizeof(double)));

        if ((poSrcDS->GetGeoTransform(tempGeoTransform) == CE_None) &&
            (tempGeoTransform[0] != 0.0 || tempGeoTransform[1] != 1.0 ||
             tempGeoTransform[2] != 0.0 || tempGeoTransform[3] != 0.0 ||
             tempGeoTransform[4] != 0.0 ||
             std::abs(tempGeoTransform[5]) != 1.0))
        {
            padfTiepoints[0] = tempGeoTransform[0] + tempGeoTransform[1] * 0.5 +
                               tempGeoTransform[2] * 0.5;

            padfTiepoints[1] = tempGeoTransform[3] + tempGeoTransform[4] * 0.5 +
                               tempGeoTransform[5] * 0.5;

            padfTiepoints[2] =
                tempGeoTransform[0] + tempGeoTransform[2] * 0.5 +
                tempGeoTransform[1] * (poSrcDS->GetRasterXSize() - 0.5);

            padfTiepoints[3] =
                tempGeoTransform[3] + tempGeoTransform[5] * 0.5 +
                tempGeoTransform[4] * (poSrcDS->GetRasterXSize() - 0.5);

            padfTiepoints[4] =
                tempGeoTransform[0] + tempGeoTransform[1] * 0.5 +
                tempGeoTransform[2] * (poSrcDS->GetRasterYSize() - 0.5);

            padfTiepoints[5] =
                tempGeoTransform[3] + tempGeoTransform[4] * 0.5 +
                tempGeoTransform[5] * (poSrcDS->GetRasterYSize() - 0.5);

            padfTiepoints[6] =
                tempGeoTransform[0] +
                tempGeoTransform[1] * (poSrcDS->GetRasterXSize() - 0.5) +
                tempGeoTransform[2] * (poSrcDS->GetRasterYSize() - 0.5);

            padfTiepoints[7] =
                tempGeoTransform[3] +
                tempGeoTransform[4] * (poSrcDS->GetRasterXSize() - 0.5) +
                tempGeoTransform[5] * (poSrcDS->GetRasterYSize() - 0.5);

            padfTiepoints[8] =
                tempGeoTransform[0] +
                tempGeoTransform[1] * (poSrcDS->GetRasterXSize()) / 2.0 +
                tempGeoTransform[2] * (poSrcDS->GetRasterYSize()) / 2.0;

            padfTiepoints[9] =
                tempGeoTransform[3] +
                tempGeoTransform[4] * (poSrcDS->GetRasterXSize()) / 2.0 +
                tempGeoTransform[5] * (poSrcDS->GetRasterYSize()) / 2.0;

            OGRSpatialReference oUTMorLL;
            const auto poSrcSRS = poSrcDS->GetSpatialRef();
            if (poSrcSRS)
                oUTMorLL = *poSrcSRS;
            auto poLLSRS = oUTMorLL.CloneGeogCS();
            if (poLLSRS && oUTMorLL.IsProjected())
            {
                poLLSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
                OGRCoordinateTransformation *poTransform =
                    OGRCreateCoordinateTransformation(&oUTMorLL, poLLSRS);

                // projected coordinate system- need to translate gcps */
                bool bSuccess = poTransform != nullptr;

                for (int index = 0; index < 5; index++)
                {
                    // TODO: If bSuccess is false, set it to false?
                    if (!bSuccess || !poTransform->Transform(
                                         1, &(padfTiepoints[index * 2]),
                                         &(padfTiepoints[index * 2 + 1])))
                        bSuccess = false;
                }
                if (bSuccess)
                    georef_created = true;
            }
            else
            {
                georef_created = true;
            }
            delete poLLSRS;
        }
        CPLFree(tempGeoTransform);
    }

    bool bOK = true;
    if (georef_created)
    {
        /* --------------------------------------------------------------------
         */
        /*      top left */
        /* --------------------------------------------------------------------
         */
        bOK &= VSIFPrintfL(fp, "TOP_LEFT_CORNER_LATITUDE = %.10f\n",
                           padfTiepoints[1]) >= 0;
        bOK &= VSIFPrintfL(fp, "TOP_LEFT_CORNER_LONGITUDE = %.10f\n",
                           padfTiepoints[0]) >= 0;
        /* --------------------------------------------------------------------
         */
        /*      top_right */
        /* --------------------------------------------------------------------
         */
        bOK &= VSIFPrintfL(fp, "TOP_RIGHT_CORNER_LATITUDE = %.10f\n",
                           padfTiepoints[3]) >= 0;
        bOK &= VSIFPrintfL(fp, "TOP_RIGHT_CORNER_LONGITUDE = %.10f\n",
                           padfTiepoints[2]) >= 0;
        /* --------------------------------------------------------------------
         */
        /*      bottom_left */
        /* --------------------------------------------------------------------
         */
        bOK &= VSIFPrintfL(fp, "BOTTOM_LEFT_CORNER_LATITUDE = %.10f\n",
                           padfTiepoints[5]) >= 0;
        bOK &= VSIFPrintfL(fp, "BOTTOM_LEFT_CORNER_LONGITUDE = %.10f\n",
                           padfTiepoints[4]) >= 0;
        /* --------------------------------------------------------------------
         */
        /*      bottom_right */
        /* --------------------------------------------------------------------
         */
        bOK &= VSIFPrintfL(fp, "BOTTOM_RIGHT_CORNER_LATITUDE = %.10f\n",
                           padfTiepoints[7]) >= 0;
        bOK &= VSIFPrintfL(fp, "BOTTOM_RIGHT_CORNER_LONGITUDE = %.10f\n",
                           padfTiepoints[6]) >= 0;
        /* --------------------------------------------------------------------
         */
        /*      Center */
        /* --------------------------------------------------------------------
         */
        bOK &=
            VSIFPrintfL(fp, "CENTRE_LATITUDE = %.10f\n", padfTiepoints[9]) >= 0;
        bOK &= VSIFPrintfL(fp, "CENTRE_LONGITUDE = %.10f\n",
                           padfTiepoints[8]) >= 0;
        /* -------------------------------------------------------------------
         */
        /*     Ellipsoid/projection */
        /* --------------------------------------------------------------------*/

        const auto poSrcSRS = poSrcDS->GetSpatialRef();
        char *spheroid_name = nullptr;

        if (poSrcSRS != nullptr)
        {
            if (poSrcSRS->IsProjected() &&
                poSrcSRS->GetAttrValue("PROJECTION") != nullptr &&
                EQUAL(poSrcSRS->GetAttrValue("PROJECTION"),
                      SRS_PT_TRANSVERSE_MERCATOR))
            {
                bOK &= VSIFPrintfL(fp, "PROJECTION_NAME = UTM\n") >= 0;
                OGRErr ogrerrorOl = OGRERR_NONE;
                bOK &=
                    VSIFPrintfL(fp, "PROJECTION_ORIGIN_LONGITUDE = %f\n",
                                poSrcSRS->GetProjParm(SRS_PP_CENTRAL_MERIDIAN,
                                                      0.0, &ogrerrorOl)) >= 0;
            }
            else if (poSrcSRS->IsGeographic())
            {
                bOK &= VSIFPrintfL(fp, "PROJECTION_NAME = LL\n") >= 0;
            }
            else
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "Unrecognized projection- no georeferencing "
                         "information transferred.");
                bOK &= VSIFPrintfL(fp, "PROJECTION_NAME = LL\n") >= 0;
            }
            OGRErr ogrerrorEq = OGRERR_NONE;
            const double eq_radius = poSrcSRS->GetSemiMajor(&ogrerrorEq);
            OGRErr ogrerrorInvf = OGRERR_NONE;
            const double inv_flattening =
                poSrcSRS->GetInvFlattening(&ogrerrorInvf);
            if (ogrerrorEq == OGRERR_NONE && ogrerrorInvf == OGRERR_NONE)
            {
                MFFSpheroidList *mffEllipsoids = new MFFSpheroidList;
                spheroid_name =
                    mffEllipsoids->GetSpheroidNameByEqRadiusAndInvFlattening(
                        eq_radius, inv_flattening);
                if (spheroid_name != nullptr)
                {
                    bOK &= VSIFPrintfL(fp, "SPHEROID_NAME = %s\n",
                                       spheroid_name) >= 0;
                }
                else
                {
                    bOK &= VSIFPrintfL(fp,
                                       "SPHEROID_NAME = USER_DEFINED\n"
                                       "SPHEROID_EQUATORIAL_RADIUS = %.10f\n"
                                       "SPHEROID_POLAR_RADIUS = %.10f\n",
                                       eq_radius,
                                       eq_radius *
                                           (1 - 1.0 / inv_flattening)) >= 0;
                }
                delete mffEllipsoids;
                CPLFree(spheroid_name);
            }
        }
    }

    CPLFree(padfTiepoints);
    bOK &= VSIFPrintfL(fp, "END\n") >= 0;
    if (VSIFCloseL(fp) != 0)
        bOK = false;

    if (!bOK)
    {
        delete poDS;
        CPLFree(pszBaseFilename);
        return nullptr;
    }

    /* End of georeferencing stuff */

    /* Make sure image data gets flushed */
    for (int iBand = 0; iBand < poDS->GetRasterCount(); iBand++)
    {
        RawRasterBand *poDstBand =
            reinterpret_cast<RawRasterBand *>(poDS->GetRasterBand(iBand + 1));
        poDstBand->FlushCache(false);
    }

    if (!pfnProgress(1.0, nullptr, pProgressData))
    {
        CPLError(CE_Failure, CPLE_UserInterrupt, "User terminated");
        delete poDS;

        GDALDriver *poMFFDriver =
            static_cast<GDALDriver *>(GDALGetDriverByName("MFF"));
        poMFFDriver->Delete(pszFilename);
        CPLFree(pszBaseFilename);
        return nullptr;
    }

    poDS->CloneInfo(poSrcDS, GCIF_PAM_DEFAULT);
    CPLFree(pszBaseFilename);

    return poDS;
}

/************************************************************************/
/*                         GDALRegister_MFF()                           */
/************************************************************************/

void GDALRegister_MFF()

{
    if (GDALGetDriverByName("MFF") != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    poDriver->SetDescription("MFF");
    poDriver->SetMetadataItem(GDAL_DCAP_RASTER, "YES");
    poDriver->SetMetadataItem(GDAL_DMD_LONGNAME, "Vexcel MFF Raster");
    poDriver->SetMetadataItem(GDAL_DMD_HELPTOPIC, "drivers/raster/mff.html");
    poDriver->SetMetadataItem(GDAL_DMD_EXTENSION, "hdr");
    poDriver->SetMetadataItem(GDAL_DMD_CREATIONDATATYPES,
                              "Byte UInt16 Float32 CInt16 CFloat32");

    poDriver->SetMetadataItem(GDAL_DCAP_VIRTUALIO, "YES");

    poDriver->pfnOpen = MFFDataset::Open;
    poDriver->pfnCreate = MFFDataset::Create;
    poDriver->pfnCreateCopy = MFFDataset::CreateCopy;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
