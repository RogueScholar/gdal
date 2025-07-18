/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements OGRSQLiteDataSource class.
 * Author:   Frank Warmerdam, warmerdam@pobox.com
 *
 ******************************************************************************
 *
 * Contributor: Alessandro Furieri, a.furieri@lqt.it
 * Portions of this module properly supporting SpatiaLite Table/Geom creation
 * Developed for Faunalia ( http://www.faunalia.it) with funding from
 * Regione Toscana - Settore SISTEMA INFORMATIVO TERRITORIALE ED AMBIENTALE
 *
 ******************************************************************************
 * Copyright (c) 2003, Frank Warmerdam <warmerdam@pobox.com>
 * Copyright (c) 2009-2013, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_port.h"
#include "ogr_sqlite.h"
#include "ogrsqlitevirtualogr.h"
#include "ogrsqliteutility.h"
#include "ogrsqlitevfs.h"

#include <cctype>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "cpl_conv.h"
#include "cpl_error.h"
#include "cpl_hash_set.h"
#include "cpl_multiproc.h"
#include "cpl_string.h"
#include "cpl_vsi.h"
#include "gdal.h"
#include "gdal_pam.h"
#include "gdal_priv.h"
#include "ogr_core.h"
#include "ogr_feature.h"
#include "ogr_geometry.h"
#include "ogr_spatialref.h"
#include "ogr_schema_override.h"
#include "ogrsf_frmts.h"
#include "sqlite3.h"

#include "proj.h"
#include "ogr_proj_p.h"

#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunknown-pragmas"
#pragma clang diagnostic ignored "-Wdocumentation"
#pragma clang diagnostic ignored "-Wdocumentation-unknown-command"
#endif

#if defined(HAVE_SPATIALITE) && !defined(SPATIALITE_DLOPEN)
#include "spatialite.h"
#endif

#ifdef __clang__
#pragma clang diagnostic pop
#endif

#undef SQLITE_STATIC
#define SQLITE_STATIC (static_cast<sqlite3_destructor_type>(nullptr))

// Keep in sync prototype of those 2 functions between gdalopeninfo.cpp,
// ogrsqlitedatasource.cpp and ogrgeopackagedatasource.cpp
void GDALOpenInfoDeclareFileNotToOpen(const char *pszFilename,
                                      const GByte *pabyHeader,
                                      int nHeaderBytes);
void GDALOpenInfoUnDeclareFileNotToOpen(const char *pszFilename);

#ifdef HAVE_SPATIALITE

#ifdef SPATIALITE_DLOPEN
static CPLMutex *hMutexLoadSpatialiteSymbols = nullptr;
static void *(*pfn_spatialite_alloc_connection)(void) = nullptr;
static void (*pfn_spatialite_shutdown)(void) = nullptr;
static void (*pfn_spatialite_init_ex)(sqlite3 *, const void *, int) = nullptr;
static void (*pfn_spatialite_cleanup_ex)(const void *) = nullptr;
static const char *(*pfn_spatialite_version)(void) = nullptr;
#else
static void *(*pfn_spatialite_alloc_connection)(void) =
    spatialite_alloc_connection;
static void (*pfn_spatialite_shutdown)(void) = spatialite_shutdown;
static void (*pfn_spatialite_init_ex)(sqlite3 *, const void *,
                                      int) = spatialite_init_ex;
static void (*pfn_spatialite_cleanup_ex)(const void *) = spatialite_cleanup_ex;
static const char *(*pfn_spatialite_version)(void) = spatialite_version;
#endif

#ifndef SPATIALITE_SONAME
#define SPATIALITE_SONAME "libspatialite.so"
#endif

#ifdef SPATIALITE_DLOPEN
static bool OGRSQLiteLoadSpatialiteSymbols()
{
    static bool bInitializationDone = false;
    CPLMutexHolderD(&hMutexLoadSpatialiteSymbols);
    if (bInitializationDone)
        return pfn_spatialite_alloc_connection != nullptr;
    bInitializationDone = true;

    const char *pszLibName =
        CPLGetConfigOption("SPATIALITESO", SPATIALITE_SONAME);
    CPLPushErrorHandler(CPLQuietErrorHandler);

    /* coverity[tainted_string] */
    pfn_spatialite_alloc_connection = (void *(*)(void))CPLGetSymbol(
        pszLibName, "spatialite_alloc_connection");
    CPLPopErrorHandler();

    if (pfn_spatialite_alloc_connection == nullptr)
    {
        CPLDebug("SQLITE", "Cannot find %s in %s",
                 "spatialite_alloc_connection", pszLibName);
        return false;
    }

    pfn_spatialite_shutdown =
        (void (*)(void))CPLGetSymbol(pszLibName, "spatialite_shutdown");
    pfn_spatialite_init_ex =
        (void (*)(sqlite3 *, const void *, int))CPLGetSymbol(
            pszLibName, "spatialite_init_ex");
    pfn_spatialite_cleanup_ex = (void (*)(const void *))CPLGetSymbol(
        pszLibName, "spatialite_cleanup_ex");
    pfn_spatialite_version =
        (const char *(*)(void))CPLGetSymbol(pszLibName, "spatialite_version");
    if (pfn_spatialite_shutdown == nullptr ||
        pfn_spatialite_init_ex == nullptr ||
        pfn_spatialite_cleanup_ex == nullptr ||
        pfn_spatialite_version == nullptr)
    {
        pfn_spatialite_shutdown = nullptr;
        pfn_spatialite_init_ex = nullptr;
        pfn_spatialite_cleanup_ex = nullptr;
        pfn_spatialite_version = nullptr;
        return false;
    }
    return true;
}
#endif

/************************************************************************/
/*                          InitSpatialite()                            */
/************************************************************************/

bool OGRSQLiteBaseDataSource::InitSpatialite()
{
    if (hSpatialiteCtxt == nullptr &&
        CPLTestBool(CPLGetConfigOption("SPATIALITE_LOAD", "TRUE")))
    {
#ifdef SPATIALITE_DLOPEN
        if (!OGRSQLiteLoadSpatialiteSymbols())
            return false;
#endif
        CPLAssert(hSpatialiteCtxt == nullptr);
        hSpatialiteCtxt = pfn_spatialite_alloc_connection();
        if (hSpatialiteCtxt != nullptr)
        {
            pfn_spatialite_init_ex(hDB, hSpatialiteCtxt,
                                   CPLTestBool(CPLGetConfigOption(
                                       "SPATIALITE_INIT_VERBOSE", "FALSE")));
        }
    }
    return hSpatialiteCtxt != nullptr;
}

/************************************************************************/
/*                         FinishSpatialite()                           */
/************************************************************************/

void OGRSQLiteBaseDataSource::FinishSpatialite()
{
    // Current implementation of spatialite_cleanup_ex() (as of libspatialite 5.1)
    // is not re-entrant due to the use of xmlCleanupParser()
    // Cf https://groups.google.com/g/spatialite-users/c/tsfZ_GDrRKs/m/aj-Dt4xoBQAJ?utm_medium=email&utm_source=footer
    static std::mutex oCleanupMutex;
    std::lock_guard oLock(oCleanupMutex);

    if (hSpatialiteCtxt != nullptr)
    {
        pfn_spatialite_cleanup_ex(hSpatialiteCtxt);
        hSpatialiteCtxt = nullptr;
    }
}

/************************************************************************/
/*                          IsSpatialiteLoaded()                        */
/************************************************************************/

bool OGRSQLiteBaseDataSource::IsSpatialiteLoaded()
{
    return hSpatialiteCtxt != nullptr;
}

#else

bool OGRSQLiteBaseDataSource::InitSpatialite()
{
    return false;
}

void OGRSQLiteBaseDataSource::FinishSpatialite()
{
}

bool OGRSQLiteBaseDataSource::IsSpatialiteLoaded()
{
    return false;
}

#endif

/************************************************************************/
/*                          OGRSQLiteDriverUnload()                     */
/************************************************************************/

void OGRSQLiteDriverUnload(GDALDriver *)
{
#ifdef HAVE_SPATIALITE
    if (pfn_spatialite_shutdown != nullptr)
        pfn_spatialite_shutdown();
#ifdef SPATIALITE_DLOPEN
    if (hMutexLoadSpatialiteSymbols != nullptr)
    {
        CPLDestroyMutex(hMutexLoadSpatialiteSymbols);
        hMutexLoadSpatialiteSymbols = nullptr;
    }
#endif
#endif
}

/************************************************************************/
/*                      DealWithOgrSchemaOpenOption()                   */
/************************************************************************/
bool OGRSQLiteBaseDataSource::DealWithOgrSchemaOpenOption(
    CSLConstList papszOpenOptionsIn)
{
    std::string osFieldsSchemaOverrideParam =
        CSLFetchNameValueDef(papszOpenOptionsIn, "OGR_SCHEMA", "");

    if (!osFieldsSchemaOverrideParam.empty())
    {
        if (GetUpdate())
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "OGR_SCHEMA open option is not supported in update mode.");
            return false;
        }

        OGRSchemaOverride osSchemaOverride;
        if (!osSchemaOverride.LoadFromJSON(osFieldsSchemaOverrideParam) ||
            !osSchemaOverride.IsValid())
        {
            return false;
        }

        const auto &oLayerOverrides = osSchemaOverride.GetLayerOverrides();
        for (const auto &oLayer : oLayerOverrides)
        {
            const auto &oLayerName = oLayer.first;
            const auto &oLayerFieldOverride = oLayer.second;
            const bool bIsFullOverride{oLayerFieldOverride.IsFullOverride()};
            auto oFieldOverrides = oLayerFieldOverride.GetFieldOverrides();
            std::vector<OGRFieldDefn *> aoFields;

            CPLDebug("SQLite", "Applying schema override for layer %s",
                     oLayerName.c_str());

            // Fail if the layer name does not exist
            auto poLayer = GetLayerByName(oLayerName.c_str());
            if (poLayer == nullptr)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Layer %s not found in SQLite DB", oLayerName.c_str());
                return false;
            }

            // Patch field definitions
            auto poLayerDefn = poLayer->GetLayerDefn();
            for (int i = 0; i < poLayerDefn->GetFieldCount(); i++)
            {
                auto poFieldDefn = poLayerDefn->GetFieldDefn(i);
                auto oFieldOverride =
                    oFieldOverrides.find(poFieldDefn->GetNameRef());
                if (oFieldOverride != oFieldOverrides.cend())
                {
                    if (oFieldOverride->second.GetFieldType().has_value())
                        whileUnsealing(poFieldDefn)
                            ->SetType(
                                oFieldOverride->second.GetFieldType().value());
                    if (oFieldOverride->second.GetFieldWidth().has_value())
                        whileUnsealing(poFieldDefn)
                            ->SetWidth(
                                oFieldOverride->second.GetFieldWidth().value());
                    if (oFieldOverride->second.GetFieldPrecision().has_value())
                        whileUnsealing(poFieldDefn)
                            ->SetPrecision(
                                oFieldOverride->second.GetFieldPrecision()
                                    .value());
                    if (oFieldOverride->second.GetFieldSubType().has_value())
                        whileUnsealing(poFieldDefn)
                            ->SetSubType(
                                oFieldOverride->second.GetFieldSubType()
                                    .value());
                    if (oFieldOverride->second.GetFieldName().has_value())
                        whileUnsealing(poFieldDefn)
                            ->SetName(oFieldOverride->second.GetFieldName()
                                          .value()
                                          .c_str());

                    if (bIsFullOverride)
                    {
                        aoFields.push_back(poFieldDefn);
                    }
                    oFieldOverrides.erase(oFieldOverride);
                }
            }

            // Error if any field override is not found
            if (!oFieldOverrides.empty())
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Field %s not found in layer %s",
                         oFieldOverrides.cbegin()->first.c_str(),
                         oLayerName.c_str());
                return false;
            }

            // Remove fields not in the override
            if (bIsFullOverride)
            {
                for (int i = poLayerDefn->GetFieldCount() - 1; i >= 0; i--)
                {
                    auto poFieldDefn = poLayerDefn->GetFieldDefn(i);
                    if (std::find(aoFields.begin(), aoFields.end(),
                                  poFieldDefn) == aoFields.end())
                    {
                        whileUnsealing(poLayerDefn)->DeleteFieldDefn(i);
                    }
                }
            }
        }
    }
    return true;
}

/************************************************************************/
/*                     GetSpatialiteVersionNumber()                     */
/************************************************************************/

int OGRSQLiteBaseDataSource::GetSpatialiteVersionNumber()
{
    int v = 0;
#ifdef HAVE_SPATIALITE
    if (IsSpatialiteLoaded())
    {
        const CPLStringList aosTokens(
            CSLTokenizeString2(pfn_spatialite_version(), ".", 0));
        if (aosTokens.size() >= 2)
        {
            v = MakeSpatialiteVersionNumber(
                atoi(aosTokens[0]), atoi(aosTokens[1]),
                aosTokens.size() == 3 ? atoi(aosTokens[2]) : 0);
        }
    }
#endif
    return v;
}

/************************************************************************/
/*                          AddRelationship()                           */
/************************************************************************/

bool OGRSQLiteDataSource::AddRelationship(
    std::unique_ptr<GDALRelationship> &&relationship,
    std::string &failureReason)
{
    if (!GetUpdate())
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "AddRelationship() not supported on read-only dataset");
        return false;
    }

    if (!ValidateRelationship(relationship.get(), failureReason))
    {
        return false;
    }

    const std::string &osLeftTableName = relationship->GetLeftTableName();
    const std::string &osRightTableName = relationship->GetRightTableName();
    const auto &aosLeftTableFields = relationship->GetLeftTableFields();
    const auto &aosRightTableFields = relationship->GetRightTableFields();

    bool bBaseKeyIsUnique = false;
    {
        const std::set<std::string> uniqueBaseFieldsUC =
            SQLGetUniqueFieldUCConstraints(GetDB(), osLeftTableName.c_str());
        if (cpl::contains(uniqueBaseFieldsUC,
                          CPLString(aosLeftTableFields[0]).toupper()))
        {
            bBaseKeyIsUnique = true;
        }
    }
    if (!bBaseKeyIsUnique)
    {
        failureReason = "Base table field must be a primary key field or have "
                        "a unique constraint set";
        return false;
    }

    OGRSQLiteTableLayer *poRightTable = dynamic_cast<OGRSQLiteTableLayer *>(
        GetLayerByName(osRightTableName.c_str()));
    if (!poRightTable)
    {
        failureReason = ("Right table " + osRightTableName +
                         " is not an existing layer in the dataset")
                            .c_str();
        return false;
    }

    char *pszForeignKeySQL = nullptr;
    if (relationship->GetType() == GDALRelationshipType::GRT_ASSOCIATION)
    {
        pszForeignKeySQL = sqlite3_mprintf(
            "FOREIGN KEY(\"%w\") REFERENCES \"%w\"(\"%w\") DEFERRABLE "
            "INITIALLY DEFERRED",
            aosRightTableFields[0].c_str(), osLeftTableName.c_str(),
            aosLeftTableFields[0].c_str());
    }
    else
    {
        pszForeignKeySQL = sqlite3_mprintf(
            "FOREIGN KEY(\"%w\") REFERENCES \"%w\"(\"%w\") ON DELETE CASCADE "
            "ON UPDATE CASCADE DEFERRABLE INITIALLY DEFERRED",
            aosRightTableFields[0].c_str(), osLeftTableName.c_str(),
            aosLeftTableFields[0].c_str());
    }

    int eErr = poRightTable->AddForeignKeysToTable(pszForeignKeySQL);
    sqlite3_free(pszForeignKeySQL);
    if (eErr != OGRERR_NONE)
    {
        failureReason = "Could not add foreign keys to table";
        return false;
    }

    char *pszSQL = sqlite3_mprintf(
        "CREATE INDEX \"idx_%qw_related_id\" ON \"%w\" (\"%w\");",
        osRightTableName.c_str(), osRightTableName.c_str(),
        aosRightTableFields[0].c_str());
    eErr = SQLCommand(hDB, pszSQL);
    sqlite3_free(pszSQL);
    if (eErr != OGRERR_NONE)
    {
        failureReason = ("Could not create index for " + osRightTableName +
                         " " + aosRightTableFields[0])
                            .c_str();
        return false;
    }

    m_bHasPopulatedRelationships = false;
    m_osMapRelationships.clear();
    return true;
}

/************************************************************************/
/*                       ValidateRelationship()                         */
/************************************************************************/

bool OGRSQLiteDataSource::ValidateRelationship(
    const GDALRelationship *poRelationship, std::string &failureReason)
{

    if (poRelationship->GetCardinality() !=
        GDALRelationshipCardinality::GRC_ONE_TO_MANY)
    {
        failureReason = "Only one to many relationships are supported for "
                        "SQLITE datasources";
        return false;
    }

    if (poRelationship->GetType() != GDALRelationshipType::GRT_COMPOSITE &&
        poRelationship->GetType() != GDALRelationshipType::GRT_ASSOCIATION)
    {
        failureReason = "Only association and composite relationship types are "
                        "supported for SQLITE datasources";
        return false;
    }

    const std::string &osLeftTableName = poRelationship->GetLeftTableName();
    OGRLayer *poLeftTable = GetLayerByName(osLeftTableName.c_str());
    if (!poLeftTable)
    {
        failureReason = ("Left table " + osLeftTableName +
                         " is not an existing layer in the dataset")
                            .c_str();
        return false;
    }
    const std::string &osRightTableName = poRelationship->GetRightTableName();
    OGRLayer *poRightTable = GetLayerByName(osRightTableName.c_str());
    if (!poRightTable)
    {
        failureReason = ("Right table " + osRightTableName +
                         " is not an existing layer in the dataset")
                            .c_str();
        return false;
    }

    const auto &aosLeftTableFields = poRelationship->GetLeftTableFields();
    if (aosLeftTableFields.empty())
    {
        failureReason = "No left table fields were specified";
        return false;
    }
    else if (aosLeftTableFields.size() > 1)
    {
        failureReason = "Only a single left table field is permitted for the "
                        "SQLITE relationships";
        return false;
    }
    else
    {
        // validate left field exists
        if (poLeftTable->GetLayerDefn()->GetFieldIndex(
                aosLeftTableFields[0].c_str()) < 0 &&
            !EQUAL(poLeftTable->GetFIDColumn(), aosLeftTableFields[0].c_str()))
        {
            failureReason = ("Left table field " + aosLeftTableFields[0] +
                             " does not exist in " + osLeftTableName)
                                .c_str();
            return false;
        }
    }

    const auto &aosRightTableFields = poRelationship->GetRightTableFields();
    if (aosRightTableFields.empty())
    {
        failureReason = "No right table fields were specified";
        return false;
    }
    else if (aosRightTableFields.size() > 1)
    {
        failureReason = "Only a single right table field is permitted for the "
                        "SQLITE relationships";
        return false;
    }
    else
    {
        // validate right field exists
        if (poRightTable->GetLayerDefn()->GetFieldIndex(
                aosRightTableFields[0].c_str()) < 0 &&
            !EQUAL(poRightTable->GetFIDColumn(),
                   aosRightTableFields[0].c_str()))
        {
            failureReason = ("Right table field " + aosRightTableFields[0] +
                             " does not exist in " + osRightTableName)
                                .c_str();
            return false;
        }
    }

    // ensure relationship is different from existing relationships
    for (const auto &kv : m_osMapRelationships)
    {
        if (osLeftTableName == kv.second->GetLeftTableName() &&
            osRightTableName == kv.second->GetRightTableName() &&
            aosLeftTableFields == kv.second->GetLeftTableFields() &&
            aosRightTableFields == kv.second->GetRightTableFields())
        {
            failureReason =
                "A relationship between these tables and fields already exists";
            return false;
        }
    }

    return true;
}

/************************************************************************/
/*                       OGRSQLiteBaseDataSource()                      */
/************************************************************************/

OGRSQLiteBaseDataSource::OGRSQLiteBaseDataSource() = default;

/************************************************************************/
/*                      ~OGRSQLiteBaseDataSource()                      */
/************************************************************************/

OGRSQLiteBaseDataSource::~OGRSQLiteBaseDataSource()

{
    CloseDB();

    FinishSpatialite();

    if (m_bCallUndeclareFileNotToOpen)
    {
        GDALOpenInfoUnDeclareFileNotToOpen(m_pszFilename);
    }

    if (!m_osFinalFilename.empty())
    {
        if (!bSuppressOnClose)
        {
            CPLDebug("SQLITE", "Copying temporary file %s onto %s",
                     m_pszFilename, m_osFinalFilename.c_str());
            if (CPLCopyFile(m_osFinalFilename.c_str(), m_pszFilename) != 0)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Copy temporary file %s onto %s failed", m_pszFilename,
                         m_osFinalFilename.c_str());
            }
        }
        CPLDebug("SQLITE", "Deleting temporary file %s", m_pszFilename);
        if (VSIUnlink(m_pszFilename) != 0)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Deleting temporary file %s failed", m_pszFilename);
        }
    }

    CPLFree(m_pszFilename);
}

/************************************************************************/
/*                               CloseDB()                              */
/************************************************************************/

bool OGRSQLiteBaseDataSource::CloseDB()
{
    bool bOK = true;
    if (hDB != nullptr)
    {
        bOK = (sqlite3_close(hDB) == SQLITE_OK);
        hDB = nullptr;

        // If we opened the DB in read-only mode, there might be spurious
        // -wal and -shm files that we can make disappear by reopening in
        // read-write
        VSIStatBufL sStat;
        if (eAccess == GA_ReadOnly &&
            !(STARTS_WITH(m_pszFilename, "/vsicurl/") ||
              STARTS_WITH(m_pszFilename, "/vsitar/") ||
              STARTS_WITH(m_pszFilename, "/vsizip/")) &&
            VSIStatL(CPLSPrintf("%s-wal", m_pszFilename), &sStat) == 0)
        {
            if (sqlite3_open(m_pszFilename, &hDB) != SQLITE_OK)
            {
                sqlite3_close(hDB);
                hDB = nullptr;
            }
            else if (hDB != nullptr)
            {
#ifdef SQLITE_FCNTL_PERSIST_WAL
                int nPersistentWAL = -1;
                sqlite3_file_control(hDB, "main", SQLITE_FCNTL_PERSIST_WAL,
                                     &nPersistentWAL);
                if (nPersistentWAL == 1)
                {
                    nPersistentWAL = 0;
                    if (sqlite3_file_control(hDB, "main",
                                             SQLITE_FCNTL_PERSIST_WAL,
                                             &nPersistentWAL) == SQLITE_OK)
                    {
                        CPLDebug("SQLITE",
                                 "Disabling persistent WAL succeeded");
                    }
                    else
                    {
                        CPLDebug("SQLITE", "Could not disable persistent WAL");
                    }
                }
#endif

                // Dummy request
                int nRowCount = 0, nColCount = 0;
                char **papszResult = nullptr;
                sqlite3_get_table(hDB, "SELECT name FROM sqlite_master WHERE 0",
                                  &papszResult, &nRowCount, &nColCount,
                                  nullptr);
                sqlite3_free_table(papszResult);

                sqlite3_close(hDB);
                hDB = nullptr;
#ifdef DEBUG_VERBOSE
                if (VSIStatL(CPLSPrintf("%s-wal", m_pszFilename), &sStat) != 0)
                {
                    CPLDebug("SQLite", "%s-wal file has been removed",
                             m_pszFilename);
                }
#endif
            }
        }
    }

    if (pMyVFS)
    {
        sqlite3_vfs_unregister(pMyVFS);
        CPLFree(pMyVFS->pAppData);
        CPLFree(pMyVFS);
        pMyVFS = nullptr;
    }

    return bOK;
}

/* Returns the first row of first column of SQL as integer */
OGRErr OGRSQLiteBaseDataSource::PragmaCheck(const char *pszPragma,
                                            const char *pszExpected,
                                            int nRowsExpected)
{
    CPLAssert(pszPragma != nullptr);
    CPLAssert(pszExpected != nullptr);
    CPLAssert(nRowsExpected >= 0);

    char **papszResult = nullptr;
    int nRowCount = 0;
    int nColCount = 0;
    char *pszErrMsg = nullptr;

    int rc =
        sqlite3_get_table(hDB, CPLSPrintf("PRAGMA %s", pszPragma), &papszResult,
                          &nRowCount, &nColCount, &pszErrMsg);

    if (rc != SQLITE_OK)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Unable to execute PRAGMA %s: %s",
                 pszPragma, pszErrMsg ? pszErrMsg : "(null)");
        sqlite3_free(pszErrMsg);
        return OGRERR_FAILURE;
    }

    if (nRowCount != nRowsExpected)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "bad result for PRAGMA %s, got %d rows, expected %d",
                 pszPragma, nRowCount, nRowsExpected);
        sqlite3_free_table(papszResult);
        return OGRERR_FAILURE;
    }

    if (nRowCount > 0 && !EQUAL(papszResult[1], pszExpected))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "invalid %s (expected '%s', got '%s')", pszPragma, pszExpected,
                 papszResult[1]);
        sqlite3_free_table(papszResult);
        return OGRERR_FAILURE;
    }

    sqlite3_free_table(papszResult);

    return OGRERR_NONE;
}

/************************************************************************/
/*                     LoadRelationships()                              */
/************************************************************************/

void OGRSQLiteBaseDataSource::LoadRelationships() const

{
    m_osMapRelationships.clear();
    LoadRelationshipsFromForeignKeys({});
    m_bHasPopulatedRelationships = true;
}

/************************************************************************/
/*                LoadRelationshipsFromForeignKeys()                    */
/************************************************************************/

void OGRSQLiteBaseDataSource::LoadRelationshipsFromForeignKeys(
    const std::vector<std::string> &excludedTables) const

{
    if (hDB)
    {
        std::string osSQL =
            "SELECT m.name, p.id, p.seq, p.\"table\" AS base_table_name, "
            "p.\"from\", p.\"to\", "
            "p.on_delete FROM sqlite_master m "
            "JOIN pragma_foreign_key_list(m.name) p ON m.name != p.\"table\" "
            "WHERE m.type = 'table' "
            // skip over foreign keys which relate to private GPKG tables
            "AND base_table_name NOT LIKE 'gpkg_%' "
            // Same with NGA GeoInt system tables
            "AND base_table_name NOT LIKE 'nga_%' "
            // Same with Spatialite system tables
            "AND base_table_name NOT IN ('geometry_columns', "
            "'spatial_ref_sys', 'views_geometry_columns', "
            "'virts_geometry_columns') ";
        if (!excludedTables.empty())
        {
            std::string oExcludedTablesList;
            for (const auto &osExcludedTable : excludedTables)
            {
                oExcludedTablesList += !oExcludedTablesList.empty() ? "," : "";
                char *pszEscapedName =
                    sqlite3_mprintf("'%q'", osExcludedTable.c_str());
                oExcludedTablesList += pszEscapedName;
                sqlite3_free(pszEscapedName);
            }

            osSQL += "AND base_table_name NOT IN (" + oExcludedTablesList +
                     ")"
                     " AND m.name NOT IN (" +
                     oExcludedTablesList + ") ";
        }
        osSQL += "ORDER BY m.name";

        auto oResult = SQLQuery(hDB, osSQL.c_str());

        if (!oResult)
        {
            CPLError(CE_Failure, CPLE_AppDefined, "Cannot load relationships");
            return;
        }

        for (int iRecord = 0; iRecord < oResult->RowCount(); iRecord++)
        {
            const char *pszRelatedTableName = oResult->GetValue(0, iRecord);
            if (!pszRelatedTableName)
                continue;

            const char *pszBaseTableName = oResult->GetValue(3, iRecord);
            if (!pszBaseTableName)
                continue;

            const char *pszRelatedFieldName = oResult->GetValue(4, iRecord);
            if (!pszRelatedFieldName)
                continue;

            const char *pszBaseFieldName = oResult->GetValue(5, iRecord);
            if (!pszBaseFieldName)
                continue;

            const int nId = oResult->GetValueAsInteger(1, iRecord);

            // form relationship name by appending foreign key id to base and
            // related table names
            std::ostringstream stream;
            stream << pszBaseTableName << '_' << pszRelatedTableName;
            if (nId > 0)
            {
                // note we use nId + 1 here as the first id will be zero, and
                // we'd like subsequent relations to have names starting with
                // _2, _3 etc, not _1, _2 etc.
                stream << '_' << (nId + 1);
            }
            const std::string osRelationName = stream.str();

            const auto it = m_osMapRelationships.find(osRelationName);
            if (it != m_osMapRelationships.end())
            {
                // already have a relationship with this name -- that means that
                // the base and related table name and id are the same, so we've
                // found a multi-column relationship
                auto osListLeftFields = it->second->GetLeftTableFields();
                osListLeftFields.emplace_back(pszBaseFieldName);
                it->second->SetLeftTableFields(osListLeftFields);

                auto osListRightFields = it->second->GetRightTableFields();
                osListRightFields.emplace_back(pszRelatedFieldName);
                it->second->SetRightTableFields(osListRightFields);
            }
            else
            {
                std::unique_ptr<GDALRelationship> poRelationship(
                    new GDALRelationship(osRelationName, pszBaseTableName,
                                         pszRelatedTableName, GRC_ONE_TO_MANY));
                poRelationship->SetLeftTableFields({pszBaseFieldName});
                poRelationship->SetRightTableFields({pszRelatedFieldName});
                poRelationship->SetRelatedTableType("features");

                if (const char *pszOnDeleteAction =
                        oResult->GetValue(6, iRecord))
                {
                    if (EQUAL(pszOnDeleteAction, "CASCADE"))
                    {
                        poRelationship->SetType(GRT_COMPOSITE);
                    }
                }

                m_osMapRelationships[osRelationName] =
                    std::move(poRelationship);
            }
        }
    }
}

/************************************************************************/
/*                        GetRelationshipNames()                        */
/************************************************************************/

std::vector<std::string> OGRSQLiteBaseDataSource::GetRelationshipNames(
    CPL_UNUSED CSLConstList papszOptions) const

{
    if (!m_bHasPopulatedRelationships)
    {
        LoadRelationships();
    }

    std::vector<std::string> oasNames;
    oasNames.reserve(m_osMapRelationships.size());
    for (const auto &kv : m_osMapRelationships)
    {
        oasNames.emplace_back(kv.first);
    }
    return oasNames;
}

/************************************************************************/
/*                        GetRelationship()                             */
/************************************************************************/

const GDALRelationship *
OGRSQLiteBaseDataSource::GetRelationship(const std::string &name) const

{
    if (!m_bHasPopulatedRelationships)
    {
        LoadRelationships();
    }

    const auto it = m_osMapRelationships.find(name);
    if (it == m_osMapRelationships.end())
        return nullptr;

    return it->second.get();
}

/***********************************************************************/
/*                         prepareSql()                                */
/***********************************************************************/

int OGRSQLiteBaseDataSource::prepareSql(sqlite3 *db, const char *zSql,
                                        int nByte, sqlite3_stmt **ppStmt,
                                        const char **pzTail)
{
    const int rc{sqlite3_prepare_v2(db, zSql, nByte, ppStmt, pzTail)};
    if (rc != SQLITE_OK && pfnQueryLoggerFunc)
    {
        std::string error{"Error preparing query: "};
        error.append(sqlite3_errmsg(db));
        pfnQueryLoggerFunc(zSql, error.c_str(), -1, -1, poQueryLoggerArg);
    }
    return rc;
}

/************************************************************************/
/*                        OGRSQLiteDataSource()                         */
/************************************************************************/

OGRSQLiteDataSource::OGRSQLiteDataSource() = default;

/************************************************************************/
/*                        ~OGRSQLiteDataSource()                        */
/************************************************************************/

OGRSQLiteDataSource::~OGRSQLiteDataSource()

{
    OGRSQLiteDataSource::Close();
}

/************************************************************************/
/*                              Close()                                 */
/************************************************************************/

CPLErr OGRSQLiteDataSource::Close()
{
    CPLErr eErr = CE_None;
    if (nOpenFlags != OPEN_FLAGS_CLOSED)
    {
        if (OGRSQLiteDataSource::FlushCache(true) != CE_None)
            eErr = CE_Failure;

#ifdef HAVE_RASTERLITE2
        if (m_pRL2Coverage != nullptr)
        {
            rl2_destroy_coverage(m_pRL2Coverage);
        }
#endif
        for (size_t i = 0; i < m_apoOverviewDS.size(); ++i)
        {
            delete m_apoOverviewDS[i];
        }

        if (!m_apoLayers.empty() || !m_apoInvisibleLayers.empty())
        {
            // Close any remaining iterator
            for (auto &poLayer : m_apoLayers)
                poLayer->ResetReading();
            for (auto &poLayer : m_apoInvisibleLayers)
                poLayer->ResetReading();

            // Create spatial indices in a transaction for faster execution
            if (hDB)
                SoftStartTransaction();
            for (auto &poLayer : m_apoLayers)
            {
                if (poLayer->IsTableLayer())
                {
                    OGRSQLiteTableLayer *poTableLayer =
                        cpl::down_cast<OGRSQLiteTableLayer *>(poLayer.get());
                    poTableLayer->RunDeferredCreationIfNecessary();
                    poTableLayer->CreateSpatialIndexIfNecessary();
                }
            }
            if (hDB)
                SoftCommitTransaction();
        }

        SaveStatistics();

        m_apoLayers.clear();
        m_apoInvisibleLayers.clear();

        m_oSRSCache.clear();

        if (!CloseDB())
            eErr = CE_Failure;
#ifdef HAVE_RASTERLITE2
        FinishRasterLite2();
#endif

        if (GDALPamDataset::Close() != CE_None)
            eErr = CE_Failure;
    }
    return eErr;
}

#ifdef HAVE_RASTERLITE2

/************************************************************************/
/*                          InitRasterLite2()                           */
/************************************************************************/

bool OGRSQLiteDataSource::InitRasterLite2()
{
    CPLAssert(m_hRL2Ctxt == nullptr);
    m_hRL2Ctxt = rl2_alloc_private();
    if (m_hRL2Ctxt != nullptr)
    {
        rl2_init(hDB, m_hRL2Ctxt, 0);
    }
    return m_hRL2Ctxt != nullptr;
}

/************************************************************************/
/*                         FinishRasterLite2()                          */
/************************************************************************/

void OGRSQLiteDataSource::FinishRasterLite2()
{
    if (m_hRL2Ctxt != nullptr)
    {
        rl2_cleanup_private(m_hRL2Ctxt);
        m_hRL2Ctxt = nullptr;
    }
}

#endif  // HAVE_RASTERLITE2

/************************************************************************/
/*                              SaveStatistics()                        */
/************************************************************************/

void OGRSQLiteDataSource::SaveStatistics()
{
    if (!m_bIsSpatiaLiteDB || !IsSpatialiteLoaded() ||
        m_bLastSQLCommandIsUpdateLayerStatistics || !GetUpdate())
        return;

    int nSavedAllLayersCacheData = -1;

    for (auto &poLayer : m_apoLayers)
    {
        if (poLayer->IsTableLayer())
        {
            OGRSQLiteTableLayer *poTableLayer =
                cpl::down_cast<OGRSQLiteTableLayer *>(poLayer.get());
            int nSaveRet = poTableLayer->SaveStatistics();
            if (nSaveRet >= 0)
            {
                if (nSavedAllLayersCacheData < 0)
                    nSavedAllLayersCacheData = nSaveRet;
                else
                    nSavedAllLayersCacheData &= nSaveRet;
            }
        }
    }

    if (hDB && nSavedAllLayersCacheData == TRUE)
    {
        int nReplaceEventId = -1;

        auto oResult = SQLQuery(
            hDB, "SELECT event_id, table_name, geometry_column, event "
                 "FROM spatialite_history ORDER BY event_id DESC LIMIT 1");

        if (oResult && oResult->RowCount() == 1)
        {
            const char *pszEventId = oResult->GetValue(0, 0);
            const char *pszTableName = oResult->GetValue(1, 0);
            const char *pszGeomCol = oResult->GetValue(2, 0);
            const char *pszEvent = oResult->GetValue(3, 0);

            if (pszEventId != nullptr && pszTableName != nullptr &&
                pszGeomCol != nullptr && pszEvent != nullptr &&
                strcmp(pszTableName, "ALL-TABLES") == 0 &&
                strcmp(pszGeomCol, "ALL-GEOMETRY-COLUMNS") == 0 &&
                strcmp(pszEvent, "UpdateLayerStatistics") == 0)
            {
                nReplaceEventId = atoi(pszEventId);
            }
        }

        const char *pszNow = HasSpatialite4Layout()
                                 ? "strftime('%Y-%m-%dT%H:%M:%fZ','now')"
                                 : "DateTime('now')";
        const char *pszSQL;
        if (nReplaceEventId >= 0)
        {
            pszSQL = CPLSPrintf("UPDATE spatialite_history SET "
                                "timestamp = %s "
                                "WHERE event_id = %d",
                                pszNow, nReplaceEventId);
        }
        else
        {
            pszSQL = CPLSPrintf(
                "INSERT INTO spatialite_history (table_name, geometry_column, "
                "event, timestamp, ver_sqlite, ver_splite) VALUES ("
                "'ALL-TABLES', 'ALL-GEOMETRY-COLUMNS', "
                "'UpdateLayerStatistics', "
                "%s, sqlite_version(), spatialite_version())",
                pszNow);
        }

        SQLCommand(hDB, pszSQL);
    }
}

/************************************************************************/
/*                              SetSynchronous()                        */
/************************************************************************/

bool OGRSQLiteBaseDataSource::SetSynchronous()
{
    const char *pszSqliteSync =
        CPLGetConfigOption("OGR_SQLITE_SYNCHRONOUS", nullptr);
    if (pszSqliteSync != nullptr)
    {
        const char *pszSQL = nullptr;
        if (EQUAL(pszSqliteSync, "OFF") || EQUAL(pszSqliteSync, "0") ||
            EQUAL(pszSqliteSync, "FALSE"))
            pszSQL = "PRAGMA synchronous = OFF";
        else if (EQUAL(pszSqliteSync, "NORMAL") || EQUAL(pszSqliteSync, "1"))
            pszSQL = "PRAGMA synchronous = NORMAL";
        else if (EQUAL(pszSqliteSync, "ON") || EQUAL(pszSqliteSync, "FULL") ||
                 EQUAL(pszSqliteSync, "2") || EQUAL(pszSqliteSync, "TRUE"))
            pszSQL = "PRAGMA synchronous = FULL";
        else
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Unrecognized value for OGR_SQLITE_SYNCHRONOUS : %s",
                     pszSqliteSync);

        return pszSQL != nullptr && SQLCommand(hDB, pszSQL) == OGRERR_NONE;
    }
    return true;
}

/************************************************************************/
/*                              LoadExtensions()                        */
/************************************************************************/

void OGRSQLiteBaseDataSource::LoadExtensions()
{
    const char *pszExtensions =
        CPLGetConfigOption("OGR_SQLITE_LOAD_EXTENSIONS", nullptr);
    if (pszExtensions != nullptr)
    {
#ifdef OGR_SQLITE_ALLOW_LOAD_EXTENSIONS
        // Allow sqlite3_load_extension() (C API only)
#ifdef SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION
        int oldMode = 0;
        if (sqlite3_db_config(hDB, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, -1,
                              &oldMode) != SQLITE_OK)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Cannot get initial value for "
                     "SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION");
            return;
        }
        CPLDebugOnly(
            "SQLite",
            "Initial mode for SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION = %d",
            oldMode);
        int newMode = 0;
        if (oldMode != 1 &&
            (sqlite3_db_config(hDB, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, 1,
                               &newMode) != SQLITE_OK ||
             newMode != 1))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION failed");
            return;
        }
#endif
        const CPLStringList aosExtensions(
            CSLTokenizeString2(pszExtensions, ",", 0));
        bool bRestoreOldMode = true;
        for (int i = 0; i < aosExtensions.size(); i++)
        {
            if (EQUAL(aosExtensions[i], "ENABLE_SQL_LOAD_EXTENSION"))
            {
                if (sqlite3_enable_load_extension(hDB, 1) == SQLITE_OK)
                {
                    bRestoreOldMode = false;
                }
                else
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "sqlite3_enable_load_extension() failed");
                }
            }
            else
            {
                char *pszErrMsg = nullptr;
                if (sqlite3_load_extension(hDB, aosExtensions[i], nullptr,
                                           &pszErrMsg) != SQLITE_OK)
                {
                    CPLError(CE_Failure, CPLE_AppDefined,
                             "Cannot load extension %s: %s", aosExtensions[i],
                             pszErrMsg ? pszErrMsg : "unknown reason");
                }
                sqlite3_free(pszErrMsg);
            }
        }
        CPL_IGNORE_RET_VAL(bRestoreOldMode);
#ifdef SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION
        if (bRestoreOldMode && oldMode != 1)
        {
            CPL_IGNORE_RET_VAL(sqlite3_db_config(
                hDB, SQLITE_DBCONFIG_ENABLE_LOAD_EXTENSION, oldMode, nullptr));
        }
#endif
#else
        CPLError(
            CE_Failure, CPLE_NotSupported,
            "The OGR_SQLITE_LOAD_EXTENSIONS was specified at run time, "
            "but GDAL has been built without OGR_SQLITE_ALLOW_LOAD_EXTENSIONS. "
            "So extensions won't be loaded");
#endif
    }
}

/************************************************************************/
/*                              SetCacheSize()                          */
/************************************************************************/

bool OGRSQLiteBaseDataSource::SetCacheSize()
{
    const char *pszSqliteCacheMB =
        CPLGetConfigOption("OGR_SQLITE_CACHE", nullptr);
    if (pszSqliteCacheMB != nullptr)
    {
        const GIntBig iSqliteCacheBytes =
            static_cast<GIntBig>(atoi(pszSqliteCacheMB)) * 1024 * 1024;

        /* querying the current PageSize */
        int iSqlitePageSize = SQLGetInteger(hDB, "PRAGMA page_size", nullptr);
        if (iSqlitePageSize <= 0)
            return false;
        /* computing the CacheSize as #Pages */
        const int iSqliteCachePages =
            static_cast<int>(iSqliteCacheBytes / iSqlitePageSize);
        if (iSqliteCachePages <= 0)
            return false;

        return SQLCommand(hDB, CPLSPrintf("PRAGMA cache_size = %d",
                                          iSqliteCachePages)) == OGRERR_NONE;
    }
    return true;
}

/************************************************************************/
/*               OGRSQLiteBaseDataSourceNotifyFileOpened()              */
/************************************************************************/

static void OGRSQLiteBaseDataSourceNotifyFileOpened(void *pfnUserData,
                                                    const char *pszFilename,
                                                    VSILFILE *fp)
{
    static_cast<OGRSQLiteBaseDataSource *>(pfnUserData)
        ->NotifyFileOpened(pszFilename, fp);
}

/************************************************************************/
/*                          NotifyFileOpened()                          */
/************************************************************************/

void OGRSQLiteBaseDataSource::NotifyFileOpened(const char *pszFilename,
                                               VSILFILE *fp)
{
    if (strcmp(pszFilename, m_pszFilename) == 0)
    {
        fpMainFile = fp;
    }
}

#ifdef USE_SQLITE_DEBUG_MEMALLOC

/* DMA9 */
constexpr int DMA_SIGNATURE = 0x444D4139;

static void *OGRSQLiteDMA_Malloc(int size)
{
    int *ret = (int *)CPLMalloc(size + 8);
    ret[0] = size;
    ret[1] = DMA_SIGNATURE;
    return ret + 2;
}

static void *OGRSQLiteDMA_Realloc(void *old_ptr, int size)
{
    CPLAssert(((int *)old_ptr)[-1] == DMA_SIGNATURE);
    int *ret = (int *)CPLRealloc(old_ptr ? (int *)old_ptr - 2 : NULL, size + 8);
    ret[0] = size;
    ret[1] = DMA_SIGNATURE;
    return ret + 2;
}

static void OGRSQLiteDMA_Free(void *ptr)
{
    if (ptr)
    {
        CPLAssert(((int *)ptr)[-1] == DMA_SIGNATURE);
        ((int *)ptr)[-1] = 0;
        CPLFree((int *)ptr - 2);
    }
}

static int OGRSQLiteDMA_Size(void *ptr)
{
    if (ptr)
    {
        CPLAssert(((int *)ptr)[-1] == DMA_SIGNATURE);
        return ((int *)ptr)[-2];
    }
    else
        return 0;
}

static int OGRSQLiteDMA_Roundup(int size)
{
    return (size + 7) & (~7);
}

static int OGRSQLiteDMA_Init(void *)
{
    return SQLITE_OK;
}

static void OGRSQLiteDMA_Shutdown(void *)
{
}

const struct sqlite3_mem_methods sDebugMemAlloc = {
    OGRSQLiteDMA_Malloc,   OGRSQLiteDMA_Free,
    OGRSQLiteDMA_Realloc,  OGRSQLiteDMA_Size,
    OGRSQLiteDMA_Roundup,  OGRSQLiteDMA_Init,
    OGRSQLiteDMA_Shutdown, NULL};

#endif  // USE_SQLITE_DEBUG_MEMALLOC

/************************************************************************/
/*                            OpenOrCreateDB()                          */
/************************************************************************/

bool OGRSQLiteBaseDataSource::OpenOrCreateDB(int flagsIn,
                                             bool bRegisterOGR2SQLiteExtensions,
                                             bool bLoadExtensions)
{
#ifdef USE_SQLITE_DEBUG_MEMALLOC
    if (CPLTestBool(CPLGetConfigOption("USE_SQLITE_DEBUG_MEMALLOC", "NO")))
        sqlite3_config(SQLITE_CONFIG_MALLOC, &sDebugMemAlloc);
#endif

    if (bRegisterOGR2SQLiteExtensions)
        OGR2SQLITE_Register();

    const bool bUseOGRVFS =
        CPLTestBool(CPLGetConfigOption("SQLITE_USE_OGR_VFS", "NO")) ||
        STARTS_WITH(m_pszFilename, "/vsi") ||
        // https://sqlite.org/forum/forumpost/0b1b8b5116: MAX_PATHNAME=512
        strlen(m_pszFilename) >= 512 - strlen(".journal");

#ifdef SQLITE_OPEN_URI
    const bool bNoLock =
        CPLTestBool(CSLFetchNameValueDef(papszOpenOptions, "NOLOCK", "NO"));
    const char *pszImmutable = CSLFetchNameValue(papszOpenOptions, "IMMUTABLE");
    const bool bImmutable = pszImmutable && CPLTestBool(pszImmutable);
    if (m_osFilenameForSQLiteOpen.empty() &&
        (flagsIn & SQLITE_OPEN_READWRITE) == 0 &&
        !STARTS_WITH(m_pszFilename, "file:") && (bNoLock || bImmutable))
    {
        m_osFilenameForSQLiteOpen = "file:";

        // Apply rules from "3.1. The URI Path" of
        // https://www.sqlite.org/uri.html
        CPLString osFilenameForURI(m_pszFilename);
        osFilenameForURI.replaceAll('?', "%3f");
        osFilenameForURI.replaceAll('#', "%23");
#ifdef _WIN32
        osFilenameForURI.replaceAll('\\', '/');
#endif
        if (!STARTS_WITH(m_pszFilename, "/vsi"))
        {
            osFilenameForURI.replaceAll("//", '/');
        }
#ifdef _WIN32
        if (osFilenameForURI.size() > 3 && osFilenameForURI[1] == ':' &&
            osFilenameForURI[2] == '/')
        {
            osFilenameForURI = '/' + osFilenameForURI;
        }
#endif

        m_osFilenameForSQLiteOpen += osFilenameForURI;
        m_osFilenameForSQLiteOpen += "?";
        if (bNoLock)
            m_osFilenameForSQLiteOpen += "nolock=1";
        if (bImmutable)
        {
            if (m_osFilenameForSQLiteOpen.back() != '?')
                m_osFilenameForSQLiteOpen += '&';
            m_osFilenameForSQLiteOpen += "immutable=1";
        }
    }
#endif
    if (m_osFilenameForSQLiteOpen.empty())
    {
        m_osFilenameForSQLiteOpen = m_pszFilename;
    }

    // No mutex since OGR objects are not supposed to be used concurrently
    // from multiple threads.
    int flags = flagsIn | SQLITE_OPEN_NOMUTEX;
#ifdef SQLITE_OPEN_URI
    // This code enables support for named memory databases in SQLite.
    // SQLITE_USE_URI is checked only to enable backward compatibility, in
    // case we accidentally hijacked some other format.
    if (STARTS_WITH(m_osFilenameForSQLiteOpen.c_str(), "file:") &&
        CPLTestBool(CPLGetConfigOption("SQLITE_USE_URI", "YES")))
    {
        flags |= SQLITE_OPEN_URI;
    }
#endif

    bool bPageSizeFound = false;
    bool bSecureDeleteFound = false;

    const char *pszSqlitePragma =
        CPLGetConfigOption("OGR_SQLITE_PRAGMA", nullptr);
    CPLString osJournalMode = CPLGetConfigOption("OGR_SQLITE_JOURNAL", "");

    if (bUseOGRVFS)
    {
        pMyVFS =
            OGRSQLiteCreateVFS(OGRSQLiteBaseDataSourceNotifyFileOpened, this);
        sqlite3_vfs_register(pMyVFS, 0);
    }

    for (int iterOpen = 0; iterOpen < 2; iterOpen++)
    {
        CPLAssert(hDB == nullptr);
        int rc = sqlite3_open_v2(m_osFilenameForSQLiteOpen.c_str(), &hDB, flags,
                                 pMyVFS ? pMyVFS->zName : nullptr);
        if (rc != SQLITE_OK || !hDB)
        {
            CPLError(CE_Failure, CPLE_OpenFailed, "sqlite3_open(%s) failed: %s",
                     m_pszFilename,
                     hDB ? sqlite3_errmsg(hDB) : "(unknown error)");
            sqlite3_close(hDB);
            hDB = nullptr;
            return false;
        }

#ifdef SQLITE_DBCONFIG_DEFENSIVE
        // SQLite builds on recent MacOS enable defensive mode by default, which
        // causes issues in the VDV driver (when updating a deleted database),
        // or in the GPKG driver (when modifying a CREATE TABLE DDL with
        // writable_schema=ON) So disable it.
        int bDefensiveOldValue = 0;
        if (sqlite3_db_config(hDB, SQLITE_DBCONFIG_DEFENSIVE, -1,
                              &bDefensiveOldValue) == SQLITE_OK &&
            bDefensiveOldValue == 1)
        {
            if (sqlite3_db_config(hDB, SQLITE_DBCONFIG_DEFENSIVE, 0, nullptr) ==
                SQLITE_OK)
            {
                CPLDebug("SQLITE", "Disabling defensive mode succeeded");
            }
            else
            {
                CPLDebug("SQLITE", "Could not disable defensive mode");
            }
        }
#endif

#ifdef SQLITE_FCNTL_PERSIST_WAL
        int nPersistentWAL = -1;
        sqlite3_file_control(hDB, "main", SQLITE_FCNTL_PERSIST_WAL,
                             &nPersistentWAL);
        if (nPersistentWAL == 1)
        {
            nPersistentWAL = 0;
            if (sqlite3_file_control(hDB, "main", SQLITE_FCNTL_PERSIST_WAL,
                                     &nPersistentWAL) == SQLITE_OK)
            {
                CPLDebug("SQLITE", "Disabling persistent WAL succeeded");
            }
            else
            {
                CPLDebug("SQLITE", "Could not disable persistent WAL");
            }
        }
#endif

        if (pszSqlitePragma != nullptr)
        {
            char **papszTokens =
                CSLTokenizeString2(pszSqlitePragma, ",", CSLT_HONOURSTRINGS);
            for (int i = 0; papszTokens[i] != nullptr; i++)
            {
                if (STARTS_WITH_CI(papszTokens[i], "PAGE_SIZE"))
                    bPageSizeFound = true;
                else if (STARTS_WITH_CI(papszTokens[i], "JOURNAL_MODE"))
                {
                    const char *pszEqual = strchr(papszTokens[i], '=');
                    if (pszEqual)
                    {
                        osJournalMode = pszEqual + 1;
                        osJournalMode.Trim();
                        // Only apply journal_mode after changing page_size
                        continue;
                    }
                }
                else if (STARTS_WITH_CI(papszTokens[i], "SECURE_DELETE"))
                    bSecureDeleteFound = true;

                const char *pszSQL = CPLSPrintf("PRAGMA %s", papszTokens[i]);

                CPL_IGNORE_RET_VAL(
                    sqlite3_exec(hDB, pszSQL, nullptr, nullptr, nullptr));
            }
            CSLDestroy(papszTokens);
        }

        const char *pszVal = CPLGetConfigOption("SQLITE_BUSY_TIMEOUT", "5000");
        if (pszVal != nullptr)
        {
            sqlite3_busy_timeout(hDB, atoi(pszVal));
        }

#ifdef SQLITE_OPEN_URI
        if (iterOpen == 0 && bNoLock && !bImmutable)
        {
            int nRowCount = 0, nColCount = 0;
            char **papszResult = nullptr;
            rc = sqlite3_get_table(hDB, "PRAGMA journal_mode", &papszResult,
                                   &nRowCount, &nColCount, nullptr);
            bool bWal = false;
            // rc == SQLITE_CANTOPEN seems to be what we get when issuing the
            // above in nolock mode on a wal enabled file
            if (rc != SQLITE_OK ||
                (nRowCount == 1 && nColCount == 1 && papszResult[1] &&
                 EQUAL(papszResult[1], "wal")))
            {
                bWal = true;
            }
            sqlite3_free_table(papszResult);
            if (bWal)
            {
                flags &= ~SQLITE_OPEN_URI;
                sqlite3_close(hDB);
                hDB = nullptr;
                CPLDebug("SQLite",
                         "Cannot open %s in nolock mode because it is "
                         "presumably in -wal mode",
                         m_pszFilename);
                m_osFilenameForSQLiteOpen = m_pszFilename;
                continue;
            }
        }
#endif
        break;
    }

    if ((flagsIn & SQLITE_OPEN_CREATE) == 0)
    {
        if (CPLTestBool(CPLGetConfigOption("OGR_VFK_DB_READ", "NO")))
        {
            if (SQLGetInteger(hDB,
                              "SELECT 1 FROM sqlite_master "
                              "WHERE type = 'table' AND name = 'vfk_tables'",
                              nullptr))
                return false; /* DB is valid VFK datasource */
        }

        int nRowCount = 0, nColCount = 0;
        char **papszResult = nullptr;
        char *pszErrMsg = nullptr;
        int rc =
            sqlite3_get_table(hDB,
                              "SELECT 1 FROM sqlite_master "
                              "WHERE (type = 'trigger' OR type = 'view') AND ("
                              "sql LIKE '%%ogr_geocode%%' OR "
                              "sql LIKE '%%ogr_datasource_load_layers%%' OR "
                              "sql LIKE '%%ogr_GetConfigOption%%' OR "
                              "sql LIKE '%%ogr_SetConfigOption%%' ) "
                              "LIMIT 1",
                              &papszResult, &nRowCount, &nColCount, &pszErrMsg);
        if (rc != SQLITE_OK)
        {
            bool bIsWAL = false;
            VSILFILE *fp = VSIFOpenL(m_pszFilename, "rb");
            if (fp != nullptr)
            {
                GByte byVal = 0;
                VSIFSeekL(fp, 18, SEEK_SET);
                VSIFReadL(&byVal, 1, 1, fp);
                bIsWAL = byVal == 2;
                VSIFCloseL(fp);
            }
            if (bIsWAL)
            {
#ifdef SQLITE_OPEN_URI
                if (pszImmutable == nullptr &&
                    (flags & SQLITE_OPEN_READONLY) != 0 &&
                    m_osFilenameForSQLiteOpen == m_pszFilename)
                {
                    CPLError(CE_Warning, CPLE_AppDefined,
                             "%s: this file is a WAL-enabled database. "
                             "It cannot be opened "
                             "because it is presumably read-only or in a "
                             "read-only directory. Retrying with IMMUTABLE=YES "
                             "open option",
                             pszErrMsg);
                    sqlite3_free(pszErrMsg);
                    CloseDB();
                    m_osFilenameForSQLiteOpen.clear();
                    papszOpenOptions =
                        CSLSetNameValue(papszOpenOptions, "IMMUTABLE", "YES");
                    return OpenOrCreateDB(flagsIn,
                                          bRegisterOGR2SQLiteExtensions,
                                          bLoadExtensions);
                }
#endif

                CPLError(CE_Failure, CPLE_AppDefined,
                         "%s: this file is a WAL-enabled database. "
                         "It cannot be opened "
                         "because it is presumably read-only or in a "
                         "read-only directory.%s",
                         pszErrMsg,
#ifdef SQLITE_OPEN_URI
                         pszImmutable != nullptr
                             ? ""
                             : " Try opening with IMMUTABLE=YES open option"
#else
                         ""
#endif
                );
            }
            else
            {
                CPLError(CE_Failure, CPLE_AppDefined, "%s", pszErrMsg);
            }
            sqlite3_free(pszErrMsg);
            return false;
        }

        sqlite3_free_table(papszResult);

        if (nRowCount > 0)
        {
            if (!CPLTestBool(CPLGetConfigOption(
                    "ALLOW_OGR_SQL_FUNCTIONS_FROM_TRIGGER_AND_VIEW", "NO")))
            {
                CPLError(CE_Failure, CPLE_OpenFailed, "%s",
                         "A trigger and/or view calls a OGR extension SQL "
                         "function that could be used to "
                         "steal data, or use network bandwidth, without your "
                         "consent.\n"
                         "The database will not be opened unless the "
                         "ALLOW_OGR_SQL_FUNCTIONS_FROM_TRIGGER_AND_VIEW "
                         "configuration option to YES.");
                return false;
            }
        }
    }

    if (m_osFilenameForSQLiteOpen != m_pszFilename &&
        (m_osFilenameForSQLiteOpen.find("?nolock=1") != std::string::npos ||
         m_osFilenameForSQLiteOpen.find("&nolock=1") != std::string::npos))
    {
        m_bNoLock = true;
        CPLDebug("SQLite", "%s open in nolock mode", m_pszFilename);
    }

    if (!bPageSizeFound && (flagsIn & SQLITE_OPEN_CREATE) != 0)
    {
        // Since sqlite 3.12 the default page_size is now 4096. But we
        // can use that even with older versions.
        CPL_IGNORE_RET_VAL(sqlite3_exec(hDB, "PRAGMA page_size = 4096", nullptr,
                                        nullptr, nullptr));
    }

    // journal_mode = WAL must be done *AFTER* changing page size.
    if (!osJournalMode.empty())
    {
        const char *pszSQL =
            CPLSPrintf("PRAGMA journal_mode = %s", osJournalMode.c_str());

        CPL_IGNORE_RET_VAL(
            sqlite3_exec(hDB, pszSQL, nullptr, nullptr, nullptr));
    }

    if (!bSecureDeleteFound)
    {
        // Turn on secure_delete by default (unless the user specifies a
        // value of this pragma through OGR_SQLITE_PRAGMA)
        // For example, Debian and Conda-Forge SQLite3 builds already turn on
        // secure_delete.
        CPL_IGNORE_RET_VAL(sqlite3_exec(hDB, "PRAGMA secure_delete = 1",
                                        nullptr, nullptr, nullptr));
    }

    SetCacheSize();
    SetSynchronous();
    if (bLoadExtensions)
        LoadExtensions();

    return true;
}

/************************************************************************/
/*                            OpenOrCreateDB()                          */
/************************************************************************/

bool OGRSQLiteDataSource::OpenOrCreateDB(int flagsIn,
                                         bool bRegisterOGR2SQLiteExtensions)
{
    {
        // Make sure that OGR2SQLITE_static_register() doesn't instantiate
        // its default OGR2SQLITEModule. Let's do it ourselves just afterwards
        //
        CPLConfigOptionSetter oSetter("OGR_SQLITE_STATIC_VIRTUAL_OGR", "NO",
                                      false);
        if (!OGRSQLiteBaseDataSource::OpenOrCreateDB(
                flagsIn, bRegisterOGR2SQLiteExtensions,
                /*bLoadExtensions=*/false))
        {
            return false;
        }
    }
    if (bRegisterOGR2SQLiteExtensions &&
        // Do not run OGR2SQLITE_Setup() if called from ogrsqlitexecute.sql
        // that will do it with other datasets.
        CPLTestBool(CPLGetConfigOption("OGR_SQLITE_STATIC_VIRTUAL_OGR", "YES")))
    {
        // Make sure this is done before registering our custom functions
        // to allow overriding Spatialite.
        InitSpatialite();

        m_poSQLiteModule = OGR2SQLITE_Setup(this, this);
    }
    // We need to do LoadExtensions() after OGR2SQLITE_Setup(), otherwise
    // tests in ogr_virtualogr.py::test_ogr_sqlite_load_extensions_load_self()
    // will crash when trying to load libgdal as an extension (which is an
    // errour we catch, but only if OGR2SQLITEModule has been created by
    // above OGR2SQLITE_Setup()
    LoadExtensions();

    const char *pszPreludeStatements =
        CSLFetchNameValue(papszOpenOptions, "PRELUDE_STATEMENTS");
    if (pszPreludeStatements)
    {
        if (SQLCommand(hDB, pszPreludeStatements) != OGRERR_NONE)
            return false;
    }

    return true;
}

/************************************************************************/
/*                       PostInitSpatialite()                           */
/************************************************************************/

void OGRSQLiteDataSource::PostInitSpatialite()
{
#ifdef HAVE_SPATIALITE
    const char *pszSqlitePragma =
        CPLGetConfigOption("OGR_SQLITE_PRAGMA", nullptr);
    OGRErr eErr = OGRERR_NONE;
    if ((!pszSqlitePragma || !strstr(pszSqlitePragma, "trusted_schema")) &&
        // Older sqlite versions don't have this pragma
        SQLGetInteger(hDB, "PRAGMA trusted_schema", &eErr) == 0 &&
        eErr == OGRERR_NONE)
    {
        // Spatialite <= 5.1.0 doesn't declare its functions as SQLITE_INNOCUOUS
        if (IsSpatialiteLoaded() && SpatialiteRequiresTrustedSchemaOn() &&
            AreSpatialiteTriggersSafe())
        {
            CPLDebug("SQLITE", "Setting PRAGMA trusted_schema = 1");
            SQLCommand(hDB, "PRAGMA trusted_schema = 1");
        }
    }
#endif
}

/************************************************************************/
/*                 SpatialiteRequiresTrustedSchemaOn()                  */
/************************************************************************/

bool OGRSQLiteBaseDataSource::SpatialiteRequiresTrustedSchemaOn()
{
#ifdef HAVE_SPATIALITE
    // Spatialite <= 5.1.0 doesn't declare its functions as SQLITE_INNOCUOUS
    if (GetSpatialiteVersionNumber() <= MakeSpatialiteVersionNumber(5, 1, 0))
    {
        return true;
    }
#endif
    return false;
}

/************************************************************************/
/*                    AreSpatialiteTriggersSafe()                       */
/************************************************************************/

bool OGRSQLiteBaseDataSource::AreSpatialiteTriggersSafe()
{
#ifdef HAVE_SPATIALITE
    // Not totally sure about the minimum spatialite version, but 4.3a is fine
    return GetSpatialiteVersionNumber() >=
               MakeSpatialiteVersionNumber(4, 3, 0) &&
           SQLGetInteger(hDB, "SELECT CountUnsafeTriggers()", nullptr) == 0;
#else
    return true;
#endif
}

/************************************************************************/
/*                          GetInternalHandle()                         */
/************************************************************************/

/* Used by MBTILES driver */
void *OGRSQLiteBaseDataSource::GetInternalHandle(const char *pszKey)
{
    if (pszKey != nullptr && EQUAL(pszKey, "SQLITE_HANDLE"))
        return hDB;
    return nullptr;
}

/************************************************************************/
/*                               Create()                               */
/************************************************************************/

bool OGRSQLiteDataSource::Create(const char *pszNameIn, char **papszOptions)
{
    CPLString osCommand;

    const bool bUseTempFile =
        CPLTestBool(CPLGetConfigOption(
            "CPL_VSIL_USE_TEMP_FILE_FOR_RANDOM_WRITE", "NO")) &&
        (VSIHasOptimizedReadMultiRange(pszNameIn) != FALSE ||
         EQUAL(
             CPLGetConfigOption("CPL_VSIL_USE_TEMP_FILE_FOR_RANDOM_WRITE", ""),
             "FORCED"));

    if (bUseTempFile)
    {
        m_osFinalFilename = pszNameIn;
        m_pszFilename = CPLStrdup(
            CPLGenerateTempFilenameSafe(CPLGetFilename(pszNameIn)).c_str());
        CPLDebug("SQLITE", "Creating temporary file %s", m_pszFilename);
    }
    else
    {
        m_pszFilename = CPLStrdup(pszNameIn);
    }

    /* -------------------------------------------------------------------- */
    /*      Check that spatialite extensions are loaded if required to      */
    /*      create a spatialite database                                    */
    /* -------------------------------------------------------------------- */
    const bool bSpatialite = CPLFetchBool(papszOptions, "SPATIALITE", false);
    const bool bMetadata = CPLFetchBool(papszOptions, "METADATA", true);

    if (bSpatialite)
    {
#ifndef HAVE_SPATIALITE
        CPLError(
            CE_Failure, CPLE_NotSupported,
            "OGR was built without libspatialite support\n"
            "... sorry, creating/writing any SpatiaLite DB is unsupported\n");

        return false;
#endif
    }

    m_bIsSpatiaLiteDB = bSpatialite;

    /* -------------------------------------------------------------------- */
    /*      Create the database file.                                       */
    /* -------------------------------------------------------------------- */
    if (!OpenOrCreateDB(SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, true))
        return false;

    /* -------------------------------------------------------------------- */
    /*      Create the SpatiaLite metadata tables.                          */
    /* -------------------------------------------------------------------- */
    if (bSpatialite)
    {
        if (!InitSpatialite())
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Creating a Spatialite database, but Spatialite "
                     "extensions are not loaded.");
            return false;
        }

        PostInitSpatialite();

#ifdef HAVE_RASTERLITE2
        InitRasterLite2();
#endif

        /*
        / SpatiaLite full support: calling InitSpatialMetadata()
        /
        / IMPORTANT NOTICE: on SpatiaLite any attempt aimed
        / to directly CREATE "geometry_columns" and "spatial_ref_sys"
        / [by-passing InitSpatialMetadata() as absolutely required]
        / will severely [and irremediably] corrupt the DB !!!
        */

        const char *pszVal = CSLFetchNameValue(papszOptions, "INIT_WITH_EPSG");
        const int nSpatialiteVersionNumber = GetSpatialiteVersionNumber();
        if (pszVal != nullptr && !CPLTestBool(pszVal) &&
            nSpatialiteVersionNumber >= MakeSpatialiteVersionNumber(4, 0, 0))
        {
            if (nSpatialiteVersionNumber >=
                MakeSpatialiteVersionNumber(4, 1, 0))
                osCommand = "SELECT InitSpatialMetadata(1, 'NONE')";
            else
                osCommand = "SELECT InitSpatialMetadata('NONE')";
        }
        else
        {
            /* Since spatialite 4.1, InitSpatialMetadata() is no longer run */
            /* into a transaction, which makes population of spatial_ref_sys */
            /* from EPSG awfully slow. We have to use InitSpatialMetadata(1) */
            /* to run within a transaction */
            if (nSpatialiteVersionNumber >= 41)
                osCommand = "SELECT InitSpatialMetadata(1)";
            else
                osCommand = "SELECT InitSpatialMetadata()";
        }
        if (SQLCommand(hDB, osCommand) != OGRERR_NONE)
        {
            return false;
        }
    }

    /* -------------------------------------------------------------------- */
    /*  Create the geometry_columns and spatial_ref_sys metadata tables.    */
    /* -------------------------------------------------------------------- */
    else if (bMetadata)
    {
        if (SQLCommand(hDB, "CREATE TABLE geometry_columns ("
                            "     f_table_name VARCHAR, "
                            "     f_geometry_column VARCHAR, "
                            "     geometry_type INTEGER, "
                            "     coord_dimension INTEGER, "
                            "     srid INTEGER,"
                            "     geometry_format VARCHAR )"
                            ";"
                            "CREATE TABLE spatial_ref_sys        ("
                            "     srid INTEGER UNIQUE,"
                            "     auth_name TEXT,"
                            "     auth_srid TEXT,"
                            "     srtext TEXT)") != OGRERR_NONE)
        {
            return false;
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Optionally initialize the content of the spatial_ref_sys table  */
    /*      with the EPSG database                                          */
    /* -------------------------------------------------------------------- */
    if ((bSpatialite || bMetadata) &&
        CPLFetchBool(papszOptions, "INIT_WITH_EPSG", false))
    {
        if (!InitWithEPSG())
            return false;
    }

    GDALOpenInfo oOpenInfo(m_pszFilename, GDAL_OF_VECTOR | GDAL_OF_UPDATE);
    return Open(&oOpenInfo);
}

/************************************************************************/
/*                           InitWithEPSG()                             */
/************************************************************************/

bool OGRSQLiteDataSource::InitWithEPSG()
{
    CPLString osCommand;

    if (m_bIsSpatiaLiteDB)
    {
        /*
        / if v.2.4.0 (or any subsequent) InitWithEPSG make no sense at all
        / because the EPSG dataset is already self-initialized at DB creation
        */
        int iSpatialiteVersion = GetSpatialiteVersionNumber();
        if (iSpatialiteVersion >= MakeSpatialiteVersionNumber(2, 4, 0))
            return true;
    }

    if (SoftStartTransaction() != OGRERR_NONE)
        return false;

    OGRSpatialReference oSRS;
    int rc = SQLITE_OK;
    for (int i = 0; i < 2 && rc == SQLITE_OK; i++)
    {
        PROJ_STRING_LIST crsCodeList = proj_get_codes_from_database(
            OSRGetProjTLSContext(), "EPSG",
            i == 0 ? PJ_TYPE_GEOGRAPHIC_2D_CRS : PJ_TYPE_PROJECTED_CRS, true);
        for (auto iterCode = crsCodeList; iterCode && *iterCode; ++iterCode)
        {
            int nSRSId = atoi(*iterCode);

            CPLPushErrorHandler(CPLQuietErrorHandler);
            oSRS.importFromEPSG(nSRSId);
            CPLPopErrorHandler();

            if (m_bIsSpatiaLiteDB)
            {
                char *pszProj4 = nullptr;

                CPLPushErrorHandler(CPLQuietErrorHandler);
                OGRErr eErr = oSRS.exportToProj4(&pszProj4);

                char *pszWKT = nullptr;
                if (eErr == OGRERR_NONE &&
                    oSRS.exportToWkt(&pszWKT) != OGRERR_NONE)
                {
                    CPLFree(pszWKT);
                    pszWKT = nullptr;
                    eErr = OGRERR_FAILURE;
                }
                CPLPopErrorHandler();

                if (eErr == OGRERR_NONE)
                {
                    const char *pszProjCS = oSRS.GetAttrValue("PROJCS");
                    if (pszProjCS == nullptr)
                        pszProjCS = oSRS.GetAttrValue("GEOGCS");

                    const char *pszSRTEXTColName = GetSRTEXTColName();
                    if (pszSRTEXTColName != nullptr)
                    {
                        /* the SPATIAL_REF_SYS table supports a SRS_WKT column
                         */
                        if (pszProjCS)
                            osCommand.Printf(
                                "INSERT INTO spatial_ref_sys "
                                "(srid, auth_name, auth_srid, ref_sys_name, "
                                "proj4text, %s) "
                                "VALUES (%d, 'EPSG', '%d', ?, ?, ?)",
                                pszSRTEXTColName, nSRSId, nSRSId);
                        else
                            osCommand.Printf(
                                "INSERT INTO spatial_ref_sys "
                                "(srid, auth_name, auth_srid, proj4text, %s) "
                                "VALUES (%d, 'EPSG', '%d', ?, ?)",
                                pszSRTEXTColName, nSRSId, nSRSId);
                    }
                    else
                    {
                        /* the SPATIAL_REF_SYS table does not support a SRS_WKT
                         * column */
                        if (pszProjCS)
                            osCommand.Printf("INSERT INTO spatial_ref_sys "
                                             "(srid, auth_name, auth_srid, "
                                             "ref_sys_name, proj4text) "
                                             "VALUES (%d, 'EPSG', '%d', ?, ?)",
                                             nSRSId, nSRSId);
                        else
                            osCommand.Printf(
                                "INSERT INTO spatial_ref_sys "
                                "(srid, auth_name, auth_srid, proj4text) "
                                "VALUES (%d, 'EPSG', '%d', ?)",
                                nSRSId, nSRSId);
                    }

                    sqlite3_stmt *hInsertStmt = nullptr;
                    rc = prepareSql(hDB, osCommand, -1, &hInsertStmt, nullptr);

                    if (pszProjCS)
                    {
                        if (rc == SQLITE_OK)
                            rc = sqlite3_bind_text(hInsertStmt, 1, pszProjCS,
                                                   -1, SQLITE_STATIC);
                        if (rc == SQLITE_OK)
                            rc = sqlite3_bind_text(hInsertStmt, 2, pszProj4, -1,
                                                   SQLITE_STATIC);
                        if (pszSRTEXTColName != nullptr)
                        {
                            /* the SPATIAL_REF_SYS table supports a SRS_WKT
                             * column */
                            if (rc == SQLITE_OK && pszWKT != nullptr)
                                rc = sqlite3_bind_text(hInsertStmt, 3, pszWKT,
                                                       -1, SQLITE_STATIC);
                        }
                    }
                    else
                    {
                        if (rc == SQLITE_OK)
                            rc = sqlite3_bind_text(hInsertStmt, 1, pszProj4, -1,
                                                   SQLITE_STATIC);
                        if (pszSRTEXTColName != nullptr)
                        {
                            /* the SPATIAL_REF_SYS table supports a SRS_WKT
                             * column */
                            if (rc == SQLITE_OK && pszWKT != nullptr)
                                rc = sqlite3_bind_text(hInsertStmt, 2, pszWKT,
                                                       -1, SQLITE_STATIC);
                        }
                    }

                    if (rc == SQLITE_OK)
                        rc = sqlite3_step(hInsertStmt);

                    if (rc != SQLITE_OK && rc != SQLITE_DONE)
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Cannot insert %s into spatial_ref_sys : %s",
                                 pszProj4, sqlite3_errmsg(hDB));

                        sqlite3_finalize(hInsertStmt);
                        CPLFree(pszProj4);
                        CPLFree(pszWKT);
                        break;
                    }
                    rc = SQLITE_OK;

                    sqlite3_finalize(hInsertStmt);
                }

                CPLFree(pszProj4);
                CPLFree(pszWKT);
            }
            else
            {
                char *pszWKT = nullptr;
                CPLPushErrorHandler(CPLQuietErrorHandler);
                bool bSuccess = (oSRS.exportToWkt(&pszWKT) == OGRERR_NONE);
                CPLPopErrorHandler();
                if (bSuccess)
                {
                    osCommand.Printf("INSERT INTO spatial_ref_sys "
                                     "(srid, auth_name, auth_srid, srtext) "
                                     "VALUES (%d, 'EPSG', '%d', ?)",
                                     nSRSId, nSRSId);

                    sqlite3_stmt *hInsertStmt = nullptr;
                    rc = prepareSql(hDB, osCommand, -1, &hInsertStmt, nullptr);

                    if (rc == SQLITE_OK)
                        rc = sqlite3_bind_text(hInsertStmt, 1, pszWKT, -1,
                                               SQLITE_STATIC);

                    if (rc == SQLITE_OK)
                        rc = sqlite3_step(hInsertStmt);

                    if (rc != SQLITE_OK && rc != SQLITE_DONE)
                    {
                        CPLError(CE_Failure, CPLE_AppDefined,
                                 "Cannot insert %s into spatial_ref_sys : %s",
                                 pszWKT, sqlite3_errmsg(hDB));

                        sqlite3_finalize(hInsertStmt);
                        CPLFree(pszWKT);
                        break;
                    }
                    rc = SQLITE_OK;

                    sqlite3_finalize(hInsertStmt);
                }

                CPLFree(pszWKT);
            }
        }

        proj_string_list_destroy(crsCodeList);
    }

    if (rc == SQLITE_OK)
    {
        if (SoftCommitTransaction() != OGRERR_NONE)
            return false;
        return true;
    }
    else
    {
        SoftRollbackTransaction();
        return false;
    }
}

/************************************************************************/
/*                        ReloadLayers()                                */
/************************************************************************/

void OGRSQLiteDataSource::ReloadLayers()
{
    m_apoLayers.clear();

    GDALOpenInfo oOpenInfo(m_pszFilename,
                           GDAL_OF_VECTOR | (GetUpdate() ? GDAL_OF_UPDATE : 0));
    Open(&oOpenInfo);
}

/************************************************************************/
/*                                Open()                                */
/************************************************************************/

bool OGRSQLiteDataSource::Open(GDALOpenInfo *poOpenInfo)

{
    const char *pszNewName = poOpenInfo->pszFilename;
    CPLAssert(m_apoLayers.empty());
    eAccess = poOpenInfo->eAccess;
    nOpenFlags = poOpenInfo->nOpenFlags & ~GDAL_OF_THREAD_SAFE;
    SetDescription(pszNewName);

    if (m_pszFilename == nullptr)
    {
#ifdef HAVE_RASTERLITE2
        if (STARTS_WITH_CI(pszNewName, "RASTERLITE2:") &&
            (nOpenFlags & GDAL_OF_RASTER) != 0)
        {
            char **papszTokens =
                CSLTokenizeString2(pszNewName, ":", CSLT_HONOURSTRINGS);
            if (CSLCount(papszTokens) < 2)
            {
                CSLDestroy(papszTokens);
                return false;
            }
            m_pszFilename = CPLStrdup(SQLUnescape(papszTokens[1]));
            CSLDestroy(papszTokens);
        }
        else
#endif
            if (STARTS_WITH_CI(pszNewName, "SQLITE:"))
        {
            m_pszFilename = CPLStrdup(pszNewName + strlen("SQLITE:"));
        }
        else
        {
            m_pszFilename = CPLStrdup(pszNewName);
            if (poOpenInfo->pabyHeader &&
                STARTS_WITH(
                    reinterpret_cast<const char *>(poOpenInfo->pabyHeader),
                    "SQLite format 3"))
            {
                m_bCallUndeclareFileNotToOpen = true;
                GDALOpenInfoDeclareFileNotToOpen(m_pszFilename,
                                                 poOpenInfo->pabyHeader,
                                                 poOpenInfo->nHeaderBytes);
            }
        }
    }
    SetPhysicalFilename(m_pszFilename);

    VSIStatBufL sStat;
    if (VSIStatL(m_pszFilename, &sStat) == 0)
    {
        m_nFileTimestamp = sStat.st_mtime;
    }

    if (poOpenInfo->papszOpenOptions)
    {
        CSLDestroy(papszOpenOptions);
        papszOpenOptions = CSLDuplicate(poOpenInfo->papszOpenOptions);
    }

    const bool bListVectorLayers = (nOpenFlags & GDAL_OF_VECTOR) != 0;

    const bool bListAllTables =
        bListVectorLayers &&
        CPLTestBool(CSLFetchNameValueDef(
            papszOpenOptions, "LIST_ALL_TABLES",
            CPLGetConfigOption("SQLITE_LIST_ALL_TABLES", "NO")));

    // Don't list by default: there might be some security implications
    // if a user is provided with a file and doesn't know that there are
    // virtual OGR tables in it.
    const bool bListVirtualOGRLayers =
        bListVectorLayers &&
        CPLTestBool(CSLFetchNameValueDef(
            papszOpenOptions, "LIST_VIRTUAL_OGR",
            CPLGetConfigOption("OGR_SQLITE_LIST_VIRTUAL_OGR", "NO")));

    /* -------------------------------------------------------------------- */
    /*      Try to open the sqlite database properly now.                   */
    /* -------------------------------------------------------------------- */
    if (hDB == nullptr)
    {
#ifdef ENABLE_SQL_SQLITE_FORMAT
        // SQLite -wal locking appears to be extremely fragile. In particular
        // if we have a file descriptor opened on the file while sqlite3_open
        // is called, then it will mis-behave (a process opening in update mode
        // the file and closing it will remove the -wal file !)
        // So make sure that the GDALOpenInfo object goes out of scope before
        // going on.
        {
            GDALOpenInfo oOpenInfo(m_pszFilename, GA_ReadOnly);
            if (oOpenInfo.pabyHeader &&
                (STARTS_WITH(
                     reinterpret_cast<const char *>(oOpenInfo.pabyHeader),
                     "-- SQL SQLITE") ||
                 STARTS_WITH(
                     reinterpret_cast<const char *>(oOpenInfo.pabyHeader),
                     "-- SQL RASTERLITE") ||
                 STARTS_WITH(
                     reinterpret_cast<const char *>(oOpenInfo.pabyHeader),
                     "-- SQL MBTILES")) &&
                oOpenInfo.fpL != nullptr)
            {
                if (sqlite3_open_v2(":memory:", &hDB, SQLITE_OPEN_READWRITE,
                                    nullptr) != SQLITE_OK)
                {
                    return false;
                }

                // We need it here for ST_MinX() and the like
                InitSpatialite();

                PostInitSpatialite();

                // Ingest the lines of the dump
                VSIFSeekL(oOpenInfo.fpL, 0, SEEK_SET);
                const char *pszLine;
                while ((pszLine = CPLReadLineL(oOpenInfo.fpL)) != nullptr)
                {
                    if (STARTS_WITH(pszLine, "--"))
                        continue;

                    if (!SQLCheckLineIsSafe(pszLine))
                        return false;

                    char *pszErrMsg = nullptr;
                    if (sqlite3_exec(hDB, pszLine, nullptr, nullptr,
                                     &pszErrMsg) != SQLITE_OK)
                    {
                        if (pszErrMsg)
                        {
                            CPLDebug("SQLITE", "Error %s at line %s", pszErrMsg,
                                     pszLine);
                        }
                    }
                    sqlite3_free(pszErrMsg);
                }
            }
        }
        if (hDB == nullptr)
#endif
        {
            if (poOpenInfo->fpL)
            {
                // See above comment about -wal locking for the importance of
                // closing that file, prior to calling sqlite3_open()
                VSIFCloseL(poOpenInfo->fpL);
                poOpenInfo->fpL = nullptr;
            }
            if (!OpenOrCreateDB(GetUpdate() ? SQLITE_OPEN_READWRITE
                                            : SQLITE_OPEN_READONLY,
                                true))
            {
                poOpenInfo->fpL =
                    VSIFOpenL(poOpenInfo->pszFilename,
                              poOpenInfo->eAccess == GA_Update ? "rb+" : "rb");
                return false;
            }
        }

        InitSpatialite();

        PostInitSpatialite();

#ifdef HAVE_RASTERLITE2
        InitRasterLite2();
#endif
    }

#ifdef HAVE_RASTERLITE2
    if (STARTS_WITH_CI(pszNewName, "RASTERLITE2:") &&
        (nOpenFlags & GDAL_OF_RASTER) != 0)
    {
        return OpenRasterSubDataset(pszNewName);
    }
#endif

    /* -------------------------------------------------------------------- */
    /*      If we have a GEOMETRY_COLUMNS tables, initialize on the basis   */
    /*      of that.                                                        */
    /* -------------------------------------------------------------------- */
    CPLHashSet *hSet =
        CPLHashSetNew(CPLHashSetHashStr, CPLHashSetEqualStr, CPLFree);

    char **papszResult = nullptr;
    char *pszErrMsg = nullptr;
    int nRowCount = 0;
    int nColCount = 0;
    int rc = sqlite3_get_table(
        hDB,
        "SELECT f_table_name, f_geometry_column, geometry_type, "
        "coord_dimension, geometry_format, srid"
        " FROM geometry_columns "
        "LIMIT 10000",
        &papszResult, &nRowCount, &nColCount, &pszErrMsg);

    if (rc == SQLITE_OK)
    {
        CPLDebug("SQLITE", "OGR style SQLite DB found !");

        m_bHaveGeometryColumns = true;

        for (int iRow = 0; bListVectorLayers && iRow < nRowCount; iRow++)
        {
            char **papszRow = papszResult + iRow * 6 + 6;
            const char *pszTableName = papszRow[0];
            const char *pszGeomCol = papszRow[1];

            if (pszTableName == nullptr || pszGeomCol == nullptr)
                continue;

            m_aoMapTableToSetOfGeomCols[pszTableName].insert(
                CPLString(pszGeomCol).tolower());
        }

        for (int iRow = 0; bListVectorLayers && iRow < nRowCount; iRow++)
        {
            char **papszRow = papszResult + iRow * 6 + 6;
            const char *pszTableName = papszRow[0];

            if (pszTableName == nullptr)
                continue;

            if (GDALDataset::GetLayerByName(pszTableName) == nullptr)
            {
                const bool bRet = OpenTable(pszTableName, true, false,
                                            /* bMayEmitError = */ true);
                if (!bRet)
                {
                    CPLDebug("SQLITE", "Failed to open layer %s", pszTableName);
                    sqlite3_free_table(papszResult);
                    CPLHashSetDestroy(hSet);
                    return false;
                }
            }

            if (bListAllTables)
                CPLHashSetInsert(hSet, CPLStrdup(pszTableName));
        }

        sqlite3_free_table(papszResult);

        /* --------------------------------------------------------------------
         */
        /*      Detect VirtualOGR layers */
        /* --------------------------------------------------------------------
         */
        if (bListVirtualOGRLayers)
        {
            rc = sqlite3_get_table(hDB,
                                   "SELECT name, sql FROM sqlite_master "
                                   "WHERE sql LIKE 'CREATE VIRTUAL TABLE %' "
                                   "LIMIT 10000",
                                   &papszResult, &nRowCount, &nColCount,
                                   &pszErrMsg);

            if (rc == SQLITE_OK)
            {
                for (int iRow = 0; iRow < nRowCount; iRow++)
                {
                    char **papszRow = papszResult + iRow * 2 + 2;
                    const char *pszName = papszRow[0];
                    const char *pszSQL = papszRow[1];
                    if (pszName == nullptr || pszSQL == nullptr)
                        continue;

                    if (strstr(pszSQL, "VirtualOGR"))
                    {
                        OpenVirtualTable(pszName, pszSQL);

                        if (bListAllTables)
                            CPLHashSetInsert(hSet, CPLStrdup(pszName));
                    }
                }
            }
            else
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Unable to fetch list of tables: %s", pszErrMsg);
                sqlite3_free(pszErrMsg);
            }

            sqlite3_free_table(papszResult);
        }

        if (bListAllTables)
            goto all_tables;

        CPLHashSetDestroy(hSet);

        if (nOpenFlags & GDAL_OF_RASTER)
        {
            bool bRet = OpenRaster();
            if (!bRet && !(nOpenFlags & GDAL_OF_VECTOR))
                return false;
        }

        return true;
    }

    /* -------------------------------------------------------------------- */
    /*      Otherwise we can deal with SpatiaLite database.                 */
    /* -------------------------------------------------------------------- */
    sqlite3_free(pszErrMsg);
    rc = sqlite3_get_table(hDB,
                           "SELECT sm.name, gc.f_geometry_column, "
                           "gc.type, gc.coord_dimension, gc.srid, "
                           "gc.spatial_index_enabled FROM geometry_columns gc "
                           "JOIN sqlite_master sm ON "
                           "LOWER(gc.f_table_name)=LOWER(sm.name) "
                           "LIMIT 10000",
                           &papszResult, &nRowCount, &nColCount, &pszErrMsg);
    if (rc != SQLITE_OK)
    {
        /* Test with SpatiaLite 4.0 schema */
        sqlite3_free(pszErrMsg);
        rc = sqlite3_get_table(
            hDB,
            "SELECT sm.name, gc.f_geometry_column, "
            "gc.geometry_type, gc.coord_dimension, gc.srid, "
            "gc.spatial_index_enabled FROM geometry_columns gc "
            "JOIN sqlite_master sm ON "
            "LOWER(gc.f_table_name)=LOWER(sm.name) "
            "LIMIT 10000",
            &papszResult, &nRowCount, &nColCount, &pszErrMsg);
        if (rc == SQLITE_OK)
        {
            m_bSpatialite4Layout = true;
            m_nUndefinedSRID = 0;
        }
    }

    if (rc == SQLITE_OK)
    {
        m_bIsSpatiaLiteDB = true;
        m_bHaveGeometryColumns = true;

        int iSpatialiteVersion = -1;

        /* Only enables write-mode if linked against SpatiaLite */
        if (IsSpatialiteLoaded())
        {
            iSpatialiteVersion = GetSpatialiteVersionNumber();
        }
        else if (GetUpdate())
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "SpatiaLite%s DB found, "
                     "but updating tables disabled because no linking against "
                     "spatialite library !",
                     (m_bSpatialite4Layout) ? " v4" : "");
            sqlite3_free_table(papszResult);
            CPLHashSetDestroy(hSet);
            return false;
        }

        if (m_bSpatialite4Layout && GetUpdate() && iSpatialiteVersion > 0 &&
            iSpatialiteVersion < MakeSpatialiteVersionNumber(4, 0, 0))
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "SpatiaLite v4 DB found, "
                     "but updating tables disabled because runtime spatialite "
                     "library is v%d.%d.%d !",
                     iSpatialiteVersion / 10000,
                     (iSpatialiteVersion % 10000) / 100,
                     (iSpatialiteVersion % 100));
            sqlite3_free_table(papszResult);
            CPLHashSetDestroy(hSet);
            return false;
        }
        else
        {
            CPLDebug("SQLITE", "SpatiaLite%s DB found !",
                     (m_bSpatialite4Layout) ? " v4" : "");
        }

        // List RasterLite2 coverages, so as to avoid listing corresponding
        // technical tables
        std::set<CPLString> aoSetTablesToIgnore;
        if (m_bSpatialite4Layout)
        {
            char **papszResults2 = nullptr;
            int nRowCount2 = 0, nColCount2 = 0;
            rc = sqlite3_get_table(
                hDB,
                "SELECT name FROM sqlite_master WHERE "
                "type = 'table' AND name = 'raster_coverages'",
                &papszResults2, &nRowCount2, &nColCount2, nullptr);
            sqlite3_free_table(papszResults2);
            if (rc == SQLITE_OK && nRowCount2 == 1)
            {
                papszResults2 = nullptr;
                nRowCount2 = 0;
                nColCount2 = 0;
                rc = sqlite3_get_table(
                    hDB,
                    "SELECT coverage_name FROM raster_coverages "
                    "LIMIT 10000",
                    &papszResults2, &nRowCount2, &nColCount2, nullptr);
                if (rc == SQLITE_OK)
                {
                    for (int i = 0; i < nRowCount2; ++i)
                    {
                        const char *const *papszRow = papszResults2 + i * 1 + 1;
                        if (papszRow[0] != nullptr)
                        {
                            aoSetTablesToIgnore.insert(CPLString(papszRow[0]) +
                                                       "_sections");
                            aoSetTablesToIgnore.insert(CPLString(papszRow[0]) +
                                                       "_tiles");
                        }
                    }
                }
                sqlite3_free_table(papszResults2);
            }
        }

        for (int iRow = 0; bListVectorLayers && iRow < nRowCount; iRow++)
        {
            char **papszRow = papszResult + iRow * 6 + 6;
            const char *pszTableName = papszRow[0];
            const char *pszGeomCol = papszRow[1];

            if (pszTableName == nullptr || pszGeomCol == nullptr)
                continue;
            if (!bListAllTables &&
                cpl::contains(aoSetTablesToIgnore, pszTableName))
            {
                continue;
            }

            m_aoMapTableToSetOfGeomCols[pszTableName].insert(
                CPLString(pszGeomCol).tolower());
        }

        for (int iRow = 0; bListVectorLayers && iRow < nRowCount; iRow++)
        {
            char **papszRow = papszResult + iRow * 6 + 6;
            const char *pszTableName = papszRow[0];

            if (pszTableName == nullptr)
                continue;
            if (!bListAllTables &&
                cpl::contains(aoSetTablesToIgnore, pszTableName))
            {
                continue;
            }

            if (GDALDataset::GetLayerByName(pszTableName) == nullptr)
                OpenTable(pszTableName, true, false,
                          /* bMayEmitError = */ true);
            if (bListAllTables)
                CPLHashSetInsert(hSet, CPLStrdup(pszTableName));
        }

        sqlite3_free_table(papszResult);
        papszResult = nullptr;

        /* --------------------------------------------------------------------
         */
        /*      Detect VirtualShape, VirtualXL and VirtualOGR layers */
        /* --------------------------------------------------------------------
         */
        rc =
            sqlite3_get_table(hDB,
                              "SELECT name, sql FROM sqlite_master "
                              "WHERE sql LIKE 'CREATE VIRTUAL TABLE %' "
                              "LIMIT 10000",
                              &papszResult, &nRowCount, &nColCount, &pszErrMsg);

        if (rc == SQLITE_OK)
        {
            for (int iRow = 0; bListVectorLayers && iRow < nRowCount; iRow++)
            {
                char **papszRow = papszResult + iRow * 2 + 2;
                const char *pszName = papszRow[0];
                const char *pszSQL = papszRow[1];
                if (pszName == nullptr || pszSQL == nullptr)
                    continue;

                if ((IsSpatialiteLoaded() && (strstr(pszSQL, "VirtualShape") ||
                                              strstr(pszSQL, "VirtualXL"))) ||
                    (bListVirtualOGRLayers && strstr(pszSQL, "VirtualOGR")))
                {
                    OpenVirtualTable(pszName, pszSQL);

                    if (bListAllTables)
                        CPLHashSetInsert(hSet, CPLStrdup(pszName));
                }
            }
        }
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "Unable to fetch list of tables: %s", pszErrMsg);
            sqlite3_free(pszErrMsg);
        }

        sqlite3_free_table(papszResult);
        papszResult = nullptr;

        /* --------------------------------------------------------------------
         */
        /*      Detect spatial views */
        /* --------------------------------------------------------------------
         */

        rc = sqlite3_get_table(hDB,
                               "SELECT view_name, view_geometry, view_rowid, "
                               "f_table_name, f_geometry_column "
                               "FROM views_geometry_columns "
                               "LIMIT 10000",
                               &papszResult, &nRowCount, &nColCount, nullptr);
        if (rc == SQLITE_OK)
        {
            for (int iRow = 0; bListVectorLayers && iRow < nRowCount; iRow++)
            {
                char **papszRow = papszResult + iRow * 5 + 5;
                const char *pszViewName = papszRow[0];
                const char *pszViewGeometry = papszRow[1];
                const char *pszViewRowid = papszRow[2];
                const char *pszTableName = papszRow[3];
                const char *pszGeometryColumn = papszRow[4];

                if (pszViewName == nullptr || pszViewGeometry == nullptr ||
                    pszViewRowid == nullptr || pszTableName == nullptr ||
                    pszGeometryColumn == nullptr)
                    continue;

                OpenView(pszViewName, pszViewGeometry, pszViewRowid,
                         pszTableName, pszGeometryColumn);

                if (bListAllTables)
                    CPLHashSetInsert(hSet, CPLStrdup(pszViewName));
            }
            sqlite3_free_table(papszResult);
        }

        if (bListAllTables)
            goto all_tables;

        CPLHashSetDestroy(hSet);

        if (nOpenFlags & GDAL_OF_RASTER)
        {
            bool bRet = OpenRaster();
            if (!bRet && !(nOpenFlags & GDAL_OF_VECTOR))
                return false;
        }

        return true;
    }

    /* -------------------------------------------------------------------- */
    /*      Otherwise our final resort is to return all tables and views    */
    /*      as non-spatial tables.                                          */
    /* -------------------------------------------------------------------- */
    sqlite3_free(pszErrMsg);

all_tables:
    rc = sqlite3_get_table(hDB,
                           "SELECT name, type FROM sqlite_master "
                           "WHERE type IN ('table','view') "
                           "UNION ALL "
                           "SELECT name, type FROM sqlite_temp_master "
                           "WHERE type IN ('table','view') "
                           "ORDER BY 1 "
                           "LIMIT 10000",
                           &papszResult, &nRowCount, &nColCount, &pszErrMsg);

    if (rc != SQLITE_OK)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Unable to fetch list of tables: %s", pszErrMsg);
        sqlite3_free(pszErrMsg);
        CPLHashSetDestroy(hSet);
        return false;
    }

    for (int iRow = 0; iRow < nRowCount; iRow++)
    {
        const char *pszTableName = papszResult[2 * (iRow + 1) + 0];
        const char *pszType = papszResult[2 * (iRow + 1) + 1];
        if (pszTableName != nullptr &&
            CPLHashSetLookup(hSet, pszTableName) == nullptr)
        {
            const bool bIsTable =
                pszType != nullptr && strcmp(pszType, "table") == 0;
            OpenTable(pszTableName, bIsTable, false,
                      /* bMayEmitError = */ true);
        }
    }

    sqlite3_free_table(papszResult);
    CPLHashSetDestroy(hSet);

    if (nOpenFlags & GDAL_OF_RASTER)
    {
        bool bRet = OpenRaster();
        if (!bRet && !(nOpenFlags & GDAL_OF_VECTOR))
            return false;
    }

    return true;
}

/************************************************************************/
/*                          OpenVirtualTable()                          */
/************************************************************************/

bool OGRSQLiteDataSource::OpenVirtualTable(const char *pszName,
                                           const char *pszSQL)
{
    int nSRID = m_nUndefinedSRID;
    const char *pszVirtualShape = strstr(pszSQL, "VirtualShape");
    if (pszVirtualShape != nullptr)
    {
        const char *pszParenthesis = strchr(pszVirtualShape, '(');
        if (pszParenthesis)
        {
            /* CREATE VIRTUAL TABLE table_name VirtualShape(shapename, codepage,
             * srid) */
            /* Extract 3rd parameter */
            char **papszTokens =
                CSLTokenizeString2(pszParenthesis + 1, ",", CSLT_HONOURSTRINGS);
            if (CSLCount(papszTokens) == 3)
            {
                nSRID = atoi(papszTokens[2]);
            }
            CSLDestroy(papszTokens);
        }
    }

    if (OpenTable(pszName, true, pszVirtualShape != nullptr,
                  /* bMayEmitError = */ true))
    {
        OGRSQLiteLayer *poLayer = m_apoLayers.back().get();
        if (poLayer->GetLayerDefn()->GetGeomFieldCount() == 1)
        {
            OGRSQLiteGeomFieldDefn *poGeomFieldDefn =
                poLayer->myGetLayerDefn()->myGetGeomFieldDefn(0);
            poGeomFieldDefn->m_eGeomFormat = OSGF_SpatiaLite;
            if (nSRID > 0)
            {
                poGeomFieldDefn->m_nSRSId = nSRID;
                poGeomFieldDefn->SetSpatialRef(FetchSRS(nSRID));
            }
        }

        OGRFeature *poFeature = poLayer->GetNextFeature();
        if (poFeature)
        {
            OGRGeometry *poGeom = poFeature->GetGeometryRef();
            if (poGeom)
                whileUnsealing(poLayer->GetLayerDefn())
                    ->SetGeomType(poGeom->getGeometryType());
            delete poFeature;
        }
        poLayer->ResetReading();
        return true;
    }

    return false;
}

/************************************************************************/
/*                             OpenTable()                              */
/************************************************************************/

bool OGRSQLiteDataSource::OpenTable(const char *pszTableName, bool bIsTable,
                                    bool bIsVirtualShape, bool bMayEmitError)

{
    /* -------------------------------------------------------------------- */
    /*      Create the layer object.                                        */
    /* -------------------------------------------------------------------- */
    auto poLayer = std::make_unique<OGRSQLiteTableLayer>(this);
    if (poLayer->Initialize(pszTableName, bIsTable, bIsVirtualShape, false,
                            bMayEmitError) != CE_None)
    {
        return false;
    }

    /* -------------------------------------------------------------------- */
    /*      Add layer to data source layer list.                            */
    /* -------------------------------------------------------------------- */
    m_apoLayers.push_back(std::move(poLayer));

    // Remove in case of error in the schema processing
    if (!DealWithOgrSchemaOpenOption(papszOpenOptions))
    {
        m_apoLayers.pop_back();
        return false;
    }

    return true;
}

/************************************************************************/
/*                             OpenView()                               */
/************************************************************************/

bool OGRSQLiteDataSource::OpenView(const char *pszViewName,
                                   const char *pszViewGeometry,
                                   const char *pszViewRowid,
                                   const char *pszTableName,
                                   const char *pszGeometryColumn)

{
    /* -------------------------------------------------------------------- */
    /*      Create the layer object.                                        */
    /* -------------------------------------------------------------------- */
    auto poLayer = std::make_unique<OGRSQLiteViewLayer>(this);

    if (poLayer->Initialize(pszViewName, pszViewGeometry, pszViewRowid,
                            pszTableName, pszGeometryColumn) != CE_None)
    {
        return false;
    }

    /* -------------------------------------------------------------------- */
    /*      Add layer to data source layer list.                            */
    /* -------------------------------------------------------------------- */
    m_apoLayers.push_back(std::move(poLayer));

    return true;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRSQLiteDataSource::TestCapability(const char *pszCap)

{
    if (EQUAL(pszCap, ODsCCreateLayer) || EQUAL(pszCap, ODsCDeleteLayer) ||
        EQUAL(pszCap, ODsCCreateGeomFieldAfterCreateLayer) ||
        EQUAL(pszCap, ODsCRandomLayerWrite) ||
        EQUAL(pszCap, GDsCAddRelationship))
        return GetUpdate();
    else if (EQUAL(pszCap, ODsCCurveGeometries))
        return !m_bIsSpatiaLiteDB;
    else if (EQUAL(pszCap, ODsCMeasuredGeometries))
        return TRUE;
    else
        return OGRSQLiteBaseDataSource::TestCapability(pszCap);
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRSQLiteBaseDataSource::TestCapability(const char *pszCap)
{
    if (EQUAL(pszCap, ODsCTransactions))
        return true;
    else if (EQUAL(pszCap, ODsCZGeometries))
        return true;
    else
        return GDALPamDataset::TestCapability(pszCap);
}

/************************************************************************/
/*                              GetLayer()                              */
/************************************************************************/

OGRLayer *OGRSQLiteDataSource::GetLayer(int iLayer)

{
    if (iLayer < 0 || iLayer >= static_cast<int>(m_apoLayers.size()))
        return nullptr;
    else
        return m_apoLayers[iLayer].get();
}

/************************************************************************/
/*                           GetLayerByName()                           */
/************************************************************************/

OGRLayer *OGRSQLiteDataSource::GetLayerByName(const char *pszLayerName)

{
    OGRLayer *poLayer = GDALDataset::GetLayerByName(pszLayerName);
    if (poLayer != nullptr)
        return poLayer;

    for (auto &poLayerIter : m_apoInvisibleLayers)
    {
        if (EQUAL(poLayerIter->GetName(), pszLayerName))
            return poLayerIter.get();
    }

    std::string osName(pszLayerName);
    bool bIsTable = true;
    for (int i = 0; i < 2; i++)
    {
        char *pszSQL = sqlite3_mprintf("SELECT type FROM sqlite_master "
                                       "WHERE type IN ('table', 'view') AND "
                                       "lower(name) = lower('%q')",
                                       osName.c_str());
        int nRowCount = 0;
        char **papszResult = nullptr;
        CPL_IGNORE_RET_VAL(sqlite3_get_table(hDB, pszSQL, &papszResult,
                                             &nRowCount, nullptr, nullptr));
        if (papszResult && nRowCount == 1 && papszResult[1])
            bIsTable = strcmp(papszResult[1], "table") == 0;
        sqlite3_free_table(papszResult);
        sqlite3_free(pszSQL);
        if (i == 0 && nRowCount == 0)
        {
            const auto nParenthesis = osName.find('(');
            if (nParenthesis != std::string::npos && osName.back() == ')')
            {
                osName.resize(nParenthesis);
                continue;
            }
        }
        break;
    }

    if (!OpenTable(pszLayerName, bIsTable, /* bIsVirtualShape = */ false,
                   /* bMayEmitError = */ false))
        return nullptr;

    poLayer = m_apoLayers.back().get();
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    poLayer->GetLayerDefn();
    CPLPopErrorHandler();
    if (CPLGetLastErrorType() != 0)
    {
        CPLErrorReset();
        m_apoLayers.pop_back();
        return nullptr;
    }

    return poLayer;
}

/************************************************************************/
/*                    IsLayerPrivate()                                  */
/************************************************************************/

bool OGRSQLiteDataSource::IsLayerPrivate(int iLayer) const
{
    if (iLayer < 0 || iLayer >= static_cast<int>(m_apoLayers.size()))
        return false;

    const std::string osName(m_apoLayers[iLayer]->GetName());
    const CPLString osLCName(CPLString(osName).tolower());
    for (const char *systemTableName : {"spatialindex",
                                        "geom_cols_ref_sys",
                                        "geometry_columns",
                                        "geometry_columns_auth",
                                        "views_geometry_column",
                                        "virts_geometry_column",
                                        "spatial_ref_sys",
                                        "spatial_ref_sys_all",
                                        "spatial_ref_sys_aux",
                                        "sqlite_sequence",
                                        "tableprefix_metadata",
                                        "tableprefix_rasters",
                                        "layer_params",
                                        "layer_statistics",
                                        "layer_sub_classes",
                                        "layer_table_layout",
                                        "pattern_bitmaps",
                                        "symbol_bitmaps",
                                        "project_defs",
                                        "raster_pyramids",
                                        "sqlite_stat1",
                                        "sqlite_stat2",
                                        "spatialite_history",
                                        "geometry_columns_field_infos",
                                        "geometry_columns_statistics",
                                        "geometry_columns_time",
                                        "sql_statements_log",
                                        "vector_layers",
                                        "vector_layers_auth",
                                        "vector_layers_field_infos",
                                        "vector_layers_statistics",
                                        "views_geometry_columns_auth",
                                        "views_geometry_columns_field_infos",
                                        "views_geometry_columns_statistics",
                                        "virts_geometry_columns_auth",
                                        "virts_geometry_columns_field_infos",
                                        "virts_geometry_columns_statistics",
                                        "virts_layer_statistics",
                                        "views_layer_statistics",
                                        "elementarygeometries"})
    {
        if (osLCName == systemTableName)
            return true;
    }

    return false;
}

/************************************************************************/
/*                    GetLayerByNameNotVisible()                        */
/************************************************************************/

OGRLayer *
OGRSQLiteDataSource::GetLayerByNameNotVisible(const char *pszLayerName)

{
    {
        OGRLayer *poLayer = GDALDataset::GetLayerByName(pszLayerName);
        if (poLayer != nullptr)
            return poLayer;
    }

    for (auto &poLayerIter : m_apoInvisibleLayers)
    {
        if (EQUAL(poLayerIter->GetName(), pszLayerName))
            return poLayerIter.get();
    }

    /* -------------------------------------------------------------------- */
    /*      Create the layer object.                                        */
    /* -------------------------------------------------------------------- */
    auto poLayer = std::make_unique<OGRSQLiteTableLayer>(this);
    if (poLayer->Initialize(pszLayerName, true, false, false,
                            /* bMayEmitError = */ true) != CE_None)
    {
        return nullptr;
    }
    CPLErrorReset();
    CPLPushErrorHandler(CPLQuietErrorHandler);
    poLayer->GetLayerDefn();
    CPLPopErrorHandler();
    if (CPLGetLastErrorType() != 0)
    {
        CPLErrorReset();
        return nullptr;
    }
    m_apoInvisibleLayers.push_back(std::move(poLayer));

    return m_apoInvisibleLayers.back().get();
}

/************************************************************************/
/*                   GetLayerWithGetSpatialWhereByName()                */
/************************************************************************/

std::pair<OGRLayer *, IOGRSQLiteGetSpatialWhere *>
OGRSQLiteDataSource::GetLayerWithGetSpatialWhereByName(const char *pszName)
{
    OGRSQLiteLayer *poRet =
        cpl::down_cast<OGRSQLiteLayer *>(GetLayerByName(pszName));
    return std::pair<OGRLayer *, IOGRSQLiteGetSpatialWhere *>(poRet, poRet);
}

/************************************************************************/
/*                              FlushCache()                            */
/************************************************************************/

CPLErr OGRSQLiteDataSource::FlushCache(bool bAtClosing)
{
    CPLErr eErr = CE_None;
    for (auto &poLayer : m_apoLayers)
    {
        if (poLayer->IsTableLayer())
        {
            OGRSQLiteTableLayer *poTableLayer =
                cpl::down_cast<OGRSQLiteTableLayer *>(poLayer.get());
            if (poTableLayer->RunDeferredCreationIfNecessary() != OGRERR_NONE)
                eErr = CE_Failure;
            poTableLayer->CreateSpatialIndexIfNecessary();
        }
    }
    if (GDALDataset::FlushCache(bAtClosing) != CE_None)
        eErr = CE_Failure;
    return eErr;
}

/************************************************************************/
/*                             ExecuteSQL()                             */
/************************************************************************/

static const char *const apszFuncsWithSideEffects[] = {
    "InitSpatialMetaData",       "AddGeometryColumn",
    "RecoverGeometryColumn",     "DiscardGeometryColumn",
    "CreateSpatialIndex",        "CreateMbrCache",
    "DisableSpatialIndex",       "UpdateLayerStatistics",

    "ogr_datasource_load_layers"};

OGRLayer *OGRSQLiteDataSource::ExecuteSQL(const char *pszSQLCommand,
                                          OGRGeometry *poSpatialFilter,
                                          const char *pszDialect)

{
    for (auto &poLayer : m_apoLayers)
    {
        if (poLayer->IsTableLayer())
        {
            OGRSQLiteTableLayer *poTableLayer =
                cpl::down_cast<OGRSQLiteTableLayer *>(poLayer.get());
            poTableLayer->RunDeferredCreationIfNecessary();
            poTableLayer->CreateSpatialIndexIfNecessary();
        }
    }

    if (pszDialect != nullptr && EQUAL(pszDialect, "INDIRECT_SQLITE"))
        return GDALDataset::ExecuteSQL(pszSQLCommand, poSpatialFilter,
                                       "SQLITE");
    else if (pszDialect != nullptr && !EQUAL(pszDialect, "") &&
             !EQUAL(pszDialect, "NATIVE") && !EQUAL(pszDialect, "SQLITE"))

        return GDALDataset::ExecuteSQL(pszSQLCommand, poSpatialFilter,
                                       pszDialect);

    if (EQUAL(pszSQLCommand, "PRAGMA case_sensitive_like = 0") ||
        EQUAL(pszSQLCommand, "PRAGMA case_sensitive_like=0") ||
        EQUAL(pszSQLCommand, "PRAGMA case_sensitive_like =0") ||
        EQUAL(pszSQLCommand, "PRAGMA case_sensitive_like= 0"))
    {
        if (m_poSQLiteModule)
            OGR2SQLITE_SetCaseSensitiveLike(m_poSQLiteModule, false);
    }
    else if (EQUAL(pszSQLCommand, "PRAGMA case_sensitive_like = 1") ||
             EQUAL(pszSQLCommand, "PRAGMA case_sensitive_like=1") ||
             EQUAL(pszSQLCommand, "PRAGMA case_sensitive_like =1") ||
             EQUAL(pszSQLCommand, "PRAGMA case_sensitive_like= 1"))
    {
        if (m_poSQLiteModule)
            OGR2SQLITE_SetCaseSensitiveLike(m_poSQLiteModule, true);
    }

    /* -------------------------------------------------------------------- */
    /*      Special case DELLAYER: command.                                 */
    /* -------------------------------------------------------------------- */
    if (STARTS_WITH_CI(pszSQLCommand, "DELLAYER:"))
    {
        const char *pszLayerName = pszSQLCommand + 9;

        while (*pszLayerName == ' ')
            pszLayerName++;

        DeleteLayer(pszLayerName);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Special case for SQLITE_HAS_COLUMN_METADATA()                   */
    /* -------------------------------------------------------------------- */
    if (strcmp(pszSQLCommand, "SQLITE_HAS_COLUMN_METADATA()") == 0)
    {
#ifdef SQLITE_HAS_COLUMN_METADATA
        return new OGRSQLiteSingleFeatureLayer("SQLITE_HAS_COLUMN_METADATA",
                                               TRUE);
#else
        return new OGRSQLiteSingleFeatureLayer("SQLITE_HAS_COLUMN_METADATA",
                                               FALSE);
#endif
    }

    /* -------------------------------------------------------------------- */
    /*      In case, this is not a SELECT, invalidate cached feature        */
    /*      count and extent to be on the safe side.                        */
    /* -------------------------------------------------------------------- */
    if (EQUAL(pszSQLCommand, "VACUUM"))
    {
        int nNeedRefresh = -1;
        for (auto &poLayer : m_apoLayers)
        {
            if (poLayer->IsTableLayer())
            {
                OGRSQLiteTableLayer *poTableLayer =
                    cpl::down_cast<OGRSQLiteTableLayer *>(poLayer.get());
                if (!(poTableLayer->AreStatisticsValid()) ||
                    poTableLayer->DoStatisticsNeedToBeFlushed())
                {
                    nNeedRefresh = FALSE;
                    break;
                }
                else if (nNeedRefresh < 0)
                    nNeedRefresh = TRUE;
            }
        }
        if (nNeedRefresh == TRUE)
        {
            for (auto &poLayer : m_apoLayers)
            {
                if (poLayer->IsTableLayer())
                {
                    OGRSQLiteTableLayer *poTableLayer =
                        cpl::down_cast<OGRSQLiteTableLayer *>(poLayer.get());
                    poTableLayer->ForceStatisticsToBeFlushed();
                }
            }
        }
    }
    else if (ProcessTransactionSQL(pszSQLCommand))
    {
        return nullptr;
    }
    else if (!STARTS_WITH_CI(pszSQLCommand, "SELECT ") &&
             !STARTS_WITH_CI(pszSQLCommand, "CREATE TABLE ") &&
             !STARTS_WITH_CI(pszSQLCommand, "PRAGMA "))
    {
        for (auto &poLayer : m_apoLayers)
            poLayer->InvalidateCachedFeatureCountAndExtent();
    }

    m_bLastSQLCommandIsUpdateLayerStatistics =
        EQUAL(pszSQLCommand, "SELECT UpdateLayerStatistics()");

    /* -------------------------------------------------------------------- */
    /*      Prepare statement.                                              */
    /* -------------------------------------------------------------------- */
    sqlite3_stmt *hSQLStmt = nullptr;

    CPLString osSQLCommand = pszSQLCommand;

    /* This will speed-up layer creation */
    /* ORDER BY are costly to evaluate and are not necessary to establish */
    /* the layer definition. */
    bool bUseStatementForGetNextFeature = true;
    bool bEmptyLayer = false;

    if (osSQLCommand.ifind("SELECT ") == 0 &&
        CPLString(osSQLCommand.substr(1)).ifind("SELECT ") ==
            std::string::npos &&
        osSQLCommand.ifind(" UNION ") == std::string::npos &&
        osSQLCommand.ifind(" INTERSECT ") == std::string::npos &&
        osSQLCommand.ifind(" EXCEPT ") == std::string::npos)
    {
        size_t nOrderByPos = osSQLCommand.ifind(" ORDER BY ");
        if (nOrderByPos != std::string::npos)
        {
            osSQLCommand.resize(nOrderByPos);
            bUseStatementForGetNextFeature = false;
        }
    }

    int rc =
        prepareSql(GetDB(), osSQLCommand.c_str(),
                   static_cast<int>(osSQLCommand.size()), &hSQLStmt, nullptr);

    if (rc != SQLITE_OK)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "In ExecuteSQL(): sqlite3_prepare_v2(%s):\n  %s",
                 osSQLCommand.c_str(), sqlite3_errmsg(GetDB()));

        if (hSQLStmt != nullptr)
        {
            sqlite3_finalize(hSQLStmt);
        }

        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Do we get a resultset?                                          */
    /* -------------------------------------------------------------------- */
    rc = sqlite3_step(hSQLStmt);
    if (rc != SQLITE_ROW)
    {
        if (rc != SQLITE_DONE)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "In ExecuteSQL(): sqlite3_step(%s):\n  %s",
                     osSQLCommand.c_str(), sqlite3_errmsg(GetDB()));

            sqlite3_finalize(hSQLStmt);
            return nullptr;
        }

        if (STARTS_WITH_CI(pszSQLCommand, "CREATE "))
        {
            char **papszTokens = CSLTokenizeString(pszSQLCommand);
            if (CSLCount(papszTokens) >= 4 &&
                EQUAL(papszTokens[1], "VIRTUAL") &&
                EQUAL(papszTokens[2], "TABLE"))
            {
                OpenVirtualTable(papszTokens[3], pszSQLCommand);
            }
            CSLDestroy(papszTokens);

            sqlite3_finalize(hSQLStmt);
            return nullptr;
        }

        if (!STARTS_WITH_CI(pszSQLCommand, "SELECT "))
        {
            sqlite3_finalize(hSQLStmt);
            return nullptr;
        }

        bUseStatementForGetNextFeature = false;
        bEmptyLayer = true;
    }

    /* -------------------------------------------------------------------- */
    /*      Special case for some functions which must be run               */
    /*      only once                                                       */
    /* -------------------------------------------------------------------- */
    if (STARTS_WITH_CI(pszSQLCommand, "SELECT "))
    {
        for (unsigned int i = 0; i < sizeof(apszFuncsWithSideEffects) /
                                         sizeof(apszFuncsWithSideEffects[0]);
             i++)
        {
            if (EQUALN(apszFuncsWithSideEffects[i], pszSQLCommand + 7,
                       strlen(apszFuncsWithSideEffects[i])))
            {
                if (sqlite3_column_count(hSQLStmt) == 1 &&
                    sqlite3_column_type(hSQLStmt, 0) == SQLITE_INTEGER)
                {
                    const int ret = sqlite3_column_int(hSQLStmt, 0);

                    sqlite3_finalize(hSQLStmt);

                    return new OGRSQLiteSingleFeatureLayer(
                        apszFuncsWithSideEffects[i], ret);
                }
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Create layer.                                                   */
    /* -------------------------------------------------------------------- */

    CPLString osSQL = pszSQLCommand;
    OGRSQLiteSelectLayer *poLayer = new OGRSQLiteSelectLayer(
        this, osSQL, hSQLStmt, bUseStatementForGetNextFeature, bEmptyLayer,
        true, /*bCanReopenBaseDS=*/true);

    if (poSpatialFilter != nullptr &&
        poLayer->GetLayerDefn()->GetGeomFieldCount() > 0)
        poLayer->SetSpatialFilter(0, poSpatialFilter);

    return poLayer;
}

/************************************************************************/
/*                          ReleaseResultSet()                          */
/************************************************************************/

void OGRSQLiteDataSource::ReleaseResultSet(OGRLayer *poLayer)

{
    delete poLayer;
}

/************************************************************************/
/*                           ICreateLayer()                             */
/************************************************************************/

OGRLayer *
OGRSQLiteDataSource::ICreateLayer(const char *pszLayerNameIn,
                                  const OGRGeomFieldDefn *poGeomFieldDefn,
                                  CSLConstList papszOptions)

{
    /* -------------------------------------------------------------------- */
    /*      Verify we are in update mode.                                   */
    /* -------------------------------------------------------------------- */
    char *pszLayerName = nullptr;
    if (!GetUpdate())
    {
        CPLError(CE_Failure, CPLE_NoWriteAccess,
                 "Data source %s opened read-only.\n"
                 "New layer %s cannot be created.\n",
                 m_pszFilename, pszLayerNameIn);

        return nullptr;
    }

    const auto eType = poGeomFieldDefn ? poGeomFieldDefn->GetType() : wkbNone;
    const auto poSRS =
        poGeomFieldDefn ? poGeomFieldDefn->GetSpatialRef() : nullptr;

    if (m_bIsSpatiaLiteDB && eType != wkbNone)
    {
        // We need to catch this right now as AddGeometryColumn does not
        // return an error
        OGRwkbGeometryType eFType = wkbFlatten(eType);
        if (eFType > wkbGeometryCollection)
        {
            CPLError(CE_Failure, CPLE_NotSupported,
                     "Cannot create geometry field of type %s",
                     OGRToOGCGeomType(eType));
            return nullptr;
        }
    }

    for (auto &poLayer : m_apoLayers)
    {
        if (poLayer->IsTableLayer())
        {
            OGRSQLiteTableLayer *poTableLayer =
                cpl::down_cast<OGRSQLiteTableLayer *>(poLayer.get());
            poTableLayer->RunDeferredCreationIfNecessary();
        }
    }

    CPLString osFIDColumnName;
    const char *pszFIDColumnNameIn =
        CSLFetchNameValueDef(papszOptions, "FID", "OGC_FID");
    if (CPLFetchBool(papszOptions, "LAUNDER", true))
    {
        char *pszFIDColumnName = LaunderName(pszFIDColumnNameIn);
        osFIDColumnName = pszFIDColumnName;
        CPLFree(pszFIDColumnName);
    }
    else
        osFIDColumnName = pszFIDColumnNameIn;

    if (CPLFetchBool(papszOptions, "LAUNDER", true))
        pszLayerName = LaunderName(pszLayerNameIn);
    else
        pszLayerName = CPLStrdup(pszLayerNameIn);

    const char *pszGeomFormat = CSLFetchNameValue(papszOptions, "FORMAT");
    if (pszGeomFormat == nullptr)
    {
        if (!m_bIsSpatiaLiteDB)
            pszGeomFormat = "WKB";
        else
            pszGeomFormat = "SpatiaLite";
    }

    if (!EQUAL(pszGeomFormat, "WKT") && !EQUAL(pszGeomFormat, "WKB") &&
        !EQUAL(pszGeomFormat, "SpatiaLite"))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "FORMAT=%s not recognised or supported.", pszGeomFormat);
        CPLFree(pszLayerName);
        return nullptr;
    }

    CPLString osGeometryName;
    const char *pszGeometryNameIn =
        CSLFetchNameValue(papszOptions, "GEOMETRY_NAME");
    if (pszGeometryNameIn == nullptr)
    {
        osGeometryName =
            (EQUAL(pszGeomFormat, "WKT")) ? "WKT_GEOMETRY" : "GEOMETRY";
    }
    else
    {
        if (CPLFetchBool(papszOptions, "LAUNDER", true))
        {
            char *pszGeometryName = LaunderName(pszGeometryNameIn);
            osGeometryName = pszGeometryName;
            CPLFree(pszGeometryName);
        }
        else
            osGeometryName = pszGeometryNameIn;
    }

    if (m_bIsSpatiaLiteDB && !EQUAL(pszGeomFormat, "SpatiaLite"))
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "FORMAT=%s not supported on a SpatiaLite enabled database.",
                 pszGeomFormat);
        CPLFree(pszLayerName);
        return nullptr;
    }

    // Should not happen since a spatialite DB should be opened in
    // read-only mode if libspatialite is not loaded.
    if (m_bIsSpatiaLiteDB && !IsSpatialiteLoaded())
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Creating layers on a SpatiaLite enabled database, "
                 "without Spatialite extensions loaded, is not supported.");
        CPLFree(pszLayerName);
        return nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      Do we already have this layer?  If so, should we blow it        */
    /*      away?                                                           */
    /* -------------------------------------------------------------------- */
    for (auto &poLayer : m_apoLayers)
    {
        if (EQUAL(pszLayerName, poLayer->GetLayerDefn()->GetName()))
        {
            if (CSLFetchNameValue(papszOptions, "OVERWRITE") != nullptr &&
                !EQUAL(CSLFetchNameValue(papszOptions, "OVERWRITE"), "NO"))
            {
                DeleteLayer(pszLayerName);
                break;
            }
            else
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Layer %s already exists, CreateLayer failed.\n"
                         "Use the layer creation option OVERWRITE=YES to "
                         "replace it.",
                         pszLayerName);
                CPLFree(pszLayerName);
                return nullptr;
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Try to get the SRS Id of this spatial reference system,         */
    /*      adding to the srs table if needed.                              */
    /* -------------------------------------------------------------------- */
    int nSRSId = m_nUndefinedSRID;
    const char *pszSRID = CSLFetchNameValue(papszOptions, "SRID");

    if (pszSRID != nullptr && pszSRID[0] != '\0')
    {
        nSRSId = atoi(pszSRID);
        if (nSRSId > 0)
        {
            OGRSpatialReference *poSRSFetched = FetchSRS(nSRSId);
            if (poSRSFetched == nullptr)
            {
                CPLError(CE_Warning, CPLE_AppDefined,
                         "SRID %d will be used, but no matching SRS is defined "
                         "in spatial_ref_sys",
                         nSRSId);
            }
        }
    }
    else if (poSRS != nullptr)
        nSRSId = FetchSRSId(poSRS);

    bool bImmediateSpatialIndexCreation = false;
    bool bDeferredSpatialIndexCreation = false;

    const char *pszSI = CSLFetchNameValue(papszOptions, "SPATIAL_INDEX");
    if (m_bHaveGeometryColumns && eType != wkbNone)
    {
        if (pszSI != nullptr && CPLTestBool(pszSI) &&
            (m_bIsSpatiaLiteDB || EQUAL(pszGeomFormat, "SpatiaLite")) &&
            !IsSpatialiteLoaded())
        {
            CPLError(CE_Warning, CPLE_OpenFailed,
                     "Cannot create a spatial index when Spatialite extensions "
                     "are not loaded.");
        }

#ifdef HAVE_SPATIALITE
        /* Only if linked against SpatiaLite and the datasource was created as a
         * SpatiaLite DB */
        if (m_bIsSpatiaLiteDB && IsSpatialiteLoaded())
#else
        if (0)
#endif
        {
            if (pszSI != nullptr && EQUAL(pszSI, "IMMEDIATE"))
            {
                bImmediateSpatialIndexCreation = true;
            }
            else if (pszSI == nullptr || CPLTestBool(pszSI))
            {
                bDeferredSpatialIndexCreation = true;
            }
        }
    }
    else if (m_bHaveGeometryColumns)
    {
#ifdef HAVE_SPATIALITE
        if (m_bIsSpatiaLiteDB && IsSpatialiteLoaded() &&
            (pszSI == nullptr || CPLTestBool(pszSI)))
            bDeferredSpatialIndexCreation = true;
#endif
    }

    /* -------------------------------------------------------------------- */
    /*      Create the layer object.                                        */
    /* -------------------------------------------------------------------- */
    auto poLayer = std::make_unique<OGRSQLiteTableLayer>(this);

    poLayer->Initialize(pszLayerName, true, false, true,
                        /* bMayEmitError = */ false);
    OGRSpatialReference *poSRSClone = nullptr;
    if (poSRS)
    {
        poSRSClone = poSRS->Clone();
        poSRSClone->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    }
    poLayer->SetCreationParameters(osFIDColumnName, eType, pszGeomFormat,
                                   osGeometryName, poSRSClone, nSRSId);
    if (poSRSClone)
        poSRSClone->Release();

    poLayer->InitFeatureCount();
    poLayer->SetLaunderFlag(CPLFetchBool(papszOptions, "LAUNDER", true));
    if (CPLFetchBool(papszOptions, "COMPRESS_GEOM", false))
        poLayer->SetUseCompressGeom(true);
    if (bImmediateSpatialIndexCreation)
        poLayer->CreateSpatialIndex(0);
    else if (bDeferredSpatialIndexCreation)
        poLayer->SetDeferredSpatialIndexCreation(true);
    poLayer->SetCompressedColumns(
        CSLFetchNameValue(papszOptions, "COMPRESS_COLUMNS"));
    poLayer->SetStrictFlag(CPLFetchBool(papszOptions, "STRICT", false));

    CPLFree(pszLayerName);

    /* -------------------------------------------------------------------- */
    /*      Add layer to data source layer list.                            */
    /* -------------------------------------------------------------------- */
    m_apoLayers.push_back(std::move(poLayer));

    return m_apoLayers.back().get();
}

/************************************************************************/
/*                            LaunderName()                             */
/************************************************************************/

char *OGRSQLiteDataSource::LaunderName(const char *pszSrcName)

{
    char *pszSafeName = CPLStrdup(pszSrcName);
    for (int i = 0; pszSafeName[i] != '\0'; i++)
    {
        pszSafeName[i] = static_cast<char>(
            CPLTolower(static_cast<unsigned char>(pszSafeName[i])));
        if (pszSafeName[i] == '\'' || pszSafeName[i] == '-' ||
            pszSafeName[i] == '#')
            pszSafeName[i] = '_';
    }

    return pszSafeName;
}

/************************************************************************/
/*                            DeleteLayer()                             */
/************************************************************************/

void OGRSQLiteDataSource::DeleteLayer(const char *pszLayerName)

{
    /* -------------------------------------------------------------------- */
    /*      Verify we are in update mode.                                   */
    /* -------------------------------------------------------------------- */
    if (!GetUpdate())
    {
        CPLError(CE_Failure, CPLE_NoWriteAccess,
                 "Data source %s opened read-only.\n"
                 "Layer %s cannot be deleted.\n",
                 m_pszFilename, pszLayerName);

        return;
    }

    /* -------------------------------------------------------------------- */
    /*      Try to find layer.                                              */
    /* -------------------------------------------------------------------- */
    int iLayer = 0;  // Used after for.

    for (; iLayer < static_cast<int>(m_apoLayers.size()); iLayer++)
    {
        if (EQUAL(pszLayerName, m_apoLayers[iLayer]->GetLayerDefn()->GetName()))
            break;
    }

    if (iLayer == static_cast<int>(m_apoLayers.size()))
    {
        CPLError(
            CE_Failure, CPLE_AppDefined,
            "Attempt to delete layer '%s', but this layer is not known to OGR.",
            pszLayerName);
        return;
    }

    DeleteLayer(iLayer);
}

/************************************************************************/
/*                            DeleteLayer()                             */
/************************************************************************/

OGRErr OGRSQLiteDataSource::DeleteLayer(int iLayer)
{
    if (iLayer < 0 || iLayer >= static_cast<int>(m_apoLayers.size()))
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Layer %d not in legal range of 0 to %d.", iLayer,
                 static_cast<int>(m_apoLayers.size()) - 1);
        return OGRERR_FAILURE;
    }

    CPLString osLayerName = GetLayer(iLayer)->GetName();
    CPLString osGeometryColumn = GetLayer(iLayer)->GetGeometryColumn();

    /* -------------------------------------------------------------------- */
    /*      Blow away our OGR structures related to the layer.  This is     */
    /*      pretty dangerous if anything has a reference to this layer!     */
    /* -------------------------------------------------------------------- */
    CPLDebug("OGR_SQLITE", "DeleteLayer(%s)", osLayerName.c_str());

    m_apoLayers.erase(m_apoLayers.begin() + iLayer);

    /* -------------------------------------------------------------------- */
    /*      Remove from the database.                                       */
    /* -------------------------------------------------------------------- */
    CPLString osEscapedLayerName = SQLEscapeLiteral(osLayerName);
    const char *pszEscapedLayerName = osEscapedLayerName.c_str();
    const char *pszGeometryColumn =
        osGeometryColumn.size() ? osGeometryColumn.c_str() : nullptr;

    if (SQLCommand(hDB, CPLSPrintf("DROP TABLE '%s'", pszEscapedLayerName)) !=
        OGRERR_NONE)
    {
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Drop from geometry_columns table.                               */
    /* -------------------------------------------------------------------- */
    if (m_bHaveGeometryColumns)
    {
        CPLString osCommand;

        osCommand.Printf(
            "DELETE FROM geometry_columns WHERE f_table_name = '%s'",
            pszEscapedLayerName);

        if (SQLCommand(hDB, osCommand) != OGRERR_NONE)
        {
            return OGRERR_FAILURE;
        }

        /* --------------------------------------------------------------------
         */
        /*      Drop spatialite spatial index tables */
        /* --------------------------------------------------------------------
         */
        if (m_bIsSpatiaLiteDB && pszGeometryColumn)
        {
            osCommand.Printf("DROP TABLE 'idx_%s_%s'", pszEscapedLayerName,
                             SQLEscapeLiteral(pszGeometryColumn).c_str());
            CPL_IGNORE_RET_VAL(
                sqlite3_exec(hDB, osCommand, nullptr, nullptr, nullptr));

            osCommand.Printf("DROP TABLE 'idx_%s_%s_node'", pszEscapedLayerName,
                             SQLEscapeLiteral(pszGeometryColumn).c_str());
            CPL_IGNORE_RET_VAL(
                sqlite3_exec(hDB, osCommand, nullptr, nullptr, nullptr));

            osCommand.Printf("DROP TABLE 'idx_%s_%s_parent'",
                             pszEscapedLayerName,
                             SQLEscapeLiteral(pszGeometryColumn).c_str());
            CPL_IGNORE_RET_VAL(
                sqlite3_exec(hDB, osCommand, nullptr, nullptr, nullptr));

            osCommand.Printf("DROP TABLE 'idx_%s_%s_rowid'",
                             pszEscapedLayerName,
                             SQLEscapeLiteral(pszGeometryColumn).c_str());
            CPL_IGNORE_RET_VAL(
                sqlite3_exec(hDB, osCommand, nullptr, nullptr, nullptr));
        }
    }
    return OGRERR_NONE;
}

/************************************************************************/
/*                         StartTransaction()                           */
/*                                                                      */
/* Should only be called by user code. Not driver internals.            */
/************************************************************************/

OGRErr OGRSQLiteBaseDataSource::StartTransaction(CPL_UNUSED int bForce)
{
    if (m_bUserTransactionActive || m_nSoftTransactionLevel != 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Transaction already established");
        return OGRERR_FAILURE;
    }

    // Check if we are in a SAVEPOINT transaction
    if (m_aosSavepoints.size() > 0)
    {
        CPLError(CE_Failure, CPLE_AppDefined,
                 "Cannot start a transaction within a SAVEPOINT");
        return OGRERR_FAILURE;
    }

    OGRErr eErr = SoftStartTransaction();
    if (eErr != OGRERR_NONE)
        return eErr;

    m_bUserTransactionActive = true;
    return OGRERR_NONE;
}

OGRErr OGRSQLiteDataSource::StartTransaction(int bForce)
{
    for (auto &poLayer : m_apoLayers)
    {
        if (poLayer->IsTableLayer())
        {
            OGRSQLiteTableLayer *poTableLayer =
                cpl::down_cast<OGRSQLiteTableLayer *>(poLayer.get());
            poTableLayer->RunDeferredCreationIfNecessary();
        }
    }

    return OGRSQLiteBaseDataSource::StartTransaction(bForce);
}

/************************************************************************/
/*                         CommitTransaction()                          */
/*                                                                      */
/* Should only be called by user code. Not driver internals.            */
/************************************************************************/

OGRErr OGRSQLiteBaseDataSource::CommitTransaction()
{
    if (!m_bUserTransactionActive && !m_bImplicitTransactionOpened)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Transaction not established");
        return OGRERR_FAILURE;
    }

    m_bUserTransactionActive = false;
    m_bImplicitTransactionOpened = false;
    CPLAssert(m_nSoftTransactionLevel == 1);
    return SoftCommitTransaction();
}

OGRErr OGRSQLiteDataSource::CommitTransaction()

{
    if (m_nSoftTransactionLevel == 1)
    {
        for (auto &poLayer : m_apoLayers)
        {
            if (poLayer->IsTableLayer())
            {
                OGRSQLiteTableLayer *poTableLayer =
                    cpl::down_cast<OGRSQLiteTableLayer *>(poLayer.get());
                poTableLayer->RunDeferredCreationIfNecessary();
            }
        }
    }

    return OGRSQLiteBaseDataSource::CommitTransaction();
}

/************************************************************************/
/*                        RollbackTransaction()                         */
/*                                                                      */
/* Should only be called by user code. Not driver internals.            */
/************************************************************************/

OGRErr OGRSQLiteBaseDataSource::RollbackTransaction()
{
    if (!m_bUserTransactionActive)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Transaction not established");
        return OGRERR_FAILURE;
    }

    m_bUserTransactionActive = false;
    CPLAssert(m_nSoftTransactionLevel == 1);

    return SoftRollbackTransaction();
}

OGRErr OGRSQLiteDataSource::RollbackTransaction()

{
    if (m_nSoftTransactionLevel == 1)
    {
        for (auto &poLayer : m_apoLayers)
        {
            if (poLayer->IsTableLayer())
            {
                OGRSQLiteTableLayer *poTableLayer =
                    cpl::down_cast<OGRSQLiteTableLayer *>(poLayer.get());
                poTableLayer->RunDeferredCreationIfNecessary();
            }
        }

        for (auto &poLayer : m_apoLayers)
        {
            poLayer->InvalidateCachedFeatureCountAndExtent();
            poLayer->ResetReading();
        }
    }

    return OGRSQLiteBaseDataSource::RollbackTransaction();
}

bool OGRSQLiteBaseDataSource::IsInTransaction() const
{
    return m_nSoftTransactionLevel > 0;
}

/************************************************************************/
/*                        SoftStartTransaction()                        */
/*                                                                      */
/*      Create a transaction scope.  If we already have a               */
/*      transaction active this isn't a real transaction, but just      */
/*      an increment to the scope count.                                */
/************************************************************************/

OGRErr OGRSQLiteBaseDataSource::SoftStartTransaction()

{
    m_nSoftTransactionLevel++;

    OGRErr eErr = OGRERR_NONE;
    if (m_nSoftTransactionLevel == 1)
    {
        for (int i = 0; i < GetLayerCount(); i++)
        {
            OGRLayer *poLayer = GetLayer(i);
            poLayer->PrepareStartTransaction();
        }

        eErr = DoTransactionCommand("BEGIN");
    }

    // CPLDebug("SQLite", "%p->SoftStartTransaction() : %d",
    //          this, nSoftTransactionLevel);

    return eErr;
}

/************************************************************************/
/*                     SoftCommitTransaction()                          */
/*                                                                      */
/*      Commit the current transaction if we are at the outer           */
/*      scope.                                                          */
/************************************************************************/

OGRErr OGRSQLiteBaseDataSource::SoftCommitTransaction()

{
    // CPLDebug("SQLite", "%p->SoftCommitTransaction() : %d",
    //          this, nSoftTransactionLevel);

    if (m_nSoftTransactionLevel <= 0)
    {
        CPLAssert(false);
        return OGRERR_FAILURE;
    }

    OGRErr eErr = OGRERR_NONE;
    m_nSoftTransactionLevel--;
    if (m_nSoftTransactionLevel == 0)
    {
        eErr = DoTransactionCommand("COMMIT");
    }

    return eErr;
}

/************************************************************************/
/*                  SoftRollbackTransaction()                           */
/*                                                                      */
/*      Do a rollback of the current transaction if we are at the 1st   */
/*      level                                                           */
/************************************************************************/

OGRErr OGRSQLiteBaseDataSource::SoftRollbackTransaction()

{
    // CPLDebug("SQLite", "%p->SoftRollbackTransaction() : %d",
    //          this, nSoftTransactionLevel);

    while (!m_aosSavepoints.empty())
    {
        if (RollbackToSavepoint(m_aosSavepoints.back()) != OGRERR_NONE)
        {
            return OGRERR_FAILURE;
        }
        m_aosSavepoints.pop_back();
    }

    if (m_nSoftTransactionLevel <= 0)
    {
        CPLAssert(false);
        return OGRERR_FAILURE;
    }

    OGRErr eErr = OGRERR_NONE;
    m_nSoftTransactionLevel--;
    if (m_nSoftTransactionLevel == 0)
    {
        eErr = DoTransactionCommand("ROLLBACK");
        if (eErr == OGRERR_NONE)
        {
            for (int i = 0; i < GetLayerCount(); i++)
            {
                OGRLayer *poLayer = GetLayer(i);
                poLayer->FinishRollbackTransaction("");
            }
        }
    }

    return eErr;
}

OGRErr OGRSQLiteBaseDataSource::StartSavepoint(const std::string &osName)
{

    // A SAVEPOINT implicitly starts a transaction, let's fake one
    if (!IsInTransaction())
    {
        m_bImplicitTransactionOpened = true;
        m_nSoftTransactionLevel++;
        for (int i = 0; i < GetLayerCount(); i++)
        {
            OGRLayer *poLayer = GetLayer(i);
            poLayer->PrepareStartTransaction();
        }
    }

    const std::string osCommand = "SAVEPOINT " + osName;
    const auto eErr = DoTransactionCommand(osCommand.c_str());

    if (eErr == OGRERR_NONE)
    {
        m_aosSavepoints.push_back(osName);
    }

    return eErr;
}

OGRErr OGRSQLiteBaseDataSource::ReleaseSavepoint(const std::string &osName)
{
    if (m_aosSavepoints.empty() ||
        std::find(m_aosSavepoints.cbegin(), m_aosSavepoints.cend(), osName) ==
            m_aosSavepoints.cend())
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Savepoint %s not found",
                 osName.c_str());
        return OGRERR_FAILURE;
    }

    const std::string osCommand = "RELEASE SAVEPOINT " + osName;
    const auto eErr = DoTransactionCommand(osCommand.c_str());

    if (eErr == OGRERR_NONE)
    {
        // If the savepoint is the outer most, this is the same as COMMIT
        // and the transaction is closed
        if (m_bImplicitTransactionOpened &&
            m_aosSavepoints.front().compare(osName) == 0)
        {
            m_bImplicitTransactionOpened = false;
            m_bUserTransactionActive = false;
            m_nSoftTransactionLevel = 0;
            m_aosSavepoints.clear();
        }
        else
        {
            // Find all savepoints up to the target one and remove them
            while (!m_aosSavepoints.empty() && m_aosSavepoints.back() != osName)
            {
                m_aosSavepoints.pop_back();
            }
            if (!m_aosSavepoints.empty())  // should always be true
            {
                m_aosSavepoints.pop_back();
            }
        }
    }
    return eErr;
}

OGRErr OGRSQLiteBaseDataSource::RollbackToSavepoint(const std::string &osName)
{
    if (m_aosSavepoints.empty() ||
        std::find(m_aosSavepoints.cbegin(), m_aosSavepoints.cend(), osName) ==
            m_aosSavepoints.cend())
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Savepoint %s not found",
                 osName.c_str());
        return OGRERR_FAILURE;
    }

    const std::string osCommand = "ROLLBACK TO SAVEPOINT " + osName;
    const auto eErr = DoTransactionCommand(osCommand.c_str());

    if (eErr == OGRERR_NONE)
    {

        // The target savepoint should become the last one in the list
        // and does not need to be removed because ROLLBACK TO SAVEPOINT
        while (!m_aosSavepoints.empty() && m_aosSavepoints.back() != osName)
        {
            m_aosSavepoints.pop_back();
        }
    }

    for (int i = 0; i < GetLayerCount(); i++)
    {
        OGRLayer *poLayer = GetLayer(i);
        poLayer->FinishRollbackTransaction(osName);
    }

    return eErr;
}

/************************************************************************/
/*                          ProcessTransactionSQL()                     */
/************************************************************************/
bool OGRSQLiteBaseDataSource::ProcessTransactionSQL(
    const std::string &osSQLCommand)
{
    bool retVal = true;

    if (EQUAL(osSQLCommand.c_str(), "BEGIN"))
    {
        SoftStartTransaction();
    }
    else if (EQUAL(osSQLCommand.c_str(), "COMMIT"))
    {
        SoftCommitTransaction();
    }
    else if (EQUAL(osSQLCommand.c_str(), "ROLLBACK"))
    {
        SoftRollbackTransaction();
    }
    else if (STARTS_WITH_CI(osSQLCommand.c_str(), "SAVEPOINT"))
    {
        const CPLStringList aosTokens(SQLTokenize(osSQLCommand.c_str()));
        if (aosTokens.size() == 2)
        {
            const char *pszSavepointName = aosTokens[1];
            StartSavepoint(pszSavepointName);
        }
        else
        {
            retVal = false;
        }
    }
    else if (STARTS_WITH_CI(osSQLCommand.c_str(), "RELEASE"))
    {
        const CPLStringList aosTokens(SQLTokenize(osSQLCommand.c_str()));
        if (aosTokens.size() == 2)
        {
            const char *pszSavepointName = aosTokens[1];
            ReleaseSavepoint(pszSavepointName);
        }
        else if (aosTokens.size() == 3 && EQUAL(aosTokens[1], "SAVEPOINT"))
        {
            const char *pszSavepointName = aosTokens[2];
            ReleaseSavepoint(pszSavepointName);
        }
        else
        {
            retVal = false;
        }
    }
    else if (STARTS_WITH_CI(osSQLCommand.c_str(), "ROLLBACK"))
    {
        const CPLStringList aosTokens(SQLTokenize(osSQLCommand.c_str()));
        if (aosTokens.size() == 2)
        {
            if (EQUAL(aosTokens[1], "TRANSACTION"))
            {
                SoftRollbackTransaction();
            }
            else
            {
                const char *pszSavepointName = aosTokens[1];
                RollbackToSavepoint(pszSavepointName);
            }
        }
        else if (aosTokens.size() > 1)  // Savepoint name is last token
        {
            const char *pszSavepointName = aosTokens[aosTokens.size() - 1];
            RollbackToSavepoint(pszSavepointName);
        }
    }
    else
    {
        retVal = false;
    }

    return retVal;
}

/************************************************************************/
/*                          DoTransactionCommand()                      */
/************************************************************************/

OGRErr OGRSQLiteBaseDataSource::DoTransactionCommand(const char *pszCommand)

{
#ifdef DEBUG
    CPLDebug("OGR_SQLITE", "%s Transaction", pszCommand);
#endif

    return SQLCommand(hDB, pszCommand);
}

/************************************************************************/
/*                          GetSRTEXTColName()                        */
/************************************************************************/

const char *OGRSQLiteDataSource::GetSRTEXTColName()
{
    if (!m_bIsSpatiaLiteDB || m_bSpatialite4Layout)
        return "srtext";

    // Testing for SRS_WKT column presence.
    bool bHasSrsWkt = false;
    char **papszResult = nullptr;
    int nRowCount = 0;
    int nColCount = 0;
    char *pszErrMsg = nullptr;
    const int rc =
        sqlite3_get_table(hDB, "PRAGMA table_info(spatial_ref_sys)",
                          &papszResult, &nRowCount, &nColCount, &pszErrMsg);

    if (rc == SQLITE_OK)
    {
        for (int iRow = 1; iRow <= nRowCount; iRow++)
        {
            if (EQUAL("srs_wkt", papszResult[(iRow * nColCount) + 1]))
                bHasSrsWkt = true;
        }
        sqlite3_free_table(papszResult);
    }
    else
    {
        sqlite3_free(pszErrMsg);
    }

    return bHasSrsWkt ? "srs_wkt" : nullptr;
}

/************************************************************************/
/*                         AddSRIDToCache()                             */
/*                                                                      */
/*      Note: this will not add a reference on the poSRS object. Make   */
/*      sure it is freshly created, or add a reference yourself if not. */
/************************************************************************/

OGRSpatialReference *OGRSQLiteDataSource::AddSRIDToCache(
    int nId,
    std::unique_ptr<OGRSpatialReference, OGRSpatialReferenceReleaser> &&poSRS)
{
    /* -------------------------------------------------------------------- */
    /*      Add to the cache.                                               */
    /* -------------------------------------------------------------------- */
    auto oIter = m_oSRSCache.emplace(nId, std::move(poSRS)).first;
    return oIter->second.get();
}

/************************************************************************/
/*                             FetchSRSId()                             */
/*                                                                      */
/*      Fetch the id corresponding to an SRS, and if not found, add     */
/*      it to the table.                                                */
/************************************************************************/

int OGRSQLiteDataSource::FetchSRSId(const OGRSpatialReference *poSRS)

{
    int nSRSId = m_nUndefinedSRID;
    if (poSRS == nullptr)
        return nSRSId;

    /* -------------------------------------------------------------------- */
    /*      First, we look through our SRID cache, is it there?             */
    /* -------------------------------------------------------------------- */
    for (const auto &pair : m_oSRSCache)
    {
        if (pair.second.get() == poSRS)
            return pair.first;
    }
    for (const auto &pair : m_oSRSCache)
    {
        if (pair.second != nullptr && pair.second->IsSame(poSRS))
            return pair.first;
    }

    /* -------------------------------------------------------------------- */
    /*      Build a copy since we may call AutoIdentifyEPSG()               */
    /* -------------------------------------------------------------------- */
    OGRSpatialReference oSRS(*poSRS);
    poSRS = nullptr;

    const char *pszAuthorityName = oSRS.GetAuthorityName(nullptr);
    const char *pszAuthorityCode = nullptr;

    if (pszAuthorityName == nullptr || strlen(pszAuthorityName) == 0)
    {
        /* --------------------------------------------------------------------
         */
        /*      Try to identify an EPSG code */
        /* --------------------------------------------------------------------
         */
        oSRS.AutoIdentifyEPSG();

        pszAuthorityName = oSRS.GetAuthorityName(nullptr);
        if (pszAuthorityName != nullptr && EQUAL(pszAuthorityName, "EPSG"))
        {
            pszAuthorityCode = oSRS.GetAuthorityCode(nullptr);
            if (pszAuthorityCode != nullptr && strlen(pszAuthorityCode) > 0)
            {
                /* Import 'clean' SRS */
                oSRS.importFromEPSG(atoi(pszAuthorityCode));

                pszAuthorityName = oSRS.GetAuthorityName(nullptr);
                pszAuthorityCode = oSRS.GetAuthorityCode(nullptr);
            }
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Check whether the EPSG authority code is already mapped to a    */
    /*      SRS ID.                                                         */
    /* -------------------------------------------------------------------- */
    char *pszErrMsg = nullptr;
    CPLString osCommand;
    char **papszResult = nullptr;
    int nRowCount = 0;
    int nColCount = 0;

    if (pszAuthorityName != nullptr && strlen(pszAuthorityName) > 0)
    {
        pszAuthorityCode = oSRS.GetAuthorityCode(nullptr);

        if (pszAuthorityCode != nullptr && strlen(pszAuthorityCode) > 0)
        {
            // XXX: We are using case insensitive comparison for "auth_name"
            // values, because there are variety of options exist. By default
            // the driver uses 'EPSG' in upper case, but SpatiaLite extension
            // uses 'epsg' in lower case.
            osCommand.Printf(
                "SELECT srid FROM spatial_ref_sys WHERE "
                "auth_name = '%s' COLLATE NOCASE AND auth_srid = '%s' "
                "LIMIT 2",
                pszAuthorityName, pszAuthorityCode);

            int rc = sqlite3_get_table(hDB, osCommand, &papszResult, &nRowCount,
                                       &nColCount, &pszErrMsg);
            if (rc != SQLITE_OK)
            {
                /* Retry without COLLATE NOCASE which may not be understood by
                 * older sqlite3 */
                sqlite3_free(pszErrMsg);

                osCommand.Printf("SELECT srid FROM spatial_ref_sys WHERE "
                                 "auth_name = '%s' AND auth_srid = '%s'",
                                 pszAuthorityName, pszAuthorityCode);

                rc = sqlite3_get_table(hDB, osCommand, &papszResult, &nRowCount,
                                       &nColCount, &pszErrMsg);

                /* Retry in lower case for SpatiaLite */
                if (rc != SQLITE_OK)
                {
                    sqlite3_free(pszErrMsg);
                }
                else if (nRowCount == 0 &&
                         strcmp(pszAuthorityName, "EPSG") == 0)
                {
                    /* If it is in upper case, look for lower case */
                    sqlite3_free_table(papszResult);

                    osCommand.Printf("SELECT srid FROM spatial_ref_sys WHERE "
                                     "auth_name = 'epsg' AND auth_srid = '%s' "
                                     "LIMIT 2",
                                     pszAuthorityCode);

                    rc = sqlite3_get_table(hDB, osCommand, &papszResult,
                                           &nRowCount, &nColCount, &pszErrMsg);

                    if (rc != SQLITE_OK)
                    {
                        sqlite3_free(pszErrMsg);
                    }
                }
            }

            if (rc == SQLITE_OK && nRowCount == 1)
            {
                nSRSId = (papszResult[1] != nullptr) ? atoi(papszResult[1])
                                                     : m_nUndefinedSRID;
                sqlite3_free_table(papszResult);

                if (nSRSId != m_nUndefinedSRID)
                {
                    std::unique_ptr<OGRSpatialReference,
                                    OGRSpatialReferenceReleaser>
                        poCachedSRS;
                    poCachedSRS.reset(oSRS.Clone());
                    if (poCachedSRS)
                    {
                        poCachedSRS->SetAxisMappingStrategy(
                            OAMS_TRADITIONAL_GIS_ORDER);
                    }
                    AddSRIDToCache(nSRSId, std::move(poCachedSRS));
                }

                return nSRSId;
            }
            sqlite3_free_table(papszResult);
        }
    }

    /* -------------------------------------------------------------------- */
    /*      Search for existing record using either WKT definition or       */
    /*      PROJ.4 string (SpatiaLite variant).                             */
    /* -------------------------------------------------------------------- */
    CPLString osWKT;
    CPLString osProj4;

    /* -------------------------------------------------------------------- */
    /*      Translate SRS to WKT.                                           */
    /* -------------------------------------------------------------------- */
    char *pszWKT = nullptr;

    if (oSRS.exportToWkt(&pszWKT) != OGRERR_NONE)
    {
        CPLFree(pszWKT);
        return m_nUndefinedSRID;
    }

    osWKT = pszWKT;
    CPLFree(pszWKT);
    pszWKT = nullptr;

    const char *pszSRTEXTColName = GetSRTEXTColName();

    if (pszSRTEXTColName != nullptr)
    {
        /* --------------------------------------------------------------------
         */
        /*      Try to find based on the WKT match. */
        /* --------------------------------------------------------------------
         */
        osCommand.Printf("SELECT srid FROM spatial_ref_sys WHERE \"%s\" = ? "
                         "LIMIT 2",
                         SQLEscapeName(pszSRTEXTColName).c_str());
    }

    /* -------------------------------------------------------------------- */
    /*      Handle SpatiaLite (< 4) flavor of the spatial_ref_sys.         */
    /* -------------------------------------------------------------------- */
    else
    {
        /* --------------------------------------------------------------------
         */
        /*      Translate SRS to PROJ.4 string. */
        /* --------------------------------------------------------------------
         */
        char *pszProj4 = nullptr;

        if (oSRS.exportToProj4(&pszProj4) != OGRERR_NONE)
        {
            CPLFree(pszProj4);
            return m_nUndefinedSRID;
        }

        osProj4 = pszProj4;
        CPLFree(pszProj4);
        pszProj4 = nullptr;

        /* --------------------------------------------------------------------
         */
        /*      Try to find based on the PROJ.4 match. */
        /* --------------------------------------------------------------------
         */
        osCommand.Printf(
            "SELECT srid FROM spatial_ref_sys WHERE proj4text = ? LIMIT 2");
    }

    sqlite3_stmt *hSelectStmt = nullptr;
    int rc = prepareSql(hDB, osCommand, -1, &hSelectStmt, nullptr);

    if (rc == SQLITE_OK)
        rc = sqlite3_bind_text(hSelectStmt, 1,
                               (pszSRTEXTColName != nullptr) ? osWKT.c_str()
                                                             : osProj4.c_str(),
                               -1, SQLITE_STATIC);

    if (rc == SQLITE_OK)
        rc = sqlite3_step(hSelectStmt);

    if (rc == SQLITE_ROW)
    {
        if (sqlite3_column_type(hSelectStmt, 0) == SQLITE_INTEGER)
            nSRSId = sqlite3_column_int(hSelectStmt, 0);
        else
            nSRSId = m_nUndefinedSRID;

        sqlite3_finalize(hSelectStmt);

        if (nSRSId != m_nUndefinedSRID)
        {
            std::unique_ptr<OGRSpatialReference, OGRSpatialReferenceReleaser>
                poSRSClone;
            poSRSClone.reset(oSRS.Clone());
            AddSRIDToCache(nSRSId, std::move(poSRSClone));
        }

        return nSRSId;
    }

    /* -------------------------------------------------------------------- */
    /*      If the command actually failed, then the metadata table is      */
    /*      likely missing, so we give up.                                  */
    /* -------------------------------------------------------------------- */
    if (rc != SQLITE_DONE && rc != SQLITE_ROW)
    {
        sqlite3_finalize(hSelectStmt);
        return m_nUndefinedSRID;
    }

    sqlite3_finalize(hSelectStmt);

    /* -------------------------------------------------------------------- */
    /*      Translate SRS to PROJ.4 string (if not already done)            */
    /* -------------------------------------------------------------------- */
    if (osProj4.empty())
    {
        char *pszProj4 = nullptr;
        if (oSRS.exportToProj4(&pszProj4) == OGRERR_NONE)
        {
            osProj4 = pszProj4;
        }
        CPLFree(pszProj4);
        pszProj4 = nullptr;
    }

    /* -------------------------------------------------------------------- */
    /*      If we have an authority code try to assign SRS ID the same      */
    /*      as that code.                                                   */
    /* -------------------------------------------------------------------- */
    if (pszAuthorityCode != nullptr && strlen(pszAuthorityCode) > 0)
    {
        osCommand.Printf("SELECT * FROM spatial_ref_sys WHERE auth_srid='%s' "
                         "LIMIT 2",
                         SQLEscapeLiteral(pszAuthorityCode).c_str());
        rc = sqlite3_get_table(hDB, osCommand, &papszResult, &nRowCount,
                               &nColCount, &pszErrMsg);

        if (rc != SQLITE_OK)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "exec(SELECT '%s' FROM spatial_ref_sys) failed: %s",
                     pszAuthorityCode, pszErrMsg);
            sqlite3_free(pszErrMsg);
        }

        /* --------------------------------------------------------------------
         */
        /*      If there is no SRS ID with such auth_srid, use it as SRS ID. */
        /* --------------------------------------------------------------------
         */
        if (nRowCount < 1)
        {
            nSRSId = atoi(pszAuthorityCode);
            /* The authority code might be non numeric, e.g. IGNF:LAMB93 */
            /* in which case we might fallback to the fake OGR authority */
            /* for spatialite, since its auth_srid is INTEGER */
            if (nSRSId == 0)
            {
                nSRSId = m_nUndefinedSRID;
                if (m_bIsSpatiaLiteDB)
                    pszAuthorityName = nullptr;
            }
        }
        sqlite3_free_table(papszResult);
    }

    /* -------------------------------------------------------------------- */
    /*      Otherwise get the current maximum srid in the srs table.        */
    /* -------------------------------------------------------------------- */
    if (nSRSId == m_nUndefinedSRID)
    {
        rc =
            sqlite3_get_table(hDB, "SELECT MAX(srid) FROM spatial_ref_sys",
                              &papszResult, &nRowCount, &nColCount, &pszErrMsg);

        if (rc != SQLITE_OK)
        {
            CPLError(CE_Failure, CPLE_AppDefined,
                     "SELECT of the maximum SRS ID failed: %s", pszErrMsg);
            sqlite3_free(pszErrMsg);
            return m_nUndefinedSRID;
        }

        if (nRowCount < 1 || !papszResult[1])
            nSRSId = 50000;
        else
            nSRSId = atoi(papszResult[1]) + 1;  // Insert as the next SRS ID
        sqlite3_free_table(papszResult);
    }

    /* -------------------------------------------------------------------- */
    /*      Try adding the SRS to the SRS table.                            */
    /* -------------------------------------------------------------------- */

    const char *apszToInsert[] = {nullptr, nullptr, nullptr,
                                  nullptr, nullptr, nullptr};

    if (!m_bIsSpatiaLiteDB)
    {
        if (pszAuthorityName != nullptr)
        {
            osCommand.Printf(
                "INSERT INTO spatial_ref_sys (srid,srtext,auth_name,auth_srid) "
                "                     VALUES (%d, ?, ?, ?)",
                nSRSId);
            apszToInsert[0] = osWKT.c_str();
            apszToInsert[1] = pszAuthorityName;
            apszToInsert[2] = pszAuthorityCode;
        }
        else
        {
            osCommand.Printf("INSERT INTO spatial_ref_sys (srid,srtext) "
                             "                     VALUES (%d, ?)",
                             nSRSId);
            apszToInsert[0] = osWKT.c_str();
        }
    }
    else
    {
        CPLString osSRTEXTColNameWithCommaBefore;
        if (pszSRTEXTColName != nullptr)
            osSRTEXTColNameWithCommaBefore.Printf(", %s", pszSRTEXTColName);

        const char *pszProjCS = oSRS.GetAttrValue("PROJCS");
        if (pszProjCS == nullptr)
            pszProjCS = oSRS.GetAttrValue("GEOGCS");

        if (pszAuthorityName != nullptr)
        {
            if (pszProjCS)
            {
                osCommand.Printf(
                    "INSERT INTO spatial_ref_sys "
                    "(srid, auth_name, auth_srid, ref_sys_name, proj4text%s) "
                    "VALUES (%d, ?, ?, ?, ?%s)",
                    (pszSRTEXTColName != nullptr)
                        ? osSRTEXTColNameWithCommaBefore.c_str()
                        : "",
                    nSRSId, (pszSRTEXTColName != nullptr) ? ", ?" : "");
                apszToInsert[0] = pszAuthorityName;
                apszToInsert[1] = pszAuthorityCode;
                apszToInsert[2] = pszProjCS;
                apszToInsert[3] = osProj4.c_str();
                apszToInsert[4] =
                    (pszSRTEXTColName != nullptr) ? osWKT.c_str() : nullptr;
            }
            else
            {
                osCommand.Printf("INSERT INTO spatial_ref_sys "
                                 "(srid, auth_name, auth_srid, proj4text%s) "
                                 "VALUES (%d, ?, ?, ?%s)",
                                 (pszSRTEXTColName != nullptr)
                                     ? osSRTEXTColNameWithCommaBefore.c_str()
                                     : "",
                                 nSRSId,
                                 (pszSRTEXTColName != nullptr) ? ", ?" : "");
                apszToInsert[0] = pszAuthorityName;
                apszToInsert[1] = pszAuthorityCode;
                apszToInsert[2] = osProj4.c_str();
                apszToInsert[3] =
                    (pszSRTEXTColName != nullptr) ? osWKT.c_str() : nullptr;
            }
        }
        else
        {
            /* SpatiaLite spatial_ref_sys auth_name and auth_srid columns must
             * be NOT NULL */
            /* so insert within a fake OGR "authority" */
            if (pszProjCS)
            {
                osCommand.Printf("INSERT INTO spatial_ref_sys "
                                 "(srid, auth_name, auth_srid, ref_sys_name, "
                                 "proj4text%s) VALUES (%d, 'OGR', %d, ?, ?%s)",
                                 (pszSRTEXTColName != nullptr)
                                     ? osSRTEXTColNameWithCommaBefore.c_str()
                                     : "",
                                 nSRSId, nSRSId,
                                 (pszSRTEXTColName != nullptr) ? ", ?" : "");
                apszToInsert[0] = pszProjCS;
                apszToInsert[1] = osProj4.c_str();
                apszToInsert[2] =
                    (pszSRTEXTColName != nullptr) ? osWKT.c_str() : nullptr;
            }
            else
            {
                osCommand.Printf("INSERT INTO spatial_ref_sys "
                                 "(srid, auth_name, auth_srid, proj4text%s) "
                                 "VALUES (%d, 'OGR', %d, ?%s)",
                                 (pszSRTEXTColName != nullptr)
                                     ? osSRTEXTColNameWithCommaBefore.c_str()
                                     : "",
                                 nSRSId, nSRSId,
                                 (pszSRTEXTColName != nullptr) ? ", ?" : "");
                apszToInsert[0] = osProj4.c_str();
                apszToInsert[1] =
                    (pszSRTEXTColName != nullptr) ? osWKT.c_str() : nullptr;
            }
        }
    }

    sqlite3_stmt *hInsertStmt = nullptr;
    rc = prepareSql(hDB, osCommand, -1, &hInsertStmt, nullptr);

    for (int i = 0; apszToInsert[i] != nullptr; i++)
    {
        if (rc == SQLITE_OK)
            rc = sqlite3_bind_text(hInsertStmt, i + 1, apszToInsert[i], -1,
                                   SQLITE_STATIC);
    }

    if (rc == SQLITE_OK)
        rc = sqlite3_step(hInsertStmt);

    if (rc != SQLITE_OK && rc != SQLITE_DONE)
    {
        CPLError(CE_Failure, CPLE_AppDefined, "Unable to insert SRID (%s): %s",
                 osCommand.c_str(), sqlite3_errmsg(hDB));

        sqlite3_finalize(hInsertStmt);
        return FALSE;
    }

    sqlite3_finalize(hInsertStmt);

    if (nSRSId != m_nUndefinedSRID)
    {
        std::unique_ptr<OGRSpatialReference, OGRSpatialReferenceReleaser>
            poCachedSRS(new OGRSpatialReference(std::move(oSRS)));
        poCachedSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        AddSRIDToCache(nSRSId, std::move(poCachedSRS));
    }

    return nSRSId;
}

/************************************************************************/
/*                              FetchSRS()                              */
/*                                                                      */
/*      Return a SRS corresponding to a particular id.  Note that       */
/*      reference counting should be honoured on the returned           */
/*      OGRSpatialReference, as handles may be cached.                  */
/************************************************************************/

OGRSpatialReference *OGRSQLiteDataSource::FetchSRS(int nId)

{
    if (nId <= 0)
        return nullptr;

    /* -------------------------------------------------------------------- */
    /*      First, we look through our SRID cache, is it there?             */
    /* -------------------------------------------------------------------- */
    const auto oIter = m_oSRSCache.find(nId);
    if (oIter != m_oSRSCache.end())
    {
        return oIter->second.get();
    }

    /* -------------------------------------------------------------------- */
    /*      Try looking up in spatial_ref_sys table.                        */
    /* -------------------------------------------------------------------- */
    char *pszErrMsg = nullptr;
    char **papszResult = nullptr;
    int nRowCount = 0;
    int nColCount = 0;
    std::unique_ptr<OGRSpatialReference, OGRSpatialReferenceReleaser> poSRS;

    CPLString osCommand;
    osCommand.Printf("SELECT srtext FROM spatial_ref_sys WHERE srid = %d "
                     "LIMIT 2",
                     nId);
    int rc = sqlite3_get_table(hDB, osCommand, &papszResult, &nRowCount,
                               &nColCount, &pszErrMsg);

    if (rc == SQLITE_OK)
    {
        if (nRowCount < 1)
        {
            sqlite3_free_table(papszResult);
            return nullptr;
        }

        char **papszRow = papszResult + nColCount;
        if (papszRow[0] != nullptr)
        {
            CPLString osWKT = papszRow[0];

            /* --------------------------------------------------------------------
             */
            /*      Translate into a spatial reference. */
            /* --------------------------------------------------------------------
             */
            poSRS.reset(new OGRSpatialReference());
            poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            if (poSRS->importFromWkt(osWKT.c_str()) != OGRERR_NONE)
            {
                poSRS.reset();
            }
        }

        sqlite3_free_table(papszResult);
    }

    /* -------------------------------------------------------------------- */
    /*      Next try SpatiaLite flavor. SpatiaLite uses PROJ.4 strings     */
    /*      in 'proj4text' column instead of WKT in 'srtext'. Note: recent  */
    /*      versions of spatialite have a srs_wkt column too                */
    /* -------------------------------------------------------------------- */
    else
    {
        sqlite3_free(pszErrMsg);
        pszErrMsg = nullptr;

        const char *pszSRTEXTColName = GetSRTEXTColName();
        CPLString osSRTEXTColNameWithCommaBefore;
        if (pszSRTEXTColName != nullptr)
            osSRTEXTColNameWithCommaBefore.Printf(", %s", pszSRTEXTColName);

        osCommand.Printf(
            "SELECT proj4text, auth_name, auth_srid%s FROM spatial_ref_sys "
            "WHERE srid = %d LIMIT 2",
            (pszSRTEXTColName != nullptr)
                ? osSRTEXTColNameWithCommaBefore.c_str()
                : "",
            nId);
        rc = sqlite3_get_table(hDB, osCommand, &papszResult, &nRowCount,
                               &nColCount, &pszErrMsg);
        if (rc == SQLITE_OK)
        {
            if (nRowCount < 1)
            {
                sqlite3_free_table(papszResult);
                return nullptr;
            }

            /* --------------------------------------------------------------------
             */
            /*      Translate into a spatial reference. */
            /* --------------------------------------------------------------------
             */
            char **papszRow = papszResult + nColCount;

            const char *pszProj4Text = papszRow[0];
            const char *pszAuthName = papszRow[1];
            int nAuthSRID = (papszRow[2] != nullptr) ? atoi(papszRow[2]) : 0;
            const char *pszWKT =
                (pszSRTEXTColName != nullptr) ? papszRow[3] : nullptr;

            poSRS.reset(new OGRSpatialReference());
            poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

            /* Try first from EPSG code */
            if (pszAuthName != nullptr && EQUAL(pszAuthName, "EPSG") &&
                poSRS->importFromEPSG(nAuthSRID) == OGRERR_NONE)
            {
                /* Do nothing */
            }
            /* Then from WKT string */
            else if (pszWKT != nullptr &&
                     poSRS->importFromWkt(pszWKT) == OGRERR_NONE)
            {
                /* Do nothing */
            }
            /* Finally from Proj4 string */
            else if (pszProj4Text != nullptr &&
                     poSRS->importFromProj4(pszProj4Text) == OGRERR_NONE)
            {
                /* Do nothing */
            }
            else
            {
                poSRS.reset();
            }

            sqlite3_free_table(papszResult);
        }

        /* --------------------------------------------------------------------
         */
        /*      No success, report an error. */
        /* --------------------------------------------------------------------
         */
        else
        {
            CPLError(CE_Failure, CPLE_AppDefined, "%s: %s", osCommand.c_str(),
                     pszErrMsg);
            sqlite3_free(pszErrMsg);
            return nullptr;
        }
    }

    if (poSRS)
        poSRS->StripTOWGS84IfKnownDatumAndAllowed();

    /* -------------------------------------------------------------------- */
    /*      Add to the cache.                                               */
    /* -------------------------------------------------------------------- */
    return AddSRIDToCache(nId, std::move(poSRS));
}

/************************************************************************/
/*                              SetName()                               */
/************************************************************************/

void OGRSQLiteDataSource::SetName(const char *pszNameIn)
{
    CPLFree(m_pszFilename);
    m_pszFilename = CPLStrdup(pszNameIn);
}

/************************************************************************/
/*                       GetEnvelopeFromSQL()                           */
/************************************************************************/

const OGREnvelope *
OGRSQLiteBaseDataSource::GetEnvelopeFromSQL(const CPLString &osSQL)
{
    const auto oIter = oMapSQLEnvelope.find(osSQL);
    if (oIter != oMapSQLEnvelope.end())
        return &oIter->second;
    else
        return nullptr;
}

/************************************************************************/
/*                         SetEnvelopeForSQL()                          */
/************************************************************************/

void OGRSQLiteBaseDataSource::SetEnvelopeForSQL(const CPLString &osSQL,
                                                const OGREnvelope &oEnvelope)
{
    oMapSQLEnvelope[osSQL] = oEnvelope;
}

/***********************************************************************/
/*                       SetQueryLoggerFunc()                          */
/***********************************************************************/

bool OGRSQLiteBaseDataSource::SetQueryLoggerFunc(
    GDALQueryLoggerFunc pfnQueryLoggerFuncIn, void *poQueryLoggerArgIn)
{
    pfnQueryLoggerFunc = pfnQueryLoggerFuncIn;
    poQueryLoggerArg = poQueryLoggerArgIn;

    if (pfnQueryLoggerFunc)
    {
        sqlite3_trace_v2(
            hDB, SQLITE_TRACE_PROFILE,
            [](unsigned int /* traceProfile */, void *context,
               void *preparedStatement, void *executionTime) -> int
            {
                if (context)
                {
                    char *pzsSql{sqlite3_expanded_sql(
                        reinterpret_cast<sqlite3_stmt *>(preparedStatement))};
                    if (pzsSql)
                    {
                        const std::string sql{pzsSql};
                        sqlite3_free(pzsSql);
                        const uint64_t executionTimeMilliSeconds{
                            static_cast<uint64_t>(
                                *reinterpret_cast<uint64_t *>(executionTime) /
                                1e+6)};
                        OGRSQLiteBaseDataSource *source{
                            reinterpret_cast<OGRSQLiteBaseDataSource *>(
                                context)};
                        if (source->pfnQueryLoggerFunc)
                        {
                            source->pfnQueryLoggerFunc(
                                sql.c_str(), nullptr, -1,
                                executionTimeMilliSeconds,
                                source->poQueryLoggerArg);
                        }
                    }
                }
                return 0;
            },
            reinterpret_cast<void *>(this));
        return true;
    }
    return false;
}

/************************************************************************/
/*                         AbortSQL()                                   */
/************************************************************************/

OGRErr OGRSQLiteBaseDataSource::AbortSQL()
{
    sqlite3_interrupt(hDB);
    return OGRERR_NONE;
}
