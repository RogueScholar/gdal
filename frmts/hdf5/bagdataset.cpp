/******************************************************************************
 *
 * Project:  Hierarchical Data Format Release 5 (HDF5)
 * Purpose:  Read BAG datasets.
 * Author:   Frank Warmerdam <warmerdam@pobox.com>
 *
 ******************************************************************************
 * Copyright (c) 2009, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2009-2018, Even Rouault <even dot rouault at spatialys dot com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "hdf5dataset.h"
#include "hdf5drivercore.h"
#include "gh5_convenience.h"

#include "cpl_mem_cache.h"
#include "cpl_string.h"
#include "cpl_time.h"
#include "gdal_alg.h"
#include "gdal_frmts.h"
#include "gdal_pam.h"
#include "gdal_priv.h"
#include "gdal_rat.h"
#include "iso19115_srs.h"
#include "ogr_core.h"
#include "ogr_spatialref.h"
#include "ogrsf_frmts.h"
#include "rat.h"

#ifdef EMBED_RESOURCE_FILES
#include "embedded_resources.h"
#endif

#include <cassert>
#include <cmath>
#include <algorithm>
#include <limits>
#include <map>
#include <utility>
#include <set>

struct BAGRefinementGrid
{
    unsigned nIndex = 0;
    unsigned nWidth = 0;
    unsigned nHeight = 0;
    float fResX = 0.0f;
    float fResY = 0.0f;
    float fSWX = 0.0f;  // offset from (bottom left corner of) the south west
                        // low resolution grid, in center-pixel convention
    float fSWY = 0.0f;  // offset from (bottom left corner of) the south west
                        // low resolution grid, in center-pixel convention
};

constexpr float fDEFAULT_NODATA = 1000000.0f;

/************************************************************************/
/*                                h5check()                             */
/************************************************************************/

#ifdef DEBUG
template <class T> static T h5check(T ret, const char *filename, int line)
{
    if (ret < 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "HDF5 API failed at %s:%d",
                 filename, line);
    }
    return ret;
}

#define H5_CHECK(x) h5check(x, __FILE__, __LINE__)
#else
#define H5_CHECK(x) (x)
#endif

/************************************************************************/
/* ==================================================================== */
/*                               BAGDataset                             */
/* ==================================================================== */
/************************************************************************/
class BAGDataset final : public GDALPamDataset
{
    friend class BAGRasterBand;
    friend class BAGSuperGridBand;
    friend class BAGBaseBand;
    friend class BAGResampledBand;
    friend class BAGGeorefMDSuperGridBand;
    friend class BAGInterpolatedBand;

    bool m_bReportVertCRS = true;

    enum class Population
    {
        MAX,
        MIN,
        MEAN,
        COUNT
    };
    Population m_ePopulation = Population::MAX;
    bool m_bMask = false;

    bool m_bIsChild = false;
    std::vector<std::unique_ptr<BAGDataset>> m_apoOverviewDS{};

    std::shared_ptr<GDAL::HDF5SharedResources> m_poSharedResources{};
    std::shared_ptr<GDALGroup> m_poRootGroup{};

    std::unique_ptr<OGRLayer> m_poTrackingListLayer{};

    OGRSpatialReference m_oSRS{};
    GDALGeoTransform m_gt{};

    int m_nLowResWidth = 0;
    int m_nLowResHeight = 0;

    double m_dfLowResMinX = 0.0;
    double m_dfLowResMinY = 0.0;
    double m_dfLowResMaxX = 0.0;
    double m_dfLowResMaxY = 0.0;

    void LoadMetadata();

    char *pszXMLMetadata = nullptr;
    char *apszMDList[2]{};

    int m_nChunkXSizeVarresMD = 0;
    int m_nChunkYSizeVarresMD = 0;
    void GetVarresMetadataChunkSizes(int &nChunkXSize, int &nChunkYSize);

    unsigned m_nChunkSizeVarresRefinement = 0;
    void GetVarresRefinementChunkSize(unsigned &nChunkSize);

    bool ReadVarresMetadataValue(int y, int x, hid_t memspace,
                                 BAGRefinementGrid *rgrid, int height,
                                 int width);

    bool LookForRefinementGrids(CSLConstList l_papszOpenOptions, int nY,
                                int nX);

    hid_t m_hVarresMetadata = -1;
    hid_t m_hVarresMetadataDataType = -1;
    hid_t m_hVarresMetadataDataspace = -1;
    hid_t m_hVarresMetadataNative = -1;
    std::map<int, BAGRefinementGrid> m_oMapRefinemendGrids{};

    CPLStringList m_aosSubdatasets{};

    hid_t m_hVarresRefinements = -1;
    hid_t m_hVarresRefinementsDataType = -1;
    hid_t m_hVarresRefinementsDataspace = -1;
    hid_t m_hVarresRefinementsNative = -1;
    unsigned m_nRefinementsSize = 0;

    unsigned m_nSuperGridRefinementStartIndex = 0;

    lru11::Cache<unsigned, std::vector<float>> m_oCacheRefinementValues{};
    const float *GetRefinementValues(unsigned nRefinementIndex);

    bool GetMeanSupergridsResolution(double &dfResX, double &dfResY);

    double m_dfResFilterMin = 0;
    double m_dfResFilterMax = std::numeric_limits<double>::infinity();

    void InitOverviewDS(BAGDataset *poParentDS, int nXSize, int nYSize);

    bool m_bMetadataWritten = false;
    CPLStringList m_aosCreationOptions{};
    bool WriteMetadataIfNeeded();

    bool OpenRaster(GDALOpenInfo *poOpenInfo, const CPLString &osFilename,
                    bool bOpenSuperGrid, int nX, int nY, bool bIsSubdataset,
                    const CPLString &osGeorefMetadataLayer,
                    CPLString &outOsSubDsName);
    bool OpenVector();

    inline hid_t GetHDF5Handle()
    {
        return m_poSharedResources->m_hHDF5;
    }

    CPL_DISALLOW_COPY_ASSIGN(BAGDataset)

  public:
    BAGDataset();
    BAGDataset(BAGDataset *poParentDS, int nOvrFactor);
    BAGDataset(BAGDataset *poParentDS, int nXSize, int nYSize);
    virtual ~BAGDataset();

    virtual CPLErr GetGeoTransform(GDALGeoTransform &gt) const override;
    const OGRSpatialReference *GetSpatialRef() const override;

    CPLErr SetGeoTransform(const GDALGeoTransform &gt) override;
    CPLErr SetSpatialRef(const OGRSpatialReference *poSRS) override;

    virtual char **GetMetadataDomainList() override;
    virtual char **GetMetadata(const char *pszDomain = "") override;

    int GetLayerCount() override
    {
        return m_poTrackingListLayer ? 1 : 0;
    }

    OGRLayer *GetLayer(int idx) override;

    static GDALDataset *Open(GDALOpenInfo *);
    static GDALDataset *OpenForCreate(GDALOpenInfo *, int nXSizeIn,
                                      int nYSizeIn, int nBandsIn,
                                      CSLConstList papszCreationOptions);
    static GDALDataset *CreateCopy(const char *pszFilename,
                                   GDALDataset *poSrcDS, int bStrict,
                                   char **papszOptions,
                                   GDALProgressFunc pfnProgress,
                                   void *pProgressData);
    static GDALDataset *Create(const char *pszFilename, int nXSize, int nYSize,
                               int nBandsIn, GDALDataType eType,
                               char **papszOptions);

    OGRErr ParseWKTFromXML(const char *pszISOXML);
};

/************************************************************************/
/*                              BAGCreator                              */
/************************************************************************/

class BAGCreator
{
    hid_t m_hdf5 = -1;
    hid_t m_bagRoot = -1;

    bool CreateBase(const char *pszFilename, char **papszOptions);
    bool CreateTrackingListDataset();
    bool CreateElevationOrUncertainty(
        GDALDataset *poSrcDS, int nBand, const char *pszDSName,
        const char *pszMaxAttrName, const char *pszMinAttrName,
        char **papszOptions, GDALProgressFunc pfnProgress, void *pProgressData);
    bool Close();

  public:
    BAGCreator() = default;
    ~BAGCreator();

    static bool SubstituteVariables(CPLXMLNode *psNode, char **papszDict);
    static CPLString GenerateMetadata(int nXSize, int nYSize,
                                      const GDALGeoTransform &gt,
                                      const OGRSpatialReference *poSRS,
                                      char **papszOptions);
    static bool CreateAndWriteMetadata(hid_t hdf5,
                                       const CPLString &osXMLMetadata);

    bool Create(const char *pszFilename, GDALDataset *poSrcDS,
                char **papszOptions, GDALProgressFunc pfnProgress,
                void *pProgressData);

    bool Create(const char *pszFilename, int nBands, GDALDataType eType,
                char **papszOptions);
};

/************************************************************************/
/* ==================================================================== */
/*                               BAGRasterBand                          */
/* ==================================================================== */
/************************************************************************/
class BAGRasterBand final : public GDALPamRasterBand
{
    friend class BAGDataset;

    hid_t m_hDatasetID = -1;
    hid_t m_hNative = -1;
    hid_t m_hDataspace = -1;

    bool m_bMinMaxSet = false;
    double m_dfMinimum = std::numeric_limits<double>::max();
    double m_dfMaximum = -std::numeric_limits<double>::max();

    bool m_bHasNoData = false;
    float m_fNoDataValue = std::numeric_limits<float>::quiet_NaN();

    bool CreateDatasetIfNeeded();
    void FinalizeDataset();

  public:
    BAGRasterBand(BAGDataset *, int);
    virtual ~BAGRasterBand();

    bool Initialize(hid_t hDataset, const char *pszName);

    virtual CPLErr IReadBlock(int, int, void *) override;
    virtual CPLErr IWriteBlock(int, int, void *) override;
    virtual double GetNoDataValue(int *) override;
    virtual CPLErr SetNoDataValue(double dfNoData) override;

    virtual double GetMinimum(int *pbSuccess = nullptr) override;
    virtual double GetMaximum(int *pbSuccess = nullptr) override;
};

/************************************************************************/
/* ==================================================================== */
/*                              BAGBaseBand                             */
/* ==================================================================== */
/************************************************************************/

class BAGBaseBand CPL_NON_FINAL : public GDALRasterBand
{
  protected:
    bool m_bHasNoData = false;
    float m_fNoDataValue = std::numeric_limits<float>::quiet_NaN();

  public:
    BAGBaseBand() = default;
    ~BAGBaseBand() = default;

    double GetNoDataValue(int *) override;

    int GetOverviewCount() override;
    GDALRasterBand *GetOverview(int) override;
};

/************************************************************************/
/*                           GetNoDataValue()                           */
/************************************************************************/

double BAGBaseBand::GetNoDataValue(int *pbSuccess)

{
    if (pbSuccess)
        *pbSuccess = m_bHasNoData;
    if (m_bHasNoData)
        return m_fNoDataValue;

    return GDALRasterBand::GetNoDataValue(pbSuccess);
}

/************************************************************************/
/*                          GetOverviewCount()                          */
/************************************************************************/

int BAGBaseBand::GetOverviewCount()
{
    BAGDataset *poGDS = cpl::down_cast<BAGDataset *>(poDS);
    return static_cast<int>(poGDS->m_apoOverviewDS.size());
}

/************************************************************************/
/*                            GetOverview()                             */
/************************************************************************/

GDALRasterBand *BAGBaseBand::GetOverview(int i)

{
    if (i < 0 || i >= GetOverviewCount())
        return nullptr;
    BAGDataset *poGDS = cpl::down_cast<BAGDataset *>(poDS);
    return poGDS->m_apoOverviewDS[i]->GetRasterBand(nBand);
}

/************************************************************************/
/* ==================================================================== */
/*                             BAGSuperGridBand                         */
/* ==================================================================== */
/************************************************************************/

class BAGSuperGridBand final : public BAGBaseBand
{
    friend class BAGDataset;

  public:
    BAGSuperGridBand(BAGDataset *, int, bool bHasNoData, float fNoDataValue);
    virtual ~BAGSuperGridBand();

    CPLErr IReadBlock(int, int, void *) override;
};

/************************************************************************/
/* ==================================================================== */
/*                             BAGResampledBand                         */
/* ==================================================================== */
/************************************************************************/

class BAGResampledBand final : public BAGBaseBand
{
    friend class BAGDataset;

    bool m_bMinMaxSet = false;
    double m_dfMinimum = 0.0;
    double m_dfMaximum = 0.0;

  public:
    BAGResampledBand(BAGDataset *, int nBandIn, bool bHasNoData,
                     float fNoDataValue, bool bInitializeMinMax);
    virtual ~BAGResampledBand();

    void InitializeMinMax();

    CPLErr IReadBlock(int, int, void *) override;

    double GetMinimum(int *pbSuccess = nullptr) override;
    double GetMaximum(int *pbSuccess = nullptr) override;
};

/************************************************************************/
/* ==================================================================== */
/*                          BAGInterpolatedBand                         */
/* ==================================================================== */
/************************************************************************/

class BAGInterpolatedBand final : public BAGBaseBand
{
    friend class BAGDataset;

    bool m_bMinMaxSet = false;
    double m_dfMinimum = 0.0;
    double m_dfMaximum = 0.0;

    void LoadClosestRefinedNodes(
        double dfX, double dfY, int iXRefinedGrid, int iYRefinedGrid,
        const std::vector<BAGRefinementGrid> &rgrids, int nLowResMinIdxX,
        int nLowResMinIdxY, int nCountLowResX, int nCountLowResY,
        double dfLowResMinX, double dfLowResMinY, double dfLowResResX,
        double dfLowResResY, std::vector<double> &adfX,
        std::vector<double> &adfY, std::vector<float> &afDepth,
        std::vector<float> &afUncrt);

  public:
    BAGInterpolatedBand(BAGDataset *, int nBandIn, bool bHasNoData,
                        float fNoDataValue, bool bInitializeMinMax);
    virtual ~BAGInterpolatedBand();

    void InitializeMinMax();

    CPLErr IReadBlock(int, int, void *) override;

    double GetMinimum(int *pbSuccess = nullptr) override;
    double GetMaximum(int *pbSuccess = nullptr) override;
};

/************************************************************************/
/*                           BAGRasterBand()                            */
/************************************************************************/
BAGRasterBand::BAGRasterBand(BAGDataset *poDSIn, int nBandIn)
{
    poDS = poDSIn;
    nBand = nBandIn;
}

/************************************************************************/
/*                           ~BAGRasterBand()                           */
/************************************************************************/

BAGRasterBand::~BAGRasterBand()
{
    HDF5_GLOBAL_LOCK();

    if (eAccess == GA_Update)
    {
        CreateDatasetIfNeeded();
        FinalizeDataset();
    }

    if (m_hDataspace > 0)
        H5Sclose(m_hDataspace);

    if (m_hNative > 0)
        H5Tclose(m_hNative);

    if (m_hDatasetID > 0)
        H5Dclose(m_hDatasetID);
}

/************************************************************************/
/*                             Initialize()                             */
/************************************************************************/

bool BAGRasterBand::Initialize(hid_t hDatasetIDIn, const char *pszName)

{
    GDALRasterBand::SetDescription(pszName);

    m_hDatasetID = hDatasetIDIn;

    const hid_t datatype = H5Dget_type(m_hDatasetID);
    m_hDataspace = H5Dget_space(m_hDatasetID);
    const int n_dims = H5Sget_simple_extent_ndims(m_hDataspace);
    m_hNative = H5Tget_native_type(datatype, H5T_DIR_ASCEND);

    eDataType = GH5_GetDataType(m_hNative);

    if (n_dims == 2)
    {
        hsize_t dims[2] = {static_cast<hsize_t>(0), static_cast<hsize_t>(0)};
        hsize_t maxdims[2] = {static_cast<hsize_t>(0), static_cast<hsize_t>(0)};

        H5Sget_simple_extent_dims(m_hDataspace, dims, maxdims);

        if (dims[0] > INT_MAX || dims[1] > INT_MAX)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "At least one dimension size exceeds INT_MAX !");
            return false;
        }
        nRasterXSize = static_cast<int>(dims[1]);
        nRasterYSize = static_cast<int>(dims[0]);
    }
    else
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Dataset not of rank 2.");
        return false;
    }

    nBlockXSize = nRasterXSize;
    nBlockYSize = 1;

    // Check for chunksize, and use it as blocksize for optimized reading.
    const hid_t listid = H5Dget_create_plist(hDatasetIDIn);
    if (listid > 0)
    {
        if (H5Pget_layout(listid) == H5D_CHUNKED)
        {
            hsize_t panChunkDims[3] = {static_cast<hsize_t>(0),
                                       static_cast<hsize_t>(0),
                                       static_cast<hsize_t>(0)};
            const int nDimSize = H5Pget_chunk(listid, 3, panChunkDims);
            nBlockXSize = static_cast<int>(panChunkDims[nDimSize - 1]);
            nBlockYSize = static_cast<int>(panChunkDims[nDimSize - 2]);
        }

        H5D_fill_value_t fillType = H5D_FILL_VALUE_UNDEFINED;
        if (H5Pfill_value_defined(listid, &fillType) >= 0 &&
            fillType == H5D_FILL_VALUE_USER_DEFINED)
        {
            float fNoDataValue = 0.0f;
            if (H5Pget_fill_value(listid, H5T_NATIVE_FLOAT, &fNoDataValue) >= 0)
            {
                m_bHasNoData = true;
                m_fNoDataValue = fNoDataValue;
            }
        }

        int nfilters = H5Pget_nfilters(listid);

        char name[120] = {};
        size_t cd_nelmts = 20;
        unsigned int cd_values[20] = {};
        unsigned int flags = 0;
        for (int i = 0; i < nfilters; i++)
        {
            const H5Z_filter_t filter = H5Pget_filter(
                listid, i, &flags, &cd_nelmts, cd_values, sizeof(name), name);
            if (filter == H5Z_FILTER_DEFLATE)
                poDS->GDALDataset::SetMetadataItem("COMPRESSION", "DEFLATE",
                                                   "IMAGE_STRUCTURE");
            else if (filter == H5Z_FILTER_NBIT)
                poDS->GDALDataset::SetMetadataItem("COMPRESSION", "NBIT",
                                                   "IMAGE_STRUCTURE");
            else if (filter == H5Z_FILTER_SCALEOFFSET)
                poDS->GDALDataset::SetMetadataItem("COMPRESSION", "SCALEOFFSET",
                                                   "IMAGE_STRUCTURE");
            else if (filter == H5Z_FILTER_SZIP)
                poDS->GDALDataset::SetMetadataItem("COMPRESSION", "SZIP",
                                                   "IMAGE_STRUCTURE");
        }

        H5Pclose(listid);
    }

    // Load min/max information.
    if (EQUAL(pszName, "elevation") &&
        GH5_FetchAttribute(hDatasetIDIn, "Maximum Elevation Value",
                           m_dfMaximum) &&
        GH5_FetchAttribute(hDatasetIDIn, "Minimum Elevation Value",
                           m_dfMinimum))
    {
        m_bMinMaxSet = true;
    }
    else if (EQUAL(pszName, "uncertainty") &&
             GH5_FetchAttribute(hDatasetIDIn, "Maximum Uncertainty Value",
                                m_dfMaximum) &&
             GH5_FetchAttribute(hDatasetIDIn, "Minimum Uncertainty Value",
                                m_dfMinimum))
    {
        // Some products where uncertainty band is completely set to nodata
        // wrongly declare minimum and maximum to 0.0.
        if (m_dfMinimum != 0.0 || m_dfMaximum != 0.0)
            m_bMinMaxSet = true;
    }
    else if (EQUAL(pszName, "nominal_elevation") &&
             GH5_FetchAttribute(hDatasetIDIn, "max_value", m_dfMaximum) &&
             GH5_FetchAttribute(hDatasetIDIn, "min_value", m_dfMinimum))
    {
        m_bMinMaxSet = true;
    }

    return true;
}

/************************************************************************/
/*                         CreateDatasetIfNeeded()                      */
/************************************************************************/

bool BAGRasterBand::CreateDatasetIfNeeded()
{
    if (m_hDatasetID > 0 || eAccess == GA_ReadOnly)
        return true;

    hsize_t dims[2] = {static_cast<hsize_t>(nRasterYSize),
                       static_cast<hsize_t>(nRasterXSize)};

    m_hDataspace = H5_CHECK(H5Screate_simple(2, dims, nullptr));
    if (m_hDataspace < 0)
        return false;

    BAGDataset *poGDS = cpl::down_cast<BAGDataset *>(poDS);
    bool bDeflate = EQUAL(
        poGDS->m_aosCreationOptions.FetchNameValueDef("COMPRESS", "DEFLATE"),
        "DEFLATE");
    int nCompressionLevel =
        atoi(poGDS->m_aosCreationOptions.FetchNameValueDef("ZLEVEL", "6"));

    bool ret = false;
    hid_t hDataType = -1;
    hid_t hParams = -1;
    do
    {
        hDataType = H5_CHECK(H5Tcopy(H5T_NATIVE_FLOAT));
        if (hDataType < 0)
            break;

        if (H5_CHECK(H5Tset_order(hDataType, H5T_ORDER_LE)) < 0)
            break;

        hParams = H5_CHECK(H5Pcreate(H5P_DATASET_CREATE));
        if (hParams < 0)
            break;

        if (H5_CHECK(H5Pset_fill_time(hParams, H5D_FILL_TIME_ALLOC)) < 0)
            break;

        if (H5_CHECK(H5Pset_fill_value(hParams, hDataType, &m_fNoDataValue)) <
            0)
            break;

        if (H5_CHECK(H5Pset_layout(hParams, H5D_CHUNKED)) < 0)
            break;
        hsize_t chunk_size[2] = {static_cast<hsize_t>(nBlockYSize),
                                 static_cast<hsize_t>(nBlockXSize)};
        if (H5_CHECK(H5Pset_chunk(hParams, 2, chunk_size)) < 0)
            break;

        if (bDeflate)
        {
            if (H5_CHECK(H5Pset_deflate(hParams, nCompressionLevel)) < 0)
                break;
        }

        m_hDatasetID = H5_CHECK(H5Dcreate(poGDS->GetHDF5Handle(),
                                          nBand == 1 ? "/BAG_root/elevation"
                                                     : "/BAG_root/uncertainty",
                                          hDataType, m_hDataspace, hParams));
        if (m_hDatasetID < 0)
            break;

        ret = true;
    } while (false);

    if (hParams >= 0)
        H5_CHECK(H5Pclose(hParams));
    if (hDataType > 0)
        H5_CHECK(H5Tclose(hDataType));

    m_hNative = H5_CHECK(H5Tcopy(H5T_NATIVE_FLOAT));

    return ret;
}

/************************************************************************/
/*                         FinalizeDataset()                            */
/************************************************************************/

void BAGRasterBand::FinalizeDataset()
{
    if (m_dfMinimum > m_dfMaximum)
        return;

    const char *pszMaxAttrName =
        nBand == 1 ? "Maximum Elevation Value" : "Maximum Uncertainty Value";
    const char *pszMinAttrName =
        nBand == 1 ? "Minimum Elevation Value" : "Minimum Uncertainty Value";

    if (!GH5_CreateAttribute(m_hDatasetID, pszMaxAttrName, m_hNative))
        return;

    if (!GH5_CreateAttribute(m_hDatasetID, pszMinAttrName, m_hNative))
        return;

    if (!GH5_WriteAttribute(m_hDatasetID, pszMaxAttrName, m_dfMaximum))
        return;

    if (!GH5_WriteAttribute(m_hDatasetID, pszMinAttrName, m_dfMinimum))
        return;
}

/************************************************************************/
/*                             GetMinimum()                             */
/************************************************************************/

double BAGRasterBand::GetMinimum(int *pbSuccess)

{
    if (m_bMinMaxSet)
    {
        if (pbSuccess)
            *pbSuccess = TRUE;
        return m_dfMinimum;
    }

    return GDALRasterBand::GetMinimum(pbSuccess);
}

/************************************************************************/
/*                             GetMaximum()                             */
/************************************************************************/

double BAGRasterBand::GetMaximum(int *pbSuccess)

{
    if (m_bMinMaxSet)
    {
        if (pbSuccess)
            *pbSuccess = TRUE;
        return m_dfMaximum;
    }

    return GDALRasterBand::GetMaximum(pbSuccess);
}

/************************************************************************/
/*                           GetNoDataValue()                           */
/************************************************************************/
double BAGRasterBand::GetNoDataValue(int *pbSuccess)

{
    if (pbSuccess)
        *pbSuccess = m_bHasNoData;
    if (m_bHasNoData)
        return m_fNoDataValue;

    return GDALPamRasterBand::GetNoDataValue(pbSuccess);
}

/************************************************************************/
/*                           SetNoDataValue()                           */
/************************************************************************/

CPLErr BAGRasterBand::SetNoDataValue(double dfNoData)
{
    if (eAccess == GA_ReadOnly)
        return GDALPamRasterBand::SetNoDataValue(dfNoData);

    if (m_hDatasetID > 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Setting the nodata value after grid values have been written "
                 "is not supported");
        return CE_Failure;
    }

    m_bHasNoData = true;
    m_fNoDataValue = static_cast<float>(dfNoData);
    return CE_None;
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/
CPLErr BAGRasterBand::IReadBlock(int nBlockXOff, int nBlockYOff, void *pImage)
{
    HDF5_GLOBAL_LOCK();

    if (!CreateDatasetIfNeeded())
        return CE_Failure;

    const int nXOff = nBlockXOff * nBlockXSize;
    H5OFFSET_TYPE offset[2] = {
        static_cast<H5OFFSET_TYPE>(
            std::max(0, nRasterYSize - (nBlockYOff + 1) * nBlockYSize)),
        static_cast<H5OFFSET_TYPE>(nXOff)};

    const int nSizeOfData = static_cast<int>(H5Tget_size(m_hNative));
    memset(pImage, 0,
           static_cast<size_t>(nBlockXSize) * nBlockYSize * nSizeOfData);

    //  Blocksize may not be a multiple of imagesize.
    hsize_t count[3] = {
        std::min(static_cast<hsize_t>(nBlockYSize), GetYSize() - offset[0]),
        std::min(static_cast<hsize_t>(nBlockXSize), GetXSize() - offset[1]),
        static_cast<hsize_t>(0)};

    if (nRasterYSize - (nBlockYOff + 1) * nBlockYSize < 0)
    {
        count[0] +=
            (nRasterYSize - static_cast<hsize_t>(nBlockYOff + 1) * nBlockYSize);
    }

    // Select block from file space.
    {
        const herr_t status = H5Sselect_hyperslab(
            m_hDataspace, H5S_SELECT_SET, offset, nullptr, count, nullptr);
        if (status < 0)
            return CE_Failure;
    }

    // Create memory space to receive the data.
    hsize_t col_dims[2] = {static_cast<hsize_t>(nBlockYSize),
                           static_cast<hsize_t>(nBlockXSize)};
    const int rank = 2;
    const hid_t memspace = H5Screate_simple(rank, col_dims, nullptr);
    H5OFFSET_TYPE mem_offset[2] = {0, 0};
    const herr_t status = H5Sselect_hyperslab(
        memspace, H5S_SELECT_SET, mem_offset, nullptr, count, nullptr);
    if (status < 0)
    {
        H5Sclose(memspace);
        return CE_Failure;
    }

    const herr_t status_read = H5Dread(m_hDatasetID, m_hNative, memspace,
                                       m_hDataspace, H5P_DEFAULT, pImage);

    H5Sclose(memspace);

    // Y flip the data.
    const int nLinesToFlip = static_cast<int>(count[0]);
    const int nLineSize = nSizeOfData * nBlockXSize;
    GByte *const pabyTemp = static_cast<GByte *>(CPLMalloc(nLineSize));
    GByte *const pbyImage = static_cast<GByte *>(pImage);

    for (int iY = 0; iY < nLinesToFlip / 2; iY++)
    {
        memcpy(pabyTemp, pbyImage + iY * nLineSize, nLineSize);
        memcpy(pbyImage + iY * nLineSize,
               pbyImage + (nLinesToFlip - iY - 1) * nLineSize, nLineSize);
        memcpy(pbyImage + (nLinesToFlip - iY - 1) * nLineSize, pabyTemp,
               nLineSize);
    }

    CPLFree(pabyTemp);

    // Return success or failure.
    if (status_read < 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "H5Dread() failed for block.");
        return CE_Failure;
    }

    return CE_None;
}

/************************************************************************/
/*                            IWriteBlock()                             */
/************************************************************************/

CPLErr BAGRasterBand::IWriteBlock(int nBlockXOff, int nBlockYOff, void *pImage)
{
    HDF5_GLOBAL_LOCK();

    if (!CreateDatasetIfNeeded())
        return CE_Failure;

    const int nXOff = nBlockXOff * nBlockXSize;
    H5OFFSET_TYPE offset[3] = {
        static_cast<H5OFFSET_TYPE>(
            std::max(0, nRasterYSize - (nBlockYOff + 1) * nBlockYSize)),
        static_cast<H5OFFSET_TYPE>(nXOff)};

    //  Blocksize may not be a multiple of imagesize.
    hsize_t count[3] = {
        std::min(static_cast<hsize_t>(nBlockYSize), GetYSize() - offset[0]),
        std::min(static_cast<hsize_t>(nBlockXSize), GetXSize() - offset[1])};

    if (nRasterYSize - (nBlockYOff + 1) * nBlockYSize < 0)
    {
        count[0] +=
            (nRasterYSize - static_cast<hsize_t>(nBlockYOff + 1) * nBlockYSize);
    }

    // Select block from file space.
    {
        const herr_t status = H5Sselect_hyperslab(
            m_hDataspace, H5S_SELECT_SET, offset, nullptr, count, nullptr);
        if (status < 0)
            return CE_Failure;
    }

    // Create memory space to receive the data.
    hsize_t col_dims[2] = {static_cast<hsize_t>(nBlockYSize),
                           static_cast<hsize_t>(nBlockXSize)};
    const int rank = 2;
    const hid_t memspace = H5Screate_simple(rank, col_dims, nullptr);
    H5OFFSET_TYPE mem_offset[2] = {0, 0};
    const herr_t status = H5Sselect_hyperslab(
        memspace, H5S_SELECT_SET, mem_offset, nullptr, count, nullptr);
    if (status < 0)
    {
        H5Sclose(memspace);
        return CE_Failure;
    }

    // Y flip the data.
    const int nLinesToFlip = static_cast<int>(count[0]);
    const int nSizeOfData = static_cast<int>(H5Tget_size(m_hNative));
    const int nLineSize = nSizeOfData * nBlockXSize;
    GByte *const pabyTemp = static_cast<GByte *>(
        CPLMalloc(static_cast<size_t>(nLineSize) * nLinesToFlip));
    GByte *const pbyImage = static_cast<GByte *>(pImage);

    for (int iY = 0; iY < nLinesToFlip; iY++)
    {
        memcpy(pabyTemp + iY * nLineSize,
               pbyImage + (nLinesToFlip - iY - 1) * nLineSize, nLineSize);
        for (int iX = 0; iX < static_cast<int>(count[1]); iX++)
        {
            float f;
            GDALCopyWords(pabyTemp + iY * nLineSize + iX * nSizeOfData,
                          eDataType, 0, &f, GDT_Float32, 0, 1);
            if (!m_bHasNoData || m_fNoDataValue != f)
            {
                m_dfMinimum = std::min(m_dfMinimum, static_cast<double>(f));
                m_dfMaximum = std::max(m_dfMaximum, static_cast<double>(f));
            }
        }
    }

    const herr_t status_write = H5Dwrite(m_hDatasetID, m_hNative, memspace,
                                         m_hDataspace, H5P_DEFAULT, pabyTemp);

    H5Sclose(memspace);

    CPLFree(pabyTemp);

    // Return success or failure.
    if (status_write < 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "H5Dwrite() failed for block.");
        return CE_Failure;
    }

    return CE_None;
}

/************************************************************************/
/*                           BAGSuperGridBand()                         */
/************************************************************************/

BAGSuperGridBand::BAGSuperGridBand(BAGDataset *poDSIn, int nBandIn,
                                   bool bHasNoData, float fNoDataValue)
{
    poDS = poDSIn;
    nBand = nBandIn;
    nRasterXSize = poDS->GetRasterXSize();
    nRasterYSize = poDS->GetRasterYSize();
    nBlockXSize = nRasterXSize;
    nBlockYSize = 1;
    eDataType = GDT_Float32;
    GDALRasterBand::SetDescription(nBand == 1 ? "elevation" : "uncertainty");
    m_bHasNoData = bHasNoData;
    m_fNoDataValue = fNoDataValue;
}

/************************************************************************/
/*                          ~BAGSuperGridBand()                         */
/************************************************************************/

BAGSuperGridBand::~BAGSuperGridBand() = default;

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr BAGSuperGridBand::IReadBlock(int, int nBlockYOff, void *pImage)
{
    HDF5_GLOBAL_LOCK();

    BAGDataset *poGDS = cpl::down_cast<BAGDataset *>(poDS);
    H5OFFSET_TYPE offset[2] = {
        static_cast<H5OFFSET_TYPE>(0),
        static_cast<H5OFFSET_TYPE>(
            poGDS->m_nSuperGridRefinementStartIndex +
            static_cast<H5OFFSET_TYPE>(nRasterYSize - 1 - nBlockYOff) *
                nBlockXSize)};
    hsize_t count[2] = {1, static_cast<hsize_t>(nBlockXSize)};
    {
        herr_t status = H5Sselect_hyperslab(
            poGDS->m_hVarresRefinementsDataspace, H5S_SELECT_SET, offset,
            nullptr, count, nullptr);
        if (status < 0)
            return CE_Failure;
    }

    // Create memory space to receive the data.
    const hid_t memspace = H5Screate_simple(2, count, nullptr);
    H5OFFSET_TYPE mem_offset[2] = {0, 0};
    {
        const herr_t status = H5Sselect_hyperslab(
            memspace, H5S_SELECT_SET, mem_offset, nullptr, count, nullptr);
        if (status < 0)
        {
            H5Sclose(memspace);
            return CE_Failure;
        }
    }

    float *afBuffer = new float[2 * nBlockXSize];
    {
        const herr_t status = H5Dread(
            poGDS->m_hVarresRefinements, poGDS->m_hVarresRefinementsNative,
            memspace, poGDS->m_hVarresRefinementsDataspace, H5P_DEFAULT,
            afBuffer);
        if (status < 0)
        {
            H5Sclose(memspace);
            delete[] afBuffer;
            return CE_Failure;
        }
    }

    GDALCopyWords(afBuffer + nBand - 1, GDT_Float32, 2 * sizeof(float), pImage,
                  GDT_Float32, sizeof(float), nBlockXSize);

    H5Sclose(memspace);
    delete[] afBuffer;
    return CE_None;
}

/************************************************************************/
/*                           BAGResampledBand()                         */
/************************************************************************/

BAGResampledBand::BAGResampledBand(BAGDataset *poDSIn, int nBandIn,
                                   bool bHasNoData, float fNoDataValue,
                                   bool bInitializeMinMax)
{
    poDS = poDSIn;
    nBand = nBandIn;
    nRasterXSize = poDS->GetRasterXSize();
    nRasterYSize = poDS->GetRasterYSize();
    // Mostly for autotest purposes
    const int nBlockSize =
        std::max(1, atoi(CPLGetConfigOption("GDAL_BAG_BLOCK_SIZE", "256")));
    nBlockXSize = std::min(nBlockSize, poDS->GetRasterXSize());
    nBlockYSize = std::min(nBlockSize, poDS->GetRasterYSize());
    if (poDSIn->m_bMask)
    {
        eDataType = GDT_Byte;
    }
    else if (poDSIn->m_ePopulation == BAGDataset::Population::COUNT)
    {
        eDataType = GDT_UInt32;
        GDALRasterBand::SetDescription("count");
    }
    else
    {
        m_bHasNoData = true;
        m_fNoDataValue = bHasNoData ? fNoDataValue : fDEFAULT_NODATA;
        eDataType = GDT_Float32;
        GDALRasterBand::SetDescription(nBand == 1 ? "elevation"
                                                  : "uncertainty");
    }
    if (bInitializeMinMax)
    {
        InitializeMinMax();
    }
}

/************************************************************************/
/*                          ~BAGResampledBand()                         */
/************************************************************************/

BAGResampledBand::~BAGResampledBand() = default;

/************************************************************************/
/*                           InitializeMinMax()                         */
/************************************************************************/

void BAGResampledBand::InitializeMinMax()
{
    BAGDataset *poGDS = cpl::down_cast<BAGDataset *>(poDS);
    if (nBand == 1 &&
        GH5_FetchAttribute(poGDS->m_hVarresRefinements, "max_depth",
                           m_dfMaximum) &&
        GH5_FetchAttribute(poGDS->m_hVarresRefinements, "min_depth",
                           m_dfMinimum))
    {
        m_bMinMaxSet = true;
    }
    else if (nBand == 2 &&
             GH5_FetchAttribute(poGDS->m_hVarresRefinements, "max_uncrt",
                                m_dfMaximum) &&
             GH5_FetchAttribute(poGDS->m_hVarresRefinements, "min_uncrt",
                                m_dfMinimum))
    {
        m_bMinMaxSet = true;
    }
}

/************************************************************************/
/*                             GetMinimum()                             */
/************************************************************************/

double BAGResampledBand::GetMinimum(int *pbSuccess)

{
    if (m_bMinMaxSet)
    {
        if (pbSuccess)
            *pbSuccess = TRUE;
        return m_dfMinimum;
    }

    return GDALRasterBand::GetMinimum(pbSuccess);
}

/************************************************************************/
/*                             GetMaximum()                             */
/************************************************************************/

double BAGResampledBand::GetMaximum(int *pbSuccess)

{
    if (m_bMinMaxSet)
    {
        if (pbSuccess)
            *pbSuccess = TRUE;
        return m_dfMaximum;
    }

    return GDALRasterBand::GetMaximum(pbSuccess);
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr BAGResampledBand::IReadBlock(int nBlockXOff, int nBlockYOff,
                                    void *pImage)
{
    HDF5_GLOBAL_LOCK();

    BAGDataset *poGDS = cpl::down_cast<BAGDataset *>(poDS);
#ifdef DEBUG_VERBOSE
    CPLDebug(
        "BAG",
        "IReadBlock: nRasterXSize=%d, nBlockXOff=%d, nBlockYOff=%d, nBand=%d",
        nRasterXSize, nBlockXOff, nBlockYOff, nBand);
#endif

    const float fNoDataValue = m_fNoDataValue;
    const float fNoSuperGridValue = m_fNoDataValue;
    float *depthsPtr = nullptr;
    float *uncrtPtr = nullptr;

    GDALRasterBlock *poBlock = nullptr;
    if (poGDS->nBands == 2)
    {
        if (nBand == 1)
        {
            depthsPtr = static_cast<float *>(pImage);
            poBlock = poGDS->GetRasterBand(2)->GetLockedBlockRef(
                nBlockXOff, nBlockYOff, TRUE);
            if (poBlock)
            {
                uncrtPtr = static_cast<float *>(poBlock->GetDataRef());
            }
            else
            {
                return CE_Failure;
            }
        }
        else
        {
            uncrtPtr = static_cast<float *>(pImage);
            poBlock = poGDS->GetRasterBand(1)->GetLockedBlockRef(
                nBlockXOff, nBlockYOff, TRUE);
            if (poBlock)
            {
                depthsPtr = static_cast<float *>(poBlock->GetDataRef());
            }
            else
            {
                return CE_Failure;
            }
        }
    }

    if (depthsPtr)
    {
        GDALCopyWords(&fNoSuperGridValue, GDT_Float32, 0, depthsPtr,
                      GDT_Float32, static_cast<int>(sizeof(float)),
                      nBlockXSize * nBlockYSize);
    }

    if (uncrtPtr)
    {
        GDALCopyWords(&fNoSuperGridValue, GDT_Float32, 0, uncrtPtr, GDT_Float32,
                      static_cast<int>(sizeof(float)),
                      nBlockXSize * nBlockYSize);
    }

    std::vector<int> counts;
    if (poGDS->m_bMask)
    {
        CPLAssert(pImage);  // to make CLang Static Analyzer happy
        memset(pImage, 0, static_cast<size_t>(nBlockXSize) * nBlockYSize);
    }
    else if (poGDS->m_ePopulation == BAGDataset::Population::MEAN)
    {
        counts.resize(static_cast<size_t>(nBlockXSize) * nBlockYSize);
    }
    else if (poGDS->m_ePopulation == BAGDataset::Population::COUNT)
    {
        CPLAssert(pImage);  // to make CLang Static Analyzer happy
        memset(pImage, 0,
               static_cast<size_t>(nBlockXSize) * nBlockYSize *
                   GDALGetDataTypeSizeBytes(eDataType));
    }

    const int nReqCountX =
        std::min(nBlockXSize, nRasterXSize - nBlockXOff * nBlockXSize);
    const int nReqCountY =
        std::min(nBlockYSize, nRasterYSize - nBlockYOff * nBlockYSize);
    // Compute extent of block in georeferenced coordinates
    double dfBlockMinX =
        poGDS->m_gt[0] + nBlockXOff * nBlockXSize * poGDS->m_gt[1];
    double dfBlockMaxX = dfBlockMinX + nReqCountX * poGDS->m_gt[1];
    double dfBlockMaxY =
        poGDS->m_gt[3] + nBlockYOff * nBlockYSize * poGDS->m_gt[5];
    double dfBlockMinY = dfBlockMaxY + nReqCountY * poGDS->m_gt[5];

    // Compute min/max indices of intersecting supergrids (origin bottom-left)
    const double dfLowResResX =
        (poGDS->m_dfLowResMaxX - poGDS->m_dfLowResMinX) / poGDS->m_nLowResWidth;
    const double dfLowResResY =
        (poGDS->m_dfLowResMaxY - poGDS->m_dfLowResMinY) /
        poGDS->m_nLowResHeight;
    int nLowResMinIdxX =
        std::max(0, static_cast<int>((dfBlockMinX - poGDS->m_dfLowResMinX) /
                                     dfLowResResX));
    int nLowResMinIdxY =
        std::max(0, static_cast<int>((dfBlockMinY - poGDS->m_dfLowResMinY) /
                                     dfLowResResY));
    int nLowResMaxIdxX = std::min(
        poGDS->m_nLowResWidth - 1,
        static_cast<int>((dfBlockMaxX - poGDS->m_dfLowResMinX) / dfLowResResX));
    int nLowResMaxIdxY = std::min(
        poGDS->m_nLowResHeight - 1,
        static_cast<int>((dfBlockMaxY - poGDS->m_dfLowResMinY) / dfLowResResY));

    // Create memory space to receive the data.
    const int nCountLowResX = nLowResMaxIdxX - nLowResMinIdxX + 1;
    const int nCountLowResY = nLowResMaxIdxY - nLowResMinIdxY + 1;
    hsize_t countVarresMD[2] = {static_cast<hsize_t>(nCountLowResY),
                                static_cast<hsize_t>(nCountLowResX)};
    const hid_t memspaceVarresMD = H5Screate_simple(2, countVarresMD, nullptr);
    H5OFFSET_TYPE mem_offset[2] = {static_cast<H5OFFSET_TYPE>(0),
                                   static_cast<H5OFFSET_TYPE>(0)};
    if (H5Sselect_hyperslab(memspaceVarresMD, H5S_SELECT_SET, mem_offset,
                            nullptr, countVarresMD, nullptr) < 0)
    {
        H5Sclose(memspaceVarresMD);
        if (poBlock != nullptr)
        {
            poBlock->DropLock();
            poBlock = nullptr;
        }
        return CE_Failure;
    }

    std::vector<BAGRefinementGrid> rgrids(static_cast<size_t>(nCountLowResY) *
                                          nCountLowResX);
    if (!(poGDS->ReadVarresMetadataValue(nLowResMinIdxY, nLowResMinIdxX,
                                         memspaceVarresMD, rgrids.data(),
                                         nCountLowResY, nCountLowResX)))
    {
        H5Sclose(memspaceVarresMD);
        if (poBlock != nullptr)
        {
            poBlock->DropLock();
            poBlock = nullptr;
        }
        return CE_Failure;
    }

    H5Sclose(memspaceVarresMD);

    CPLErr eErr = CE_None;
    for (int y = nLowResMinIdxY; y <= nLowResMaxIdxY; y++)
    {
        for (int x = nLowResMinIdxX; x <= nLowResMaxIdxX; x++)
        {
            const auto &rgrid = rgrids[(y - nLowResMinIdxY) * nCountLowResX +
                                       (x - nLowResMinIdxX)];
            if (rgrid.nWidth == 0)
            {
                continue;
            }
            const float gridRes = std::max(rgrid.fResX, rgrid.fResY);
            if (!(gridRes > poGDS->m_dfResFilterMin &&
                  gridRes <= poGDS->m_dfResFilterMax))
            {
                continue;
            }

            // Super grid bounding box with pixel-center convention
            const double dfMinX =
                poGDS->m_dfLowResMinX + x * dfLowResResX + rgrid.fSWX;
            const double dfMaxX =
                dfMinX + (rgrid.nWidth - 1) * static_cast<double>(rgrid.fResX);
            const double dfMinY =
                poGDS->m_dfLowResMinY + y * dfLowResResY + rgrid.fSWY;
            const double dfMaxY =
                dfMinY + (rgrid.nHeight - 1) * static_cast<double>(rgrid.fResY);

            // Intersection of super grid with block
            const double dfInterMinX = std::max(dfBlockMinX, dfMinX);
            const double dfInterMinY = std::max(dfBlockMinY, dfMinY);
            const double dfInterMaxX = std::min(dfBlockMaxX, dfMaxX);
            const double dfInterMaxY = std::min(dfBlockMaxY, dfMaxY);

            // Min/max indices in the super grid
            const int nMinSrcX = std::max(
                0, static_cast<int>((dfInterMinX - dfMinX) / rgrid.fResX));
            const int nMinSrcY = std::max(
                0, static_cast<int>((dfInterMinY - dfMinY) / rgrid.fResY));
            // Need to use ceil due to numerical imprecision
            const int nMaxSrcX =
                std::min(static_cast<int>(rgrid.nWidth) - 1,
                         static_cast<int>(
                             std::ceil((dfInterMaxX - dfMinX) / rgrid.fResX)));
            const int nMaxSrcY =
                std::min(static_cast<int>(rgrid.nHeight) - 1,
                         static_cast<int>(
                             std::ceil((dfInterMaxY - dfMinY) / rgrid.fResY)));
#ifdef DEBUG_VERBOSE
            CPLDebug(
                "BAG",
                "y = %d, x = %d, minx = %d, miny = %d, maxx = %d, maxy = %d", y,
                x, nMinSrcX, nMinSrcY, nMaxSrcX, nMaxSrcY);
#endif
            const double dfCstX = (dfMinX - dfBlockMinX) / poGDS->m_gt[1];
            const double dfMulX = rgrid.fResX / poGDS->m_gt[1];

            for (int super_y = nMinSrcY; super_y <= nMaxSrcY; super_y++)
            {
                const double dfSrcY =
                    dfMinY + super_y * static_cast<double>(rgrid.fResY);
                const int nTargetY = static_cast<int>(
                    std::floor((dfBlockMaxY - dfSrcY) / -poGDS->m_gt[5]));
                if (!(nTargetY >= 0 && nTargetY < nReqCountY))
                {
                    continue;
                }

                const unsigned nTargetIdxBase = nTargetY * nBlockXSize;
                const unsigned nRefinementIndexase =
                    rgrid.nIndex + super_y * rgrid.nWidth;

                for (int super_x = nMinSrcX; super_x <= nMaxSrcX; super_x++)
                {
                    /*
                    const double dfSrcX = dfMinX + super_x * rgrid.fResX;
                    const int nTargetX = static_cast<int>(std::floor(
                        (dfSrcX - dfBlockMinX) / poGDS->m_gt[1]));
                    */
                    const int nTargetX =
                        static_cast<int>(std::floor(dfCstX + super_x * dfMulX));
                    if (!(nTargetX >= 0 && nTargetX < nReqCountX))
                    {
                        continue;
                    }

                    const unsigned nTargetIdx = nTargetIdxBase + nTargetX;
                    if (poGDS->m_bMask)
                    {
                        static_cast<GByte *>(pImage)[nTargetIdx] = 255;
                        continue;
                    }

                    if (poGDS->m_ePopulation == BAGDataset::Population::COUNT)
                    {
                        static_cast<GUInt32 *>(pImage)[nTargetIdx]++;
                        continue;
                    }

                    CPLAssert(depthsPtr);
                    CPLAssert(uncrtPtr);

                    const unsigned nRefinementIndex =
                        nRefinementIndexase + super_x;
                    const auto *pafRefValues =
                        poGDS->GetRefinementValues(nRefinementIndex);
                    if (!pafRefValues)
                    {
                        eErr = CE_Failure;
                        goto end;
                    }

                    float depth = pafRefValues[0];
                    if (depth == fNoDataValue)
                    {
                        if (depthsPtr[nTargetIdx] == fNoSuperGridValue)
                        {
                            depthsPtr[nTargetIdx] = fNoDataValue;
                        }
                        continue;
                    }

                    if (poGDS->m_ePopulation == BAGDataset::Population::MEAN)
                    {

                        if (counts[nTargetIdx] == 0)
                        {
                            depthsPtr[nTargetIdx] = depth;
                        }
                        else
                        {
                            depthsPtr[nTargetIdx] += depth;
                        }
                        counts[nTargetIdx]++;

                        auto uncrt = pafRefValues[1];
                        auto &target_uncrt_ptr = uncrtPtr[nTargetIdx];
                        if (uncrt > target_uncrt_ptr ||
                            target_uncrt_ptr == fNoDataValue)
                        {
                            target_uncrt_ptr = uncrt;
                        }
                    }
                    else if ((poGDS->m_ePopulation ==
                                  BAGDataset::Population::MAX &&
                              depth > depthsPtr[nTargetIdx]) ||
                             (poGDS->m_ePopulation ==
                                  BAGDataset::Population::MIN &&
                              depth < depthsPtr[nTargetIdx]) ||
                             depthsPtr[nTargetIdx] == fNoDataValue ||
                             depthsPtr[nTargetIdx] == fNoSuperGridValue)
                    {
                        depthsPtr[nTargetIdx] = depth;
                        uncrtPtr[nTargetIdx] = pafRefValues[1];
                    }
                }
            }
        }
    }

    if (poGDS->m_ePopulation == BAGDataset::Population::MEAN && depthsPtr)
    {
        for (int i = 0; i < nBlockXSize * nBlockYSize; i++)
        {
            if (counts[i])
            {
                depthsPtr[i] /= counts[i];
            }
        }
    }

end:
    if (poBlock != nullptr)
    {
        poBlock->DropLock();
        poBlock = nullptr;
    }

    return eErr;
}

/************************************************************************/
/*                        BAGInterpolatedBand()                         */
/************************************************************************/

BAGInterpolatedBand::BAGInterpolatedBand(BAGDataset *poDSIn, int nBandIn,
                                         bool bHasNoData, float fNoDataValue,
                                         bool bInitializeMinMax)
{
    poDS = poDSIn;
    nBand = nBandIn;
    nRasterXSize = poDS->GetRasterXSize();
    nRasterYSize = poDS->GetRasterYSize();
    // Mostly for autotest purposes
    const int nBlockSize =
        std::max(1, atoi(CPLGetConfigOption("GDAL_BAG_BLOCK_SIZE", "256")));
    nBlockXSize = std::min(nBlockSize, poDS->GetRasterXSize());
    nBlockYSize = std::min(nBlockSize, poDS->GetRasterYSize());

    m_bHasNoData = true;
    m_fNoDataValue = bHasNoData ? fNoDataValue : fDEFAULT_NODATA;
    eDataType = GDT_Float32;
    GDALRasterBand::SetDescription(nBand == 1 ? "elevation" : "uncertainty");
    if (bInitializeMinMax)
    {
        InitializeMinMax();
    }
}

/************************************************************************/
/*                        ~BAGInterpolatedBand()                        */
/************************************************************************/

BAGInterpolatedBand::~BAGInterpolatedBand() = default;

/************************************************************************/
/*                           InitializeMinMax()                         */
/************************************************************************/

void BAGInterpolatedBand::InitializeMinMax()
{
    BAGDataset *poGDS = cpl::down_cast<BAGDataset *>(poDS);
    if (nBand == 1 &&
        GH5_FetchAttribute(poGDS->m_hVarresRefinements, "max_depth",
                           m_dfMaximum) &&
        GH5_FetchAttribute(poGDS->m_hVarresRefinements, "min_depth",
                           m_dfMinimum))
    {
        m_bMinMaxSet = true;
    }
    else if (nBand == 2 &&
             GH5_FetchAttribute(poGDS->m_hVarresRefinements, "max_uncrt",
                                m_dfMaximum) &&
             GH5_FetchAttribute(poGDS->m_hVarresRefinements, "min_uncrt",
                                m_dfMinimum))
    {
        m_bMinMaxSet = true;
    }
}

/************************************************************************/
/*                             GetMinimum()                             */
/************************************************************************/

double BAGInterpolatedBand::GetMinimum(int *pbSuccess)

{
    if (m_bMinMaxSet)
    {
        if (pbSuccess)
            *pbSuccess = TRUE;
        return m_dfMinimum;
    }

    return GDALRasterBand::GetMinimum(pbSuccess);
}

/************************************************************************/
/*                             GetMaximum()                             */
/************************************************************************/

double BAGInterpolatedBand::GetMaximum(int *pbSuccess)

{
    if (m_bMinMaxSet)
    {
        if (pbSuccess)
            *pbSuccess = TRUE;
        return m_dfMaximum;
    }

    return GDALRasterBand::GetMaximum(pbSuccess);
}

/************************************************************************/
/*                     BarycentricInterpolation()                       */
/************************************************************************/

static bool BarycentricInterpolation(double dfX, double dfY,
                                     const double adfX[3], const double adfY[3],
                                     double &dfCoord0, double &dfCoord1,
                                     double &dfCoord2)
{
    /* See https://en.wikipedia.org/wiki/Barycentric_coordinate_system */
    const double dfDenom = (adfY[1] - adfY[2]) * (adfX[0] - adfX[2]) +
                           (adfX[2] - adfX[1]) * (adfY[0] - adfY[2]);
    if (fabs(dfDenom) < 1e-5)
    {
        // Degenerate triangle
        return false;
    }
    const double dfTerm0X = adfY[1] - adfY[2];
    const double dfTerm0Y = adfX[2] - adfX[1];
    const double dfTerm1X = adfY[2] - adfY[0];
    const double dfTerm1Y = adfX[0] - adfX[2];
    const double dfDiffX = dfX - adfX[2];
    const double dfDiffY = dfY - adfY[2];
    dfCoord0 = (dfTerm0X * dfDiffX + dfTerm0Y * dfDiffY) / dfDenom;
    dfCoord1 = (dfTerm1X * dfDiffX + dfTerm1Y * dfDiffY) / dfDenom;
    dfCoord2 = 1 - dfCoord0 - dfCoord1;
    return (dfCoord0 >= 0 && dfCoord0 <= 1 && dfCoord1 >= 0 && dfCoord1 <= 1 &&
            dfCoord2 >= 0 && dfCoord2 <= 1);
}

/************************************************************************/
/*                               SQ()                                   */
/************************************************************************/

static inline double SQ(double v)
{
    return v * v;
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr BAGInterpolatedBand::IReadBlock(int nBlockXOff, int nBlockYOff,
                                       void *pImage)
{
    HDF5_GLOBAL_LOCK();

    BAGDataset *poGDS = cpl::down_cast<BAGDataset *>(poDS);
#ifdef DEBUG_VERBOSE
    CPLDebug(
        "BAG",
        "IReadBlock: nRasterXSize=%d, nBlockXOff=%d, nBlockYOff=%d, nBand=%d",
        nRasterXSize, nBlockXOff, nBlockYOff, nBand);
#endif

    float *depthsPtr = nullptr;
    float *uncrtPtr = nullptr;

    GDALRasterBlock *poBlock = nullptr;
    if (nBand == 1)
    {
        depthsPtr = static_cast<float *>(pImage);
        poBlock = poGDS->GetRasterBand(2)->GetLockedBlockRef(nBlockXOff,
                                                             nBlockYOff, TRUE);
        if (poBlock)
        {
            uncrtPtr = static_cast<float *>(poBlock->GetDataRef());
        }
    }
    else
    {
        uncrtPtr = static_cast<float *>(pImage);
        poBlock = poGDS->GetRasterBand(1)->GetLockedBlockRef(nBlockXOff,
                                                             nBlockYOff, TRUE);
        if (poBlock)
        {
            depthsPtr = static_cast<float *>(poBlock->GetDataRef());
        }
    }

    if (!depthsPtr || !uncrtPtr)
        return CE_Failure;

    GDALCopyWords(&m_fNoDataValue, GDT_Float32, 0, depthsPtr, GDT_Float32,
                  static_cast<int>(sizeof(float)), nBlockXSize * nBlockYSize);

    GDALCopyWords(&m_fNoDataValue, GDT_Float32, 0, uncrtPtr, GDT_Float32,
                  static_cast<int>(sizeof(float)), nBlockXSize * nBlockYSize);

    const int nReqCountX =
        std::min(nBlockXSize, nRasterXSize - nBlockXOff * nBlockXSize);
    const int nReqCountY =
        std::min(nBlockYSize, nRasterYSize - nBlockYOff * nBlockYSize);
    // Compute extent of block in georeferenced coordinates
    const double dfBlockMinX =
        poGDS->m_gt[0] + nBlockXOff * nBlockXSize * poGDS->m_gt[1];
    const double dfBlockMaxX = dfBlockMinX + nReqCountX * poGDS->m_gt[1];
    const double dfBlockMaxY =
        poGDS->m_gt[3] + nBlockYOff * nBlockYSize * poGDS->m_gt[5];
    const double dfBlockMinY = dfBlockMaxY + nReqCountY * poGDS->m_gt[5];

    // Compute min/max indices of intersecting supergrids (origin bottom-left)
    // We add a margin of (dfLowResResX, dfLowResResY) to be able to
    // properly interpolate if the edges of a GDAL block align with the edge
    // of a low-res cell
    const double dfLowResResX =
        (poGDS->m_dfLowResMaxX - poGDS->m_dfLowResMinX) / poGDS->m_nLowResWidth;
    const double dfLowResResY =
        (poGDS->m_dfLowResMaxY - poGDS->m_dfLowResMinY) /
        poGDS->m_nLowResHeight;
    const int nLowResMinIdxX = std::max(
        0,
        static_cast<int>((dfBlockMinX - poGDS->m_dfLowResMinX - dfLowResResX) /
                         dfLowResResX));
    const int nLowResMinIdxY = std::max(
        0,
        static_cast<int>((dfBlockMinY - poGDS->m_dfLowResMinY - dfLowResResY) /
                         dfLowResResY));
    const int nLowResMaxIdxX = std::min(
        poGDS->m_nLowResWidth - 1,
        static_cast<int>((dfBlockMaxX - poGDS->m_dfLowResMinX + dfLowResResX) /
                         dfLowResResX));
    const int nLowResMaxIdxY = std::min(
        poGDS->m_nLowResHeight - 1,
        static_cast<int>((dfBlockMaxY - poGDS->m_dfLowResMinY + dfLowResResY) /
                         dfLowResResY));
    if (nLowResMinIdxX >= poGDS->m_nLowResWidth ||
        nLowResMinIdxY >= poGDS->m_nLowResHeight || nLowResMaxIdxX < 0 ||
        nLowResMaxIdxY < 0)
    {
        return CE_None;
    }

    // Create memory space to receive the data.
    const int nCountLowResX = nLowResMaxIdxX - nLowResMinIdxX + 1;
    const int nCountLowResY = nLowResMaxIdxY - nLowResMinIdxY + 1;
    hsize_t countVarresMD[2] = {static_cast<hsize_t>(nCountLowResY),
                                static_cast<hsize_t>(nCountLowResX)};
    const hid_t memspaceVarresMD = H5Screate_simple(2, countVarresMD, nullptr);
    H5OFFSET_TYPE mem_offset[2] = {static_cast<H5OFFSET_TYPE>(0),
                                   static_cast<H5OFFSET_TYPE>(0)};
    if (H5Sselect_hyperslab(memspaceVarresMD, H5S_SELECT_SET, mem_offset,
                            nullptr, countVarresMD, nullptr) < 0)
    {
        H5Sclose(memspaceVarresMD);
        if (poBlock != nullptr)
        {
            poBlock->DropLock();
            poBlock = nullptr;
        }
        return CE_Failure;
    }

    std::vector<BAGRefinementGrid> rgrids(static_cast<size_t>(nCountLowResY) *
                                          nCountLowResX);
    if (!(poGDS->ReadVarresMetadataValue(nLowResMinIdxY, nLowResMinIdxX,
                                         memspaceVarresMD, rgrids.data(),
                                         nCountLowResY, nCountLowResX)))
    {
        H5Sclose(memspaceVarresMD);
        if (poBlock != nullptr)
        {
            poBlock->DropLock();
            poBlock = nullptr;
        }
        return CE_Failure;
    }

    H5Sclose(memspaceVarresMD);

    CPLErr eErr = CE_None;
    const double dfLowResMinX = poGDS->m_dfLowResMinX;
    const double dfLowResMinY = poGDS->m_dfLowResMinY;

    // georeferenced (X,Y) coordinates of the source nodes from the refinement
    //grids
    std::vector<double> adfX, adfY;
    // Depth and uncertainty values from the source nodes from the refinement
    //grids
    std::vector<float> afDepth, afUncrt;
    // Work variable to sort adfX, adfY, afDepth, afUncrt w.r.t to their
    // distance with the (dfX, dfY) point to be interpolated
    std::vector<int> anIndices;

    // Maximum distance of candidate source nodes to the point to be
    // interpolated
    const double dfMaxDistance = 0.5 * std::max(dfLowResResX, dfLowResResY);

    for (int y = 0; y < nReqCountY; ++y)
    {
        // Y georeference ordinate of the center of the cell to interpolate
        const double dfY = dfBlockMaxY + (y + 0.5) * poGDS->m_gt[5];
        // Y index of the corresponding refinement grid
        const int iYRefinedGrid =
            static_cast<int>(floor((dfY - dfLowResMinY) / dfLowResResY));
        if (iYRefinedGrid < nLowResMinIdxY || iYRefinedGrid > nLowResMaxIdxY)
            continue;
        for (int x = 0; x < nReqCountX; ++x)
        {
            // X georeference ordinate of the center of the cell to interpolate
            const double dfX = dfBlockMinX + (x + 0.5) * poGDS->m_gt[1];
            // X index of the corresponding refinement grid
            const int iXRefinedGrid =
                static_cast<int>((dfX - dfLowResMinX) / dfLowResResX);
            if (iXRefinedGrid < nLowResMinIdxX ||
                iXRefinedGrid > nLowResMaxIdxX)
                continue;

            // Correspond refinement grid
            const auto &rgrid =
                rgrids[(iYRefinedGrid - nLowResMinIdxY) * nCountLowResX +
                       (iXRefinedGrid - nLowResMinIdxX)];
            if (rgrid.nWidth == 0)
            {
                continue;
            }
            const float gridRes = std::max(rgrid.fResX, rgrid.fResY);
            if (!(gridRes > poGDS->m_dfResFilterMin &&
                  gridRes <= poGDS->m_dfResFilterMax))
            {
                continue;
            }

            // (dfMinRefinedX, dfMinRefinedY) is the georeferenced coordinate
            // of the bottom-left corner of the refinement grid
            const double dfMinRefinedX =
                dfLowResMinX + iXRefinedGrid * dfLowResResX + rgrid.fSWX;
            const double dfMinRefinedY =
                dfLowResMinY + iYRefinedGrid * dfLowResResY + rgrid.fSWY;

            // (iXInRefinedGrid, iYInRefinedGrid) is the index of the cell
            // within the refinement grid into which (dfX, dfY) falls into.
            const int iXInRefinedGrid =
                static_cast<int>(floor((dfX - dfMinRefinedX) / rgrid.fResX));
            const int iYInRefinedGrid =
                static_cast<int>(floor((dfY - dfMinRefinedY) / rgrid.fResY));

            if (iXInRefinedGrid >= 0 &&
                iXInRefinedGrid < static_cast<int>(rgrid.nWidth) - 1 &&
                iYInRefinedGrid >= 0 &&
                iYInRefinedGrid < static_cast<int>(rgrid.nHeight) - 1)
            {
                // The point to interpolate is fully within a single refinement
                // grid
                const unsigned nRefinementIndexBase =
                    rgrid.nIndex + iYInRefinedGrid * rgrid.nWidth +
                    iXInRefinedGrid;
                float d[2][2];
                float u[2][2];
                int nCountNoData = 0;

                // Load the depth and uncertainty values of the 4 nodes of
                // the refinement grid surrounding the center of the target
                // cell.
                for (int j = 0; j < 2; ++j)
                {
                    for (int i = 0; i < 2; ++i)
                    {
                        const unsigned nRefinementIndex =
                            nRefinementIndexBase + j * rgrid.nWidth + i;
                        const auto pafRefValues =
                            poGDS->GetRefinementValues(nRefinementIndex);
                        if (!pafRefValues)
                        {
                            eErr = CE_Failure;
                            goto end;
                        }
                        d[j][i] = pafRefValues[0];
                        u[j][i] = pafRefValues[1];
                        if (d[j][i] == m_fNoDataValue)
                            ++nCountNoData;
                    }
                }

                // Compute the relative distance of the point to be
                // interpolated compared to the closest bottom-left most node
                // of the refinement grid.
                // (alphaX,alphaY)=(0,0): point to be interpolated matches the
                // closest bottom-left most node
                // (alphaX,alphaY)=(1,1): point to be interpolated matches the
                // closest top-right most node
                const double alphaX =
                    fmod(dfX - dfMinRefinedX, rgrid.fResX) / rgrid.fResX;
                const double alphaY =
                    fmod(dfY - dfMinRefinedY, rgrid.fResY) / rgrid.fResY;
                if (nCountNoData == 0)
                {
                    // If the 4 nodes of the supergrid around the point
                    // to be interpolated are valid, do bilinear interpolation
#define BILINEAR_INTERP(var)                                                   \
    ((1 - alphaY) * ((1 - alphaX) * var[0][0] + alphaX * var[0][1]) +          \
     alphaY * ((1 - alphaX) * var[1][0] + alphaX * var[1][1]))

                    depthsPtr[y * nBlockXSize + x] =
                        static_cast<float>(BILINEAR_INTERP(d));
                    uncrtPtr[y * nBlockXSize + x] =
                        static_cast<float>(BILINEAR_INTERP(u));
                }
                else if (nCountNoData == 1)
                {
                    // If only one of the 4 nodes is at nodata, determine if the
                    // point to be interpolated is within the triangle formed
                    // by the remaining 3 valid nodes.
                    // If so, do barycentric interpolation
                    adfX.resize(3);
                    adfY.resize(3);
                    afDepth.resize(3);
                    afUncrt.resize(3);

                    int idx = 0;
                    for (int j = 0; j < 2; ++j)
                    {
                        for (int i = 0; i < 2; ++i)
                        {
                            if (d[j][i] != m_fNoDataValue)
                            {
                                CPLAssert(idx < 3);
                                adfX[idx] = i;
                                adfY[idx] = j;
                                afDepth[idx] = d[j][i];
                                afUncrt[idx] = u[j][i];
                                ++idx;
                            }
                        }
                    }
                    CPLAssert(idx == 3);
                    double dfCoord0;
                    double dfCoord1;
                    double dfCoord2;
                    if (BarycentricInterpolation(alphaX, alphaY, adfX.data(),
                                                 adfY.data(), dfCoord0,
                                                 dfCoord1, dfCoord2))
                    {
                        // Inside triangle
                        depthsPtr[y * nBlockXSize + x] = static_cast<float>(
                            dfCoord0 * afDepth[0] + dfCoord1 * afDepth[1] +
                            dfCoord2 * afDepth[2]);
                        uncrtPtr[y * nBlockXSize + x] = static_cast<float>(
                            dfCoord0 * afUncrt[0] + dfCoord1 * afUncrt[1] +
                            dfCoord2 * afUncrt[2]);
                    }
                }
                // else: 2 or more nodes invalid. Target point is set at nodata
            }
            else
            {
                // Point to interpolate is on an edge or corner of the
                // refinement grid
                adfX.clear();
                adfY.clear();
                afDepth.clear();
                afUncrt.clear();

                const auto LoadValues =
                    [this, dfX, dfY, &rgrids, nLowResMinIdxX, nLowResMinIdxY,
                     nCountLowResX, nCountLowResY, dfLowResMinX, dfLowResMinY,
                     dfLowResResX, dfLowResResY, &adfX, &adfY, &afDepth,
                     &afUncrt](int iX, int iY)
                {
                    LoadClosestRefinedNodes(
                        dfX, dfY, iX, iY, rgrids, nLowResMinIdxX,
                        nLowResMinIdxY, nCountLowResX, nCountLowResY,
                        dfLowResMinX, dfLowResMinY, dfLowResResX, dfLowResResY,
                        adfX, adfY, afDepth, afUncrt);
                };

                // Load values of the closest point to the point to be
                // interpolated in the current refinement grid
                LoadValues(iXRefinedGrid, iYRefinedGrid);

                const bool bAtLeft = iXInRefinedGrid < 0 && iXRefinedGrid > 0;
                const bool bAtRight =
                    iXInRefinedGrid >= static_cast<int>(rgrid.nWidth) - 1 &&
                    iXRefinedGrid + 1 < poGDS->m_nLowResWidth;
                const bool bAtBottom = iYInRefinedGrid < 0 && iYRefinedGrid > 0;
                const bool bAtTop =
                    iYInRefinedGrid >= static_cast<int>(rgrid.nHeight) - 1 &&
                    iYRefinedGrid + 1 < poGDS->m_nLowResHeight;
                const int nXShift = bAtLeft ? -1 : bAtRight ? 1 : 0;
                const int nYShift = bAtBottom ? -1 : bAtTop ? 1 : 0;

                // Load values of the closest point to the point to be
                // interpolated in the surrounding refinement grids.
                if (nXShift)
                {
                    LoadValues(iXRefinedGrid + nXShift, iYRefinedGrid);
                    if (nYShift)
                    {
                        LoadValues(iXRefinedGrid + nXShift,
                                   iYRefinedGrid + nYShift);
                    }
                }
                if (nYShift)
                {
                    LoadValues(iXRefinedGrid, iYRefinedGrid + nYShift);
                }

                // Filter out candidate source points that are away from
                // target point of more than dfMaxDistance
                size_t j = 0;
                for (size_t i = 0; i < adfX.size(); ++i)
                {
                    if (SQ(adfX[i] - dfX) + SQ(adfY[i] - dfY) <=
                        dfMaxDistance * dfMaxDistance)
                    {
                        adfX[j] = adfX[i];
                        adfY[j] = adfY[i];
                        afDepth[j] = afDepth[i];
                        afUncrt[j] = afUncrt[i];
                        ++j;
                    }
                }
                adfX.resize(j);
                adfY.resize(j);
                afDepth.resize(j);
                afUncrt.resize(j);

                // Now interpolate the target point from the source values
                bool bTryIDW = false;
                if (adfX.size() >= 3)
                {
                    // If there are at least 3 source nodes, sort them
                    // by increasing distance w.r.t the point to be interpolated
                    anIndices.clear();
                    for (size_t i = 0; i < adfX.size(); ++i)
                        anIndices.push_back(static_cast<int>(i));
                    // Sort nodes by increasing distance w.r.t (dfX, dfY)
                    std::sort(
                        anIndices.begin(), anIndices.end(),
                        [&adfX, &adfY, dfX, dfY](int i1, int i2)
                        {
                            return SQ(adfX[i1] - dfX) + SQ(adfY[i1] - dfY) <
                                   SQ(adfX[i2] - dfX) + SQ(adfY[i2] - dfY);
                        });
                    double adfXSorted[3];
                    double adfYSorted[3];
                    float afDepthSorted[3];
                    float afUncrtSorted[3];
                    for (int i = 0; i < 3; ++i)
                    {
                        adfXSorted[i] = adfX[anIndices[i]];
                        adfYSorted[i] = adfY[anIndices[i]];
                        afDepthSorted[i] = afDepth[anIndices[i]];
                        afUncrtSorted[i] = afUncrt[anIndices[i]];
                    }
                    double dfCoord0;
                    double dfCoord1;
                    double dfCoord2;
                    // Perform barycentric interpolation with those 3
                    // points if they are all valid, and if the point to
                    // be interpolated falls into it.
                    if (afDepthSorted[0] != m_fNoDataValue &&
                        afDepthSorted[1] != m_fNoDataValue &&
                        afDepthSorted[2] != m_fNoDataValue)
                    {
                        if (BarycentricInterpolation(dfX, dfY, adfXSorted,
                                                     adfYSorted, dfCoord0,
                                                     dfCoord1, dfCoord2))
                        {
                            // Inside triangle
                            depthsPtr[y * nBlockXSize + x] =
                                static_cast<float>(dfCoord0 * afDepthSorted[0] +
                                                   dfCoord1 * afDepthSorted[1] +
                                                   dfCoord2 * afDepthSorted[2]);
                            uncrtPtr[y * nBlockXSize + x] =
                                static_cast<float>(dfCoord0 * afUncrtSorted[0] +
                                                   dfCoord1 * afUncrtSorted[1] +
                                                   dfCoord2 * afUncrtSorted[2]);
                        }
                        else
                        {
                            // Attempt inverse distance weighting in the
                            // cases where the point to be interpolated doesn't
                            // fall within the triangle formed by the 3
                            // closes points.
                            bTryIDW = true;
                        }
                    }
                }
                if (bTryIDW)
                {
                    // Do inverse distance weighting a a fallback.

                    int nCountValid = 0;
                    double dfTotalDepth = 0;
                    double dfTotalUncrt = 0;
                    double dfTotalWeight = 0;
                    // Epsilon value to add to weights to avoid potential
                    // divergence to infinity if a source node is too close
                    // to the target point
                    const double EPS =
                        SQ(std::min(poGDS->m_gt[1], -poGDS->m_gt[5]) / 10);
                    for (size_t i = 0; i < adfX.size(); ++i)
                    {
                        if (afDepth[i] != m_fNoDataValue)
                        {
                            nCountValid++;
                            double dfSqrDistance =
                                SQ(adfX[i] - dfX) + SQ(adfY[i] - dfY) + EPS;
                            double dfWeight = 1. / dfSqrDistance;
                            dfTotalDepth += dfWeight * afDepth[i];
                            dfTotalUncrt += dfWeight * afUncrt[i];
                            dfTotalWeight += dfWeight;
                        }
                    }
                    if (nCountValid >= 3)
                    {
                        depthsPtr[y * nBlockXSize + x] =
                            static_cast<float>(dfTotalDepth / dfTotalWeight);
                        uncrtPtr[y * nBlockXSize + x] =
                            static_cast<float>(dfTotalUncrt / dfTotalWeight);
                    }
                }
            }
        }
    }
end:
    if (poBlock != nullptr)
    {
        poBlock->DropLock();
        poBlock = nullptr;
    }

    return eErr;
}

/************************************************************************/
/*                   LoadClosestRefinedNodes()                          */
/************************************************************************/

void BAGInterpolatedBand::LoadClosestRefinedNodes(
    double dfX, double dfY, int iXRefinedGrid, int iYRefinedGrid,
    const std::vector<BAGRefinementGrid> &rgrids, int nLowResMinIdxX,
    int nLowResMinIdxY, int nCountLowResX, int nCountLowResY,
    double dfLowResMinX, double dfLowResMinY, double dfLowResResX,
    double dfLowResResY, std::vector<double> &adfX, std::vector<double> &adfY,
    std::vector<float> &afDepth, std::vector<float> &afUncrt)
{
    BAGDataset *poGDS = cpl::down_cast<BAGDataset *>(poDS);
    CPLAssert(iXRefinedGrid >= nLowResMinIdxX);
    CPLAssert(iXRefinedGrid < nLowResMinIdxX + nCountLowResX);
    CPLAssert(iYRefinedGrid >= nLowResMinIdxY);
    CPLAssert(iYRefinedGrid < nLowResMinIdxY + nCountLowResY);
    CPL_IGNORE_RET_VAL(nCountLowResY);
    const auto &rgrid =
        rgrids[(iYRefinedGrid - nLowResMinIdxY) * nCountLowResX +
               (iXRefinedGrid - nLowResMinIdxX)];
    if (rgrid.nWidth == 0)
    {
        return;
    }
    const float gridRes = std::max(rgrid.fResX, rgrid.fResY);
    if (!(gridRes > poGDS->m_dfResFilterMin &&
          gridRes <= poGDS->m_dfResFilterMax))
    {
        return;
    }

    const double dfMinRefinedX =
        dfLowResMinX + iXRefinedGrid * dfLowResResX + rgrid.fSWX;
    const double dfMinRefinedY =
        dfLowResMinY + iYRefinedGrid * dfLowResResY + rgrid.fSWY;
    const int iXInRefinedGrid =
        static_cast<int>(floor((dfX - dfMinRefinedX) / rgrid.fResX));
    const int iYInRefinedGrid =
        static_cast<int>(floor((dfY - dfMinRefinedY) / rgrid.fResY));

    const auto LoadValues = [poGDS, dfMinRefinedX, dfMinRefinedY, &rgrid, &adfX,
                             &adfY, &afDepth,
                             &afUncrt](int iXAdjusted, int iYAdjusted)
    {
        const unsigned nRefinementIndex =
            rgrid.nIndex + iYAdjusted * rgrid.nWidth + iXAdjusted;
        const auto pafRefValues = poGDS->GetRefinementValues(nRefinementIndex);
        if (pafRefValues)
        {
            adfX.push_back(dfMinRefinedX +
                           iXAdjusted * static_cast<double>(rgrid.fResX));
            adfY.push_back(dfMinRefinedY +
                           iYAdjusted * static_cast<double>(rgrid.fResY));
            afDepth.push_back(pafRefValues[0]);
            afUncrt.push_back(pafRefValues[1]);
        }
    };

    const int iXAdjusted = std::max(
        0, std::min(iXInRefinedGrid, static_cast<int>(rgrid.nWidth) - 1));
    const int iYAdjusted = std::max(
        0, std::min(iYInRefinedGrid, static_cast<int>(rgrid.nHeight) - 1));
    LoadValues(iXAdjusted, iYAdjusted);
    if (iYInRefinedGrid >= 0 &&
        iYInRefinedGrid < static_cast<int>(rgrid.nHeight) - 1)
        LoadValues(iXAdjusted, iYInRefinedGrid + 1);
    if (iXInRefinedGrid >= 0 &&
        iXInRefinedGrid < static_cast<int>(rgrid.nWidth) - 1)
        LoadValues(iXInRefinedGrid + 1, iYAdjusted);
}

/************************************************************************/
/* ==================================================================== */
/*                        BAGGeorefMDBandBase                           */
/* ==================================================================== */
/************************************************************************/

class BAGGeorefMDBandBase CPL_NON_FINAL : public GDALPamRasterBand
{
  protected:
    std::shared_ptr<GDALMDArray> m_poKeys;
    std::unique_ptr<GDALRasterBand> m_poElevBand;
    std::unique_ptr<GDALRasterAttributeTable> m_poRAT{};

    BAGGeorefMDBandBase(const std::shared_ptr<GDALMDArray> &poValues,
                        const std::shared_ptr<GDALMDArray> &poKeys,
                        GDALRasterBand *poElevBand)
        : m_poKeys(poKeys), m_poElevBand(poElevBand),
          m_poRAT(HDF5CreateRAT(poValues, false))
    {
    }

    CPLErr IReadBlockFromElevBand(int nBlockXOff, int nBlockYOff, void *pImage);

  public:
    GDALRasterAttributeTable *GetDefaultRAT() override
    {
        return m_poRAT.get();
    }

    double GetNoDataValue(int *pbSuccess) override;
};

/************************************************************************/
/*                         GetNoDataValue()                             */
/************************************************************************/

double BAGGeorefMDBandBase::GetNoDataValue(int *pbSuccess)
{
    if (pbSuccess)
        *pbSuccess = TRUE;
    return 0;
}

/************************************************************************/
/*                   IReadBlockFromElevBand()                           */
/************************************************************************/

CPLErr BAGGeorefMDBandBase::IReadBlockFromElevBand(int nBlockXOff,
                                                   int nBlockYOff, void *pImage)
{
    std::vector<float> afData(static_cast<size_t>(nBlockXSize) * nBlockYSize);
    const int nXOff = nBlockXOff * nBlockXSize;
    const int nReqXSize = std::min(nBlockXSize, nRasterXSize - nXOff);
    const int nYOff = nBlockYOff * nBlockYSize;
    const int nReqYSize = std::min(nBlockYSize, nRasterYSize - nYOff);
    if (m_poElevBand->RasterIO(GF_Read, nXOff, nYOff, nReqXSize, nReqYSize,
                               &afData[0], nReqXSize, nReqYSize, GDT_Float32, 4,
                               nBlockXSize * 4, nullptr) != CE_None)
    {
        return CE_Failure;
    }
    int bHasNoData = FALSE;
    const float fNoDataValue =
        static_cast<float>(m_poElevBand->GetNoDataValue(&bHasNoData));
    GByte *const pbyImage = static_cast<GByte *>(pImage);
    for (int y = 0; y < nReqYSize; y++)
    {
        for (int x = 0; x < nReqXSize; x++)
        {
            pbyImage[y * nBlockXSize + x] =
                (afData[y * nBlockXSize + x] == fNoDataValue ||
                 std::isnan(afData[y * nBlockXSize + x]))
                    ? 0
                    : 1;
        }
    }

    return CE_None;
}

/************************************************************************/
/* ==================================================================== */
/*                             BAGGeorefMDBand                          */
/* ==================================================================== */
/************************************************************************/

class BAGGeorefMDBand final : public BAGGeorefMDBandBase
{
  public:
    BAGGeorefMDBand(const std::shared_ptr<GDALMDArray> &poValues,
                    const std::shared_ptr<GDALMDArray> &poKeys,
                    GDALRasterBand *poElevBand);

    CPLErr IReadBlock(int, int, void *) override;
};

/************************************************************************/
/*                         BAGGeorefMDBand()                            */
/************************************************************************/

BAGGeorefMDBand::BAGGeorefMDBand(const std::shared_ptr<GDALMDArray> &poValues,
                                 const std::shared_ptr<GDALMDArray> &poKeys,
                                 GDALRasterBand *poElevBand)
    : BAGGeorefMDBandBase(poValues, poKeys, poElevBand)
{
    nRasterXSize = poElevBand->GetXSize();
    nRasterYSize = poElevBand->GetYSize();
    if (poKeys)
    {
        auto blockSize = poKeys->GetBlockSize();
        CPLAssert(blockSize.size() == 2);
        nBlockYSize = static_cast<int>(blockSize[0]);
        nBlockXSize = static_cast<int>(blockSize[1]);
        eDataType = poKeys->GetDataType().GetNumericDataType();
        if (nBlockXSize == 0 || nBlockYSize == 0)
        {
            nBlockXSize = nRasterXSize;
            nBlockYSize = 1;
        }
    }
    else
    {
        eDataType = GDT_Byte;
        m_poElevBand->GetBlockSize(&nBlockXSize, &nBlockYSize);
    }

    // For testing purposes
    const char *pszBlockXSize =
        CPLGetConfigOption("BAG_GEOREF_MD_BLOCKXSIZE", nullptr);
    if (pszBlockXSize)
        nBlockXSize = atoi(pszBlockXSize);
    const char *pszBlockYSize =
        CPLGetConfigOption("BAG_GEOREF_MD_BLOCKYSIZE", nullptr);
    if (pszBlockYSize)
        nBlockYSize = atoi(pszBlockYSize);
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr BAGGeorefMDBand::IReadBlock(int nBlockXOff, int nBlockYOff, void *pImage)
{
    HDF5_GLOBAL_LOCK();

    if (m_poKeys)
    {
        const GUInt64 arrayStartIdx[2] = {
            static_cast<GUInt64>(
                std::max(0, nRasterYSize - (nBlockYOff + 1) * nBlockYSize)),
            static_cast<GUInt64>(nBlockXOff) * nBlockXSize};
        size_t count[2] = {
            std::min(static_cast<size_t>(nBlockYSize),
                     static_cast<size_t>(GetYSize() - arrayStartIdx[0])),
            std::min(static_cast<size_t>(nBlockXSize),
                     static_cast<size_t>(GetXSize() - arrayStartIdx[1]))};
        if (nRasterYSize - (nBlockYOff + 1) * nBlockYSize < 0)
        {
            count[0] += (nRasterYSize - (nBlockYOff + 1) * nBlockYSize);
        }
        const GInt64 arrayStep[2] = {1, 1};
        const GPtrDiff_t bufferStride[2] = {nBlockXSize, 1};

        if (!m_poKeys->Read(arrayStartIdx, count, arrayStep, bufferStride,
                            m_poKeys->GetDataType(), pImage))
        {
            return CE_Failure;
        }

        // Y flip the data.
        const int nLinesToFlip = static_cast<int>(count[0]);
        if (nLinesToFlip > 1)
        {
            const int nLineSize =
                GDALGetDataTypeSizeBytes(eDataType) * nBlockXSize;
            GByte *const pabyTemp = static_cast<GByte *>(CPLMalloc(nLineSize));
            GByte *const pbyImage = static_cast<GByte *>(pImage);

            for (int iY = 0; iY < nLinesToFlip / 2; iY++)
            {
                memcpy(pabyTemp, pbyImage + iY * nLineSize, nLineSize);
                memcpy(pbyImage + iY * nLineSize,
                       pbyImage + (nLinesToFlip - iY - 1) * nLineSize,
                       nLineSize);
                memcpy(pbyImage + (nLinesToFlip - iY - 1) * nLineSize, pabyTemp,
                       nLineSize);
            }

            CPLFree(pabyTemp);
        }
        return CE_None;
    }
    else
    {
        return IReadBlockFromElevBand(nBlockXOff, nBlockYOff, pImage);
    }
}

/************************************************************************/
/* ==================================================================== */
/*                    BAGGeorefMDSuperGridBand                          */
/* ==================================================================== */
/************************************************************************/

class BAGGeorefMDSuperGridBand final : public BAGGeorefMDBandBase
{
  public:
    BAGGeorefMDSuperGridBand(const std::shared_ptr<GDALMDArray> &poValues,
                             const std::shared_ptr<GDALMDArray> &poKeys,
                             GDALRasterBand *poElevBand);

    CPLErr IReadBlock(int, int, void *) override;
};

/************************************************************************/
/*                     BAGGeorefMDSuperGridBand()                       */
/************************************************************************/

BAGGeorefMDSuperGridBand::BAGGeorefMDSuperGridBand(
    const std::shared_ptr<GDALMDArray> &poValues,
    const std::shared_ptr<GDALMDArray> &poKeys, GDALRasterBand *poElevBand)
    : BAGGeorefMDBandBase(poValues, poKeys, poElevBand)
{
    nRasterXSize = poElevBand->GetXSize();
    nRasterYSize = poElevBand->GetYSize();
    if (poKeys)
    {
        nBlockYSize = 1;
        nBlockXSize = nRasterXSize;
        eDataType = poKeys->GetDataType().GetNumericDataType();
    }
    else
    {
        eDataType = GDT_Byte;
        m_poElevBand->GetBlockSize(&nBlockXSize, &nBlockYSize);
    }
}

/************************************************************************/
/*                             IReadBlock()                             */
/************************************************************************/

CPLErr BAGGeorefMDSuperGridBand::IReadBlock(int nBlockXOff, int nBlockYOff,
                                            void *pImage)
{
    BAGDataset *poGDS = cpl::down_cast<BAGDataset *>(poDS);
    if (m_poKeys)
    {
        const GUInt64 arrayStartIdx[2] = {
            0, poGDS->m_nSuperGridRefinementStartIndex +
                   static_cast<GUInt64>(nRasterYSize - 1 - nBlockYOff) *
                       nBlockXSize};
        size_t count[2] = {1, static_cast<size_t>(nBlockXSize)};
        const GInt64 arrayStep[2] = {1, 1};
        const GPtrDiff_t bufferStride[2] = {nBlockXSize, 1};

        if (!m_poKeys->Read(arrayStartIdx, count, arrayStep, bufferStride,
                            m_poKeys->GetDataType(), pImage))
        {
            return CE_Failure;
        }
        return CE_None;
    }
    else
    {
        return IReadBlockFromElevBand(nBlockXOff, nBlockYOff, pImage);
    }
}

/************************************************************************/
/* ==================================================================== */
/*                              BAGDataset                              */
/* ==================================================================== */
/************************************************************************/

/************************************************************************/
/*                             BAGDataset()                             */
/************************************************************************/

BAGDataset::BAGDataset()
{
    m_oSRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
}

BAGDataset::BAGDataset(BAGDataset *poParentDS, int nOvrFactor)
{
    const int nXSize = poParentDS->nRasterXSize / nOvrFactor;
    const int nYSize = poParentDS->nRasterYSize / nOvrFactor;
    InitOverviewDS(poParentDS, nXSize, nYSize);
}

BAGDataset::BAGDataset(BAGDataset *poParentDS, int nXSize, int nYSize)
{
    InitOverviewDS(poParentDS, nXSize, nYSize);
}

void BAGDataset::InitOverviewDS(BAGDataset *poParentDS, int nXSize, int nYSize)
{
    m_ePopulation = poParentDS->m_ePopulation;
    m_bMask = poParentDS->m_bMask;
    m_bIsChild = true;
    // m_apoOverviewDS
    m_poSharedResources = poParentDS->m_poSharedResources;
    m_poRootGroup = poParentDS->m_poRootGroup;
    m_oSRS = poParentDS->m_oSRS;
    nRasterXSize = nXSize;
    nRasterYSize = nYSize;
    m_gt[0] = poParentDS->m_gt[0];
    m_gt[1] = poParentDS->m_gt[1] * poParentDS->nRasterXSize / nRasterXSize;
    m_gt[2] = poParentDS->m_gt[2];
    m_gt[3] = poParentDS->m_gt[3];
    m_gt[4] = poParentDS->m_gt[4];
    m_gt[5] = poParentDS->m_gt[5] * poParentDS->nRasterYSize / nRasterYSize;
    m_nLowResWidth = poParentDS->m_nLowResWidth;
    m_nLowResHeight = poParentDS->m_nLowResHeight;
    m_dfLowResMinX = poParentDS->m_dfLowResMinX;
    m_dfLowResMinY = poParentDS->m_dfLowResMinY;
    m_dfLowResMaxX = poParentDS->m_dfLowResMaxX;
    m_dfLowResMaxY = poParentDS->m_dfLowResMaxY;
    // char        *pszXMLMetadata = nullptr;
    // char        *apszMDList[2]{};
    m_nChunkXSizeVarresMD = poParentDS->m_nChunkXSizeVarresMD;
    m_nChunkYSizeVarresMD = poParentDS->m_nChunkYSizeVarresMD;
    m_nChunkSizeVarresRefinement = poParentDS->m_nChunkSizeVarresRefinement;

    m_hVarresMetadata = poParentDS->m_hVarresMetadata;
    m_hVarresMetadataDataType = poParentDS->m_hVarresMetadataDataType;
    m_hVarresMetadataDataspace = poParentDS->m_hVarresMetadataDataspace;
    m_hVarresMetadataNative = poParentDS->m_hVarresMetadataNative;
    // m_oMapRefinemendGrids;

    // m_aosSubdatasets;

    m_hVarresRefinements = poParentDS->m_hVarresRefinements;
    m_hVarresRefinementsDataType = poParentDS->m_hVarresRefinementsDataType;
    m_hVarresRefinementsDataspace = poParentDS->m_hVarresRefinementsDataspace;
    m_hVarresRefinementsNative = poParentDS->m_hVarresRefinementsNative;
    m_nRefinementsSize = poParentDS->m_nRefinementsSize;

    m_nSuperGridRefinementStartIndex =
        poParentDS->m_nSuperGridRefinementStartIndex;
    m_dfResFilterMin = poParentDS->m_dfResFilterMin;
    m_dfResFilterMax = poParentDS->m_dfResFilterMax;

    if (poParentDS->GetRasterCount() > 1)
    {
        GDALDataset::SetMetadataItem("INTERLEAVE", "PIXEL", "IMAGE_STRUCTURE");
    }
}

/************************************************************************/
/*                            ~BAGDataset()                             */
/************************************************************************/
BAGDataset::~BAGDataset()
{
    if (eAccess == GA_Update && nBands == 1)
    {
        auto poFirstBand = cpl::down_cast<BAGRasterBand *>(GetRasterBand(1));
        auto poBand = new BAGRasterBand(this, 2);
        poBand->nBlockXSize = poFirstBand->nBlockXSize;
        poBand->nBlockYSize = poFirstBand->nBlockYSize;
        poBand->eDataType = GDT_Float32;
        poBand->m_bHasNoData = true;
        poBand->m_fNoDataValue = poFirstBand->m_fNoDataValue;
        SetBand(2, poBand);
    }

    if (eAccess == GA_Update)
    {
        for (int i = 0; i < nBands; i++)
        {
            cpl::down_cast<BAGRasterBand *>(GetRasterBand(i + 1))
                ->CreateDatasetIfNeeded();
        }
    }

    FlushCache(true);

    m_apoOverviewDS.clear();
    if (!m_bIsChild)
    {
        if (m_hVarresMetadataDataType >= 0)
            H5Tclose(m_hVarresMetadataDataType);

        if (m_hVarresMetadataDataspace >= 0)
            H5Sclose(m_hVarresMetadataDataspace);

        if (m_hVarresMetadataNative >= 0)
            H5Tclose(m_hVarresMetadataNative);

        if (m_hVarresMetadata >= 0)
            H5Dclose(m_hVarresMetadata);

        if (m_hVarresRefinementsDataType >= 0)
            H5Tclose(m_hVarresRefinementsDataType);

        if (m_hVarresRefinementsDataspace >= 0)
            H5Sclose(m_hVarresRefinementsDataspace);

        if (m_hVarresRefinementsNative >= 0)
            H5Tclose(m_hVarresRefinementsNative);

        if (m_hVarresRefinements >= 0)
            H5Dclose(m_hVarresRefinements);

        CPLFree(pszXMLMetadata);
    }
}

/************************************************************************/
/*                          GH5DopenNoWarning()                         */
/************************************************************************/

static hid_t GH5DopenNoWarning(hid_t hHDF5, const char *pszDatasetName)
{
    hid_t hDataset;
#ifdef HAVE_GCC_WARNING_ZERO_AS_NULL_POINTER_CONSTANT
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wzero-as-null-pointer-constant"
#endif
    H5E_BEGIN_TRY
    {
        hDataset = H5Dopen(hHDF5, pszDatasetName);
    }
    H5E_END_TRY;

#ifdef HAVE_GCC_WARNING_ZERO_AS_NULL_POINTER_CONSTANT
#pragma GCC diagnostic pop
#endif
    return hDataset;
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

GDALDataset *BAGDataset::Open(GDALOpenInfo *poOpenInfo)

{
    // Confirm that this appears to be a BAG file.
    if (!BAGDatasetIdentify(poOpenInfo))
        return nullptr;

    HDF5_GLOBAL_LOCK();

    if (poOpenInfo->nOpenFlags & GDAL_OF_MULTIDIM_RASTER)
    {
        return HDF5Dataset::OpenMultiDim(poOpenInfo);
    }

    // Confirm the requested access is supported.
    if (poOpenInfo->eAccess == GA_Update)
    {
        ReportUpdateNotSupportedByDriver("BAG");
        return nullptr;
    }

    bool bOpenSuperGrid = false;
    int nX = -1;
    int nY = -1;
    CPLString osFilename(poOpenInfo->pszFilename);
    CPLString osGeorefMetadataLayer;
    bool bIsSubdataset = false;
    if (STARTS_WITH(poOpenInfo->pszFilename, "BAG:"))
    {
        bIsSubdataset = true;
        char **papszTokens =
            CSLTokenizeString2(poOpenInfo->pszFilename, ":",
                               CSLT_HONOURSTRINGS | CSLT_PRESERVEESCAPES);

        if (CSLCount(papszTokens) == 3 &&
            EQUAL(papszTokens[2], "bathymetry_coverage"))
        {
            osFilename = papszTokens[1];
        }
        else if (CSLCount(papszTokens) == 4 &&
                 EQUAL(papszTokens[2], "georef_metadata"))
        {
            osFilename = papszTokens[1];
            osGeorefMetadataLayer = papszTokens[3];
        }
        else if (CSLCount(papszTokens) == 6 &&
                 EQUAL(papszTokens[2], "georef_metadata"))
        {
            osFilename = papszTokens[1];
            osGeorefMetadataLayer = papszTokens[3];
            bOpenSuperGrid = true;
            nY = atoi(papszTokens[4]);
            nX = atoi(papszTokens[5]);
        }
        else
        {
            if (CSLCount(papszTokens) != 5)
            {
                CSLDestroy(papszTokens);
                return nullptr;
            }
            bOpenSuperGrid = true;
            osFilename = papszTokens[1];
            nY = atoi(papszTokens[3]);
            nX = atoi(papszTokens[4]);
        }
        if (bOpenSuperGrid)
        {

            if (CSLFetchNameValue(poOpenInfo->papszOpenOptions, "MINX") !=
                    nullptr ||
                CSLFetchNameValue(poOpenInfo->papszOpenOptions, "MINY") !=
                    nullptr ||
                CSLFetchNameValue(poOpenInfo->papszOpenOptions, "MAXX") !=
                    nullptr ||
                CSLFetchNameValue(poOpenInfo->papszOpenOptions, "MAXY") !=
                    nullptr ||
                CSLFetchNameValue(poOpenInfo->papszOpenOptions,
                                  "SUPERGRIDS_INDICES") != nullptr)
            {
                CPLError(
                    CE_Warning, CPLE_AppDefined,
                    "Open options MINX/MINY/MAXX/MAXY/SUPERGRIDS_INDICES are "
                    "ignored when opening a supergrid");
            }
        }
        CSLDestroy(papszTokens);
    }

    // Open the file as an HDF5 file.
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    H5Pset_driver(fapl, HDF5GetFileDriver(), nullptr);
    hid_t hHDF5 = H5Fopen(
        osFilename,
        /*poOpenInfo->eAccess == GA_Update ? H5F_ACC_RDWR : */ H5F_ACC_RDONLY,
        fapl);
    H5Pclose(fapl);
    if (hHDF5 < 0)
        return nullptr;

    // Confirm it is a BAG dataset by checking for the
    // BAG_root/Bag Version attribute.
    const hid_t hBagRoot = H5Gopen(hHDF5, "/BAG_root");
    const hid_t hVersion =
        hBagRoot >= 0 ? H5Aopen_name(hBagRoot, "Bag Version") : -1;

    if (hVersion < 0)
    {
        if (hBagRoot >= 0)
            H5Gclose(hBagRoot);
        H5Fclose(hHDF5);
        return nullptr;
    }
    H5Aclose(hVersion);

    auto poSharedResources = GDAL::HDF5SharedResources::Create(osFilename);
    poSharedResources->m_hHDF5 = hHDF5;

    auto poRootGroup = HDF5Dataset::OpenGroup(poSharedResources);
    if (poRootGroup == nullptr)
        return nullptr;

    // Create a corresponding dataset.
    BAGDataset *const poDS = new BAGDataset();

    poDS->eAccess = poOpenInfo->eAccess;
    poDS->m_poRootGroup = std::move(poRootGroup);
    poDS->m_poSharedResources = std::move(poSharedResources);

    // Extract version as metadata.
    CPLString osVersion;

    if (GH5_FetchAttribute(hBagRoot, "Bag Version", osVersion))
        poDS->GDALDataset::SetMetadataItem("BagVersion", osVersion);

    H5Gclose(hBagRoot);

    CPLString osSubDsName;
    if (poOpenInfo->nOpenFlags & GDAL_OF_RASTER)
    {
        if (poDS->OpenRaster(poOpenInfo, osFilename, bOpenSuperGrid, nX, nY,
                             bIsSubdataset, osGeorefMetadataLayer, osSubDsName))
        {
            if (!osSubDsName.empty())
            {
                delete poDS;
                GDALOpenInfo oOpenInfo(osSubDsName, GA_ReadOnly);
                oOpenInfo.nOpenFlags = poOpenInfo->nOpenFlags;
                return Open(&oOpenInfo);
            }
        }
        else
        {
            delete poDS;
            return nullptr;
        }
    }

    if (poOpenInfo->nOpenFlags & GDAL_OF_VECTOR)
    {
        if (!poDS->OpenVector() &&
            (poOpenInfo->nOpenFlags & GDAL_OF_RASTER) == 0)
        {
            delete poDS;
            return nullptr;
        }
    }

    return poDS;
}

/************************************************************************/
/*                          OpenRaster()                                */
/************************************************************************/

bool BAGDataset::OpenRaster(GDALOpenInfo *poOpenInfo,
                            const CPLString &osFilename, bool bOpenSuperGrid,
                            int nX, int nY, bool bIsSubdataset,
                            const CPLString &osGeorefMetadataLayer,
                            CPLString &outOsSubDsName)
{
    const char *pszMode =
        CSLFetchNameValueDef(poOpenInfo->papszOpenOptions, "MODE", "AUTO");
    const bool bLowResGrid = EQUAL(pszMode, "LOW_RES_GRID");
    if (bLowResGrid &&
        (CSLFetchNameValue(poOpenInfo->papszOpenOptions, "MINX") != nullptr ||
         CSLFetchNameValue(poOpenInfo->papszOpenOptions, "MINY") != nullptr ||
         CSLFetchNameValue(poOpenInfo->papszOpenOptions, "MAXX") != nullptr ||
         CSLFetchNameValue(poOpenInfo->papszOpenOptions, "MAXY") != nullptr ||
         CSLFetchNameValue(poOpenInfo->papszOpenOptions,
                           "SUPERGRIDS_INDICES") != nullptr))
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Open options MINX/MINY/MAXX/MAXY/SUPERGRIDS_INDICES are "
                 "ignored when opening the low resolution grid");
    }

    const bool bListSubDS =
        !bLowResGrid &&
        (EQUAL(pszMode, "LIST_SUPERGRIDS") ||
         CSLFetchNameValue(poOpenInfo->papszOpenOptions, "MINX") != nullptr ||
         CSLFetchNameValue(poOpenInfo->papszOpenOptions, "MINY") != nullptr ||
         CSLFetchNameValue(poOpenInfo->papszOpenOptions, "MAXX") != nullptr ||
         CSLFetchNameValue(poOpenInfo->papszOpenOptions, "MAXY") != nullptr ||
         CSLFetchNameValue(poOpenInfo->papszOpenOptions,
                           "SUPERGRIDS_INDICES") != nullptr);
    const bool bResampledGrid = EQUAL(pszMode, "RESAMPLED_GRID");
    const bool bInterpolate = EQUAL(pszMode, "INTERPOLATED");

    const char *pszNoDataValue =
        CSLFetchNameValue(poOpenInfo->papszOpenOptions, "NODATA_VALUE");
    bool bHasNoData = pszNoDataValue != nullptr;
    float fNoDataValue =
        bHasNoData ? static_cast<float>(CPLAtof(pszNoDataValue)) : 0.0f;

    // Fetch the elevation dataset and attach as a band.
    int nNextBand = 1;

    BAGRasterBand *poElevBand = new BAGRasterBand(this, nNextBand);

    {
        const hid_t hElevation =
            H5Dopen(GetHDF5Handle(), "/BAG_root/elevation");
        if (hElevation < 0 || !poElevBand->Initialize(hElevation, "elevation"))
        {
            delete poElevBand;
            return false;
        }
    }

    m_nLowResWidth = poElevBand->nRasterXSize;
    m_nLowResHeight = poElevBand->nRasterYSize;

    if (bOpenSuperGrid || bListSubDS || bResampledGrid || bInterpolate)
    {
        if (!bHasNoData)
        {
            int nHasNoData = FALSE;
            double dfNoData = poElevBand->GetNoDataValue(&nHasNoData);
            if (nHasNoData)
            {
                bHasNoData = true;
                fNoDataValue = static_cast<float>(dfNoData);
            }
        }
        delete poElevBand;
        nRasterXSize = 0;
        nRasterYSize = 0;
    }
    else if (!osGeorefMetadataLayer.empty())
    {
        auto poGeoref_metadataLayer = m_poRootGroup->OpenGroupFromFullname(
            "/BAG_root/georef_metadata/" + osGeorefMetadataLayer, nullptr);
        if (poGeoref_metadataLayer == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot find georef_metadata layer %s",
                     osGeorefMetadataLayer.c_str());
            delete poElevBand;
            return false;
        }

        auto poKeys = poGeoref_metadataLayer->OpenMDArray("keys");
        if (poKeys != nullptr)
        {
            auto poDims = poKeys->GetDimensions();
            if (poDims.size() != 2 ||
                poDims[0]->GetSize() !=
                    static_cast<size_t>(poElevBand->nRasterYSize) ||
                poDims[1]->GetSize() !=
                    static_cast<size_t>(poElevBand->nRasterXSize))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Wrong dimensions for %s/keys",
                         osGeorefMetadataLayer.c_str());
                delete poElevBand;
                return false;
            }
            if (poKeys->GetDataType().GetClass() != GEDTC_NUMERIC ||
                !GDALDataTypeIsInteger(
                    poKeys->GetDataType().GetNumericDataType()))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Only integer data type supported for %s/keys",
                         osGeorefMetadataLayer.c_str());
                delete poElevBand;
                return false;
            }
        }

        auto poValues = poGeoref_metadataLayer->OpenMDArray("values");
        if (poValues == nullptr)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot find array values of georef_metadata layer %s",
                     osGeorefMetadataLayer.c_str());
            delete poElevBand;
            return false;
        }
        if (poValues->GetDimensionCount() != 1)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Wrong dimensions for %s/values",
                     osGeorefMetadataLayer.c_str());
            delete poElevBand;
            return false;
        }
        if (poValues->GetDataType().GetClass() != GEDTC_COMPOUND)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Only compound data type supported for %s/values",
                     osGeorefMetadataLayer.c_str());
            delete poElevBand;
            return false;
        }

        nRasterXSize = poElevBand->nRasterXSize;
        nRasterYSize = poElevBand->nRasterYSize;
        SetBand(1, new BAGGeorefMDBand(poValues, poKeys, poElevBand));
    }
    else
    {
        nRasterXSize = poElevBand->nRasterXSize;
        nRasterYSize = poElevBand->nRasterYSize;

        SetBand(nNextBand++, poElevBand);

        // Try to do the same for the uncertainty band.
        const hid_t hUncertainty =
            H5Dopen(GetHDF5Handle(), "/BAG_root/uncertainty");
        BAGRasterBand *poUBand = new BAGRasterBand(this, nNextBand);

        if (hUncertainty >= 0 &&
            poUBand->Initialize(hUncertainty, "uncertainty"))
        {
            SetBand(nNextBand++, poUBand);
        }
        else
        {
            delete poUBand;
        }

        // Load other root datasets (such as nominal_elevation)
        auto poBAG_root = m_poRootGroup->OpenGroup("BAG_root", nullptr);
        if (poBAG_root)
        {
            const auto arrayNames = poBAG_root->GetMDArrayNames(nullptr);
            for (const auto &arrayName : arrayNames)
            {
                if (arrayName != "elevation" && arrayName != "uncertainty")
                {
                    auto poArray = poBAG_root->OpenMDArray(arrayName, nullptr);
                    if (poArray && poArray->GetDimensions().size() == 2 &&
                        poArray->GetDimensions()[0]->GetSize() ==
                            static_cast<unsigned>(nRasterYSize) &&
                        poArray->GetDimensions()[1]->GetSize() ==
                            static_cast<unsigned>(nRasterXSize) &&
                        poArray->GetDataType().GetClass() == GEDTC_NUMERIC)
                    {
                        hid_t hBandId = GH5DopenNoWarning(
                            GetHDF5Handle(),
                            ("/BAG_root/" + arrayName).c_str());
                        BAGRasterBand *const poBand =
                            new BAGRasterBand(this, nNextBand);
                        if (hBandId >= 0 &&
                            poBand->Initialize(hBandId, arrayName.c_str()))
                        {
                            SetBand(nNextBand++, poBand);
                        }
                        else
                        {
                            delete poBand;
                        }
                    }
                }
            }
        }
    }

    SetDescription(poOpenInfo->pszFilename);

    m_bReportVertCRS = CPLTestBool(CSLFetchNameValueDef(
        poOpenInfo->papszOpenOptions, "REPORT_VERTCRS", "YES"));

    // Load the XML metadata.
    LoadMetadata();

    if (bResampledGrid)
    {
        m_bMask = CPLTestBool(CSLFetchNameValueDef(poOpenInfo->papszOpenOptions,
                                                   "SUPERGRIDS_MASK", "NO"));
    }

    if (!m_bMask)
    {
        GDALDataset::SetMetadataItem(GDALMD_AREA_OR_POINT, GDALMD_AOP_POINT);
    }

    // Load for refinements grids for variable resolution datasets
    bool bHasRefinementGrids = false;
    if (bOpenSuperGrid || bListSubDS || bResampledGrid || bInterpolate)
    {
        bHasRefinementGrids =
            LookForRefinementGrids(poOpenInfo->papszOpenOptions, nY, nX);
        if (!bOpenSuperGrid && m_aosSubdatasets.size() == 2 &&
            EQUAL(pszMode, "AUTO"))
        {
            outOsSubDsName =
                CSLFetchNameValueDef(m_aosSubdatasets, "SUBDATASET_1_NAME", "");
            return true;
        }
    }
    else
    {
        if (LookForRefinementGrids(poOpenInfo->papszOpenOptions, 0, 0))
        {
            GDALDataset::SetMetadataItem("HAS_SUPERGRIDS", "TRUE");
        }
        m_aosSubdatasets.Clear();
    }

    if (!bIsSubdataset && osGeorefMetadataLayer.empty())
    {
        auto poGeoref_metadata = m_poRootGroup->OpenGroupFromFullname(
            "/BAG_root/georef_metadata", nullptr);
        if (poGeoref_metadata)
        {
            const auto groupNames = poGeoref_metadata->GetGroupNames(nullptr);
            if (!groupNames.empty())
            {
                if (m_aosSubdatasets.empty())
                {
                    const int nIdx = 1;
                    m_aosSubdatasets.AddNameValue(
                        CPLSPrintf("SUBDATASET_%d_NAME", nIdx),
                        CPLSPrintf("BAG:\"%s\":bathymetry_coverage",
                                   GetDescription()));
                    m_aosSubdatasets.AddNameValue(
                        CPLSPrintf("SUBDATASET_%d_DESC", nIdx),
                        "Bathymetry gridded data");
                }
                for (const auto &groupName : groupNames)
                {
                    const int nIdx = m_aosSubdatasets.size() / 2 + 1;
                    m_aosSubdatasets.AddNameValue(
                        CPLSPrintf("SUBDATASET_%d_NAME", nIdx),
                        CPLSPrintf("BAG:\"%s\":georef_metadata:%s",
                                   GetDescription(), groupName.c_str()));
                    m_aosSubdatasets.AddNameValue(
                        CPLSPrintf("SUBDATASET_%d_DESC", nIdx),
                        CPLSPrintf("Georeferenced metadata %s",
                                   groupName.c_str()));
                }
            }
        }
    }

    double dfMinResX = 0.0;
    double dfMinResY = 0.0;
    double dfMaxResX = 0.0;
    double dfMaxResY = 0.0;
    if (m_hVarresMetadata >= 0)
    {
        if (!GH5_FetchAttribute(m_hVarresMetadata, "min_resolution_x",
                                dfMinResX) ||
            !GH5_FetchAttribute(m_hVarresMetadata, "min_resolution_y",
                                dfMinResY))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot get min_resolution_x and/or min_resolution_y");
            return false;
        }

        if (!GH5_FetchAttribute(m_hVarresMetadata, "max_resolution_x",
                                dfMaxResX) ||
            !GH5_FetchAttribute(m_hVarresMetadata, "max_resolution_y",
                                dfMaxResY))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot get max_resolution_x and/or max_resolution_y");
            return false;
        }

        if (!bOpenSuperGrid && !bResampledGrid && !bInterpolate)
        {
            GDALDataset::SetMetadataItem("MIN_RESOLUTION_X",
                                         CPLSPrintf("%f", dfMinResX));
            GDALDataset::SetMetadataItem("MIN_RESOLUTION_Y",
                                         CPLSPrintf("%f", dfMinResY));
            GDALDataset::SetMetadataItem("MAX_RESOLUTION_X",
                                         CPLSPrintf("%f", dfMaxResX));
            GDALDataset::SetMetadataItem("MAX_RESOLUTION_Y",
                                         CPLSPrintf("%f", dfMaxResY));
        }
    }

    if (bResampledGrid || bInterpolate)
    {
        if (!bHasRefinementGrids)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "No supergrids available. "
                     "%s mode not available",
                     pszMode);
            return false;
        }

        const char *pszValuePopStrategy = CSLFetchNameValueDef(
            poOpenInfo->papszOpenOptions, "VALUE_POPULATION", "MAX");
        if (EQUAL(pszValuePopStrategy, "MIN"))
        {
            m_ePopulation = BAGDataset::Population::MIN;
        }
        else if (EQUAL(pszValuePopStrategy, "MEAN"))
        {
            m_ePopulation = BAGDataset::Population::MEAN;
        }
        else if (EQUAL(pszValuePopStrategy, "MAX"))
        {
            m_ePopulation = BAGDataset::Population::MAX;
        }
        else
        {
            m_ePopulation = BAGDataset::Population::COUNT;
            bHasNoData = false;
            fNoDataValue = 0;
        }

        const char *pszResX =
            CSLFetchNameValue(poOpenInfo->papszOpenOptions, "RESX");
        const char *pszResY =
            CSLFetchNameValue(poOpenInfo->papszOpenOptions, "RESY");
        const char *pszResStrategy = CSLFetchNameValueDef(
            poOpenInfo->papszOpenOptions, "RES_STRATEGY", "AUTO");
        double dfDefaultResX = 0.0;
        double dfDefaultResY = 0.0;

        const char *pszResFilterMin =
            CSLFetchNameValue(poOpenInfo->papszOpenOptions, "RES_FILTER_MIN");
        const char *pszResFilterMax =
            CSLFetchNameValue(poOpenInfo->papszOpenOptions, "RES_FILTER_MAX");

        double dfResFilterMin = 0;
        if (pszResFilterMin != nullptr)
        {
            dfResFilterMin = CPLAtof(pszResFilterMin);
            const double dfMaxRes = std::min(dfMaxResX, dfMaxResY);
            if (dfResFilterMin >= dfMaxRes)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Cannot specified RES_FILTER_MIN >= %g", dfMaxRes);
                return false;
            }
            GDALDataset::SetMetadataItem("RES_FILTER_MIN",
                                         CPLSPrintf("%g", dfResFilterMin));
        }

        double dfResFilterMax = std::numeric_limits<double>::infinity();
        if (pszResFilterMax != nullptr)
        {
            dfResFilterMax = CPLAtof(pszResFilterMax);
            const double dfMinRes = std::min(dfMinResX, dfMinResY);
            if (dfResFilterMax < dfMinRes)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Cannot specified RES_FILTER_MAX < %g", dfMinRes);
                return false;
            }
            GDALDataset::SetMetadataItem("RES_FILTER_MAX",
                                         CPLSPrintf("%g", dfResFilterMax));
        }

        if (dfResFilterMin >= dfResFilterMax)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot specified RES_FILTER_MIN >= RES_FILTER_MAX");
            return false;
        }

        if (EQUAL(pszResStrategy, "AUTO") &&
            (pszResFilterMin || pszResFilterMax))
        {
            if (pszResFilterMax)
            {
                dfDefaultResX = dfResFilterMax;
                dfDefaultResY = dfResFilterMax;
            }
            else
            {
                dfDefaultResX = dfMaxResX;
                dfDefaultResY = dfMaxResY;
            }
        }
        else if (EQUAL(pszResStrategy, "AUTO") || EQUAL(pszResStrategy, "MIN"))
        {
            dfDefaultResX = dfMinResX;
            dfDefaultResY = dfMinResY;
        }
        else if (EQUAL(pszResStrategy, "MAX"))
        {
            dfDefaultResX = dfMaxResX;
            dfDefaultResY = dfMaxResY;
        }
        else if (EQUAL(pszResStrategy, "MEAN"))
        {
            if (!GetMeanSupergridsResolution(dfDefaultResX, dfDefaultResY))
            {
                return false;
            }
        }

        const char *pszMinX =
            CSLFetchNameValue(poOpenInfo->papszOpenOptions, "MINX");
        const char *pszMinY =
            CSLFetchNameValue(poOpenInfo->papszOpenOptions, "MINY");
        const char *pszMaxX =
            CSLFetchNameValue(poOpenInfo->papszOpenOptions, "MAXX");
        const char *pszMaxY =
            CSLFetchNameValue(poOpenInfo->papszOpenOptions, "MAXY");

        double dfMinX = m_dfLowResMinX;
        double dfMinY = m_dfLowResMinY;
        double dfMaxX = m_dfLowResMaxX;
        double dfMaxY = m_dfLowResMaxY;
        double dfResX = dfDefaultResX;
        double dfResY = dfDefaultResY;
        if (pszMinX)
            dfMinX = CPLAtof(pszMinX);
        if (pszMinY)
            dfMinY = CPLAtof(pszMinY);
        if (pszMaxX)
            dfMaxX = CPLAtof(pszMaxX);
        if (pszMaxY)
            dfMaxY = CPLAtof(pszMaxY);
        if (pszResX)
            dfResX = CPLAtof(pszResX);
        if (pszResY)
            dfResY = CPLAtof(pszResY);

        if (dfResX <= 0.0 || dfResY <= 0.0)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Invalid resolution: %f x %f", dfResX, dfResY);
            return false;
        }
        const double dfRasterXSize = (dfMaxX - dfMinX) / dfResX;
        const double dfRasterYSize = (dfMaxY - dfMinY) / dfResY;
        if (dfRasterXSize <= 1 || dfRasterYSize <= 1 ||
            dfRasterXSize > INT_MAX || dfRasterYSize > INT_MAX)
        {
            CPLError(CE_Failure, CPLE_NotSupported, "Invalid raster dimension");
            return false;
        }
        nRasterXSize = static_cast<int>(dfRasterXSize + 0.5);
        nRasterYSize = static_cast<int>(dfRasterYSize + 0.5);
        m_gt[0] = dfMinX;
        m_gt[1] = dfResX;
        m_gt[3] = dfMaxY;
        m_gt[5] = -dfResY;
        if (pszMaxY == nullptr || pszMinY != nullptr)
        {
            // if the constraint is not given by MAXY, we may need to tweak
            // m_gt[3] / maxy, so that we get the requested MINY
            // value
            m_gt[3] += dfMinY - (dfMaxY - nRasterYSize * dfResY);
        }

        const double dfMinRes = std::min(dfMinResX, dfMinResY);
        if (dfResFilterMin > dfMinRes)
        {
            m_dfResFilterMin = dfResFilterMin;
        }
        m_dfResFilterMax = dfResFilterMax;

        // Use min/max BAG refinement metadata items only if the
        // GDAL dataset bounding box is equal or larger to the BAG dataset
        const bool bInitializeMinMax =
            (!m_bMask && m_ePopulation != BAGDataset::Population::COUNT &&
             dfMinX <= m_dfLowResMinX && dfMinY <= m_dfLowResMinY &&
             dfMaxX >= m_dfLowResMaxX && dfMaxY >= m_dfLowResMaxY);

        if (bInterpolate)
        {
            // Depth
            SetBand(1,
                    new BAGInterpolatedBand(this, 1, bHasNoData, fNoDataValue,
                                            bInitializeMinMax));

            // Uncertainty
            SetBand(2,
                    new BAGInterpolatedBand(this, 2, bHasNoData, fNoDataValue,
                                            bInitializeMinMax));
        }
        else if (m_bMask || m_ePopulation == BAGDataset::Population::COUNT)
        {
            SetBand(1, new BAGResampledBand(this, 1, false, 0.0f, false));
        }
        else
        {
            SetBand(1, new BAGResampledBand(this, 1, bHasNoData, fNoDataValue,
                                            bInitializeMinMax));

            SetBand(2, new BAGResampledBand(this, 2, bHasNoData, fNoDataValue,
                                            bInitializeMinMax));
        }

        if (GetRasterCount() > 1)
        {
            GDALDataset::SetMetadataItem("INTERLEAVE", "PIXEL",
                                         "IMAGE_STRUCTURE");
        }

        const bool bCanUseLowResAsOvr = nRasterXSize > m_nLowResWidth &&
                                        nRasterYSize > m_nLowResHeight &&
                                        GetRasterCount() > 1;
        const int nMinOvrSize =
            bCanUseLowResAsOvr ? 1 + std::min(m_nLowResWidth, m_nLowResHeight)
                               : 256;
        for (int nOvrFactor = 2; nRasterXSize / nOvrFactor >= nMinOvrSize &&
                                 nRasterYSize / nOvrFactor >= nMinOvrSize;
             nOvrFactor *= 2)
        {
            auto poOvrDS = std::make_unique<BAGDataset>(this, nOvrFactor);

            for (int i = 1; i <= GetRasterCount(); i++)
            {
                if (bInterpolate)
                {
                    poOvrDS->SetBand(
                        i, new BAGInterpolatedBand(poOvrDS.get(), i, bHasNoData,
                                                   fNoDataValue, false));
                }
                else
                {
                    poOvrDS->SetBand(
                        i, new BAGResampledBand(poOvrDS.get(), i, bHasNoData,
                                                fNoDataValue, false));
                }
            }

            m_apoOverviewDS.emplace_back(std::move(poOvrDS));
        }

        // Use the low resolution grid as the last overview level
        if (bCanUseLowResAsOvr)
        {
            auto poOvrDS = std::make_unique<BAGDataset>(this, m_nLowResWidth,
                                                        m_nLowResHeight);

            poElevBand = new BAGRasterBand(poOvrDS.get(), 1);
            const hid_t hElevation =
                H5Dopen(GetHDF5Handle(), "/BAG_root/elevation");
            if (hElevation < 0 ||
                !poElevBand->Initialize(hElevation, "elevation"))
            {
                delete poElevBand;
                return false;
            }
            poOvrDS->SetBand(1, poElevBand);

            const hid_t hUncertainty =
                H5Dopen(GetHDF5Handle(), "/BAG_root/uncertainty");
            BAGRasterBand *poUBand = new BAGRasterBand(poOvrDS.get(), 2);
            if (hUncertainty < 0 ||
                !poUBand->Initialize(hUncertainty, "uncertainty"))
            {
                delete poUBand;
                return false;
            }
            poOvrDS->SetBand(2, poUBand);

            m_apoOverviewDS.emplace_back(std::move(poOvrDS));
        }
    }
    else if (bOpenSuperGrid)
    {
        auto oIter = m_oMapRefinemendGrids.find(nY * m_nLowResWidth + nX);
        if (oIter == m_oMapRefinemendGrids.end())
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Invalid subdataset");
            return false;
        }

        m_aosSubdatasets.Clear();
        const auto &pSuperGrid = oIter->second;
        nRasterXSize = static_cast<int>(pSuperGrid.nWidth);
        nRasterYSize = static_cast<int>(pSuperGrid.nHeight);

        // Convert from pixel-center convention to corner-pixel convention
        const double dfMinX =
            m_gt[0] + nX * m_gt[1] + pSuperGrid.fSWX - pSuperGrid.fResX / 2;
        const double dfMinY = m_gt[3] + m_nLowResHeight * m_gt[5] +
                              nY * -m_gt[5] + pSuperGrid.fSWY -
                              pSuperGrid.fResY / 2;
        const double dfMaxY =
            dfMinY + pSuperGrid.nHeight * static_cast<double>(pSuperGrid.fResY);

        m_gt[0] = dfMinX;
        m_gt[1] = pSuperGrid.fResX;
        m_gt[3] = dfMaxY;
        m_gt[5] = -pSuperGrid.fResY;
        m_nSuperGridRefinementStartIndex = pSuperGrid.nIndex;

        if (!osGeorefMetadataLayer.empty())
        {
            auto poGeoref_metadataLayer = m_poRootGroup->OpenGroupFromFullname(
                "/BAG_root/georef_metadata/" + osGeorefMetadataLayer, nullptr);
            if (poGeoref_metadataLayer == nullptr)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Cannot find georef_metadata layer %s",
                         osGeorefMetadataLayer.c_str());
                return false;
            }

            auto poKeys = poGeoref_metadataLayer->OpenMDArray("varres_keys");
            if (poKeys != nullptr)
            {
                auto poDims = poKeys->GetDimensions();
                if (poDims.size() != 2 || poDims[0]->GetSize() != 1 ||
                    poDims[1]->GetSize() != m_nRefinementsSize)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Wrong dimensions for %s/varres_keys",
                             osGeorefMetadataLayer.c_str());
                    return false;
                }
                if (poKeys->GetDataType().GetClass() != GEDTC_NUMERIC ||
                    !GDALDataTypeIsInteger(
                        poKeys->GetDataType().GetNumericDataType()))
                {
                    CPLError(
                        CE_Failure, CPLE_AppDefined,
                        "Only integer data type supported for %s/varres_keys",
                        osGeorefMetadataLayer.c_str());
                    return false;
                }
            }

            auto poValues = poGeoref_metadataLayer->OpenMDArray("values");
            if (poValues == nullptr)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Cannot find array values of georef_metadata layer %s",
                         osGeorefMetadataLayer.c_str());
                return false;
            }
            if (poValues->GetDimensionCount() != 1)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Wrong dimensions for %s/values",
                         osGeorefMetadataLayer.c_str());
                return false;
            }
            if (poValues->GetDataType().GetClass() != GEDTC_COMPOUND)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Only compound data type supported for %s/values",
                         osGeorefMetadataLayer.c_str());
                return false;
            }
            SetBand(1, new BAGGeorefMDSuperGridBand(
                           poValues, poKeys,
                           new BAGSuperGridBand(this, 1, bHasNoData,
                                                fNoDataValue)));
        }
        else
        {
            for (int i = 0; i < 2; i++)
            {
                SetBand(i + 1, new BAGSuperGridBand(this, i + 1, bHasNoData,
                                                    fNoDataValue));
            }

            GDALDataset::SetMetadataItem("INTERLEAVE", "PIXEL",
                                         "IMAGE_STRUCTURE");
        }

        SetPhysicalFilename(osFilename);

        m_oMapRefinemendGrids.clear();
    }

    // Setup/check for pam .aux.xml.
    TryLoadXML();

    // Setup overviews.
    oOvManager.Initialize(this, poOpenInfo->pszFilename);

    return true;
}

/************************************************************************/
/*                            GetLayer()                                */
/************************************************************************/

OGRLayer *BAGDataset::GetLayer(int idx)
{
    if (idx != 0)
        return nullptr;
    return m_poTrackingListLayer.get();
}

/************************************************************************/
/*                        BAGTrackingListLayer                          */
/************************************************************************/

class BAGTrackingListLayer final
    : public OGRLayer,
      public OGRGetNextFeatureThroughRaw<BAGTrackingListLayer>
{
    std::shared_ptr<GDALMDArray> m_poArray{};
    OGRFeatureDefn *m_poFeatureDefn = nullptr;
    int m_nIdx = 0;

    OGRFeature *GetNextRawFeature();

    CPL_DISALLOW_COPY_ASSIGN(BAGTrackingListLayer)

  public:
    explicit BAGTrackingListLayer(const std::shared_ptr<GDALMDArray> &poArray);
    ~BAGTrackingListLayer();

    OGRFeatureDefn *GetLayerDefn() override
    {
        return m_poFeatureDefn;
    }

    void ResetReading() override;
    DEFINE_GET_NEXT_FEATURE_THROUGH_RAW(BAGTrackingListLayer)

    int TestCapability(const char *) override
    {
        return false;
    }
};

/************************************************************************/
/*                       BAGTrackingListLayer()                         */
/************************************************************************/

BAGTrackingListLayer::BAGTrackingListLayer(
    const std::shared_ptr<GDALMDArray> &poArray)
    : m_poArray(poArray)
{
    m_poFeatureDefn = new OGRFeatureDefn("tracking_list");
    SetDescription(m_poFeatureDefn->GetName());
    m_poFeatureDefn->Reference();
    m_poFeatureDefn->SetGeomType(wkbNone);

    const auto &poComponents = poArray->GetDataType().GetComponents();
    for (const auto &poComponent : poComponents)
    {
        if (poComponent->GetType().GetClass() == GEDTC_NUMERIC)
        {
            OGRFieldType eType;
            if (GDALDataTypeIsInteger(
                    poComponent->GetType().GetNumericDataType()))
                eType = OFTInteger;
            else
                eType = OFTReal;
            OGRFieldDefn oFieldDefn(poComponent->GetName().c_str(), eType);
            m_poFeatureDefn->AddFieldDefn(&oFieldDefn);
        }
    }
}

/************************************************************************/
/*                      ~BAGTrackingListLayer()                         */
/************************************************************************/

BAGTrackingListLayer::~BAGTrackingListLayer()
{
    m_poFeatureDefn->Release();
}

/************************************************************************/
/*                            ResetReading()                            */
/************************************************************************/

void BAGTrackingListLayer::ResetReading()
{
    m_nIdx = 0;
}

/************************************************************************/
/*                          GetNextRawFeature()                         */
/************************************************************************/

OGRFeature *BAGTrackingListLayer::GetNextRawFeature()
{
    if (static_cast<GUInt64>(m_nIdx) >=
        m_poArray->GetDimensions()[0]->GetSize())
        return nullptr;

    const auto &oDataType = m_poArray->GetDataType();
    std::vector<GByte> abyRow(oDataType.GetSize());

    const GUInt64 arrayStartIdx = static_cast<GUInt64>(m_nIdx);
    const size_t count = 1;
    const GInt64 arrayStep = 0;
    const GPtrDiff_t bufferStride = 0;
    m_poArray->Read(&arrayStartIdx, &count, &arrayStep, &bufferStride,
                    oDataType, &abyRow[0]);
    int iCol = 0;
    auto poFeature = new OGRFeature(m_poFeatureDefn);
    poFeature->SetFID(m_nIdx);
    m_nIdx++;

    const auto &poComponents = oDataType.GetComponents();
    for (const auto &poComponent : poComponents)
    {
        if (poComponent->GetType().GetClass() == GEDTC_NUMERIC)
        {
            if (GDALDataTypeIsInteger(
                    poComponent->GetType().GetNumericDataType()))
            {
                int nValue = 0;
                GDALCopyWords(&abyRow[poComponent->GetOffset()],
                              poComponent->GetType().GetNumericDataType(), 0,
                              &nValue, GDT_Int32, 0, 1);
                poFeature->SetField(iCol, nValue);
            }
            else
            {
                double dfValue = 0;
                GDALCopyWords(&abyRow[poComponent->GetOffset()],
                              poComponent->GetType().GetNumericDataType(), 0,
                              &dfValue, GDT_Float64, 0, 1);
                poFeature->SetField(iCol, dfValue);
            }
            iCol++;
        }
    }

    return poFeature;
}

/************************************************************************/
/*                          OpenVector()                                */
/************************************************************************/

bool BAGDataset::OpenVector()
{
    auto poTrackingList =
        m_poRootGroup->OpenMDArrayFromFullname("/BAG_root/tracking_list");
    if (!poTrackingList)
        return false;
    if (poTrackingList->GetDimensions().size() != 1)
        return false;
    if (poTrackingList->GetDataType().GetClass() != GEDTC_COMPOUND)
        return false;

    m_poTrackingListLayer.reset(new BAGTrackingListLayer(poTrackingList));
    return true;
}

/************************************************************************/
/*                          OpenForCreate()                             */
/************************************************************************/

GDALDataset *BAGDataset::OpenForCreate(GDALOpenInfo *poOpenInfo, int nXSizeIn,
                                       int nYSizeIn, int nBandsIn,
                                       CSLConstList papszCreationOptions)
{
    CPLString osFilename(poOpenInfo->pszFilename);

    // Open the file as an HDF5 file.
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    H5Pset_driver(fapl, HDF5GetFileDriver(), nullptr);
    hid_t hHDF5 = H5Fopen(osFilename, H5F_ACC_RDWR, fapl);
    H5Pclose(fapl);
    if (hHDF5 < 0)
        return nullptr;

    auto poSharedResources = GDAL::HDF5SharedResources::Create(osFilename);
    poSharedResources->m_hHDF5 = hHDF5;

    auto poRootGroup = HDF5Dataset::OpenGroup(poSharedResources);
    if (poRootGroup == nullptr)
        return nullptr;

    // Create a corresponding dataset.
    BAGDataset *const poDS = new BAGDataset();

    poDS->eAccess = poOpenInfo->eAccess;
    poDS->m_poRootGroup = std::move(poRootGroup);
    poDS->m_poSharedResources = std::move(poSharedResources);
    poDS->m_aosCreationOptions = papszCreationOptions;

    poDS->nRasterXSize = nXSizeIn;
    poDS->nRasterYSize = nYSizeIn;

    const int nBlockSize = std::min(
        4096,
        atoi(CSLFetchNameValueDef(papszCreationOptions, "BLOCK_SIZE", "100")));
    const int nBlockXSize = std::min(poDS->nRasterXSize, nBlockSize);
    const int nBlockYSize = std::min(poDS->nRasterYSize, nBlockSize);

    for (int i = 0; i < nBandsIn; i++)
    {
        auto poBand = new BAGRasterBand(poDS, i + 1);
        poBand->nBlockXSize = nBlockXSize;
        poBand->nBlockYSize = nBlockYSize;
        poBand->eDataType = GDT_Float32;
        poBand->m_bHasNoData = true;
        poBand->m_fNoDataValue = fDEFAULT_NODATA;
        poBand->GDALRasterBand::SetDescription(i == 0 ? "elevation"
                                                      : "uncertainty");
        poDS->SetBand(i + 1, poBand);
    }

    poDS->SetDescription(poOpenInfo->pszFilename);

    poDS->m_bReportVertCRS = CPLTestBool(CSLFetchNameValueDef(
        poOpenInfo->papszOpenOptions, "REPORT_VERTCRS", "YES"));

    poDS->GDALDataset::SetMetadataItem(GDALMD_AREA_OR_POINT, GDALMD_AOP_POINT);

    // Setup/check for pam .aux.xml.
    poDS->TryLoadXML();

    // Setup overviews.
    poDS->oOvManager.Initialize(poDS, poOpenInfo->pszFilename);

    return poDS;
}

/************************************************************************/
/*                      GetMeanSupergridsResolution()                   */
/************************************************************************/

bool BAGDataset::GetMeanSupergridsResolution(double &dfResX, double &dfResY)
{
    const int nChunkXSize = m_nChunkXSizeVarresMD;
    const int nChunkYSize = m_nChunkYSizeVarresMD;

    dfResX = 0.0;
    dfResY = 0.0;
    int nValidSuperGrids = 0;
    std::vector<BAGRefinementGrid> rgrids(static_cast<size_t>(nChunkXSize) *
                                          nChunkYSize);
    const int county = DIV_ROUND_UP(m_nLowResHeight, nChunkYSize);
    const int countx = DIV_ROUND_UP(m_nLowResWidth, nChunkXSize);
    for (int y = 0; y < county; y++)
    {
        const int nReqCountY =
            std::min(nChunkYSize, m_nLowResHeight - y * nChunkYSize);
        for (int x = 0; x < countx; x++)
        {
            const int nReqCountX =
                std::min(nChunkXSize, m_nLowResWidth - x * nChunkXSize);

            // Create memory space to receive the data.
            hsize_t count[2] = {static_cast<hsize_t>(nReqCountY),
                                static_cast<hsize_t>(nReqCountX)};
            const hid_t memspace = H5Screate_simple(2, count, nullptr);
            H5OFFSET_TYPE mem_offset[2] = {static_cast<H5OFFSET_TYPE>(0),
                                           static_cast<H5OFFSET_TYPE>(0)};
            if (H5Sselect_hyperslab(memspace, H5S_SELECT_SET, mem_offset,
                                    nullptr, count, nullptr) < 0)
            {
                H5Sclose(memspace);
                return false;
            }

            if (ReadVarresMetadataValue(y * nChunkYSize, x * nChunkXSize,
                                        memspace, rgrids.data(), nReqCountY,
                                        nReqCountX))
            {
                for (int i = 0; i < nReqCountX * nReqCountY; i++)
                {
                    if (rgrids[i].nWidth > 0)
                    {
                        dfResX += rgrids[i].fResX;
                        dfResY += rgrids[i].fResY;
                        nValidSuperGrids++;
                    }
                }
            }
            H5Sclose(memspace);
        }
    }

    if (nValidSuperGrids == 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "No valid supergrids");
        return false;
    }

    dfResX /= nValidSuperGrids;
    dfResY /= nValidSuperGrids;
    return true;
}

/************************************************************************/
/*                      GetVarresMetadataChunkSizes()                   */
/************************************************************************/

void BAGDataset::GetVarresMetadataChunkSizes(int &nChunkXSize, int &nChunkYSize)
{
    const hid_t listid = H5Dget_create_plist(m_hVarresMetadata);
    nChunkXSize = m_nLowResWidth;
    nChunkYSize = std::max(
        1, std::min(10 * 1024 * 1024 / m_nLowResWidth, m_nLowResHeight));
    if (listid > 0)
    {
        if (H5Pget_layout(listid) == H5D_CHUNKED)
        {
            hsize_t panChunkDims[2] = {0, 0};
            const int nDimSize = H5Pget_chunk(listid, 2, panChunkDims);
            CPL_IGNORE_RET_VAL(nDimSize);
            CPLAssert(nDimSize == 2);
            nChunkXSize = static_cast<int>(panChunkDims[1]);
            nChunkYSize = static_cast<int>(panChunkDims[0]);
        }

        H5Pclose(listid);
    }
}

/************************************************************************/
/*                     GetVarresRefinementChunkSize()                   */
/************************************************************************/

void BAGDataset::GetVarresRefinementChunkSize(unsigned &nChunkSize)
{
    const hid_t listid = H5Dget_create_plist(m_hVarresRefinements);
    nChunkSize = 1024;
    if (listid > 0)
    {
        if (H5Pget_layout(listid) == H5D_CHUNKED)
        {
            hsize_t panChunkDims[2] = {0, 0};
            const int nDimSize = H5Pget_chunk(listid, 2, panChunkDims);
            CPL_IGNORE_RET_VAL(nDimSize);
            CPLAssert(nDimSize == 2);
            nChunkSize = static_cast<unsigned>(panChunkDims[1]);
        }

        H5Pclose(listid);
    }
}

/************************************************************************/
/*                        GetRefinementValues()                         */
/************************************************************************/

const float *BAGDataset::GetRefinementValues(unsigned nRefinementIndex)
{
    unsigned nStartIndex = (nRefinementIndex / m_nChunkSizeVarresRefinement) *
                           m_nChunkSizeVarresRefinement;
    const auto vPtr = m_oCacheRefinementValues.getPtr(nStartIndex);
    if (vPtr)
        return vPtr->data() + 2 * (nRefinementIndex - nStartIndex);

    const unsigned nCachedRefinementCount = std::min(
        m_nChunkSizeVarresRefinement, m_nRefinementsSize - nStartIndex);
    std::vector<float> values(2 * nCachedRefinementCount);

    hsize_t countVarresRefinements[2] = {
        static_cast<hsize_t>(1), static_cast<hsize_t>(nCachedRefinementCount)};
    const hid_t memspaceVarresRefinements =
        H5Screate_simple(2, countVarresRefinements, nullptr);
    H5OFFSET_TYPE mem_offset[2] = {static_cast<H5OFFSET_TYPE>(0),
                                   static_cast<H5OFFSET_TYPE>(0)};
    if (H5Sselect_hyperslab(memspaceVarresRefinements, H5S_SELECT_SET,
                            mem_offset, nullptr, countVarresRefinements,
                            nullptr) < 0)
    {
        H5Sclose(memspaceVarresRefinements);
        return nullptr;
    }

    H5OFFSET_TYPE offsetRefinement[2] = {
        static_cast<H5OFFSET_TYPE>(0), static_cast<H5OFFSET_TYPE>(nStartIndex)};
    if (H5Sselect_hyperslab(m_hVarresRefinementsDataspace, H5S_SELECT_SET,
                            offsetRefinement, nullptr, countVarresRefinements,
                            nullptr) < 0)
    {
        H5Sclose(memspaceVarresRefinements);
        return nullptr;
    }
    if (H5Dread(m_hVarresRefinements, m_hVarresRefinementsNative,
                memspaceVarresRefinements, m_hVarresRefinementsDataspace,
                H5P_DEFAULT, values.data()) < 0)
    {
        H5Sclose(memspaceVarresRefinements);
        return nullptr;
    }
    H5Sclose(memspaceVarresRefinements);
    const auto &vRef =
        m_oCacheRefinementValues.insert(nStartIndex, std::move(values));
    return vRef.data() + 2 * (nRefinementIndex - nStartIndex);
}

/************************************************************************/
/*                        ReadVarresMetadataValue()                     */
/************************************************************************/

bool BAGDataset::ReadVarresMetadataValue(int y, int x, hid_t memspace,
                                         BAGRefinementGrid *rgrid, int height,
                                         int width)
{
    constexpr int metadata_elt_size = 3 * 4 + 4 * 4;  // 3 uint and 4 float
    std::vector<char> buffer(static_cast<size_t>(metadata_elt_size) * height *
                             width);

    hsize_t count[2] = {static_cast<hsize_t>(height),
                        static_cast<hsize_t>(width)};
    H5OFFSET_TYPE offset[2] = {static_cast<H5OFFSET_TYPE>(y),
                               static_cast<H5OFFSET_TYPE>(x)};
    if (H5Sselect_hyperslab(m_hVarresMetadataDataspace, H5S_SELECT_SET, offset,
                            nullptr, count, nullptr) < 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "ReadVarresMetadataValue(): H5Sselect_hyperslab() failed");
        return false;
    }

    if (H5Dread(m_hVarresMetadata, m_hVarresMetadataNative, memspace,
                m_hVarresMetadataDataspace, H5P_DEFAULT, buffer.data()) < 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "ReadVarresMetadataValue(): H5Dread() failed");
        return false;
    }

    for (int i = 0; i < width * height; i++)
    {
        const char *src_ptr = buffer.data() + metadata_elt_size * i;
        memcpy(&rgrid[i].nIndex, src_ptr, 4);
        memcpy(&rgrid[i].nWidth, src_ptr + 4, 4);
        memcpy(&rgrid[i].nHeight, src_ptr + 8, 4);
        memcpy(&rgrid[i].fResX, src_ptr + 12, 4);
        memcpy(&rgrid[i].fResY, src_ptr + 16, 4);
        memcpy(&rgrid[i].fSWX, src_ptr + 20, 4);
        memcpy(&rgrid[i].fSWY, src_ptr + 24, 4);
    }
    return true;
}

/************************************************************************/
/*                        LookForRefinementGrids()                      */
/************************************************************************/

bool BAGDataset::LookForRefinementGrids(CSLConstList l_papszOpenOptions,
                                        int nYSubDS, int nXSubDS)

{
    m_hVarresMetadata = GH5DopenNoWarning(m_poSharedResources->m_hHDF5,
                                          "/BAG_root/varres_metadata");
    if (m_hVarresMetadata < 0)
        return false;
    m_hVarresRefinements =
        H5Dopen(m_poSharedResources->m_hHDF5, "/BAG_root/varres_refinements");
    if (m_hVarresRefinements < 0)
        return false;

    m_hVarresMetadataDataType = H5Dget_type(m_hVarresMetadata);
    if (H5Tget_class(m_hVarresMetadataDataType) != H5T_COMPOUND)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "m_hVarresMetadataDataType is not compound");
        return false;
    }

    const struct
    {
        const char *pszName;
        hid_t eType;
    } asMetadataFields[] = {
        {"index", H5T_NATIVE_UINT},         {"dimensions_x", H5T_NATIVE_UINT},
        {"dimensions_y", H5T_NATIVE_UINT},  {"resolution_x", H5T_NATIVE_FLOAT},
        {"resolution_y", H5T_NATIVE_FLOAT}, {"sw_corner_x", H5T_NATIVE_FLOAT},
        {"sw_corner_y", H5T_NATIVE_FLOAT},
    };

    if (H5Tget_nmembers(m_hVarresMetadataDataType) !=
        CPL_ARRAYSIZE(asMetadataFields))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "m_hVarresMetadataDataType has not %u members",
                 static_cast<unsigned>(CPL_ARRAYSIZE(asMetadataFields)));
        return false;
    }

    for (unsigned i = 0; i < CPL_ARRAYSIZE(asMetadataFields); i++)
    {
        char *pszName = H5Tget_member_name(m_hVarresMetadataDataType, i);
        if (strcmp(pszName, asMetadataFields[i].pszName) != 0)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "asMetadataFields[%u].pszName = %s instead of %s",
                     static_cast<unsigned>(i), pszName,
                     asMetadataFields[i].pszName);
            H5free_memory(pszName);
            return false;
        }
        H5free_memory(pszName);
        const hid_t type = H5Tget_member_type(m_hVarresMetadataDataType, i);
        const hid_t hNativeType = H5Tget_native_type(type, H5T_DIR_DEFAULT);
        bool bTypeOK = H5Tequal(asMetadataFields[i].eType, hNativeType) > 0;
        H5Tclose(hNativeType);
        H5Tclose(type);
        if (!bTypeOK)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "asMetadataFields[%u].eType is not of expected type", i);
            return false;
        }
    }

    m_hVarresMetadataDataspace = H5Dget_space(m_hVarresMetadata);
    if (H5Sget_simple_extent_ndims(m_hVarresMetadataDataspace) != 2)
    {
        CPLDebug("BAG",
                 "H5Sget_simple_extent_ndims(m_hVarresMetadataDataspace) != 2");
        return false;
    }

    {
        hsize_t dims[2] = {static_cast<hsize_t>(0), static_cast<hsize_t>(0)};
        hsize_t maxdims[2] = {static_cast<hsize_t>(0), static_cast<hsize_t>(0)};

        H5Sget_simple_extent_dims(m_hVarresMetadataDataspace, dims, maxdims);
        if (dims[0] != static_cast<hsize_t>(m_nLowResHeight) ||
            dims[1] != static_cast<hsize_t>(m_nLowResWidth))
        {
            CPLDebug("BAG", "Unexpected dimension for m_hVarresMetadata");
            return false;
        }
    }

    // We could potentially go beyond but we'd need to make sure that
    // m_oMapRefinemendGrids is indexed by a int64_t
    if (m_nLowResWidth > std::numeric_limits<int>::max() / m_nLowResHeight)
    {
        CPLError(CE_Failure, CPLE_NotSupported, "Too many refinement grids");
        return false;
    }

    m_hVarresMetadataNative =
        H5Tget_native_type(m_hVarresMetadataDataType, H5T_DIR_ASCEND);

    m_hVarresRefinementsDataType = H5Dget_type(m_hVarresRefinements);
    if (H5Tget_class(m_hVarresRefinementsDataType) != H5T_COMPOUND)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "m_hVarresRefinementsDataType is not compound");
        return false;
    }

    const struct
    {
        const char *pszName;
        hid_t eType;
    } asRefinementsFields[] = {
        {"depth", H5T_NATIVE_FLOAT},
        {"depth_uncrt", H5T_NATIVE_FLOAT},
    };

    if (H5Tget_nmembers(m_hVarresRefinementsDataType) !=
        CPL_ARRAYSIZE(asRefinementsFields))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "m_hVarresRefinementsDataType has not %u members",
                 static_cast<unsigned>(CPL_ARRAYSIZE(asRefinementsFields)));
        return false;
    }

    for (unsigned i = 0; i < CPL_ARRAYSIZE(asRefinementsFields); i++)
    {
        char *pszName = H5Tget_member_name(m_hVarresRefinementsDataType, i);
        if (strcmp(pszName, asRefinementsFields[i].pszName) != 0)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "asRefinementsFields[%u].pszName = %s instead of %s",
                     static_cast<unsigned>(i), pszName,
                     asRefinementsFields[i].pszName);
            H5free_memory(pszName);
            return false;
        }
        H5free_memory(pszName);
        const hid_t type = H5Tget_member_type(m_hVarresRefinementsDataType, i);
        const hid_t hNativeType = H5Tget_native_type(type, H5T_DIR_DEFAULT);
        bool bTypeOK = H5Tequal(asRefinementsFields[i].eType, hNativeType) > 0;
        H5Tclose(hNativeType);
        H5Tclose(type);
        if (!bTypeOK)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "asRefinementsFields[%u].eType is not of expected type",
                     i);
            return false;
        }
    }

    m_hVarresRefinementsDataspace = H5Dget_space(m_hVarresRefinements);
    if (H5Sget_simple_extent_ndims(m_hVarresRefinementsDataspace) != 2)
    {
        CPLDebug(
            "BAG",
            "H5Sget_simple_extent_ndims(m_hVarresRefinementsDataspace) != 2");
        return false;
    }

    m_hVarresRefinementsNative =
        H5Tget_native_type(m_hVarresRefinementsDataType, H5T_DIR_ASCEND);

    hsize_t nRefinementsSize;
    {
        hsize_t dims[2] = {static_cast<hsize_t>(0), static_cast<hsize_t>(0)};
        hsize_t maxdims[2] = {static_cast<hsize_t>(0), static_cast<hsize_t>(0)};

        H5Sget_simple_extent_dims(m_hVarresRefinementsDataspace, dims, maxdims);
        if (dims[0] != 1)
        {
            CPLDebug("BAG", "Unexpected dimension for m_hVarresRefinements");
            return false;
        }
        nRefinementsSize = dims[1];
        m_nRefinementsSize = static_cast<unsigned>(nRefinementsSize);
        CPLDebug("BAG", "m_nRefinementsSize = %u", m_nRefinementsSize);
    }

    GetVarresMetadataChunkSizes(m_nChunkXSizeVarresMD, m_nChunkYSizeVarresMD);
    CPLDebug("BAG", "m_nChunkXSizeVarresMD = %d, m_nChunkYSizeVarresMD = %d",
             m_nChunkXSizeVarresMD, m_nChunkYSizeVarresMD);
    GetVarresRefinementChunkSize(m_nChunkSizeVarresRefinement);
    CPLDebug("BAG", "m_nChunkSizeVarresRefinement = %u",
             m_nChunkSizeVarresRefinement);

    const char *pszMode = CSLFetchNameValueDef(l_papszOpenOptions, "MODE", "");
    if (EQUAL(pszMode, "RESAMPLED_GRID") || EQUAL(pszMode, "INTERPOLATED"))
    {
        return true;
    }

    const char *pszSUPERGRIDS =
        CSLFetchNameValue(l_papszOpenOptions, "SUPERGRIDS_INDICES");

    struct yx
    {
        int y;
        int x;

        yx(int yin, int xin) : y(yin), x(xin)
        {
        }

        bool operator<(const yx &other) const
        {
            return y < other.y || (y == other.y && x < other.x);
        }
    };

    std::set<yx> oSupergrids;
    int nMinX = 0;
    int nMinY = 0;
    int nMaxX = m_nLowResWidth - 1;
    int nMaxY = m_nLowResHeight - 1;
    if (nYSubDS >= 0 && nXSubDS >= 0)
    {
        nMinX = nXSubDS;
        nMaxX = nXSubDS;
        nMinY = nYSubDS;
        nMaxY = nYSubDS;
    }
    else if (pszSUPERGRIDS)
    {
        char chExpectedChar = '(';
        bool bNextIsY = false;
        bool bNextIsX = false;
        bool bHasY = false;
        bool bHasX = false;
        int nY = 0;
        int i = 0;
        for (; pszSUPERGRIDS[i]; i++)
        {
            if (chExpectedChar && pszSUPERGRIDS[i] != chExpectedChar)
            {
                CPLError(
                    CE_Warning, CPLE_AppDefined,
                    "Invalid formatting for SUPERGRIDS_INDICES at index %d. "
                    "Expecting %c, got %c",
                    i, chExpectedChar, pszSUPERGRIDS[i]);
                break;
            }
            else if (chExpectedChar == '(')
            {
                chExpectedChar = 0;
                bNextIsY = true;
            }
            else if (chExpectedChar == ',')
            {
                chExpectedChar = '(';
            }
            else
            {
                CPLAssert(chExpectedChar == 0);
                if (bNextIsY && pszSUPERGRIDS[i] >= '0' &&
                    pszSUPERGRIDS[i] <= '9')
                {
                    nY = atoi(pszSUPERGRIDS + i);
                    bNextIsY = false;
                    bHasY = true;
                }
                else if (bNextIsX && pszSUPERGRIDS[i] >= '0' &&
                         pszSUPERGRIDS[i] <= '9')
                {
                    int nX = atoi(pszSUPERGRIDS + i);
                    bNextIsX = false;
                    oSupergrids.insert(yx(nY, nX));
                    bHasX = true;
                }
                else if ((bHasX || bHasY) && pszSUPERGRIDS[i] >= '0' &&
                         pszSUPERGRIDS[i] <= '9')
                {
                    // ok
                }
                else if (bHasY && pszSUPERGRIDS[i] == ',')
                {
                    bNextIsX = true;
                }
                else if (bHasX && bHasY && pszSUPERGRIDS[i] == ')')
                {
                    chExpectedChar = ',';
                    bHasX = false;
                    bHasY = false;
                }
                else
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "Invalid formatting for SUPERGRIDS_INDICES at "
                             "index %d. "
                             "Got %c",
                             i, pszSUPERGRIDS[i]);
                    break;
                }
            }
        }
        if (pszSUPERGRIDS[i] == 0 && chExpectedChar != ',')
        {
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Invalid formatting for SUPERGRIDS_INDICES at index %d.",
                     i);
        }

        bool bFirst = true;
        for (const auto &yxPair : oSupergrids)
        {
            if (bFirst)
            {
                nMinX = yxPair.x;
                nMaxX = yxPair.x;
                nMinY = yxPair.y;
                nMaxY = yxPair.y;
                bFirst = false;
            }
            else
            {
                nMinX = std::min(nMinX, yxPair.x);
                nMaxX = std::max(nMaxX, yxPair.x);
                nMinY = std::min(nMinY, yxPair.y);
                nMaxY = std::max(nMaxY, yxPair.y);
            }
        }
    }
    const char *pszMinX = CSLFetchNameValue(l_papszOpenOptions, "MINX");
    const char *pszMinY = CSLFetchNameValue(l_papszOpenOptions, "MINY");
    const char *pszMaxX = CSLFetchNameValue(l_papszOpenOptions, "MAXX");
    const char *pszMaxY = CSLFetchNameValue(l_papszOpenOptions, "MAXY");
    const int nCountBBoxElts = (pszMinX ? 1 : 0) + (pszMinY ? 1 : 0) +
                               (pszMaxX ? 1 : 0) + (pszMaxY ? 1 : 0);
    const bool bHasBoundingBoxFilter =
        !(nYSubDS >= 0 && nXSubDS >= 0) && (nCountBBoxElts == 4);
    double dfFilterMinX = 0.0;
    double dfFilterMinY = 0.0;
    double dfFilterMaxX = 0.0;
    double dfFilterMaxY = 0.0;
    if (nYSubDS >= 0 && nXSubDS >= 0)
    {
        // do nothing
    }
    else if (bHasBoundingBoxFilter)
    {
        dfFilterMinX = CPLAtof(pszMinX);
        dfFilterMinY = CPLAtof(pszMinY);
        dfFilterMaxX = CPLAtof(pszMaxX);
        dfFilterMaxY = CPLAtof(pszMaxY);

        nMinX = std::max(nMinX,
                         static_cast<int>((dfFilterMinX - m_gt[0]) / m_gt[1]));
        nMaxX = std::min(nMaxX,
                         static_cast<int>((dfFilterMaxX - m_gt[0]) / m_gt[1]));

        nMinY = std::max(
            nMinY, static_cast<int>(
                       (dfFilterMinY - (m_gt[3] + m_nLowResHeight * m_gt[5])) /
                       -m_gt[5]));
        nMaxY = std::min(
            nMaxY, static_cast<int>(
                       (dfFilterMaxY - (m_gt[3] + m_nLowResHeight * m_gt[5])) /
                       -m_gt[5]));
    }
    else if (nCountBBoxElts > 0)
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Bounding box filter ignored since only part of "
                 "MINX, MINY, MAXX and MAXY has been specified");
    }

    const double dfResFilterMin = CPLAtof(
        CSLFetchNameValueDef(l_papszOpenOptions, "RES_FILTER_MIN", "0"));
    const double dfResFilterMax = CPLAtof(
        CSLFetchNameValueDef(l_papszOpenOptions, "RES_FILTER_MAX", "inf"));

    std::vector<std::string> georefMDLayerNames;
    auto poGeoref_metadata = m_poRootGroup->OpenGroupFromFullname(
        "/BAG_root/georef_metadata", nullptr);
    if (poGeoref_metadata)
    {
        const auto groupNames = poGeoref_metadata->GetGroupNames(nullptr);
        for (const auto &groupName : groupNames)
        {
            georefMDLayerNames.push_back(groupName);
        }
    }

    const int nMaxSizeMap =
        atoi(CPLGetConfigOption("GDAL_BAG_MAX_SIZE_VARRES_MAP", "50000000"));
    const int nChunkXSize = m_nChunkXSizeVarresMD;
    const int nChunkYSize = m_nChunkYSizeVarresMD;
    std::vector<BAGRefinementGrid> rgrids(static_cast<size_t>(nChunkXSize) *
                                          nChunkYSize);
    bool bOK = true;
    for (int blockY = nMinY / nChunkYSize; bOK && blockY <= nMaxY / nChunkYSize;
         blockY++)
    {
        int nReqCountY =
            std::min(nChunkYSize, m_nLowResHeight - blockY * nChunkYSize);
        for (int blockX = nMinX / nChunkXSize;
             bOK && blockX <= nMaxX / nChunkXSize; blockX++)
        {
            int nReqCountX =
                std::min(nChunkXSize, m_nLowResWidth - blockX * nChunkXSize);

            // Create memory space to receive the data.
            hsize_t count[2] = {static_cast<hsize_t>(nReqCountY),
                                static_cast<hsize_t>(nReqCountX)};
            const hid_t memspace = H5Screate_simple(2, count, nullptr);
            H5OFFSET_TYPE mem_offset[2] = {static_cast<H5OFFSET_TYPE>(0),
                                           static_cast<H5OFFSET_TYPE>(0)};
            if (H5Sselect_hyperslab(memspace, H5S_SELECT_SET, mem_offset,
                                    nullptr, count, nullptr) < 0)
            {
                H5Sclose(memspace);
                bOK = false;
                break;
            }

            if (!ReadVarresMetadataValue(blockY * nChunkYSize,
                                         blockX * nChunkXSize, memspace,
                                         rgrids.data(), nReqCountY, nReqCountX))
            {
                bOK = false;
                H5Sclose(memspace);
                break;
            }
            H5Sclose(memspace);

            for (int yInBlock = std::max(0, nMinY - blockY * nChunkYSize);
                 bOK && yInBlock <= std::min(nReqCountY - 1,
                                             nMaxY - blockY * nChunkYSize);
                 yInBlock++)
            {
                for (int xInBlock = std::max(0, nMinX - blockX * nChunkXSize);
                     bOK && xInBlock <= std::min(nReqCountX - 1,
                                                 nMaxX - blockX * nChunkXSize);
                     xInBlock++)
                {
                    const int y = yInBlock + blockY * nChunkYSize;
                    const int x = xInBlock + blockX * nChunkXSize;
                    const auto &rgrid =
                        rgrids[yInBlock * nReqCountX + xInBlock];
                    if (rgrid.nWidth > 0)
                    {
                        if (rgrid.fResX <= 0 || rgrid.fResY <= 0)
                        {
                            CPLError(CE_Failure, CPLE_NotSupported,
                                     "Incorrect resolution for supergrid "
                                     "(%d, %d).",
                                     static_cast<int>(y), static_cast<int>(x));
                            bOK = false;
                            break;
                        }
                        if (rgrid.nIndex + static_cast<GUInt64>(rgrid.nWidth) *
                                               rgrid.nHeight >
                            nRefinementsSize)
                        {
                            CPLError(
                                CE_Failure, CPLE_NotSupported,
                                "Incorrect index / dimensions for supergrid "
                                "(%d, %d).",
                                static_cast<int>(y), static_cast<int>(x));
                            bOK = false;
                            break;
                        }

                        if (rgrid.fSWX < 0.0f || rgrid.fSWY < 0.0f ||
                            // 0.1 is to deal with numeric imprecisions
                            rgrid.fSWX +
                                    (rgrid.nWidth - 1 - 0.1) * rgrid.fResX >
                                m_gt[1] ||
                            rgrid.fSWY +
                                    (rgrid.nHeight - 1 - 0.1) * rgrid.fResY >
                                -m_gt[5])
                        {
                            CPLError(
                                CE_Failure, CPLE_NotSupported,
                                "Incorrect bounds for supergrid "
                                "(%d, %d): %f, %f, %f, %f.",
                                static_cast<int>(y), static_cast<int>(x),
                                rgrid.fSWX, rgrid.fSWY,
                                rgrid.fSWX + (rgrid.nWidth - 1) * rgrid.fResX,
                                rgrid.fSWY + (rgrid.nHeight - 1) * rgrid.fResY);
                            bOK = false;
                            break;
                        }

                        const float gridRes =
                            std::max(rgrid.fResX, rgrid.fResY);
                        if (gridRes < dfResFilterMin ||
                            gridRes >= dfResFilterMax)
                        {
                            continue;
                        }

                        if (static_cast<int>(m_oMapRefinemendGrids.size()) ==
                            nMaxSizeMap)
                        {
                            CPLError(
                                CE_Failure, CPLE_AppDefined,
                                "Size of map of refinement grids has reached "
                                "%d entries. "
                                "Set the GDAL_BAG_MAX_SIZE_VARRES_MAP "
                                "configuration option "
                                "to an higher value if you want to allow more",
                                nMaxSizeMap);
                            return false;
                        }

                        try
                        {
                            m_oMapRefinemendGrids[y * m_nLowResWidth + x] =
                                rgrid;
                        }
                        catch (const std::exception &e)
                        {
                            CPLError(CE_Failure, CPLE_OutOfMemory,
                                     "Out of memory adding entries to map of "
                                     "refinement "
                                     "grids: %s",
                                     e.what());
                            return false;
                        }

                        const double dfMinX = m_gt[0] + x * m_gt[1] +
                                              rgrid.fSWX - rgrid.fResX / 2;
                        const double dfMaxX =
                            dfMinX +
                            rgrid.nWidth * static_cast<double>(rgrid.fResX);
                        const double dfMinY =
                            m_gt[3] + m_nLowResHeight * m_gt[5] + y * -m_gt[5] +
                            rgrid.fSWY - rgrid.fResY / 2;
                        const double dfMaxY =
                            dfMinY +
                            static_cast<double>(rgrid.nHeight) * rgrid.fResY;

                        if ((oSupergrids.empty() ||
                             oSupergrids.find(yx(static_cast<int>(y),
                                                 static_cast<int>(x))) !=
                                 oSupergrids.end()) &&
                            (!bHasBoundingBoxFilter ||
                             (dfMinX >= dfFilterMinX &&
                              dfMinY >= dfFilterMinY &&
                              dfMaxX <= dfFilterMaxX &&
                              dfMaxY <= dfFilterMaxY)))
                        {
                            {
                                const int nIdx =
                                    m_aosSubdatasets.size() / 2 + 1;
                                m_aosSubdatasets.AddNameValue(
                                    CPLSPrintf("SUBDATASET_%d_NAME", nIdx),
                                    CPLSPrintf("BAG:\"%s\":supergrid:%d:%d",
                                               GetDescription(),
                                               static_cast<int>(y),
                                               static_cast<int>(x)));
                                m_aosSubdatasets.AddNameValue(
                                    CPLSPrintf("SUBDATASET_%d_DESC", nIdx),
                                    CPLSPrintf(
                                        "Supergrid (y=%d, x=%d) from "
                                        "(x=%f,y=%f) to "
                                        "(x=%f,y=%f), resolution (x=%f,y=%f)",
                                        static_cast<int>(y),
                                        static_cast<int>(x), dfMinX, dfMinY,
                                        dfMaxX, dfMaxY, rgrid.fResX,
                                        rgrid.fResY));
                            }

                            for (const auto &groupName : georefMDLayerNames)
                            {
                                const int nIdx =
                                    m_aosSubdatasets.size() / 2 + 1;
                                m_aosSubdatasets.AddNameValue(
                                    CPLSPrintf("SUBDATASET_%d_NAME", nIdx),
                                    CPLSPrintf(
                                        "BAG:\"%s\":georef_metadata:%s:%d:%d",
                                        GetDescription(), groupName.c_str(),
                                        static_cast<int>(y),
                                        static_cast<int>(x)));
                                m_aosSubdatasets.AddNameValue(
                                    CPLSPrintf("SUBDATASET_%d_DESC", nIdx),
                                    CPLSPrintf("Georeferenced metadata %s of "
                                               "supergrid (y=%d, x=%d)",
                                               groupName.c_str(),
                                               static_cast<int>(y),
                                               static_cast<int>(x)));
                            }
                        }
                    }
                }
            }
        }
    }

    if (!bOK || m_oMapRefinemendGrids.empty())
    {
        m_aosSubdatasets.Clear();
        m_oMapRefinemendGrids.clear();
        return false;
    }

    CPLDebug("BAG", "variable resolution extensions detected");
    return true;
}

/************************************************************************/
/*                            LoadMetadata()                            */
/************************************************************************/

void BAGDataset::LoadMetadata()

{
    // Load the metadata from the file.
    const hid_t hMDDS =
        H5Dopen(m_poSharedResources->m_hHDF5, "/BAG_root/metadata");
    const hid_t datatype = H5Dget_type(hMDDS);
    const hid_t dataspace = H5Dget_space(hMDDS);
    const hid_t native = H5Tget_native_type(datatype, H5T_DIR_ASCEND);

    const int n_dims = H5Sget_simple_extent_ndims(dataspace);
    hsize_t dims[1] = {static_cast<hsize_t>(0)};
    hsize_t maxdims[1] = {static_cast<hsize_t>(0)};

    if (n_dims == 1 && H5Tget_class(native) == H5T_STRING &&
        !H5Tis_variable_str(native) && H5Tget_size(native) == 1)
    {
        H5Sget_simple_extent_dims(dataspace, dims, maxdims);

        pszXMLMetadata =
            static_cast<char *>(CPLCalloc(static_cast<int>(dims[0] + 1), 1));

        H5Dread(hMDDS, native, H5S_ALL, dataspace, H5P_DEFAULT, pszXMLMetadata);
    }

    H5Tclose(native);
    H5Sclose(dataspace);
    H5Tclose(datatype);
    H5Dclose(hMDDS);

    if (pszXMLMetadata == nullptr || pszXMLMetadata[0] == 0)
        return;

    // Try to get the geotransform.
    CPLXMLNode *psRoot = CPLParseXMLString(pszXMLMetadata);

    if (psRoot == nullptr)
        return;

    CPLStripXMLNamespace(psRoot, nullptr, TRUE);

    CPLXMLNode *const psGeo = CPLSearchXMLNode(psRoot, "=MD_Georectified");

    if (psGeo != nullptr)
    {
        CPLString osResHeight, osResWidth;
        for (const auto *psIter = psGeo->psChild; psIter;
             psIter = psIter->psNext)
        {
            if (strcmp(psIter->pszValue, "axisDimensionProperties") == 0)
            {
                // since BAG format 1.5 version
                const char *pszDim = CPLGetXMLValue(
                    psIter,
                    "MD_Dimension.dimensionName.MD_DimensionNameTypeCode",
                    nullptr);
                const char *pszRes = nullptr;
                if (pszDim)
                {
                    pszRes = CPLGetXMLValue(
                        psIter, "MD_Dimension.resolution.Measure", nullptr);
                }
                else
                {
                    // prior to BAG format 1.5 version
                    pszDim = CPLGetXMLValue(
                        psIter, "MD_Dimension.dimensionName", nullptr);
                    pszRes = CPLGetXMLValue(
                        psIter, "MD_Dimension.resolution.Measure.value",
                        nullptr);
                }

                if (pszDim && EQUAL(pszDim, "row") && pszRes)
                {
                    osResHeight = pszRes;
                }
                else if (pszDim && EQUAL(pszDim, "column") && pszRes)
                {
                    osResWidth = pszRes;
                }
            }
        }

        char **papszCornerTokens = CSLTokenizeStringComplex(
            CPLGetXMLValue(psGeo, "cornerPoints.Point.coordinates", ""), " ,",
            FALSE, FALSE);

        if (CSLCount(papszCornerTokens) == 4)
        {
            const double dfLLX = CPLAtof(papszCornerTokens[0]);
            const double dfLLY = CPLAtof(papszCornerTokens[1]);
            const double dfURX = CPLAtof(papszCornerTokens[2]);
            const double dfURY = CPLAtof(papszCornerTokens[3]);

            double dfResWidth = CPLAtof(osResWidth);
            double dfResHeight = CPLAtof(osResHeight);
            if (dfResWidth > 0 && dfResHeight > 0)
            {
                if (fabs((dfURX - dfLLX) / dfResWidth - m_nLowResWidth) <
                        1e-2 &&
                    fabs((dfURY - dfLLY) / dfResHeight - m_nLowResHeight) <
                        1e-2)
                {
                    // Found with
                    // https://data.ngdc.noaa.gov/platforms/ocean/nos/coast/H12001-H14000/H12525/BAG/H12525_MB_4m_MLLW_1of2.bag
                    // to address issue
                    // https://github.com/OSGeo/gdal/issues/1643
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "cornerPoints not consistent with resolution "
                             "given in metadata");
                }
                else if (fabs((dfURX - dfLLX) / dfResWidth -
                              (m_nLowResWidth - 1)) < 1e-2 &&
                         fabs((dfURY - dfLLY) / dfResHeight -
                              (m_nLowResHeight - 1)) < 1e-2)
                {
                    // pixel center convention. OK
                }
                else
                {
                    CPLDebug("BAG", "cornerPoints not consistent with "
                                    "resolution given in metadata");
                    CPLDebug("BAG",
                             "Metadata horizontal resolution: %f. "
                             "Computed resolution: %f. "
                             "Computed width: %f vs %d",
                             dfResWidth, (dfURX - dfLLX) / (m_nLowResWidth - 1),
                             (dfURX - dfLLX) / dfResWidth, m_nLowResWidth);
                    CPLDebug("BAG",
                             "Metadata vertical resolution: %f. "
                             "Computed resolution: %f. "
                             "Computed height: %f vs %d",
                             dfResHeight,
                             (dfURY - dfLLY) / (m_nLowResHeight - 1),
                             (dfURY - dfLLY) / dfResHeight, m_nLowResHeight);
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "cornerPoints not consistent with resolution "
                             "given in metadata");
                }
            }

            m_gt[0] = dfLLX;
            m_gt[1] = dfResWidth;
            m_gt[3] = dfLLY + dfResHeight * (m_nLowResHeight - 1);
            m_gt[5] = dfResHeight * (-1);

            // shift to pixel corner convention
            m_gt[0] -= m_gt[1] * 0.5;
            m_gt[3] -= m_gt[5] * 0.5;

            m_dfLowResMinX = m_gt[0];
            m_dfLowResMaxX = m_dfLowResMinX + m_nLowResWidth * m_gt[1];
            m_dfLowResMaxY = m_gt[3];
            m_dfLowResMinY = m_dfLowResMaxY + m_nLowResHeight * m_gt[5];
        }
        CSLDestroy(papszCornerTokens);
    }

    // Try to get the coordinate system.
    if (OGR_SRS_ImportFromISO19115(&m_oSRS, pszXMLMetadata) != OGRERR_NONE)
    {
        ParseWKTFromXML(pszXMLMetadata);
    }

    // Fetch acquisition date.
    CPLXMLNode *const psDateTime = CPLSearchXMLNode(psRoot, "=dateTime");
    if (psDateTime != nullptr)
    {
        const char *pszDateTimeValue =
            psDateTime->psChild && psDateTime->psChild->eType == CXT_Element
                ? CPLGetXMLValue(psDateTime->psChild, nullptr, nullptr)
                : CPLGetXMLValue(psDateTime, nullptr, nullptr);
        if (pszDateTimeValue)
            GDALDataset::SetMetadataItem("BAG_DATETIME", pszDateTimeValue);
    }

    CPLDestroyXMLNode(psRoot);
}

/************************************************************************/
/*                          ParseWKTFromXML()                           */
/************************************************************************/
OGRErr BAGDataset::ParseWKTFromXML(const char *pszISOXML)
{
    CPLXMLNode *const psRoot = CPLParseXMLString(pszISOXML);

    if (psRoot == nullptr)
        return OGRERR_FAILURE;

    CPLStripXMLNamespace(psRoot, nullptr, TRUE);

    CPLXMLNode *psRSI = CPLSearchXMLNode(psRoot, "=referenceSystemInfo");
    if (psRSI == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Unable to find <referenceSystemInfo> in metadata.");
        CPLDestroyXMLNode(psRoot);
        return OGRERR_FAILURE;
    }

    const char *pszSRCodeString =
        CPLGetXMLValue(psRSI,
                       "MD_ReferenceSystem.referenceSystemIdentifier."
                       "RS_Identifier.code.CharacterString",
                       nullptr);
    if (pszSRCodeString == nullptr)
    {
        CPLDebug("BAG",
                 "Unable to find /MI_Metadata/referenceSystemInfo[1]/"
                 "MD_ReferenceSystem[1]/referenceSystemIdentifier[1]/"
                 "RS_Identifier[1]/code[1]/CharacterString[1] in metadata.");
        CPLDestroyXMLNode(psRoot);
        return OGRERR_FAILURE;
    }

    const char *pszSRCodeSpace =
        CPLGetXMLValue(psRSI,
                       "MD_ReferenceSystem.referenceSystemIdentifier."
                       "RS_Identifier.codeSpace.CharacterString",
                       "");
    if (!EQUAL(pszSRCodeSpace, "WKT"))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Spatial reference string is not in WKT.");
        CPLDestroyXMLNode(psRoot);
        return OGRERR_FAILURE;
    }

    if (m_oSRS.importFromWkt(pszSRCodeString) != OGRERR_NONE)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Failed parsing WKT string \"%s\".", pszSRCodeString);
        CPLDestroyXMLNode(psRoot);
        return OGRERR_FAILURE;
    }

    psRSI = CPLSearchXMLNode(psRSI->psNext, "=referenceSystemInfo");
    if (psRSI == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Unable to find second instance of <referenceSystemInfo> "
                 "in metadata.");
        CPLDestroyXMLNode(psRoot);
        return OGRERR_NONE;
    }

    pszSRCodeString =
        CPLGetXMLValue(psRSI,
                       "MD_ReferenceSystem.referenceSystemIdentifier."
                       "RS_Identifier.code.CharacterString",
                       nullptr);
    if (pszSRCodeString == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Unable to find /MI_Metadata/referenceSystemInfo[2]/"
                 "MD_ReferenceSystem[1]/referenceSystemIdentifier[1]/"
                 "RS_Identifier[1]/code[1]/CharacterString[1] in metadata.");
        CPLDestroyXMLNode(psRoot);
        return OGRERR_NONE;
    }

    pszSRCodeSpace =
        CPLGetXMLValue(psRSI,
                       "MD_ReferenceSystem.referenceSystemIdentifier."
                       "RS_Identifier.codeSpace.CharacterString",
                       "");
    if (!EQUAL(pszSRCodeSpace, "WKT"))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Spatial reference string is not in WKT.");
        CPLDestroyXMLNode(psRoot);
        return OGRERR_NONE;
    }

    if (m_bReportVertCRS && (STARTS_WITH_CI(pszSRCodeString, "VERTCS") ||
                             STARTS_WITH_CI(pszSRCodeString, "VERT_CS")))
    {
        OGR_SRSNode oVertCRSRootNode;
        const char *pszInput = pszSRCodeString;
        if (oVertCRSRootNode.importFromWkt(&pszInput) == OGRERR_NONE)
        {
            if (oVertCRSRootNode.GetNode("UNIT") == nullptr)
            {
                // UNIT is required
                auto poUnits = new OGR_SRSNode("UNIT");
                poUnits->AddChild(new OGR_SRSNode("metre"));
                poUnits->AddChild(new OGR_SRSNode("1.0"));
                oVertCRSRootNode.AddChild(poUnits);
            }
            if (oVertCRSRootNode.GetNode("AXIS") == nullptr)
            {
                // If AXIS is missing, add an explicit Depth AXIS
                auto poAxis = new OGR_SRSNode("AXIS");
                poAxis->AddChild(new OGR_SRSNode("Depth"));
                poAxis->AddChild(new OGR_SRSNode("DOWN"));
                oVertCRSRootNode.AddChild(poAxis);
            }

            char *pszVertCRSWKT = nullptr;
            oVertCRSRootNode.exportToWkt(&pszVertCRSWKT);

            OGRSpatialReference oVertCRS;
            if (oVertCRS.importFromWkt(pszVertCRSWKT) == OGRERR_NONE)
            {
                if (EQUAL(oVertCRS.GetName(), "MLLW"))
                {
                    oVertCRS.importFromEPSG(5866);
                }

                OGRSpatialReference oCompoundCRS;
                oCompoundCRS.SetCompoundCS(
                    (CPLString(m_oSRS.GetName()) + " + " + oVertCRS.GetName())
                        .c_str(),
                    &m_oSRS, &oVertCRS);
                oCompoundCRS.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

                m_oSRS = std::move(oCompoundCRS);
            }

            CPLFree(pszVertCRSWKT);
        }
    }

    CPLDestroyXMLNode(psRoot);

    return OGRERR_NONE;
}

/************************************************************************/
/*                          GetGeoTransform()                           */
/************************************************************************/

CPLErr BAGDataset::GetGeoTransform(GDALGeoTransform &gt) const

{
    if (m_gt[0] != 0.0 || m_gt[3] != 0.0)
    {
        gt = m_gt;
        return CE_None;
    }

    return GDALPamDataset::GetGeoTransform(gt);
}

/************************************************************************/
/*                         GetSpatialRef()                              */
/************************************************************************/

const OGRSpatialReference *BAGDataset::GetSpatialRef() const
{
    if (!m_oSRS.IsEmpty())
        return &m_oSRS;
    return GDALPamDataset::GetSpatialRef();
}

/************************************************************************/
/*                          SetGeoTransform()                           */
/************************************************************************/

CPLErr BAGDataset::SetGeoTransform(const GDALGeoTransform &gt)
{
    if (eAccess == GA_ReadOnly)
        return GDALPamDataset::SetGeoTransform(gt);

    if (gt[2] != 0 || gt[4] != 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "BAG driver requires a non-rotated geotransform");
        return CE_Failure;
    }
    m_gt = gt;
    return WriteMetadataIfNeeded() ? CE_None : CE_Failure;
}

/************************************************************************/
/*                           SetSpatialRef()                            */
/************************************************************************/

CPLErr BAGDataset::SetSpatialRef(const OGRSpatialReference *poSRS)
{
    if (eAccess == GA_ReadOnly)
        return GDALPamDataset::SetSpatialRef(poSRS);

    if (poSRS == nullptr || poSRS->IsEmpty())
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "BAG driver requires a valid SRS");
        return CE_Failure;
    }

    m_oSRS = *poSRS;
    return WriteMetadataIfNeeded() ? CE_None : CE_Failure;
}

/************************************************************************/
/*                         WriteMetadataIfNeeded()                      */
/************************************************************************/

bool BAGDataset::WriteMetadataIfNeeded()
{
    if (m_bMetadataWritten)
    {
        return true;
    }
    if (m_gt == GDALGeoTransform() || m_oSRS.IsEmpty())
    {
        return true;
    }
    m_bMetadataWritten = true;

    CPLString osXMLMetadata = BAGCreator::GenerateMetadata(
        nRasterXSize, nRasterYSize, m_gt, m_oSRS.IsEmpty() ? nullptr : &m_oSRS,
        m_aosCreationOptions.List());
    if (osXMLMetadata.empty())
    {
        return false;
    }

    if (!BAGCreator::CreateAndWriteMetadata(m_poSharedResources->m_hHDF5,
                                            osXMLMetadata))
    {
        return false;
    }

    return true;
}

/************************************************************************/
/*                      GetMetadataDomainList()                         */
/************************************************************************/

char **BAGDataset::GetMetadataDomainList()
{
    return BuildMetadataDomainList(GDALPamDataset::GetMetadataDomainList(),
                                   TRUE, "xml:BAG", nullptr);
}

/************************************************************************/
/*                            GetMetadata()                             */
/************************************************************************/

char **BAGDataset::GetMetadata(const char *pszDomain)

{
    if (pszDomain != nullptr && EQUAL(pszDomain, "xml:BAG"))
    {
        apszMDList[0] = pszXMLMetadata;
        apszMDList[1] = nullptr;

        return apszMDList;
    }

    if (pszDomain != nullptr && EQUAL(pszDomain, "SUBDATASETS"))
    {
        return m_aosSubdatasets.List();
    }

    return GDALPamDataset::GetMetadata(pszDomain);
}

/************************************************************************/
/*                      BAGDatasetDriverUnload()                        */
/************************************************************************/

static void BAGDatasetDriverUnload(GDALDriver *)
{
    HDF5UnloadFileDriver();
}

/************************************************************************/
/*                            ~BAGCreator()()                           */
/************************************************************************/

BAGCreator::~BAGCreator()
{
    Close();
}

/************************************************************************/
/*                               Close()                                */
/************************************************************************/

bool BAGCreator::Close()
{
    bool ret = true;
    if (m_bagRoot >= 0)
    {
        ret = (H5_CHECK(H5Gclose(m_bagRoot)) >= 0) && ret;
        m_bagRoot = -1;
    }
    if (m_hdf5 >= 0)
    {
        ret = (H5_CHECK(H5Fclose(m_hdf5)) >= 0) && ret;
        m_hdf5 = -1;
    }
    return ret;
}

/************************************************************************/
/*                         SubstituteVariables()                        */
/************************************************************************/

bool BAGCreator::SubstituteVariables(CPLXMLNode *psNode, char **papszDict)
{
    if (psNode->eType == CXT_Text && psNode->pszValue &&
        strstr(psNode->pszValue, "${"))
    {
        CPLString osVal(psNode->pszValue);
        size_t nPos = 0;
        while (true)
        {
            nPos = osVal.find("${", nPos);
            if (nPos == std::string::npos)
            {
                break;
            }
            CPLString osKeyName;
            bool bHasDefaultValue = false;
            CPLString osDefaultValue;
            size_t nAfterKeyName = 0;
            for (size_t i = nPos + 2; i < osVal.size(); i++)
            {
                if (osVal[i] == ':')
                {
                    osKeyName = osVal.substr(nPos + 2, i - (nPos + 2));
                }
                else if (osVal[i] == '}')
                {
                    if (osKeyName.empty())
                    {
                        osKeyName = osVal.substr(nPos + 2, i - (nPos + 2));
                    }
                    else
                    {
                        bHasDefaultValue = true;
                        size_t nStartDefaultVal =
                            nPos + 2 + osKeyName.size() + 1;
                        osDefaultValue = osVal.substr(nStartDefaultVal,
                                                      i - nStartDefaultVal);
                    }
                    nAfterKeyName = i + 1;
                    break;
                }
            }
            if (nAfterKeyName == 0)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Invalid variable name in template");
                return false;
            }

            bool bSubstFound = false;
            for (char **papszIter = papszDict;
                 !bSubstFound && papszIter && *papszIter; papszIter++)
            {
                if (STARTS_WITH_CI(*papszIter, "VAR_"))
                {
                    char *pszKey = nullptr;
                    const char *pszValue =
                        CPLParseNameValue(*papszIter, &pszKey);
                    if (pszKey && pszValue)
                    {
                        const char *pszVarName = pszKey + strlen("VAR_");
                        if (EQUAL(pszVarName, osKeyName))
                        {
                            bSubstFound = true;
                            osVal = osVal.substr(0, nPos) + pszValue +
                                    osVal.substr(nAfterKeyName);
                        }
                        CPLFree(pszKey);
                    }
                }
            }
            if (!bSubstFound)
            {
                if (bHasDefaultValue)
                {
                    osVal = osVal.substr(0, nPos) + osDefaultValue +
                            osVal.substr(nAfterKeyName);
                }
                else
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "%s could not be substituted", osKeyName.c_str());
                    return false;
                }
            }
        }

        if (!osVal.empty() && osVal[0] == '<' && osVal.back() == '>')
        {
            CPLXMLNode *psSubNode = CPLParseXMLString(osVal);
            if (psSubNode)
            {
                CPLFree(psNode->pszValue);
                psNode->eType = psSubNode->eType;
                psNode->pszValue = psSubNode->pszValue;
                psNode->psChild = psSubNode->psChild;
                psSubNode->pszValue = nullptr;
                psSubNode->psChild = nullptr;
                CPLDestroyXMLNode(psSubNode);
            }
            else
            {
                CPLFree(psNode->pszValue);
                psNode->pszValue = CPLStrdup(osVal);
            }
        }
        else
        {
            CPLFree(psNode->pszValue);
            psNode->pszValue = CPLStrdup(osVal);
        }
    }

    for (CPLXMLNode *psIter = psNode->psChild; psIter; psIter = psIter->psNext)
    {
        if (!SubstituteVariables(psIter, papszDict))
            return false;
    }
    return true;
}

/************************************************************************/
/*                          GenerateMetadata()                          */
/************************************************************************/

CPLString BAGCreator::GenerateMetadata(int nXSize, int nYSize,
                                       const GDALGeoTransform &gt,
                                       const OGRSpatialReference *poSRS,
                                       char **papszOptions)
{
    CPLXMLNode *psRoot;
    CPLString osTemplateFilename =
        CSLFetchNameValueDef(papszOptions, "TEMPLATE", "");
    if (!osTemplateFilename.empty())
    {
        psRoot = CPLParseXMLFile(osTemplateFilename);
    }
    else
    {
#ifndef USE_ONLY_EMBEDDED_RESOURCE_FILES
#ifdef EMBED_RESOURCE_FILES
        CPLErrorStateBackuper oErrorStateBackuper(CPLQuietErrorHandler);
#endif
        const char *pszDefaultTemplateFilename =
            CPLFindFile("gdal", "bag_template.xml");
        if (pszDefaultTemplateFilename == nullptr)
#endif
        {
#ifdef EMBED_RESOURCE_FILES
            static const bool bOnce [[maybe_unused]] = []()
            {
                CPLDebug("BAG", "Using embedded bag_template.xml");
                return true;
            }();
            psRoot = CPLParseXMLString(BAGGetEmbeddedTemplateFile());
#else
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot find bag_template.xml and TEMPLATE "
                     "creation option not specified");
            return CPLString();
#endif
        }
#ifndef USE_ONLY_EMBEDDED_RESOURCE_FILES
        else
        {
            psRoot = CPLParseXMLFile(pszDefaultTemplateFilename);
        }
#endif
    }
    if (psRoot == nullptr)
        return CPLString();
    CPLXMLTreeCloser oCloser(psRoot);
    CPL_IGNORE_RET_VAL(oCloser);

    CPLXMLNode *psMain = psRoot;
    for (; psMain; psMain = psMain->psNext)
    {
        if (psMain->eType == CXT_Element && !STARTS_WITH(psMain->pszValue, "?"))
        {
            break;
        }
    }
    if (psMain == nullptr)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Cannot find main XML node");
        return CPLString();
    }

    CPLStringList osOptions(papszOptions, FALSE);
    if (osOptions.FetchNameValue("VAR_PROCESS_STEP_DESCRIPTION") == nullptr)
    {
        osOptions.SetNameValue("VAR_PROCESS_STEP_DESCRIPTION",
                               CPLSPrintf("Generated by GDAL %s",
                                          GDALVersionInfo("RELEASE_NAME")));
    }
    osOptions.SetNameValue("VAR_HEIGHT", CPLSPrintf("%d", nYSize));
    osOptions.SetNameValue("VAR_WIDTH", CPLSPrintf("%d", nXSize));

    struct tm brokenDown;
    CPLUnixTimeToYMDHMS(time(nullptr), &brokenDown);
    if (osOptions.FetchNameValue("VAR_DATE") == nullptr)
    {
        osOptions.SetNameValue(
            "VAR_DATE", CPLSPrintf("%04d-%02d-%02d", brokenDown.tm_year + 1900,
                                   brokenDown.tm_mon + 1, brokenDown.tm_mday));
    }
    if (osOptions.FetchNameValue("VAR_DATETIME") == nullptr)
    {
        osOptions.SetNameValue(
            "VAR_DATETIME",
            CPLSPrintf("%04d-%02d-%02dT%02d:%02d:%02d",
                       brokenDown.tm_year + 1900, brokenDown.tm_mon + 1,
                       brokenDown.tm_mday, brokenDown.tm_hour,
                       brokenDown.tm_min, brokenDown.tm_sec));
    }

    osOptions.SetNameValue("VAR_RESX", CPLSPrintf("%.17g", gt[1]));
    osOptions.SetNameValue("VAR_RESY", CPLSPrintf("%.17g", fabs(gt[5])));
    osOptions.SetNameValue("VAR_RES",
                           CPLSPrintf("%.17g", std::max(gt[1], fabs(gt[5]))));

    char *pszProjection = nullptr;
    if (poSRS)
        poSRS->exportToWkt(&pszProjection);
    if (pszProjection == nullptr || EQUAL(pszProjection, ""))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "BAG driver requires a source dataset with a projection");
    }
    osOptions.SetNameValue("VAR_HORIZ_WKT", pszProjection);
    CPLFree(pszProjection);

    OGRSpatialReference oSRS;
    if (poSRS)
        oSRS = *poSRS;
    if (oSRS.IsCompound())
    {
        auto node = oSRS.GetRoot();
        if (node && node->GetChildCount() == 3)
        {
            char *pszHorizWKT = nullptr;
            node->GetChild(1)->exportToWkt(&pszHorizWKT);
            char *pszVertWKT = nullptr;
            node->GetChild(2)->exportToWkt(&pszVertWKT);

            oSRS.StripVertical();

            osOptions.SetNameValue("VAR_HORIZ_WKT", pszHorizWKT);
            if (osOptions.FetchNameValue("VAR_VERT_WKT") == nullptr)
            {
                osOptions.SetNameValue("VAR_VERT_WKT", pszVertWKT);
            }
            CPLFree(pszHorizWKT);
            CPLFree(pszVertWKT);
        }
    }

    const char *pszUnits = "m";
    if (oSRS.IsProjected())
    {
        oSRS.GetLinearUnits(&pszUnits);
        if (EQUAL(pszUnits, "metre"))
            pszUnits = "m";
    }
    else
    {
        pszUnits = "deg";
    }
    osOptions.SetNameValue("VAR_RES_UNIT", pszUnits);

    // get bounds as pixel center
    double dfMinX = gt[0] + gt[1] / 2;
    double dfMaxX = dfMinX + (nXSize - 1) * gt[1];
    double dfMaxY = gt[3] + gt[5] / 2;
    double dfMinY = dfMaxY + (nYSize - 1) * gt[5];

    if (gt[5] > 0)
    {
        std::swap(dfMinY, dfMaxY);
    }
    osOptions.SetNameValue(
        "VAR_CORNER_POINTS",
        CPLSPrintf("%.17g,%.17g %.17g,%.17g", dfMinX, dfMinY, dfMaxX, dfMaxY));

    double adfCornerX[4] = {dfMinX, dfMinX, dfMaxX, dfMaxX};
    double adfCornerY[4] = {dfMinY, dfMaxY, dfMaxY, dfMinY};
    OGRSpatialReference oSRS_WGS84;
    oSRS_WGS84.SetFromUserInput("WGS84");
    oSRS_WGS84.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRCoordinateTransformation *poCT =
        OGRCreateCoordinateTransformation(&oSRS, &oSRS_WGS84);
    if (!poCT)
        return CPLString();
    if (!poCT->Transform(4, adfCornerX, adfCornerY))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot compute raster extent in geodetic coordinates");
        delete poCT;
        return CPLString();
    }
    delete poCT;
    double dfWest = std::min(std::min(adfCornerX[0], adfCornerX[1]),
                             std::min(adfCornerX[2], adfCornerX[3]));
    double dfSouth = std::min(std::min(adfCornerY[0], adfCornerY[1]),
                              std::min(adfCornerY[2], adfCornerY[3]));
    double dfEast = std::max(std::max(adfCornerX[0], adfCornerX[1]),
                             std::max(adfCornerX[2], adfCornerX[3]));
    double dfNorth = std::max(std::max(adfCornerY[0], adfCornerY[1]),
                              std::max(adfCornerY[2], adfCornerY[3]));
    osOptions.SetNameValue("VAR_WEST_LONGITUDE", CPLSPrintf("%.17g", dfWest));
    osOptions.SetNameValue("VAR_SOUTH_LATITUDE", CPLSPrintf("%.17g", dfSouth));
    osOptions.SetNameValue("VAR_EAST_LONGITUDE", CPLSPrintf("%.17g", dfEast));
    osOptions.SetNameValue("VAR_NORTH_LATITUDE", CPLSPrintf("%.17g", dfNorth));

    if (!SubstituteVariables(psMain, osOptions.List()))
    {
        return CPLString();
    }

    char *pszXML = CPLSerializeXMLTree(psRoot);
    CPLString osXML(pszXML);
    CPLFree(pszXML);
    return osXML;
}

/************************************************************************/
/*                         CreateAndWriteMetadata()                     */
/************************************************************************/

bool BAGCreator::CreateAndWriteMetadata(hid_t hdf5,
                                        const CPLString &osXMLMetadata)
{
    hsize_t dim_init[1] = {1 + osXMLMetadata.size()};
    hsize_t dim_max[1] = {H5S_UNLIMITED};

    hid_t hDataSpace = H5_CHECK(H5Screate_simple(1, dim_init, dim_max));
    if (hDataSpace < 0)
        return false;

    hid_t hParams = -1;
    hid_t hDataType = -1;
    hid_t hDatasetID = -1;
    hid_t hFileSpace = -1;
    bool ret = false;
    do
    {
        hParams = H5_CHECK(H5Pcreate(H5P_DATASET_CREATE));
        if (hParams < 0)
            break;

        hsize_t chunk_dims[1] = {1024};
        if (H5_CHECK(H5Pset_chunk(hParams, 1, chunk_dims)) < 0)
            break;

        hDataType = H5_CHECK(H5Tcopy(H5T_C_S1));
        if (hDataType < 0)
            break;

        hDatasetID = H5_CHECK(H5Dcreate(hdf5, "/BAG_root/metadata", hDataType,
                                        hDataSpace, hParams));
        if (hDatasetID < 0)
            break;

        if (H5_CHECK(H5Dextend(hDatasetID, dim_init)) < 0)
            break;

        hFileSpace = H5_CHECK(H5Dget_space(hDatasetID));
        if (hFileSpace < 0)
            break;

        H5OFFSET_TYPE offset[1] = {0};
        if (H5_CHECK(H5Sselect_hyperslab(hFileSpace, H5S_SELECT_SET, offset,
                                         nullptr, dim_init, nullptr)) < 0)
        {
            break;
        }

        if (H5_CHECK(H5Dwrite(hDatasetID, hDataType, hDataSpace, hFileSpace,
                              H5P_DEFAULT, osXMLMetadata.data())) < 0)
        {
            break;
        }

        ret = true;
    } while (0);

    if (hParams >= 0)
        H5_CHECK(H5Pclose(hParams));
    if (hDataType >= 0)
        H5_CHECK(H5Tclose(hDataType));
    if (hFileSpace >= 0)
        H5_CHECK(H5Sclose(hFileSpace));
    if (hDatasetID >= 0)
        H5_CHECK(H5Dclose(hDatasetID));
    H5_CHECK(H5Sclose(hDataSpace));

    return ret;
}

/************************************************************************/
/*                       CreateTrackingListDataset()                    */
/************************************************************************/

bool BAGCreator::CreateTrackingListDataset()
{
    typedef struct
    {
        uint32_t row;
        uint32_t col;
        float depth;
        float uncertainty;
        uint8_t track_code;
        uint16_t list_series;
    } TrackingListItem;

    hsize_t dim_init[1] = {0};
    hsize_t dim_max[1] = {H5S_UNLIMITED};

    hid_t hDataSpace = H5_CHECK(H5Screate_simple(1, dim_init, dim_max));
    if (hDataSpace < 0)
        return false;

    hid_t hParams = -1;
    hid_t hDataType = -1;
    hid_t hDatasetID = -1;
    bool ret = false;
    do
    {
        hParams = H5_CHECK(H5Pcreate(H5P_DATASET_CREATE));
        if (hParams < 0)
            break;

        hsize_t chunk_dims[1] = {10};
        if (H5_CHECK(H5Pset_chunk(hParams, 1, chunk_dims)) < 0)
            break;

        TrackingListItem unusedItem;
        (void)unusedItem.row;
        (void)unusedItem.col;
        (void)unusedItem.depth;
        (void)unusedItem.uncertainty;
        (void)unusedItem.track_code;
        (void)unusedItem.list_series;
        hDataType = H5_CHECK(H5Tcreate(H5T_COMPOUND, sizeof(unusedItem)));
        if (hDataType < 0)
            break;

        if (H5Tinsert(hDataType, "row", HOFFSET(TrackingListItem, row),
                      H5T_NATIVE_UINT) < 0 ||
            H5Tinsert(hDataType, "col", HOFFSET(TrackingListItem, col),
                      H5T_NATIVE_UINT) < 0 ||
            H5Tinsert(hDataType, "depth", HOFFSET(TrackingListItem, depth),
                      H5T_NATIVE_FLOAT) < 0 ||
            H5Tinsert(hDataType, "uncertainty",
                      HOFFSET(TrackingListItem, uncertainty),
                      H5T_NATIVE_FLOAT) < 0 ||
            H5Tinsert(hDataType, "track_code",
                      HOFFSET(TrackingListItem, track_code),
                      H5T_NATIVE_UCHAR) < 0 ||
            H5Tinsert(hDataType, "list_series",
                      HOFFSET(TrackingListItem, list_series),
                      H5T_NATIVE_SHORT) < 0)
        {
            break;
        }

        hDatasetID = H5_CHECK(H5Dcreate(m_hdf5, "/BAG_root/tracking_list",
                                        hDataType, hDataSpace, hParams));
        if (hDatasetID < 0)
            break;

        if (H5_CHECK(H5Dextend(hDatasetID, dim_init)) < 0)
            break;

        if (!GH5_CreateAttribute(hDatasetID, "Tracking List Length",
                                 H5T_NATIVE_UINT))
            break;

        if (!GH5_WriteAttribute(hDatasetID, "Tracking List Length", 0U))
            break;

        ret = true;
    } while (0);

    if (hParams >= 0)
        H5_CHECK(H5Pclose(hParams));
    if (hDataType >= 0)
        H5_CHECK(H5Tclose(hDataType));
    if (hDatasetID >= 0)
        H5_CHECK(H5Dclose(hDatasetID));
    H5_CHECK(H5Sclose(hDataSpace));

    return ret;
}

/************************************************************************/
/*                     CreateElevationOrUncertainty()                   */
/************************************************************************/

bool BAGCreator::CreateElevationOrUncertainty(
    GDALDataset *poSrcDS, int nBand, const char *pszDSName,
    const char *pszMaxAttrName, const char *pszMinAttrName, char **papszOptions,
    GDALProgressFunc pfnProgress, void *pProgressData)
{
    const int nYSize = poSrcDS->GetRasterYSize();
    const int nXSize = poSrcDS->GetRasterXSize();

    GDALGeoTransform gt;
    poSrcDS->GetGeoTransform(gt);

    hsize_t dims[2] = {static_cast<hsize_t>(nYSize),
                       static_cast<hsize_t>(nXSize)};

    hid_t hDataSpace = H5_CHECK(H5Screate_simple(2, dims, nullptr));
    if (hDataSpace < 0)
        return false;

    hid_t hParams = -1;
    hid_t hDataType = -1;
    hid_t hDatasetID = -1;
    hid_t hFileSpace = -1;
    bool bDeflate = EQUAL(
        CSLFetchNameValueDef(papszOptions, "COMPRESS", "DEFLATE"), "DEFLATE");
    int nCompressionLevel =
        atoi(CSLFetchNameValueDef(papszOptions, "ZLEVEL", "6"));
    const int nBlockSize = std::min(
        4096, atoi(CSLFetchNameValueDef(papszOptions, "BLOCK_SIZE", "100")));
    const int nBlockXSize = std::min(nXSize, nBlockSize);
    const int nBlockYSize = std::min(nYSize, nBlockSize);
    bool ret = false;
    const float fNoDataValue = fDEFAULT_NODATA;
    do
    {
        hDataType = H5_CHECK(H5Tcopy(H5T_NATIVE_FLOAT));
        if (hDataType < 0)
            break;

        if (H5_CHECK(H5Tset_order(hDataType, H5T_ORDER_LE)) < 0)
            break;

        hParams = H5_CHECK(H5Pcreate(H5P_DATASET_CREATE));
        if (hParams < 0)
            break;

        if (H5_CHECK(H5Pset_fill_time(hParams, H5D_FILL_TIME_ALLOC)) < 0)
            break;

        if (H5_CHECK(H5Pset_fill_value(hParams, hDataType, &fNoDataValue)) < 0)
            break;

        if (H5_CHECK(H5Pset_layout(hParams, H5D_CHUNKED)) < 0)
            break;
        hsize_t chunk_size[2] = {static_cast<hsize_t>(nBlockYSize),
                                 static_cast<hsize_t>(nBlockXSize)};
        if (H5_CHECK(H5Pset_chunk(hParams, 2, chunk_size)) < 0)
            break;

        if (bDeflate)
        {
            if (H5_CHECK(H5Pset_deflate(hParams, nCompressionLevel)) < 0)
                break;
        }

        hDatasetID = H5_CHECK(
            H5Dcreate(m_hdf5, pszDSName, hDataType, hDataSpace, hParams));
        if (hDatasetID < 0)
            break;

        if (!GH5_CreateAttribute(hDatasetID, pszMaxAttrName, hDataType))
            break;

        if (!GH5_CreateAttribute(hDatasetID, pszMinAttrName, hDataType))
            break;

        hFileSpace = H5_CHECK(H5Dget_space(hDatasetID));
        if (hFileSpace < 0)
            break;

        const int nYBlocks =
            static_cast<int>(DIV_ROUND_UP(nYSize, nBlockYSize));
        const int nXBlocks =
            static_cast<int>(DIV_ROUND_UP(nXSize, nBlockXSize));
        std::vector<float> afValues(static_cast<size_t>(nBlockYSize) *
                                    nBlockXSize);
        ret = true;
        const bool bReverseY = gt[5] < 0;

        float fMin = std::numeric_limits<float>::infinity();
        float fMax = -std::numeric_limits<float>::infinity();

        if (nBand == 1 || poSrcDS->GetRasterCount() == 2)
        {
            int bHasNoData = FALSE;
            const double dfSrcNoData =
                poSrcDS->GetRasterBand(nBand)->GetNoDataValue(&bHasNoData);
            const float fSrcNoData = static_cast<float>(dfSrcNoData);

            for (int iY = 0; ret && iY < nYBlocks; iY++)
            {
                const int nSrcYOff =
                    bReverseY ? std::max(0, nYSize - (iY + 1) * nBlockYSize)
                              : iY * nBlockYSize;
                const int nReqCountY =
                    std::min(nBlockYSize, nYSize - iY * nBlockYSize);
                for (int iX = 0; iX < nXBlocks; iX++)
                {
                    const int nReqCountX =
                        std::min(nBlockXSize, nXSize - iX * nBlockXSize);

                    if (poSrcDS->GetRasterBand(nBand)->RasterIO(
                            GF_Read, iX * nBlockXSize, nSrcYOff, nReqCountX,
                            nReqCountY,
                            bReverseY ? afValues.data() +
                                            (nReqCountY - 1) * nReqCountX
                                      : afValues.data(),
                            nReqCountX, nReqCountY, GDT_Float32, 0,
                            bReverseY ? -4 * nReqCountX : 0,
                            nullptr) != CE_None)
                    {
                        ret = false;
                        break;
                    }

                    for (int i = 0; i < nReqCountY * nReqCountX; i++)
                    {
                        const float fVal = afValues[i];
                        if ((bHasNoData && fVal == fSrcNoData) ||
                            std::isnan(fVal))
                        {
                            afValues[i] = fNoDataValue;
                        }
                        else
                        {
                            fMin = std::min(fMin, fVal);
                            fMax = std::max(fMax, fVal);
                        }
                    }

                    H5OFFSET_TYPE offset[2] = {
                        static_cast<H5OFFSET_TYPE>(iY) *
                            static_cast<H5OFFSET_TYPE>(nBlockYSize),
                        static_cast<H5OFFSET_TYPE>(iX) *
                            static_cast<H5OFFSET_TYPE>(nBlockXSize)};
                    hsize_t count[2] = {static_cast<hsize_t>(nReqCountY),
                                        static_cast<hsize_t>(nReqCountX)};
                    if (H5_CHECK(H5Sselect_hyperslab(hFileSpace, H5S_SELECT_SET,
                                                     offset, nullptr, count,
                                                     nullptr)) < 0)
                    {
                        ret = false;
                        break;
                    }

                    hid_t hMemSpace = H5Screate_simple(2, count, nullptr);
                    if (hMemSpace < 0)
                        break;

                    if (H5_CHECK(H5Dwrite(hDatasetID, H5T_NATIVE_FLOAT,
                                          hMemSpace, hFileSpace, H5P_DEFAULT,
                                          afValues.data())) < 0)
                    {
                        H5Sclose(hMemSpace);
                        ret = false;
                        break;
                    }

                    H5Sclose(hMemSpace);

                    if (!pfnProgress(
                            (static_cast<double>(iY) * nXBlocks + iX + 1) /
                                (static_cast<double>(nXBlocks) * nYBlocks),
                            "", pProgressData))
                    {
                        ret = false;
                        break;
                    }
                }
            }
        }
        if (!ret)
            break;

        if (fMin > fMax)
            fMin = fMax = fNoDataValue;

        if (!GH5_WriteAttribute(hDatasetID, pszMaxAttrName, fMax))
            break;

        if (!GH5_WriteAttribute(hDatasetID, pszMinAttrName, fMin))
            break;

        ret = true;
    } while (0);

    if (hParams >= 0)
        H5_CHECK(H5Pclose(hParams));
    if (hDataType >= 0)
        H5_CHECK(H5Tclose(hDataType));
    if (hFileSpace >= 0)
        H5_CHECK(H5Sclose(hFileSpace));
    if (hDatasetID >= 0)
        H5_CHECK(H5Dclose(hDatasetID));
    H5_CHECK(H5Sclose(hDataSpace));

    return ret;
}

/************************************************************************/
/*                           CreateBase()                               */
/************************************************************************/

bool BAGCreator::CreateBase(const char *pszFilename, char **papszOptions)
{
    hid_t fapl = H5Pcreate(H5P_FILE_ACCESS);
    H5Pset_driver(fapl, HDF5GetFileDriver(), nullptr);
    m_hdf5 = H5Fcreate(pszFilename, H5F_ACC_TRUNC, H5P_DEFAULT, fapl);
    H5Pclose(fapl);
    if (m_hdf5 < 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Cannot create file");
        return false;
    }

    m_bagRoot = H5_CHECK(H5Gcreate(m_hdf5, "/BAG_root", 0));
    if (m_bagRoot < 0)
    {
        return false;
    }

    const char *pszVersion =
        CSLFetchNameValueDef(papszOptions, "BAG_VERSION", "1.6.2");
    constexpr unsigned knVersionLength = 32;
    char szVersion[knVersionLength] = {};
    snprintf(szVersion, sizeof(szVersion), "%s", pszVersion);
    if (!GH5_CreateAttribute(m_bagRoot, "Bag Version", H5T_C_S1,
                             knVersionLength) ||
        !GH5_WriteAttribute(m_bagRoot, "Bag Version", szVersion))
    {
        return false;
    }

    if (!CreateTrackingListDataset())
    {
        return false;
    }
    return true;
}

/************************************************************************/
/*                               Create()                               */
/************************************************************************/

bool BAGCreator::Create(const char *pszFilename, GDALDataset *poSrcDS,
                        char **papszOptions, GDALProgressFunc pfnProgress,
                        void *pProgressData)
{
    const int nBands = poSrcDS->GetRasterCount();
    if (nBands != 1 && nBands != 2)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "BAG driver doesn't support %d bands. Must be 1 or 2.",
                 nBands);
        return false;
    }
    GDALGeoTransform gt;
    if (poSrcDS->GetGeoTransform(gt) != CE_None)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "BAG driver requires a source dataset with a geotransform");
        return false;
    }
    if (gt[2] != 0 || gt[4] != 0)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "BAG driver requires a source dataset with a non-rotated "
                 "geotransform");
        return false;
    }

    CPLString osXMLMetadata =
        GenerateMetadata(poSrcDS->GetRasterXSize(), poSrcDS->GetRasterYSize(),
                         gt, poSrcDS->GetSpatialRef(), papszOptions);
    if (osXMLMetadata.empty())
    {
        return false;
    }

    if (!CreateBase(pszFilename, papszOptions))
    {
        return false;
    }

    if (!CreateAndWriteMetadata(m_hdf5, osXMLMetadata))
    {
        return false;
    }

    void *pScaled = GDALCreateScaledProgress(0, 1. / poSrcDS->GetRasterCount(),
                                             pfnProgress, pProgressData);
    bool bRet;
    bRet = CreateElevationOrUncertainty(
        poSrcDS, 1, "/BAG_root/elevation", "Maximum Elevation Value",
        "Minimum Elevation Value", papszOptions, GDALScaledProgress, pScaled);
    GDALDestroyScaledProgress(pScaled);
    if (!bRet)
    {
        return false;
    }

    pScaled = GDALCreateScaledProgress(1. / poSrcDS->GetRasterCount(), 1.0,
                                       pfnProgress, pProgressData);
    bRet = CreateElevationOrUncertainty(
        poSrcDS, 2, "/BAG_root/uncertainty", "Maximum Uncertainty Value",
        "Minimum Uncertainty Value", papszOptions, GDALScaledProgress, pScaled);
    GDALDestroyScaledProgress(pScaled);
    if (!bRet)
    {
        return false;
    }

    return Close();
}

/************************************************************************/
/*                               Create()                               */
/************************************************************************/

bool BAGCreator::Create(const char *pszFilename, int nBands, GDALDataType eType,
                        char **papszOptions)
{
    if (nBands != 1 && nBands != 2)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "BAG driver doesn't support %d bands. Must be 1 or 2.",
                 nBands);
        return false;
    }
    if (eType != GDT_Float32)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "BAG driver only supports Float32");
        return false;
    }

    if (!CreateBase(pszFilename, papszOptions))
    {
        return false;
    }

    return Close();
}

/************************************************************************/
/*                              CreateCopy()                            */
/************************************************************************/

GDALDataset *BAGDataset::CreateCopy(const char *pszFilename,
                                    GDALDataset *poSrcDS, int /* bStrict */,
                                    char **papszOptions,
                                    GDALProgressFunc pfnProgress,
                                    void *pProgressData)

{
    if (!BAGCreator().Create(pszFilename, poSrcDS, papszOptions, pfnProgress,
                             pProgressData))
    {
        return nullptr;
    }

    GDALOpenInfo oOpenInfo(pszFilename, GA_ReadOnly);
    oOpenInfo.nOpenFlags = GDAL_OF_RASTER;
    return Open(&oOpenInfo);
}

/************************************************************************/
/*                               Create()                               */
/************************************************************************/

GDALDataset *BAGDataset::Create(const char *pszFilename, int nXSize, int nYSize,
                                int nBandsIn, GDALDataType eType,
                                char **papszOptions)
{
    if (!BAGCreator().Create(pszFilename, nBandsIn, eType, papszOptions))
    {
        return nullptr;
    }

    GDALOpenInfo oOpenInfo(pszFilename, GA_Update);
    oOpenInfo.nOpenFlags = GDAL_OF_RASTER;
    return OpenForCreate(&oOpenInfo, nXSize, nYSize, nBandsIn, papszOptions);
}

/************************************************************************/
/*                          GDALRegister_BAG()                          */
/************************************************************************/
void GDALRegister_BAG()

{
    if (!GDAL_CHECK_VERSION("BAG"))
        return;

    if (GDALGetDriverByName(BAG_DRIVER_NAME) != nullptr)
        return;

    GDALDriver *poDriver = new GDALDriver();

    BAGDriverSetCommonMetadata(poDriver);

    poDriver->pfnOpen = BAGDataset::Open;
    poDriver->pfnUnloadDriver = BAGDatasetDriverUnload;
    poDriver->pfnCreateCopy = BAGDataset::CreateCopy;
    poDriver->pfnCreate = BAGDataset::Create;

    GetGDALDriverManager()->RegisterDriver(poDriver);
}
