/******************************************************************************
 *
 * Project:  GDAL Core
 * Purpose:  Read metadata from OrbView imagery.
 * Author:   Alexander Lisovenko
 * Author:   Dmitry Baryshnikov, polimax@mail.ru
 *
 ******************************************************************************
 * Copyright (c) 2014-2015 NextGIS <info@nextgis.ru>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "reader_orb_view.h"

#include <ctime>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_string.h"
#include "cpl_time.h"

#include "gdal_priv.h"

/**
 * GDALMDReaderOrbView()
 */
GDALMDReaderOrbView::GDALMDReaderOrbView(const char *pszPath,
                                         char **papszSiblingFiles)
    : GDALMDReaderBase(pszPath, papszSiblingFiles),
      m_osIMDSourceFilename(
          GDALFindAssociatedFile(pszPath, "PVL", papszSiblingFiles, 0)),
      m_osRPBSourceFilename(CPLString())
{
    const std::string osBaseName = CPLGetBasenameSafe(pszPath);
    const std::string osDirName = CPLGetDirnameSafe(pszPath);

    std::string osRPBSourceFilename = CPLFormFilenameSafe(
        osDirName.c_str(), (osBaseName + "_rpc").c_str(), "txt");
    if (CPLCheckForFile(&osRPBSourceFilename[0], papszSiblingFiles))
    {
        m_osRPBSourceFilename = std::move(osRPBSourceFilename);
    }
    else
    {
        osRPBSourceFilename = CPLFormFilenameSafe(
            osDirName.c_str(), (osBaseName + "_RPC").c_str(), "TXT");
        if (CPLCheckForFile(&osRPBSourceFilename[0], papszSiblingFiles))
        {
            m_osRPBSourceFilename = std::move(osRPBSourceFilename);
        }
    }

    if (!m_osIMDSourceFilename.empty())
        CPLDebug("MDReaderOrbView", "IMD Filename: %s",
                 m_osIMDSourceFilename.c_str());
    if (!m_osRPBSourceFilename.empty())
        CPLDebug("MDReaderOrbView", "RPB Filename: %s",
                 m_osRPBSourceFilename.c_str());
}

/**
 * ~GDALMDReaderOrbView()
 */
GDALMDReaderOrbView::~GDALMDReaderOrbView()
{
}

/**
 * HasRequiredFiles()
 */
bool GDALMDReaderOrbView::HasRequiredFiles() const
{
    if (!m_osIMDSourceFilename.empty() && !m_osRPBSourceFilename.empty())
        return true;

    return false;
}

/**
 * GetMetadataFiles()
 */
char **GDALMDReaderOrbView::GetMetadataFiles() const
{
    char **papszFileList = nullptr;
    if (!m_osIMDSourceFilename.empty())
        papszFileList = CSLAddString(papszFileList, m_osIMDSourceFilename);
    if (!m_osRPBSourceFilename.empty())
        papszFileList = CSLAddString(papszFileList, m_osRPBSourceFilename);

    return papszFileList;
}

/**
 * LoadMetadata()
 */
void GDALMDReaderOrbView::LoadMetadata()
{
    if (m_bIsMetadataLoad)
        return;

    if (!m_osIMDSourceFilename.empty())
    {
        m_papszIMDMD = GDALLoadIMDFile(m_osIMDSourceFilename);
    }

    if (!m_osRPBSourceFilename.empty())
    {
        m_papszRPCMD = GDALLoadRPCFile(m_osRPBSourceFilename);
    }

    m_papszDEFAULTMD = CSLAddNameValue(m_papszDEFAULTMD, MD_NAME_MDTYPE, "OV");

    m_bIsMetadataLoad = true;

    if (nullptr == m_papszIMDMD)
    {
        return;
    }

    // extract imagery metadata
    const char *pszSatId =
        CSLFetchNameValue(m_papszIMDMD, "sensorInfo.satelliteName");
    if (nullptr != pszSatId)
    {
        m_papszIMAGERYMD = CSLAddNameValue(m_papszIMAGERYMD, MD_NAME_SATELLITE,
                                           CPLStripQuotes(pszSatId));
    }

    const char *pszCloudCover = CSLFetchNameValue(
        m_papszIMDMD, "productInfo.productCloudCoverPercentage");
    if (nullptr != pszCloudCover)
    {
        m_papszIMAGERYMD = CSLAddNameValue(m_papszIMAGERYMD, MD_NAME_CLOUDCOVER,
                                           pszCloudCover);
    }

    const char *pszDateTime = CSLFetchNameValue(
        m_papszIMDMD, "inputImageInfo.firstLineAcquisitionDateTime");

    if (nullptr != pszDateTime)
    {
        char buffer[80];
        GIntBig timeMid = GetAcquisitionTimeFromString(pszDateTime);
        struct tm tmBuf;
        strftime(buffer, 80, MD_DATETIMEFORMAT,
                 CPLUnixTimeToYMDHMS(timeMid, &tmBuf));
        m_papszIMAGERYMD =
            CSLAddNameValue(m_papszIMAGERYMD, MD_NAME_ACQDATETIME, buffer);
    }
}
