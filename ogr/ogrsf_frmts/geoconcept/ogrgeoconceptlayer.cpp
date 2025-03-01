/******************************************************************************
 *
 * Project:  OpenGIS Simple Features Reference Implementation
 * Purpose:  Implements OGRGeoconceptLayer class.
 * Author:   Didier Richard, didier.richard@ign.fr
 * Language: C++
 *
 ******************************************************************************
 * Copyright (c) 2007,  Geoconcept and IGN
 * Copyright (c) 2008, Even Rouault <even dot rouault at spatialys.com>
 *
 * SPDX-License-Identifier: MIT
 ****************************************************************************/

#include "cpl_conv.h"
#include "cpl_string.h"
#include "ogrgeoconceptlayer.h"

/************************************************************************/
/*                         OGRGeoconceptLayer()                         */
/************************************************************************/

OGRGeoconceptLayer::OGRGeoconceptLayer()
    : _poFeatureDefn(nullptr), _gcFeature(nullptr)
{
}

/************************************************************************/
/*                          ~OGRGeoconceptLayer()                      */
/************************************************************************/

OGRGeoconceptLayer::~OGRGeoconceptLayer()

{
    if (_poFeatureDefn)
    {
        CPLDebug("GEOCONCEPT", "%ld features on layer %s.",
                 GetSubTypeNbFeatures_GCIO(_gcFeature),
                 _poFeatureDefn->GetName());

        _poFeatureDefn->Release();
    }

    _gcFeature = nullptr; /* deleted when OGCGeoconceptDatasource destroyed */
}

/************************************************************************/
/*                              Open()                                  */
/************************************************************************/

OGRErr OGRGeoconceptLayer::Open(GCSubType *Subclass)

{
    _gcFeature = Subclass;
    if (GetSubTypeFeatureDefn_GCIO(_gcFeature))
    {
        _poFeatureDefn = reinterpret_cast<OGRFeatureDefn *>(
            GetSubTypeFeatureDefn_GCIO(_gcFeature));
        SetDescription(_poFeatureDefn->GetName());
        _poFeatureDefn->Reference();
    }
    else
    {
        char pszln[512];
        snprintf(pszln, 511, "%s.%s", GetSubTypeName_GCIO(_gcFeature),
                 GetTypeName_GCIO(GetSubTypeType_GCIO(_gcFeature)));
        pszln[511] = '\0';

        _poFeatureDefn = new OGRFeatureDefn(pszln);
        SetDescription(_poFeatureDefn->GetName());
        _poFeatureDefn->Reference();
        _poFeatureDefn->SetGeomType(wkbUnknown);

        const int n = CountSubTypeFields_GCIO(_gcFeature);
        if (n > 0)
        {
            OGRFieldType oft;
            for (int i = 0; i < n; i++)
            {
                GCField *aField = GetSubTypeField_GCIO(_gcFeature, i);
                if (aField)
                {
                    if (IsPrivateField_GCIO(aField))
                        continue;
                    switch (GetFieldKind_GCIO(aField))
                    {
                        case vIntFld_GCIO:
                        case vPositionFld_GCIO:
                            oft = OFTInteger;
                            break;
                        case vRealFld_GCIO:
                        case vLengthFld_GCIO:
                        case vAreaFld_GCIO:
                            oft = OFTReal;
                            break;
                        case vDateFld_GCIO:
                            oft = OFTDate;
                            break;
                        case vTimeFld_GCIO:
                            oft = OFTTime;
                            break;
                        case vMemoFld_GCIO:
                        case vChoiceFld_GCIO:
                        case vInterFld_GCIO:
                        default:
                            oft = OFTString;
                            break;
                    }
                    OGRFieldDefn ofd(GetFieldName_GCIO(aField), oft);
                    _poFeatureDefn->AddFieldDefn(&ofd);
                }
            }
        }
        SetSubTypeFeatureDefn_GCIO(_gcFeature, (OGRFeatureDefnH)_poFeatureDefn);
        _poFeatureDefn->Reference();
    }

    if (_poFeatureDefn->GetGeomFieldCount() > 0)
        _poFeatureDefn->GetGeomFieldDefn(0)->SetSpatialRef(GetSpatialRef());

    return OGRERR_NONE;
}

/************************************************************************/
/*                            ResetReading()                            */
/************************************************************************/

void OGRGeoconceptLayer::ResetReading()

{
    Rewind_GCIO(GetSubTypeGCHandle_GCIO(_gcFeature), _gcFeature);
}

/************************************************************************/
/*                           GetNextFeature()                           */
/************************************************************************/

OGRFeature *OGRGeoconceptLayer::GetNextFeature()

{
    OGRFeature *poFeature = nullptr;

    for (;;)
    {
        if (!(poFeature = (OGRFeature *)ReadNextFeature_GCIO(_gcFeature)))
        {
            /*
             * As several features are embed in the Geoconcept file,
             * when reaching the end of the feature type, resetting
             * the reader would allow reading other features :
             * ogrinfo -ro export.gxt FT1 FT2 ...
             * will be all features for all features types !
             */
            Rewind_GCIO(GetSubTypeGCHandle_GCIO(_gcFeature), nullptr);
            break;
        }
        if ((m_poFilterGeom == nullptr ||
             FilterGeometry(poFeature->GetGeometryRef())) &&
            (m_poAttrQuery == nullptr || m_poAttrQuery->Evaluate(poFeature)))
        {
            break;
        }
        delete poFeature;
    }

    CPLDebug("GEOCONCEPT",
             "FID : " CPL_FRMT_GIB "\n"
             "%s  : %s",
             poFeature ? poFeature->GetFID() : -1L,
             poFeature && poFeature->GetFieldCount() > 0
                 ? poFeature->GetFieldDefnRef(0)->GetNameRef()
                 : "-",
             poFeature && poFeature->GetFieldCount() > 0
                 ? poFeature->GetFieldAsString(0)
                 : "");

    return poFeature;
}

/************************************************************************/
/*            OGRGeoconceptLayer_GetCompatibleFieldName()               */
/************************************************************************/

static char *OGRGeoconceptLayer_GetCompatibleFieldName(const char *pszName)
{
    char *pszCompatibleName = CPLStrdup(pszName);
    for (int i = 0; pszCompatibleName[i] != 0; i++)
    {
        if (pszCompatibleName[i] == ' ')
            pszCompatibleName[i] = '_';
    }
    return pszCompatibleName;
}

/************************************************************************/
/*                           ICreateFeature()                            */
/************************************************************************/

OGRErr OGRGeoconceptLayer::ICreateFeature(OGRFeature *poFeature)

{
    OGRGeometry *poGeom = poFeature->GetGeometryRef();

    if (poGeom == nullptr)
    {
        CPLError(
            CE_Warning, CPLE_NotSupported,
            "NULL geometry not supported in Geoconcept, feature skipped.\n");
        return OGRERR_NONE;
    }

    OGRwkbGeometryType eGt = poGeom->getGeometryType();
    switch (wkbFlatten(eGt))
    {
        case wkbPoint:
        case wkbMultiPoint:
            if (GetSubTypeKind_GCIO(_gcFeature) == vUnknownItemType_GCIO)
            {
                SetSubTypeKind_GCIO(_gcFeature, vPoint_GCIO);
            }
            else if (GetSubTypeKind_GCIO(_gcFeature) != vPoint_GCIO)
            {
                CPLError(CE_Failure, CPLE_NotSupported,
                         "Can't write non ponctual feature in a ponctual "
                         "Geoconcept layer %s.\n",
                         _poFeatureDefn->GetName());
                return OGRERR_FAILURE;
            }
            break;
        case wkbLineString:
        case wkbMultiLineString:
            if (GetSubTypeKind_GCIO(_gcFeature) == vUnknownItemType_GCIO)
            {
                SetSubTypeKind_GCIO(_gcFeature, vLine_GCIO);
            }
            else if (GetSubTypeKind_GCIO(_gcFeature) != vLine_GCIO)
            {
                CPLError(
                    CE_Failure, CPLE_NotSupported,
                    "Can't write non linear feature in a linear Geoconcept "
                    "layer %s.\n",
                    _poFeatureDefn->GetName());
                return OGRERR_FAILURE;
            }
            break;
        case wkbPolygon:
        case wkbMultiPolygon:
            if (GetSubTypeKind_GCIO(_gcFeature) == vUnknownItemType_GCIO)
            {
                SetSubTypeKind_GCIO(_gcFeature, vPoly_GCIO);
            }
            else if (GetSubTypeKind_GCIO(_gcFeature) != vPoly_GCIO)
            {
                CPLError(CE_Failure, CPLE_NotSupported,
                         "Can't write non polygonal feature in a polygonal "
                         "Geoconcept layer %s.\n",
                         _poFeatureDefn->GetName());
                return OGRERR_FAILURE;
            }
            break;
        default:
            CPLError(CE_Warning, CPLE_AppDefined,
                     "Geometry type %s not supported in Geoconcept, "
                     "feature skipped.\n",
                     OGRGeometryTypeToName(eGt));
            return OGRERR_NONE;
    }
    if (GetSubTypeDim_GCIO(_gcFeature) == vUnknown3D_GCIO)
    {
        if (poGeom->getCoordinateDimension() == 3)
        {
            SetSubTypeDim_GCIO(_gcFeature, v3D_GCIO);
        }
        else
        {
            SetSubTypeDim_GCIO(_gcFeature, v2D_GCIO);
        }
    }

    int nbGeom = 0;
    bool isSingle = false;

    switch (wkbFlatten(eGt))
    {
        case wkbPoint:
        case wkbLineString:
        case wkbPolygon:
            nbGeom = 1;
            isSingle = true;
            break;
        case wkbMultiPoint:
        case wkbMultiLineString:
        case wkbMultiPolygon:
            nbGeom = poGeom->toGeometryCollection()->getNumGeometries();
            isSingle = false;
            break;
        default:
            nbGeom = 0;
            isSingle = false;
            break;
    }

    /* 1st feature, let's write header : */
    if (GetGCMode_GCIO(GetSubTypeGCHandle_GCIO(_gcFeature)) ==
            vWriteAccess_GCIO &&
        GetFeatureCount(TRUE) == 0)
        if (WriteHeader_GCIO(GetSubTypeGCHandle_GCIO(_gcFeature)) == nullptr)
        {
            return OGRERR_FAILURE;
        }

    if (nbGeom > 0)
    {
        for (int iGeom = 0; iGeom < nbGeom; iGeom++)
        {
            int nextField = StartWritingFeature_GCIO(
                _gcFeature,
                isSingle ? static_cast<int>(poFeature->GetFID()) : OGRNullFID);
            while (nextField != WRITECOMPLETED_GCIO)
            {
                if (nextField == WRITEERROR_GCIO)
                {
                    return OGRERR_FAILURE;
                }
                if (nextField == GEOMETRYEXPECTED_GCIO)
                {
                    OGRGeometry *poGeomPart =
                        isSingle
                            ? poGeom
                            : poGeom->toGeometryCollection()->getGeometryRef(
                                  iGeom);
                    nextField = WriteFeatureGeometry_GCIO(
                        _gcFeature, (OGRGeometryH)poGeomPart);
                }
                else
                {
                    GCField *theField =
                        GetSubTypeField_GCIO(_gcFeature, nextField);
                    /* for each field, find out its mapping ... */
                    int nF = poFeature->GetFieldCount();
                    if (nF > 0)
                    {
                        int iF = 0;
                        for (; iF < nF; iF++)
                        {
                            OGRFieldDefn *poField =
                                poFeature->GetFieldDefnRef(iF);
                            char *pszName =
                                OGRGeoconceptLayer_GetCompatibleFieldName(
                                    poField->GetNameRef());
                            if (EQUAL(pszName, GetFieldName_GCIO(theField)))
                            {
                                CPLFree(pszName);
                                nextField = WriteFeatureFieldAsString_GCIO(
                                    _gcFeature, nextField,
                                    poFeature->IsFieldSetAndNotNull(iF)
                                        ? poFeature->GetFieldAsString(iF)
                                        : nullptr);
                                break;
                            }
                            CPLFree(pszName);
                        }
                        if (iF == nF)
                        {
                            CPLError(CE_Failure, CPLE_AppDefined,
                                     "Can't find a field attached to %s on "
                                     "Geoconcept layer %s.\n",
                                     GetFieldName_GCIO(theField),
                                     _poFeatureDefn->GetName());
                            return OGRERR_FAILURE;
                        }
                    }
                    else
                    {
                        nextField = WRITECOMPLETED_GCIO;
                    }
                }
            }
            StopWritingFeature_GCIO(_gcFeature);
        }
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                           GetSpatialRef()                            */
/************************************************************************/

OGRSpatialReference *OGRGeoconceptLayer::GetSpatialRef()

{
    GCExportFileH *hGXT = GetSubTypeGCHandle_GCIO(_gcFeature);
    if (!hGXT)
        return nullptr;
    GCExportFileMetadata *Meta = GetGCMeta_GCIO(hGXT);
    if (!Meta)
        return nullptr;
    return (OGRSpatialReference *)GetMetaSRS_GCIO(Meta);
}

/************************************************************************/
/*                          GetFeatureCount()                           */
/*                                                                      */
/*      If a spatial filter is in effect, we turn control over to       */
/*      the generic counter.  Otherwise we return the total count.      */
/************************************************************************/

GIntBig OGRGeoconceptLayer::GetFeatureCount(int bForce)

{
    if (m_poFilterGeom != nullptr || m_poAttrQuery != nullptr)
        return OGRLayer::GetFeatureCount(bForce);

    return GetSubTypeNbFeatures_GCIO(_gcFeature);
}

/************************************************************************/
/*                            IGetExtent()                              */
/************************************************************************/

OGRErr OGRGeoconceptLayer::IGetExtent(int /* iGeomField*/,
                                      OGREnvelope *psExtent, bool)
{
    GCExtent *theExtent = GetSubTypeExtent_GCIO(_gcFeature);
    if (!theExtent)
        return OGRERR_FAILURE;
    psExtent->MinX = GetExtentULAbscissa_GCIO(theExtent);
    psExtent->MinY = GetExtentLROrdinate_GCIO(theExtent);
    psExtent->MaxX = GetExtentLRAbscissa_GCIO(theExtent);
    psExtent->MaxY = GetExtentULOrdinate_GCIO(theExtent);

    return OGRERR_NONE;
}

/************************************************************************/
/*                           TestCapability()                           */
/************************************************************************/

int OGRGeoconceptLayer::TestCapability(const char *pszCap)

{
    if (EQUAL(pszCap, OLCRandomRead))
        return FALSE;  // the GetFeature() method does not work for this layer.
                       // TODO

    else if (EQUAL(pszCap, OLCSequentialWrite))
        return TRUE;  // the CreateFeature() method works for this layer.

    else if (EQUAL(pszCap, OLCRandomWrite))
        return FALSE;  // the SetFeature() method is not operational on this
                       // layer.

    else if (EQUAL(pszCap, OLCFastSpatialFilter))
        return FALSE;  // this layer does not implement spatial filtering
                       // efficiently.

    else if (EQUAL(pszCap, OLCFastFeatureCount))
        return FALSE;  // this layer can not return a feature count efficiently.
                       // FIXME

    else if (EQUAL(pszCap, OLCFastGetExtent))
        return FALSE;  // this layer can not return its data extent efficiently.
                       // FIXME

    else if (EQUAL(pszCap, OLCFastSetNextByIndex))
        return FALSE;  // this layer can not perform the SetNextByIndex() call
                       // efficiently.

    else if (EQUAL(pszCap, OLCDeleteFeature))
        return FALSE;

    else if (EQUAL(pszCap, OLCCreateField))
        return TRUE;

    else if (EQUAL(pszCap, OLCZGeometries))
        return TRUE;

    return FALSE;
}

/************************************************************************/
/*                            CreateField()                             */
/************************************************************************/

OGRErr OGRGeoconceptLayer::CreateField(const OGRFieldDefn *poField,
                                       CPL_UNUSED int bApproxOK)
{
    if (GetGCMode_GCIO(GetSubTypeGCHandle_GCIO(_gcFeature)) == vReadAccess_GCIO)
    {
        CPLError(CE_Failure, CPLE_NotSupported,
                 "Can't create fields on a read-only Geoconcept layer.\n");
        return OGRERR_FAILURE;
    }

    /* -------------------------------------------------------------------- */
    /*      Add field to layer                                              */
    /* -------------------------------------------------------------------- */

    {
        /* check whether field exists ... */
        char *pszName =
            OGRGeoconceptLayer_GetCompatibleFieldName(poField->GetNameRef());

        GCField *theField = FindFeatureField_GCIO(_gcFeature, pszName);
        if (!theField)
        {
            if (GetFeatureCount(TRUE) > 0)
            {
                CPLError(CE_Failure, CPLE_NotSupported,
                         "Can't create field '%s' on existing Geoconcept layer "
                         "'%s.%s'.\n",
                         pszName, GetSubTypeName_GCIO(_gcFeature),
                         GetTypeName_GCIO(GetSubTypeType_GCIO(_gcFeature)));
                CPLFree(pszName);
                return OGRERR_FAILURE;
            }
            if (GetSubTypeNbFields_GCIO(_gcFeature) == -1)
                SetSubTypeNbFields_GCIO(_gcFeature, 0L);
            if (!(theField = AddSubTypeField_GCIO(
                      GetSubTypeGCHandle_GCIO(_gcFeature),
                      GetTypeName_GCIO(GetSubTypeType_GCIO(_gcFeature)),
                      GetSubTypeName_GCIO(_gcFeature),
                      FindFeatureFieldIndex_GCIO(_gcFeature, kNbFields_GCIO) +
                          GetSubTypeNbFields_GCIO(_gcFeature) + 1,
                      pszName, GetSubTypeNbFields_GCIO(_gcFeature) - 999L,
                      vUnknownItemType_GCIO, nullptr, nullptr)))
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Field '%s' could not be created for Feature %s.%s.\n",
                         pszName, GetSubTypeName_GCIO(_gcFeature),
                         GetTypeName_GCIO(GetSubTypeType_GCIO(_gcFeature)));
                CPLFree(pszName);
                return OGRERR_FAILURE;
            }
            SetSubTypeNbFields_GCIO(_gcFeature,
                                    GetSubTypeNbFields_GCIO(_gcFeature) + 1);
            _poFeatureDefn->AddFieldDefn(poField);
        }
        else
        {
            if (_poFeatureDefn->GetFieldIndex(GetFieldName_GCIO(theField)) ==
                -1)
            {
                CPLError(CE_Failure, CPLE_AppDefined,
                         "Field %s not found for Feature %s.%s.\n",
                         GetFieldName_GCIO(theField),
                         GetSubTypeName_GCIO(_gcFeature),
                         GetTypeName_GCIO(GetSubTypeType_GCIO(_gcFeature)));
                CPLFree(pszName);
                return OGRERR_FAILURE;
            }
        }

        CPLFree(pszName);
        pszName = nullptr;

        /* check/update type ? */
        if (GetFieldKind_GCIO(theField) == vUnknownItemType_GCIO)
        {
            switch (poField->GetType())
            {
                case OFTInteger:
                    SetFieldKind_GCIO(theField, vIntFld_GCIO);
                    break;
                case OFTReal:
                    SetFieldKind_GCIO(theField, vRealFld_GCIO);
                    break;
                case OFTDate:
                    SetFieldKind_GCIO(theField, vDateFld_GCIO);
                    break;
                case OFTTime:
                case OFTDateTime:
                    SetFieldKind_GCIO(theField, vTimeFld_GCIO);
                    break;
                case OFTString:
                    SetFieldKind_GCIO(theField, vMemoFld_GCIO);
                    break;
                case OFTIntegerList:
                case OFTRealList:
                case OFTStringList:
                case OFTBinary:
                default:
                    CPLError(CE_Failure, CPLE_NotSupported,
                             "Can't create fields of type %s on Geoconcept "
                             "feature %s.\n",
                             OGRFieldDefn::GetFieldTypeName(poField->GetType()),
                             _poFeatureDefn->GetName());
                    return OGRERR_FAILURE;
            }
        }
    }

    return OGRERR_NONE;
}

/************************************************************************/
/*                             SyncToDisk()                             */
/************************************************************************/

OGRErr OGRGeoconceptLayer::SyncToDisk()

{
    FFlush_GCIO(GetSubTypeGCHandle_GCIO(_gcFeature));
    return OGRERR_NONE;
}

/************************************************************************/
/*                             SetSpatialRef()                          */
/************************************************************************/

void OGRGeoconceptLayer::SetSpatialRef(OGRSpatialReference *poSpatialRef)

{
    OGRSpatialReference *poSRS = GetSpatialRef();

    GCExportFileH *hGXT = GetSubTypeGCHandle_GCIO(_gcFeature);
    if (hGXT)
    {
        GCExportFileMetadata *Meta = GetGCMeta_GCIO(hGXT);
        if (Meta)
        {
            if (poSRS)
                poSRS->Release();
            SetMetaSRS_GCIO(Meta, nullptr);
        }
        else
            return;
    }
    else
    {
        return;
    }

    poSRS = poSpatialRef->Clone();
    poSRS->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    GCExportFileMetadata *Meta = GetGCMeta_GCIO(hGXT);
    GCSysCoord *os = GetMetaSysCoord_GCIO(Meta);
    GCSysCoord *ns = OGRSpatialReference2SysCoord_GCSRS(
        reinterpret_cast<OGRSpatialReferenceH>(poSRS));

    if (os && ns && GetSysCoordSystemID_GCSRS(os) != -1 &&
        (GetSysCoordSystemID_GCSRS(os) != GetSysCoordSystemID_GCSRS(ns) ||
         GetSysCoordTimeZone_GCSRS(os) != GetSysCoordTimeZone_GCSRS(ns)))
    {
        CPLError(CE_Warning, CPLE_AppDefined,
                 "Can't change SRS on Geoconcept layers.\n");
        DestroySysCoord_GCSRS(&ns);
        poSRS->Release();
        return;
    }

    if (os)
        DestroySysCoord_GCSRS(&os);
    SetMetaSysCoord_GCIO(Meta, ns);
    SetMetaSRS_GCIO(Meta, (OGRSpatialReferenceH)poSRS);
    return;
}
